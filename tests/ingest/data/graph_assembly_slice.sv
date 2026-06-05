module graph_assembly_slice(
    input logic [7:0] data,
    input logic [127:0] packed_bits,
    input logic [3:0] dyn_idx,
    input logic [2:0] small_lane0,
    input logic [2:0] small_lane1,
    input logic small_idx,
    input logic [2:0] upto_lane0,
    input logic [2:0] upto_lane1,
    input logic upto_idx,
    input logic [5:0] start,
    output logic [7:0] y,
    output logic [7:0] lane9,
    output logic [7:0] lane_dyn,
    output logic [2:0] small_out,
    output logic [2:0] upto_out,
    output logic [63:0] shifted
);
    wire [15:0][7:0] packed_data;
    wire [1:0][2:0] small_lanes;
    wire [0:1][2:0] upto_lanes;
    wire [63:0] first_rot;

    always_comb begin
        y = 8'h00;
        y[3] = data[0];
        y[7:4] = data[7:4];
    end

    assign packed_data = packed_bits;
    assign lane9 = packed_data[4'h9];
    assign lane_dyn = packed_data[dyn_idx];
    assign small_lanes = {small_lane1, small_lane0};
    assign small_out = small_lanes[small_idx];
    assign upto_lanes = {upto_lane0, upto_lane1};
    assign upto_out = upto_lanes[upto_idx];
    assign first_rot = 64'h4000;
    assign shifted = first_rot >> (6'hB - start);
endmodule
