// nvcx: a VHPI plugin for NVC that simulates xsim snapshots behind plain VHDL
// shell entities. A shell is an entity with the core's ports whose
// architecture holds an instance of nvcx.bridge, implementing the foreign
// procedure:
//
//   procedure nvcx_open(snapshot, name, generics : string);
//
// ...which opens the snapshot on Vivado's xsim kernel and finds the shell's
// ports by name through VHPI. Every instance shares the one kernel, which
// keeps its state in process-wide globals: the core switches them from one
// design to the next (Simulation::make_current).
//
// Timing: the two simulators advance in lockstep, one delta cycle each. At the
// end of every NVC delta's process phase the plugin writes the inputs that
// changed in that delta into the kernel and lets it run one delta: its
// processes, then its signal updates. The outputs that changed are deposited
// onto the shell for NVC's next delta. Each side therefore sees the other's
// assignments one delta after they were made, exactly as in one simulator.
// When NVC has no delta pending, the kernels still in the middle of the time
// step run on without it until one changes an output: nothing could have
// happened on NVC's side in the deltas skipped.
//
// A quiescent kernel's next event (a transaction maturing, a process's wait
// expiring) is registered with NVC as an after-delay callback, so that NVC
// visits that time. The kernel's first delta there begins with its signal
// updates: the outputs they change belong in NVC's first delta there too, in
// its update phase, as a native core's transactions would. NVC runs
// after-delay callbacks during that phase and applies a deposit made then in
// the next delta, so instead the kernel is brought to the time and its
// updated outputs deposited from the next-time-step callback, which runs
// before NVC's update phase: a deposit made there lands in it.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>
#include <vhpi_user.h>
#include "nvcx/simulation.hpp"
#include "nvcx/vivado.hpp"

namespace fs = std::filesystem;
using namespace nvcx;

