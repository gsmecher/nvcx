"""Xilinx XPM macros, each simulated by xsim and by Verilator side by side, with their outputs
compared.

Every macro is treated alike. For a generic point, the macro is wrapped twice in a trivial Verilog
module with the generics fixed (xs_<point> for xsim, vl_<point> for Verilator), each wrapper
holding its own glbl so that the macro's glbl.GSR finds it. One NVC testbench drives both wrappers
with the same seeded random stimulus and compares every output once each time step has settled,
after the startup reset (glbl's GSR) and a warm-up. Any difference fails the test: the suite flags
differences and does not decide which simulator, or the bridge, is wrong. So does any error a core
reports (a configuration it refuses), or its $finish.

Verilator is 2-state, so a bit xsim leaves undefined (U, X, Z, W, -) matches any value Verilator
gives it: an undefined value allows any defined one. Every defined bit must be equal. The
testbench counts the checks that relied on this, per output, and reports the counts alongside the
differences (xpm_memory, for one, gives X on a read/write address collision). Two more cases count
as differences, since they compare little or nothing: an output xsim leaves undefined at more than
a small fraction of its checks (NVCX_XPM_UNDEFINED, default 0.1), and a run in which no output
changes after the warm-up.

The ports come from Verilator (its model's header), at each generic point, and the stimulus follows
their names:
- each clock gets its own period, 2 to 8 ns, chosen by the seed, so that independent clocks drift
  past one another;
- a reset, sleep or error injection is asserted for the first cycles, then now and then for 1 to
  32 cycles (active low if its name ends in n: aresetn, rst_n);
- a write enable vector (byte enables) is clear half the time, so that its port reads too;
- any other input takes a random value at each falling edge of its clock (the clock whose name
  shares its prefix, else the first).
A memory starts with random contents (MEMORY_INIT_FILE), unless it has ECC, which XPM_MEMORY does
not allow with initialization; and it is a few thousand words deep, so that two ports' random
addresses rarely collide (xsim stores X in a word both ports write at once). SIM_ASSERT_CHK stays
0: a model's $error on a protocol violation would end the run.

Quick tier (default): each macro's first generic point, one seed. Deep tier (-m xpm_deep): every
point, NVCX_XPM_SEEDS seeds each (default 4). NVCX_XPM_VARGS adds Verilator arguments to every
model. Snapshots are cached in NVCX_XPM_WORK if it is set,
so that repeated runs skip xelab and Verilator.
"""

import os
import random
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

CDC, MEMORY, FIFO = "xpm_cdc", "xpm_memory", "xpm_fifo"

