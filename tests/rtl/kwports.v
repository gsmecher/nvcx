// Ports whose names VHDL can only spell as extended identifiers: reserved
// words (next, out, and In, whose case is then part of its name) and a name
// that is not a basic identifier (in_) (tests/test_regress.py)
module kwports (input next, input In, input [3:0] in_, output [3:0] out, output both);
assign out = in_ + 4'd1;
assign both = next & In;
endmodule
