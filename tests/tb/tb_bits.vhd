entity tb_bits is end;
architecture sim of tb_bits is
    component bits is
        port (a : in bit; v : in bit_vector(3 downto 0); y : out bit; w : out bit_vector(3 downto 0));
    end component;
    signal a, y : bit;
    signal v, w : bit_vector(3 downto 0);
begin
    b : bits port map (a => a, v => v, y => y, w => w);
    process
    begin
        -- bit's '0' and '1' are positions 0 and 1, std_ulogic's 2 and 3: each must cross a
        -- Verilog core's boundary as the value, both ways
        a <= '1'; v <= "1010";
        wait for 10 ns;
        assert y = '0' and w = "0101"
            report "FAIL: y=" & bit'image(y) & " w=" & to_string(w) severity failure;
        a <= '0'; v <= "0011";
        wait for 10 ns;
        assert y = '1' and w = "1100"
            report "FAIL: y=" & bit'image(y) & " w=" & to_string(w) severity failure;
        report "PASS" severity note;
        wait;
    end process;
end;
