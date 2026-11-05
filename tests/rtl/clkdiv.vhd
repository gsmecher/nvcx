library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
-- Clocks made inside the core: clk divided by two (a register, so its edges
-- come a delta after clk's) and clk gated by en (en latched while clk is low,
-- so glitch-free, its edges a delta after clk's). A register on each samples
-- d, and one on clk samples both of those, on edges they share, in the
-- deltas before they change. The divided clock leaves the core.
entity clkdiv is port (
  clk    : in  std_logic;
  en     : in  std_logic;
  d      : in  unsigned(7 downto 0);
  clk_d2 : out std_logic;
  q_d2   : out unsigned(7 downto 0);
  q_g    : out unsigned(7 downto 0);
  q_x    : out unsigned(7 downto 0));
end;
architecture rtl of clkdiv is
  signal div, en_l, gclk : std_logic := '0';
  signal r_d2, r_g, r_x : unsigned(7 downto 0) := (others => '0');
begin
  process(clk) begin
    if rising_edge(clk) then div <= not div; end if;
  end process;
  process(clk, en) begin
    if clk = '0' then en_l <= en; end if;
  end process;
  gclk <= clk and en_l;
  process(div) begin
    if rising_edge(div) then r_d2 <= d; end if;
  end process;
  process(gclk) begin
    if rising_edge(gclk) then r_g <= d; end if;
  end process;
  process(clk) begin
    if rising_edge(clk) then r_x <= r_d2 xor r_g; end if;
  end process;
  clk_d2 <= div;
  q_d2   <= r_d2;
  q_g    <= r_g;
  q_x    <= r_x;
end;
