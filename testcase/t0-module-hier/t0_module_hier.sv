typedef struct packed {
    logic [15:0] data;
    logic [3:0]  id;
    logic        valid;
} request_t;

interface config_if #(parameter int WIDTH = 16) (input logic clk);
    logic [WIDTH-1:0] cfg_data;
    logic             cfg_valid;

    modport producer (input clk, output cfg_data, output cfg_valid);
    modport consumer (input clk, input cfg_data, input cfg_valid);
endinterface

module status_bundle_adapter (.status_bus({status_even, status_odd}));
    output logic status_even;
    output logic status_odd;

    assign status_even = 1'b1;
    assign status_odd  = 1'b0;
endmodule

module config_probe #(
    parameter int WIDTH = 16
) (
    input  logic        clk,
    config_if.consumer  cfg
);
    logic [WIDTH-1:0] captured;

    always_ff @(posedge clk) begin
        if (cfg.cfg_valid) begin
            captured <= cfg.cfg_data;
        end
    end
endmodule

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
    clk,
    upstream,
    combined,
    .status_bus({status_even, status_odd}),
    cfg_link
);
    input  logic     clk;
    input  request_t upstream;
    output logic [WIDTH-1:0] combined;
    output logic             status_even;
    output logic             status_odd;
    config_if.consumer       cfg_link;

    request_t staged_req;

    // Simple structural assignment to demonstrate struct ports.
    assign staged_req = '{data: upstream.data, id: upstream.id + 1, valid: upstream.valid};

    logic [WIDTH-1:0] aggregate_value;
    logic [WIDTH-1:0] config_value;
    logic [WIDTH-1:0] status_vector;
    logic [WIDTH-1:0] cfg_sampled;

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

    status_bundle_adapter u_status_adapter (
        .status_bus({status_even, status_odd})
    );

    always_comb begin
        status_vector = '0;
        if (WIDTH > 0) begin
            status_vector[0] = status_odd;
        end
        if (WIDTH > 1) begin
            status_vector[1] = status_even;
        end
    end

    config_probe #(
        .WIDTH(WIDTH)
    ) u_cfg_probe (
        .clk(clk),
        .cfg(cfg_link)
    );

    assign cfg_sampled = cfg_link.cfg_valid ? cfg_link.cfg_data : '0;
    assign combined = config_value ^ aggregate_value ^ status_vector ^ cfg_sampled;
endmodule
