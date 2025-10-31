module ifstmt_top (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [3:0]  sel,
    input  logic [7:0]  data_in,
    output logic [7:0]  data_out,
    output logic        branch_flag,
    output logic [7:0]  comb_preview
);

    logic [7:0] buffer;
    logic [7:0] comb_result;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            data_out    <= '0;
            buffer      <= '0;
            branch_flag <= 1'b0;
        end else if (sel[3]) begin
            // Multiple assignments within this branch.
            buffer      <= data_in;
            data_out    <= data_in;
            branch_flag <= 1'b1;
        end else if (sel[2:1] == 2'b10) begin
            buffer      <= ~data_in;
            data_out    <= ~data_in;
            branch_flag <= 1'b0;
        end else begin
            // Nested if structure to exercise additional branches.
            if (sel[0]) begin
                buffer      <= {data_out[6:0], data_in[0]};
                branch_flag <= ~branch_flag;
            end else begin
                buffer      <= data_out + 8'd1;
                branch_flag <= 1'b0;
            end

            // Second if structure in the same always block.
            if (buffer[0])
                data_out <= buffer;
            else
                data_out <= buffer ^ 8'hFF;

            if (sel == 4'b0000)
                data_out <= 8'hAA;
        end

        // Same variable assigned in multiple ifs.
        if (sel[3] && sel[0])
            data_out <= data_out | 8'h55;
    end

    always_comb begin
        comb_result = data_in;
        if (sel[3]) begin
            comb_result = data_in ^ 8'h3C;
        end else if (sel[2]) begin
            comb_result = {sel, data_in[3:0]};
        end else begin
            if (sel[1:0] == 2'b01)
                comb_result = buffer + 8'd2;
            else if (sel[1:0] == 2'b10)
                comb_result = buffer - 8'd2;
            else
                comb_result = buffer;

            if (branch_flag)
                comb_result = comb_result & 8'hF0;
        end

        if (!branch_flag && sel == 4'b1111)
            comb_result = comb_result | 8'h0F;
    end

    assign comb_preview = comb_result;

endmodule
