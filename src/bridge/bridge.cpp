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

struct Instance;

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
    // Per-step scratch, sized once so that a step does not allocate: the
    // value in the core's layout, and (vectors) the VHPI side's elements
    std::vector<uint8_t> raw;
    std::vector<vhpiEnumT> enums;
    Instance* owner = nullptr; // an input's instance, for its callback
    bool dirty = false;        // an input listed in owner->changed
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
    uint64_t timeout_at = 0;       // ...and when it fires (fs)
    bool dirty = false;            // an input changed in this NVC delta
    std::vector<Port*> changed;    // ...namely these
    bool due = false;              // the kernel's next event is at this time
    bool active = false;           // listed in g_active
    uint64_t eop_fs = UINT64_MAX;  // the NVC delta it was last stepped in
    long eop_delta = -1;           // ...at the end of processes
    // A pass's: whether this instance takes part, and what its kernel's delta
    // left for the NVC side (see stage_run)
    bool stepping = false;
    bool arriving = false; // ...only to arrive at this time (see Pass)
    Simulation::RunResult result{Simulation::RunStatus::Completed, ""};
    std::vector<Port*> outs_changed;
    bool next_pending = false; // the kernel has a next event, at next_at (fs)
    uint64_t next_at = 0;
};

std::vector<std::unique_ptr<Instance>> g_instances;
// the instances with something to do: an input changed, a timer fired, or
// the kernel is in the middle of a time step
std::vector<Instance*> g_active;
// What a pass steps (Pass::active): it and g_active swap at each pass, so
// that both keep their capacity rather than growing anew every pass
std::vector<Instance*> g_stepping;

void activate(Instance& in) {
    if (!in.active) {
        in.active = true;
        g_active.push_back(&in);
    }
}

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

// std_logic ordinals (LOGIC_CHARS: U X 0 1 Z W L H -) to XSI's Verilog
// words, element 0 being the most significant bit: 'L' is 0, 'H' is 1, 'Z' is
// Z, and anything else X, as logic_to_vlog.
void enums_to_vlog(const vhpiEnumT* e, size_t nbits, uint8_t* raw) {
    std::memset(raw, 0, (nbits + 31) / 32 * sizeof(VlogLogicVal));
    auto* w = reinterpret_cast<VlogLogicVal*>(raw);
    for (size_t i = 0; i < nbits; i++) {
        bool a, b;
        switch (e[nbits - 1 - i]) {
        case vhpi0:
        case vhpiL:
            a = 0, b = 0;
            break;
        case vhpi1:
        case vhpiH:
            a = 1, b = 0;
            break;
        case vhpiZ:
            a = 0, b = 1;
            break;
        default:
            a = 1, b = 1;
        }
        w[i / 32].aVal |= (uint32_t)a << (i % 32);
        w[i / 32].bVal |= (uint32_t)b << (i % 32);
    }
}

// ...and back: '0', '1', 'Z' or 'X', as vlog_to_logic
void vlog_to_enums(const uint8_t* raw, size_t nbits, vhpiEnumT* e) {
    const auto* w = reinterpret_cast<const VlogLogicVal*>(raw);
    for (size_t i = 0; i < nbits; i++) {
        bool a = (w[i / 32].aVal >> (i % 32)) & 1;
        bool b = (w[i / 32].bVal >> (i % 32)) & 1;
        e[nbits - 1 - i] = b ? (a ? vhpiX : vhpiZ) : (a ? vhpi1 : vhpi0);
    }
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
    p.raw.resize(xp.raw_size());
    if (p.kind == Port::LogicVec || p.kind == Port::Logic) {
        p.enums.resize(p.bits);
    }
    return p;
}

