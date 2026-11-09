library ieee; use ieee.std_logic_1164.all;
entity tb_stop is end;
architecture sim of tb_stop is
    signal clk, q : std_logic := '0';
    component stop is
        port (clk : in std_logic; q : out std_logic);
    end component;
begin
    -- the core stops its simulation at the third clock edge. The testbench checks nothing of the
    -- core's and passes regardless, so that only nvcx can tell the run what the core did.
    s : stop port map (clk => clk, q => q);
    process
    begin
        for i in 1 to 6 loop
            wait for 5 ns;
            clk <= '1';
            wait for 5 ns;
            clk <= '0';
        end loop;
        report "PASS";
        std.env.finish;
    end process;
end;
