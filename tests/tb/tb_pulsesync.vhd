library ieee; use ieee.std_logic_1164.all;
use work.fuzz_pkg.all;
-- Two clocks, of periods A_PS and B_PS (first rising edges at A_PHASE_PS and
-- B_PHASE_PS), into pulsesync, and p_a toggling after each of P_T
-- (picoseconds; 0: the next delta). With pulsesync's twin, the bridged and
-- native cores must agree throughout.
entity tb_pulsesync is
    generic (
        A_PS : positive := 1000; A_PHASE_PS : natural := 0;
        B_PS : positive := 1300; B_PHASE_PS : natural := 0;
        P_T  : string := "500,1000");
end;
architecture sim of tb_pulsesync is
    signal clk_a, clk_b, p_a, busy_a, p_b : std_logic := '0';
    signal done : boolean := false;
    component pulsesync is
        port (clk_a, clk_b, p_a : in std_logic; busy_a, p_b : out std_logic);
    end component;

    procedure clock(signal c : out std_logic; signal done : in boolean;
                    period : positive; phase : natural) is
    begin
        wait for phase * 1 ps;
        while not done loop
            c <= '1';
            wait for (period / 2) * 1 ps;
            c <= '0';
            wait for (period - period / 2) * 1 ps;
        end loop;
        wait;
    end;
begin
    dut : pulsesync port map (clk_a, clk_b, p_a, busy_a, p_b);
    process begin clock(clk_a, done, A_PS, A_PHASE_PS); end process;
    process begin clock(clk_b, done, B_PS, B_PHASE_PS); end process;
    process
        constant T : integer_vector := ints(P_T);
    begin
        for i in T'range loop
            wait for T(i) * 1 ps;
            p_a <= not p_a;
        end loop;
        wait for 8 * (A_PS + B_PS) * 1 ps;
        done <= true;
        report "PASS" severity note;
        wait;
    end process;
end;
