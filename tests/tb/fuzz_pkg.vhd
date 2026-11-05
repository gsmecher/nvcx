-- For the property-based testbenches (tests/test_fuzz.py): their stimulus
-- comes in as a generic string of comma-separated naturals, and for several
-- instances, one such field each, separated by semicolons.
package fuzz_pkg is
    function ints(s : string) return integer_vector;
    function field(s : string; n : natural) return string;
end;
package body fuzz_pkg is
    function ints(s : string) return integer_vector is
        variable n, count : natural := 0;
        variable v : integer_vector(0 to s'length);
        variable any : boolean := false;
    begin
        for i in s'range loop
            if s(i) = ',' then
                v(count) := n;
                count := count + 1;
                n := 0;
            else
                n := n * 10 + character'pos(s(i)) - character'pos('0');
                any := true;
            end if;
        end loop;
        if any then
            v(count) := n;
            count := count + 1;
        end if;
        return v(0 to count - 1);
    end;

    function field(s : string; n : natural) return string is
        variable first : positive := s'left;
        variable count : natural := 0;
    begin
        for i in s'range loop
            if s(i) = ';' then
                if count = n then
                    return s(first to i - 1);
                end if;
                count := count + 1;
                first := i + 1;
            end if;
        end loop;
        assert count = n
            report "field " & integer'image(n) & " of """ & s & """: there is none"
            severity failure;
        return s(first to s'right);
    end;
end;
