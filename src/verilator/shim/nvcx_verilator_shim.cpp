// The shim between nvcx and one verilated model, compiled with it into
// verilator.so by nvcx.tcl (nvcx::verilate). The model is always verilated
// as Vcore (--prefix), with --timing; nvcx.tcl generates nvcx_ports.h from
// Vcore.h, one NVCX_PORT(name, dir, width, member) per top-level port, and
// nvcx_threads.h from Vcore.cpp, the threads it was verilated for.
//
// It implements nvcx_verilator_abi.h, and what nvcx_verilator_hooks.h asks
// for: the model's printing, which reaches the sink of the model being run
// (set around every call into it) rather than stdout, and the runtime's
// vl_stop_maybe and vl_fatal.

// (first: Verilator's headers act on it)
#include "nvcx_verilator_hooks.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include "Vcore.h"
#include "nvcx_threads.h"
#include "nvcx_verilator_abi.h"
#include "verilated.h"

struct nvcx_vlt_model {
    nvcx_vlt_sink sink;
    std::unique_ptr<VerilatedContext> context;
    std::unique_ptr<Vcore> top;
    std::string line;           // printed text not yet ended by a newline
    bool error_printed = false; // the last line printed was an %Error report
    bool evaluated = false;     // (so final blocks run only after initial ones)
};

namespace {

// vl_fatal's way out of eval: Verilator's runtime does not expect it to return
struct Fatal {};

// The model being run on this thread, whose sink gets what it prints
thread_local nvcx_vlt_model* t_current = nullptr;

// "[T] %Error: ..." or "%Error-CODE: ...", as $error and a failed assertion
// print before they stop (maybe)
bool is_error_report(const std::string& line) {
    size_t pos = 0;
    if (!line.empty() && line[0] == '[') {
        pos = line.find("] ");
        if (pos == std::string::npos)
            return false;
        pos += 2;
    }
    return line.compare(pos, 6, "%Error") == 0;
}

void flush_line(nvcx_vlt_model& m) {
    if (!m.line.empty()) {
        m.error_printed = is_error_report(m.line);
        std::string line;
        line.swap(m.line);
        m.sink.print(m.sink.user, line.c_str());
    }
}

struct Current {
    nvcx_vlt_model* outer = t_current;
    explicit Current(nvcx_vlt_model* m) {
        t_current = m;
        Verilated::threadContextp(m->context.get());
    }
    ~Current() {
        flush_line(*t_current);
        t_current = outer;
        if (outer)
            Verilated::threadContextp(outer->context.get());
    }
};

void emit(const char* text, size_t n) {
    nvcx_vlt_model* m = t_current;
    if (!m) {
        fwrite(text, 1, n, stdout);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n')
            flush_line(*m);
        else
            m->line += text[i];
    }
}

// Values as 32-bit words: a port is CData, SData, IData or QData by width,
// or VlWide<N> above 64 bits; Verilator keeps the bits above the width zero
template <typename T> void load(const T& v, uint32_t* w, unsigned width) {
    const uint64_t x = v;
    w[0] = (uint32_t)x;
    if (width > 32)
        w[1] = (uint32_t)(x >> 32);
}
template <std::size_t N> void load(const VlWide<N>& v, uint32_t* w, unsigned) {
    for (std::size_t i = 0; i < N; i++)
        w[i] = v[i];
}
template <typename T> void store(T& v, const uint32_t* w, unsigned width) {
    uint64_t x = w[0];
    if (width > 32)
        x |= (uint64_t)w[1] << 32;
    if (width < 64)
        x &= ((uint64_t)1 << width) - 1;
    v = (T)x;
}
template <std::size_t N>
void store(VlWide<N>& v, const uint32_t* w, unsigned width) {
    for (std::size_t i = 0; i < N; i++)
        v[i] = w[i];
    if (width % 32)
        v[N - 1] &= ((uint32_t)1 << (width % 32)) - 1;
}

// One entry per port, then a terminator (a model may have no ports)
const nvcx_vlt_port k_ports[] = {
#define NVCX_PORT(name, dir, width, member) {name, dir, width},
#include "nvcx_ports.h"
#undef NVCX_PORT
    {nullptr, 0, 0}};

using Getter = void (*)(const Vcore&, uint32_t*);
const Getter k_getters[] = {
#define NVCX_PORT(name, dir, width, member)                                    \
    [](const Vcore& m, uint32_t* w) {                                          \
        load(m.member, w, width);                                              \
    },
#include "nvcx_ports.h"
#undef NVCX_PORT
    nullptr};

using Putter = void (*)(Vcore&, const uint32_t*);
const Putter k_putters[] = {
#define NVCX_PORT(name, dir, width, member)                                    \
    [](Vcore& m, const uint32_t* w) {                                          \
        store(m.member, w, width);                                             \
    },
#include "nvcx_ports.h"
#undef NVCX_PORT
    nullptr};

nvcx_vlt_model* vlt_open(const nvcx_vlt_sink* sink) {
    try {
        auto m = std::make_unique<nvcx_vlt_model>();
        m->sink = *sink;
        m->context = std::make_unique<VerilatedContext>();
        // the threads the model was verilated for (1 without --threads): the
        // context would otherwise default to a thread per CPU, and Vcore's
        // constructor start that many workers, but one, in every instance,
        // where a single-threaded model never uses them
        m->context->threads(NVCX_VLT_THREADS);
        // $stop and $fatal finish the model, not the process; $error never
        // does (vl_stop_maybe), and the runtime's accounting agrees
        m->context->fatalOnError(false);
        m->context->errorLimit(std::numeric_limits<int>::max());
        Current current(m.get());
        // named as the Verilog top, so that %m reads as it would elsewhere
        m->top = std::make_unique<Vcore>(m->context.get(), "");
        return m.release();
    } catch (...) {
        return nullptr;
    }
}

void vlt_close(nvcx_vlt_model* m) {
    {
        Current current(m);
        try {
            if (m->evaluated)
                m->top->final();
        } catch (const Fatal&) {
        }
        m->top.reset();
    }
    delete m;
}

void vlt_get(const nvcx_vlt_model* m, unsigned port, uint32_t* words) {
    k_getters[port](*m->top, words);
}

void vlt_put(nvcx_vlt_model* m, unsigned port, const uint32_t* words) {
    k_putters[port](*m->top, words);
}

int vlt_eval(nvcx_vlt_model* m) {
    Current current(m);
    m->evaluated = true;
    try {
        m->top->eval();
    } catch (const Fatal&) {
        m->context->gotFinish(true);
        return -1;
    }
    return 0;
}

uint64_t vlt_time(const nvcx_vlt_model* m) {
    return m->context->time();
}

void vlt_set_time(nvcx_vlt_model* m, uint64_t t) {
    m->context->time(t);
}

int vlt_precision(const nvcx_vlt_model* m) {
    return m->context->timeprecision();
}

int vlt_next_event(const nvcx_vlt_model* m, uint64_t* t) {
    // (nextTimeSlot() is fatal when nothing is pending)
    if (!m->top->eventsPending())
        return 0;
    *t = m->top->nextTimeSlot();
    return 1;
}

int vlt_finished(const nvcx_vlt_model* m) {
    return m->context->gotFinish();
}

const nvcx_vlt_api k_api = {
    .abi_version = NVCX_VLT_ABI_VERSION,
    .verilator_version = Verilated::productVersion(),
    .nports = sizeof k_ports / sizeof k_ports[0] - 1,
    .ports = k_ports,
    .open = vlt_open,
    .close = vlt_close,
    .get = vlt_get,
    .put = vlt_put,
    .eval = vlt_eval,
    .time = vlt_time,
    .set_time = vlt_set_time,
    .precision = vlt_precision,
    .next_event = vlt_next_event,
    .finished = vlt_finished,
};

} // namespace

