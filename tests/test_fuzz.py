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


# A known xsim quirk, not the bridge's: when a port feeding an `after` delay
# (yi <= a after t_ps * 1 ps) is written through XSI during the kernel's
# initialisation, yi's zero-delay dependents (y <= yi, and on) never take
# their initial values, staying 'U' until the transaction matures. Plain XSI
# does it, without NVC. Hence tb_chain's SET_PS, by default after the cores
# have initialised.
@pytest.mark.xfail(
    strict=True, reason="xsim: a delay written during init leaves dependents 'U'"
)
def test_chain_init(nvc, build, bridge_env, designs, tmp_path_factory):
    run_twin(
        "chain",
        CHAIN,
        "tb_chain",
        {"T_PS": 1001, "T2_PS": 1001, "PULSES": "0", "SET_PS": 0},
        "100ns",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


@st.composite
def clocks(draw, top=3000):
    """A clock's period and the time of its first rising edge"""
    period = draw(st.one_of(st.sampled_from([2, 3, 1000]), st.integers(2, top)))
    phase = draw(times([period // 2, period], 2 * period))
    return period, phase


def csv(values):
    return ",".join(map(str, values))


CLKDIV = [
    Port("clk", "in", "std_logic"),
    Port("en", "in", "std_logic"),
    Port("d", "in", "unsigned(7 downto 0)"),
    Port("clk_d2", "out", "std_logic"),
    Port("q_d2", "out", "unsigned(7 downto 0)"),
    Port("q_g", "out", "unsigned(7 downto 0)"),
    Port("q_x", "out", "unsigned(7 downto 0)"),
]


@st.composite
def clkdiv_timing(draw):
    period, phase = draw(clocks())
    edges = [period // 2, period - period // 2, period]
    en = draw(st.lists(times(edges, 6 * period), min_size=1, max_size=6))
    d_t = draw(st.lists(times(edges, 3 * period), min_size=1, max_size=12))
    d_v = draw(st.lists(st.integers(0, 255), min_size=len(d_t), max_size=len(d_t)))
    return period, phase, en, d_t, d_v, draw(st.booleans())


@given(timing=clkdiv_timing())
@example(timing=(1000, 0, [1500, 3000], [0, 500, 1000], [1, 2, 3], False))
@example(timing=(1000, 0, [0, 1000], [1000, 0, 1000, 500], [1, 2, 3, 4], True))
def test_clkdiv(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """Derived and gated clocks inside the core, data sampled on them and on
    edges shared with the clock they come from, and the derived clock out
    through NVC and data back in"""
    period, phase, en, d_t, d_v, feedback = timing
    run_twin(
        "clkdiv",
        CLKDIV,
        "tb_clkdiv",
        {
            "CLK_PS": period,
            "PHASE_PS": phase,
            "EN_T": csv(en),
            "D_T": csv(d_t),
            "D_V": csv(d_v),
            "FEEDBACK": str(feedback).lower(),
        },
        "1ms",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


PULSESYNC = [
    Port("clk_a", "in", "std_logic"),
    Port("clk_b", "in", "std_logic"),
    Port("p_a", "in", "std_logic"),
    Port("busy_a", "out", "std_logic"),
    Port("p_b", "out", "std_logic"),
]


@st.composite
def pulsesync_timing(draw):
    a = draw(clocks())
    # the same clock as often as not: edges of both in the same delta
    b = draw(st.one_of(st.just(a), clocks()))
    edges = sorted({a[0], b[0], a[0] // 2, b[0] // 2})
    p = draw(st.lists(times(edges, 4 * max(a[0], b[0])), min_size=1, max_size=8))
    return a, b, p


@given(timing=pulsesync_timing())
@example(timing=((1000, 0), (1300, 0), [500, 1000]))
@example(timing=((1000, 0), (1000, 0), [1000, 0, 3000]))
def test_pulsesync(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """A pulse synchroniser between two clock domains, their edges apart or
    together"""
    (a, a_ph), (b, b_ph), p = timing
    run_twin(
        "pulsesync",
        PULSESYNC,
        "tb_pulsesync",
        {
            "A_PS": a,
            "A_PHASE_PS": a_ph,
            "B_PS": b,
            "B_PHASE_PS": b_ph,
            "P_T": csv(p),
        },
        "1ms",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )


RING = [
    Port("x", "in", "unsigned(7 downto 0)"),
    Port("n", "in", "unsigned(7 downto 0)"),
    Port("y", "out", "unsigned(7 downto 0)"),
    Port("odd", "out", "std_logic"),
]


@st.composite
def ring_timing(draw):
    k = draw(st.integers(1, 4))
    limits = draw(st.lists(st.integers(0, 255), min_size=k, max_size=k))
    # 0: the next delta, in the middle of a chain or loop
    inj = draw(st.lists(times([1, 2, 3, 4], 2000), min_size=1, max_size=8))
    seeds = draw(st.lists(st.integers(0, 255), min_size=len(inj), max_size=len(inj)))
    return k, limits, inj, seeds


@given(timing=ring_timing())
@example(timing=(2, [50, 60], [1000, 0, 1000], [3, 4, 5]))
@example(timing=(1, [255], [1000, 0, 0, 0], [0, 0, 7, 0]))
@example(timing=(4, [255, 255, 255, 255], [1000, 1000], [0, 0]))
@example(timing=(1, [0], [0], [0]))
@example(timing=(1, [0], [1, 0, 0], [0, 1, 0]))
def test_ring(timing, nvc, build, bridge_env, designs, tmp_path_factory):
    """Zero-delay chains through several cores, and loops through them and
    NVC that run for hundreds of deltas within one time step, broken and
    restarted in the middle"""
    k, limits, inj, seeds = timing
    run_twin(
        "ring",
        RING,
        "tb_ring",
        {"K": k, "LIMITS": csv(limits), "INJ_T": csv(inj), "SEEDS": csv(seeds)},
        "1ms",
        nvc,
        build,
        bridge_env,
        designs,
        tmp_path_factory,
    )
