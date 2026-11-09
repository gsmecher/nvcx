// The Verilog twin of bits.vhd, behind a shell with bit ports (tests/test_bits.py). Its $display
// has a colon of its own, which is not a severity prefix's.
module bits (input a, input [3:0] v, output y, output [3:0] w);
assign y = ~a;
assign w = ~v;
initial $display("bits: up");
endmodule
