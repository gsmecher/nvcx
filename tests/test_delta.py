"""Delta-cycle ordering across the boundary, xsim to NVC.

Each testbench passes in a unified simulator (NVC simulating the core itself
is the oracle) and depends on the order in which the core's outputs change
within one time step. The bridge must reproduce that order.
"""

import dataclasses

import pytest

from test_nvc import Case, Core, run_case, run_native
from twin import Port

# the cores' ports, for their twins (tests/twin.py)
PORTS = {
    "clkgen": [
        Port("clk_out", "out", "std_logic"),
        Port("cnt_out", "out", "unsigned(7 downto 0)"),
    ],
    "regcomb": [
        Port("clk", "in", "std_logic"),
        Port("d", "in", "std_logic_vector(7 downto 0)"),
        Port("q", "out", "std_logic_vector(7 downto 0)"),
        Port("nq", "out", "std_logic_vector(7 downto 0)"),
    ],
    "glitch": [Port("a", "in", "std_logic"), Port("pulse", "out", "std_logic")],
}

CASES = {
    # the core's own clock and a counter registered on it: a register outside
    # the core, clocked by that clock, must sample the counter before its update
    "clock_then_data": Case(
        [Core("clkgen", "clkgen")], "tb_clkgen.vhd", "tb_clkgen", "120ns"
    ),
    # a combinational output derived from a registered one changes one delta
    # after it, not with it
    "comb_after_reg": Case(
        [Core("regcomb", "regcomb")], "tb_regcomb.vhd", "tb_regcomb", "120ns"
    ),
    # a zero-time pulse on an output is an event outside the core too
    "glitch": Case([Core("glitch", "glitch")], "tb_glitch.vhd", "tb_glitch", "100ns"),
    # the number of deltas from a clock edge outside the core to a registered
    # output's change is the core's own: two for regcomb
    "latency": Case(
        [Core("regcomb", "regcomb")], "tb_latency.vhd", "tb_latency", "100ns"
    ),
    # the core's inputs change while it is still working through the deltas
    # of an earlier change (a second edge, with new data, two deltas after
    # the first): it sees each in the delta after NVC makes it
    "midchain": Case(
        [Core("regcomb", "regcomb")], "tb_midchain.vhd", "tb_midchain", "100ns"
    ),
}


@pytest.mark.parametrize("name", CASES)
def test_native(name, nvc, tmp_path):
    run_native(CASES[name], nvc, tmp_path)


@pytest.mark.parametrize("name", CASES)
def test_bridge(name, nvc, build, bridge_env, designs, tmp_path):
    run_case(CASES[name], nvc, build, bridge_env, designs, tmp_path)


@pytest.mark.parametrize("name", CASES)
def test_twin(name, nvc, build, bridge_env, designs, tmp_path):
    """The same testbenches with each core replaced by its twin: the bridged
    core and NVC's own simulation of its RTL must agree in every delta."""
    case = CASES[name]
    cores = [dataclasses.replace(c, ports=PORTS[c.design]) for c in case.cores]
    run_case(
        dataclasses.replace(case, cores=cores, twin=True),
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path,
    )
