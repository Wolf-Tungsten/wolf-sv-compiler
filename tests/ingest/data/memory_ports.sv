package mem_pkg;
    parameter int PW = 4;
endpackage

module mem_read_comb(
    input logic [3:0] addr,
    output logic [7:0] q
);
    logic [7:0] mem [0:15];
    assign q = mem[addr];
endmodule

module mem_read_seq(
    input logic clk,
    input logic [3:0] addr,
    output logic [7:0] q
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        q <= mem[addr];
    end
endmodule

module mem_read_seq_en(
    input logic clk,
    input logic en,
    input logic [3:0] addr,
    output logic [7:0] q
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        if (en) begin
            q <= mem[addr];
        end
    end
endmodule

module mem_read_seq_self_hold(
    input logic clk,
    input logic en,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        if (en) begin
            mem[addr] <= data;
        end else begin
            mem[addr] <= mem[addr];
        end
    end
endmodule

module mem_read_element_bit(
    input logic [3:0] addr,
    input logic sel,
    output logic [2:0] q
);
    logic [2:0] mem [0:15];
    assign q = {~sel, sel ? mem[addr][1] : mem[addr][0], mem[addr][0]};
endmodule

module mem_read_cast(
    input logic [3:0] addr,
    input logic sel,
    output logic [15:0] q
);
    logic [7:0] mem [0:15];
    logic [1:0][7:0] packed_state;
    assign q = sel ? 16'(mem[addr]) : packed_state;
endmodule

module packed_aggregate_whole_reg(
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

module packed_aggregate_indexed_mem(
    input logic clk,
    input logic [1:0] addr,
    input logic [7:0] data,
    output logic [7:0] out
);
    logic [3:0][7:0] mem;
    always_ff @(posedge clk) begin
        mem[addr] <= data;
    end
    assign out = mem[addr];
endmodule

module packed_aggregate_priority_row_write(
    input logic clk,
    input logic a,
    input logic b,
    input logic [7:0] d0,
    input logic [7:0] d1,
    output logic [7:0] out
);
    logic [3:0][7:0] mem;
    always_ff @(posedge clk) begin
        if (a) begin
            mem[2'h1] <= d0;
        end
        else if (b) begin
            mem[2'h1] <= d1;
        end
    end
    assign out = mem[2'h1];
endmodule

module mem_write_mask(
    input logic clk,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][3:0] <= data[3:0];
    end
endmodule

module mem_write_dynamic_up(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: 4] <= data;
    end
endmodule

module mem_write_dynamic_down(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base -: 4] <= data;
    end
endmodule

module mem_write_dynamic_param_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    localparam int W = 4;
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: W] <= data;
    end
endmodule

module mem_write_dynamic_expr_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: (2 + 2)] <= data;
    end
endmodule

module mem_write_dynamic_pkg_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    import mem_pkg::*;
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: PW] <= data;
    end
endmodule

module mem_write_dynamic_pkg_qualified(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: mem_pkg::PW] <= data;
    end
endmodule

module mem_write_dynamic_expr_complex(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: ((1 ? mem_pkg::PW : 2) + 0)] <= data;
    end
endmodule

module mem_write_dynamic_concat_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][2 +: {2'b01, 2'b00}] <= data[3:0];
    end
endmodule

module mem_write_dynamic_concat_expr_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][1 +: {(1'd1 + 1'd0), 2'b01}] <= data[4:0];
    end
endmodule

module mem_write_dynamic_repl_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][1 +: {2{2'b01}}] <= data[4:0];
    end
endmodule

module mem_write_dynamic_repl_expr_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][1 +: {(1 + 1){2'b01}}] <= data[4:0];
    end
endmodule

module mem_write_dynamic_base_warn(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: 4] <= data;
    end
endmodule

module mem_write_multi_dim(
    input logic clk,
    input logic [1:0] idx0,
    input logic [2:0] idx1,
    input logic [7:0] data
);
    logic [7:0] mem [0:3][0:7];
    always_ff @(posedge clk) begin
        mem[idx0][idx1] <= data;
    end
endmodule

module mem_write_multi_dim_offset(
    input logic clk,
    input logic [2:0] idx0,
    input logic [2:0] idx1,
    input logic [7:0] data
);
    logic [7:0] mem [4:7][7:4];
    always_ff @(posedge clk) begin
        mem[idx0][idx1] <= data;
    end
endmodule

module mem_write_range_oob(
    input logic clk,
    input logic [3:0] addr,
    input logic [6:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][10:4] <= data;
    end
endmodule

module mem_write_dynamic_bad_width(
    input logic clk,
    input logic [3:0] addr,
    input logic [2:0] base,
    input logic [2:0] width,
    input logic [7:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][base +: width] <= data;
    end
endmodule

module mem_write_dynamic_oob_up(
    input logic clk,
    input logic [3:0] addr,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][6 +: 4] <= data;
    end
endmodule

module mem_write_dynamic_oob_down(
    input logic clk,
    input logic [3:0] addr,
    input logic [3:0] data
);
    logic [7:0] mem [0:15];
    always_ff @(posedge clk) begin
        mem[addr][1 -: 4] <= data;
    end
endmodule
