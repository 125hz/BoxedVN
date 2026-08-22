#!/usr/bin/env bash
# Execute the loader SIMD/control-flow fixture through FEX's ARM64 JIT using
# the VIXL simulator. This is intentionally separate from the iOS build and
# never modifies third_party/fex64/fex.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fex_source="${FEX_SOURCE:-${root}/third_party/fex64/fex}"
fex_build="${FEX_BUILD:-${root}/build/fex-vixl-probe}"
fixture="${root}/scripts/guest-probes/fex64-loader-stall.asm"
cc="${CC:-clang}"
cxx="${CXX:-clang++}"

die() {
    echo "error: $*" >&2
    exit 1
}

[[ "$(uname -s)" == "Linux" ]] || die "the VIXL probe requires a Linux host"
[[ "$(uname -m)" == "x86_64" ]] || die "the VIXL probe expects an x86-64 host"
command -v cmake >/dev/null || die "cmake is required"
command -v ninja >/dev/null || die "ninja is required"
command -v nasm >/dev/null || die "nasm is required"
command -v python3 >/dev/null || die "python3 is required"
command -v "${cc}" >/dev/null || die "the Clang C compiler is required"
command -v "${cxx}" >/dev/null || die "the Clang C++ compiler is required"
[[ -d "${fex_source}/FEXCore" ]] || die "FEX source not found: ${fex_source}"
[[ -f "${fixture}" ]] || die "fixture not found: ${fixture}"

if [[ ! -x "${fex_build}/Bin/TestHarnessRunner" ]]; then
    cmake -S "${fex_source}" -B "${fex_build}" -G Ninja \
        -DCMAKE_C_COMPILER="${cc}" \
        -DCMAKE_CXX_COMPILER="${cxx}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DENABLE_LTO=OFF \
        -DENABLE_ASSERTIONS=ON \
        -DENABLE_X86_HOST_DEBUG=ON \
        -DENABLE_VIXL_SIMULATOR=ON \
        -DBUILD_TESTING=ON \
        -DBUILD_FEXCONFIG=OFF \
        -DBUILD_THUNKS=OFF \
        -DBUILD_FEX_LINUX_TESTS=OFF \
        -DENABLE_GDB_SYMBOLS=OFF \
        -DENABLE_ZYDIS=OFF
    cmake --build "${fex_build}" --target TestHarnessRunner
fi

runner="${fex_build}/Bin/TestHarnessRunner"
[[ -x "${runner}" ]] || die "TestHarnessRunner was not built: ${runner}"

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-fex-vixl.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

probe_bin="${tmp_dir}/fex64-loader-stall.bin"
probe_config="${tmp_dir}/fex64-loader-stall.config.bin"
nasm -f bin "${fixture}" -o "${probe_bin}"
PYTHONDONTWRITEBYTECODE=1 python3 \
    "${fex_source}/Scripts/json_asm_config_parse.py" \
    "${fixture}" "${probe_config}"

[[ -s "${probe_bin}" ]] || die "NASM produced an empty fixture"
[[ -s "${probe_config}" ]] || die "FEX configuration was not generated"

run_one() {
    local maxinst="$1"
    local multiblock="$2"
    echo "[fex-vixl] maxinst=${maxinst} multiblock=${multiblock}"
    FEX_SILENTLOG=0 \
    FEX_MAXINST="${maxinst}" \
    FEX_MULTIBLOCK="${multiblock}" \
    FEX_TSOENABLED=0 \
    python3 - "${runner}" "${probe_bin}" "${probe_config}" <<'PY'
import subprocess
import sys

try:
    result = subprocess.run(
        sys.argv[1:],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=20,
    )
except subprocess.TimeoutExpired as exc:
    if exc.stdout:
        print(exc.stdout, end="")
    print("error: translated fixture exceeded 20 seconds", file=sys.stderr)
    raise SystemExit(124)

print(result.stdout, end="")
raise SystemExit(result.returncode)
PY
}

# Single-block and multiblock modes catch both the scalar dispatcher path and
# the optimized cyclic control-flow path implicated by the loader stall.
run_one 1 0
run_one 500 0
run_one 500 1
echo "[fex-vixl] PASS: SIMD/string/control-flow fixture completed in all modes"
