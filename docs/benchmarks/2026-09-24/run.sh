#!/usr/bin/env bash
# Reproduce the complete eight-run NUMAflow cost-model benchmark matrix.
# The "cxlcal" profile changes MODEL constants; it is not a live CXL test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/docs/benchmarks/2026-09-24"
make -C "$ROOT/numaflow" -j2 all
for model in default cxlcal; do
    for workload in zipf uniform hotspot temporal; do
        extra=()
        if [[ "$model" == "cxlcal" ]]; then
            extra=(--cxl-latency-ns 125 --cxl-bandwidth-mbps 25000)
        fi
        "$ROOT/numaflow/build/numaflow" eval \
            --workload "$workload" --keys 20000 --accesses 200000 \
            --epoch 5000 --budget 64 --nodes 2 --seed 20240517 \
            "${extra[@]}" --out "$OUT/bench_${workload}_${model}.json"
    done
done
NF_RESULTS="$OUT" python3 "$ROOT/numaflow/eval/report.py"
mkdir -p "$OUT/reference"
for workload in zipf uniform hotspot temporal; do
    "$ROOT/numaflow/build/numaflow" eval \
        --workload "$workload" --keys 3000 --accesses 120000 \
        --epoch 3000 --budget 256 --nodes 2 --seed 20240517 \
        --out "$OUT/reference/bench_${workload}.json"
done
NF_RESULTS="$OUT/reference" python3 "$ROOT/numaflow/eval/report.py"
python3 "$OUT/validate.py"
