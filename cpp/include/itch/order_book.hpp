// order_book.hpp - price-level limit order book for one instrument.
//
// * Orders : open-addressing hash map, order ref -> {side, price, shares}
//            (pre-sized, so no rehash or allocation on the hot path).
// * Levels : one flat, sorted std::vector per side with the BEST price at the
//            back. Almost all activity happens near the top of book, so a short
//            linear scan from the back beats a tree, and there is no allocation
//            on the hot path (vectors are pre-reserved). An earlier std::map
//            version is kept in the README benchmark for comparison.
// * Every mutating call returns true when the top of book changed, so callers
//   publish only real BBO updates (same rule as the FPGA design).
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

#include "messages.hpp"
#include "parser.hpp"

namespace itch {

struct Bbo {
    uint32_t bid_px = 0;  uint64_t bid_qty = 0;   // 0 when side is empty
    uint32_t ask_px = 0;  uint64_t ask_qty = 0;
    bool operator==(const Bbo& o) const {
        return bid_px == o.bid_px && bid_qty == o.bid_qty && ask_px == o.ask_px && ask_qty == o.ask_qty;
    }
    bool operator!=(const Bbo& o) const { return !(*this == o); }
};

// Minimal open-addressing hash map with linear probing and tombstones.
// Order refs are unique 64-bit integers, so a multiplicative hash is enough.
class OrderMap {
public:
    struct Order { uint64_t ref; uint32_t price; uint32_t shares; Side side; uint8_t state; };
    enum : uint8_t { kEmpty = 0, kFull = 1, kTomb = 2 };

    explicit OrderMap(size_t capacity_pow2 = 1u << 16) : slots_(capacity_pow2), mask_(capacity_pow2 - 1) {}

    Order* find(uint64_t ref) {
        for (size_t i = hash(ref);; i = (i + 1) & mask_) {
            Order& s = slots_[i];
            if (s.state == kEmpty) return nullptr;
            if (s.state == kFull && s.ref == ref) return &s;
        }
    }
    void insert(uint64_t ref, Side side, uint32_t price, uint32_t shares) {
        size_t tomb = SIZE_MAX;
        for (size_t i = hash(ref);; i = (i + 1) & mask_) {
            Order& s = slots_[i];
            if (s.state == kFull && s.ref == ref) { s = {ref, price, shares, side, kFull}; return; }
            if (s.state == kTomb && tomb == SIZE_MAX) tomb = i;
            if (s.state == kEmpty) {
                if (tomb != SIZE_MAX) { slots_[tomb] = {ref, price, shares, side, kFull}; --tombs_; }
                else                  { slots_[i]    = {ref, price, shares, side, kFull}; }
                ++size_;
                if ((size_ + tombs_) * 4 > slots_.size() * 3) rebuild();   // keep probes short
                return;
            }
        }
    }
    void erase(Order* o) { o->state = kTomb; --size_; ++tombs_; }
    size_t size() const { return size_; }

private:
    size_t hash(uint64_t k) const { return size_t((k * 0x9E3779B97F4A7C15ull) >> 20) & mask_; }

    // Drop tombstones (and grow if genuinely full). Rare: amortised O(1).
    void rebuild() {
        std::vector<Order> old;
        old.swap(slots_);
        const size_t cap = (size_ * 2 > old.size()) ? old.size() * 2 : old.size();
        slots_.assign(cap, Order{});
        mask_ = cap - 1;
        size_ = tombs_ = 0;
        for (const Order& o : old)
            if (o.state == kFull) insert(o.ref, o.side, o.price, o.shares);
    }

    std::vector<Order> slots_;
    size_t mask_;
    size_t size_ = 0;
    size_t tombs_ = 0;
};

class OrderBook {
public:
    explicit OrderBook(size_t order_capacity_pow2 = 1u << 16) : orders_(order_capacity_pow2) {
        bids_.reserve(4096);
        asks_.reserve(4096);
    }

    bool add(uint64_t ref, Side side, uint32_t price, uint32_t shares) {
        if (shares == 0) return false;
        orders_.insert(ref, side, price, shares);
        level_add(side, price, shares);
        return refresh();
    }

