#!/usr/bin/env bash
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

echo "[V1.1] cmake configure (Debug)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON

echo "[V1.2] make all"
cmake --build build -j

echo "[V1.3] check key symbols in iotcam binary"
BIN=build/src/iotcam

REQ_SYMS=(
    "build_dmabuf_pipeline"
    "build_mmap_pipeline"
    "detect_mode"
    "PipelineBuilder"
)

for s in "${REQ_SYMS[@]}"; do
    if nm -C "$BIN" | grep -q "$s"; then
        echo "  [OK] $s"
    else
        echo "  [FAIL] $s missing in $BIN"
        exit 1
    fi
done

echo "[V1] PASS"