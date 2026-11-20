// itch_bench - throughput and per-message latency of parse + book update.
//
// Usage: itch_bench <stream.bin|stream.hex> <stock_locate> [iterations]
//
// 1) Throughput: replays the whole in-memory stream `iterations` times
//    (fresh book each time) and reports ns/message and messages/second.
// 2) Latency: times every message individually (parse + book update + BBO
//    check) and reports percentiles. Timer overhead is measured and subtracted.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "io.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"

using Clock = std::chrono::steady_clock;

static inline uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <stream> <locate> [iterations]\n", argv[0]); return 2; }
    const auto data = load_stream(argv[1]);
    const auto locate = uint16_t(std::strtoul(argv[2], nullptr, 10));
    const int iters = argc > 3 ? std::atoi(argv[3]) : 50;

    volatile uint64_t sink = 0;                  // keeps the compiler from removing work
    auto on_bbo = [&](uint64_t, const itch::Bbo& b) { sink = sink + b.bid_px + b.ask_qty; };

    // ---- throughput
    uint64_t msgs = 0, best_ns = UINT64_MAX;
    for (int it = 0; it < iters; ++it) {
        itch::FeedHandler<decltype(on_bbo)> h(locate, on_bbo);
        itch::Parser<decltype(h)> p(h);
        const uint64_t t0 = now_ns();
        p.parse(data.data(), data.size());
        const uint64_t dt = now_ns() - t0;
        best_ns = std::min(best_ns, dt);
        msgs = p.stats().messages;
    }
    const double ns_per_msg = double(best_ns) / double(msgs);

    // ---- per-message latency
    uint64_t overhead = UINT64_MAX;
    for (int i = 0; i < 10000; ++i) { const uint64_t a = now_ns(); overhead = std::min(overhead, now_ns() - a); }

    std::vector<uint32_t> lat;
    lat.reserve(msgs * 5);
    for (int rep = 0; rep < 5; ++rep) {
        itch::FeedHandler<decltype(on_bbo)> h(locate, on_bbo);
        itch::Parser<decltype(h)> p(h);
        size_t off = 0;
        while (off < data.size()) {
            const uint64_t t0 = now_ns();
            const size_t used = p.parse_one(data.data() + off, data.size() - off);
            const uint64_t dt = now_ns() - t0;
            if (!used) break;
            off += used;
            lat.push_back(uint32_t(dt > overhead ? dt - overhead : 0));
        }
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double q) { return lat[std::min(lat.size() - 1, size_t(q * double(lat.size())))]; };

    std::printf("stream: %zu bytes, %llu messages\n", data.size(), (unsigned long long)msgs);
    std::printf("throughput: %.1f ns/message  (%.1f M messages/s, %.2f Gbit/s of ITCH)\n",
                ns_per_msg, 1e3 / ns_per_msg, double(data.size()) * 8 / double(best_ns));
    std::printf("latency per message (timer overhead %llu ns removed): p50 %u ns  p90 %u ns  p99 %u ns  p99.9 %u ns  max %u ns\n",
                (unsigned long long)overhead, pct(0.50), pct(0.90), pct(0.99), pct(0.999), lat.back());
    return 0;
}
