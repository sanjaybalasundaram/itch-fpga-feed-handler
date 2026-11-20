// ============================================================================
// itch_parser.v - NASDAQ TotalView-ITCH 5.0 message parser (byte stream)
//
// Input : 1 byte per cycle (in_valid/in_data), messages in the standard
//         length-prefixed framing: 2-byte big-endian length, then the message.
// Output: one-cycle msg_valid pulse after the last byte of each message, with
//         the fields needed to maintain an order book already decoded.
//
// Decoded message types (all others are skipped using the length field):
//   'A' Add Order            'F' Add Order w/ MPID     'E' Order Executed
//   'C' Order Executed w/ Px 'X' Order Cancel          'D' Order Delete
//   'U' Order Replace
// Field offsets follow the ITCH 5.0 specification (big-endian integers).
// ============================================================================
`timescale 1ns/1ps
module itch_parser (
    input  wire        clk,
    input  wire        rst,
    input  wire        in_valid,
    input  wire [7:0]  in_data,

    output reg         msg_valid,
    output reg  [7:0]  msg_type,
    output reg  [15:0] locate,      // stock locate code
    output reg  [47:0] timestamp,   // ns since midnight
    output reg  [63:0] order_ref,   // order reference number (orig ref for 'U')
    output reg  [63:0] new_ref,     // new order reference ('U' only)
    output reg         side_buy,    // 1 = 'B' buy, 0 = 'S' sell ('A'/'F')
    output reg  [31:0] shares,      // add / executed / cancelled / replace shares
    output reg  [31:0] price        // price, 4 implied decimals ('A'/'F'/'U')
);
    localparam S_LEN_HI = 2'd0, S_LEN_LO = 2'd1, S_BODY = 2'd2;

    reg [1:0]  state;
    reg [7:0]  len_hi;
    reg [15:0] len;
    reg [15:0] cnt;          // byte offset within the message body
    reg [7:0]  mtype;        // type byte of the message being received

    // helper: true when cnt is within [a, b]
    function automatic rng(input [15:0] c, input [15:0] a, input [15:0] b);
        rng = (c >= a) && (c <= b);
    endfunction

    wire is_add  = (mtype == "A") || (mtype == "F");
    wire is_exec = (mtype == "E") || (mtype == "C") || (mtype == "X");
    wire is_del  = (mtype == "D");
    wire is_rep  = (mtype == "U");

    always @(posedge clk) begin
        if (rst) begin
            state     <= S_LEN_HI;
            msg_valid <= 1'b0;
            cnt       <= 16'd0;
            len       <= 16'd0;
            mtype     <= 8'd0;
        end else begin
            msg_valid <= 1'b0;
            if (in_valid) begin
                case (state)
                S_LEN_HI: begin
                    len_hi <= in_data;
                    state  <= S_LEN_LO;
                end
                S_LEN_LO: begin
                    len   <= {len_hi, in_data};
                    cnt   <= 16'd0;
                    state <= ({len_hi, in_data} == 16'd0) ? S_LEN_HI : S_BODY;
                end
                S_BODY: begin
                    if (cnt == 16'd0) begin
                        mtype     <= in_data;
                        msg_type  <= in_data;
                        // clear fields that are not present in every message
                        shares    <= 32'd0;
                        price     <= 32'd0;
                        new_ref   <= 64'd0;
                        side_buy  <= 1'b0;
                    end else begin
                        if (rng(cnt, 1, 2))   locate    <= {locate[7:0], in_data};
                        if (rng(cnt, 5, 10))  timestamp <= {timestamp[39:0], in_data};
                        if (rng(cnt, 11, 18) && (is_add || is_exec || is_del || is_rep))
                                              order_ref <= {order_ref[55:0], in_data};
                        if (is_add) begin
                            if (cnt == 16'd19)    side_buy <= (in_data == "B");
                            if (rng(cnt, 20, 23)) shares   <= {shares[23:0], in_data};
                            if (rng(cnt, 32, 35)) price    <= {price[23:0], in_data};
                        end
                        if (is_exec && rng(cnt, 19, 22)) shares <= {shares[23:0], in_data};
                        if (is_rep) begin
                            if (rng(cnt, 19, 26)) new_ref <= {new_ref[55:0], in_data};
                            if (rng(cnt, 27, 30)) shares  <= {shares[23:0], in_data};
                            if (rng(cnt, 31, 34)) price   <= {price[23:0], in_data};
                        end
                    end

                    if (cnt == len - 16'd1) begin
                        msg_valid <= 1'b1;     // fields include this last byte
                        state     <= S_LEN_HI;
                    end
                    cnt <= cnt + 16'd1;
                end
                default: state <= S_LEN_HI;
                endcase
            end
        end
    end
endmodule
