-- std.env.stop at the third clock edge (tests/test_regress.py)
library ieee; use ieee.std_logic_1164.all;
entity stop is
    port (clk : in std_logic; q : out std_logic := '0');
end;
architecture rtl of stop is
begin
    process (clk)
        variable n : natural := 0;
        variable v : std_logic := '0';
    begin
        if rising_edge(clk) then
            n := n + 1;
            v := not v;
            q <= v;
            if n = 3 then
                std.env.stop;
            end if;
        end if;
    end process;
end;
