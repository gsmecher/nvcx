library ieee; use ieee.std_logic_1164.all;
use work.fuzz_pkg.all;
-- a toggles after each of PULSES (picoseconds; 0: in the next delta), and
-- goes through pdelay's three kinds of delay of T_PS, rejecting below R_PS.
-- With pdelay's twin, the bridged and native cores must agree throughout.
entity tb_pdelay is
    generic (T_PS : natural := 1000; R_PS : natural := 0; PULSES : string := "500,500");
end;
architecture sim of tb_pdelay is
    signal a : std_logic := '0';
    signal delay_ps, reject_ps : natural := 0;
    signal y_tr, y_in, y_rj : std_logic;
    component pdelay is
        port (a : in std_logic; t_ps, r_ps : in natural; y_tr, y_in, y_rj : out std_logic);
    end component;
begin
    dut : pdelay port map (a, delay_ps, reject_ps, y_tr, y_in, y_rj);
    process
        constant P : integer_vector := ints(PULSES);
    begin
        delay_ps <= T_PS;
        reject_ps <= R_PS;
        wait for 1 ns;
        for i in P'range loop
            a <= not a;
            wait for P(i) * 1 ps;
        end loop;
        wait for T_PS * 1 ps + 1 ns;
        report "PASS" severity note;
        wait;
    end process;
end;
