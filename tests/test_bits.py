"""bit and bit_vector shell ports, on each kind of core.

NVC gives a bit port the same VHPI format as a std_ulogic one, with the type's own enumeration
positions as values: '0' is 0 for bit and 2 for std_ulogic. The bridge took a bit port's values
for std_ulogic's, so a Verilog core (either backend) read every bit input as X, and its outputs
were deposited as positions 2 and 3, which bit does not have. A VHDL core on xsim takes positions
as they are, and always worked.
"""

import pytest

from conftest import DESIGNS
from test_nvc import Case, Core, run_case

# (here rather than in conftest.py: these designs are this file's alone)
DESIGNS.update(bits=("vhdl", "bits.vhd", "bits"), bits_v=("sv", "bits.v", "bits"))


def display_whole(tmp_path, r):
    """xsim's $display text arrives as a note. The bridge cut a report at its first colon, taking
    whatever came before for a severity prefix: "bits: up" arrived as "up"."""
    out = r.stdout + r.stderr
    assert ":tb_bits:b:core:: bits: up" in out, out


def display_printed(tmp_path, r):
    assert "bits: up" in r.stdout + r.stderr, r.stdout + r.stderr


XSIM = {
    "vhdl": Case([Core("bits", "bits")], "tb_bits.vhd", "tb_bits", "100ns"),
    "verilog": Case(
        [Core("bits", "bits_v")], "tb_bits.vhd", "tb_bits", "100ns", check=display_whole
    ),
}


@pytest.mark.parametrize("name", XSIM)
def test_bits_xsim(name, nvc, build, bridge_env, designs, tmp_path):
    run_case(XSIM[name], nvc, build, bridge_env, designs, tmp_path)


def test_bits_verilator(nvc, build, verilator_env, verilated, tmp_path):
    case = Case(
        [Core("bits", "bits_v", backend="verilator")],
        "tb_bits.vhd",
        "tb_bits",
        "100ns",
        check=display_printed,
    )
    run_case(case, nvc, build, verilator_env, None, tmp_path, verilated)
