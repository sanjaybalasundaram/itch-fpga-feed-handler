#!/usr/bin/env python3
"""Compare RTL BBO output (sim/actual.txt) with the golden model (sim/expected.txt)."""
import os
import sys

SIM = os.path.join(os.path.dirname(__file__), "..", "sim")


def load(name):
    with open(os.path.join(SIM, name)) as f:
        return [tuple(map(int, line.split())) for line in f if line.strip()]


def main():
    exp, act = load("expected.txt"), load("actual.txt")
    for i, (e, a) in enumerate(zip(exp, act)):
        if e != a:
            print(f"MISMATCH at update {i}:\n  expected {e}\n  actual   {a}")
            sys.exit(1)
    if len(exp) != len(act):
        print(f"MISMATCH: expected {len(exp)} updates, got {len(act)}")
        sys.exit(1)
    print(f"PASS: {len(act)} BBO updates match the golden model bit-for-bit (0 mismatches)")


if __name__ == "__main__":
    main()
