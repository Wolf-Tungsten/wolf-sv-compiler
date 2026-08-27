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

module graph_assembly_memory_priority(
    input logic clk,
    input logic en0,
    input logic en1,
    input logic [3:0] addr0,
    input logic [3:0] addr1,
    input logic [7:0] data0,
    input logic [7:0] data1,
    output logic [7:0] q
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        if (en0)
            mem[addr0] <= data0;
        if (en1)
            mem[addr1] <= data1;
    end
    assign q = mem[addr0];
endmodule

module graph_assembly_packed_aggregate_sink(
    input logic [8:0] in,
    output logic [8:0] out
);
    assign out = in;
endmodule

module graph_assembly_packed_aggregate_instance_sink(
    input logic clk,
    input logic reset,
    input logic [8:0] start_idx,
    output logic [8:0] out0,
    output logic [8:0] out1
);
    logic [7:0][8:0] t0;
    logic [7:0][8:0] t1;
    logic [7:0][8:0] t2;

    assign t0 = {
        start_idx + 9'h7,
        start_idx + 9'h6,
        start_idx + 9'h5,
        start_idx + 9'h4,
        start_idx + 9'h3,
        start_idx + 9'h2,
        start_idx + 9'h1,
        start_idx
    };

    always_ff @(posedge clk) begin
        if (reset) begin
            t1 <= '0;
            t2 <= '0;
        end
        else begin
            t1 <= t0;
            t2 <= t1;
        end
    end

    graph_assembly_packed_aggregate_sink u0 (
        .in(t2[3'h0]),
        .out(out0)
    );

    graph_assembly_packed_aggregate_sink u1 (
        .in(t2[3'h1]),
        .out(out1)
    );
endmodule
