library ieee; use ieee.std_logic_1164.all;
use work.fuzz_pkg.all;
-- A clock of period CLK_PS whose first rising edge is at PHASE_PS, and
-- arst_n, asserted at first, toggling after each of RST (picoseconds; 0: in
-- the next delta). With rstsync's twin, the bridged and native cores must
-- agree throughout.
entity tb_rstsync is
    generic (CLK_PS : positive := 1000; PHASE_PS : natural := 0; RST : string := "300,2000");
end;
architecture sim of tb_rstsync is
    signal clk, arst_n : std_logic := '0';
    signal rst_n : std_logic;
    signal done : boolean := false;
    component rstsync is
        port (clk, arst_n : in std_logic; rst_n : out std_logic);
    end component;
begin
    dut : rstsync port map (clk, arst_n, rst_n);
    process
    begin
        wait for PHASE_PS * 1 ps;
        while not done loop
            clk <= '1';
            wait for (CLK_PS / 2) * 1 ps;
            clk <= '0';
            wait for (CLK_PS - CLK_PS / 2) * 1 ps;
        end loop;
        wait;
    end process;
    process
        constant T : integer_vector := ints(RST);
    begin
        for i in T'range loop
            wait for T(i) * 1 ps;
            arst_n <= not arst_n;
        end loop;
        wait for 4 * CLK_PS * 1 ps;
        done <= true;
        report "PASS" severity note;
        wait;
    end process;
end;
