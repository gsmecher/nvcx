library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.fuzz_pkg.all;
-- K ring stages in a ring, each one's x the previous one's y, port to port, and
-- the first's x either the last's y, through an NVC assignment, or (while
-- inj is high) seed: a zero-delay ring through K cores, or a loop through
-- them, counting up to the limits (LIMITS, one per ring) within a time
-- step, and settling there whatever is in flight. After each of INJ_T (picoseconds; 0: the next delta) inj toggles and
-- seed takes the matching SEEDS value, in the middle of a loop as often as
-- not. With ring's twin, the bridged and native cores must agree throughout.
entity tb_ring is
    generic (
        K      : positive := 2;
        LIMITS : string := "50,60";
        INJ_T  : string := "1000,0,1000";
        SEEDS  : string := "3,4,5");
end;
architecture sim of tb_ring is
    type u8_vector is array (natural range <>) of unsigned(7 downto 0);
    constant LIM : integer_vector := ints(LIMITS);
    signal x, y, n : u8_vector(0 to K - 1) := (others => (others => '0'));
    signal odd : std_logic_vector(0 to K - 1);
    signal inj : std_logic := '1';
    signal seed : unsigned(7 downto 0) := (others => '0');
    component ring is
        port (x, n : in unsigned(7 downto 0); y : out unsigned(7 downto 0); odd : out std_logic);
    end component;
begin
    x(0) <= seed when inj = '1' else y(K - 1);
    g : for i in 0 to K - 1 generate
        n(i) <= to_unsigned(LIM(i), 8);
        first : if i = 0 generate
            dut : ring port map (x(0), n(i), y(i), odd(i));
        else generate
            dut : ring port map (y(i - 1), n(i), y(i), odd(i));
        end generate;
    end generate;
    process
        constant T : integer_vector := ints(INJ_T);
        constant S : integer_vector := ints(SEEDS);
    begin
        for i in T'range loop
            wait for T(i) * 1 ps;
            inj <= not inj;
            seed <= to_unsigned(S(i), 8);
        end loop;
        wait for 1 ns;
        report "PASS" severity note;
        wait;
    end process;
end;
