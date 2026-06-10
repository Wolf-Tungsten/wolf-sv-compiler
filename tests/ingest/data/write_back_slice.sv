module write_back_slice_static(
    input logic [7:0] data,
    output logic [7:0] y
);
    always_comb begin
        y = 8'h00;
        y[3] = data[0];
        y[7:4] = data[7:4];
    end
endmodule

module write_back_slice_dynamic(
    input logic [7:0] data,
    input logic [2:0] idx,
    output logic [7:0] y
);
    always_comb begin
        y = 8'h00;
        y[idx] = data[0];
        y[idx +: 2] = data[2:1];
    end
endmodule

typedef struct packed {
    logic [3:0] hi;
    logic [3:0] lo;
} wb_pair_t;

module write_back_slice_member(
    input logic [3:0] a,
    input logic [3:0] b,
    output wb_pair_t y
);
    always_comb begin
        y.hi = a;
        y.lo = b;
    end
endmodule

module write_back_whole_self_slice_acyclic(
    input logic [7:0] data,
    output logic [6:0] y
);
    wire [6:0] left =
        {data[3], data[0], data[6], data[1], data[7], data[2], data[3]};
    wire [6:0] right =
        {data[6], data[4], data[7], y[1],    data[4], data[7], data[0]};
    assign y = left ^ right;
endmodule

module write_back_whole_self_slice_true_loop(
    input logic [6:0] data,
    output logic [6:0] y
);
    assign y = data ^ {6'b0, y[0]};
endmodule

module write_back_packed_array_self_slice_acyclic(
    input logic [41:0] data,
    output logic [41:0] out
);
    wire [2:0][13:0] y;
    wire [41:0] left =
        {{14{data[0]}}, {14{data[1]}}, {14{data[2]}}};
    wire [41:0] right =
        {{y[2'h1][11:0], 2'h2},
         {y[2'h0][11:0], 2'h1},
         data[13:0]};
    assign y = left ^ right;
    assign out = y;
endmodule

module write_back_mutual_self_slice_acyclic(
    input logic [7:0] data,
    output logic [7:0] out
);
    wire [7:0] a;
    wire [7:0] b;
    assign a = {b[6:0], data[0]};
    assign b = {data[7], a[5:0], data[1]};
    assign out = a ^ b;
endmodule

module write_back_indirect_mutual_self_slice_acyclic(
    input logic [7:0] data,
    output logic [7:0] out
);
    wire [7:0] a;
    wire [7:0] b;
    wire [7:0] gen_a = {b[6:0], data[0]};
    wire [7:0] gen_b = {data[7], a[5:0], data[1]};
    assign a = gen_a;
    assign b = gen_b;
    assign out = a ^ b;
endmodule
