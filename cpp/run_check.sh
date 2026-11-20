#!/usr/bin/env bash
# Build the C++ feed handler and check it against the golden model on several seeds.
# Usage: cpp/run_check.sh [num_messages] [seeds...]
set -euo pipefail
cd "$(dirname "$0")/.."
MSGS=${1:-20000}; shift || true
SEEDS=${*:-"1 2 3 4 5 6"}

cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build cpp/build -j >/dev/null
./cpp/build/test_book

for s in $SEEDS; do
  python3 model/itch_model.py --msgs "$MSGS" --seed "$s" >/dev/null
  locate=$(cut -d' ' -f1 sim/config.txt)
  ./cpp/build/itch_bbo sim/stream.hex "$locate" sim/cpp_actual.txt 2>/dev/null
  if cmp -s sim/cpp_actual.txt sim/expected.txt; then
    echo "seed $s: PASS ($(wc -l < sim/expected.txt) BBO updates identical to golden model)"
  else
    echo "seed $s: MISMATCH"; diff <(head -n 1000 sim/expected.txt) <(head -n 1000 sim/cpp_actual.txt) | head; exit 1
  fi
done
