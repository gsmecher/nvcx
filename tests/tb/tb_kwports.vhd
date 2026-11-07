library ieee; use ieee.std_logic_1164.all;
use work.kwports_comp.all;
entity tb_kwports is end;
architecture sim of tb_kwports is
    signal nx, upper, both : std_logic := '0';
    signal i, o : std_logic_vector(3 downto 0) := x"0";
begin
    dut : kwports port map (\next\ => nx, \In\ => upper, \in_\ => i, \out\ => o, both => both);
    process
    begin
        nx <= '1';
        upper <= '1';
        i <= x"5";
        wait for 1 ns;
        assert o = x"6" report "out: " & to_hstring(o) severity failure;
        assert both = '1' report "next and In: " & std_logic'image(both) severity failure;
        upper <= '0';
        wait for 1 ns;
        assert both = '0' report "next and not In: " & std_logic'image(both) severity failure;
        report "PASS" severity note;
        wait;
    end process;
end;
