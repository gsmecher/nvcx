import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TB = ROOT / "tests" / "tb"


@dataclass
class Core:
    name: str
    design: str
    library: str | None = None  # xsim: elaborate per generic set, in this library
    ports: list = field(default_factory=list)  # twin.Port, for a twin
    backend: str = "xsim"  # or "verilator"
    per_generics: bool = False  # verilator: verilate per generic set (-verilator)
    vargs: list[str] = field(
        default_factory=list
    )  # ...with these verilator options too


@dataclass
class Case:
    cores: list[Core]
    tb: str  # testbench file in tests/tb
    top: str
    stop: str
    sources: list[str] = field(
        default_factory=list
    )  # extra tests/tb files to analyse first
    run_args: list[str] = field(default_factory=list)
    check: object = None  # check(tmp_path, result) after a passing run
    expect_failure: list[str] | None = (
        None  # the run must stop, with these texts in its output, in this order
    )
    expect_absent: list[str] = field(
        default_factory=list
    )  # texts the run's output must not have
    expect_once: list[str] = field(
        default_factory=list
    )  # texts the run's output must have exactly once
    tcl_expect: list[str] = field(default_factory=list)  # texts nvcx.tcl must print
    before_run: object = (
        None  # before_run(tmp_path): tamper between nvcx.tcl and the run
    )
    twin: bool = False  # each core as its twin (tests/twin.py), not bare
    elab_args: list[str] = field(default_factory=list)  # e.g. the tb's generics


def ports_in_vcd(tmp_path, r):
    """The shell's ports are ordinary NVC signals: they appear in NVC's own waveform output."""
    text = (tmp_path / "nvc.vcd").read_text()
    assert "cnt_out" in text and "y_comb" in text, text[:2000]


def two_snapshots_then_cached(tmp_path, r):
    snapshots = sorted(p.name for p in (tmp_path / "work" / "snapshots").iterdir())
    assert len(snapshots) == 2 and all(
        s.startswith("dut-") for s in snapshots
    ), snapshots
    r.rerun()


def drop_width_12(tmp_path):
    """Remove the WIDTH=12 snapshot after nvcx.tcl made it: the plugin must not elaborate."""
    for key in (tmp_path / "work" / "snapshots").glob("*/key.txt"):
        if "WIDTH=12" in key.read_text():
            shutil.rmtree(key.parent)


