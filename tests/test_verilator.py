"""Cores simulated by Verilator (src/verilator), through the same bridge and testbenches as xsim's.

The Verilog cores under tests/rtl, verilated by nvcx::verilate, behind testbenches that check what
a 2-state model that settles a whole time slot at once can promise: values crossing both ways, the
core's own timed events, reports at their severities, generics, port names. None of these needs
Vivado, except the mixed case, which puts an xsim core and Verilator cores in one simulation.

The two concessions the README documents are pinned as expected failures, against the testbenches
xsim must pass: should Verilator's behaviour change, these fail and the README needs revisiting.
"""

import os
import subprocess
from pathlib import Path

import pytest

from test_nvc import Case, Core, run_case


def vl(name, design, **kw):
    return Core(name, design, backend="verilator", **kw)


SEVERITIES = [
    "** Note: 1ns+1: :tb_sev:s:core:: sev note",
    "** Warning: 1ns+1: :tb_sev:s:core:: sev warning",
    "** Error: 1ns+1: :tb_sev:s:core:: sev error",
    "** Failure: 1ns+1: :tb_sev:s:core:: sev failure",
]

CASES = {
    # combinational and registered paths, and the core's own free-running oscillator (#5 delays),
    # during activity and idle
    "exp": Case([vl("exp", "exp_v")], "tb_exp.vhd", "tb_exp", "250ns"),
    # two instances of one model and one of another, each with its own VerilatedContext
    "multi": Case(
        [vl("dut", "dut_v"), vl("exp", "exp_v")], "tb_multi.vhd", "tb_multi", "100ns"
    ),
    # 2-state: X, Z, U, W and L read as 0 inside the core, H as 1
    "two_state": Case([vl("dut_v", "dut_v")], "tb_vlog2.vhd", "tb_vlog2", "100ns"),
    # ports wider than 64 bits, as several words each way
    "wide": Case(
        [vl("dut", "dut_v", per_generics=True)],
        "tb_wide.vhd",
        "tb_wide",
        "100ns",
        sources=["dut_comp.vhd"],
    ),
    # a model Verilator splits into several files, which it compiles with precompiled headers: the
    # harness's hooks must not come before them (GCC then ignores the .gch, and the build fails)
    "output_split": Case(
        [
            vl(
                "exp",
                "exp_v",
                per_generics=True,
                vargs=["--output-split", "1", "--output-split-cfuncs", "1"],
            )
        ],
        "tb_exp.vhd",
        "tb_exp",
        "250ns",
    ),
    # a model built with --threads (Verilator's own threads, within one model) still works through
    # the harness, should a core's arguments ask for it
    "threads": Case(
        [vl("exp", "exp_v", per_generics=True, vargs=["--threads", "2"])],
        "tb_exp.vhd",
        "tb_exp",
        "250ns",
    ),
    # one component, two generic sets: a model verilated per set (-G)
    "generic": Case(
        [vl("dut", "dut_v", per_generics=True)],
        "tb_generic.vhd",
        "tb_generic",
        "100ns",
        sources=["dut_comp.vhd"],
        tcl_expect=[
            "verilating dut for :TB_GENERIC:NARROW",
            "verilating dut for :TB_GENERIC:WIDE",
        ],
    ),
    # string generics, quoted for -G
    "string_generic": Case(
        [vl("strgen", "strgen_v", per_generics=True)],
        "tb_strgen.vhd",
        "tb_strgen",
        "10ns",
        sources=["strgen_comp.vhd"],
    ),
    # Verilog port names VHDL can only spell as extended identifiers
    "extended_identifier_ports": Case(
        [vl("kwports", "kwports_v", per_generics=True)],
        "tb_kwports.vhd",
        "tb_kwports",
        "10ns",
        sources=["kwports_comp.vhd"],
    ),
    # $info, $warning, $error and $fatal arrive as NVC's assertions at their severities, in order;
    # $error does not stop the model (NVC's --exit-severity decides), and $fatal ends the run
    "severity": Case(
        [vl("sev", "sev_v")],
        "tb_sev.vhd",
        "tb_sev",
        "100ns",
        run_args=["--exit-severity=failure"],
        expect_failure=SEVERITIES,
        expect_absent=["FAIL"],
        # reported once: the failure must not unwind through the model's evaluation, nor its
        # report be left to be reported again when the model closes
        expect_once=["sev failure"],
    ),
    "severity_exit": Case(
        [vl("sev", "sev_v")],
        "tb_sev.vhd",
        "tb_sev",
        "100ns",
        run_args=["--exit-severity=error"],
        expect_failure=SEVERITIES[:3],
        expect_absent=["sev failure", "FAIL"],
    ),
    # an assertion in the core fails the run, which never reaches the testbench's own FAIL
    "doom": Case(
        [vl("dut", "dut_v")],
        "tb_doom.vhd",
        "tb_doom",
        "100ns",
        expect_failure=["** Failure:", "DOOM!"],
        expect_absent=["FAIL"],
        expect_once=["DOOM!"],
    ),
    # $stop is an error that finishes the model, as Verilator reports it standalone. Verilator
    # calls the runtime for $stop as it does for $error, which only counts: the shim once let
    # $stop only count too, silently, and the run passed.
    "stop": Case(
        [vl("stop", "stop_v")],
        "tb_stop.vhd",
        "tb_stop",
        "100ns",
        expect_failure=[
            "** Error: 25ns+1: :tb_stop:s:core:: Verilog $stop",
            "the core finished its simulation",
            "PASS",
        ],
    ),
    # Concession 1: a time slot settles at once, so a combinational output derived from a
    # registered one changes in the same NVC delta as it, not one later (xsim passes this
    # testbench: tests/test_delta.py)
    "same_delta": Case(
        [vl("regcomb", "regcomb_v")],
        "tb_regcomb.vhd",
        "tb_regcomb",
        "120ns",
        expect_failure=["nq changed in the same delta as q"],
    ),
    # Concession 2, and why a Verilator core must not drive a clock NVC uses: the core's clock and
    # the counter registered on it change in the same NVC delta, so a register outside the core
    # on that clock samples the counter after its update (xsim passes: tests/test_delta.py)
    "clock_output": Case(
        [vl("clkgen", "clkgen_v")],
        "tb_clkgen.vhd",
        "tb_clkgen",
        "120ns",
        expect_failure=["sampled its counter after the update"],
    ),
}