extern "C" const nvcx_vlt_api* nvcx_verilator_api(void) {
    return &k_api;
}

// The hooks (nvcx_verilator_hooks.h)

int nvcx_vl_vprintf(const char* format, va_list ap) {
    va_list again;
    va_copy(again, ap);
    char small[256];
    int n = vsnprintf(small, sizeof small, format, ap);
    if (n >= 0 && (size_t)n < sizeof small) {
        emit(small, n);
    } else if (n >= 0) {
        std::string big((size_t)n + 1, '\0');
        vsnprintf(big.data(), big.size(), format, again);
        emit(big.data(), n);
    }
    va_end(again);
    return n;
}

int nvcx_vl_printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = nvcx_vl_vprintf(format, ap);
    va_end(ap);
    return n;
}

// $error and failed assertions only count: what they mean for the
// simulation is NVC's call (--exit-severity), as for any other assertion.
// $stop and $fatal stop, as Verilator's would: with fatalOnError off, its
// vl_stop reports "%Error: ...: Verilog $stop", finishes the model and
// returns. $fatal comes here not maybe, but $stop maybe, as $error and a
// failed assertion do (all three are counted towards Verilator's
// +verilator+error+limit); what tells them apart is that $error and the
// assertion have just printed their %Error report, and $stop nothing.
void vl_stop_maybe(const char* filename,
                   int linenum,
                   const char* hier,
                   bool maybe) VL_MT_UNSAFE {
    nvcx_vlt_model* m = t_current;
    const bool reported = m && m->error_printed && m->line.empty();
    if (m)
        m->error_printed = false; // (a $stop after $error stops)
    Verilated::threadContextp()->errorCountInc();
    if (!maybe || !reported)
        vl_stop(filename, linenum, hier);
}

// The runtime's own fatal errors (a region that does not converge, say).
// Verilator's would exit; the runtime does not expect it to return at all,
// so this unwinds to eval, which finishes the model. (A coroutine aborts on
// an exception, but the runtime raises these outside them.)
void vl_fatal(const char* filename, int linenum, const char*, const char* msg)
    VL_MT_UNSAFE {
    VerilatedContext* context = Verilated::threadContextp();
    context->gotError(true);
    context->gotFinish(true);
    nvcx_vl_printf("%%Error: %s:%d: %s\n",
                   filename ? filename : "",
                   linenum,
                   msg ? msg : "");
    throw Fatal{};
}
