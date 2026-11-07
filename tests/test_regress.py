"""Regressions: one case per fixed bug, each the smallest design that showed it. A case's comment
says what went wrong, so that a failure here names the bug that came back."""

import pytest

from test_nvc import Case, Core, run_case

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
}


@pytest.mark.parametrize("name", REGRESSIONS)
def test_regression(name, nvc, build, bridge_env, designs, tmp_path):
    run_case(REGRESSIONS[name], nvc, build, bridge_env, designs, tmp_path)
