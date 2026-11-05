"""Property-based tests: hand-written cores, their timing chosen by
Hypothesis, each run as its twin (tests/twin.py), so that the bridged core
and NVC's own simulation of its RTL must agree in every delta. No result is
written down anywhere: NVC is the oracle.

The timing goes in through the testbenches' generics and the cores' input
ports, not the cores' generics, so that every example shares one snapshot,
and one design, prepared once per test: an example only elaborates the
testbench with its generics and runs it (about a second). So every run
tries a good number of examples; HYPOTHESIS_PROFILE=quick tries a few, for
a quick look. A failure Hypothesis finds belongs in an @example, for good.
"""

import os

import pytest

hypothesis = pytest.importorskip("hypothesis")
from hypothesis import HealthCheck, example, given, settings  # noqa: E402
from hypothesis import strategies as st  # noqa: E402

from test_nvc import Case, Core, prepare_case  # noqa: E402
from twin import Port  # noqa: E402

for name, examples in (("default", 100), ("quick", 10)):
    settings.register_profile(
        name,
        max_examples=examples,
        deadline=None,
        suppress_health_check=[
            HealthCheck.function_scoped_fixture,
            HealthCheck.too_slow,
        ],
    )
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "default"))

# a testbench's design, prepared once per test process: run(elab_args)
_prepared = {}


def run_twin(
    core, ports, tb, generics, stop, nvc, build, bridge_env, designs, tmp_path_factory
):
    if tb not in _prepared:
        case = Case(
            [Core(core, core, ports=ports)],
            f"{tb}.vhd",
            tb,
            stop,
            sources=["fuzz_pkg.vhd"],
            twin=True,
        )
        _prepared[tb] = prepare_case(
            case, nvc, build, bridge_env, designs, tmp_path_factory.mktemp(tb)
        )
    _prepared[tb]([f"-g{k}={v}" for k, v in generics.items()])


def times(edges, top):
    """Picoseconds, with the edge cases (0: the next delta; and *edges*)
    more likely than chance would make them"""
    return st.one_of(st.sampled_from([0, *edges]), st.integers(0, top))


PDELAY = [
    Port("a", "in", "std_logic"),
    Port("t_ps", "in", "natural"),
    Port("r_ps", "in", "natural"),
    Port("y_tr", "out", "std_logic"),
    Port("y_in", "out", "std_logic"),
    Port("y_rj", "out", "std_logic"),
]


@st.composite
def pdelay_timing(draw):
    t = draw(times([1, 1000], 3000))
    r = draw(st.integers(0, t))
    # pulses as wide as the delay or the rejection limit, and either side
    edges = sorted({t, r, max(t - 1, 0), t + 1, max(r - 1, 0), r + 1})
    pulses = draw(st.lists(times(edges, 4000), min_size=1, max_size=8))
    return t, r, pulses


@given(timing=pdelay_timing())
@example(timing=(1000, 0, [500, 500]))
@example(timing=(1000, 1000, [0, 0, 1000, 999]))
def test_pdelay(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """Pulses through transport, inertial and reject-inertial delays"""
    t, r, pulses = timing
    run_twin(
        "pdelay",
        PDELAY,
        "tb_pdelay",
        {"T_PS": t, "R_PS": r, "PULSES": ",".join(map(str, pulses))},
        "100ns",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


RSTSYNC = [
    Port("clk", "in", "std_logic"),
    Port("arst_n", "in", "std_logic"),
    Port("rst_n", "out", "std_logic"),
]


@st.composite
def rstsync_timing(draw):
    period = draw(st.one_of(st.sampled_from([2, 3, 1000]), st.integers(2, 3000)))
    phase = draw(times([period // 2, period], 2 * period))
    # reset edges on clock edges (the gaps that put them there) and between
    edges = [period // 2, period - period // 2, period]
    rst = draw(st.lists(times(edges, 5 * period), min_size=1, max_size=8))
    return period, phase, rst


@given(timing=rstsync_timing())
@example(timing=(1000, 0, [300, 2000]))
@example(timing=(1000, 0, [1000, 0, 500]))
def test_rstsync(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """A reset synchroniser: asynchronous assertion, synchronous release,
    with reset edges at, between and right on top of clock edges"""
    period, phase, rst = timing
    run_twin(
        "rstsync",
        RSTSYNC,
        "tb_rstsync",
        {"CLK_PS": period, "PHASE_PS": phase, "RST": ",".join(map(str, rst))},
        "1ms",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


@st.composite
def rstsync_multi_timing(draw):
    k = draw(st.integers(1, 4))
    chain = draw(st.booleans())
    instances = [draw(rstsync_timing()) for _ in range(k)]
    return k, chain, instances


@given(timing=rstsync_multi_timing())
@example(timing=(2, False, [(1000, 0, [300, 2000]), (1300, 0, [700, 1500])]))
@example(
    timing=(
        3,
        True,
        [(1000, 0, [300, 2000]), (1000, 0, [300, 2000]), (1000, 500, [0, 0])],
    )
)
def test_rstsync_multi(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """Several reset synchronisers at once, each with its own clock and reset
    stimulus, and, chained, each one's reset held by the previous one's
    output: cores stepped in the same passes, depending on one another
    through NVC within a time step"""
    k, chain, instances = timing

    def fields(i):
        return ";".join(
            ",".join(map(str, inst[i])) if isinstance(inst[i], list) else str(inst[i])
            for inst in instances
        )

    run_twin(
        "rstsync",
        RSTSYNC,
        "tb_rstsync_multi",
        {
            "K": k,
            "CHAIN": str(chain).lower(),
            "PERIODS": fields(0),
            "PHASES": fields(1),
            "RESETS": fields(2),
        },
        "1ms",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


CHAIN = [
    Port("a", "in", "std_logic"),
    Port("t_ps", "in", "natural"),
    Port("y", "out", "std_logic"),
    Port("z", "out", "std_logic"),
    Port("w", "out", "std_logic"),
]


@st.composite
def chain_timing(draw):
    t = draw(times([1, 1000], 3000))
    t2 = draw(st.one_of(st.just(t), times([1, t, 2 * t], 3000)))
    edges = sorted({t, t2, max(t - 1, 0), t + 1, abs(t - t2)})
    pulses = draw(st.lists(times(edges, 4000), min_size=1, max_size=8))
    return t, t2, pulses


@given(timing=chain_timing())
@example(timing=(1000, 700, [500, 500]))
@example(timing=(1000, 1000, [1000, 1000, 0, 1000]))
@example(timing=(1000, 500, [500, 500, 500]))
def test_chain(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """Two bridged cores whose matured transactions re-drive outputs
    combinationally in the same time step, with their events on the same
    and on different time steps"""
    t, t2, pulses = timing
    run_twin(
        "chain",
        CHAIN,
        "tb_chain",
        {"T_PS": t, "T2_PS": t2, "PULSES": ",".join(map(str, pulses))},
        "100ns",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )
