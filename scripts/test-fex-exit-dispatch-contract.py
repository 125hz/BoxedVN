#!/usr/bin/env python3
"""Verify BoxedVN's no-link callback against the pinned FEX source ABI."""

from __future__ import annotations

import argparse
from pathlib import Path


def require_ordered(text: str, fragments: list[str], label: str) -> None:
    cursor = 0
    for fragment in fragments:
        found = text.find(fragment, cursor)
        if found < 0:
            raise SystemExit(
                f"{label}: missing ordered contract fragment: {fragment!r}"
            )
        cursor = found + len(fragment)


def read(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(f"required contract source is missing: {path}")
    return path.read_text(encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fex-root", type=Path, required=True)
    arguments = parser.parse_args()

    repository = Path(__file__).resolve().parent.parent
    fex = arguments.fex_root.resolve()

    context = read(
        fex / "FEXCore/Source/Interface/Context/Context.h"
    )
    dispatcher = read(
        fex / "FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp"
    )
    jit = read(fex / "FEXCore/Source/Interface/Core/JIT/JIT.cpp")
    contract = read(
        repository
        / "ios/support/include/boxedvn/fex_exit_dispatch_contract.h"
    )
    backend = read(repository / "ios/runtime/src/BVNFEXBackend.mm")
    loadstore_mask_patch = read(
        repository
        / "scripts/fex64-patches/fex-arm64-pair-immediate-mask.patch"
    )
    loader_fixture = read(
        repository / "scripts/guest-probes/fex64-loader-stall.asm"
    )

    require_ordered(
        context,
        [
            "struct FEX_PACKED ExitFunctionLinkData {",
            "uint64_t HostCode;",
            "uint64_t GuestRIP;",
            "int64_t CallerOffset;",
        ],
        "pinned FEX exit record",
    )
    require_ordered(
        contract,
        [
            "struct FexExitFunctionLinkData final {",
            "std::uint64_t hostCode;",
            "std::uint64_t guestRIP;",
            "std::int64_t callerOffset;",
            "offsetof(FexExitFunctionLinkData, hostCode) == 0",
            "offsetof(FexExitFunctionLinkData, guestRIP) == 8",
            "offsetof(FexExitFunctionLinkData, callerOffset) == 16",
        ],
        "BoxedVN exit record mirror",
    )
    require_ordered(
        dispatcher,
        [
            "ExitFunctionLinkerAddress = GetCursorAddress<uint64_t>();",
            "mov(ARMEmitter::XReg::x0, STATE);",
            "mov(ARMEmitter::XReg::x1, ARMEmitter::XReg::lr);",
            "STATE_PTR(CpuStateFrame, Pointers.ExitFunctionLink)",
            "FillStaticRegs();",
            "br(TMP1);",
        ],
        "pinned FEX dispatcher callback sequence",
    )
    require_ordered(
        jit,
        [
            "uint64_t Arm64JITCore::ExitFunctionLink",
            "auto GuestRip = Record->GuestRIP;",
            "Frame->State.rip = GuestRip;",
            "return Frame->Pointers.DispatcherLoopTop;",
        ],
        "pinned FEX dispatcher-bounce behavior",
    )
    require_ordered(
        backend,
        [
            "state->fexThread = processState->fex->initialThread;",
            "state->fexThread = createFEXThread(*processState->fex, 0, 0);",
            "disableLiveBlockLinking(state->fexThread);",
        ],
        "BoxedVN callback installation",
    )
    require_ordered(
        backend,
        [
            "boxedvn::dispatchWithoutBlockLinking(",
            "frame->State.rip = transition.guestRIP;",
            "return transition.hostTarget;",
        ],
        "BoxedVN callback behavior",
    )

    require_ordered(
        backend,
        [
            "FEXCore::Core::InternalThreadState* createFEXThread(",
            "FEXGuestModeConfigScope modeScope(bundle.mode);",
            "return bundle.context->CreateThread(rip, stack, state);",
            "bool primeFEXThread(",
            "bundle.context->CompileRIP(thread, rip);",
        ],
        "BoxedVN mode-scoped FEX thread construction",
    )
    require_ordered(
        backend,
        [
            "bool recreateLiveContextAfterExec(",
            "savedState.InlineJITBlockHeader = 0;",
            "auto replacementBundle = createFEXContext(",
            "auto* replacement = createFEXThread(",
            "disableLiveBlockLinking(replacement);",
            "primeFEXThread(*replacementBundle, replacement, resumeRIP)",
            "BVNFEXCPU64AdapterBindFEX(",
            "gLiveThreadContexts.erase(retired);",
            "gLiveThreadContexts.emplace(replacement,",
            "threadState->fexThread = replacement;",
            "retiredEpoch->thread = retired;",
            "retiredEpoch->bundle = std::move(processState->fex);",
            "processState->retiredEpochs.push_back(std::move(retiredEpoch));",
            "processState->fex = std::move(replacementBundle);",
            "BOXEDWINE_FEX64_CONTEXT_RESET_RETAINED",
            "BOXEDWINE_FEX64_CONTEXT_RESET_DONE",
            "BVNFEXCPU64AdapterResetAction(adapter);",
            "BOXEDWINE_FEX64_CONTEXT_RESET_DEFERRED",
            "break;",
        ],
        "BoxedVN loader execution-epoch replacement",
    )
    require_ordered(
        loadstore_mask_patch,
        [
            "void LoadStoreNoAllocate(",
            "Instr |= (Imm & 0b111'1111) << 15;",
            "void LoadStorePair(",
            "Instr |= (Imm & 0b111'1111) << 15;",
            "void LoadStoreImm(",
            "Instr |= (Imm & 0b1'1111'1111) << 12;",
        ],
        "FEX ARM64 signed load/store immediate encoding",
    )

    # Device build 137 faulted on this exact lowering of libc's
    # `movups [rax + 0x30], xmm0`: STUR Q23, [X11, #-16]. Without an imm9
    # mask, the sign-extended displacement turns the opcode into 0xffff0177.
    stur_q23_x11 = 0x3C800000 | ((-16 & 0x1FF) << 12) | (11 << 5) | 23
    if stur_q23_x11 != 0x3C9F0177:
        raise SystemExit(
            "FEX ARM64 signed load/store immediate regression: "
            f"expected 0x3c9f0177, got 0x{stur_q23_x11:08x}"
        )
    require_ordered(
        loader_fixture,
        [
            "std",
            "rep movsb",
            "cld",
        ],
        "FEX backward REP MOVS regression fixture",
    )

    # The retired exec epoch owns the host frames the still-running
    # BVNFEXCPU64Run returns through. Ownership must be declared once, held by
    # the process, and released only at process teardown.
    require_ordered(
        backend,
        [
            "struct RetiredFEXEpoch {",
            "std::unique_ptr<FEXContextBundle> bundle;",
            "FEXCore::Core::InternalThreadState* thread = nullptr;",
            "~RetiredFEXEpoch() {",
            "bundle->context->DestroyThread(thread);",
            "struct LiveProcessState {",
            "std::vector<std::unique_ptr<RetiredFEXEpoch>> retiredEpochs;",
        ],
        "BoxedVN retired execution-epoch ownership",
    )
    require_ordered(
        backend,
        [
            "void releaseLiveProcessRun(",
            "state->fex->context->DestroyThread(thread->fexThread);",
            "state->threads.clear();",
            "state->retiredEpochs.clear();",
        ],
        "BoxedVN retired-epoch teardown ordering",
    )

    # Device build 137 reached CONTEXT_RESET_DEFERRED and then faulted at
    # host_pc=0 because the retired epoch was torn down inside the same
    # BVNFEXCPU64Run. Nothing in the recreate path may destroy it.
    recreate_start = backend.index("bool recreateLiveContextAfterExec(")
    terminator = chr(10) + "}" + chr(10)
    recreate_end = backend.index(terminator, recreate_start)
    recreate_body = backend[recreate_start:recreate_end]
    for forbidden in ("DestroyThread", "retiredBundle"):
        if forbidden in recreate_body:
            raise SystemExit(
                "recreateLiveContextAfterExec must not destroy the retired "
                f"execution epoch; found {forbidden!r}"
            )

    # The indexed-context 128-bit path materialized the base offset into TMP1
    # and then passed it to LDUR/STUR again, double-applying the displacement
    # and driving a signed immediate into the unscaled field.
    memory_ops = read(
        fex / "FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp"
    )
    context_offset_patch = read(
        repository
        / "scripts/fex64-patches"
        / "fex-arm64-context-indexed-unaligned-offset.patch"
    )
    # The probe applies and then reverses the maintained patches, so accept
    # either state -- but only those two, and never a mixture.
    unpatched = (
        "ldur(Dst.Q(), TMP1, Op->BaseOffset);" in memory_ops
        and "stur(Value.Q(), TMP1, Op->BaseOffset);" in memory_ops
    )
    patched = (
        "ldur(Dst.Q(), TMP1);" in memory_ops
        and "stur(Value.Q(), TMP1);" in memory_ops
    )
    if unpatched == patched:
        raise SystemExit(
            "FEX indexed context 128-bit path is in an unexpected state: the "
            "pinned source must either double-apply Op->BaseOffset or carry "
            "the downstream fix, not both and not neither"
        )
    if unpatched:
        require_ordered(
            memory_ops,
            [
                "DEF_OP(LoadContextIndexed) {",
                "add(ARMEmitter::Size::i64Bit, TMP1, TMP1, Op->BaseOffset);",
                "ldur(Dst.Q(), TMP1, Op->BaseOffset);",
                "DEF_OP(StoreContextIndexed) {",
                "add(ARMEmitter::Size::i64Bit, TMP1, TMP1, Op->BaseOffset);",
                "stur(Value.Q(), TMP1, Op->BaseOffset);",
            ],
            "pinned FEX double-applied indexed context offset",
        )
    require_ordered(
        context_offset_patch,
        [
            "a/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp",
            "-          ldur(Dst.Q(), TMP1, Op->BaseOffset);",
            "+          ldur(Dst.Q(), TMP1);",
            "-          stur(Value.Q(), TMP1, Op->BaseOffset);",
            "+          stur(Value.Q(), TMP1);",
        ],
        "BoxedVN indexed context offset patch",
    )

    # Both the iOS translator build and the simulator probe must carry every
    # maintained encoding patch, or the probe validates code the device never
    # runs.
    fex_build_script = read(repository / "scripts/build-fex64-fex.sh")
    vixl_probe = read(repository / "scripts/run-fex64-vixl-probe.sh")
    for label, text, fragment in (
        ("iOS translator build", fex_build_script,
         "apply_patch fex-arm64-context-indexed-unaligned-offset.patch"),
        ("VIXL probe", vixl_probe,
         "fex-arm64-context-indexed-unaligned-offset.patch"),
    ):
        if fragment not in text:
            raise SystemExit(
                f"{label} does not apply the indexed context offset patch"
            )

    # A restored TestHarnessRunner is not proof that the patches in this tree
    # were compiled. The probe must rebuild, and must build the emitter
    # encoding regression from source every run.
    if 'if [[ ! -x "${fex_build}/Bin/TestHarnessRunner" ]]; then' in vixl_probe:
        raise SystemExit(
            "the VIXL probe still accepts a cached TestHarnessRunner as proof "
            "that the maintained patches were compiled"
        )
    require_ordered(
        vixl_probe,
        [
            "cmake --build \"${fex_build}\" --target TestHarnessRunner",
            "${encoding_check_source}\" -o \"${encoding_check}\"",
            "\"${encoding_check}\"",
            "build_runner" + chr(10),
            'runner="${fex_build}/Bin/TestHarnessRunner"',
        ],
        "VIXL probe rebuild ordering",
    )

    # The encoding regression drives the real ARM64 emitter, not arithmetic.
    encoding_check = read(
        repository / "scripts/guest-probes/fex64-emitter-encoding-check.cpp"
    )
    require_ordered(
        encoding_check,
        [
            "#include <CodeEmitter/Emitter.h>",
            "ARMEmitter::Emitter emitter(buffer, sizeof(buffer));",
            "emitter.stur(ARMEmitter::QRegister(23), "
            "ARMEmitter::Register(11), -16);",
            "if (word != 0x3c9f0177u) {",
            "emitter.stur(ARMEmitter::QRegister(23), "
            "ARMEmitter::Register(11));",
            "for (int32_t immediate = -256; immediate <= 255; ++immediate) {",
            "kDeviceFaultWord",
        ],
        "BoxedVN ARM64 emitter encoding regression",
    )

    print("FEX exit-dispatch and loader-boundary contracts verified")


if __name__ == "__main__":
    main()
