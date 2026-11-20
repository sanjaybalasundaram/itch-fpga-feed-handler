#!/usr/bin/env bash
# Generate a stream, simulate the RTL, and check it against the golden model.
# Usage: ./run_sim.sh [num_messages] [seed] [gaps(0|1)]
set -euo pipefail
cd "$(dirname "$0")"
MSGS=${1:-20000}
SEED=${2:-1}
GAPS=${3:-0}

python3 model/itch_model.py --msgs "$MSGS" --seed "$SEED"
iverilog -g2012 -o sim/tb.vvp tb/tb_feed_handler.v rtl/*.v
vvp -n sim/tb.vvp +GAPS="$GAPS"
python3 model/compare.py
cat sim/latency.txt
