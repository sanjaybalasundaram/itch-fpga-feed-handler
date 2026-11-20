#!/usr/bin/env python3
"""ITCH 5.0 stream generator + bit-accurate golden model of the feed handler.

Writes:
  sim/stream.hex     one byte per line, length-prefixed ITCH messages (for $readmemh)
  sim/expected.txt   expected BBO updates: "<ts> <bid_px> <bid_qty> <ask_px> <ask_qty>"
  sim/config.txt     target locate, base price, byte count (read by the testbench)
  sim/stats.txt      message mix

The model mirrors the hardware rules exactly (direct-mapped order table,
64 price levels, executions capped at resting size, BBO emitted only on change).
It also keeps an independent "ideal" dict-based book and asserts both agree,
so the generator never produces a stream the simplified hardware would get wrong.
"""
from __future__ import annotations

import argparse
import os
import random
import struct
from collections import Counter

TARGET_LOCATE = 42
OTHER_LOCATES = [7, 13, 99]
BASE_PRICE = 1_500_000          # $150.0000 in ITCH units (4 implied decimals)
TICK = 100                      # $0.01
LEVELS = 64
ORD_BITS = 10
MAX_LIVE = 300                  # resting orders kept by the generator


# ---------------------------------------------------------------- encoders (ITCH 5.0 layouts)
def _hdr(t: str, locate: int, ts: int) -> bytes:
    return t.encode() + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")

def add_order(locate, ts, ref, side, shares, price, mpid=False):
    body = _hdr("F" if mpid else "A", locate, ts) + struct.pack(">Qc I", ref, side.encode(), shares) \
        + b"ACME    " + struct.pack(">I", price)
    if mpid:
        body += b"MPID"
    return body

def order_executed(locate, ts, ref, shares, match):
    return _hdr("E", locate, ts) + struct.pack(">QIQ", ref, shares, match)

def order_executed_px(locate, ts, ref, shares, match, price):
    return _hdr("C", locate, ts) + struct.pack(">QIQcI", ref, shares, match, b"Y", price)

def order_cancel(locate, ts, ref, shares):
    return _hdr("X", locate, ts) + struct.pack(">QI", ref, shares)

def order_delete(locate, ts, ref):
    return _hdr("D", locate, ts) + struct.pack(">Q", ref)

def order_replace(locate, ts, ref, new_ref, shares, price):
    return _hdr("U", locate, ts) + struct.pack(">QQII", ref, new_ref, shares, price)

def system_event(ts):          # 'S', 12 bytes - must be skipped
    return _hdr("S", 0, ts) + b"O"

def stock_directory(locate, ts):  # 'R', 39 bytes - must be skipped
    return _hdr("R", locate, ts) + b"ACME    " + b"Q" + b"N" + struct.pack(">I", 100) + b"N" + b"C" + b"Z " \
        + b"P" + b"N" + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N"

def trade(locate, ts, ref):    # 'P', 44 bytes - must be skipped
    return _hdr("P", locate, ts) + struct.pack(">QcI", ref, b"B", 100) + b"ACME    " \
        + struct.pack(">IQ", BASE_PRICE, 1)

