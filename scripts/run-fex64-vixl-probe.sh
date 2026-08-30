#!/usr/bin/env bash
# Execute the x64 loader and IA-32 core fixtures through FEX's ARM64 JIT using
# the VIXL simulator. This is intentionally separate from the iOS build and
# leaves third_party/fex64/fex unchanged when it exits.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fex_source="${FEX_SOURCE:-${root}/third_party/fex64/fex}"
fex_build="${FEX_BUILD:-${root}/build/fex-vixl-probe}"
fixture64="${root}/scripts/guest-probes/fex64-loader-stall.asm"
fixture32="${root}/scripts/guest-probes/fex32-core-contract.asm"
fixture_vector="${root}/scripts/guest-probes/fex64-vector-store-contract.asm"
host_stubs_source="${root}/scripts/guest-probes/fex64-host-stubs.cpp"
encoding_check_source="${root}/scripts/guest-probes/fex64-emitter-encoding-check.cpp"
cc="${CC:-clang}"
cxx="${CXX:-clang++}"
allocator_declaration_header="${fex_source}/FEXCore/Source/Utils/Allocator.h"
runtime_patches=(
    "${root}/scripts/fex64-patches/fex-ios-host-diagnostics-guard.patch"
    "${root}/scripts/fex64-patches/fex-ios-caspal-diagnostic-host.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-block-diagnostics.patch"
    "${root}/scripts/fex64-patches/fex-apple-dual-map-cache-publish.patch"
    "${root}/scripts/fex64-patches/fex-arm64-pair-immediate-mask.patch"
    "${root}/scripts/fex64-patches/fex-arm64-context-indexed-unaligned-offset.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-ir-capture-arm.patch"
)

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
[[ -f "${fixture64}" ]] || die "fixture not found: ${fixture64}"
[[ -f "${fixture32}" ]] || die "fixture not found: ${fixture32}"
[[ -f "${fixture_vector}" ]] || \
    die "fixture not found: ${fixture_vector}"
[[ -f "${host_stubs_source}" ]] || die "host stubs not found: ${host_stubs_source}"
[[ -f "${encoding_check_source}" ]] || \
    die "encoding check not found: ${encoding_check_source}"
[[ -f "${allocator_declaration_header}" ]] || \
    die "allocator declaration header not found: ${allocator_declaration_header}"
for runtime_patch in "${runtime_patches[@]}"; do
    [[ -f "${runtime_patch}" ]] || die "runtime patch not found: ${runtime_patch}"
done

