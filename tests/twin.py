"""Twins: a core simulated twice under one NVC design, by xsim through the
bridge and by NVC itself, with assertions that they agree.

A twin is an entity with the core's own name and ports, so it replaces the
core in any testbench unchanged. It holds two instances: the core's shell
(component NAME_x, bound by nvcx to the xsim snapshot) and the core's RTL,
compiled by NVC into library `native`. Both see the same inputs; the
shell's outputs are the twin's, so the design runs on the bridged core and
the native one shadows it. NVC's own simulation of the RTL is the oracle,
so the twin assumes nothing about how many deltas anything should take:

- strictly, outputs must agree in every delta: a process sensitive to both
  instances' outputs asserts equality whenever either changes, so an output
  that changes a delta early or late, or a glitch in one only, fails;
- loosely, a postponed process asserts that they agree once each time
  step has settled.

The time-0 initialisation is left out: nvcx runs a core's initialisation
on until an output changes, before NVC can see it, by design.
"""

from dataclasses import dataclass


@dataclass
class Port:
    name: str
    mode: str  # "in" or "out"
    type: str  # a VHDL subtype indication


def shell_name(core: str) -> str:
    """The component the twin's xsim instance is declared as"""
    return f"{core}_x"


def write(core: str, ports: list[Port], path) -> None:
    """Write the twin of *core* to *path*"""
    outs = [p for p in ports if p.mode == "out"]
    decl = ";\n        ".join(f"{p.name} : {p.mode} {p.type}" for p in ports)
    # the shell drives the twin's own outputs directly (a signal assignment
    # between them would be a delta of the twin's own); the native core's go
    # to signals of their own
    signals = "\n".join(f"    signal n_{p.name} : {p.type};" for p in outs)

    def assoc(prefix):
        return ", ".join(
            f"{p.name} => {prefix + p.name if p.mode == 'out' else p.name}"
            for p in ports
        )

    sens = ", ".join(f"{p.name}, n_{p.name}" for p in outs)
    checks = "\n".join(
        f"""        assert {p.name} = n_{p.name}
            report "{core}.{p.name}: xsim " & to_string({p.name}) & ", NVC " & to_string(n_{p.name})
                 & context_msg & " (" & to_string(now) & ")"
            severity failure;"""
        for p in outs
    )
    path.write_text(
        f"""library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
library native;
-- The twin of {core} (see tests/twin.py)
entity {core} is
    port (
        {decl});
end;
architecture twin of {core} is
    component {shell_name(core)} is
        port (
        {decl});
    end component;
{signals}
begin
    x : {shell_name(core)} port map ({assoc("")});
    n : entity native.{core} port map ({assoc("n_")});

    -- in every delta, after the initialisation
    process ({sens})
        constant context_msg : string := " in the same delta";
    begin
        if now > 0 fs then
{checks}
        end if;
    end process;

    -- once each time step has settled
    postponed process ({sens})
        constant context_msg : string := " at the end of the time step";
    begin
        if now > 0 fs then
{checks}
        end if;
    end process;
end;
"""
    )
