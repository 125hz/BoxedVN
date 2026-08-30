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
command -v objdump >/dev/null || { echo "error: objdump is required" >&2; exit 1; }
command -v strings >/dev/null || { echo "error: strings is required" >&2; exit 1; }
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
if readelf -rW "${output}" | grep -q 'R_X86_64_'; then
    echo "error: the kernel probe has relocations the custom guest mapper cannot apply" >&2
    readelf -rW "${output}" >&2
    exit 1
fi

disassembly="$(objdump -d "${output}")"
for instruction in movlpd movhpd pcmpeqb psubb pmovmskb bsf; do
    grep -Eq "[[:space:]]${instruction}[[:space:]]" <<<"${disassembly}" || {
        echo "error: the kernel probe is missing ${instruction}" >&2
        exit 1
    }
done
grep -Eq '[[:space:]]rep[[:space:]]+stos' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its REP STOS fill path" >&2
    exit 1
}
grep -Eq '[[:space:]]add[[:space:]].*0x40.*%rdx' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its normalized loader pointer update" >&2
    exit 1
}
grep -Eq '[[:space:]]sub[[:space:]].*0xffff' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its scalar mask subtraction" >&2
    exit 1
}
grep -Eq '[[:space:]]jne[[:space:]].*<strcmp_vector_found>' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its mask branch" >&2
    exit 1
}
grep -Eq '[[:space:]]call[[:space:]].*<strcmp_vector_probe>' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its guest call path" >&2
    exit 1
}
grep -Eq '[[:space:]]ret[[:space:]]*$' <<<"${disassembly}" || {
    echo "error: the kernel probe is missing its guest return path" >&2
    exit 1
}
strings "${output}" | grep -Fq 'BoxedWine FEX64 SSE2/REP STOS/indexed-alias/call-ret PASS' || {
    echo "error: the kernel probe is missing its translated-memory success marker" >&2
    exit 1
}

if [[ "$(uname -m)" == "x86_64" ]]; then
    set +e
    native_output="$("${output}" 2>&1)"
    native_status=$?
    set -e
    [[ ${native_status} -eq 47 ]] || {
        echo "error: native kernel probe returned ${native_status}, expected 47" >&2
        exit 1
    }
    [[ "${native_output}" == 'BoxedWine FEX64 SSE2/REP STOS/indexed-alias/call-ret PASS' ]] || {
        echo "error: native kernel probe produced unexpected output" >&2
        printf '%s\n' "${native_output}" >&2
        exit 1
    }
fi

shasum -a 256 "${output}" > "${output}.sha256"
echo "built ${output}"
cat "${output}.sha256"
