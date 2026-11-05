library ieee; use ieee.std_logic_1164.all;
-- A pulse synchroniser between two clock domains: while p_a is high and the
-- last pulse is not still crossing (busy_a), each rising edge of clk_a
-- toggles a flag; two registers on clk_b take it into b, where each change
-- is a one-cycle pulse on p_b; two more on clk_a bring it back, to end
-- busy_a.
entity pulsesync is port (
  clk_a  : in  std_logic;
  clk_b  : in  std_logic;
  p_a    : in  std_logic;
  busy_a : out std_logic;
  p_b    : out std_logic);
end;
architecture rtl of pulsesync is
  signal t_a  : std_logic := '0';
  signal s_b  : std_logic_vector(2 downto 0) := "000";
  signal s_a  : std_logic_vector(1 downto 0) := "00";
  signal busy : std_logic;
begin
  busy <= t_a xor s_a(1);
  process(clk_a) begin
    if rising_edge(clk_a) then
      if p_a = '1' and busy = '0' then t_a <= not t_a; end if;
      s_a <= s_a(0) & s_b(2);
    end if;
  end process;
  process(clk_b) begin
    if rising_edge(clk_b) then s_b <= s_b(1 downto 0) & t_a; end if;
  end process;
  busy_a <= busy;
  p_b    <= s_b(2) xor s_b(1);
end;
