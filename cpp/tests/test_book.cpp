// Unit tests for the C++ ITCH parser and order book (no external framework).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/order_book.hpp"
#include "itch/parser.hpp"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------- tiny ITCH encoder
struct Enc {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void be(uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
    void hdr(char t, uint16_t loc, uint64_t ts) { u8(uint8_t(t)); be(loc, 2); be(0, 2); be(ts, 6); }
    std::vector<uint8_t> frame() const {
        std::vector<uint8_t> f{uint8_t(b.size() >> 8), uint8_t(b.size())};
        f.insert(f.end(), b.begin(), b.end());
        return f;
    }
};
static std::vector<uint8_t> A(uint16_t loc, uint64_t ref, char side, uint32_t sh, uint32_t px) {
    Enc e; e.hdr('A', loc, 1); e.be(ref, 8); e.u8(uint8_t(side)); e.be(sh, 4); for (int i = 0; i < 8; ++i) e.u8(' '); e.be(px, 4); return e.frame();
}
static std::vector<uint8_t> E(uint16_t loc, uint64_t ref, uint32_t sh) {
    Enc e; e.hdr('E', loc, 2); e.be(ref, 8); e.be(sh, 4); e.be(7, 8); return e.frame();
}
static std::vector<uint8_t> X(uint16_t loc, uint64_t ref, uint32_t sh) {
    Enc e; e.hdr('X', loc, 3); e.be(ref, 8); e.be(sh, 4); return e.frame();
}
static std::vector<uint8_t> D(uint16_t loc, uint64_t ref) {
    Enc e; e.hdr('D', loc, 4); e.be(ref, 8); return e.frame();
}
static std::vector<uint8_t> U(uint16_t loc, uint64_t ref, uint64_t nref, uint32_t sh, uint32_t px) {
    Enc e; e.hdr('U', loc, 5); e.be(ref, 8); e.be(nref, 8); e.be(sh, 4); e.be(px, 4); return e.frame();
}
static std::vector<uint8_t> S() { Enc e; e.hdr('S', 0, 0); e.u8('O'); return e.frame(); }

struct Rig {
    std::vector<itch::Bbo> updates;
    std::function<void(uint64_t, const itch::Bbo&)> cb = [this](uint64_t, const itch::Bbo& b) { updates.push_back(b); };
    itch::FeedHandler<std::function<void(uint64_t, const itch::Bbo&)>> h{42, cb, 1u << 10};
    itch::Parser<decltype(h)> p{h};
    void feed(const std::vector<uint8_t>& m) { CHECK(p.parse(m.data(), m.size()) == m.size()); }
    const itch::Bbo& bbo() const { return h.book().bbo(); }
};

static void test_basic_book() {
    Rig r;
    r.feed(A(42, 1, 'B', 100, 1500000));
    r.feed(A(42, 2, 'B', 200, 1500100));               // better bid
    r.feed(A(42, 3, 'S', 300, 1500300));
    r.feed(A(42, 4, 'S', 50, 1500300));                // same level, adds size
    CHECK(r.bbo().bid_px == 1500100 && r.bbo().bid_qty == 200);
    CHECK(r.bbo().ask_px == 1500300 && r.bbo().ask_qty == 350);
    CHECK(r.updates.size() == 4);

    r.feed(A(42, 5, 'B', 10, 1499000));                // deep bid: top unchanged -> no update
    CHECK(r.updates.size() == 4);

    r.feed(E(42, 2, 150));                             // partial fill
    CHECK(r.bbo().bid_qty == 50);
    r.feed(E(42, 2, 999));                             // over-fill is capped, order gone
    CHECK(r.bbo().bid_px == 1500000 && r.bbo().bid_qty == 100);
    r.feed(X(42, 3, 100));                             // partial cancel
    CHECK(r.bbo().ask_qty == 250);
    r.feed(D(42, 4));
    CHECK(r.bbo().ask_qty == 200);
    r.feed(U(42, 3, 30, 400, 1500200));                // replace: keeps sell side, new price
    CHECK(r.bbo().ask_px == 1500200 && r.bbo().ask_qty == 400);
    r.feed(D(42, 30)); r.feed(D(42, 1)); r.feed(D(42, 5));
    CHECK(r.bbo() == itch::Bbo{});                     // empty book
    CHECK(r.h.book().order_count() == 0 && r.h.book().bid_levels() == 0 && r.h.book().ask_levels() == 0);
}

static void test_filtering_and_skips() {
    Rig r;
    r.feed(S());                                       // system event: skipped
    r.feed(A(7, 1, 'B', 100, 1500000));                // other stock: ignored
    r.feed(D(42, 999));                                // unknown order: ignored
    CHECK(r.updates.empty());
    CHECK(r.p.stats().skipped == 1 && r.p.stats().decoded == 2);

    std::vector<uint8_t> bad = {0, 5, 'A', 0, 42, 0, 0};    // 'A' shorter than spec
    r.feed(bad);
    CHECK(r.p.stats().malformed == 1);
}

static void test_streaming_chunks_match_one_shot() {
    std::vector<uint8_t> s;
    std::mt19937 rng(3);
    for (uint64_t i = 1; i <= 500; ++i) {
        auto m = A(42, i, (i & 1) ? 'B' : 'S', 100 + uint32_t(i), 1500000 + uint32_t(rng() % 20) * 100);
        s.insert(s.end(), m.begin(), m.end());
        if (i % 3 == 0) { auto d = E(42, i - 1, 50); s.insert(s.end(), d.begin(), d.end()); }
    }
    Rig one; one.feed(s);

    Rig chunked;
    std::vector<uint8_t> pending;
    size_t off = 0;
    while (off < s.size()) {                           // deliver in random-sized network chunks
        size_t n = std::min(s.size() - off, size_t(1 + rng() % 97));
        pending.insert(pending.end(), s.begin() + long(off), s.begin() + long(off + n));
        off += n;
        size_t used = chunked.p.parse(pending.data(), pending.size());
        pending.erase(pending.begin(), pending.begin() + long(used));
    }
    CHECK(pending.empty());
    CHECK(one.updates.size() == chunked.updates.size());
    CHECK(one.bbo() == chunked.bbo());
}

static void test_order_map_against_std() {
    itch::OrderMap m(1u << 8);                         // tiny on purpose: forces many rebuilds
    std::unordered_map<uint64_t, uint32_t> ref;
    std::mt19937_64 rng(11);
    for (int i = 0; i < 300000; ++i) {
        uint64_t k = rng() % 5000;
        if (rng() % 3) { uint32_t v = uint32_t(rng()); m.insert(k, itch::Side::Buy, 0, v); ref[k] = v; }
        else if (auto* o = m.find(k)) { m.erase(o); ref.erase(k); }
    }
    bool ok = m.size() == ref.size();
    for (auto& [k, v] : ref) { auto* o = m.find(k); ok = ok && o && o->shares == v; }
    CHECK(ok);
}

int main() {
    test_basic_book();
    test_filtering_and_skips();
    test_streaming_chunks_match_one_shot();
    test_order_map_against_std();
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