@pytest.mark.parametrize("name", CASES)
def test_verilator(name, nvc, build, verilator_env, verilated, tmp_path):
    run_case(CASES[name], nvc, build, verilator_env, None, tmp_path, verilated)


# xsim and Verilator cores in one simulation: tb_multi's two dut instances on xsim (the VHDL
# core), its exp on Verilator
MIXED = Case(
    [Core("dut", "dut"), vl("exp", "exp_v")], "tb_multi.vhd", "tb_multi", "100ns"
)


def test_mixed(nvc, build, bridge_env, designs, verilated, tmp_path):
    run_case(MIXED, nvc, build, bridge_env, designs, tmp_path, verilated)


def test_cache(verilator, build, tmp_path):
    """nvcx::verilate's cache follows its sources' contents: a source rewritten unchanged keeps the
    model, a changed one rebuilds it."""
    src = tmp_path / "konst.v"
    script = f"package require nvcx\nputs [nvcx::verilate konst {{{src}}}]\n"
    env = dict(
        os.environ, TCLLIBPATH=str(build / "tcl"), NVCX_WORK=str(tmp_path / "work")
    )

    def model():
        r = subprocess.run(
            ["tclsh"], input=script, env=env, capture_output=True, text=True
        )
        assert r.returncode == 0, r.stdout + r.stderr
        so = Path(r.stdout.split()[-1]) / "verilator.so"
        return so.stat().st_ino

    src.write_text("module konst (output [7:0] k); assign k = 8'd1; endmodule\n")
    first = model()
    src.write_text(src.read_text())  # a new mtime, the same contents
    assert model() == first
    src.write_text("module konst (output [7:0] k); assign k = 8'd2; endmodule\n")
    assert model() != first


# Opens a model's instances through the shim's C API (nvcx_verilator_abi.h), in a process of its
# own, and prints how many threads that added
THREADS_PROBE = """
import ctypes, os, sys
SINK_PRINT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)
class Sink(ctypes.Structure):
    _fields_ = [("user", ctypes.c_void_p), ("print", SINK_PRINT)]
class Api(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint),
        ("verilator_version", ctypes.c_char_p),
        ("nports", ctypes.c_uint),
        ("ports", ctypes.c_void_p),
        ("open", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.POINTER(Sink))),
        ("close", ctypes.CFUNCTYPE(None, ctypes.c_void_p)),
    ]
lib = ctypes.CDLL(sys.argv[1])
lib.nvcx_verilator_api.restype = ctypes.POINTER(Api)
api = lib.nvcx_verilator_api().contents
sink = Sink(None, SINK_PRINT(lambda user, line: None))
before = len(os.listdir("/proc/self/task"))
models = [api.open(ctypes.byref(sink)) for _ in range(4)]
assert all(models)
print(len(os.listdir("/proc/self/task")) - before)
for m in models:
    api.close(m)
"""


def test_no_threads(verilated):
    """A single-threaded model's instances start no threads: each has a VerilatedContext of its
    own, whose default thread pool (a worker per CPU but one) would otherwise sit idle in every
    instance."""
    so = verilated("dut_v") / "verilator.so"
    r = subprocess.run(
        ["python3", "-I", "-c", THREADS_PROBE, str(so)], capture_output=True, text=True
    )
    assert r.returncode == 0, r.stdout + r.stderr
    assert r.stdout.strip() == "0", f"{r.stdout.strip()} threads started"
