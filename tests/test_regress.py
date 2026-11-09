"""Regressions: one case per fixed bug, each the smallest design that showed it. A case's comment
says what went wrong, so that a failure here names the bug that came back."""

import pytest

from test_nvc import Case, Core, run_case


def core_finished(r, core):
    out = r.stdout + r.stderr
    assert f"nvcx: {core}: the core finished its simulation" in out, out


REGRESSIONS = {
    # nvcx.tcl passed string generics to xelab as -generic_top NAME=VALUE, unquoted, so xelab took
    # a numeric-looking one ("0") as a number. XPM's READ_RESET_VALUE_A => "0" arrived as the
    # integer 0, and its DRC called that non-zero.
    "string_generic": Case(
        [Core("strgen", "strgen", library="work")],
        "tb_strgen.vhd",
        "tb_strgen",
        "10ns",
        sources=["strgen_comp.vhd"],
    ),
    "string_generic_v": Case(
        [Core("strgen", "strgen_v", library="work")],
        "tb_strgen.vhd",
        "tb_strgen",
        "10ns",
        sources=["strgen_comp.vhd"],
    ),
    # A Verilog port whose name is a VHDL reserved word (the SGen FFT cores' "next") or otherwise not
    # a basic identifier is an extended identifier in the component, \next\. The bridge looked the
    # shell's port up by the bare Verilog name and found nothing, and nvcx.tcl lower-cased extended
    # identifiers too, making \In\ a different port, \in\.
    "extended_identifier_ports": Case(
        [Core("kwports", "kwports_v", library="work")],
        "tb_kwports.vhd",
        "tb_kwports",
        "10ns",
        sources=["kwports_comp.vhd"],
    ),
    # A core's $stop went unreported, and the run passed. The kernel notes it in its status, but
    # then resumes the run, which overwrites the status at its end; the core silently stopped
    # running. It is an error now, as Verilator reports it, and the core finishes.
    "stop": Case(
        [Core("stop", "stop_v")],
        "tb_stop.vhd",
        "tb_stop",
        "100ns",
        expect_failure=[
            "** Error: 25ns+1: :tb_stop:s:core:: $stop called",
            "the core finished its simulation",
            "PASS",
        ],
    ),
    # Likewise std.env.stop in a VHDL core: its status is not the kernel's to see, so it ends the
    # core's simulation as std.env.finish and $finish do, with a note, not silently.
    "stop_vhdl": Case(
        [Core("stop", "stop")],
        "tb_stop.vhd",
        "tb_stop",
        "100ns",
        check=lambda tmp_path, r: core_finished(r, ":tb_stop:s:core:"),
    ),
    # ...and std.env.finish, whose status the kernel overwrites in the same way
    "finish_vhdl": Case(
        [Core("stop", "finish")],
        "tb_stop.vhd",
        "tb_stop",
        "100ns",
        check=lambda tmp_path, r: core_finished(r, ":tb_stop:s:core:"),
    ),
}


@pytest.mark.parametrize("name", REGRESSIONS)
def test_regression(name, nvc, build, bridge_env, designs, tmp_path):
    run_case(REGRESSIONS[name], nvc, build, bridge_env, designs, tmp_path)
