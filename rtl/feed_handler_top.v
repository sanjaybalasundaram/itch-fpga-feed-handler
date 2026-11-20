// ============================================================================
// feed_handler_top.v - ITCH 5.0 feed handler: parser -> order book -> BBO
// ============================================================================
`timescale 1ns/1ps
module feed_handler_top #(
    parameter LVL_BITS = 6,
    parameter ORD_BITS = 10,
    parameter TICK     = 100
)(
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] target_locate,
    input  wire [31:0] base_price,
    input  wire        in_valid,
    input  wire [7:0]  in_data,

    output wire        bbo_valid,
    output wire [47:0] bbo_timestamp,
    output wire [31:0] bid_price,
    output wire [31:0] bid_qty,
    output wire [31:0] ask_price,
    output wire [31:0] ask_qty,
    output wire        msg_valid,          // exposed for latency measurement
    output wire [31:0] msgs_processed
);
    wire [7:0]  msg_type;
    wire [15:0] locate;
    wire [47:0] timestamp;
    wire [63:0] order_ref, new_ref;
    wire        side_buy;
    wire [31:0] shares, price;

    itch_parser u_parser (
        .clk(clk), .rst(rst), .in_valid(in_valid), .in_data(in_data),
        .msg_valid(msg_valid), .msg_type(msg_type), .locate(locate), .timestamp(timestamp),
        .order_ref(order_ref), .new_ref(new_ref), .side_buy(side_buy), .shares(shares), .price(price));

    order_book #(.LVL_BITS(LVL_BITS), .ORD_BITS(ORD_BITS), .TICK(TICK)) u_book (
        .clk(clk), .rst(rst), .target_locate(target_locate), .base_price(base_price),
        .msg_valid(msg_valid), .msg_type(msg_type), .locate(locate), .timestamp(timestamp),
        .order_ref(order_ref), .new_ref(new_ref), .side_buy(side_buy), .shares(shares), .price(price),
        .bbo_valid(bbo_valid), .bbo_timestamp(bbo_timestamp),
        .bid_price(bid_price), .bid_qty(bid_qty), .ask_price(ask_price), .ask_qty(ask_qty),
        .msgs_processed(msgs_processed));
endmodule