CASES = {
    # combinational and registered paths, and the core's own oscillator, during activity and idle
    "exp": Case([Core("exp", "exp")], "tb_exp.vhd", "tb_exp", "250ns"),
    # two instances of one snapshot and one of another, all on the one kernel
    "multi": Case(
        [Core("dut", "dut"), Core("exp", "exp")], "tb_multi.vhd", "tb_multi", "100ns"
    ),
    # a Verilog-top snapshot behind the testbench's own component declaration of it, 4-state
    # values crossing both ways
    "verilog": Case([Core("dut_v", "dut_v")], "tb_vlog.vhd", "tb_vlog", "100ns"),
    # integer, real and boolean ports cross the boundary with their values intact
    "scalars": Case([Core("hier", "hier")], "tb_hier.vhd", "tb_hier", "10ns"),
    # the shell's ports are NVC signals: present in NVC's waveform output
    "waves": Case(
        [Core("exp", "exp")],
        "tb_exp.vhd",
        "tb_exp",
        "250ns",
        run_args=["--wave=nvc.vcd", "--format=vcd"],
        check=ports_in_vcd,
    ),
    # a component from a package, instantiated with two generic sets: one shell, one snapshot per set
    "generic": Case(
        [Core("dut", "dut", library="work")],
        "tb_generic.vhd",
        "tb_generic",
        "100ns",
        sources=["dut_comp.vhd"],
        check=two_snapshots_then_cached,
        tcl_expect=[
            "elaborating work.dut for :TB_GENERIC:NARROW",
            "elaborating work.dut for :TB_GENERIC:WIDE",
        ],
    ),
    # the plugin never elaborates: a snapshot gone missing is a warning naming the script to
    # re-run, and its instance a black box, so the testbench's own check of that instance fails
    "generic_missing": Case(
        [Core("dut", "dut", library="work")],
        "tb_generic.vhd",
        "tb_generic",
        "100ns",
        sources=["dut_comp.vhd"],
        before_run=drop_width_12,
        expect_failure=["WIDTH=12", "is missing", "re-run", "black box"],
    ),
    # a report of severity failure inside the core arrives as an NVC assertion failure and ends
    # the run; the testbench's own "FAIL" report must never be reached
    "doom": Case(
        [Core("dut", "dut")],
        "tb_doom.vhd",
        "tb_doom",
        "100ns",
        expect_failure=["** Failure:", "DOOM!"],
        expect_absent=["FAIL"],
    ),
    # the core's reports and assertions at every severity arrive as NVC's, at their severity, in
    # the order the core made them, the failure ending the run
    "severity": Case(
        [Core("sev", "sev")],
        "tb_sev.vhd",
        "tb_sev",
        "100ns",
        run_args=["--exit-severity=failure"],
        expect_failure=[
            "** Note: 1ns+1: :tb_sev:s:core:: sev note",
            "** Warning: 1ns+1: :tb_sev:s:core:: sev warning",
            "** Error: 1ns+1: :tb_sev:s:core:: sev error",
            "** Failure: 1ns+1: :tb_sev:s:core:: sev failure",
        ],
        expect_absent=["FAIL"],
    ),
    # the same from Verilog: $info, $warning, $error and $fatal
    "severity_v": Case(
        [Core("sev", "sev_v")],
        "tb_sev.vhd",
        "tb_sev",
        "100ns",
        run_args=["--exit-severity=failure"],
        expect_failure=[
            "** Note: 1ns+1: :tb_sev:s:core:: sev note",
            "** Warning: 1ns+1: :tb_sev:s:core:: sev warning",
            "** Error: 1ns+1: :tb_sev:s:core:: sev error",
            "** Failure: 1ns+1: :tb_sev:s:core:: sev failure",
        ],
        expect_absent=["FAIL"],
    ),
    # NVC's --exit-severity governs them: the error ends the run, and the failure after it in the
    # same process is never reported
    "severity_exit": Case(
        [Core("sev", "sev")],
        "tb_sev.vhd",
        "tb_sev",
        "100ns",
        run_args=["--exit-severity=error"],
        expect_failure=[
            "** Note: 1ns+1: :tb_sev:s:core:: sev note",
            "** Warning: 1ns+1: :tb_sev:s:core:: sev warning",
            "** Error: 1ns+1: :tb_sev:s:core:: sev error",
        ],
        expect_absent=["sev failure", "FAIL"],
    ),
}


def run_case(case, nvc, build, bridge_env, designs, tmp_path, verilated=None):
    """Run one Case through the bridge: the two NVC passes, nvcx.tcl between
    them, and the run itself, checking what the Case says to check. designs
    and verilated give the cores' xsim and Verilator snapshots by design name
    (conftest.py); a Case needs only those of the backends it uses."""
    run = prepare_case(case, nvc, build, bridge_env, designs, tmp_path, verilated)
    r = run(case.elab_args)
    if case.check:
        case.check(tmp_path, r)
    return r