    // Execution or cancel: reduce by up to `shares` (never below zero).
    bool reduce(uint64_t ref, uint32_t shares) {
        auto* o = orders_.find(ref);
        if (!o) return false;
        const uint32_t dec = shares < o->shares ? shares : o->shares;
        level_sub(o->side, o->price, dec);
        o->shares -= dec;
        if (o->shares == 0) orders_.erase(o);
        return refresh();
    }

    bool remove(uint64_t ref) {
        auto* o = orders_.find(ref);
        if (!o) return false;
        level_sub(o->side, o->price, o->shares);
        orders_.erase(o);
        return refresh();
    }

    // Replace keeps the original side; the new order loses time priority.
    bool replace(uint64_t ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
        auto* o = orders_.find(ref);
        if (!o) return false;
        const Side side = o->side;
        level_sub(side, o->price, o->shares);
        orders_.erase(o);
        if (shares) {
            orders_.insert(new_ref, side, price, shares);
            level_add(side, price, shares);
        }
        return refresh();
    }

    const Bbo& bbo() const { return bbo_; }
    size_t order_count() const { return orders_.size(); }
    size_t bid_levels() const { return bids_.size(); }
    size_t ask_levels() const { return asks_.size(); }

private:
    struct Level { uint32_t px; uint64_t qty; };
    // bids: ascending (best = highest = back); asks: descending (best = lowest = back)
    static bool better(Side s, uint32_t a, uint32_t b) { return s == Side::Buy ? a > b : a < b; }

    std::vector<Level>& side_of(Side s) { return s == Side::Buy ? bids_ : asks_; }

    void level_add(Side s, uint32_t px, uint64_t q) {
        auto& v = side_of(s);
        size_t i = v.size();
        while (i > 0 && better(s, v[i - 1].px, px)) --i;      // walk down from the top
        if (i > 0 && v[i - 1].px == px) { v[i - 1].qty += q; return; }
        v.insert(v.begin() + long(i), Level{px, q});
    }
    void level_sub(Side s, uint32_t px, uint64_t q) {
        auto& v = side_of(s);
        size_t i = v.size();
        while (v[i - 1].px != px) --i;                        // level must exist
        if ((v[i - 1].qty -= q) == 0) v.erase(v.begin() + long(i - 1));
    }
    bool refresh() {
        Bbo n;
        if (!bids_.empty()) { n.bid_px = bids_.back().px; n.bid_qty = bids_.back().qty; }
        if (!asks_.empty()) { n.ask_px = asks_.back().px; n.ask_qty = asks_.back().qty; }
        if (n == bbo_) return false;
        bbo_ = n;
        return true;
    }

    OrderMap orders_;
    std::vector<Level> bids_, asks_;
    Bbo bbo_;
};

// Glue: parser callbacks -> book for one stock locate; calls on_bbo(ts, bbo) on change.
template <class OnBbo>
class FeedHandler : public NullHandler {
public:
    FeedHandler(uint16_t locate, OnBbo cb, size_t cap = 1u << 16) : locate_(locate), cb_(cb), book_(cap) {}

    void on_add(const AddOrder& m)        { if (mine(m.h) && book_.add(m.ref, m.side, m.price, m.shares)) emit(m.h); }
    void on_executed(const OrderExecuted& m) { if (mine(m.h) && book_.reduce(m.ref, m.shares)) emit(m.h); }
    void on_cancel(const OrderCancel& m)  { if (mine(m.h) && book_.reduce(m.ref, m.shares)) emit(m.h); }
    void on_delete(const OrderDelete& m)  { if (mine(m.h) && book_.remove(m.ref)) emit(m.h); }
    void on_replace(const OrderReplace& m){ if (mine(m.h) && book_.replace(m.ref, m.new_ref, m.shares, m.price)) emit(m.h); }

    const OrderBook& book() const { return book_; }

private:
    bool mine(const Header& h) const { return h.locate == locate_; }
    void emit(const Header& h) { cb_(h.timestamp, book_.bbo()); }
    uint16_t  locate_;
    OnBbo     cb_;
    OrderBook book_;
};

}  // namespace itch
