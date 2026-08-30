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

    # ------------------------------------------------------------------ #
    # Canonical low guest addresses served through a high host alias.      #
    # Wine's TEB reservation is below 2 GiB and cannot be host-mapped at    #
    # its own address, so it is dereferenced through the alias instead.     #
    # ------------------------------------------------------------------ #
    alias_header = read(repository / "include/guest_low_alias.h")
    alias_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-low-address-alias.patch"
    )
    kmemory64_header_text = read(repository / "include/kmemory64.h")
    kmemory64_text = read(repository / "source/kernel/kmemory64.cpp")
    backend_text = read(repository / "ios/runtime/src/BVNFEXBackend.mm")

    # The translation must be a single OR, which requires the base to be
    # aligned to the limit and every high address to already carry its bits.
    require_ordered(
        alias_header,
        [
            "kGuestLowLimit = 0x200000000ULL",
            "kGuestLowAliasBase = 0x7800000000ULL",
            "kGuestHighBase = kGuestLowAliasEnd",
            "static_assert((kGuestLowAliasBase & (kGuestLowLimit - 1)) == 0,",
            "static_assert((kGuestHighBase & kGuestLowAliasBase) == "
            "kGuestLowAliasBase,",
            "guestToHostAddress(",
            "return guestAddress | kGuestLowAliasBase;",
            "hostToGuestAddress(",
            "guestRangeHostable(",
        ],
        "BoxedVN low guest alias contract",
    )

    # The address space layout and the shared arithmetic must agree.
    require_ordered(
        kmemory64_header_text,
        [
            "#define K64_NATIVE_LOW_ALIAS_BASE",
            "#define K64_NATIVE_LOW_GUEST_LIMIT",
            "#define K64_NATIVE_GUEST_IMAGE_BASE   K64_NATIVE_LOW_ALIAS_END",
            '#include "guest_low_alias.h"',
            "K64_NATIVE_LOW_ALIAS_BASE == boxedvn::kGuestLowAliasBase",
            "K64_NATIVE_GUEST_IMAGE_BASE == boxedvn::kGuestHighBase",
            "k64GuestToHostAddress(",
        ],
        "BoxedVN aliased native layout",
    )

    # Every native host dereference derives its address through the alias, and
    # the canonical low range is admitted so Wine's reservation can be placed.
    require_ordered(
        kmemory64_text,
        [
            "bool KMemory64::nativeGuestRangeAllowed(",
            "if (end <= K64_NATIVE_LOW_GUEST_LIMIT) {",
            "return addr >= K64_NATIVE_GUEST_IMAGE_BASE &&",
            "end <= K64_NATIVE_GUEST_HIGH_END;",
            "const U64 hostAddr = k64GuestToHostAddress(addr);",
            "::memset((void*)(uintptr_t)hostAddr, 0, (size_t)len);",
        ],
        "BoxedVN aliased native mapping",
    )
    if "k64GuestToHostAddress(guestPageAddress)" not in kmemory64_text:
        raise SystemExit(
            "guest pages must adopt their aliased host backing, not their "
            "canonical address"
        )
    # KUSER_SHARED_DATA must resolve through the same alias as every other low
    # address rather than keeping a second, independent backing.
    kuser_start = kmemory64_text.index("static bool k64IsKuserRange(")
    kuser_end = kmemory64_text.index(chr(10) + "}" + chr(10), kuser_start)
    if "return false;" not in kmemory64_text[kuser_start:kuser_end]:
        raise SystemExit(
            "KUSER_SHARED_DATA must fold into the low alias rather than "
            "retaining a second host backing for 0x7ffe0000"
        )

    # The translator is told about the alias before any guest code compiles.
    require_ordered(
        backend_text,
        [
            '#include "guest_low_alias.h"',
            "bundle->context->SetGuestLowAlias(boxedvn::kGuestLowAliasBase,",
            "boxedvn::kGuestLowLimit);",
            "BOXEDWINE_FEX64_LOW_ALIAS",
            "if (!bundle->context->InitCore()) {",
        ],
        "BoxedVN translator alias arming",
    )

    # The downstream patch applies the translation only at host dereference
    # boundaries, and encodes it as one logical-immediate OR.
    require_ordered(
        alias_patch,
        [
            "a/FEXCore/Source/Interface/Context/Context.h",
            "+  void SetGuestLowAlias(uint64_t Base, uint64_t Limit) override {",
            "+    GuestLowAliasBase = Base;",
            "+  uint64_t GuestToHostAddress(uint64_t GuestAddress) const {",
            "+    return GuestAddress | GuestLowAliasBase;",
            "a/FEXCore/Source/Interface/Core/JIT/JITClass.h",
            "+  ARMEmitter::Register AliasGuestAddress(ARMEmitter::Register Base);",
            "a/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp",
            "+ARMEmitter::Register Arm64JITCore::AliasGuestAddress(",
            "+  if (AliasBase == 0) {",
            "+    return Base;",
            "+  orr(ARMEmitter::Size::i64Bit, TMP4, Base, AliasBase);",
            "a/FEXCore/include/FEXCore/Core/Context.h",
            "+  FEX_DEFAULT_VISIBILITY virtual void SetGuestLowAlias(",
        ],
        "BoxedVN translator low-address alias patch",
    )
    # An unarmed alias must leave every emitted address untouched, so the
    # patch is inert until the runtime publishes a base.
    alias_start = alias_patch.index("+ARMEmitter::Register Arm64JITCore::AliasGuestAddress(")
    alias_body = alias_patch[alias_start:alias_start + 600]
    if "AliasBase == 0" not in alias_body:
        raise SystemExit(
            "the translator alias must be inert when no base is published"
        )

    for label, text, fragment in (
        ("iOS translator build", fex_build_script,
         "apply_patch fex-boxedwine-low-address-alias.patch"),
        ("VIXL probe", vixl_probe,
         "fex-boxedwine-low-address-alias.patch"),
    ):
        if fragment not in text:
            raise SystemExit(
                f"{label} does not apply the low-address alias patch"
            )

    # ------------------------------------------------------------------ #
    # The correctness probe must exercise the alias it validates: a         #
    # canonical low image, mapped by KMemory64 in native-identity mode and  #
    # written through the translated host backing. The previous probe used  #
    # an iOS-selected mmap pointer as the guest address, which had nothing  #
    # mapped behind it once instruction fetch started translating.          #
    # ------------------------------------------------------------------ #
    require_ordered(
        backend_text,
        [
            "constexpr uint64_t kProbeGuestImageBase =",
            "constexpr uint64_t kProbeGuestStackBase =",
            "probe image must stay inside the canonical low range",
            "probe image must clear KUSER_SHARED_DATA",
            "const uint64_t guestImageBase = kProbeGuestImageBase;",
            "std::make_unique<KMemory64>(nullptr, /*nativeIdentity*/ true)",
            "gProbeMemory->mmapAnonymousFixed(",
            "boxedvn::guestToHostAddress(guestImageBase)",
            "boxedvn::guestToHostAddress(guestStackBase)",
            "std::memcpy(hostImage + destinationOffset,",
            "gGuestEntry = guestImageBase + entryOffset;",
            "gProbeCPU->reg[X64_RSP].setU64(guestStackBase + kGuestStackBytes",
            "BOXEDWINE_FEX64_PROBE_MAP",
        ],
        "BoxedVN correctness probe canonical mapping",
    )
    probe_start = backend_text.index("bool mapBundledELFProbe() {")
    probe_end = backend_text.index(chr(10) + "}" + chr(10), probe_start)
    probe_body = backend_text[probe_start:probe_end]
    if "mmap(nullptr" in probe_body:
        raise SystemExit(
            "the correctness probe must not take an iOS-selected host pointer "
            "as its guest address"
        )

    # Translated paths that carry no guest-visible writeback.
    require_ordered(
        alias_patch,
        [
            "a/FEXCore/Source/Interface/Core/JIT/AtomicOps.cpp",
            "+  auto MemSrc = AliasGuestAddress(GetReg(Op->Addr));",
            "a/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp",
            "DEF_OP(LoadMemPair) {",
            "+  const auto Addr = AliasGuestAddress(GetReg(Op->Addr));",
            "DEF_OP(StoreMemPair) {",
            "+  const auto Addr = AliasGuestAddress(GetReg(Op->Addr));",
            "+  if (const uint64_t AliasBase = CTX->GetGuestLowAliasBase()) {",
            "+    orr(ARMEmitter::Size::i64Bit, TMP2, TMP2, AliasBase);",
        ],
        "BoxedVN translated pair, string and atomic paths",
    )
    if alias_patch.count("+  auto MemSrc = AliasGuestAddress(GetReg(Op->Addr));") != 10:
        raise SystemExit(
            "every atomic and locked operation must translate its address"
        )
    # The stack write-back paths are deliberately not translated yet, so no
    # pre/post-indexed stack form may have been rewritten.
    for forbidden in ("IndexType::PRE>(Src1.W(), Src2.W(), StoreAddr",
                      "ldurb(Dst.W(), LoadAddr"):
        if forbidden in alias_patch:
            raise SystemExit(
                "stack write-back translation is not part of this change; the "
                "probe and guest stack lanes are high identity addresses"
            )

    # ------------------------------------------------------------------ #
    # The decoder's two address domains. InstStream feeds the executable   #
    # range query and must stay canonical; AdjustedInstStream is the host   #
    # pointer whose bytes are read. Collapsing them made the query run in   #
    # host space, where no guest range is registered, and every entry block #
    # decoded as NoExec.                                                    #
    # ------------------------------------------------------------------ #
    frontend = read(fex / "FEXCore/Source/Interface/Core/Frontend.cpp")
    require_ordered(
        frontend,
        [
            "std::optional<uint8_t> Decoder::PeekByte(",
            "reinterpret_cast<uint64_t>(InstStream.InstStream",
            "CheckRangeExecutable(ByteAddress, 1)",
            "return InstStream.AdjustedInstStream[",
        ],
        "pinned FEX decoder address domains",
    )

    # Core.cpp must hand the decoder a CANONICAL entry stream. The broken form
    # passed the translated pointer, which put both fields in host space.
    for broken in (
        "CheckIfCacheable(Thread, reinterpret_cast<const uint8_t*>("
        "GuestToHostAddress(GuestRIP))",
        "GuestCode = reinterpret_cast<const uint8_t*>("
        "GuestToHostAddress(GuestRIP));",
    ):
        if broken in alias_patch:
            raise SystemExit(
                "the decoder must be entered in canonical guest space; "
                "translating the entry stream puts the executable-range query "
                "in the wrong domain and every entry block decodes as NoExec"
            )
    require_ordered(
        alias_patch,
        [
            "a/FEXCore/Source/Interface/Core/Core.cpp",
            "+    // The decoder is entered in CANONICAL guest space.",
            "     GuestCode = reinterpret_cast<const uint8_t*>(GuestRIP);",
            "+          const uint8_t* InstBytes = reinterpret_cast<const "
            "uint8_t*>(GuestToHostAddress(InstAddress));",
            "a/FEXCore/Source/Interface/Core/Frontend.cpp",
            "+  const uint8_t* const CanonicalInstStream = _InstStream - "
            "EntryPoint + RIP;",
            "+  const uint8_t* const HostInstStream = reinterpret_cast<const "
            "uint8_t*>(",
            "+    CTX->GuestToHostAddress(reinterpret_cast<uint64_t>("
            "CanonicalInstStream)));",
            "BOXEDWINE_FEX64_FETCH_MAP",
            "+    .InstStream = CanonicalInstStream,",
            "+    .AdjustedInstStream = HostInstStream,",
        ],
        "BoxedVN decoder canonical and host stream split",
    )
    # The VSyscall region keeps its own dedicated backing.
    require_ordered(
        frontend,
        [
            "VSyscall",
            ".InstStream = _InstStream - EntryPoint + RIP,",
            ".AdjustedInstStream = VSyscallData + Offset,",
        ],
        "pinned FEX VSyscall stream mapping",
    )

    # The probe proves the translator's question can be answered before it is
    # asked, using the canonical address the translator will use.
    require_ordered(
        backend_text,
        [
            "BOXEDWINE_FEX64_PROBE_MAP",
            "gAddressSpace.executableRange(gGuestEntry)",
            "BOXEDWINE_FEX64_PROBE_ENTRY_UNMAPPED",
            "entryRange->contains(gGuestEntry)",
            "boxedvn::guestToHostAddress(entryRange->guestBase)",
            "BOXEDWINE_FEX64_PROBE_ENTRY_MISMATCH",
            "BOXEDWINE_FEX64_PROBE_ENTRY entry=",
        ],
        "BoxedVN probe entry executability assertion",
    )

    # ------------------------------------------------------------------ #
    # No JIT or translator probe may run on the main thread: both wait on   #
    # a semaphore for their timeout, and the main thread is the only one    #
    # servicing UIKit's run loop.                                          #
    # ------------------------------------------------------------------ #
    runtime = read(repository / "ios/runtime/src/BVNRuntime.mm")
    preflight_header = read(
        repository / "ios/support/include/boxedvn/launch_preflight.h"
    )
    require_ordered(
        preflight_header,
        [
            "enum class LaunchPreflightOutcome",
            "enum class LaunchPreflightAction",
            "class LaunchPreflight",
            "LaunchPreflightAction complete(",
            "if (currentGeneration != generation_) {",
            "return LaunchPreflightAction::Ignore;",
            "bool claimCleanup() noexcept {",
        ],
        "BoxedVN launch preflight sequencing",
    )
    require_ordered(
        runtime,
        [
            'boxedvn::LaunchPreflightOutcome runLaunchPreflight(',
            "if ([NSThread isMainThread]) {",
            "BOXEDWINE_PREFLIGHT_MISPLACED",
            "probeJitWithTimeout();",
            "probeFexWithTimeout();",
            'extern "C" bool BVNRuntimeRequestLaunch(',
            "preflight->begin(generation);",
            "BOXEDWINE_PREFLIGHT_BEGIN",
            "dispatch_async(preflightQueue(), ^{",
            "runLaunchPreflight(launch.useFEX64, preflightError);",
            "dispatch_async(dispatch_get_main_queue(), ^{",
            "BOXEDWINE_PREFLIGHT_COMPLETE",
            "preflight->complete(",
            "boxedvn::LaunchPreflightAction::Ignore",
            "boxedvn::LaunchPreflightAction::Fail",
            "if (!preflight->claimCleanup()) return;",
            "runSession(launch);",
            "releaseGuestPresentation();",
        ],
        "BoxedVN asynchronous launch preflight",
    )
    # runSession runs on the main thread and must contain no probe wait.
    session_start = runtime.index("void runSession(const BVNLaunchConfiguration& launch) {")
    session_end = runtime.index(chr(10) + "}" + chr(10), session_start)
    session_body = runtime[session_start:session_end]
    for forbidden in ("probeJitWithTimeout()", "probeFexWithTimeout()",
                      "dispatch_semaphore_wait"):
        if forbidden in session_body:
            raise SystemExit(
                f"runSession runs on the main thread and must not call "
                f"{forbidden}: it would stop UIKit for the whole timeout"
            )
    # The presentation must not be torn down merely because preflight was
    # scheduled; the release belongs to the single completion path.
    launch_start = runtime.index('extern "C" bool BVNRuntimeRequestLaunch(')
    launch_body = runtime[launch_start:]
    if launch_body.count("SDL_iPhoneSetEventPump(SDL_FALSE)") != 1:
        raise SystemExit(
            "the event pump and presentation must be released through one "
            "claimed cleanup path, not on every early return"
        )

    # ------------------------------------------------------------------ #
    # Every guest-address dereference that bypasses the operand funnels.   #
    # movlpd/movhpd lower to VLoadVectorElement, which reads through the    #
    # raw address register; with a canonical low address that reached an    #
    # unmapped host page and the correctness probe stalled inside the SSE2  #
    # strcmp loop with no further block, fault or exit.                     #
    # ------------------------------------------------------------------ #
    memory_ops_text = read(
        fex / "FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp"
    )
    bypassing_ops = (
        "LoadMemTSO", "StoreMemTSO",
        "VLoadVectorElement", "VStoreVectorElement",
        "LoadMemX87SVEOptPredicate", "StoreMemX87SVEOptPredicate",
        "CacheLineClear", "CacheLineClean", "CacheLineZero", "Prefetch",
        "VLoadNonTemporal", "VStoreNonTemporal", "VStoreNonTemporalPair",
    )
    for name in bypassing_ops:
        if ("DEF_OP(%s)" % name) not in memory_ops_text:
            raise SystemExit(
                "the pinned translator no longer defines %s; the alias "
                "coverage list must be re-derived" % name
            )
        if ("+  const auto MemReg = AliasGuestAddress(GetReg(Op->Addr));"
                not in alias_patch and
                "+  auto MemReg = AliasGuestAddress(GetReg(Op->Addr));"
                not in alias_patch):
            raise SystemExit(
                "the address these ops dereference directly must be translated"
            )
    aliased = alias_patch.count(
        "MemReg = AliasGuestAddress(GetReg(Op->Addr));")
    if aliased < len(bypassing_ops):
        raise SystemExit(
            "expected every op that dereferences the raw address register to "
            "be translated; found %d of %d" % (aliased, len(bypassing_ops))
        )

    # The stack write-back forms stay untranslated on purpose: both the probe
    # stack and the guest stack lane are high identity addresses, and rewriting
    # a pre/post-indexed form would put a translated address into the guest's
    # stack pointer.
    for forbidden in ("AliasGuestAddress(AddrSrc)",
                      "IndexType::PRE>(Src.W(), StoreAddr"):
        if forbidden in alias_patch:
            raise SystemExit(
                "stack write-back translation is out of scope; it would make "
                "the alias visible in guest architectural state"
            )

    # The probe retires one guest instruction per block so the bounded block
    # trace names the exact last instruction that executed. Compilation
    # "leave" is not execution proof.
    require_ordered(
        backend_text,
        [
            "if (!mapGuestProbe()) {",
            "FEXCore::Config::ConfigOption::CONFIG_MAXINST",
            "BOXEDWINE_FEX64_PROBE_TRACE maxinst=1 scope=probe",
            "struct ProbeInstructionLimitScope final {",
            "FEXCore::Config::Erase(",
            "gProbeContext = createFEXContext(",
        ],
        "BoxedVN probe instruction-level retirement trace",
    )

    # ------------------------------------------------------------------ #
    # Nothing that belongs to a guest session may start before preflight   #
    # succeeds. Rotating the scene, holding the refresh rate, enabling      #
    # SDL's pump and preparing DXMT were all happening with no SDL guest    #
    # window in existence, and the main queue then serviced a queued block  #
    # seconds late.                                                        #
    # ------------------------------------------------------------------ #
    launch_start = runtime.index('extern "C" bool BVNRuntimeRequestLaunch(')
    launch_body = runtime[launch_start:runtime.index(
        'extern "C" bool BVNRuntimeRequestShutdown', launch_start)]
    fail_branch = launch_body.index("boxedvn::LaunchPreflightAction::Fail")
    for guest_facility in ("BVNPrepareGuestPresentation(",
                           "SDL_iPhoneSetEventPump(SDL_TRUE)",
                           "BVNDXMTDisplayPrepare("):
        if guest_facility not in launch_body:
            raise SystemExit(
                "%s must still be reachable on the success path" % guest_facility
            )
        if launch_body.index(guest_facility) < fail_branch:
            raise SystemExit(
                "%s belongs to a guest session and must not run before "
                "preflight succeeds" % guest_facility
            )

    require_ordered(
        runtime,
        [
            "constexpr uint64_t kMainStallThresholdMilliseconds = 250;",
            "dispatch_queue_t preflightQueue() {",
            "QOS_CLASS_UTILITY",
            "void reportMainThreadStall(",
            "thread_get_state(thread, ARM_THREAD_STATE64,",
            "dladdr(reinterpret_cast<const void*>(pc), &pcInfo)",
            "BOXEDWINE_MAIN_STALL latency_ms=",
            "BOXEDWINE_PREFLIGHT_MAIN_SENTINEL generation=",
        ],
        "BoxedVN main-thread availability instrumentation",
    )
    require_ordered(
        launch_body,
        [
            "preflight->begin(generation);",
            "const uint64_t sentinelStart = monotonicMilliseconds();",
            "dispatch_async(dispatch_get_main_queue(), ^{",
            "reportMainThreadStall(latency);",
            "BOXEDWINE_PREFLIGHT_BEGIN",
            "dispatch_async(preflightQueue(), ^{",
            "BOXEDWINE_PREFLIGHT_WORKER_BEGIN",
            "runLaunchPreflight(launch.useFEX64, preflightError);",
            "BOXEDWINE_PREFLIGHT_WORKER_END",
            "BOXEDWINE_PREFLIGHT_COMPLETE",
            "boxedvn::LaunchPreflightAction::Fail",
            "BVNPrepareGuestPresentation(^{",
            "SDL_iPhoneSetEventPump(SDL_TRUE);",
            "BVNDXMTDisplayPrepare(",
            "runSession(launch);",
        ],
        "BoxedVN library-mode preflight ordering",
    )
    # The failure path acquired nothing, so it must release nothing.
    fail_to_success = launch_body[fail_branch:launch_body.index(
        "BVNPrepareGuestPresentation(")]
    for forbidden in ("SDL_iPhoneSetEventPump(SDL_FALSE)",
                      "BVNFinishGuestPresentation()"):
        if forbidden in fail_to_success:
            raise SystemExit(
                "the preflight failure path must not release a guest facility "
                "it never acquired"
            )

    # ------------------------------------------------------------------ #
    # The alias belongs on the COMPLETE effective address. A guest          #
    # `mov [r10 + r14], rax` with a canonical low base and a high index     #
    # computed alias(base) + index, which lands in neither half of the      #
    # address space; the loader faulted at exactly that address.            #
    # ------------------------------------------------------------------ #
    require_ordered(
        alias_patch,
        [
            "ARMEmitter::ExtendedMemOperand Arm64JITCore::GenerateMemOperand(",
            "+  const uint64_t AliasBase = CTX->GetGuestLowAliasBase();",
            "+      if (AliasBase != 0) {",
            "+        const uint32_t Shift = FEXCore::ilog2(OffsetScale);",
            "+          add(ARMEmitter::Size::i64Bit, TMP4, Base, RegOffset, "
            "ARMEmitter::ExtendedType::SXTX, Shift);",
            "+        orr(ARMEmitter::Size::i64Bit, TMP4, TMP4, AliasBase);",
            "+        return ARMEmitter::ExtendedMemOperand(TMP4, "
            "ARMEmitter::IndexType::OFFSET, 0);",
        ],
        "BoxedVN complete effective address before translation",
    )
    # The canonical ADD must precede the alias ORR, never the reverse.
    add_at = alias_patch.index(
        "add(ARMEmitter::Size::i64Bit, TMP4, Base, RegOffset,")
    orr_at = alias_patch.index(
        "orr(ARMEmitter::Size::i64Bit, TMP4, TMP4, AliasBase);", add_at)
    if not add_at < orr_at:
        raise SystemExit(
            "the effective address must be completed in canonical space "
            "before the host alias is applied"
        )
    # Every extension form has to be covered, or a UXTW/SXTW index would fall
    # back to the broken ordering.
    for extension in ("SXTX", "UXTW", "SXTW"):
        if ("add(ARMEmitter::Size::i64Bit, TMP4, Base, RegOffset, "
                "ARMEmitter::ExtendedType::%s, Shift);" % extension
                not in alias_patch):
            raise SystemExit(
                "the %s indexed form must complete its address canonically"
                % extension
            )
    # The SVE operand carries the same rule.
    require_ordered(
        alias_patch,
        [
            "Arm64JITCore::GenerateSVEMemOperand(",
            "+  const uint64_t SVEAliasBase = CTX->GetGuestLowAliasBase();",
            "+  const ARMEmitter::Register CanonicalBase = Base;",
            "+      add(ARMEmitter::Size::i64Bit, TMP4, CanonicalBase, TMP1);",
            "+    add(ARMEmitter::Size::i64Bit, TMP4, CanonicalBase, RegOffset);",
        ],
        "BoxedVN complete SVE effective address before translation",
    )

    # The device probe must execute the faulting operand shape itself, before
    # Wine is admitted, and fail with its own exit code.
    kernel_probe = read(
        repository / "scripts/guest-probes/fex64-kernel-probe.S"
    )
    require_ordered(
        kernel_probe,
        [
            "_start:",
            "call indexed_alias_probe",
            "indexed_alias_failed:",
            "mov $48, %rdi",
            "indexed_alias_probe:",
            "mov %rsp, %rax",
            "and $-4096, %r14",
            "and $4095, %r10",
            "mov %rdx, (%r10,%r14)",
            "mov (%r10,%r14), %rcx",
            "jne indexed_alias_failed",
            "mov %rsi, (%r14,%r10)",
            "mov %r8, (%r14,%r11,8)",
            "mov %r9, 0x40(%r10,%r14)",
        ],
        "BoxedVN device indexed-alias probe",
    )
    alias_probe_start = kernel_probe.index("indexed_alias_probe:")
    alias_probe_body = kernel_probe[alias_probe_start:
                                    kernel_probe.index("ret", alias_probe_start)]
    if "movabs $0x7a" in alias_probe_body:
        raise SystemExit(
            "the indexed-alias probe must derive both address components from "
            "a real mapped address; a hardcoded guest address cannot run "
            "natively, where the packaged probe is validated"
        )
    probe_body = kernel_probe[kernel_probe.index("_start:"):]
    if probe_body.index("call indexed_alias_probe") > probe_body.index(
            "call strcmp_vector_probe"):
        raise SystemExit(
            "the indexed-alias contract must be proven before anything else "
            "the translator reports about memory is trusted"
        )
    # A stale packaged probe must not satisfy the gate.
    build_ios = read(repository / "scripts/build-ios.sh")
    marker = "BoxedWine FEX64 SSE2/REP STOS/indexed-alias/call-ret PASS"
    if marker not in kernel_probe or marker not in build_ios:
        raise SystemExit(
            "the packaged-probe staleness gate must name the current probe "
            "contract"
        )

    # CI proves the same operand shape through the real ARM64 JIT.
    indexed_fixture = read(
        repository / "scripts/guest-probes/fex64-indexed-alias-contract.asm"
    )
    require_ordered(
        indexed_fixture,
        [
            "times 0x0fc - ($ - $$) db 0x90",
            "indexed_alias_target:",
            "mov     [r10 + r14], rax",
            "mov     rcx, [r10 + r14]",
            "mov     [r14 + r10], rsi",
            "mov     [r14 + r11*8], r8",
            "mov     [r10 + r14 + 0x40], rdx",
        ],
        "BoxedVN indexed-alias VIXL fixture",
    )
    require_ordered(
        vixl_probe,
        [
            'prepare_fixture "${fixture_indexed_alias}" fex64-indexed-alias',
            "run_one x64-indexed-alias ",
            '"${tmp_dir}/fex64-indexed-alias.config.bin" 1 0',
            '"${tmp_dir}/fex64-indexed-alias.config.bin" 500 0',
            '"${tmp_dir}/fex64-indexed-alias.config.bin" 500 1',
            "run_one x64-indexed-alias-ircap ",
            "--label indexed-alias",
        ],
        "VIXL indexed-alias regression",
    )

    # Both caches must see the new inputs, or a restored artifact could hide
    # the change.
    workflow = read(repository / ".github/workflows/build-ios.yml")
    for cached in ("scripts/guest-probes/fex64-indexed-alias-contract.asm",
                   "scripts/guest-probes/fex64-kernel-probe.S"):
        if workflow.count("'%s'" % cached) < 1:
            raise SystemExit(
                "%s must participate in the workflow cache keys" % cached
            )

    print("FEX exit-dispatch and loader-boundary contracts verified")


if __name__ == "__main__":
    main()
