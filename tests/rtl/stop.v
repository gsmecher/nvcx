// $stop at the third clock edge (tests/test_regress.py, tests/test_verilator.py)
module stop (input clk, output reg q = 0);
reg [1:0] n = 0;
always @(posedge clk) begin
    n <= n + 1;
    q <= ~q;
    if (n == 2)
        $stop;
end
endmodule