// Read the shell's port into the core's layout, in p.raw.
void read_port(const Instance& in, Port& p) {
    uint8_t* raw = p.raw.data();
    vhpiValueT v{};
    if (p.kind == Port::Integer) {
        v.format = vhpiIntVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        int32_t x = (int32_t)v.value.intg;
        std::memcpy(raw, &x, 4);
    } else if (p.kind == Port::Real) {
        v.format = vhpiRealVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        double x = v.value.real;
        std::memcpy(raw, &x, 8);
    } else if (p.kind == Port::Enum || p.kind == Port::Logic) {
        v.format = p.kind == Port::Enum ? vhpiEnumVal : vhpiLogicVal;
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        if (p.xp->vhdl) {
            raw[0] = (uint8_t)v.value.enumv;
        } else {
            p.enums[0] = v.value.enumv;
            enums_to_vlog(p.enums.data(), 1, raw);
        }
    } else {
        // the size is fixed (classify), so one call reads it
        v.format = vhpiLogicVecVal;
        v.bufSize = p.bits * sizeof(vhpiEnumT);
        v.value.enumvs = p.enums.data();
        if (vhpi_get_value(p.handle, &v) != 0) {
            fail(in.name + ": cannot read " + p.name());
        }
        if ((size_t)v.numElems != p.bits) {
            fail(in.name + ": port " + p.name() + " changed size on the shell");
        }
        if (p.xp->vhdl) {
            for (size_t i = 0; i < p.bits; i++) {
                raw[i] = (uint8_t)p.enums[i];
            }
        } else {
            enums_to_vlog(p.enums.data(), p.bits, raw);
        }
    }
}

