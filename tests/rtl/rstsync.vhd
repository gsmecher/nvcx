library ieee; use ieee.std_logic_1164.all;
-- A reset synchroniser: rst_n falls with arst_n, at once (asynchronously),
-- and rises on the second rising edge of clk after arst_n does.
entity rstsync is port (
  clk    : in  std_logic;
  arst_n : in  std_logic;
  rst_n  : out std_logic);
end;
architecture rtl of rstsync is
  signal r : std_logic_vector(1 downto 0) := "00";
begin
  process(clk, arst_n) begin
    if arst_n = '0' then
      r <= "00";
    elsif rising_edge(clk) then
      r <= r(0) & '1';
    end if;
  end process;
  rst_n <= r(1);
end;
