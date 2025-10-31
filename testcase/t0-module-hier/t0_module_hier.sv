typedef struct packed {
    logic [15:0] data;
    logic [3:0]  id;
    logic        valid;
} request_t;

module leaf_stage #(
    parameter int WIDTH  = 16,
    parameter int OFFSET = 0
) (
    input  logic     clk,
    input  request_t req,
    output logic [WIDTH-1:0] masked_out
);
    logic [WIDTH-1:0] id_ext;
    assign id_ext = {{(WIDTH-4){1'b0}}, req.id};

    // Simple combinational manipulation using parameters.
    assign masked_out = (req.data >> OFFSET) ^ (req.valid ? id_ext : '0);
endmodule

// Blackbox module: port-only declaration, empty body.
module blackbox_stage #(
    parameter int WIDTH = 16
) (
    input  logic     clk,
    input  request_t req,
    output logic [WIDTH-1:0] shaped_out
);
endmodule

module aggregator #(
    parameter int WIDTH = 16,
    parameter int COUNT = 3
) (
    input  logic     clk,
    input  request_t upstream,
    output logic [WIDTH-1:0] aggregated
);
    logic [WIDTH-1:0] leaf_data [COUNT];

    // Generate hierarchy: build a configurable array of leaf stages.
    genvar g;
    generate
        for (g = 0; g < COUNT; g++) begin : gen_leafs
            localparam int OFFSET = g * 2;
            leaf_stage #(
                .WIDTH (WIDTH),
                .OFFSET(OFFSET)
            ) u_leaf (
                .clk       (clk),
                .req       (upstream),
                .masked_out(leaf_data[g])
            );
        end
    endgenerate

    // Instantiate a blackbox stage to ensure it's part of the hierarchy.
    blackbox_stage #(
        .WIDTH(WIDTH)
    ) u_blackbox (
        .clk      (clk),
        .req      (upstream),
        .shaped_out()
    );

    always_comb begin
        aggregated = '0;
        for (int i = 0; i < COUNT; i++) begin
            aggregated ^= leaf_data[i];
        end
    end
endmodule

module t0_module_hier_top #(
    parameter int WIDTH      = 16,
    parameter int LEAF_COUNT = 3
) (
    input  logic     clk,
    input  request_t upstream,
    output logic [WIDTH-1:0] combined
);
    request_t staged_req;

    // Simple structural assignment to demonstrate struct ports.
    assign staged_req = '{data: upstream.data, id: upstream.id + 1, valid: upstream.valid};

    logic [WIDTH-1:0] aggregate_value;
    logic [WIDTH-1:0] config_value;

    aggregator #(
        .WIDTH(WIDTH),
        .COUNT(LEAF_COUNT)
    ) u_aggregator (
        .clk      (clk),
        .upstream (staged_req),
        .aggregated(aggregate_value)
    );

    leaf_stage #(
        .WIDTH (WIDTH),
        .OFFSET(1)
    ) u_config_leaf (
        .clk       (clk),
        .req       (staged_req),
        .masked_out(config_value)
    );

    assign combined = config_value ^ aggregate_value;
endmodule
