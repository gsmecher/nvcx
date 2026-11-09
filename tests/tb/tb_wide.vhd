library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.dut_comp.all;
-- Ports wider than 64 bits (several words each way), with a carry across
-- word boundaries
entity tb_wide is end;
architecture sim of tb_wide is
    constant W : natural := 100;
    signal clk : std_logic := '0';
    signal u_in, u_out, u_inc : unsigned(W-1 downto 0) := (others => '0');
    signal s_out : signed(W-1 downto 0);
    signal v_out : std_logic_vector(W-1 downto 0);
    constant PATTERN : std_logic_vector(W-1 downto 0) := x"A" & x"5A5A5A5A_12345678_9ABCDEF0";
begin
    d : dut generic map (WIDTH => W)
        port map (clk, '0', u_in, u_out, u_inc, (others => '1'), s_out, PATTERN, v_out);
    process
    begin
        -- the low 96 bits all ones: their increment carries into the top word
        u_in <= x"3" & x"FFFFFFFF_FFFFFFFF_FFFFFFFF";
        wait for 1 ns;
        clk <= '1'; wait for 5 ns; clk <= '0'; wait for 4 ns;
        assert u_out = x"3" & x"FFFFFFFF_FFFFFFFF_FFFFFFFF" report "u_out: " & to_hstring(u_out) severity failure;
        assert u_inc = x"4" & x"00000000_00000000_00000000" report "u_inc: " & to_hstring(u_inc) severity failure;
        assert v_out = PATTERN report "v_out: " & to_hstring(v_out) severity failure;
        assert s_out = -1 report "s_out: " & to_hstring(s_out) severity failure;
        report "PASS" severity note;
        wait;
    end process;
end;