# macro -> (the XPM libraries it needs, its generic points); a point gives only what differs
# from the macro's defaults
MACROS = {
    "xpm_cdc_single": (
        [CDC],
        [
            {},
            {"DEST_SYNC_FF": 2, "SRC_INPUT_REG": 0},
            {"DEST_SYNC_FF": 10, "INIT_SYNC_FF": 1},
        ],
    ),
    "xpm_cdc_array_single": (
        [CDC],
        [
            {"WIDTH": 8},
            {"WIDTH": 1, "DEST_SYNC_FF": 2, "SRC_INPUT_REG": 0},
            {"WIDTH": 70, "INIT_SYNC_FF": 1},
        ],
    ),
    "xpm_cdc_gray": (
        [CDC],
        [
            {"WIDTH": 4},
            {"WIDTH": 2, "DEST_SYNC_FF": 2, "REG_OUTPUT": 1},
            {"WIDTH": 31, "INIT_SYNC_FF": 1, "DEST_SYNC_FF": 6},
        ],
    ),
    "xpm_cdc_async_rst": (
        [CDC],
        [{}, {"RST_ACTIVE_HIGH": 1, "DEST_SYNC_FF": 2}, {"INIT_SYNC_FF": 1}],
    ),
    "xpm_cdc_sync_rst": (
        [CDC],
        [{}, {"INIT": 0, "DEST_SYNC_FF": 2}, {"INIT_SYNC_FF": 1, "DEST_SYNC_FF": 7}],
    ),
    "xpm_cdc_pulse": (
        [CDC],
        [{}, {"RST_USED": 0, "REG_OUTPUT": 1}, {"DEST_SYNC_FF": 2, "INIT_SYNC_FF": 1}],
    ),
    "xpm_cdc_handshake": (
        [CDC],
        [
            {"WIDTH": 8},
            {"WIDTH": 1, "DEST_EXT_HSK": 0, "SRC_SYNC_FF": 2, "DEST_SYNC_FF": 2},
            {"WIDTH": 40, "INIT_SYNC_FF": 1},
        ],
    ),
    "xpm_cdc_low_latency_handshake": (
        [CDC],
        [
            {"WIDTH": 8},
            {
                "WIDTH": 1,
                "DEST_EXT_HSK": 0,
                "SRC_SYNC_FF": 2,
                "DEST_SYNC_FF": 2,
                "INIT_SYNC_FF": 1,
            },
            {"WIDTH": 40, "INIT_SYNC_FF": 1},
        ],
    ),
    # MEMORY_SIZE is in bits: these are 256 to 4096 words, so that random addresses on two ports
    # rarely collide (xsim stores X in a word two ports write at once, as the hardware may)
    "xpm_memory_spram": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 1024 * 32, "WRITE_DATA_WIDTH_A": 32, "READ_LATENCY_A": 1},
            {
                "MEMORY_SIZE": 512 * 32,
                "WRITE_DATA_WIDTH_A": 32,
                "BYTE_WRITE_WIDTH_A": 8,
                "READ_LATENCY_A": 2,
                "WRITE_MODE_A": "write_first",
            },
            {
                "MEMORY_SIZE": 256 * 18,
                "WRITE_DATA_WIDTH_A": 18,
                "MEMORY_PRIMITIVE": "distributed",
                "READ_LATENCY_A": 0,
                "WRITE_MODE_A": "read_first",
            },
            {
                "MEMORY_SIZE": 4096 * 64,
                "WRITE_DATA_WIDTH_A": 64,
                "MEMORY_PRIMITIVE": "ultra",
                "READ_LATENCY_A": 3,
                "WRITE_MODE_A": "no_change",
                "ECC_MODE": "both_encode_and_decode",
            },
        ],
    ),
    "xpm_memory_sdpram": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 1024 * 16, "WRITE_DATA_WIDTH_A": 16, "READ_LATENCY_B": 1},
            {
                "MEMORY_SIZE": 2048 * 32,
                "WRITE_DATA_WIDTH_A": 32,
                "READ_DATA_WIDTH_B": 64,
                "CLOCKING_MODE": "independent_clock",
                "READ_LATENCY_B": 2,
                "WRITE_MODE_B": "read_first",
            },
            {
                "MEMORY_SIZE": 512 * 32,
                "WRITE_DATA_WIDTH_A": 32,
                "BYTE_WRITE_WIDTH_A": 8,
                "MEMORY_PRIMITIVE": "block",
                "READ_LATENCY_B": 3,
            },
        ],
    ),
    "xpm_memory_tdpram": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 1024 * 16, "WRITE_DATA_WIDTH_A": 16, "READ_LATENCY_A": 1},
            {
                "MEMORY_SIZE": 2048 * 32,
                "WRITE_DATA_WIDTH_A": 32,
                "WRITE_DATA_WIDTH_B": 16,
                "CLOCKING_MODE": "independent_clock",
                "READ_LATENCY_A": 2,
                "WRITE_MODE_A": "read_first",
                "WRITE_MODE_B": "write_first",
            },
            {
                "MEMORY_SIZE": 4096 * 32,
                "WRITE_DATA_WIDTH_A": 32,
                "BYTE_WRITE_WIDTH_A": 8,
                "BYTE_WRITE_WIDTH_B": 8,
                "READ_LATENCY_A": 3,
            },
        ],
    ),
    "xpm_memory_sprom": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 1024 * 16, "READ_DATA_WIDTH_A": 16, "READ_LATENCY_A": 1},
            {
                "MEMORY_SIZE": 256 * 8,
                "READ_DATA_WIDTH_A": 8,
                "MEMORY_PRIMITIVE": "distributed",
                "READ_LATENCY_A": 0,
            },
        ],
    ),
    "xpm_memory_dprom": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 1024 * 16, "READ_DATA_WIDTH_A": 16, "READ_LATENCY_A": 1},
            {
                "MEMORY_SIZE": 2048 * 32,
                "READ_DATA_WIDTH_A": 32,
                "CLOCKING_MODE": "independent_clock",
                "READ_LATENCY_A": 2,
            },
        ],
    ),
    "xpm_memory_dpdistram": (
        [MEMORY],
        [
            {"MEMORY_SIZE": 256 * 16, "WRITE_DATA_WIDTH_A": 16, "READ_LATENCY_A": 1},
            {
                "MEMORY_SIZE": 128 * 8,
                "WRITE_DATA_WIDTH_A": 8,
                "CLOCKING_MODE": "independent_clock",
                "READ_LATENCY_A": 0,
                "READ_LATENCY_B": 0,
            },
        ],
    ),
    "xpm_fifo_sync": (
        [FIFO, MEMORY, CDC],
        [
            {
                "FIFO_WRITE_DEPTH": 16,
                "WRITE_DATA_WIDTH": 8,
                "USE_ADV_FEATURES": "1F1F",
                "WR_DATA_COUNT_WIDTH": 5,
                "RD_DATA_COUNT_WIDTH": 5,
                "PROG_FULL_THRESH": 10,
                "PROG_EMPTY_THRESH": 5,
            },
            {
                "FIFO_WRITE_DEPTH": 512,
                "WRITE_DATA_WIDTH": 36,
                "READ_MODE": "fwft",
                "FIFO_READ_LATENCY": 0,
                "USE_ADV_FEATURES": "1F1F",
                "WR_DATA_COUNT_WIDTH": 10,
                "RD_DATA_COUNT_WIDTH": 10,
                "FIFO_MEMORY_TYPE": "block",
            },
            {
                "FIFO_WRITE_DEPTH": 32,
                "WRITE_DATA_WIDTH": 16,
                "READ_DATA_WIDTH": 32,
                "FIFO_READ_LATENCY": 2,
                "FIFO_MEMORY_TYPE": "block",
                "USE_ADV_FEATURES": "0707",
                "WR_DATA_COUNT_WIDTH": 6,
                "RD_DATA_COUNT_WIDTH": 5,
            },
        ],
    ),
    "xpm_fifo_async": (
        [FIFO, MEMORY, CDC],
        [
            {
                "FIFO_WRITE_DEPTH": 16,
                "WRITE_DATA_WIDTH": 8,
                "USE_ADV_FEATURES": "1F1F",
                "WR_DATA_COUNT_WIDTH": 5,
                "RD_DATA_COUNT_WIDTH": 5,
                "PROG_FULL_THRESH": 10,
                "PROG_EMPTY_THRESH": 5,
            },
            {
                "FIFO_WRITE_DEPTH": 512,
                "WRITE_DATA_WIDTH": 36,
                "READ_MODE": "fwft",
                "FIFO_READ_LATENCY": 0,
                "USE_ADV_FEATURES": "1F1F",
                "WR_DATA_COUNT_WIDTH": 10,
                "RD_DATA_COUNT_WIDTH": 10,
                "CDC_SYNC_STAGES": 4,
            },
            {
                "FIFO_WRITE_DEPTH": 64,
                "WRITE_DATA_WIDTH": 32,
                "READ_DATA_WIDTH": 16,
                "RELATED_CLOCKS": 1,
                "FIFO_MEMORY_TYPE": "block",
                "WR_DATA_COUNT_WIDTH": 7,
                "RD_DATA_COUNT_WIDTH": 8,
            },
        ],
    ),
    "xpm_fifo_axis": (
        [FIFO, MEMORY, CDC],
        [
            {
                "FIFO_DEPTH": 16,
                "TDATA_WIDTH": 8,
                "USE_ADV_FEATURES": "1F1F",
                "WR_DATA_COUNT_WIDTH": 5,
                "RD_DATA_COUNT_WIDTH": 5,
                "PROG_FULL_THRESH": 10,
                "PROG_EMPTY_THRESH": 5,
            },
            {
                "FIFO_DEPTH": 64,
                "TDATA_WIDTH": 32,
                "TUSER_WIDTH": 4,
                "TID_WIDTH": 2,
                "TDEST_WIDTH": 3,
                "CLOCKING_MODE": "independent_clock",
                "PACKET_FIFO": "true",
            },
        ],
    ),
}

# Nanoseconds before outputs are compared: glbl's GSR (100 ns), then a warm-up
ARM_NS = 400
# Cycles of the first clock a run lasts
CYCLES = 3000
# The fraction of an output's checks xsim may leave undefined (see above) before that is a
# difference itself
UNDEFINED = float(os.environ.get("NVCX_XPM_UNDEFINED", "0.1"))
# Extra Verilator arguments for every macro's model (e.g. --x-initial-edge)
VERILATOR_ARGS = os.environ.get("NVCX_XPM_VARGS", "").split()
# Reports of differences before the testbench only counts them
MAX_REPORTS = 20
# Name-based stimulus: ports asserted rarely, and clocks
RARE = re.compile(r"rst|reset|sleep|inject")
# a rare event's polarity: active low if named so (aresetn, rst_n)
ACTIVE_LOW = re.compile(r"(rst|reset)_?n$")
# a write enable vector (byte enables): all clear half the time, so that its port also reads
WRITE_ENABLE = re.compile(r"^we[a-z]?$|_we$")
CLOCK = re.compile(r"(^|_)clk|clk$|aclk$")


@dataclass
class Port:
    name: str
    dir: str  # "in", "out" or "inout"
    width: int


def point_id(macro, i):
    return f"{macro}_{i}"


def verilog_value(v):
    return f'"{v}"' if isinstance(v, str) else str(v)


def xpm_sources(vivado, libs):
    return [vivado / "data" / "ip" / "xpm" / lib / "hdl" / f"{lib}.sv" for lib in libs]


def init_file(macro, params, path):
    """For a memory, random initial contents (MEMORY_INIT_FILE), so that every read means
    something, ROMs' included: words of the narrowest data width, MEMORY_SIZE bits in all
    """
    widths = [v for k, v in params.items() if "DATA_WIDTH" in k] or [32]
    width = min(widths)
    rng = random.Random(f"{macro} {sorted(params.items())}")
    digits = (width + 3) // 4
    words = params.get("MEMORY_SIZE", 2048) // width
    path.write_text(
        "".join(f"{rng.getrandbits(width):0{digits}x}\n" for _ in range(words))
    )
    return dict(params, MEMORY_INIT_FILE=str(path))


def glbl(vivado):
    return vivado / "data" / "verilog" / "src" / "glbl.v"


def ports_of(macro, params, sources, work):
    """The macro's ports at this generic point, as Verilator's model declares them (ONESPIN
    leaves out the startup reset's glbl.GSR, which has no glbl here and no bearing on ports)
    """
    obj = work / "ports"
    gs = [f"-G{k}={verilog_value(v)}" for k, v in params.items()]
    r = subprocess.run(
        [
            "verilator",
            "--cc",
            "--timing",
            "-Wno-fatal",
            "-DONESPIN",
            "--top-module",
            macro,
            "--prefix",
            "Vports",
            "-Mdir",
            str(obj),
            *gs,
            *map(str, sources),
        ],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, r.stdout + r.stderr
    ports = []
    for line in (obj / "Vports.h").read_text().splitlines():
        m = re.match(r"\s*VL_(IN|OUT|INOUT)(\w*)\(&(\w+),\s*(-?\d+),\s*(-?\d+)", line)
        if m:
            d, _, name, msb, lsb = m.groups()
            ports.append(Port(name, d.lower(), abs(int(msb) - int(lsb)) + 1))
    assert ports, f"no ports for {macro}"
    return ports


def wrapper(name, macro, params, ports):
    decl = ",\n  ".join(
        f"{'input ' if p.dir == 'in' else 'output'} wire "
        f"{'' if p.width == 1 else f'[{p.width - 1}:0] '}{p.name}"
        for p in ports
    )
    gs = ", ".join(f".{k}({verilog_value(v)})" for k, v in params.items())
    conn = ", ".join(f".{p.name}({p.name})" for p in ports)
    return f"""// nvcx XPM differential test: {macro} {params}
`timescale 1ps / 1ps  // as the XPM sources and glbl.v; xelab rejects a mix (Vivado 2025.1)
module {name} (
  {decl}
);
  glbl glbl ();  // the macro's glbl.GSR resolves upwards to this instance
  {macro} #({gs}) u ({conn});
endmodule
"""


def vhdl_type(p):
    return "std_logic" if p.width == 1 else f"std_logic_vector({p.width - 1} downto 0)"


def clock_of(port, clocks):
    """The clock an input changes with: the one whose name shares its prefix, else the first"""
    best, best_len = clocks[0], 0
    for c in clocks:
        n = 0
        while n < min(len(c), len(port.name)) and c[n] == port.name[n]:
            n += 1
        # a clock named for a port's suffix: clka with addra, clkb with dinb
        if c.startswith("clk") and len(c) == 4 and port.name.endswith(c[3]):
            n = max(n, 2)
        if n > best_len:
            best, best_len = c, n
    return best


def tb_text(top, xs, vl, macro, pid, ports):
    """The testbench: both wrappers, their stimulus, and the comparison of their outputs"""
    ins = [p for p in ports if p.dir == "in"]
    outs = [p for p in ports if p.dir == "out"]
    assert not [p for p in ports if p.dir == "inout"], f"{macro}: inout ports"
    clocks = [p.name for p in ins if CLOCK.search(p.name)]
    assert clocks, f"{macro}: no clock"
    data = [p for p in ins if p.name not in clocks]

    def comp(name):
        decl = ";\n      ".join(f"{p.name} : {p.dir} {vhdl_type(p)}" for p in ports)
        return f"  component {name} is\n    port (\n      {decl});\n  end component;"

    def pmap(prefix):
        return ", ".join(
            f"{p.name} => {(prefix + p.name) if p.dir == 'out' else p.name}"
            for p in ports
        )

    signals = "\n".join(
        f"  signal {p.name} : {vhdl_type(p)} := {zero(p)};" for p in ins
    )
    signals += "\n" + "\n".join(
        f"  signal x_{p.name}, v_{p.name} : {vhdl_type(p)};" for p in outs
    )
    # each clock's half period, in ps: a generic, which the seed chooses (clock_halves)
    clockgen = "\n".join(
        f"""  {c}_gen : process
    constant half : time := HALF_{i} * 1 ps;
  begin
    while not done loop
      {c} <= '0'; wait for half;
      {c} <= '1'; wait for half;
    end loop;
    wait;
  end process;"""
        for i, c in enumerate(clocks)
    )
    stim = []
    for i, p in enumerate(data):
        clk = clock_of(p, clocks)
        rare = bool(RARE.search(p.name))
        idle, active = ("'1'", "'0'") if ACTIVE_LOW.search(p.name) else ("'0'", "'1'")
        if p.width == 1 and rare:
            # asserted for the first cycles; after, now and then, for 1 to 32 cycles
            first = f"{p.name} <= {active};"
            draw = "uniform(s1, s2, r);"
            assign = (
                f"if r < 0.02 then {p.name} <= {active}; uniform(s1, s2, r); "
                f"for c in 1 to integer(floor(r * 31.0)) loop wait until falling_edge({clk}); "
                f"end loop; end if; {p.name} <= {idle};"
            )
        elif p.width == 1:
            first = ""
            draw = "uniform(s1, s2, r);"
            assign = f"{p.name} <= '1' when r < 0.5 else '0';"
        else:
            # one random bit at a time: any width
            assign = (
                f"for b in 0 to {p.width - 1} loop uniform(s1, s2, r); "
                f"v({p.width - 1} - b) := '1' when r < 0.5 else '0'; end loop; {p.name} <= v;"
            )
            first = ""
            draw = "uniform(s1, s2, r);"
            if rare:
                assign = f"if r < 0.02 then {assign} else {p.name} <= (others => '0'); end if;"
            elif WRITE_ENABLE.search(p.name):
                assign = f"if r < 0.5 then {assign} else {p.name} <= (others => '0'); end if;"
        decl_v = (
            f"    variable v : std_logic_vector({p.width - 1} downto 0);\n"
            if p.width > 1
            else ""
        )
        stim.append(
            f"""  {p.name}_stim : process
    variable s1, s2 : positive;
    variable r : real;
{decl_v}  begin
    s1 := SEED; s2 := {1000 + i};
    {first}
    for c in 1 to 16 loop wait until falling_edge({clk}); end loop;
    while not done loop
      wait until falling_edge({clk});
      {draw}
      {assign}
    end loop;
    wait;
  end process;"""
        )
    checks = "\n".join(
        f"""      c_{p.name} := c_{p.name} + 1;
      if undefined(x_{p.name}) then u := u + 1; u_{p.name} := u_{p.name} + 1; end if;
      if not agree(x_{p.name}, v_{p.name}) then
        n := n + 1;
        if n <= {MAX_REPORTS} then
          report "{macro} {pid} seed " & integer'image(SEED) & ": {p.name}: xsim "
            & to_string(x_{p.name}) & ", verilator " & to_string(v_{p.name}) severity error;
        end if;
      end if;"""
        for p in outs
    )
    sens = ", ".join(f"x_{p.name}, v_{p.name}" for p in outs)
    total = " + ".join(f"c_{p.name}" for p in outs)
    uvars = "\n".join(
        f"    variable u_{p.name}, c_{p.name} : natural := 0;" for p in outs
    )
    # an output xsim leaves undefined too often is barely compared: that is a difference too
    never = "\n".join(
        f"""      if c_{p.name} > 0 and real(u_{p.name}) > UNDEF_MAX * real(c_{p.name}) then
        n := n + 1;
        report "{macro} {pid} seed " & integer'image(SEED) & ": {p.name}: xsim undefined at "
          & integer'image(u_{p.name}) & " of " & integer'image(c_{p.name}) & " checks"
          severity error;
      end if;"""
        for p in outs
    )
    ureport = "".join(
        f' & " {p.name} " & integer\'image(u_{p.name}) & "/" & integer\'image(c_{p.name})'
        for p in outs
    )
    halves = "".join(f"; HALF_{i} : positive := 5000" for i in range(len(clocks)))
    return f"""library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all; use ieee.math_real.all;
-- nvcx XPM differential test: {macro} {pid} (tests/test_xpm.py)
entity {top} is
  generic (SEED : positive := 1; UNDEF_MAX : real := 0.1{halves});
end;
architecture tb of {top} is
  -- xsim's value x against Verilator's v: every bit x defines, v must equal
  function agree(x, v : std_logic_vector) return boolean is
    alias xa : std_logic_vector(x'length - 1 downto 0) is x;
    alias va : std_logic_vector(v'length - 1 downto 0) is v;
  begin
    for i in xa'range loop
      if to_x01(xa(i)) /= 'X' and to_x01(xa(i)) /= to_x01(va(i)) then return false; end if;
    end loop;
    return true;
  end;
  function agree(x, v : std_logic) return boolean is
  begin
    return to_x01(x) = 'X' or to_x01(x) = to_x01(v);
  end;
  function undefined(x : std_logic_vector) return boolean is
  begin
    return is_x(x);
  end;
  function undefined(x : std_logic) return boolean is
  begin
    return is_x(x);
  end;
{comp(xs)}
{comp(vl)}
{signals}
  signal done, armed : boolean := false;
begin
  x : {xs} port map ({pmap("x_")});
  v : {vl} port map ({pmap("v_")});

{clockgen}

{chr(10).join(stim)}

  arm : process
  begin
    wait for {ARM_NS} ns;
    armed <= true;
    for c in 1 to {CYCLES} loop wait until rising_edge({clocks[0]}); end loop;
    done <= true;
    wait;
  end process;

  -- every output, once each time step has settled
  check : postponed process ({sens}, done)
    variable n : natural := 0;  -- differences
    variable u : natural := 0;  -- checks of an output xsim left (partly) undefined
{uvars}
  begin
    if done then
{never}
      if {total} = 0 then
        n := n + 1;
        report "{macro} {pid} seed " & integer'image(SEED)
          & ": no output changed after the warm-up: nothing was compared" severity error;
      end if;
      report "differences: " & integer'image(n) & "; checks with xsim undefined: "
        & integer'image(u){ureport};
      if n = 0 then report "PASS"; end if;
    elsif armed then
{checks}
    end if;
  end process;
end;
"""


def clock_halves(seed, n):
    """Each of n clocks' half period in ps, 2 to 8 ns, from the seed: independent clocks get
    unrelated periods, so their edges drift past one another"""
    rng = random.Random(f"clocks {seed}")
    return [rng.randint(2000, 8000) for _ in range(n)]


def zero(p):
    return "'0'" if p.width == 1 else "(others => '0')"


def points(deep):
    out = []
    for macro, (_, pts) in MACROS.items():
        for i, _ in enumerate(pts if deep else pts[:1]):
            out.append((macro, i))
    return out


def seeds(deep):
    n = int(os.environ.get("NVCX_XPM_SEEDS", "4")) if deep else 1
    return list(range(1, n + 1))


@pytest.fixture(scope="session")
def xpm_work(tmp_path_factory):
    cache = os.environ.get("NVCX_XPM_WORK")
    return Path(cache) if cache else tmp_path_factory.mktemp("xpm_work")


@pytest.fixture(scope="session")
def xpm_prepared(nvc, build, vivado, verilator, xpm_work, tmp_path_factory):
    """prepared(macro, i) -> run(seed), for the testbench of that generic point, built once"""
    done = {}

    def prepare(macro, i):
        if (macro, i) in done:
            return done[(macro, i)]
        libs, pts = MACROS[macro]
        params = pts[i]
        pid = point_id(macro, i)
        d = tmp_path_factory.mktemp(pid)
        # (XPM_MEMORY does not support initialization with ECC)
        if (
            macro.startswith("xpm_memory")
            and params.get("ECC_MODE", "no_ecc") == "no_ecc"
        ):
            params = init_file(macro, params, d / f"{pid}.mem")
        sources = xpm_sources(vivado, libs)
        ports = ports_of(macro, params, [*sources, glbl(vivado)], d)
        xs, vl, top = f"xs_{pid}", f"vl_{pid}", f"tb_{pid}"
        (d / f"{xs}.sv").write_text(wrapper(xs, macro, params, ports))
        (d / f"{vl}.sv").write_text(wrapper(vl, macro, params, ports))
        (d / f"{top}.vhd").write_text(tb_text(top, xs, vl, macro, pid, ports))
        prj = d / "xs.prj"
        prj.write_text(
            "".join(f"sv xpm {s}\n" for s in sources)
            + f"verilog xpm {glbl(vivado)}\nsv xpm {d / f'{xs}.sv'}\n"
        )
        vargs = " ".join(
            [*map(str, [d / f"{vl}.sv", *sources, glbl(vivado)]), *VERILATOR_ARGS]
        )
        (d / "nvc.tcl").write_text(
            "package require nvcx\n"
            f"nvcx::write_shell -component {xs} -output {d / xs}_shell.vhd"
            f" -library xpm -elab {{-prj {prj}}}\n"
            f"nvcx::write_shell -component {vl} -output {d / vl}_shell.vhd -verilator {{{vargs}}}\n"
        )
        env = dict(
            os.environ,
            TCLLIBPATH=str(build / "tcl"),
            NVCX_WORK=str(xpm_work),
            PATH=f"{vivado / 'bin'}:{os.environ['PATH']}",
        )
        common = [
            nvc,
            "--std=2008",
            f"--map=nvcx:{d / 'nvcx'}",
            f"--work=work:{d / 'work'}",
        ]

        def sh(*args):
            r = subprocess.run(args, cwd=d, env=env, capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            return r

        sh(
            nvc,
            "--std=2008",
            f"--work=nvcx:{d / 'nvcx'}",
            "-a",
            str(build / "nvcx.vhd"),
        )
        sh(*common, "-a", f"{top}.vhd")
        sh(*common, "-e", top)
        sh(*common, "--do", top, "nvc.tcl")
        sh(*common, "-a", f"{xs}_shell.vhd", f"{vl}_shell.vhd")

        nclocks = len([p for p in ports if p.dir == "in" and CLOCK.search(p.name)])

        def run(seed):
            halves = [
                f"-gHALF_{i}={h}" for i, h in enumerate(clock_halves(seed, nclocks))
            ]
            sh(
                *common,
                "-e",
                f"-gSEED={seed}",
                f"-gUNDEF_MAX={UNDEFINED}",
                *halves,
                top,
            )
            r = subprocess.run(
                [*common, "-r", "--load", str(build / "libnvcx.so"), top],
                cwd=d,
                env=env,
                capture_output=True,
                text=True,
            )
            return r

        done[(macro, i)] = run
        return run

    return prepare


def check(run, seed):
    r = run(seed)
    out = r.stdout + r.stderr
    # the testbench's summary, for -rP or -s
    print("\n".join(line for line in out.splitlines() if "differences:" in line))
    # a core's own error (a configuration it refuses, say) or its $finish fails the run too
    assert r.returncode == 0 and "PASS" in out, out
    assert "** Error" not in out and "** Failure" not in out, out
    assert "finished its simulation" not in out, out


@pytest.mark.parametrize("macro,i", points(False), ids=lambda x: str(x))
def test_xpm(macro, i, xpm_prepared):
    check(xpm_prepared(macro, i), 1)


@pytest.mark.xpm_deep
@pytest.mark.parametrize(
    "macro,i,seed",
    [(m, i, s) for m, i in points(True) for s in seeds(True)],
)
def test_xpm_deep(macro, i, seed, xpm_prepared):
    # deselected unless -m names xpm_deep (conftest.py)
    check(xpm_prepared(macro, i), seed)
