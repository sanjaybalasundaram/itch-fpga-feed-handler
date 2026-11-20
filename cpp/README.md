# C++17 ITCH 5.0 Feed Handler

A software version of the same feed handler: a zero-copy NASDAQ ITCH 5.0 parser and a limit order book that publishes top-of-book (BBO) changes. It's checked against the **same golden model** as the Verilog design, so the C++ and FPGA outputs are proven identical.

## Results

**Correctness**
- 6 seeds × 20,000 messages, plus a 500,000-message stream: output **byte-for-byte identical** to the golden model, and therefore to the RTL.
- 36 unit checks. These cover partial and over-filled executions, replace, stock filtering, skipped and malformed messages, streaming in random network-sized chunks, and the hash map tested against `std::unordered_map`.

**Performance** (500,000 messages, 16.5 MB; Intel Xeon 2.1 GHz shared cloud VM; g++ 13 `-O3 -march=native`)

| Version | Throughput | p50 | p90 | p99 |
|---|---|---|---|---|
| v1: `std::map` levels, 24 MB order hash table | ~48 ns/msg | ~48 ns | ~204 ns | ~390 ns |
| **v2: flat sorted-vector levels, cache-sized hash table** | **~39 ns/msg (≈25M msg/s)** | **~43 ns** | **~105 ns** | **~250 ns** |

These are per-message parse + book update + BBO check times, with timer overhead (~23 ns) subtracted. Full output is in `sim/cpp_bench.txt`.

**What made it faster:** profiling by experiment showed the tree wasn't the main cost. Moving price levels from `std::map` to a flat vector only gained about 5%. The real problem was the 1M-slot (24 MB) order hash table, where almost every lookup was a cache miss. Starting it at 64K slots (1.5 MB, fits in cache) and growing on demand cut throughput time by about 20% and roughly halved p90 latency.

## Design

- **`include/itch/parser.hpp`:** a templated `Parser<Handler>`. Callbacks are resolved at compile time (no virtual calls), fields are read straight from the buffer with `memcpy` + `bswap` (no copies or allocations), and `parse()` returns bytes consumed, so partial messages across network packets are handled correctly.
- **`include/itch/order_book.hpp`**
  - `OrderMap`: an open-addressing hash map (linear probing, tombstones, amortised rebuild).
  - Price levels: a sorted `std::vector` per side with the best price at the back. Most updates happen near the top of book, so a short scan from the back is faster than a tree, and nothing is allocated on the hot path.
  - `FeedHandler`: filters by stock locate and calls your callback only when the BBO actually changes.

## Build and run

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build -j
./cpp/build/test_book                                   # unit tests
./cpp/run_check.sh                                      # compare with golden model, 6 seeds
./cpp/build/itch_bbo sim/stream.hex 42 bbo.txt          # replay a stream (.hex or raw binary)
./cpp/build/itch_bench sim/stream.hex 42 20             # benchmark
```

`itch_bbo` also reads raw binary ITCH files in the standard length-prefixed format, such as NASDAQ's public historical ITCH 5.0 sample files.

## C++ vs FPGA

| | C++ (this folder) | FPGA (Verilog, `rtl/`) |
|---|---|---|
| Latency per message | ~43 ns median, ~250 ns p99, varies with cache and OS | **4–5 clock cycles, fixed** (deterministic) |
| Instruments / levels | unlimited | 1 instrument, 64 levels (prototype) |
| Order capacity | grows automatically | 1024-entry direct-mapped table |
| Jitter | occasional large spikes from OS scheduling | none |

This is why trading firms put the most latency-critical path in FPGAs, and use C++ for logic that needs flexibility.
