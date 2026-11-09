-- bit and bit_vector ports, each way (tests/test_bits.py)
entity bits is
  port (a : in bit; v : in bit_vector(3 downto 0); y : out bit; w : out bit_vector(3 downto 0));
end;
architecture rtl of bits is
begin
  y <= not a;
  w <= not v;
end;
