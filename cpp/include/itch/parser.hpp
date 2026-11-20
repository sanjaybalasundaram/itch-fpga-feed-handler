// parser.hpp - zero-copy ITCH 5.0 parser for length-prefixed streams
// (2-byte big-endian length, then the message - the NASDAQ binary file format,
// and the payload framing inside MoldUDP64 packets).
//
// The handler is a template parameter, so every callback is resolved at compile
// time and inlined: no virtual calls, no allocations, no copies of the payload.
#pragma once
#include <cstddef>
#include <cstdint>

#include "messages.hpp"

namespace itch {

struct ParseStats {
    uint64_t messages = 0;     // complete messages seen
    uint64_t decoded  = 0;     // book messages decoded (A F E C X D U)
    uint64_t skipped  = 0;     // other message types
    uint64_t malformed = 0;    // book message shorter than the spec length
};

// Default no-op handler: inherit and override only what you need.
struct NullHandler {
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
};

template <class Handler>
class Parser {
public:
    explicit Parser(Handler& h) : h_(h) {}

    // Decode one framed message at p (avail bytes available).
    // Returns bytes consumed, or 0 if the message is not complete yet.
    size_t parse_one(const uint8_t* p, size_t avail) {
        if (avail < 2) return 0;
        const uint16_t len = be16(p);
        if (avail < size_t(len) + 2) return 0;
        if (len) dispatch(p + 2, len);
        return size_t(len) + 2;
    }

    // Decode every complete message in [p, p+n). Returns bytes consumed; any
    // trailing partial message should be kept by the caller and retried.
    size_t parse(const uint8_t* p, size_t n) {
        size_t off = 0;
        while (size_t used = parse_one(p + off, n - off)) off += used;
        return off;
    }

    const ParseStats& stats() const { return stats_; }

private:
    void dispatch(const uint8_t* m, uint16_t len) {
        ++stats_.messages;
        switch (m[0]) {
        case 'A': case 'F':
            if (len < kLenAdd) break;
            h_.on_add({decode_header(m), be64(m + 11), Side(m[19]), be32(m + 20), be32(m + 32)});
            ++stats_.decoded; return;
        case 'E': case 'C':
            if (len < kLenExec) break;
            h_.on_executed({decode_header(m), be64(m + 11), be32(m + 19)});
            ++stats_.decoded; return;
        case 'X':
            if (len < kLenCancel) break;
            h_.on_cancel({decode_header(m), be64(m + 11), be32(m + 19)});
            ++stats_.decoded; return;
        case 'D':
            if (len < kLenDelete) break;
            h_.on_delete({decode_header(m), be64(m + 11)});
            ++stats_.decoded; return;
        case 'U':
            if (len < kLenReplace) break;
            h_.on_replace({decode_header(m), be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31)});
            ++stats_.decoded; return;
        default:
            ++stats_.skipped; return;
        }
        ++stats_.malformed;
    }

    Handler&   h_;
    ParseStats stats_;
};

}  // namespace itch
