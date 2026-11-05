library ieee; use ieee.std_logic_1164.all;
-- A delayed copy of the input, re-driven combinationally in the delta after it
-- matures, and delayed again: outputs that change in the first delta of a
-- time step (y, w), the second (z), and both when a edges as y matures.
entity chain is port (
  a    : in  std_logic;
  t_ps : in  natural;
  y    : out std_logic;
  z    : out std_logic;
  w    : out std_logic);
end;
architecture rtl of chain is
  signal yi, zi : std_logic := '0';
begin
  yi <= a after t_ps * 1 ps;
  zi <= yi xor a;
  y <= yi;
  z <= zi;
  w <= transport zi after t_ps * 1 ps;
end;
