library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
-- The core's inputs change while it is still working through the deltas of
-- an earlier change: a second rising edge two deltas after the first, with d
-- changed between them. Counting deltas from the first edge (delta 0), in one
-- simulator: q_i takes 01 at delta 1, q 01 at 2, nq FE at 3; the second edge
-- (delta 2) samples d = 02, which reaches q_i at 3, q at 4 and nq (FD) at 5.
-- The core must see each input change in the delta after NVC makes it, even
-- while the deltas of its own earlier change are still running.
entity tb_midchain is end;
architecture sim of tb_midchain is
    signal clk : std_logic := '0';
    signal d, q, nq : std_logic_vector(7 downto 0) := (others => '0');
    signal tick   : bit := '0';      -- toggles every delta while counting
    signal delta  : integer := -1;   -- deltas since the first edge, minus 1
    signal q_seen, nq_seen : integer := 0;
    component regcomb is
        port (clk : in std_logic; d : in std_logic_vector(7 downto 0); q, nq : out std_logic_vector(7 downto 0));
    end component;
begin
    dut : regcomb port map (clk, d, q, nq);

    -- delta counts from the first edge only: in delta N (N >= 1), delta is
    -- N - 1, as an event seen there sees it
    process(clk, tick)
    begin
        if delta < 0 and rising_edge(clk) then
            delta <= 0;
            tick <= not tick;
        elsif tick'event and delta >= 0 and delta < 20 then
            delta <= delta + 1;
            tick <= not tick;
        end if;
    end process;

    -- each change of q and nq after the first edge, with its delta
    process(q, nq)
    begin
        if delta >= 0 and q'event then
            if q_seen = 0 then
                assert q = x"01" and delta + 1 = 2
                    report "first q change: " & to_hstring(q) & " at delta " & integer'image(delta + 1)
                         & " (in one simulator: 01 at 2)" severity failure;
            else
                assert q = x"02" and delta + 1 = 4
                    report "second q change: " & to_hstring(q) & " at delta " & integer'image(delta + 1)
                         & " (in one simulator: 02 at 4)" severity failure;
            end if;
            q_seen <= q_seen + 1;
        end if;
        if delta >= 0 and nq'event then
            if nq_seen = 0 then
                assert nq = x"FE" and delta + 1 = 3
                    report "first nq change: " & to_hstring(nq) & " at delta " & integer'image(delta + 1)
                         & " (in one simulator: FE at 3)" severity failure;
            else
                assert nq = x"FD" and delta + 1 = 5
                    report "second nq change: " & to_hstring(nq) & " at delta " & integer'image(delta + 1)
                         & " (in one simulator: FD at 5)" severity failure;
            end if;
            nq_seen <= nq_seen + 1;
        end if;
    end process;

    process
    begin
        d <= x"01";
        wait for 1 ns;
        clk <= '1';              -- the first edge: delta 0
        wait for 0 ns;
        d <= x"02";              -- delta 1, while the core works on the first
        clk <= '0';
        wait for 0 ns;
        clk <= '1';              -- delta 2: the second edge
        wait for 5 ns;
        assert q_seen = 2 and nq_seen = 2
            report "q changed " & integer'image(q_seen) & " times, nq " & integer'image(nq_seen)
                 & " (in one simulator: twice each)" severity failure;
        report "PASS" severity note;
        wait;
    end process;
end;
