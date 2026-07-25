import "DPI-C" function void dpi_capture(input logic [7:0] in_val, output logic [7:0] out_val);
import "DPI-C" function int dpi_add(input int lhs, input int rhs);
import "DPI-C" function longint difftest_ram_read(input longint rIdx);

module graph_assembly_dpi_display(
    input logic clk,
    input logic [7:0] a,
    input logic [7:0] b,
    output logic [7:0] y
);
    always @(posedge clk) begin
        $display("a=%0d", a);
        $error("oops");
        dpi_capture(a, y);
        dpi_add(a, b);
    end
endmodule

module graph_assembly_dpi_comb_return(
    input logic r_enable,
    input logic [63:0] r_index,
    output logic [63:0] r_data
);
    always @(*) begin
        r_data = 64'b0;
        if (r_enable) begin
            r_data = difftest_ram_read(r_index);
        end
    end
endmodule
