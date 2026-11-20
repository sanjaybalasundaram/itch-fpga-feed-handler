// ============================================================================
// order_book.v - single-instrument limit order book with top-of-book output
//
// * Order table : direct-mapped by the low ORD_BITS of the order reference.
//                 Entry = {valid, is_bid, level_index, shares}. One registered
//                 read port + one write port, so it maps onto block RAM.
// * Price levels: LEVELS ticks of TICK (price units, 4 implied decimals)
//                 starting at base_price. Aggregate shares per level per side.
// * Top of book : priority encoders pick the highest non-empty bid level and
//                 the lowest non-empty ask level. A bbo_valid pulse is emitted
//                 only when best bid/ask price or size actually changes.
//
// Processing takes 3-4 clock cycles per message. The shortest book message
// ('D', 19 bytes + 2 length bytes) takes 21 cycles to arrive at 1 byte/cycle,
// so the book always finishes before the next message is complete; no
// back-pressure is needed.
// ============================================================================
`timescale 1ns/1ps
module order_book #(
    parameter LVL_BITS = 6,                  // 64 price levels
    parameter ORD_BITS = 10,                 // 1024-entry order table
    parameter TICK     = 100                 // $0.01 in ITCH price units
)(
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] target_locate,        // instrument to track
    input  wire [31:0] base_price,           // price of level 0

    input  wire        msg_valid,
    input  wire [7:0]  msg_type,
    input  wire [15:0] locate,
    input  wire [47:0] timestamp,
    input  wire [63:0] order_ref,
    input  wire [63:0] new_ref,
    input  wire        side_buy,
    input  wire [31:0] shares,
    input  wire [31:0] price,

    output reg         bbo_valid,
    output reg  [47:0] bbo_timestamp,
    output reg  [31:0] bid_price,            // 0 when no bids
    output reg  [31:0] bid_qty,
    output reg  [31:0] ask_price,            // 0 when no asks
    output reg  [31:0] ask_qty,
    output reg  [31:0] msgs_processed        // book messages applied (debug/stats)
);
    localparam LEVELS = 1 << LVL_BITS;
    localparam ENT_W  = 1 + 1 + LVL_BITS + 32;

    localparam S_IDLE = 3'd0, S_READ = 3'd1, S_EXEC = 3'd2, S_EXEC2 = 3'd3, S_BBO = 3'd4;

    // ---------------- storage
    reg [ENT_W-1:0] order_tbl [0:(1<<ORD_BITS)-1];
    reg [31:0]      bid_lvl   [0:LEVELS-1];
    reg [31:0]      ask_lvl   [0:LEVELS-1];

    // ---------------- latched message
    reg [2:0]          state;
    reg [7:0]          m_type;
    reg [47:0]         m_ts;
    reg [ORD_BITS-1:0] m_ref, m_ref2;
    reg                m_buy;
    reg [31:0]         m_shares;
    reg [LVL_BITS-1:0] m_idx;
    reg                m_px_ok;
    reg [ENT_W-1:0]    rd;                   // registered table read

    wire                rd_valid  = rd[ENT_W-1];
    wire                rd_bid    = rd[ENT_W-2];
    wire [LVL_BITS-1:0] rd_idx    = rd[32 +: LVL_BITS];
    wire [31:0]         rd_shares = rd[31:0];

    // price -> level index: (price - base) / TICK, using a constant
    // reciprocal multiply instead of a divider (exact for on-tick prices).
    localparam [31:0] RECIP = (32'd1 << 17) / TICK + 1;   // 1311 for TICK=100
    wire [31:0] px_diff  = price - base_price;
    wire [48:0] px_scale = px_diff * RECIP;
    wire        px_in    = (price >= base_price) && (px_diff < LEVELS * TICK);
    wire [LVL_BITS-1:0] px_idx = px_scale[17 +: LVL_BITS];

    integer i;

    // ---------------- top-of-book priority encoders (combinational)
    reg                has_bid, has_ask;
    reg [LVL_BITS-1:0] best_bid, best_ask;
    always @* begin
        has_bid = 1'b0; best_bid = {LVL_BITS{1'b0}};
        for (i = 0; i < LEVELS; i = i + 1)
            if (bid_lvl[i] != 32'd0) begin has_bid = 1'b1; best_bid = i[LVL_BITS-1:0]; end
        has_ask = 1'b0; best_ask = {LVL_BITS{1'b0}};
        for (i = LEVELS - 1; i >= 0; i = i - 1)
            if (ask_lvl[i] != 32'd0) begin has_ask = 1'b1; best_ask = i[LVL_BITS-1:0]; end
    end

    wire [31:0] nb_px  = has_bid ? base_price + best_bid * TICK : 32'd0;
    wire [31:0] nb_qty = has_bid ? bid_lvl[best_bid]            : 32'd0;
    wire [31:0] na_px  = has_ask ? base_price + best_ask * TICK : 32'd0;
    wire [31:0] na_qty = has_ask ? ask_lvl[best_ask]            : 32'd0;

    // executed / cancelled amount can never exceed what is resting
    wire [31:0] dec_amt = (m_shares > rd_shares) ? rd_shares : m_shares;

    always @(posedge clk) begin
        if (rst) begin
            state <= S_IDLE;
            bbo_valid <= 1'b0;
            bid_price <= 0; bid_qty <= 0; ask_price <= 0; ask_qty <= 0;
            msgs_processed <= 0;
            for (i = 0; i < LEVELS; i = i + 1) begin bid_lvl[i] <= 0; ask_lvl[i] <= 0; end
        end else begin
            bbo_valid <= 1'b0;
            case (state)
            S_IDLE: if (msg_valid && locate == target_locate &&
                        (msg_type == "A" || msg_type == "F" || msg_type == "E" || msg_type == "C" ||
                         msg_type == "X" || msg_type == "D" || msg_type == "U")) begin
                m_type   <= msg_type;
                m_ts     <= timestamp;
                m_ref    <= order_ref[ORD_BITS-1:0];
                m_ref2   <= new_ref[ORD_BITS-1:0];
                m_buy    <= side_buy;
                m_shares <= shares;
                m_idx    <= px_idx;
                m_px_ok  <= px_in;
                state    <= S_READ;
            end

            S_READ: begin
                rd    <= order_tbl[m_ref];
                state <= S_EXEC;
            end

            S_EXEC: begin
                state <= S_BBO;
                msgs_processed <= msgs_processed + 1;
                case (m_type)
                "A", "F": if (m_px_ok && m_shares != 0) begin
                    order_tbl[m_ref] <= {1'b1, m_buy, m_idx, m_shares};
                    if (m_buy) bid_lvl[m_idx] <= bid_lvl[m_idx] + m_shares;
                    else       ask_lvl[m_idx] <= ask_lvl[m_idx] + m_shares;
                end
                "E", "C", "X": if (rd_valid) begin
                    order_tbl[m_ref] <= {(rd_shares != dec_amt), rd_bid, rd_idx, rd_shares - dec_amt};
                    if (rd_bid) bid_lvl[rd_idx] <= bid_lvl[rd_idx] - dec_amt;
                    else        ask_lvl[rd_idx] <= ask_lvl[rd_idx] - dec_amt;
                end
                "D": if (rd_valid) begin
                    order_tbl[m_ref] <= {ENT_W{1'b0}};
                    if (rd_bid) bid_lvl[rd_idx] <= bid_lvl[rd_idx] - rd_shares;
                    else        ask_lvl[rd_idx] <= ask_lvl[rd_idx] - rd_shares;
                end
                "U": if (rd_valid) begin
                    // remove the original order, then add the replacement next cycle
                    order_tbl[m_ref] <= {ENT_W{1'b0}};
                    if (rd_bid) bid_lvl[rd_idx] <= bid_lvl[rd_idx] - rd_shares;
                    else        ask_lvl[rd_idx] <= ask_lvl[rd_idx] - rd_shares;
                    state <= S_EXEC2;
                end
                default: ;
                endcase
            end

            S_EXEC2: begin   // 'U' second half: new order keeps the original side
                if (m_px_ok && m_shares != 0) begin
                    order_tbl[m_ref2] <= {1'b1, rd_bid, m_idx, m_shares};
                    if (rd_bid) bid_lvl[m_idx] <= bid_lvl[m_idx] + m_shares;
                    else        ask_lvl[m_idx] <= ask_lvl[m_idx] + m_shares;
                end
                state <= S_BBO;
            end

            S_BBO: begin
                if (nb_px != bid_price || nb_qty != bid_qty || na_px != ask_price || na_qty != ask_qty) begin
                    bbo_valid     <= 1'b1;
                    bbo_timestamp <= m_ts;
                    bid_price <= nb_px; bid_qty <= nb_qty;
                    ask_price <= na_px; ask_qty <= na_qty;
                end
                state <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule
