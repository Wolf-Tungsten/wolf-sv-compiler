module aggregate_lane_child(
    input logic [3:0][5:0] req,
    output logic [3:0][5:0] out
);
    assign out = {req[3] | 6'h01, req[2] | 6'h02, req[1] | 6'h04, req[0] | 6'h08};
endmodule

module graph_assembly_aggregate_port_slice(
    input logic [5:0] req0,
    input logic [5:0] req1,
    input logic [5:0] req2,
    output logic [5:0] y
);
    logic [5:0] feedback;
    logic [5:0] req3;
    logic [3:0][5:0] child_req;
    logic [3:0][5:0] child_out;
    wire [3:0][5:0] packed_req_net;
    logic [5:0] static_lane;

    assign req3 = req2 & ~feedback;
    assign child_req = {{req3}, {req2}, {req1}, {req0}};
    assign packed_req_net = {{req3}, {req2}, {req1}, {req0}};
    assign static_lane = packed_req_net[2'h2];
    assign feedback = static_lane;
    assign y = feedback ^ child_out[2'h2];

    aggregate_lane_child u_child(
        .req(child_req),
        .out(child_out)
    );
endmodule
