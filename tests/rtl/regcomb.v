// regcomb.vhd in Verilog: a registered output and a combinational output
// derived from it, one delta apart within the core (tests/test_verilator.py)
module regcomb (input clk, input [7:0] d, output [7:0] q, output [7:0] nq);
reg [7:0] q_i = 0;
always @(posedge clk) q_i <= d;
assign q = q_i;
assign nq = ~q_i;
endmodule
