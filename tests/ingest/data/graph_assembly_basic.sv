module graph_assembly_basic(
    input  logic clk,
    input  logic a,
    input  logic b,
    input  logic en,
    output logic y,
    output logic q,
    output logic l
);
    assign y = a & b;

    always_ff @(posedge clk) begin
        if (en)
            q <= a;
    end

    always_latch begin
        if (en)
            l <= b;
    end
endmodule

module graph_assembly_procedural_local(
    input  logic clk,
    input  logic [2:0] a,
    input  logic [2:0] b,
    input  logic en,
    output logic [2:0] y
);
    always_ff @(posedge clk) begin
        automatic logic [2:0] tmp;
        automatic logic [2:0] next = a ^ b;
        tmp = en ? next : b;
        if (en) begin
            automatic logic [1:0] lane;
            lane = tmp[2:1];
            y <= {lane, a[0]};
        end else begin
            y <= tmp;
        end
    end
endmodule
