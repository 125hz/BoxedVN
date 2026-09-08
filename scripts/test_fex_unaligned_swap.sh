#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
binary=$(mktemp /tmp/boxedvn-swap.XXXXXX)
trap 'rm -f "$binary"' EXIT
aarch64-linux-gnu-g++ -O2 -std=c++17 -pthread -Iinclude scripts/tests/fex_unaligned_swap.cpp -o "$binary"
qemu-aarch64 -L /usr/aarch64-linux-gnu "$binary"
