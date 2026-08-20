#!/usr/bin/env bash
# Build the deterministic x86-64 Linux ELF acceptance process.
# Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_file="${root}/scripts/guest-probes/fex64-kernel-probe.S"
output_dir="${root}/build/guest-probes"
output="${output_dir}/boxedvn-fex64-kernel-probe"

command -v cc >/dev/null || { echo "error: a native x86-64 C compiler is required" >&2; exit 1; }
command -v readelf >/dev/null || { echo "error: readelf is required" >&2; exit 1; }
mkdir -p "${output_dir}"

cc -nostdlib -fPIE -pie \
    -Wl,--no-dynamic-linker \
    -Wl,--build-id=none \
    -Wl,-z,max-page-size=0x1000 \
    -Wl,-e,_start \
    -o "${output}" "${source_file}"

header="$(readelf -h "${output}")"
grep -q 'Class:[[:space:]]*ELF64' <<<"${header}"
grep -q 'Machine:[[:space:]]*Advanced Micro Devices X86-64' <<<"${header}"
grep -q 'Type:[[:space:]]*DYN' <<<"${header}"
if readelf -l "${output}" | grep -q 'INTERP'; then
    echo "error: the kernel probe unexpectedly needs a host dynamic linker" >&2
    exit 1
fi

shasum -a 256 "${output}" > "${output}.sha256"
echo "built ${output}"
cat "${output}.sha256"