def prepare_case(case, nvc, build, bridge_env, designs, tmp_path, verilated=None):
    """Everything up to the run: the two NVC passes and nvcx.tcl between
    them. Returns run(elab_args), which elaborates the top with those
    arguments (its generics, say) and runs it, checking what the Case says
    to check, so that a design prepared once runs many times."""
    from conftest import DESIGNS, RTL
    import twin

    env = dict(bridge_env, NVCX_WORK=str(tmp_path / "work"))

    def shell(c):
        return twin.shell_name(c.name) if case.twin else c.name

    if env.get("XILINX_VIVADO"):
        env["PATH"] = f"{Path(env['XILINX_VIVADO']) / 'bin'}:{env['PATH']}"

    # the design's NVC-side script: what xsim or Verilator provides for each core
    calls = ["package require nvcx"]
    for c in case.cores:
        if c.backend == "verilator" and c.per_generics:
            _, source, _ = DESIGNS[c.design]
            calls.append(
                f"nvcx::write_shell -component {shell(c)} -output {tmp_path / shell(c)}_shell.vhd"
                f" -verilator {{{' '.join([str(RTL / source), *c.vargs])}}}"
            )
        elif c.library is None:
            snapshots = verilated if c.backend == "verilator" else designs
            calls.append(
                f"nvcx::write_shell -component {shell(c)} -output {tmp_path / shell(c)}_shell.vhd"
                f" -snapshot {snapshots(c.design)}"
            )
        else:
            lang, source, _ = DESIGNS[c.design]
            prj = tmp_path / f"{c.design}.prj"
            prj.write_text(
                f"{'vhdl2008' if lang == 'vhdl' else 'sv'} {c.library} {RTL / source}\n"
            )
            calls.append(
                f"nvcx::write_shell -component {shell(c)} -output {tmp_path / shell(c)}_shell.vhd"
                f" -library {c.library} -elab {{-prj {prj}}}"
            )
    script = tmp_path / "nvc.tcl"
    script.write_text("\n".join(calls) + "\n")
    env["TCLLIBPATH"] = str(build / "tcl")

    # the build tree keeps everything side by side (an install spreads it over lib/ and share/)
    core = str(build / "nvcx.vhd")
    plugin = str(build / "libnvcx.so")
    subprocess.run(
        [nvc, "--std=2008", f"--work=nvcx:{tmp_path / 'nvcx'}", "-a", core],
        check=True,
        cwd=tmp_path,
    )
    common = [
        nvc,
        "--std=2008",
        f"--map=nvcx:{tmp_path / 'nvcx'}",
        f"--work=work:{tmp_path / 'work_lib'}",
    ]
    # a twin's other half: the cores' RTL, simulated by NVC itself
    twins = []
    if case.twin:
        native = tmp_path / "native_lib"
        for c in case.cores:
            subprocess.run(
                [
                    nvc,
                    "--std=2008",
                    f"--work=native:{native}",
                    "-a",
                    str(RTL / DESIGNS[c.design][1]),
                ],
                check=True,
                cwd=tmp_path,
            )
            path = tmp_path / f"{c.name}_twin.vhd"
            twin.write(c.name, c.ports, path)
            twins.append(str(path))
        common.insert(2, f"--map=native:{native}")

    def nvc_run(*args, **kw):
        return subprocess.run(
            [*common, *args],
            cwd=tmp_path,
            env=env,
            capture_output=True,
            text=True,
            **kw,
        )

    # pass 1: the cores unbound
    r = nvc_run("-a", *twins, *(str(TB / f) for f in case.sources), str(TB / case.tb))
    assert r.returncode == 0, r.stdout + r.stderr
    r = nvc_run("-e", *case.elab_args, case.top)
    assert r.returncode == 0, r.stdout + r.stderr
    r = nvc_run("--do", case.top, str(script))
    assert r.returncode == 0, r.stdout + r.stderr
    for text in case.tcl_expect:
        assert text in r.stdout, (text, r.stdout + r.stderr)
    shells = sorted(tmp_path.glob("*_shell.vhd"))
    assert {s.name for s in shells} == {
        f"{shell(c)}_shell.vhd" for c in case.cores
    }, shells
    if case.before_run:
        case.before_run(tmp_path)
    # pass 2: the shells provide the entities
    r = nvc_run("-a", *map(str, shells))
    assert r.returncode == 0, r.stdout + r.stderr

    def run(elab_args):
        r = nvc_run("-e", *elab_args, case.top)
        assert r.returncode == 0, r.stdout + r.stderr
        r = nvc_run(
            "-r", "--load", plugin, f"--stop-time={case.stop}", *case.run_args, case.top
        )
        out = r.stdout + r.stderr
        if case.expect_failure:
            # NVC's own failure exit, not a crash on the way out
            assert r.returncode == 1 and "Caught signal" not in out, out
            pos = 0
            for text in case.expect_failure:
                pos = out.find(text, pos)
                assert pos >= 0, (text, out)
                pos += len(text)
            for text in case.expect_absent:
                assert text not in out, (text, out)
            for text in case.expect_once:
                assert out.count(text) == 1, (text, out)
            return r
        assert r.returncode == 0 and "PASS" in out, out
        r.rerun = lambda: run(elab_args)
        return r

    return run


def run_native(case, nvc, tmp_path):
    """The oracle: the same testbench with the cores simulated by NVC itself."""
    from conftest import DESIGNS, RTL

    sources = [str(RTL / DESIGNS[c.design][1]) for c in case.cores]
    sources += [str(TB / f) for f in case.sources] + [str(TB / case.tb)]
    common = [nvc, "--std=2008", f"--work=work:{tmp_path / 'work_lib'}"]
    r = subprocess.run(
        [
            *common,
            "-a",
            *sources,
            "-e",
            case.top,
            "-r",
            f"--stop-time={case.stop}",
            *case.run_args,
            case.top,
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    out = r.stdout + r.stderr
    assert r.returncode == 0 and "PASS" in out, out
    return r


@pytest.mark.parametrize("name", CASES)
def test_nvc_case(name, nvc, build, bridge_env, designs, tmp_path):
    run_case(CASES[name], nvc, build, bridge_env, designs, tmp_path)