namespace {

struct Port {
    const Simulation::Port* xp; // the core's port, in XSI terms
    enum Kind {
        Logic,    // a lone std_ulogic / bit: vhpiLogicVal
        LogicVec, // an array of them: vhpiLogicVecVal
        Integer,  // vhpiIntVal
        Real,     // vhpiRealVal
        Enum,     // any other scalar enumeration (boolean): vhpiEnumVal
    } kind;
    size_t bits;               // scalars on the VHPI side (1 unless LogicVec)
    vhpiHandleT handle;        // the shell's port of the same name
    std::vector<uint8_t> last; // last value crossed, to skip redundant deposits
    const std::string& name() const {
        return xp->name;
    }
};

struct Instance {
    std::string name;
    std::unique_ptr<Simulation> sim;
    std::vector<Port> ins, outs;
    uint64_t fs_per_tick = 1;
    bool finished_reported = false;
    vhpiHandleT timeout = nullptr; // pending after-delay callback, if any
    uint64_t timeout_fs = 0;       // the NVC time it fires at
    bool dirty = false;            // an input changed in this NVC delta
    bool due = false;              // the kernel's next event is at this time
    bool active = false;           // listed in g_active
};

std::vector<std::unique_ptr<Instance>> g_instances;
// the instances with something to do: an input changed, a timer fired, or
// the kernel is in the middle of a time step
std::vector<Instance*> g_active;

void activate(Instance& in) {
    if (!in.active) {
        in.active = true;
        g_active.push_back(&in);
    }
}

bool step(Instance& in);

[[noreturn]]
void fail(const std::string& msg) {
    static char fmt[] = "nvcx: %s";
    vhpi_assert(vhpiFailure, fmt, msg.c_str());
    vhpi_control(vhpiFinish, 1);
    abort();
}

std::string get_string(vhpiHandleT param) {
    vhpiValueT v{};
    v.format = vhpiStrVal;
    v.bufSize = 0;
    v.value.str = nullptr;
    int need = vhpi_get_value(param, &v);
    if (need < 0)
        fail("cannot read string parameter");
    std::string buf((size_t)need, '\0');
    v.bufSize = (size_t)need;
    v.value.str = reinterpret_cast<vhpiCharT*>(buf.data());
    if (vhpi_get_value(param, &v) != 0)
        fail("cannot read string parameter");
    buf.resize(strnlen(buf.data(), buf.size()));
    return buf;
}

// The snapshot is the directory xelab wrote, xsim.dir/<name>, resolved by
// tcl/nvcx.tcl when it wrote the shell.
fs::path resolve_snapshot(const std::string& name,
                          const std::string& snapshot,
                          const std::string& generics) {
    std::string gen = generics;
    for (size_t i = 0; (i = gen.find('\n', i)) != std::string::npos; i += 2) {
        gen.replace(i, 1, ", ");
    }
    std::string why;
    if (snapshot.empty()) {
        why = "no snapshot was elaborated for the generics {" + gen + "}";
    } else if (!fs::is_directory(snapshot)) {
        why = "snapshot " + snapshot + " (generics {" + gen + "}) is missing";
    }
    if (!why.empty()) {
        static char fmt[] = "nvcx: %s";
        std::string msg =
            std::format("{}: {}; re-run the script that wrote its shell. It "
                        "is a black box with undriven outputs",
                        name,
                        why);
        vhpi_assert(vhpiWarning, fmt, msg.c_str());
        return {};
    }
    // the kernel needs xsimk.so with the metadata beside it, in an xsim.dir
    if (!fs::is_regular_file(fs::path(snapshot) / "xsimk.so")) {
        fail(std::format("snapshot directory {} has no xsimk.so (elaborate "
                         "with xelab -dll)",
                         snapshot));
    }
    return fs::path(snapshot) / "xsimk.so";
}

std::string lower(std::string s) {
    for (auto& c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

// The shell port's natural VHPI value format decides how values are read and
// deposited; XSI's size for the core's port must match it.
Port classify(const std::string& instance,
              const Simulation::Port& xp,
              vhpiHandleT handle) {
    Port p{.xp = &xp, .kind = Port::Logic, .bits = 1, .handle = handle};
    vhpiValueT v{};
    v.format = vhpiObjTypeVal;
    v.bufSize = 0;
    int need = vhpi_get_value(handle, &v);
    switch (v.format) {
    case vhpiLogicVal:
        p.kind = Port::Logic;
        break;
    case vhpiLogicVecVal:
        p.kind = Port::LogicVec;
        p.bits =
            need > 0 ? (size_t)need / sizeof(vhpiEnumT) : (size_t)v.numElems;
        break;
    case vhpiIntVal:
        p.kind = Port::Integer;
        break;
    case vhpiRealVal:
        p.kind = Port::Real;
        break;
    case vhpiEnumVal:
    case vhpiSmallEnumVal:
        p.kind = Port::Enum;
        break;
    default:
        fail(std::format("{}: port {}: unsupported type on the shell (VHPI "
                         "value format {}); std_logic/bit scalars and arrays, "
                         "integer, real and boolean are supported",
                         instance,
                         xp.name,
                         static_cast<int>(v.format)));
    }
    // the core's side: XSI reports bytes for VHDL, bits for Verilog
    size_t core = xp.vhdl ? (p.kind == Port::Integer ? 4
                             : p.kind == Port::Real  ? 8
                                                     : p.bits)
                          : p.bits;
    if (!xp.vhdl && (p.kind == Port::Integer || p.kind == Port::Real ||
                     p.kind == Port::Enum)) {
        fail(std::format("{}: port {}: a Verilog port needs a std_logic or "
                         "std_logic_vector shell port",
                         instance,
                         xp.name));
    }
    if (xp.size != core) {
        fail(std::format("{}: port {} has {} elements on the shell but {} {} "
                         "in the snapshot",
                         instance,
                         xp.name,
                         p.bits,
                         xp.size,
                         xp.vhdl ? "bytes" : "bits"));
    }
    return p;
}

// Read the shell's port into the core's layout.
std::vector<uint8_t> read_port(const Instance& in, const Port& p) {
    std::vector<uint8_t> raw(p.xp->raw_size());
    vhpiValueT v{};
    if (p.kind == Port::Integer) {
        v.format = vhpiIntVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        int32_t x = (int32_t)v.value.intg;
        std::memcpy(raw.data(), &x, 4);
    } else if (p.kind == Port::Real) {
        v.format = vhpiRealVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        double x = v.value.real;
        std::memcpy(raw.data(), &x, 8);
    } else if (p.kind == Port::Enum || p.kind == Port::Logic) {
        v.format = p.kind == Port::Enum ? vhpiEnumVal : vhpiLogicVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        uint8_t ordinal = (uint8_t)v.value.enumv;
        if (p.xp->vhdl) {
            raw[0] = ordinal;
        } else {
            logic_to_vlog(
                std::string(1, ordinal < 9 ? LOGIC_CHARS[ordinal] : 'X'),
                raw.data(),
                1);
        }
    } else {
        v.format = vhpiLogicVecVal;
        v.bufSize = 0;
        int need = vhpi_get_value(p.handle, &v);
        if (need < 0) {
            fail(in.name + ": cannot size " + p.name());
        }
        std::vector<vhpiEnumT> buf((size_t)need / sizeof(vhpiEnumT) + 1);
        v.bufSize = (size_t)need;
        v.value.enumvs = buf.data();
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        if ((size_t)v.numElems != p.bits) {
            fail(in.name + ": port " + p.name() + " changed size on the shell");
        }
        if (p.xp->vhdl) {
            for (size_t i = 0; i < p.bits; i++) {
                raw[i] = (uint8_t)buf[i];
            }
        } else {
            std::string str(p.bits, 'X');
            for (size_t i = 0; i < p.bits; i++) {
                str[i] = buf[i] < 9 ? LOGIC_CHARS[buf[i]] : 'X';
            }
            logic_to_vlog(str, raw.data(), (unsigned)p.bits);
        }
    }
    return raw;
}

// Deposit a value in the core's layout onto the shell's port (next delta,
// like an assignment).
void write_port(const Instance& in,
                const Port& p,
                const std::vector<uint8_t>& raw) {
    vhpiValueT v{};
    std::vector<vhpiEnumT> buf;
    if (p.kind == Port::Integer) {
        int32_t x;
        std::memcpy(&x, raw.data(), 4);
        v.format = vhpiIntVal;
        v.value.intg = x;
    } else if (p.kind == Port::Real) {
        double x;
        std::memcpy(&x, raw.data(), 8);
        v.format = vhpiRealVal;
        v.value.real = x;
    } else if (p.kind == Port::Enum) {
        v.format = vhpiEnumVal;
        v.value.enumv = raw[0];
    } else {
        std::string str = p.xp->vhdl
                              ? std::string()
                              : vlog_to_logic(raw.data(), (unsigned)p.bits);
        buf.resize(p.bits);
        for (size_t i = 0; i < p.bits; i++) {
            buf[i] =
                p.xp->vhdl
                    ? raw[i]
                    : (vhpiEnumT)(strchr(LOGIC_CHARS, str[i]) - LOGIC_CHARS);
        }
        if (p.kind == Port::Logic) {
            v.format = vhpiLogicVal;
            v.value.enumv = buf[0];
        } else {
            v.format = vhpiLogicVecVal;
            v.bufSize = buf.size() * sizeof(vhpiEnumT);
            v.numElems = (uint32_t)buf.size();
            v.value.enumvs = buf.data();
        }
    }
    if (vhpi_put_value(p.handle, &v, vhpiDepositPropagate) != 0) {
        fail(in.name + ": cannot write " + p.name());
    }
}

void on_timeout(const vhpiCbDataT* cb) {
    Instance& in = *static_cast<Instance*>(cb->user_data);
    vhpi_release_handle(in.timeout);
    in.timeout = nullptr;
    in.due = true;
    activate(in);
}

void on_input_change(const vhpiCbDataT* cb) {
    Instance& in = *static_cast<Instance*>(cb->user_data);
    in.dirty = true;
    activate(in);
}

// One kernel delta for every active instance; those still in the middle of
// the time step stay active. True if any output was deposited.
bool step_active() {
    std::vector<Instance*> active;
    active.swap(g_active);
    bool deposited = false;
    for (Instance* in : active) {
        in->active = false;
        deposited |= step(*in);
        if (in->sim->suspended()) {
            activate(*in);
        }
    }
    return deposited;
}

// At the end of each NVC delta's process phase
void on_end_of_processes(const vhpiCbDataT*) {
    step_active();
}

// NVC has no delta pending at this time: the kernels still in the middle of
// it run on, a delta each in turn, until one changes an output (that deposit
// gives NVC a delta, and lockstep resumes there) or all are quiescent. In the
// deltas skipped nothing could have happened on NVC's side: it had nothing
// to run and no input of theirs could have changed.
void on_last_known_delta(const vhpiCbDataT*) {
    while (!g_active.empty() && !step_active()) {
    }
}

// NVC's current time in femtoseconds
uint64_t nvc_now_fs() {
    vhpiTimeT now{};
    vhpi_get_time(&now, nullptr);
    return ((uint64_t)(uint32_t)now.high << 32) | now.low;
}

// Bring a quiescent kernel to NVC's current time, paused after the signal
// updates of its first delta here (its own events, if any) and before their
// processes. A pause at an earlier time has nothing for NVC: nothing was
// scheduled there. A suspended kernel is here already.
Simulation::RunResult arrive(Instance& in) {
    Simulation& d = *in.sim;
    using Status = Simulation::RunStatus;
    Simulation::RunResult r{Status::Completed, ""};
    if (d.suspended()) {
        return r;
    }
    uint64_t now_fs = nvc_now_fs();
    if (now_fs % in.fs_per_tick != 0) {
        fail(std::format("{}: NVC time {} fs is not a multiple of the "
                         "kernel's {} fs tick; elaborate the core with a finer "
                         "precision",
                         in.name,
                         now_fs,
                         in.fs_per_tick));
    }
    uint64_t target = now_fs / in.fs_per_tick;
    r = d.start(target > d.time() ? target - d.time() : 0);
    while (r.status == Status::Yielded && d.time() < target) {
        r = d.resume();
    }
    return r;
}

// How a run ended: an error is fatal; a core's own end of simulation is
// reported once.
void check(Instance& in, const Simulation::RunResult& r) {
    using Status = Simulation::RunStatus;
    if (r.status == Status::Error) {
        fail(in.name + ": " + r.message);
    }
    if (r.status == Status::Finished && !in.finished_reported) {
        in.finished_reported = true;
        vhpi_printf("nvcx: %s: the core finished its simulation; its outputs "
                    "are frozen",
                    in.name.c_str());
    }
}

// Deposit the outputs that changed since they last crossed. True if any did.
bool deposit_outputs(Instance& in) {
    bool deposited = false;
    for (Port& p : in.outs) {
        std::vector<uint8_t> raw(p.xp->raw_size());
        in.sim->get(*p.xp, raw.data());
        if (raw != p.last) {
            write_port(in, p, raw);
            p.last = raw;
            deposited = true;
        }
    }
    return deposited;
}

// At the start of each time step, before NVC's own signal updates: the
// kernels whose next event is now run their first delta's updates here, and
// the outputs those change are deposited into NVC's update phase (see the
// header comment). Their processes wait for the end of NVC's, in step().
void on_next_time_step(const vhpiCbDataT*) {
    uint64_t now_fs = nvc_now_fs();
    for (auto& inst : g_instances) {
        Instance& in = *inst;
        if (!in.timeout || in.timeout_fs != now_fs) {
            continue;
        }
        check(in, arrive(in));
        deposit_outputs(in);
        in.due = true;
        activate(in);
    }
}

bool step(Instance& in) {
    Simulation& d = *in.sim;
    if (!in.dirty && !in.due && !d.suspended()) {
        return false;
    }
    using Status = Simulation::RunStatus;
    Simulation::RunResult r = arrive(in);
    check(in, r);
    // the inputs this delta changed, for the processes about to run (the
    // value-change callbacks say whether there are any; reading them all
    // through VHPI is the costly part of a step)
    bool changed = false;
    if (in.dirty) {
        for (Port& p : in.ins) {
            std::vector<uint8_t> raw = read_port(in, p);
            if (raw != p.last) {
                d.put(*p.xp, raw.data());
                p.last = raw;
                changed = true;
            }
        }
    }
    in.dirty = false;
    in.due = false;
    // one delta: the processes, then the updates they schedule
    if (d.suspended()) {
        r = d.resume();
    } else if (changed) {
        r = d.start(0);
        if (r.status == Status::Yielded) {
            r = d.resume();
        }
    }
    check(in, r);
    bool deposited = deposit_outputs(in);
    if (in.timeout) {
        vhpi_remove_cb(in.timeout);
        in.timeout = nullptr;
    }
    if (r.status == Status::Yielded) {
        return deposited; // more deltas at this time
    }
    // quiescent: the kernel's next event becomes an after-delay callback
    uint64_t next;
    if (d.next_event_time(next) && !d.finished()) {
        uint64_t delay_fs =
            (next > d.time() ? next - d.time() : 1) * in.fs_per_tick;
        vhpiTimeT t{.high = (int32_t)(delay_fs >> 32),
                    .low = (uint32_t)delay_fs};
        vhpiCbDataT cb{.reason = vhpiCbAfterDelay,
                       .cb_rtn = on_timeout,
                       .time = &t,
                       .user_data = &in};
        in.timeout = vhpi_register_cb(&cb, vhpiReturnCb);
        if (!in.timeout) {
            fail(in.name + ": cannot register the after-delay callback");
        }
        in.timeout_fs = nvc_now_fs() + delay_fs;
    }
    return deposited;
}

void exec_open(const vhpiCbDataT* cb) {
    std::string args[3];
    for (int i = 0; i < 3; i++) {
        vhpiHandleT p = vhpi_handle_by_index(vhpiParamDecls, cb->obj, i);
        args[i] = get_string(p);
        vhpi_release_handle(p);
    }
    const std::string &snapshot = args[0], &name = args[1], &generics = args[2];
    fs::path design_so = resolve_snapshot(name, snapshot, generics);
    if (design_so.empty()) {
        return; // a black box
    }
    auto inst = std::make_unique<Instance>();
    inst->name = name;
    try {
        // loads Vivado's kernel and its closure on the first instance
        inst->sim = std::make_unique<Simulation>(design_so.string());
    } catch (const std::exception& e) {
        fail(name + ": " + e.what());
    }
    Simulation& d = *inst->sim;
    // The core's reports and assertions become NVC's, at their severity, so
    // NVC's --exit-severity governs them like any other assertion.
    d.set_report_sink([name](const std::string& msg) {
        static char fmt[] = "%s";
        static const vhpiSeverityT sev[] = {
            vhpiNote, vhpiWarning, vhpiError, vhpiFailure};
        int s = Simulation::report_severity(msg);
        size_t colon = msg.find(':');
        std::string text =
            colon == std::string::npos ? msg : msg.substr(colon + 1);
        while (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }
        vhpi_assert(sev[s], fmt, (name + ": " + text).c_str());
    });

    // The shell is the parent of this bridge instance: ":tb:dut:core:" ->
    // ":tb:dut:"
    std::string shell = name;
    if (shell.size() > 1 && shell.back() == ':') {
        shell.pop_back();
    }
    shell = shell.substr(0, shell.rfind(':') + 1);
    for (const Simulation::Port& xp : d.ports()) {
        if (xp.dir != Direction::In && xp.dir != Direction::Out) {
            fail(std::format(
                "{}: port {}: inout ports are not supported", name, xp.name));
        }
        std::string port_name = shell + lower(xp.name);
        vhpiHandleT h =
            vhpi_handle_by_name(const_cast<char*>(port_name.c_str()), nullptr);
        if (!h) {
            fail(std::format("{}: the shell has no port named {} (looked "
                             "for {})",
                             name,
                             xp.name,
                             port_name));
        }
        (xp.dir == Direction::In ? inst->ins : inst->outs)
            .push_back(classify(name, xp, h));
    }
    // kernel time unit (10^precision s) in NVC's femtoseconds
    int prec = d.precision();
    if (prec < -15 || prec > 0) {
        fail(std::format("unsupported kernel time precision 10^{} s", prec));
    }
    inst->fs_per_tick = 1;
    for (int i = -15; i < prec; i++) {
        inst->fs_per_tick *= 10;
    }
    g_instances.push_back(std::move(inst));
    Instance& in = *g_instances.back();
    for (Port& p : in.ins) {
        vhpiCbDataT change{.reason = vhpiCbValueChange,
                           .cb_rtn = on_input_change,
                           .obj = p.handle,
                           .user_data = &in};
        if (!vhpi_register_cb(&change, vhpiReturnCb)) {
            fail(std::format(
                "{}: cannot register the value-change callback on {}",
                name,
                p.name()));
        }
    }
    // Initial values now: the cycle callbacks do not run during
    // initialization, and nothing on NVC's side can see the core until its
    // first delta, so its initialization runs on until an output changes.
    in.dirty = true;
    activate(in);
    while (in.active && !step_active()) {
    }
}

void end_of_sim(const vhpiCbDataT*) {
    g_active.clear();
    for (auto& inst : g_instances) {
        inst->sim.reset();
    }
    g_instances.clear();
}

void startup() {
    // the VHPI header declares these as char*, hence the static arrays
    static char lib_name[] = "nvcx";
    static char open_name[] = "nvcx_open";
    vhpiForeignDataT open_data{.kind = vhpiProcF,
                               .libraryName = lib_name,
                               .modelName = open_name,
                               .elabf = nullptr,
                               .execf = exec_open};
    if (!vhpi_register_foreignf(&open_data)) {
        vhpi_printf("nvcx: vhpi_register_foreignf failed");
        return;
    }
    vhpiCbDataT cb{.reason = vhpiCbEndOfSimulation, .cb_rtn = end_of_sim};
    vhpi_register_cb(&cb, 0);
    vhpiCbDataT eop{.reason = vhpiCbRepEndOfProcesses,
                    .cb_rtn = on_end_of_processes};
    vhpiHandleT h = vhpi_register_cb(&eop, vhpiReturnCb);
    if (!h) {
        vhpi_printf("nvcx: cannot register the end-of-processes callback");
        return;
    }
    vhpi_release_handle(h);
    vhpiCbDataT lkd{.reason = vhpiCbRepLastKnownDeltaCycle,
                    .cb_rtn = on_last_known_delta};
    h = vhpi_register_cb(&lkd, vhpiReturnCb);
    if (!h) {
        vhpi_printf("nvcx: cannot register the last-known-delta callback");
        return;
    }
    vhpi_release_handle(h);
    vhpiCbDataT nts{.reason = vhpiCbRepNextTimeStep,
                    .cb_rtn = on_next_time_step};
    h = vhpi_register_cb(&nts, vhpiReturnCb);
    if (!h) {
        vhpi_printf("nvcx: cannot register the next-time-step callback");
        return;
    }
    vhpi_release_handle(h);
}

} // namespace

extern "C" {
    using startup_routine = void (*)();

    [[gnu::visibility("default")]]
    startup_routine vhpi_startup_routines[] = {startup, nullptr};
}
