library ieee; use ieee.std_logic_1164.all;
use work.fuzz_pkg.all;
-- K reset synchronisers, each with a clock of its own (period PERIODS,
-- first rising edge PHASES) and a reset stimulus of its own (RESETS: arst_n, asserted
-- at first, toggling after each value; picoseconds, 0: the next delta). The
-- generics hold one field per instance, separated by ';'. With CHAIN, each
-- one's reset is also held by the previous one's output, through NVC: cores
-- depending on one another within a time step. With rstsync's twin, every
-- instance's bridged and native cores must agree throughout.
entity tb_rstsync_multi is
    generic (
        K     : positive := 2;
        CHAIN : boolean := false;
        PERIODS : string := "1000;1300";
        PHASES  : string := "0;0";
        RESETS  : string := "300,2000;700,1500");
end;
architecture sim of tb_rstsync_multi is
    component rstsync is
        port (clk, arst_n : in std_logic; rst_n : out std_logic);
    end component;
    signal clk, stim, arst_n, rst_n, done : std_logic_vector(0 to K - 1) := (others => '0');
begin
    g : for i in 0 to K - 1 generate
        constant P  : positive := ints(field(PERIODS, i))(0);
        constant PH : natural := ints(field(PHASES, i))(0);
        constant T  : integer_vector := ints(field(RESETS, i));
    begin
        dut : rstsync port map (clk(i), arst_n(i), rst_n(i));
        chained : if CHAIN and i > 0 generate
            arst_n(i) <= stim(i) and rst_n(i - 1);
        else generate
            arst_n(i) <= stim(i);
        end generate;
        process
        begin
            wait for PH * 1 ps;
            while done(i) = '0' loop
                clk(i) <= '1';
                wait for (P / 2) * 1 ps;
                clk(i) <= '0';
                wait for (P - P / 2) * 1 ps;
            end loop;
            wait;
        end process;
        process
        begin
            for j in T'range loop
                wait for T(j) * 1 ps;
                stim(i) <= not stim(i);
            end loop;
            wait for 4 * P * 1 ps;
            done(i) <= '1';
            wait;
        end process;
    end generate;
    process
    begin
        wait until done = (0 to K - 1 => '1');
        report "PASS" severity note;
        wait;
    end process;
end;
