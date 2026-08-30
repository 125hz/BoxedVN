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

    # ------------------------------------------------------------------ #
    # Device codegen provenance: exact guest RIP, targeted IR capture,     #
    # production emitter self-test, and the cold exec-replacement unwind.  #
    # ------------------------------------------------------------------ #
    adapter = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    ircap_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-ir-capture-arm.patch"
    )
    vector_fixture = read(
        repository / "scripts/guest-probes/fex64-vector-store-contract.asm"
    )
    core = read(fex / "FEXCore/Source/Interface/Core/Core.cpp")
    pass_manager = read(fex / "FEXCore/Source/Interface/IR/PassManager.cpp")

    # The pinned translator already exports the reconstruction and owns the
    # capture implementation; the downstream patch only adds an arming surface.
    require_ordered(
        core,
        [
            'extern "C" uint64_t ios_fex_rip_from_hostpc(uint64_t BlockBegin,',
            "InlineTail->NumberOfRIPEntries",
            "return StartingGuestRIP;",
        ],
        "pinned FEX host-PC to guest-RIP reconstruction",
    )
    require_ordered(
        pass_manager,
        [
            "uint64_t FEX_MythicIRCapTarget = 0;",
            "std::atomic<uint32_t> IRCapTaken {0};",
            'extern "C" void FEX_MythicIRCapMark(uint64_t GuestRIP) {',
            "CONTAINS target",
        ],
        "pinned FEX targeted IR capture implementation",
    )

    # The signal path reconstructs the exact RIP from the frame's own block
    # header, before it rewrites the machine context, and only publishes it.
    require_ordered(
        adapter,
        [
            "static bool containUnclassifiedFEXFault(",
            "const uint64_t blockBegin = frame->State.InlineJITBlockHeader;",
            "exactGuestRIP = ios_fex_rip_from_hostpc(blockBegin, hostPC);",
            "BOXEDWINE_FEX64_EXACT_RIP",
            "BVNFEXBackendPublishPendingIRCapTarget(exactGuestRIP);",
            "BOXEDWINE_FEX64_HOST_FAULT_CONTAINED",
        ],
        "BoxedVN exact host-PC to guest-RIP reconstruction",
    )
    fault_start = adapter.index("static bool containUnclassifiedFEXFault(")
    fault_end = adapter.index(chr(10) + "}" + chr(10), fault_start)
    fault_body = adapter[fault_start:fault_end]
    for forbidden in ("RestoreRIPFromHostPC(", "FEX_BoxedWineIRCapArm(",
                      "createFEXContext("):
        if forbidden in fault_body:
            raise SystemExit(
                "the contained-fault signal path must not use "
                f"{forbidden!r}: it runs before the frame is repaired and must "
                "not re-enter the translator"
            )

    # Publication is lock-free and outlives teardown of the failed guest.
    require_ordered(
        adapter,
        [
            "static std::atomic<uint64_t> gPendingIRCapTarget {0};",
            'extern "C" void BVNFEXBackendPublishPendingIRCapTarget(',
            "gPendingIRCapTarget.store(guestRIP, std::memory_order_relaxed);",
            'extern "C" uint64_t BVNFEXBackendTakePendingIRCapTarget(',
            "gPendingIRCapTarget.exchange(0, std::memory_order_relaxed);",
        ],
        "BoxedVN pending capture-target publication",
    )

    # Arming happens on the next ordinary live x86-64 context, exactly once,
    # after that context exists and before anything can execute on it.
    require_ordered(
        backend,
        [
            "LiveProcessState* getLiveProcessState(",
            "const bool firstLiveProcess = gLiveProcesses.empty();",
            "state->fex = createFEXContext(boxedvn::FexGuestMode::X86_64,",
            "firstLiveProcess ? BVNFEXBackendTakePendingIRCapTarget() : 0;",
            "FEX_BoxedWineIRCapArm(pendingIRCapTarget);",
            "BOXEDWINE_FEX64_IRCAP_ARMED",
            "gLiveProcesses.emplace(process, std::move(state));",
        ],
        "BoxedVN targeted capture arming on the next live context",
    )
    probe_context = backend.index("gProbeContext = createFEXContext(")
    if "BVNFEXBackendTakePendingIRCapTarget" in backend[probe_context:]:
        raise SystemExit(
            "the backend startup probe must not consume the pending capture "
            "target"
        )

    # The device self-test runs the production encoder out of the linked
    # libFEXCore.a, not a constant and not a separate host rebuild of it.
    require_ordered(
        backend,
        [
            'extern "C" uint32_t FEX_BoxedWineEmitterSelfTest(uint32_t* actual);',
            "const uint32_t expected = FEX_BoxedWineEmitterSelfTest(&emitted);",
            "BOXEDWINE_FEX64_EMITTER_SELFTEST actual=0x%08x ",
            "expected=0x%08x pass=%d",
        ],
        "BoxedVN device emitter self-test",
    )
    require_ordered(
        ircap_patch,
        [
            "a/FEXCore/Source/Interface/Core/JIT/JIT.cpp",
            '+extern "C" uint32_t FEX_BoxedWineEmitterSelfTest(uint32_t* ActualOut) {',
            "+  ARMEmitter::Emitter Emit(Buffer, sizeof(Buffer));",
            "+  Emit.stur(ARMEmitter::QRegister(23), ARMEmitter::Register(11), -16);",
            "+  return 0x3c9f0177u;",
            "a/FEXCore/Source/Interface/IR/PassManager.cpp",
            '+extern "C" void FEX_BoxedWineIRCapArm(uint64_t Target) {',
            "+  FEX_MythicIRCapTarget = Target;",
            "+  IRCapTaken.store(0, std::memory_order_relaxed);",
            '+extern "C" void FEX_BoxedWineIRCapDisarm() {',
        ],
        "BoxedVN targeted capture arming patch",
    )

    # Both consumers of the maintained patch set must apply it.
    for label, text, fragment in (
        ("iOS translator build", fex_build_script,
         "apply_patch fex-boxedwine-ir-capture-arm.patch"),
        ("VIXL probe", vixl_probe,
         "fex-boxedwine-ir-capture-arm.patch"),
    ):
        if fragment not in text:
            raise SystemExit(
                f"{label} does not apply the targeted capture arming patch"
            )

    # The generic guest regression: the exact operation, at a pinned address,
    # driven through the real JIT with the capture armed on it.
    require_ordered(
        vector_fixture,
        [
            "ORG 0x10000",
            "times 0x100 - ($ - $$) db 0x90",
            "vector_store_target:",
            "movups  [rax + 0x30], xmm0",
            "movups  [rbx - 0x10], xmm0",
        ],
        "BoxedVN unaligned vector-store guest fixture",
    )
    require_ordered(
        vixl_probe,
        [
            'prepare_fixture "${fixture_vector}" fex64-vector-store',
            "run_one x64-vector-store ",
            "run_one x64-vector-store-ircap ",
            "0x10100 ",
            '--label vector-store --forbid-word ffff0177',
        ],
        "VIXL vector-store regression and capture assertion",
    )

    # The cold exec-replacement unwind must be traceable in order: re-entry
    # into a replacement epoch, the ExecuteThread return, and the return to
    # the BoxedWine scheduler.
    require_ordered(
        backend,
        [
            "std::atomic<uint32_t> execEpoch {0};",
            "BOXEDWINE_FEX64_UNWIND_REENTER",
            "auto* enteredContext = processState->fex->context.get();",
            "processState->fex->context->ExecuteThread(threadState->fexThread);",
            "BOXEDWINE_FEX64_UNWIND_RETURN",
            "BOXEDWINE_FEX64_CONTEXT_RESET_DEFERRED",
            "BOXEDWINE_FEX64_UNWIND_RUN_RETURN",
            "finish(retireThread, retireProcess);",
        ],
        "BoxedVN cold exec-replacement unwind trace",
    )
    epoch_advance = (
        "processState->execEpoch.fetch_add(1, std::memory_order_acq_rel);"
    )
    if epoch_advance not in backend:
        raise SystemExit(
            "the exec-replacement path must advance the epoch identity the "
            "unwind trace reports"
        )

    # ------------------------------------------------------------------ #
    # Unencodable add/sub constants must reach a register, not an          #
    # immediate field. The device executed 0xffff0177 where a valid add    #
    # belonged, because a sign-extended -0x40 was shifted into the opcode. #
    # ------------------------------------------------------------------ #
    addsub_patch = read(
        repository
        / "scripts/fex64-patches/fex-arm64-addsub-immediate-range.patch"
    )
    negative_add_fixture = read(
        repository / "scripts/guest-probes/fex64-negative-add-contract.asm"
    )
    host_word_check = read(
        repository / "scripts/guest-probes/check-ircap-host-words.py"
    )
    alu_ops = read(fex / "FEXCore/Source/Interface/Core/JIT/ALUOps.cpp")
    loadstore_emitter = read(
        fex / "CodeEmitter/CodeEmitter/ALUOps.inl"
    )

    # The pinned defect: selection is unconditional on IsInlineConstant, and
    # the emitter only rejects an oversized immediate through an assertion
    # that the Release iOS build compiles out.
    alu_unpatched = "DEF_BINOP_WITH_CONSTANT(Add, add, add)" in alu_ops
    alu_patched = "DEF_ADDSUB_WITH_CONSTANT(Add, add, add)" in alu_ops
    if alu_unpatched == alu_patched:
        raise SystemExit(
            "FEX ARM64 add/sub constant selection is in an unexpected state: "
            "the pinned source must either route every inline constant to the "
            "immediate form or carry the downstream fix, not both and neither"
        )
    if alu_unpatched:
        require_ordered(
            alu_ops,
            [
                "#define DEF_BINOP_WITH_CONSTANT(FEXOp, VarOp, ConstOp)",
                "if (IsInlineConstant(Op->Src2, &Const)) {",
                "ConstOp(ConvertSize(IROp), GetReg(Node), GetReg(Op->Src1), "
                "Const);",
                "DEF_BINOP_WITH_CONSTANT(Add, add, add)",
            ],
            "pinned FEX unconditional add/sub immediate selection",
        )
    require_ordered(
        loadstore_emitter,
        [
            "DataProcessing_AddSub_Imm",
            "LOGMAN_THROW_A_FMT",
            "Instr |= Imm << 10;",
        ],
        "pinned FEX add/sub immediate encoder",
    )

    # The correction is in JIT selection, and only there.
    require_ordered(
        addsub_patch,
        [
            "a/FEXCore/Source/Interface/Core/JIT/ALUOps.cpp",
            "#define DEF_ADDSUB_WITH_CONSTANT(FEXOp, VarOp, ConstOp)",
            "if (IsInlineConstant(Op->Src2, &Const)) {",
            "if (ARMEmitter::IsImmAddSub(Const)) {",
            "LoadConstant(ConvertSize(IROp), TMP1, Const);",
            "VarOp(ConvertSize(IROp), GetReg(Node), "
            "GetZeroableReg(Op->Src1), TMP1);",
            "+DEF_ADDSUB_WITH_CONSTANT(Add, add, add)",
            "+DEF_ADDSUB_WITH_CONSTANT(Sub, sub, sub)",
            "+DEF_ADDSUB_WITH_CONSTANT(AddWithFlags, adds, adds)",
            "+DEF_ADDSUB_WITH_CONSTANT(SubWithFlags, subs, subs)",
        ],
        "BoxedVN add/sub immediate range patch",
    )
    for touched in ("CodeEmitter/CodeEmitter/ALUOps.inl",
                    "CodeEmitter/CodeEmitter/Emitter.h"):
        if touched in addsub_patch:
            raise SystemExit(
                "the add/sub fix must correct JIT selection, not the encoder: "
                f"masking the immediate in {touched} would turn a negative "
                "displacement into a positive one and corrupt guest results"
            )
    if "DEF_BINOP_WITH_CONSTANT(Or, orr, orr)" not in alu_ops:
        raise SystemExit(
            "the generic logical and shift lowering must remain on the "
            "original macro"
        )

    # Every maintained clean-source route applies it.
    for label, text, fragment in (
        ("iOS translator build", fex_build_script,
         "apply_patch fex-arm64-addsub-immediate-range.patch"),
        ("VIXL probe", vixl_probe,
         "fex-arm64-addsub-immediate-range.patch"),
    ):
        if fragment not in text:
            raise SystemExit(
                f"{label} does not apply the add/sub immediate range patch"
            )

    # The regression drives the proven operation and checks arithmetic and
    # flags, not just an encoding scan.
    require_ordered(
        negative_add_fixture,
        [
            "ORG 0x10000",
            "times 0x0fc - ($ - $$) db 0x90",
            "negative_add_entry:",
            "negative_add_target:",
            "lea     rbx, [rdi - 0x40]",
            "lea     edx, [esi - 0x40]",
            "add     r8, -0x40",
            "jnc     fail",
            "sub     r12, 0x12345",
            "add     rax, 0x40",
            "add     rax, 0x1000",
        ],
        "BoxedVN unencodable add/sub guest fixture",
    )
    require_ordered(
        host_word_check,
        [
            "ADDSUB_SHIFTED_REGISTER_MASK = 0x7F200000",
            "0x0B000000",
            "0x2B000000",
            "0x4B000000",
            "0x6B000000",
            'if "[ircap]" not in text:',
            'if "ml623 HOST bytes" not in text:',
            "if (int(word, 16) >> 21) == 0x7FF:",
            "require_addsub_register",
        ],
        "BoxedVN emitted host-word checker",
    )
    require_ordered(
        vixl_probe,
        [
            'prepare_fixture "${fixture_negative_add}" fex64-negative-add',
            "run_one x64-negative-add ",
            '"${tmp_dir}/fex64-negative-add.config.bin" 1 0',
            '"${tmp_dir}/fex64-negative-add.config.bin" 500 0',
            '"${tmp_dir}/fex64-negative-add.config.bin" 500 1',
            "run_one x64-negative-add-ircap ",
            "0x10100 ",
            '--label negative-add --forbid-word ffff0177 '
            '--require-addsub-register',
        ],
        "VIXL unencodable add/sub regression",
    )

    # ------------------------------------------------------------------ #
    # Guest mmap placement. Wine's low reservation walk produced 53,753    #
    # high-window relocations per launch because only MAP_FIXED was        #
    # recognised; the window a direct guest can execute from is bounded.   #
    # ------------------------------------------------------------------ #
    placement_header = read(repository / "include/guest_mmap_placement.h")
    syscall64 = read(repository / "source/kernel/syscall64.cpp")
    kmemory64_header = read(repository / "include/kmemory64.h")
    kmemory64 = read(repository / "source/kernel/kmemory64.cpp")
    process_header = read(repository / "include/kprocess.h")

    if "#define K_MAP_FIXED_NOREPLACE 0x100000" not in process_header:
        raise SystemExit(
            "MAP_FIXED_NOREPLACE must keep the Linux flag value the guest sends"
        )

    require_ordered(
        placement_header,
        [
            "enum class GuestMmapPlacement",
            "MapExactNoReplace",
            "RelocateHighWindow",
            "FailExists",
            "FailNoMemory",
            "chooseGuestMmapPlacement(",
            "if (request.fixedNoReplace) {",
            "if (!request.exactRangeUnmapped) {",
            "return GuestMmapPlacement::FailExists;",
            "if (request.nativeIdentity && !request.exactRangeAllowed) {",
            "return GuestMmapPlacement::FailExists;",
            "return GuestMmapPlacement::MapExactNoReplace;",
            "if (request.fixed) {",
            "request.protection == 0",
            "return GuestMmapPlacement::FailNoMemory;",
        ],
        "BoxedVN guest mmap placement policy",
    )

    # The address space owns the atomic no-replace operation; the syscall layer
    # must not open-code a check-then-MAP_FIXED sequence.
    require_ordered(
        kmemory64_header,
        [
            "U64 mmapAnonymousNoReplace(U64 addr, U64 len, U32 prot);",
            "bool rangeCompletelyUnmapped(U64 addr, U64 len) const;",
        ],
        "BoxedVN no-replace address-space API",
    )
    require_ordered(
        kmemory64,
        [
            "bool KMemory64::rangeCompletelyUnmapped(",
            "flags & K64_PAGE_MAPPED",
            "U64 KMemory64::mmapAnonymousNoReplace(",
            "BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);",
            "nativeIdentityMode() && !nativeGuestRangeAllowed(addr, mapLen)",
            "return (U64)-K_EEXIST;",
            "if (!rangeCompletelyUnmapped(addr, mapLen)) {",
            "return (U64)-K_EEXIST;",
            "mmapAnonymousFixed(addr, mapLen, prot);",
        ],
        "BoxedVN atomic no-replace mapping",
    )
    noreplace_start = kmemory64.index("U64 KMemory64::mmapAnonymousNoReplace(")
    noreplace_end = kmemory64.index(chr(10) + "}" + chr(10), noreplace_start)
    noreplace_body = kmemory64[noreplace_start:noreplace_end]
    if "mmapReserveAndMap(" in noreplace_body:
        raise SystemExit(
            "a no-replace mapping must never fall through to the relocating "
            "allocator"
        )

    # Both syscall paths route through the policy and account for what they do.
    require_ordered(
        syscall64,
        [
            '#include "guest_mmap_placement.h"',
            "struct GuestMmapCounters {",
            "lowIneligibleHints",
            "fixedNoReplaceRequests",
            "highWindowRelocations",
            "rejectedWithoutAllocating",
            "highWindowFailures",
            "BOXEDWINE_X64_MMAP_SUMMARY",
            "static U64 sys_mmap64(",
            "const bool fixedNoReplace = (flags & K_MAP_FIXED_NOREPLACE) != 0;",
            "boxedvn::chooseGuestMmapPlacement(request);",
            "case boxedvn::GuestMmapPlacement::MapExactNoReplace:",
            "cpu->memory->mmapAnonymousNoReplace(",
            "case boxedvn::GuestMmapPlacement::FailExists:",
            "ret = (U64)-K_EEXIST;",
            "case boxedvn::GuestMmapPlacement::FailNoMemory:",
            "ret = (U64)-K_ENOMEM;",
            "BOXEDWINE_X64_MMAP ordinal=",
        ],
        "BoxedVN anonymous mmap placement routing",
    )
    require_ordered(
        syscall64,
        [
            "static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, "
            "U64 prot,",
            "const bool fixedNoReplace = (flags & K_MAP_FIXED_NOREPLACE) != 0;",
            "if (fixedNoReplace) {",
            "cpu->memory->rangeCompletelyUnmapped(",
            "return (U64)-K_EEXIST;",
            "} else if (!fixed && !hintAllowed) {",
            "mmapReserveAndMap(length, loadProt);",
        ],
        "BoxedVN file-backed mmap no-replace distinction",
    )

    # The enriched fault report must name the guest registers the effective
    # address is built from, so a zero base is distinguishable from a bad
    # lowering without another build.
    require_ordered(
        adapter,
        [
            "BOXEDWINE_FEX64_HOST_FAULT_CONTAINED",
            "BOXEDWINE_FEX64_FAULT_STATE",
            "REG_RAX",
            "REG_RSP",
            "REG_R12",
            "fs_cached",
            "gs_cached",
            "BOXEDWINE_FEX64_FAULT_HOSTREGS",
        ],
        "BoxedVN contained-fault guest state report",
    )

    print("FEX exit-dispatch and loader-boundary contracts verified")


if __name__ == "__main__":
    main()