// Deposit p.raw, a value in the core's layout, onto the shell's port (next
// delta, like an assignment).
void write_port(const Instance& in, Port& p) {
    const uint8_t* raw = p.raw.data();
    vhpiValueT v{};
    if (p.kind == Port::Integer) {
        int32_t x;
        std::memcpy(&x, raw, 4);
        v.format = vhpiIntVal;
        v.value.intg = x;
    } else if (p.kind == Port::Real) {
        double x;
        std::memcpy(&x, raw, 8);
        v.format = vhpiRealVal;
        v.value.real = x;
    } else if (p.kind == Port::Enum) {
        v.format = vhpiEnumVal;
        v.value.enumv = raw[0];
    } else {
        if (p.xp->vhdl) {
            for (size_t i = 0; i < p.bits; i++) {
                p.enums[i] = raw[i];
            }
        } else {
            vlog_to_enums(raw, p.bits, p.enums.data());
        }
        if (p.kind == Port::Logic) {
            v.format = vhpiLogicVal;
            v.value.enumv = p.enums[0];
        } else {
            v.format = vhpiLogicVecVal;
            v.bufSize = p.bits * sizeof(vhpiEnumT);
            v.numElems = (uint32_t)p.bits;
            v.value.enumvs = p.enums.data();
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

void mark_changed(Port& p) {
    Instance& in = *p.owner;
    if (!p.dirty) {
        p.dirty = true;
        in.changed.push_back(&p);
    }
    in.dirty = true;
    activate(in);
}

void on_input_change(const vhpiCbDataT* cb) {
    mark_changed(*static_cast<Port*>(cb->user_data));
}

// A pass: one kernel delta for every active instance; those still in the
// middle of the time step stay active. It runs in three stages, each over
// all the instances, so that NVC's side (VHPI) and the kernels' are not
// interleaved instance by instance: read the changed inputs (stage_read), run
// each kernel's delta (stage_run), then deposit the changed outputs
// (stage_deposit). Nothing NVC can see changes during the pass: every
// instance reads the values of the same NVC delta, and deposits only take
// effect in the next one. So the stages, and the order of the instances,
// change nothing but the order of the cores' reports.
//
// eop: the pass at the end of an NVC delta's process phase, at most one of
// which may step a core in any NVC delta (see run_pass). arrive: the pass at
// the start of a time step (on_next_time_step), over the instances in
// active, whose kernels are only brought to this time.
struct Pass {
    std::vector<Instance*>& active = g_stepping; // one pass at a time
    uint64_t now_fs = 0;
    long delta = 0;
    bool eop = false;
    bool arrive = false;
};

void stage_read(Instance& in, uint64_t now_fs);
void stage_run(Instance& in, uint64_t now_fs);
bool stage_deposit(Instance& in, uint64_t now_fs);

// True if any output was deposited
bool run_pass(Pass& p) {
    if (!p.arrive) {
        p.active.clear();
        p.active.swap(g_active);
    }
    vhpiTimeT now{};
    vhpi_get_time(&now, &p.delta);
    p.now_fs = ((uint64_t)(uint32_t)now.high << 32) | now.low;
    for (Instance* in : p.active) {
        if (!p.arrive) {
            in->active = false;
        }
        in->arriving = p.arrive;
        stage_read(*in, p.now_fs);
        // Lockstep: a core gets one kernel delta per NVC delta. Passes that
        // run while NVC has nothing pending (the last known delta, a core's
        // initialization) step cores again in the same NVC delta, by design;
        // two passes at the end of one NVC delta's processes never may.
        if (p.eop && in->stepping) {
            if (in->eop_fs == p.now_fs && in->eop_delta == p.delta) {
                fail(std::format("{}: stepped twice in one NVC delta ({} fs, "
                                 "delta {}): a bug in nvcx's scheduling",
                                 in->name,
                                 p.now_fs,
                                 p.delta));
            }
            in->eop_fs = p.now_fs;
            in->eop_delta = p.delta;
        }
    }
    for (Instance* in : p.active) {
        stage_run(*in, p.now_fs);
    }
    bool deposited = false;
    for (Instance* in : p.active) {
        deposited |= stage_deposit(*in, p.now_fs);
        if (in->sim->suspended()) {
            activate(*in);
        }
    }
    return deposited;
}

bool step_active(bool eop = false) {
    Pass p;
    p.eop = eop;
    return run_pass(p);
}

// At the end of each NVC delta's process phase: the delta's inputs, and its
// value-change callbacks, which NVC runs among the processes, are all in
void on_end_of_processes(const vhpiCbDataT*) {
    step_active(true);
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

// At the start of each time step, before NVC's own signal updates: the
// kernels whose next event is now are brought to this time, through their
// first delta's signal updates, and the outputs those change are deposited
// into NVC's update phase (see the header comment). Their processes wait for
// the end of NVC's, in the usual pass.
void on_next_time_step(const vhpiCbDataT*) {
    vhpiTimeT now{};
    vhpi_get_time(&now, nullptr);
    const uint64_t now_fs = ((uint64_t)(uint32_t)now.high << 32) | now.low;
    Pass p;
    p.arrive = true;
    p.active.clear();
    for (auto& inst : g_instances) {
        if (inst->timeout && inst->timeout_at == now_fs) {
            p.active.push_back(inst.get());
        }
    }
    if (p.active.empty()) {
        return;
    }
    run_pass(p);
    for (Instance* in : p.active) {
        in->due = true;
        activate(*in);
    }
}

// Stage A (NVC): whether the instance has anything to do in this pass, and
// the values of its changed inputs. in.changed keeps only the ports whose
// value differs from the kernel's, with the new value in their last.
void stage_read(Instance& in, uint64_t now_fs) {
    in.stepping = in.arriving || in.dirty || in.due || in.sim->suspended();
    if (!in.stepping) {
        return;
    }
    if (now_fs % in.fs_per_tick != 0) {
        fail(std::format("{}: NVC time {} fs is not a multiple of the "
                         "kernel's {} fs tick; elaborate the core with a finer "
                         "precision",
                         in.name,
                         now_fs,
                         in.fs_per_tick));
    }
    if (in.arriving) {
        return; // the inputs wait for the usual pass
    }
    // only the inputs whose value-change callbacks fired (reading through
    // VHPI is the costly part)
    size_t n = 0;
    for (Port* p : in.changed) {
        p->dirty = false;
        read_port(in, *p);
        if (p->raw != p->last) {
            p->last = p->raw;
            in.changed[n++] = p;
        }
    }
    in.changed.resize(n);
    in.dirty = false;
    in.due = false;
}

// Stage B (the kernel): one delta, and what it leaves for NVC: the outputs
// that changed, the run's status and, once quiescent, the next event.
void stage_run(Instance& in, uint64_t now_fs) {
    if (!in.stepping) {
        return;
    }
    Simulation& d = *in.sim;
    const uint64_t target = now_fs / in.fs_per_tick;
    using Status = Simulation::RunStatus;
    Simulation::RunResult r{Status::Completed, ""};
    // Bring the kernel to this time, paused after the signal updates of its
    // first delta here (its own events) and before their processes. A pause
    // at an earlier time has nothing for NVC: nothing was scheduled there.
    if (!d.suspended()) {
        r = d.start(target > d.time() ? target - d.time() : 0);
        while (r.status == Status::Yielded && d.time() < target) {
            r = d.resume();
        }
    }
    if (r.status != Status::Error && !in.arriving) {
        // the changed inputs, for the processes about to run
        const bool changed = !in.changed.empty();
        for (Port* p : in.changed) {
            d.put(*p->xp, p->last.data());
        }
        in.changed.clear();
        // one delta: the processes, then the updates they schedule
        if (d.suspended()) {
            r = d.resume();
        } else if (changed) {
            r = d.start(0);
            if (r.status == Status::Yielded) {
                r = d.resume();
            }
        }
    }
    if (r.status == Status::Error) {
        fail(in.name + ": " + r.message);
    }
    in.outs_changed.clear();
    for (Port& p : in.outs) {
        d.get(*p.xp, p.raw.data());
        if (p.raw != p.last) {
            p.last = p.raw;
            in.outs_changed.push_back(&p);
        }
    }
    uint64_t next;
    in.next_pending =
        r.status != Status::Yielded && d.next_event_time(next) && !d.finished();
    if (in.next_pending) {
        in.next_at =
            now_fs + (next > d.time() ? next - d.time() : 1) * in.fs_per_tick;
    }
    in.result = std::move(r);
}

// Stage C (NVC): deposit the changed outputs (in their raw) and keep the
// after-delay callback for the kernel's next event. True if anything was
// deposited.
bool stage_deposit(Instance& in, uint64_t now_fs) {
    if (!in.stepping) {
        return false;
    }
    in.stepping = false;
    in.arriving = false;
    using Status = Simulation::RunStatus;
    if (in.result.status == Status::Finished && !in.finished_reported) {
        in.finished_reported = true;
        vhpi_printf("nvcx: %s: the core finished its simulation; its outputs "
                    "are frozen",
                    in.name.c_str());
    }
    const bool deposited = !in.outs_changed.empty();
    for (Port* p : in.outs_changed) {
        write_port(in, *p);
    }
    in.outs_changed.clear();
    if (in.result.status == Status::Yielded) {
        // more deltas at this time. A pending timeout stays: the kernel's
        // next event is settled once it is quiescent, and should the timeout
        // fire first, the step it causes finds nothing to do.
        return deposited;
    }
    // quiescent: the kernel's next event becomes an after-delay callback.
    // Registering one costs NVC a search of its handle table, so one already
    // pending for that time is kept.
    if (in.timeout && !(in.next_pending && in.next_at == in.timeout_at)) {
        vhpi_remove_cb(in.timeout);
        in.timeout = nullptr;
    }
    if (in.next_pending && !in.timeout) {
        uint64_t delay_fs = in.next_at - now_fs;
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
        in.timeout_at = in.next_at;
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
        // A basic identifier (any case), or else an extended one: a Verilog
        // name that is a VHDL reserved word, or not a basic identifier, can
        // only be declared as \next\, verbatim
        std::string port_name = shell + lower(xp.name);
        vhpiHandleT h =
            vhpi_handle_by_name(const_cast<char*>(port_name.c_str()), nullptr);
        if (!h) {
            port_name = shell + "\\" + xp.name + "\\";
            h = vhpi_handle_by_name(const_cast<char*>(port_name.c_str()),
                                    nullptr);
        }
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
    // (in.ins is complete: the callbacks keep pointers into it)
    for (Port& p : in.ins) {
        p.owner = &in;
        vhpiCbDataT change{.reason = vhpiCbValueChange,
                           .cb_rtn = on_input_change,
                           .obj = p.handle,
                           .user_data = &p};
        vhpiHandleT h = vhpi_register_cb(&change, vhpiReturnCb);
        if (!h) {
            fail(std::format(
                "{}: cannot register the value-change callback on {}",
                name,
                p.name()));
        }
        // the callback outlives its handle; kept, the handles of every
        // input of every instance slow NVC's search for a free one
        vhpi_release_handle(h);
    }
    // Initial values now: the cycle callbacks do not run during
    // initialization, and nothing on NVC's side can see the core until its
    // first delta, so its initialization runs on until an output changes.
    for (Port& p : in.ins) {
        mark_changed(p);
    }
    in.dirty = true; // (a core without inputs too)
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
