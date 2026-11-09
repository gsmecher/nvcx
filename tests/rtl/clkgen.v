// clkgen.vhd in Verilog: the core's own clock, and a counter registered on
// it (tests/test_verilator.py)
`timescale 1ns/1ps
module clkgen (output clk_out, output [7:0] cnt_out);
reg clk = 0;
reg [7:0] cnt = 0;
always #5 clk = ~clk;
always @(posedge clk) cnt <= cnt + 1;
assign clk_out = clk;
assign cnt_out = cnt;
endmodule
