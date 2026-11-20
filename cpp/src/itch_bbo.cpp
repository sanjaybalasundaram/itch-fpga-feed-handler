// itch_bbo - replay an ITCH 5.0 stream and print every top-of-book change.
// Output format matches sim/expected.txt:  <timestamp> <bid_px> <bid_qty> <ask_px> <ask_qty>
//
// Usage: itch_bbo <stream.bin|stream.hex> <stock_locate> [out.txt]
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "io.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <stream.bin|stream.hex> <stock_locate> [out.txt]\n";
        return 2;
    }
    const auto data = load_stream(argv[1]);
    const auto locate = uint16_t(std::strtoul(argv[2], nullptr, 10));
    FILE* out = argc > 3 ? std::fopen(argv[3], "w") : stdout;
    if (!out) { std::perror("open output"); return 1; }

    uint64_t updates = 0;
    auto on_bbo = [&](uint64_t ts, const itch::Bbo& b) {
        std::fprintf(out, "%llu %u %llu %u %llu\n", (unsigned long long)ts, b.bid_px,
                     (unsigned long long)b.bid_qty, b.ask_px, (unsigned long long)b.ask_qty);
        ++updates;
    };
    itch::FeedHandler<decltype(on_bbo)> handler(locate, on_bbo);
    itch::Parser<decltype(handler)> parser(handler);

    const size_t used = parser.parse(data.data(), data.size());
    if (out != stdout) std::fclose(out);

    const auto& st = parser.stats();
    std::cerr << "bytes " << used << "/" << data.size() << ", messages " << st.messages
              << ", decoded " << st.decoded << ", skipped " << st.skipped << ", malformed " << st.malformed
              << ", bbo updates " << updates << ", resting orders " << handler.book().order_count() << "\n";
    return used == data.size() ? 0 : 1;
}
