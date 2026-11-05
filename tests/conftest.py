"""Shared fixtures: the built tree, Vivado's kernel, NVC, and the test designs elaborated on
demand into a session directory."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
RTL = ROOT / "tests" / "rtl"
TB = ROOT / "tests" / "tb"

# The cores under tests/rtl, in VHDL and in Verilog: snapshot name -> (language, source, top entity)
DESIGNS = {
    "dut": ("vhdl", "dut.vhd", "dut"),
    "dut_v": ("sv", "dut.v", "dut"),
    "exp": ("vhdl", "exp.vhd", "exp"),
    "exp_v": ("sv", "exp.v", "exp"),
    "hier": ("vhdl", "hier.vhd", "hier"),
    "hier_v": ("sv", "hier.v", "hier"),
    "kinds": ("vhdl", "kinds.vhd", "kinds"),
    "kinds_v": ("sv", "kinds.v", "kinds"),
    "sev": ("vhdl", "sev.vhd", "sev"),
    "sev_v": ("sv", "sev.v", "sev"),
    # delta-ordering cores (tests/test_delta.py)
    "clkgen": ("vhdl", "clkgen.vhd", "clkgen"),
    "regcomb": ("vhdl", "regcomb.vhd", "regcomb"),
    "glitch": ("vhdl", "glitch.vhd", "glitch"),
    # property-based (tests/test_fuzz.py)
    "pdelay": ("vhdl", "pdelay.vhd", "pdelay"),
    "rstsync": ("vhdl", "rstsync.vhd", "rstsync"),
    "chain": ("vhdl", "chain.vhd", "chain"),
    "clkdiv": ("vhdl", "clkdiv.vhd", "clkdiv"),
    "pulsesync": ("vhdl", "pulsesync.vhd", "pulsesync"),
    "ring": ("vhdl", "ring.vhd", "ring"),
    # regressions (tests/test_regress.py)
    "strgen": ("vhdl", "strgen.vhd", "strgen"),
    "strgen_v": ("sv", "strgen.v", "strgen"),
}


@pytest.fixture(scope="session")
def build():
    cand = Path(os.environ.get("NVCX_BUILD") or ROOT / "build")
    if not (cand / "libnvcx.so").is_file():
        pytest.skip("project not built (run `make` or set NVCX_BUILD)")
    return cand


@pytest.fixture(scope="session")
def vivado():
    if not os.environ.get("XILINX_VIVADO") or shutil.which("xelab") is None:
        pytest.skip("XILINX_VIVADO not set or xelab not on PATH")
    return Path(os.environ["XILINX_VIVADO"])


@pytest.fixture(scope="session")
def nvc():
    cand = os.environ.get("NVC") or shutil.which("nvc")
    if not cand:
        pytest.skip("nvc not found (set NVC or put it on PATH)")
    probe = subprocess.run([cand, "--do", "/dev/null"], capture_output=True, text=True)
    if "without TCL" in probe.stderr:
        pytest.skip(f"{cand} was built without --enable-tcl, which nvcx.tcl needs")
    return cand


@pytest.fixture(scope="session")
def bridge_env(vivado):
    """Environment for the CLI and the plugin: XILINX_VIVADO is all they need."""
    return dict(os.environ)


@pytest.fixture(scope="session")
def designs(vivado, tmp_path_factory):
    """designs(name) -> the snapshot directory (xsim.dir/<name>) of that test design, elaborated on
    first use (xelab -dll); designs.all() elaborates
    every design and returns the session directory holding xsim.dir."""
    out = tmp_path_factory.mktemp("rtl")
    done = {}

    def elaborate(name):
        if name not in done:
            lang, source, top = DESIGNS[name]
            compiler = ["xvhdl", "--2008"] if lang == "vhdl" else ["xvlog", "--sv"]
            subprocess.run(
                [*compiler, str(RTL / source)],
                cwd=out,
                check=True,
                stdout=subprocess.DEVNULL,
            )
            subprocess.run(
                ["xelab", "-dll", f"work.{top}", "-s", name],
                cwd=out,
                check=True,
                stdout=subprocess.DEVNULL,
            )
            done[name] = out / "xsim.dir" / name
        return done[name]

    elaborate.dir = out
    elaborate.all = lambda: [elaborate(n) for n in DESIGNS] and out
    return elaborate
