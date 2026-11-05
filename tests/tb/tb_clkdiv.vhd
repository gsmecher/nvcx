library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.fuzz_pkg.all;
-- A clock of period CLK_PS (first rising edge at PHASE_PS) into clkdiv; en
-- toggling after each of EN_T, and d taking each of D_V after the matching
-- D_T (picoseconds; 0: the next delta). With FEEDBACK, d is also xored with
-- a counter outside the core, on the divided clock the core puts out. With
-- clkdiv's twin, the bridged and native cores must agree throughout.
entity tb_clkdiv is
    generic (
        CLK_PS   : positive := 1000;
        PHASE_PS : natural := 0;
        EN_T     : string := "1500,3000";
        D_T      : string := "0,500,1000";
        D_V      : string := "1,2,3";
        FEEDBACK : boolean := false);
end;
architecture sim of tb_clkdiv is
    signal clk, en, clk_d2 : std_logic := '0';
    signal stim, cnt, d, q_d2, q_g, q_x : unsigned(7 downto 0) := (others => '0');
    signal done : boolean := false;
    component clkdiv is
        port (clk, en : in std_logic; d : in unsigned(7 downto 0);
              clk_d2 : out std_logic; q_d2, q_g, q_x : out unsigned(7 downto 0));
    end component;
begin
    dut : clkdiv port map (clk, en, d, clk_d2, q_d2, q_g, q_x);
    d <= stim xor cnt when FEEDBACK else stim;
    process(clk_d2) begin
        if rising_edge(clk_d2) then cnt <= cnt + 1; end if;
    end process;
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
        constant T : integer_vector := ints(EN_T);
    begin
        for i in T'range loop
            wait for T(i) * 1 ps;
            en <= not en;
        end loop;
        wait;
    end process;
    process
        constant T : integer_vector := ints(D_T);
        constant V : integer_vector := ints(D_V);
    begin
        for i in T'range loop
            wait for T(i) * 1 ps;
            stim <= to_unsigned(V(i), 8);
        end loop;
        wait for 6 * CLK_PS * 1 ps;
        done <= true;
        report "PASS" severity note;
        wait;
    end process;
end;
