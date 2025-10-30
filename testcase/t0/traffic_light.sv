module traffic_light_controller #(
    parameter int GREEN_TICKS  = 5,
    parameter int YELLOW_TICKS = 2,
    parameter int RED_TICKS    = 5
) (
    input  logic clk,
    input  logic rst_n,
    output logic green_on,
    output logic yellow_on,
    output logic red_on
);
    typedef enum logic [1:0] {
        STATE_GREEN,
        STATE_YELLOW,
        STATE_RED
    } state_t;

    state_t state, next_state;
    int     counter, next_counter;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state   <= STATE_RED;
            counter <= RED_TICKS;
        end
        else begin
            state   <= next_state;
            counter <= next_counter;
        end
    end

    // Rotate through light phases with configurable dwell counts.
    always_comb begin
        next_state   = state;
        next_counter = counter;

        unique case (state)
            STATE_GREEN: begin
                if (counter == 0) begin
                    next_state   = STATE_YELLOW;
                    next_counter = YELLOW_TICKS - 1;
                end
                else begin
                    next_counter = counter - 1;
                end
            end
            STATE_YELLOW: begin
                if (counter == 0) begin
                    next_state   = STATE_RED;
                    next_counter = RED_TICKS - 1;
                end
                else begin
                    next_counter = counter - 1;
                end
            end
            STATE_RED: begin
                if (counter == 0) begin
                    next_state   = STATE_GREEN;
                    next_counter = GREEN_TICKS - 1;
                end
                else begin
                    next_counter = counter - 1;
                end
            end
            default: begin
                next_state   = STATE_RED;
                next_counter = RED_TICKS - 1;
            end
        endcase;
    end

    assign green_on  = (state == STATE_GREEN);
    assign yellow_on = (state == STATE_YELLOW);
    assign red_on    = (state == STATE_RED);
endmodule

module traffic_light_top (
    input  logic clk,
    input  logic rst_n,
    output logic [2:0] lights,
    output logic [2:0] lights1
);
    traffic_light_controller #(
        .GREEN_TICKS(4),
        .YELLOW_TICKS(2),
        .RED_TICKS(6)
    ) u_ctrl (
        .clk       (clk),
        .rst_n     (rst_n),
        .green_on  (lights[0]),
        .yellow_on (lights[1]),
        .red_on    (lights[2])
    );

    traffic_light_controller #(
        .GREEN_TICKS(4),
        .YELLOW_TICKS(2),
        .RED_TICKS(6)
    ) u_ctrl_1 (
        .clk       (clk),
        .rst_n     (rst_n),
        .green_on  (lights1[0]),
        .yellow_on (lights1[1]),
        .red_on    (lights1[2])
    );
endmodule
