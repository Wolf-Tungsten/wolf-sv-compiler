module graph_assembly_slice(
    input logic [7:0] data,
    input logic [127:0] packed_bits,
    input logic [5:0] start,
    output logic [7:0] y,
    output logic [7:0] lane9,
    output logic [63:0] shifted
);
    wire [15:0][7:0] packed_data;
    wire [63:0] first_rot;

    always_comb begin
        y = 8'h00;
        y[3] = data[0];
        y[7:4] = data[7:4];
    end

    assign packed_data = packed_bits;
    assign lane9 = packed_data[4'h9];
    assign first_rot = 64'h4000;
    assign shifted = first_rot >> (6'hB - start);
endmodule
