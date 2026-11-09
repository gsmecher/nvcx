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


def pytest_collection_modifyitems(config, items):
    """Deselect the deep XPM tier unless a -m expression names xpm_deep, so that it does not show
    as a skip on every default run."""
    if "xpm_deep" in config.getoption("markexpr"):
        return
    deep = [item for item in items if item.get_closest_marker("xpm_deep")]
    if deep:
        config.hook.pytest_deselected(items=deep)
        items[:] = [item for item in items if not item.get_closest_marker("xpm_deep")]


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
    "kwports_v": ("sv", "kwports.v", "kwports"),
    # Verilator's delta-cycle concessions (tests/test_verilator.py)
    "regcomb_v": ("sv", "regcomb.v", "regcomb"),
    "clkgen_v": ("sv", "clkgen.v", "clkgen"),
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
def verilator():
    if shutil.which("verilator") is None:
        pytest.skip("verilator not on PATH")
    return shutil.which("verilator")


@pytest.fixture(scope="session")
def verilator_env(verilator):
    """Environment for a run whose cores are all Verilator's: no Vivado needed."""
    return dict(os.environ)


@pytest.fixture(scope="session")
def verilated(verilator, build, tmp_path_factory):
    """verilated(name) -> the snapshot directory of that (Verilog) test design's model, built on
    first use by nvcx::verilate, as nvcx.tcl builds it for a design."""
    out = tmp_path_factory.mktemp("vl")
    done = {}

    def verilate(name):
        if name not in done:
            lang, source, top = DESIGNS[name]
            assert lang == "sv", f"{name} is not a Verilog design"
            script = (
                "package require nvcx\n"
                f"puts [nvcx::verilate {top} {{{RTL / source}}}]\n"
            )
            r = subprocess.run(
                ["tclsh"],
                input=script,
                env=dict(os.environ, TCLLIBPATH=str(build / "tcl"), NVCX_WORK=str(out)),
                capture_output=True,
                text=True,
            )
            assert r.returncode == 0, r.stdout + r.stderr
            done[name] = Path(r.stdout.strip().splitlines()[-1])
        return done[name]

    return verilate


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
