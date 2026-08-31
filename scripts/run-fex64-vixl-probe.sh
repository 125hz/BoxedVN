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
fixture_negative_add="${root}/scripts/guest-probes/fex64-negative-add-contract.asm"
fixture_indexed_alias="${root}/scripts/guest-probes/fex64-indexed-alias-contract.asm"
fixture_top_alias="${root}/scripts/guest-probes/fex64-top-alias-contract.asm"
fixture_top_alias_repmov="${root}/scripts/guest-probes/fex64-top-alias-repmov.asm"
fixture_top_alias_stack="${root}/scripts/guest-probes/fex64-top-alias-stack.asm"
fixture_dispatcher_return="${root}/scripts/guest-probes/fex64-dispatcher-return.asm"
host_word_check="${root}/scripts/guest-probes/check-ircap-host-words.py"
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
    "${root}/scripts/fex64-patches/fex-arm64-addsub-immediate-range.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-ir-capture-arm.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-low-address-alias.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-longmode-segment-base.patch"
    "${root}/scripts/fex64-patches/fex-boxedwine-harness-alias.patch"
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
[[ -f "${fixture_negative_add}" ]] || \
    die "fixture not found: ${fixture_negative_add}"
[[ -f "${fixture_indexed_alias}" ]] || \
    die "fixture not found: ${fixture_indexed_alias}"
[[ -f "${fixture_top_alias}" ]] || \
    die "fixture not found: ${fixture_top_alias}"
[[ -f "${fixture_top_alias_repmov}" ]] || \
    die "fixture not found: ${fixture_top_alias_repmov}"
[[ -f "${fixture_top_alias_stack}" ]] || \
    die "fixture not found: ${fixture_top_alias_stack}"
[[ -f "${fixture_dispatcher_return}" ]] || \
    die "fixture not found: ${fixture_dispatcher_return}"
[[ -f "${host_word_check}" ]] || \
    die "host-word checker not found: ${host_word_check}"
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
    local boxedwine_alias="${7:-}"
    echo "[fex-vixl] fixture=${label} maxinst=${maxinst} multiblock=${multiblock} alias=${boxedwine_alias:-0}"
    FEX_BOXEDWINE_IRCAP_TARGET="${ircap_target}" \
    FEX_BOXEDWINE_ALIAS="${boxedwine_alias}" \
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
prepare_fixture "${fixture_negative_add}" fex64-negative-add
prepare_fixture "${fixture_indexed_alias}" fex64-indexed-alias
prepare_fixture "${fixture_top_alias}" fex64-top-alias
prepare_fixture "${fixture_top_alias_repmov}" fex64-top-alias-repmov
prepare_fixture "${fixture_top_alias_stack}" fex64-top-alias-stack
prepare_fixture "${fixture_dispatcher_return}" fex64-dispatcher-return

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
python3 "${host_word_check}" "${vector_capture}" \
    --label vector-store --forbid-word ffff0177

# A negative guest displacement reaches the backend as a sign-extended
# 64-bit inline constant, which the ARM64 add/sub immediate form cannot
# encode. The device shifted it into the opcode field instead. This
# fixture checks the arithmetic and the flags, so a truncated or
# sign-flipped immediate fails on results rather than on an encoding scan.
run_one x64-negative-add "${tmp_dir}/fex64-negative-add.bin" \
    "${tmp_dir}/fex64-negative-add.config.bin" 1 0
run_one x64-negative-add "${tmp_dir}/fex64-negative-add.bin" \
    "${tmp_dir}/fex64-negative-add.config.bin" 500 0
run_one x64-negative-add "${tmp_dir}/fex64-negative-add.bin" \
    "${tmp_dir}/fex64-negative-add.config.bin" 500 1

negative_add_capture="${tmp_dir}/fex64-negative-add.ircap.txt"
run_one x64-negative-add-ircap "${tmp_dir}/fex64-negative-add.bin" \
    "${tmp_dir}/fex64-negative-add.config.bin" 500 0 0x10100 \
    | tee "${negative_add_capture}"
python3 "${host_word_check}" "${negative_add_capture}" \
    --label negative-add --forbid-word ffff0177 --require-addsub-register


# A guest effective address that combines a canonical low base with a high
# index. The loader faulted on exactly this shape because the host
# translation was applied to the base before the index contributed its own
# high bits. Every store here is read back and compared, so a wrongly
# ordered translation fails on the value rather than on an encoding scan.
run_one x64-indexed-alias "${tmp_dir}/fex64-indexed-alias.bin" \
    "${tmp_dir}/fex64-indexed-alias.config.bin" 1 0
run_one x64-indexed-alias "${tmp_dir}/fex64-indexed-alias.bin" \
    "${tmp_dir}/fex64-indexed-alias.config.bin" 500 0
run_one x64-indexed-alias "${tmp_dir}/fex64-indexed-alias.bin" \
    "${tmp_dir}/fex64-indexed-alias.config.bin" 500 1

indexed_alias_capture="${tmp_dir}/fex64-indexed-alias.ircap.txt"
run_one x64-indexed-alias-ircap "${tmp_dir}/fex64-indexed-alias.bin" \
    "${tmp_dir}/fex64-indexed-alias.config.bin" 500 0 0x10100 \
    | tee "${indexed_alias_capture}"
