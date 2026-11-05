library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
-- A zero-delay ring: y is x + 1 while x < n (n otherwise, so that whatever
-- is in flight in a ring of them settles at n), three deltas after x, and
-- odd its low bit, a delta later still. In a ring, through NVC, it counts up
-- to n within one time step.
entity ring is port (
  x   : in  unsigned(7 downto 0);
  n   : in  unsigned(7 downto 0);
  y   : out unsigned(7 downto 0);
  odd : out std_logic);
end;
architecture rtl of ring is
  signal s1, s2, y_i : unsigned(7 downto 0) := (others => '0');
begin
  s1  <= x + 1 when x < n else n;
  s2  <= s1;
  y_i <= s2;
  y   <= y_i;
  odd <= y_i(0);
end;