EXPECTED_LEN = {"A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19, "U": 35, "S": 12, "R": 39, "P": 44}


# ---------------------------------------------------------------- golden model (hardware semantics)
class HwBook:
    def __init__(self):
        self.tbl = [None] * (1 << ORD_BITS)        # (is_bid, idx, shares)
        self.bid = [0] * LEVELS
        self.ask = [0] * LEVELS
        self.last = (0, 0, 0, 0)
        self.out: list[tuple] = []

    @staticmethod
    def idx_of(price):
        diff = price - BASE_PRICE
        ok = price >= BASE_PRICE and diff < LEVELS * TICK
        return (diff // TICK if ok else 0), ok

    def _lvl(self, is_bid):
        return self.bid if is_bid else self.ask

    def apply(self, msg: bytes):
        t = chr(msg[0])
        locate = struct.unpack(">H", msg[1:3])[0]
        ts = int.from_bytes(msg[5:11], "big")
        if locate != TARGET_LOCATE or t not in "AFECXDU":
            return
        ref = struct.unpack(">Q", msg[11:19])[0]
        slot = ref & ((1 << ORD_BITS) - 1)
        e = self.tbl[slot]
        if t in "AF":
            side, shares, price = chr(msg[19]), *struct.unpack(">I", msg[20:24]), struct.unpack(">I", msg[32:36])[0]
            idx, ok = self.idx_of(price)
            if ok and shares:
                self.tbl[slot] = (side == "B", idx, shares)
                self._lvl(side == "B")[idx] += shares
        elif t in "ECX":
            shares = struct.unpack(">I", msg[19:23])[0]
            if e:
                dec = min(shares, e[2])
                self._lvl(e[0])[e[1]] -= dec
                self.tbl[slot] = (e[0], e[1], e[2] - dec) if e[2] - dec else None
        elif t == "D":
            if e:
                self._lvl(e[0])[e[1]] -= e[2]
                self.tbl[slot] = None
        elif t == "U":
            new_ref, shares, price = struct.unpack(">QII", msg[19:35])
            if e:
                self._lvl(e[0])[e[1]] -= e[2]
                self.tbl[slot] = None
                idx, ok = self.idx_of(price)
                if ok and shares:
                    self.tbl[new_ref & ((1 << ORD_BITS) - 1)] = (e[0], idx, shares)
                    self._lvl(e[0])[idx] += shares
        self._emit(ts)

    def _emit(self, ts):
        bb = max((i for i in range(LEVELS) if self.bid[i]), default=None)
        ba = min((i for i in range(LEVELS) if self.ask[i]), default=None)
        cur = (BASE_PRICE + bb * TICK if bb is not None else 0, self.bid[bb] if bb is not None else 0,
               BASE_PRICE + ba * TICK if ba is not None else 0, self.ask[ba] if ba is not None else 0)
        if cur != self.last:
            self.last = cur
            self.out.append((ts, *cur))


# ---------------------------------------------------------------- stream generator
def generate(n_msgs: int, seed: int):
    rng = random.Random(seed)
    ts = 34_200_000_000_000                       # 09:30:00.000 in ns
    ref_ctr = 1000
    match = 1
    live: dict[int, list] = {}                    # ref -> [side, price, shares] (ideal book, target stock)
    msgs: list[bytes] = [system_event(ts), stock_directory(TARGET_LOCATE, ts)]
    mix = Counter()
    mid = 32                                      # mid level, drifts slowly

    def new_ref():
        nonlocal ref_ctr
        ref_ctr += 1
        return ref_ctr

    def price_for(side, book=None):
        # bids below / asks above a drifting mid, kept inside the 64-level window.
        # For the tracked stock the book is never allowed to cross (bid < ask),
        # as on a real exchange where crossing orders would trade instead.
        off = int(abs(rng.gauss(0, 3))) + 1
        lvl = mid - off if side == "B" else mid + off - 1
        lvl = max(0, min(LEVELS - 1, lvl))
        if book:
            bids = [(p - BASE_PRICE) // TICK for sd, p, q in book.values() if sd == "B"]
            asks = [(p - BASE_PRICE) // TICK for sd, p, q in book.values() if sd == "S"]
            if side == "B" and asks:
                lvl = min(lvl, min(asks) - 1)
            if side == "S" and bids:
                lvl = max(lvl, max(bids) + 1)
            lvl = max(0, min(LEVELS - 1, lvl))
        return BASE_PRICE + lvl * TICK

    while len(msgs) < n_msgs:
        ts += rng.randint(200, 50_000)
        if rng.random() < 0.02:
            mid = max(10, min(LEVELS - 10, mid + rng.choice([-1, 1])))
        r = rng.random()
        if r < 0.15:                              # noise: other instruments / skipped types
            loc = rng.choice(OTHER_LOCATES)
            kind = rng.random()
            if kind < 0.5:
                msgs.append(add_order(loc, ts, new_ref(), rng.choice("BS"), rng.randint(1, 900), price_for("B")))
            elif kind < 0.8:
                msgs.append(trade(loc, ts, rng.randint(1, 10**6)))
            else:
                msgs.append(order_delete(loc, ts, rng.randint(1, 10**6)))
            mix["noise"] += 1
            continue

        # never let two live orders share a table slot (keeps hw == ideal)
        def slot_free(ref):
            return (ref & ((1 << ORD_BITS) - 1)) not in {k & ((1 << ORD_BITS) - 1) for k in live}

        # keep the resting book realistic (a few hundred orders) so the table never fills
        if not live or (r < 0.50 and len(live) < MAX_LIVE):
            ref = new_ref()
            while not slot_free(ref):
                ref = new_ref()
            side = rng.choice("BS")
            shares = rng.choice([100, 100, 200, 300, 500, 1000]) * rng.randint(1, 3)
            price = price_for(side, live)
            msgs.append(add_order(TARGET_LOCATE, ts, ref, side, shares, price, mpid=rng.random() < 0.2))
            live[ref] = [side, price, shares]
            mix["add"] += 1
            continue

        ref = rng.choice(list(live))
        side, price, shares = live[ref]
        if r < 0.65:                              # execution (partial or full, sometimes over-fill)
            ex = rng.randint(1, shares + (50 if rng.random() < 0.1 else 0))
            msgs.append(order_executed(TARGET_LOCATE, ts, ref, ex, match) if rng.random() < 0.7
                        else order_executed_px(TARGET_LOCATE, ts, ref, ex, match, price))
            match += 1
            live[ref][2] -= min(ex, shares)
            mix["exec"] += 1
        elif r < 0.77:
            c = rng.randint(1, shares)
            msgs.append(order_cancel(TARGET_LOCATE, ts, ref, c))
            live[ref][2] -= c
            mix["cancel"] += 1
        elif r < 0.90:
            msgs.append(order_delete(TARGET_LOCATE, ts, ref))
            live[ref][2] = 0
            mix["delete"] += 1
        else:
            nr = new_ref()
            while not slot_free(nr):
                nr = new_ref()
            ns = rng.choice([100, 200, 300, 500])
            np_ = price_for(side, {k: v for k, v in live.items() if k != ref})
            msgs.append(order_replace(TARGET_LOCATE, ts, ref, nr, ns, np_))
            del live[ref]
            live[nr] = [side, np_, ns]
            mix["replace"] += 1
            continue
        if live[ref][2] == 0:
            del live[ref]
    return msgs, mix, live


def ideal_bbo(live):
    bids = Counter(); asks = Counter()
    for side, price, shares in live.values():
        (bids if side == "B" else asks)[price] += shares
    bb = max(bids) if bids else 0
    ba = min(asks) if asks else 0
    return (bb, bids[bb] if bb else 0, ba, asks[ba] if ba else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--msgs", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "sim"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    msgs, mix, live = generate(a.msgs, a.seed)
    book = HwBook()
    stream = bytearray()
    for m in msgs:
        assert len(m) == EXPECTED_LEN[chr(m[0])], (chr(m[0]), len(m))
        stream += struct.pack(">H", len(m)) + m
        book.apply(m)

    crossed = [u for u in book.out if u[1] and u[3] and u[1] >= u[3]]
    assert not crossed, f"generator produced a crossed book: {crossed[0]}"

    # hardware-semantics model must agree with an ideal unbounded book
    assert book.last == ideal_bbo(live), (book.last, ideal_bbo(live))

    with open(os.path.join(a.out, "stream.hex"), "w") as f:
        f.write("\n".join(f"{b:02x}" for b in stream) + "\n")
    with open(os.path.join(a.out, "expected.txt"), "w") as f:
        for ts, bp, bq, ap_, aq in book.out:
            f.write(f"{ts} {bp} {bq} {ap_} {aq}\n")
    with open(os.path.join(a.out, "config.txt"), "w") as f:
        f.write(f"{TARGET_LOCATE} {BASE_PRICE} {len(stream)}\n")
    with open(os.path.join(a.out, "stats.txt"), "w") as f:
        f.write(f"messages {len(msgs)}\nbytes {len(stream)}\nbbo_updates {len(book.out)}\n")
        for k, v in sorted(mix.items()):
            f.write(f"{k} {v}\n")
    print(f"{len(msgs)} messages, {len(stream)} bytes, {len(book.out)} expected BBO updates, mix={dict(mix)}")


if __name__ == "__main__":
    main()