python3 "${host_word_check}" "${indexed_alias_capture}" \
    --label indexed-alias --forbid-word ffff0177
# Wine's top-down arena. Its pointers need 47 bits, a different shape
# from every other guest address BoxedWine hosts: the canonical low lane
# fits in 33 and the identity lane in 39. Loads, stores, sub-qword
# accesses and two locked read-modify-writes are all read back and
# compared, so a truncated or sign-extended address fails on the value.
run_one x64-top-alias "${tmp_dir}/fex64-top-alias.bin" \
    "${tmp_dir}/fex64-top-alias.config.bin" 1 0
run_one x64-top-alias "${tmp_dir}/fex64-top-alias.bin" \
    "${tmp_dir}/fex64-top-alias.config.bin" 500 0
run_one x64-top-alias "${tmp_dir}/fex64-top-alias.bin" \
    "${tmp_dir}/fex64-top-alias.config.bin" 500 1

# No targeted IR capture here. The other fixtures pin one instruction at
# guest 0x10100 because a specific host encoding was in question; this
# fixture's claim is behavioural -- every access is read back and compared
# -- and the encoding of the two translation instructions is proven
# against the real emitter in fex64-emitter-encoding-check.cpp instead.
# The same arena, but with BoxedWine's address translation ENABLED, so a
# memory operation that forgets to apply it dereferences an unmapped host
# address and faults. That is the gap the device found: MemCpy ORed the
# low alias base into its working pointers and never relocated the top
# arena, and no fixture running with the translation off could see it.
#
# REP MOVSQ/MOVSB in both lane directions and both DF directions, and REP
# STOSQ/STOSB, at a length that forces the vectorised ldp/stp loop and
# leaves a scalar tail. Guest-visible RSI/RDI/RCX are checked to be
# canonical afterwards.
# The sixth argument is the IR-capture target, unused here; the seventh
# turns the alias on for this fixture and only this fixture.
run_one x64-top-alias-repmov "${tmp_dir}/fex64-top-alias-repmov.bin" \
    "${tmp_dir}/fex64-top-alias-repmov.config.bin" 1 0 "" 1
run_one x64-top-alias-repmov "${tmp_dir}/fex64-top-alias-repmov.bin" \
    "${tmp_dir}/fex64-top-alias-repmov.config.bin" 500 0 "" 1
run_one x64-top-alias-repmov "${tmp_dir}/fex64-top-alias-repmov.bin" \
    "${tmp_dir}/fex64-top-alias-repmov.config.bin" 500 1 "" 1
# The stack ops, with the alias on. Push/PushTwo/Pop/PopTwo used ARM64
# pre/post-indexed forms on the guest-visible address register, whose
# writeback cannot be translated without putting a host address in RSP,
# so they kept dereferencing the canonical stack. Wine's ntdll spun on a
# `push rbp` writing canonical 0x7ffcfc78.
#
# The fixture deliberately never proves a push with a matching pop: the
# harness maps both the canonical page and its alias, so two untranslated
# operations would agree with each other. Each push is read back with an
# ordinary translated load, and each pop is fed by an ordinary translated
# store, so a missing translation fails on the value.
run_one x64-top-alias-stack "${tmp_dir}/fex64-top-alias-stack.bin" \
    "${tmp_dir}/fex64-top-alias-stack.config.bin" 1 0 "" 1
run_one x64-top-alias-stack "${tmp_dir}/fex64-top-alias-stack.bin" \
    "${tmp_dir}/fex64-top-alias-stack.config.bin" 500 0 "" 1
run_one x64-top-alias-stack "${tmp_dir}/fex64-top-alias-stack.bin" \
    "${tmp_dir}/fex64-top-alias-stack.config.bin" 500 1 "" 1
# Wine's dispatcher return. server_init_process_done hands the process its PE
# entry point, call_init_thunk installs the Windows TEB and builds a five-qword
# iretq frame, and __wine_syscall_dispatcher_return takes it into
# LdrInitializeThunk. The device gets through the whole exchange and the
# arch_prctl pair and then exits without running a single Windows instruction,
# so this transition is the one thing on that path never proven under this
# backend.
#
# The fixture restores a saved register set and then crosses to a different
# canonical low stack through iretq, with Wine's own selectors and RFLAGS. That
# is what makes it different from FEX's Primary_CF.asm, which neither restores
# a register set nor changes stacks.
#
# Run with the alias OFF first: that path takes FEX's ordinary descriptor-table
# read, against the harness's real thirty-two entry GDT. Then with the alias
# ON, which is the device's configuration -- there the descriptor table is a
# host pointer that the guest memory funnel must not translate, and an
# untranslated read of it would fault before reaching the target.
run_one x64-dispatcher-return "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 1 0
run_one x64-dispatcher-return "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 500 0
run_one x64-dispatcher-return "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 500 1
run_one x64-dispatcher-return-alias "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 1 0 "" 1
run_one x64-dispatcher-return-alias "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 500 0 "" 1
run_one x64-dispatcher-return-alias "${tmp_dir}/fex64-dispatcher-return.bin" \
    "${tmp_dir}/fex64-dispatcher-return.config.bin" 500 1 "" 1
echo "[fex-vixl] PASS: x64 loader, IA-32 core, vector-store, negative-add, indexed-alias, top-alias, alias-enabled rep-movs and stack, and dispatcher-return fixtures completed in all modes"