build_runner() (
    applied_patches=()
    restore_source() {
        local index
        for ((index=${#applied_patches[@]} - 1; index >= 0; index--)); do
            git -C "${fex_source}" apply --reverse \
                "${applied_patches[index]}" || true
        done
    }
    trap restore_source EXIT

    # Exercise the same maintained translator patches as the iOS build. Apply
    # them only for this build and restore the fetched checkout on every exit.
    for runtime_patch in "${runtime_patches[@]}"; do
        if git -C "${fex_source}" apply --reverse --check \
                "${runtime_patch}" 2>/dev/null; then
            continue
        fi
        git -C "${fex_source}" apply --check "${runtime_patch}" || \
            die "runtime patch no longer applies: ${runtime_patch}"
        git -C "${fex_source}" apply "${runtime_patch}"
        applied_patches+=("${runtime_patch}")
    done

    # The pinned iOS fork's Linux Allocator.cpp calls its private
    # InitializeAllocator declaration without including the private header.
    # Preinclude that declaration for this host-only conformance build; keep
    # the pinned third-party checkout untouched.
    mkdir -p "${fex_build}"
    host_stubs_object="${fex_build}/boxedvn-probe-host-stubs.o"
    "${cxx}" -std=c++20 -c "${host_stubs_source}" -o "${host_stubs_object}"
    cmake -S "${fex_source}" -B "${fex_build}" -G Ninja \
        -DCMAKE_C_COMPILER="${cc}" \
        -DCMAKE_CXX_COMPILER="${cxx}" \
        -DCMAKE_CXX_FLAGS="-include${allocator_declaration_header}" \
        -DCMAKE_EXE_LINKER_FLAGS="${host_stubs_object}" \
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

    # The device SIGILL was an ARM64 encoding defect, so validate the emitter
    # itself here, with the maintained patches applied, before any fixture
    # runs. Building this from source is also what proves the patches in this
    # tree were compiled rather than inherited from a cached runner.
    encoding_check="${fex_build}/boxedvn-emitter-encoding-check"
    fmt_include=""
    for candidate in "${fex_source}/External/fmt/include" \
                     "${fex_source}/External/fmt"; do
        if [[ -f "${candidate}/fmt/format.h" ]]; then
            fmt_include="${candidate}"
            break
        fi
    done
    [[ -n "${fmt_include}" ]] || \
        die "vendored fmt headers not found under ${fex_source}/External/fmt"
    "${cxx}" -std=c++20 -O1 -DFMT_HEADER_ONLY \
        -I"${fex_source}/CodeEmitter" \
        -I"${fex_source}/FEXCore/include" \
        -I"${fex_source}/FEXHeaderUtils" \
        -I"${fmt_include}" \
        "${encoding_check_source}" -o "${encoding_check}"
    "${encoding_check}"
)

# Always apply the maintained patches and rebuild. A restored cache can carry a
# TestHarnessRunner that predates the patches in this tree, and accepting that
# executable as proof reported a green fixture for code that was never built.
build_runner

runner="${fex_build}/Bin/TestHarnessRunner"
[[ -x "${runner}" ]] || die "TestHarnessRunner was not built: ${runner}"

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-fex-vixl.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

run_one() {
    local label="$1"
    local probe_bin="$2"
    local probe_config="$3"
    local maxinst="$4"
    local multiblock="$5"
    local ircap_target="${6:-}"
    echo "[fex-vixl] fixture=${label} maxinst=${maxinst} multiblock=${multiblock}"
    FEX_BOXEDWINE_IRCAP_TARGET="${ircap_target}" \
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

prepare_fixture() {
    local fixture="$1"
    local stem="$2"
    local probe_bin="${tmp_dir}/${stem}.bin"
    local probe_config="${tmp_dir}/${stem}.config.bin"
    nasm -f bin "${fixture}" -o "${probe_bin}"
    PYTHONDONTWRITEBYTECODE=1 python3 \
        "${fex_source}/Scripts/json_asm_config_parse.py" \
        "${fixture}" "${probe_config}"
    [[ -s "${probe_bin}" ]] || die "NASM produced an empty ${stem} fixture"
    [[ -s "${probe_config}" ]] || \
        die "FEX configuration was not generated for ${stem}"
}

prepare_fixture "${fixture64}" fex64-loader-stall
prepare_fixture "${fixture32}" fex32-core-contract
prepare_fixture "${fixture_vector}" fex64-vector-store

# Single-block and multiblock modes catch both the scalar dispatcher path and
# the optimized cyclic control-flow path implicated by the loader stall.
run_one x64-loader "${tmp_dir}/fex64-loader-stall.bin" \
    "${tmp_dir}/fex64-loader-stall.config.bin" 1 0
run_one x64-loader "${tmp_dir}/fex64-loader-stall.bin" \
    "${tmp_dir}/fex64-loader-stall.config.bin" 500 0
run_one x64-loader "${tmp_dir}/fex64-loader-stall.bin" \
    "${tmp_dir}/fex64-loader-stall.config.bin" 500 1

# The IA-32 fixture is a separate contract: it catches accidental 64-bit
# address arithmetic, 64-bit call/return state and SSE/REP regressions before
# a future biased memory window is allowed to select FEX32 in BoxedWine.
run_one ia32-core "${tmp_dir}/fex32-core-contract.bin" \
    "${tmp_dir}/fex32-core-contract.config.bin" 1 0
run_one ia32-core "${tmp_dir}/fex32-core-contract.bin" \
    "${tmp_dir}/fex32-core-contract.config.bin" 500 0
run_one ia32-core "${tmp_dir}/fex32-core-contract.bin" \
    "${tmp_dir}/fex32-core-contract.config.bin" 500 1

# The unaligned 128-bit vector store the device reported as an invalid
# host word. Run it through the real ARM64 JIT in every mode, then once
# more with FEX's targeted IR capture armed on the pinned guest address,
# so the host words emitted for that one instruction are proven rather
# than inferred from a separate rebuild of the encoder.
run_one x64-vector-store "${tmp_dir}/fex64-vector-store.bin" \
    "${tmp_dir}/fex64-vector-store.config.bin" 1 0
run_one x64-vector-store "${tmp_dir}/fex64-vector-store.bin" \
    "${tmp_dir}/fex64-vector-store.config.bin" 500 0
run_one x64-vector-store "${tmp_dir}/fex64-vector-store.bin" \
    "${tmp_dir}/fex64-vector-store.config.bin" 500 1

vector_capture="${tmp_dir}/fex64-vector-store.ircap.txt"
run_one x64-vector-store-ircap "${tmp_dir}/fex64-vector-store.bin" \
    "${tmp_dir}/fex64-vector-store.config.bin" 500 0 0x10100 \
    | tee "${vector_capture}"

python3 - "${vector_capture}" <<'CAPTURE'
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()

if "[ircap]" not in text:
    raise SystemExit(
        "error: targeted IR capture produced no output; the downstream "
        "arming patch is not compiled into this translator"
    )

if "ml623 HOST bytes" not in text:
    raise SystemExit(
        "error: IR capture never reached the host-word stage for the "
        "pinned guest target"
    )

words = re.findall(r"ircap[]][ ]+[+]0x[0-9a-f]+[ ]+([0-9a-f]{8})", text)
if not words:
    raise SystemExit("error: IR capture printed no emitted host words")

if "ffff0177" in words:
    raise SystemExit(
        "error: the emitter produced the invalid device word 0xffff0177 "
        "for movups [rax + 0x30], xmm0"
    )

print("[fex-vixl] ircap host words: " + " ".join(words))
CAPTURE
echo "[fex-vixl] PASS: x64 loader, IA-32 core and vector-store fixtures completed in all modes"
