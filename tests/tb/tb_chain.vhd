library ieee; use ieee.std_logic_1164.all;
use work.fuzz_pkg.all;
-- a toggles after each of PULSES (picoseconds; 0: in the next delta) and goes
-- through two instances of chain, delaying T_PS and T2_PS: two bridged cores
-- whose timed events fall on the same and on different time steps.
entity tb_chain is
    generic (T_PS : natural := 1000; T2_PS : natural := 700; PULSES : string := "500,500");
end;
architecture sim of tb_chain is
    signal a : std_logic := '0';
    signal delay_ps, delay2_ps : natural := 0;
    signal y, z, w, y2, z2, w2 : std_logic;
    component chain is
        port (a : in std_logic; t_ps : in natural; y, z, w : out std_logic);
    end component;
begin
    u0 : chain port map (a, delay_ps, y, z, w);
    u1 : chain port map (a, delay2_ps, y2, z2, w2);
    process
        constant P : integer_vector := ints(PULSES);
    begin
        -- The delays go in once the cores have initialised: xsim loses the
        -- initial value of a signal's zero-delay dependents (y <= yi) when
        -- an input feeding that signal's `after` delay changes during its
        -- initialisation (a separate, known xsim/XSI quirk).
        wait for 500 ps;
        delay_ps <= T_PS;
        delay2_ps <= T2_PS;
        wait for 500 ps;
        for i in P'range loop
            a <= not a;
            wait for P(i) * 1 ps;
        end loop;
        wait for 2 * (T_PS + T2_PS) * 1 ps + 1 ns;
        report "PASS" severity note;
        wait;
    end process;
end;
