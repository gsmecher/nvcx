library ieee; use ieee.std_logic_1164.all;
-- One input through the three kinds of delay, of t_ps picoseconds:
-- transport, inertial, and inertial rejecting only pulses shorter than r_ps
-- (r_ps <= t_ps). Pulses narrower than the delay are where they differ.
entity pdelay is port (
  a    : in  std_logic;
  t_ps : in  natural;
  r_ps : in  natural;
  y_tr : out std_logic;
  y_in : out std_logic;
  y_rj : out std_logic);
end;
architecture rtl of pdelay is
begin
  y_tr <= transport a after t_ps * 1 ps;
  y_in <= a after t_ps * 1 ps;
  y_rj <= reject r_ps * 1 ps inertial a after t_ps * 1 ps;
end;
