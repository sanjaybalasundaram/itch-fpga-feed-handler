# ITCH 5.0 FPGA Feed Handler: Hardware Order Book in Verilog

A Verilog design that parses a **NASDAQ TotalView-ITCH 5.0** market-data feed byte by byte, maintains a **limit order book in hardware**, and outputs the **best bid and ask (top of book)** **4–5 clock cycles** after the last byte of each message arrives.

Low-latency trading firms use FPGAs for exactly this, because software network stacks and CPUs add microseconds that hardware avoids.

```
             +-----------------+   decoded fields    +--------------------------------+
 ITCH bytes  |  itch_parser    |  ----------------->  |  order_book                    |   BBO update
 ----------> |  length-framed  |   type, ref, side,   |  order table (BRAM, 1024)      | ------------>
 1 byte/clk  |  byte FSM       |   shares, price      |  64 price levels x 2 sides     |  bid px/qty
             +-----------------+                      |  priority encoders -> best bid |  ask px/qty
                                                      +--------------------------------+
```

**Includes a C++17 software version** ([`cpp/`](cpp/README.md)) that is checked against the same golden model and runs at about **39 ns/message (~25M messages/s)**. That makes for a direct comparison between FPGA and software latency.

## Results

| | |
|---|---|
| **Verification** | 6 random seeds × 20,000 messages (~120k messages, ~38k BBO updates), with and without random idle gaps: **0 mismatches** against the Python golden model |
| **Bug-detection check** | A deliberately planted bug (removing the over-fill cap on executions) is caught by the checker immediately |
| **Latency** | **4–5 clock cycles** from the last byte of a message to the BBO output (average 4.14) |
| **Throughput** | 1 byte per cycle with no back-pressure. Book updates take 3–4 cycles, while the shortest book message takes 21 cycles to arrive |
| **Resources** (Yosys, Xilinx 7-series) | ~8.8k LUTs, 4.7k FFs, 3 RAMB18, 4 DSP48: about 14% of the LUTs on an Artix-7 XC7A100T |

Timing has **not** been closed (no place-and-route was run), so no Fmax is claimed. The testbench uses a 4 ns clock for reporting only.

## Supported ITCH 5.0 messages

| Type | Message | Book action |
|---|---|---|
| `A` | Add Order | insert order, add shares to its price level |
| `F` | Add Order with MPID | same as `A` |
| `E` | Order Executed | reduce order and level (capped at resting shares) |
| `C` | Order Executed with Price | same as `E` |
| `X` | Order Cancel | partial cancel |
| `D` | Order Delete | remove order |
| `U` | Order Replace | remove the original order and add the new one (keeps side) |
| other | `S`, `R`, `P`, … | skipped using the length prefix |

Messages for other instruments are filtered by **stock locate** code.

## How it works

**`rtl/itch_parser.v`** is a 3-state FSM (length high byte, length low byte, body). It captures each field at its ITCH 5.0 byte offset by shifting bytes into big-endian registers, then emits a one-cycle `msg_valid` pulse with every field decoded.

**`rtl/order_book.v`**
- **Order table:** 1024 entries, direct-mapped by the low 10 bits of the order reference. Each entry is `{valid, is_bid, level, shares}`, and it maps onto block RAM (one registered read, one write).
- **Price levels:** 64 ticks of $0.01 from a configurable base price. Price → level uses a **constant reciprocal multiply** (`× 1311 >> 17`) instead of a hardware divider.
- **Top of book:** combinational priority encoders find the highest non-empty bid and the lowest non-empty ask. `bbo_valid` fires **only when price or size at the top actually changes**.
- A 5-state FSM (IDLE → READ → EXEC → EXEC2 for replace → BBO) applies each message.

**`model/itch_model.py`**
- Generates realistic ITCH 5.0 binary streams: a drifting mid-price, a book that never crosses, partial and over-filled executions, replaces, noise from other stocks, and message types that must be skipped. Each message's length is checked against the specification.
- Includes a **bit-accurate golden model** that mirrors the hardware rules, and checks it against an independent "ideal" dictionary-based book.

**`tb/tb_feed_handler.v`** streams the bytes into the design, logs every BBO update, and measures latency. **`model/compare.py`** checks the RTL output against the golden model line by line.

## Run it

Requires Icarus Verilog, Yosys and Python 3.

```bash
./run_sim.sh                 # 20,000 messages, seed 1, no gaps
./run_sim.sh 50000 7 1       # 50,000 messages, seed 7, random idle gaps
yosys synth.ys               # Xilinx 7-series resource estimate -> sim/synth_report.txt
```

## Design limitations (and how a production design would handle them)

- **One instrument, 64 price levels.** Production designs track thousands of symbols, with level arrays in BRAM and a sorted structure for the best price.
- **Direct-mapped order table.** Two live orders whose references share low bits would collide. Real designs use a hash table with collision handling or a CAM. The generator avoids collisions so the results stay exact.
- **Raw ITCH byte stream input.** A full system would add the Ethernet MAC, IP/UDP and MoldUDP64 session layers in front of the parser, including sequence-gap detection.
- **No place-and-route or timing closure yet.** The next step is Vivado implementation on an Artix-7 board.

## Project structure

```
cpp/   C++17 version: header-only parser + order book, tools, tests, benchmark (see cpp/README.md)
rtl/   itch_parser.v  order_book.v  feed_handler_top.v
tb/    tb_feed_handler.v
model/ itch_model.py (generator + golden model)  compare.py
sim/   generated stream, expected/actual output, latency and synthesis reports
run_sim.sh   synth.ys
```
