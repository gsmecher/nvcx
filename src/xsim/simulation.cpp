#include "nvcx/xsim/simulation.hpp"

#include <dlfcn.h>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <format>
#include <fstream>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace nvcx::xsim {

namespace {

    std::string dirname_of(const std::string& p) {
        auto s = p.rfind('/');
        return s == std::string::npos ? "." : p.substr(0, s);
    }

    // The release that elaborated the snapshot in *dir*, from its xsim.version
    // (xelab 2025.2 and later), else "". 2025.2 writes "hjhoth" rather than
    // its version; 2026.1 writes "2026.1".
    std::string snapshot_version(const std::string& dir) {
        std::ifstream f(dir + "/xsim.version");
        std::string v;
        f >> v;

        // for some reason...
        if (v == "hjhoth")
            return "2025.2";

        return v;
    }

    // The open designs, closed at exit if their owners have not
    std::unordered_set<Simulation*> g_live;
    std::mutex g_live_mu;

    // xsi_open is serialized across instances
    std::mutex g_open_mu;

    // The Simulation whose kernel is executing (xsi_open, xsi_close, a run),
    // for the hook on its delta cycles
    thread_local Simulation* t_current = nullptr;
    struct Current {
        Simulation* outer = t_current;
        explicit Current(Simulation* s) {
            t_current = s;
        }
        ~Current() {
            t_current = outer;
        }
    };

    constexpr size_t STACK_SIZE = 8 << 20; // the kernel's fiber

    // Registered with atexit once the first Simulation has loaded its
    // libraries, so it runs before any of their static destructors
    void close_live_at_exit() {
        std::vector<Simulation*> live;
        {
            std::lock_guard<std::mutex> lock(g_live_mu);
            live.assign(g_live.begin(), g_live.end());
        }
        for (Simulation* sim : live)
            sim->close();
    }

} // namespace

Simulation::Simulation(const std::string& design_so)
    : design_dir_(dirname_of(design_so)) {
    kernel_ = &Kernel::get();
    kernel_->hook_active_events(on_active_events);
    // The kernel's fiber: each start() hands it the ticks to run and resumes
    // it; it hands back when the run yields or returns.
    run_ = boost::context::fiber(
        std::allocator_arg,
        boost::context::protected_fixedsize_stack(STACK_SIZE),
        [this](boost::context::fiber&& main) {
            main_ = std::move(main);
            for (;;) {
                running_ = true;
                kernel_->xsi_run(handle_, run_ticks_);
                running_ = false;
                suspended_ = false;
                main_ = std::move(main_).resume();
            }
            return std::move(main_);
        });
    // A snapshot only loads on the release that elaborated it; the kernel
    // does not check, and a mismatch dies with a garbage allocation size.
    const std::string elaborated_by = snapshot_version(design_dir_);
    if (!elaborated_by.empty() && !kernel_->version.empty() &&
        elaborated_by != kernel_->version)
        throw std::runtime_error(
            std::format("snapshot {} was elaborated by Vivado {} but the "
                        "simulator kernel is {} ({}); re-elaborate it or set "
                        "XILINX_VIVADO to the {} installation",
                        design_dir_,
                        elaborated_by,
                        kernel_->version,
                        kernel_->path(),
                        elaborated_by));
    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        g_live.insert(this);
    }
    std::lock_guard<std::mutex> open_lock(g_open_mu);
    try {
        dlerror();
        design_handle_ =
            dlopen(design_so.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
        if (!design_handle_)
            throw std::runtime_error(
                std::format("cannot load design {}: {}", design_so, dlerror()));
        auto xsi_open = reinterpret_cast<XsiHandle (*)(XsiSetupInfo*)>(
            dlsym(design_handle_, "xsi_open"));
        if (!xsi_open)
            throw std::runtime_error("design library has no xsi_open: " +
                                     design_so);
        setup_.logFileName = nullptr;
        setup_.wdbFileName = nullptr;
        // XSI resolves xsim.dir/<snapshot>/... relative to the working
        // directory unless told otherwise;
        xsim_dir_ = dirname_of(design_dir_);
        setup_.xsimDir = xsim_dir_.data();
        try {
            Current current(this);
            handle_ = xsi_open(&setup_);
            kernel_->current = nullptr; // whatever xsi_open left there
        } catch (...) {
            throw std::runtime_error(std::format(
                "xsi_open threw (is {} the xsim.dir of this design?)",
                xsim_dir_));
        }
        if (!handle_)
            throw std::runtime_error("xsi_open failed for " + design_so);
        int n = kernel_->xsi_get_int(handle_, xsiNumTopPorts);
        for (int i = 0; i < n; i++) {
            Port p;
            p.index = i;
            const char* nm =
                kernel_->xsi_get_str_port(handle_, i, xsiNameTopPort);
            p.name = nm ? nm : "";
            switch (
                kernel_->xsi_get_int_port(handle_, i, xsiDirectionTopPort)) {
            case xsiInputPort:
                p.dir = Direction::In;
                break;
            case xsiOutputPort:
                p.dir = Direction::Out;
                break;
            case xsiInoutPort:
                p.dir = Direction::Inout;
                break;
            }
            p.size = (unsigned)kernel_->xsi_get_int_port(
                handle_, i, xsiHDLValueSize);
            p.vhdl = kernel_->host_isPortVHDL(handle_, i);
            ports_.push_back(std::move(p));
        }
        static std::once_flag at_exit;
        std::call_once(at_exit, [] {
            atexit(close_live_at_exit);
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(g_live_mu);
            g_live.erase(this);
        }
        if (handle_) {
            kernel_->xsi_close(handle_);
            kernel_->current = nullptr;
        }
        if (design_handle_)
            dlclose(design_handle_);
        throw;
    }
}

void Simulation::close() {
    {
        std::lock_guard<std::mutex> lock(g_live_mu);
        g_live.erase(this);
    }
    // the kernel cannot be closed in the middle of a time step
    while (suspended_)
        resume();
    if (handle_) {
        Current current(this);
        make_current();
        kernel_->xsi_close(handle_);
        kernel_->current = nullptr;
    }
    handle_ = nullptr;
}

Simulation::~Simulation() {
    close();
    if (design_handle_)
        dlclose(design_handle_);
}

// The xsi_* calls on this design set its globals too; the ones that do not
// (xsi_get_time), the kernel's globals read directly (status_kind,
// next_event_time), and a run resumed in the middle of a time step rely on
// this. Whatever another design did in between, its calls were preceded by
// its own make_current(), so *current* is always right when set.
void Simulation::make_current() const {
    if (kernel_->current != handle_) {
        kernel_->host_setGlobals(handle_, true);
        kernel_->current = handle_;
    }
}

void Simulation::get(const Port& p, uint8_t* buf) const {
    make_current();
    // XSI writes only the significant bits of each Verilog word
    std::memset(buf, 0, p.raw_size());
    kernel_->xsi_get_value(handle_, p.index, buf);
}

void Simulation::put(const Port& p, const uint8_t* buf) {
    make_current();
    kernel_->xsi_put_value(handle_, p.index, const_cast<uint8_t*>(buf));
}

// xsi_get_error_info() is the kernel's message buffer for the last run, which
// also carries plain `report`/`assert` messages (as '@"Note: ...\nTime:
// ...\n"'); the status tells errors apart.
std::string Simulation::last_error() {
    const char* e = kernel_->xsi_get_error_info(handle_);
    return e ? e : "";
}

// The buffer holds every report of the run, oldest first, each as
// '@"<text>\nTime: ... Process: ...\n"': split them, drop the kernel's own
// location trailer (the sink adds its simulator's), emit them in order.
void Simulation::emit_report(const std::string& raw) {
    std::vector<std::string> msgs;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t start = raw.find("@\"", pos);
        if (start == std::string::npos) {
            if (msgs.empty())
                msgs.push_back(raw);
            break;
        }
        size_t next = raw.find("\n@\"", start + 2);
        std::string m = raw.substr(
            start + 2, next == std::string::npos ? next : next - (start + 2));
        while (!m.empty() && (m.back() == '"' || m.back() == '\n'))
            m.pop_back();
        size_t nl = m.rfind('\n');
        if (nl != std::string::npos && m.compare(nl + 1, 5, "Time:") == 0)
            m.erase(nl);
        msgs.push_back(m);
        pos = next == std::string::npos ? raw.size() : next + 1;
    }
    for (const std::string& m : msgs)
        report(report_severity(m), report_text(m), m);
}

namespace {

    // The kernel's severity prefixes. A message without one ($display's
    // text) is a note, and all of it is text: a colon of its own is not a
    // prefix's.
    const struct {
        std::string_view prefix;
        Core::Severity severity;
    } SEVERITIES[] = {
        {"Failure:", Core::Severity::Failure},
        {"Fatal:", Core::Severity::Failure},
        {"Error:", Core::Severity::Error},
        {"Warning:", Core::Severity::Warning},
        {"Note:", Core::Severity::Note},
        {"Info:", Core::Severity::Note},
    };

} // namespace

Core::Severity Simulation::report_severity(const std::string& msg) {
    for (const auto& s : SEVERITIES)
        if (msg.starts_with(s.prefix))
            return s.severity;
    return Severity::Note;
}

std::string Simulation::report_text(const std::string& msg) {
    for (const auto& s : SEVERITIES)
        if (msg.starts_with(s.prefix)) {
            size_t start = msg.find_first_not_of(' ', s.prefix.size());
            return start == std::string::npos ? "" : msg.substr(start);
        }
    return msg;
}

// The hook on the kernel's signal-update phase: do the updates, then, in a
// run, pause the kernel until the next resume(). The design's initialization
// (xsi_open) and finalization (xsi_close) have deltas too; those pass.
void Simulation::on_active_events(void* eventq) {
    Simulation* s = t_current;
    s->kernel_->process_active_events(eventq);
    if (!s->running_)
        return;
    s->suspended_ = true;
    s->main_ = std::move(s->main_).resume();
}

Simulation::RunResult Simulation::start(uint64_t ticks) {
    if (finished_)
        return {RunStatus::Finished, "simulation finished"};
    run_ticks_ = ticks;
    return resume();
}

Simulation::RunResult Simulation::resume() {
    Current current(this);
    make_current();
    run_ = std::move(run_).resume();
    if (suspended_)
        return {RunStatus::Yielded, ""};
    return after_run();
}

Simulation::RunResult Simulation::after_run() {
    std::string err = last_error();
    int st = kernel_->xsi_get_status(handle_);
    if (st == xsiError || st == xsiFatalError)
        return {RunStatus::Error,
                err.empty() ? "kernel status " + std::to_string(st) : err};
    if (!err.empty())
        emit_report(
            err); // a report; what it means for the run is the sink's call
    int ks = kernel_->status_kind();
    if (ks == abi::KS_FINISHED || ks == abi::KS_VHDL_FINISH ||
        ks == abi::KS_CLOSED) {
        finished_ = true;
        return {RunStatus::Finished, "simulation finished"};
    }
    return {RunStatus::Completed, ""};
}

} // namespace nvcx::xsim
