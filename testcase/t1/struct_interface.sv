typedef struct packed {
    logic [7:0] payload;
    logic       valid;
    logic [1:0] channel;
} packet_t;

module packet_register (
    input  logic     clk,
    input  logic     rst_n,
    input  packet_t  in_pkt,
    output packet_t  out_pkt
);
    packet_t pkt_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            pkt_reg <= '{default: '0};
        else
            pkt_reg.payload <= in_pkt.payload;
    end

    assign out_pkt = pkt_reg;
endmodule

module struct_interface_top (
    input  logic     clk,
    input  logic     rst_n,
    input  packet_t  upstream,
    output packet_t  downstream
);
    packet_t stage_pkt;

    packet_register u_reg (
        .clk    (clk),
        .rst_n  (rst_n),
        .in_pkt (upstream),
        .out_pkt(stage_pkt)
    );

    assign downstream = '{
        payload: stage_pkt.payload + 8'h1,
        valid:   stage_pkt.valid,
        channel: stage_pkt.channel ^ 2'b11
    };
endmodule
