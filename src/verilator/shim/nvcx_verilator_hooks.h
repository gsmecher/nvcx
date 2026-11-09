// Included first in Verilator's runtime objects (make OPT_GLOBAL=-include,
// see nvcx::verilate) and in the harness: Verilator's printing goes through
// these, and nvcx_verilator_shim.cpp provides them, so that a model's output
// reaches nvcx rather than stdout. The model's own code calls the runtime to
// print, so it needs none of this (and must not: it may be compiled with a
// precompiled header, which has to come first).
//
// The shim also replaces two of the runtime's functions (VL_USER_*), the
// minimum for a model to share a process: vl_fatal, which Verilator's would
// end with exit(), and vl_stop_maybe, so that $error only counts (and $stop,
// which Verilator calls it for too, still stops). The rest
// keep Verilator's own behaviour, with fatalOnError off: $stop and $fatal
// finish the model rather than the process.

#ifndef NVCX_VERILATOR_HOOKS_H
#define NVCX_VERILATOR_HOOKS_H

#include <cstdarg>

int nvcx_vl_printf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));
int nvcx_vl_vprintf(const char* format, va_list ap);
// (verilated.cpp calls it, but no header declares it)
void vl_stop_maybe(const char* filename,
                   int linenum,
                   const char* hier,
                   bool maybe);

#define VL_PRINTF nvcx_vl_printf
#define VL_VPRINTF nvcx_vl_vprintf
#define VL_USER_STOP_MAYBE
#define VL_USER_FATAL

#endif
