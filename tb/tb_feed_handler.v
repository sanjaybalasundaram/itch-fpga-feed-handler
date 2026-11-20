// ============================================================================
// tb_feed_handler.v - drives the generated ITCH stream into the DUT one byte
// per clock (with optional random idle gaps), logs every BBO update and
// measures latency from the last byte of a message to the BBO output.
//
// Files (from model/itch_model.py):  sim/stream.hex  sim/config.txt
// Output: sim/actual.txt (same format as sim/expected.txt), sim/latency.txt
// Plusargs: +GAPS=1 inserts random idle cycles between bytes
// ============================================================================
`timescale 1ns/1ps
module tb_feed_handler;
    localparam MAX_BYTES = 4_000_000;

    reg         clk = 0, rst = 1;
    reg         in_valid = 0;
    reg  [7:0]  in_data  = 0;
    reg  [15:0] target_locate;
    reg  [31:0] base_price;

    wire        bbo_valid, msg_valid;
    wire [47:0] bbo_ts;
    wire [31:0] bid_px, bid_qty, ask_px, ask_qty, msgs_processed;

    feed_handler_top dut (
        .clk(clk), .rst(rst), .target_locate(target_locate), .base_price(base_price),
        .in_valid(in_valid), .in_data(in_data),
        .bbo_valid(bbo_valid), .bbo_timestamp(bbo_ts),
        .bid_price(bid_px), .bid_qty(bid_qty), .ask_price(ask_px), .ask_qty(ask_qty),
        .msg_valid(msg_valid), .msgs_processed(msgs_processed));

    always #2 clk = ~clk;          // 250 MHz reference clock (4 ns period) for reporting

    reg [7:0] mem [0:MAX_BYTES-1];
    integer   n_bytes, cfg, fd_out, fd_lat, k, gaps, r;
    integer   cycle = 0, last_msg_cycle = 0, n_bbo = 0, n_msgs = 0;
    integer   lat, lat_min = 1000, lat_max = 0, lat_sum = 0;
    integer   loc_i, base_i;

    always @(posedge clk) cycle <= cycle + 1;

    always @(posedge clk) begin
        if (msg_valid) begin
            last_msg_cycle <= cycle;
            n_msgs <= n_msgs + 1;
        end
        if (bbo_valid) begin
            $fwrite(fd_out, "%0d %0d %0d %0d %0d\n", bbo_ts, bid_px, bid_qty, ask_px, ask_qty);
            lat = cycle - last_msg_cycle;
            lat_sum = lat_sum + lat;
            if (lat < lat_min) lat_min = lat;
            if (lat > lat_max) lat_max = lat;
            n_bbo = n_bbo + 1;
        end
    end

    initial begin
        cfg = $fopen("sim/config.txt", "r");
        r = $fscanf(cfg, "%d %d %d", loc_i, base_i, n_bytes);
        $fclose(cfg);
        target_locate = loc_i;
        base_price    = base_i;
        if (!$value$plusargs("GAPS=%d", gaps)) gaps = 0;
        $readmemh("sim/stream.hex", mem, 0, n_bytes - 1);
        fd_out = $fopen("sim/actual.txt", "w");

        repeat (5) @(posedge clk);
        rst <= 0;
        @(posedge clk);

        for (k = 0; k < n_bytes; k = k + 1) begin
            in_valid <= 1'b1;
            in_data  <= mem[k];
            @(posedge clk);
            if (gaps != 0 && ($random & 7) == 0) begin   // ~1 in 8 bytes followed by idle cycles
                in_valid <= 1'b0;
                repeat (($random & 3) + 1) @(posedge clk);
            end
        end
        in_valid <= 1'b0;
        repeat (20) @(posedge clk);

        $fclose(fd_out);
        fd_lat = $fopen("sim/latency.txt", "w");
        $fwrite(fd_lat, "bytes %0d\nmessages_parsed %0d\nbook_messages %0d\nbbo_updates %0d\n",
                n_bytes, n_msgs, msgs_processed, n_bbo);
        if (n_bbo > 0)
            $fwrite(fd_lat, "latency_cycles_min %0d\nlatency_cycles_max %0d\nlatency_cycles_avg %0d.%02d\n",
                    lat_min, lat_max, lat_sum / n_bbo, (lat_sum * 100 / n_bbo) % 100);
        $fclose(fd_lat);
        $display("Simulation done: %0d bytes, %0d messages, %0d BBO updates, latency %0d-%0d cycles",
                 n_bytes, n_msgs, n_bbo, lat_min, lat_max);
        $finish;
    end
endmodule
