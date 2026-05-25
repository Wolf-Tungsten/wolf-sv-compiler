module graph_assembly_memory(
    input logic clk,
    input logic en,
    input logic [3:0] addr,
    input logic [7:0] data,
    output logic [7:0] q_comb,
    output logic [7:0] q_seq
);
    logic [7:0] mem [0:15];
    assign q_comb = mem[addr];
    always @(posedge clk) begin
        if (en) begin
            q_seq <= mem[addr];
            mem[addr][3:0] <= data[3:0];
        end
    end
    initial begin
        $readmemh("mem_init.hex", mem);
        $readmemb("mem_init.bin", mem, 2, 7);
    end
endmodule

module graph_assembly_packed_aggregate_reg(
    input logic clk,
    input logic [3:0][7:0] in,
    output logic [3:0][7:0] out
);
    logic [3:0][7:0] r0;
    logic [3:0][7:0] r1;
    always_ff @(posedge clk) begin
        r0 <= in;
        r1 <= r0;
    end
    assign out = r1;
endmodule

module graph_assembly_memory_read_coalesce(
    input logic [3:0] addr,
    output logic [7:0] q,
    output logic bit0,
    output logic bit1
);
    logic [7:0] mem [0:15];
    assign q = mem[addr];
    assign bit0 = mem[addr][0];
    assign bit1 = mem[addr][1];
endmodule
