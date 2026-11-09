/* The interface between nvcx's Verilator backend (src/verilator) and a
 * verilated model's shim (nvcx_verilator_shim.cpp), which nvcx.tcl compiles
 * with the model into verilator.so. The backend dlopens that and calls
 * nvcx_verilator_api(); everything else in it is hidden.
 *
 * A model's own Verilator runtime is linked into its .so, so models built by
 * different Verilator releases, or with different options, can share a
 * process. C, so that the two sides need not share a C++ ABI either. */

#ifndef NVCX_VERILATOR_ABI_H
#define NVCX_VERILATOR_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever anything below changes: the backend refuses a model built
 * against another version (rebuild it with this nvcx's nvcx.tcl) */
#define NVCX_VLT_ABI_VERSION 1

    enum {
        NVCX_VLT_IN = 1,
        NVCX_VLT_OUT = 2,
        NVCX_VLT_INOUT = 3,
    };

    /* A top-level port. Its value crosses as 32-bit words, least significant
     * first, (width + 31) / 32 of them; bits above the width are zero. */
    typedef struct nvcx_vlt_port {
        const char* name; /* the Verilog name */
        int dir;          /* NVCX_VLT_IN, ... */
        unsigned width;   /* bits */
    } nvcx_vlt_port;

    /* Where a model's output goes: each line it writes, without its newline, as
     * Verilator would print it. That is $display's, the messages of $info and
     * friends ("[T] -Info: file:line: hier: text", %Warning, %Error, %Fatal),
     * and the runtime's own ("%Error: file:line: Verilog $stop", ...). It runs
     * inside the call (eval, close) that caused it. */
    typedef struct nvcx_vlt_sink {
        void* user;
        void (*print)(void* user, const char* line);
    } nvcx_vlt_sink;

    typedef struct nvcx_vlt_model nvcx_vlt_model;

    typedef struct nvcx_vlt_api {
        unsigned abi_version;          /* NVCX_VLT_ABI_VERSION */
        const char* verilator_version; /* of the runtime linked in */
        unsigned nports;
        const nvcx_vlt_port* ports;

        /* A new instance of the model, at time 0 and not yet evaluated, with
         * its own VerilatedContext; NULL on failure. The sink is copied. */
        nvcx_vlt_model* (*open)(const nvcx_vlt_sink* sink);
        /* Run its final blocks, if it was evaluated, and free it */
        void (*close)(nvcx_vlt_model* m);

        /* Port *port*'s value, as words (see nvcx_vlt_port) */
        void (*get)(const nvcx_vlt_model* m, unsigned port, uint32_t* words);
        void (*put)(nvcx_vlt_model* m, unsigned port, const uint32_t* words);

        /* Evaluate the current time slot to convergence. 0 on success, or -1 if
         * the runtime hit a fatal error (reported through the sink first),
         * after which the model is finished and must not be evaluated again. */
        int (*eval)(nvcx_vlt_model* m);

        /* Time, in units of 10^precision seconds */
        uint64_t (*time)(const nvcx_vlt_model* m);
        void (*set_time)(nvcx_vlt_model* m, uint64_t t);
        int (*precision)(const nvcx_vlt_model* m);
        /* The time of the next scheduled event, or 0 if there is none */
        int (*next_event)(const nvcx_vlt_model* m, uint64_t* t);
        /* Whether the design has ended its simulation ($finish, $stop, ...) */
        int (*finished)(const nvcx_vlt_model* m);
    } nvcx_vlt_api;

    /* The one symbol a model's .so exports */
    const nvcx_vlt_api* nvcx_verilator_api(void);

#ifdef __cplusplus
}
#endif

#endif /* NVCX_VERILATOR_ABI_H */
