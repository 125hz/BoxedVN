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
            "const uint64_t pendingIRCapTarget = firstLiveProcess",
            "? BVNFEXBackendTakePendingIRCapTarget(",
            "->commandLine.c_str())",
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
            # Occupancy is the only thing that may be reported as EEXIST.
            "if (!request.exactRangeUnmapped) {",
            "return GuestMmapPlacement::FailExists;",
            "if (request.nativeIdentity && !request.exactRangeAllowed) {",
            "return GuestMmapPlacement::ReserveSparse;",
            # An EMPTY range whose address cannot be hosted is -ENOMEM, never
            # -EEXIST. A placement search answers EEXIST by stepping one
            # granule and asking again, so a phantom occupant turns a single
            # refusal into a walk across the whole unhostable region: the
            # device shows 4,194,305 mmaps, 4,193,283 outside every hostable
            # lane, 4,194,054 refused, with the guest RIP never leaving the
            # syscall in the libc mmap wrapper.
            "return GuestMmapPlacement::FailNoMemory;",
            "return GuestMmapPlacement::MapExactNoReplace;",
            "if (request.fixed) {",
            "request.protection == 0",
            "return GuestMmapPlacement::FailNoMemory;",
        ],
        "BoxedVN guest mmap placement policy",
    )
    fixed_no_replace_start = placement_header.index(
        "if (request.fixedNoReplace) {")
    fixed_no_replace_body = placement_header[
        fixed_no_replace_start:placement_header.index(
            "if (request.fixed) {", fixed_no_replace_start)]
    if fixed_no_replace_body.count("FailExists") != 1:
        raise SystemExit(
            "MAP_FIXED_NOREPLACE may report -EEXIST for exactly one reason: "
            "the range is occupied"
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
            # Occupancy first: a range that is both occupied and unhostable is
            # reported as occupied, which is the stronger, more specific fact.
            "if (!rangeCompletelyUnmapped(addr, mapLen) ||",
            "sparseReservationOverlaps(addr, mapLen)) {",
            "return (U64)-K_EEXIST;",
            # An empty range the address space cannot host is -ENOMEM.
            "nativeIdentityMode() && !nativeGuestRangeAllowed(addr, mapLen)",
            "return (U64)-K_ENOMEM;",
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
            "gGuestMmapDetailBudget.take(signature.processId,",
            "gGuestMmapRepeats.record(signature);",
            "reportGuestMmapRequest(cpu, reportKind, signature, request,",
        ],
        "BoxedVN anonymous mmap placement routing",
    )
    # Every reported request names the address space, the guest RIP, the
    # arguments, the derived facts, the placement and the repeat count, so one
    # line is enough to act on.
    for field in ("BOXEDWINE_X64_MMAP_%s pid=", "space=%llu", "rip=0x%llx",
                  "caller=0x%llx", "sparse_overlap=%d", "sparse_count=%llu",
                  "placement=%s", "errno=%lld", "repeat=%llu"):
        if field not in syscall64:
            raise SystemExit(
                "the bounded mmap report must carry " + field
            )
    require_ordered(
        syscall64,
        [
            "static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, "
            "U64 prot,",
            "const bool fixedNoReplace = (flags & K_MAP_FIXED_NOREPLACE) != 0;",
            "if (fixedNoReplace) {",
            "cpu->memory->rangeCompletelyUnmapped(",
            # Same distinction as the anonymous path: occupied is EEXIST,
            # empty-but-unhostable is ENOMEM.
            "hintUnmapped ? (U64)-K_ENOMEM : (U64)-K_EEXIST;",
            "BOXEDWINE_X64_MMAP_FILE_REFUSE pid=",
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
            # The OR still serves the low and identity lanes; the mask
            # is a no-op on both and relocates only the top arena.
            "return (guestAddress | kGuestLowAliasBase) & ~kGuestTopClearMask;",
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
            "if (addr >= K64_NATIVE_GUEST_IMAGE_BASE &&",
            "end <= K64_NATIVE_GUEST_HIGH_END) {",
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
            "+    return (GuestAddress | GuestLowAliasBase) & ~GuestTopClearMask;",
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

    # ------------------------------------------------------------------ #
    # A fork child that execs unwinds the whole inherited process before it #
    # can load its replacement image. When that stalls the child produces    #
    # nothing at all and its parent waits in wait4 forever, which is the     #
    # correct thing for the parent to do. Every phase announces itself.      #
    # ------------------------------------------------------------------ #
    kprocess = read(repository / "source/kernel/kprocess.cpp")
    kthread = read(repository / "source/kernel/kthread.cpp")
    exec_header = read(
        repository / "ios/support/include/boxedvn/exec_transition.h"
    )

    require_ordered(
        kprocess,
        [
            "void KProcess::logExecPhase(const char* phase) const noexcept {",
            "BOXEDWINE_X64_EXEC_PHASE pid=%u phase=%s",
            "void KProcess::onExec(KThread* thread) {",
            'logExecPhase("onexec-begin");',
            'logExecPhase("cloexec-begin");',
            'logExecPhase("cloexec-end");',
            'logExecPhase("shm-begin");',
            'logExecPhase("shm-end");',
            'logExecPhase("retire-mapped-begin");',
            "this->retireAllMappedFiles();",
            'logExecPhase("retire-mapped-end");',
            'logExecPhase("signals-begin");',
            'logExecPhase("signals-end");',
            'logExecPhase("siblings-begin");',
            "terminateOtherThread(shared_from_this(), otherThread->id);",
            'logExecPhase("siblings-end");',
            'logExecPhase("onexec-end");',
        ],
        "BoxedVN exec phase trace",
    )
    require_ordered(
        kprocess,
        [
            "BOXEDWINE_X64_EXEC_REMAP",
            'logExecPhase("thread-reset-begin");',
            "thread->reset();",
            'logExecPhase("thread-reset-end");',
            "this->onExec(thread);",
            'logExecPhase("loader-begin");',
            "ElfLoader::loadProgram(thread, openNode",
            'logExecPhase("loader-end");',
        ],
        "BoxedVN exec call-site phase order",
    )

    # A 64-bit process never runs on the legacy stack: the loader builds its
    # own, and execve already skips setupThreadStack for it. Building one
    # anyway mmaps and then touches sixteen pages of an address space that,
    # for a fork child, is a fresh clone of the parent's.
    require_ordered(
        kthread,
        [
            "void KThread::reset() {",
            "memory->threadCleanup(id);",
            "this->clearFutexes();",
            "if (this->process && this->process->is64Bit) {",
            "return;",
            "this->setupStack();",
        ],
        "BoxedVN 64-bit exec reset path",
    )
    reset_start = kthread.index("void KThread::reset() {")
    reset_end = kthread.index(chr(10) + "}" + chr(10), reset_start)
    reset_body = kthread[reset_start:reset_end]
    if reset_body.index("is64Bit") > reset_body.index("this->setupStack();"):
        raise SystemExit(
            "the legacy stack must not be built for a 64-bit process; "
            "ElfLoader64 owns that stack"
        )
    for retained in ("memory->threadCleanup(id);", "this->clearFutexes();",
                     "this->cpu->reset();", "this->setupStack();"):
        if retained not in reset_body:
            raise SystemExit(
                "the 32-bit reset path must keep %r" % retained
            )

    require_ordered(
        exec_header,
        [
            "enum class ExecPhase",
            "ThreadReset",
            "CloseOnExec",
            "RetireMappedFiles",
            "SiblingThreads",
            "Loader",
            "enum class ExecTransitionResult",
            "Stalled",
            "struct ExecReplacementImage",
            "bool crossLinked() const noexcept",
            "bool belongsToImage(",
            "ExecTransitionResult finish() noexcept",
        ],
        "BoxedVN exec transition contract",
    )

    # ------------------------------------------------------------------ #
    # A 64-bit process must not exec a script. The interpreter resolves     #
    # through the 32-bit loader, ElfLoader takes its ELF32 path while the    #
    # process is still marked 64-bit, and loading reports success without    #
    # installing a usable image.                                            #
    # ------------------------------------------------------------------ #
    require_ordered(
        kprocess,
        [
            "BOXEDWINE_X64_EXEC pid=%u path=%s fex=%u native=%u",
            "BOXEDWINE_X64_EXEC_RESOLVE pid=%u target=%s kind=%s ",
            'interpreter.length() ? "script" : "elf",',
            "if (interpreter.length()) {",
            "BOXEDWINE_X64_EXEC_RESOLVE_INVALID pid=%u target=%s ",
            "return -K_ENOEXEC;",
        ],
        "BoxedVN 64-bit exec resolution invariant",
    )

    # The loader, server and module trees must share one canonical root. Both
    # server names there have to be the same real x86-64 executable; the
    # distro also ships a /bin/sh wrapper under the generic name.
    wine_builder = read(repository / "scripts/build-wine64-runtime-ci.sh")
    wine_validator = read(repository / "scripts/validate-wine64-runtime.sh")
    require_ordered(
        wine_builder,
        [
            'WINE_MODULE_ROOT="/usr/lib/x86_64-linux-gnu/wine"',
            "is_elf64_x86_64() {",
            "find_first_elf64_x86_64() {",
            'WINE_SERVER="$(find_first_elf64_x86_64',
            'copy_as "${WINE64}" "${WINE_MODULE_ROOT}/wine64"',
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver64"',
            'copy_as "${WINE_SERVER}" "${WINE_MODULE_ROOT}/wineserver"',
            'guest_link "${WINE_MODULE_ROOT}/wine64" /usr/lib/wine/wine64',
        ],
        "BoxedVN wineserver packaging",
    )
    if 'copy_as "${WINE_ROOT}/wineserver"' in wine_builder:
        raise SystemExit(
            "the distro shell wrapper must not be packaged as the guest "
            "wineserver"
        )
    require_ordered(
        wine_validator,
        [
            "check_zip_entry_elf64_x86_64() {",
            "is not an ELF file",
            "is not ELFCLASS64",
            "is not EM_X86_64",
            "check_zip_entry_pe32plus_amd64() {",
            "check_zip_guest_link() {",
            "WINE_MODULE_ROOT=usr/lib/x86_64-linux-gnu/wine",
            'check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver64"',
            'check_zip_path "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver"',
            'check_zip_entry_elf64_x86_64 "${WINE_ARCHIVE}" "${WINE_MODULE_ROOT}/wineserver"',
            'wineserver_generic_sha=',
            'wineserver64_sha=',
            "for required_builtin in ntdll.dll kernel32.dll kernelbase.dll",
        ],
        "BoxedVN wineserver archive validation",
    )

    # ------------------------------------------------------------------ #
    # Wine's server puts its socket directory in the modelled user's XDG      #
    # runtime directory. ZIP entries carry no Unix mode, so the directory     #
    # arrived read-only and wineserver died on mkdir. The write exception     #
    # must stay exactly that one subtree.                                     #
    # ------------------------------------------------------------------ #
    runtime_dir_header = read(repository / "include/x64_runtime_dir.h")
    fsfilenode = read(repository / "source/io/fsfilenode.cpp")
    require_ordered(
        runtime_dir_header,
        [
            '#define K_X64_MODELLED_UID 1000',
            '#define K_X64_USER_RUNTIME_DIR "/run/user/1000"',
            '#define K_X64_USER_RUNTIME_DIR_MODE 0700',
            'inline bool isX64UserRuntimePath(',
            "return path[index] == 0 || path[index] == '/';",
        ],
        "BoxedVN guest runtime directory contract",
    )
    require_ordered(
        fsfilenode,
        [
            '#include "x64_runtime_dir.h"',
            'U32 FsFileNode::getMode() {',
            'bool isUserRuntimePath = boxedvn::isX64UserRuntimePath(this->path.c_str());',
            'isUserRuntimePath ||',
            'result |= K__S_IWRITE;',
            'if (!this->path.startsWith("/tmp/.wine") && !isUserRuntimePath) {',
            'result |= K__S_IWGRP | K__S_IWOTH;',
            'BOXEDWINE_X64_RUNTIME_DIR path=%s mode=0%o writable=%d ',
        ],
        "BoxedVN guest runtime directory permission policy",
    )
    # The exception must never hand out group or other write.
    mode_start = fsfilenode.index('U32 FsFileNode::getMode() {')
    mode_end = fsfilenode.index(chr(10) + '}' + chr(10), mode_start)
    mode_body = fsfilenode[mode_start:mode_end]
    if mode_body.count('K__S_IWGRP') != 1 or mode_body.count('K__S_IWOTH') != 1:
        raise SystemExit(
            'the runtime directory exception must not add a second group or '
            'other write path'
        )
    for widened in ('startsWith("/run")', 'startsWith("/run/")',
                    'startsWith("/run/user")'):
        if widened in fsfilenode:
            raise SystemExit(
                'the write exception must not widen beyond the modelled '
                "user's runtime directory"
            )
    validator = read(repository / "scripts/validate-wine64-runtime.sh")
    if 'check_zip_path "${GLIBC_ARCHIVE}" run/user/1000' not in validator:
        raise SystemExit(
            'the rootfs layer must ship the guest runtime directory'
        )

    # ------------------------------------------------------------------ #
    # The root filesystem ships one Wine prefix and it is a 32-bit          #
    # installation, so Wine64 refused it: "'/home/username/.wine' is a      #
    # 32-bit installation, it cannot support 64-bit applications". A        #
    # 64-bit launch gets its own prefix, and everything that used to spell  #
    # the one fixed prefix out has to resolve it from the environment       #
    # instead -- a drive link written into a prefix the guest never opens   #
    # is invisible to it.                                                   #
    # ------------------------------------------------------------------ #
    newline = chr(10)
    prefix_header = read(repository / "include/guest_wine_prefix.h")
    require_ordered(
        prefix_header,
        [
            '#define K_DEFAULT_GUEST_WINE_PREFIX "/home/username/.wine"',
            '#define K_X64_GUEST_WINE_PREFIX "/home/username/.wine64"',
            '#define K_X64_GUEST_WINE_ARCH "win64"',
            "inline bool isUsableGuestWinePrefix(",
            "inline std::string resolveGuestWinePrefix(",
            "inline const char* guestWinePrefixAssignment(",
        ],
        "BoxedVN guest Wine prefix contract",
    )

    launch_arguments = read(repository / "ios/runtime/src/BVNLaunchArguments.cpp")
    require_ordered(
        launch_arguments,
        [
            '#include "guest_wine_prefix.h"',
            "if (launch.useFEX64) {",
            'argv.push_back("BOXEDWINE_CPU64=fex");',
            'if (entry.rfind("WINEPREFIX=", 0) == 0) {',
            'if (entry.rfind("WINEARCH=", 0) == 0) {',
            "if (!callerSetWinePrefix) {",
            "K_X64_GUEST_WINE_PREFIX);",
            "if (!callerSetWineArch) {",
            "K_X64_GUEST_WINE_ARCH);",
            "boxedvn::environmentSetsWineDllPath(launch.environment)",
            "boxedvn::wineDllPathAssignment()",
            "K_X64_WINE_LOADER",
        ],
        "BoxedVN x86-64 Wine prefix defaults",
    )
    # The defaults belong to the 64-bit path only. A 32-bit launch keeps the
    # bundled prefix, which is never renamed, converted or migrated.
    fex64_start = launch_arguments.index("if (launch.useFEX64) {" + newline +
                                         '        argv.push_back("-env");')
    fex64_end = launch_arguments.index(newline + "    }" + newline, fex64_start)
    fex64_body = launch_arguments[fex64_start:fex64_end]
    for required in ("K_X64_GUEST_WINE_PREFIX", "K_X64_GUEST_WINE_ARCH"):
        if required not in fex64_body:
            raise SystemExit(
                "the x86-64 Wine prefix defaults must be inside the useFEX64 "
                "branch"
            )

    startup_args = read(repository / "source/sdl/startupArgs.cpp")
    require_ordered(
        startup_args,
        [
            '#include "guest_wine_prefix.h"',
            "static BString guestWinePrefixFromEnv(",
            "boxedvn::guestWinePrefixAssignment(entry.c_str())",
            "boxedvn::resolveGuestWinePrefix(selected)",
            "static BString guestWineArchFromEnv(",
            "const BString winePrefix = guestWinePrefixFromEnv(this->envValues);",
            'const BString wineDosDevices = winePrefix + "/dosdevices";',
            "Fs::makeLocalDirs(wineDosDevices);",
            "BOXEDWINE_X64_PREFIX path=%s arch=%s dosdevices=%s ",
        ],
        "BoxedVN startup prefix resolution",
    )
    require_ordered(
        startup_args,
        [
            "static bool guestUsesFex64(",
            "static void projectX64WineSystemModules(",
            "Fs::makeLocalDirs(system32);",
            "sourceDirectory->getAllChildren(sourceModules);",
            "boxedvn::shouldProjectGuestWineSystemModule(",
            "Fs::addFileNode(destination, source->path, B(\"\"), false,",
            "BOXEDWINE_X64_SYSTEM32_OVERLAY source=%s destination=%s",
            "const bool requestedFEX64 = guestUsesFex64(this->envValues);",
            "if (requestedFEX64) {",
            "projectX64WineSystemModules(winePrefix);",
        ],
        "BoxedVN x64 Wine system32 builtin overlay",
    )
    # No launch-setup path may go back to assuming one fixed prefix. Comments
    # may still name it; code may not.
    for line in startup_args.split(newline):
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        if "/home/username/.wine" in stripped:
            raise SystemExit(
                "startupArgs.cpp must resolve the guest Wine prefix rather "
                "than hardcoding it: " + stripped
            )
    # And the drive links have to be created under the resolved prefix.
    for required in ('Fs::getNodeFromLocalPath(B(""), wineDosDevices, true)',
                     'Fs::addFileNode(wineDosDevices + "/" + info.localPath',
            'Fs::addFileNode(wineDosDevices + "/t:"'):
        if required not in startup_args:
            raise SystemExit(
                "the dosdevices drive links must use the resolved prefix: " +
                required
            )

    wine64_layout = read(repository / "include/guest_wine64_layout.h")
    require_ordered(
        wine64_layout,
        [
            '#define K_X64_WINE_MODULE_ROOT "/usr/lib/x86_64-linux-gnu/wine"',
            '#define K_X64_WINE_PE_DIR K_X64_WINE_MODULE_ROOT "/x86_64-windows"',
            '#define K_X64_WINE_LOADER K_X64_WINE_MODULE_ROOT "/wine"',
            '#define K_X64_WINE_LOADER64 K_X64_WINE_MODULE_ROOT "/wine64"',
            '#define K_X64_WINE_DLL_PATH_ASSIGNMENT "WINEDLLPATH=" K_X64_WINE_MODULE_ROOT',
            '#define K_X64_WINE_BUILTIN_PROBE K_X64_WINE_PE_DIR "/kernel32.dll"',
        ],
        "BoxedVN Wine64 canonical layout",
    )
    require_ordered(
        startup_args,
        [
            "static void reportX64BuiltinPreflight(",
            "BOXEDWINE_X64_BUILTIN_PREFLIGHT path=%s open=ok",
            "reportX64WineLayoutPreflight();",
            "reportX64BuiltinPreflight(K_X64_WINE_BUILTIN_PROBE);",
        ],
        "BoxedVN Wine64 guest VFS preflight",
    )

    # AliasGuestAddress returns the translated address in TMP4, so no lowering
    # that consumes it may reuse TMP4 before the access. The CASPair lowering
    # parked an unpaired destination in TMP3/TMP4 and every 64-bit process
    # died at exit inside RtlInterlockedPushEntrySList's lock cmpxchg16b with
    # a "page fault on read access to 0": the address had become Expected1,
    # which is 0 for an empty SList header. The LL/SC fallbacks used TMP4 as
    # the store-exclusive status register, which is the address register.
    #
    # Checked on the patch's added lines, not on the tree: this job holds the
    # pristine pin, and the alias patch is what the device build applies.
    alias_patch = read(repository / "scripts/fex64-patches/fex-boxedwine-low-address-alias.patch")
    atomic_start = alias_patch.index(
        "diff --git a/FEXCore/Source/Interface/Core/JIT/AtomicOps.cpp")
    atomic_end = alias_patch.find(newline + "diff --git ", atomic_start + 1)
    atomic_section = alias_patch[atomic_start:atomic_end if atomic_end != -1 else len(alias_patch)]
    atomic_added = newline.join(
        line[1:] for line in atomic_section.split(newline)
        if line.startswith("+") and not line.startswith("+++"))
    atomic_removed = newline.join(
        line[1:] for line in atomic_section.split(newline)
        if line.startswith("-") and not line.startswith("---"))
    if "AliasGuestAddress(GetReg(Op->Addr))" not in atomic_added:
        raise SystemExit("the atomic lowerings no longer translate their address")
    for forbidden in ("CaspalDst1 = TMP4;", "CaspalDst0 = TMP3;",
                      "stlxr(SubEmitSize, TMP4,", "mov(EmitSize, TMP4, TMP2);"):
        if forbidden in atomic_added:
            raise SystemExit(
                "an atomic lowering reuses TMP4, which carries the translated "
                f"address: {forbidden!r}"
            )
        if forbidden not in atomic_removed:
            raise SystemExit(
                "the alias patch no longer removes the upstream TMP4 reuse "
                f"{forbidden!r}; re-audit AtomicOps.cpp against the pin"
            )
    require_ordered(
        atomic_added,
        [
            "auto MemSrc = AliasGuestAddress(GetReg(Op->Addr));",
            "const bool DesiredPaired =",
            "const bool DstPaired =",
            "if (CTX->HostFeatures.SupportsAtomics && (DesiredPaired || DstPaired)) {",
            "CaspalDst0 = TMP1;",
            "CaspalDst1 = TMP2;",
        ],
        "CASPair keeps the translated address out of its destination temporaries",
    )

    # BoxedWine's -w takes a guest Linux directory. The x64 probe passed a
    # Windows path, and the device log shows the result: open(".") -> -2.
    # The x64 launchers (probe and desktop) share X64Runtime, whose
    # guestWorkingDirectory is that guest path; each launcher must pass it.
    app_model = read(repository / "ios/app/Sources/AppModel.swift")
    if '"/mnt/drive_d/.boxedvn-x64-diagnostics"' not in app_model:
        raise SystemExit(
            "the x86-64 graphics probe must pass a guest Linux working "
            "directory to BoxedWine -w"
        )
    x64_probe_start = app_model.index("func launchX64GraphicsProbe(")
    x64_probe = app_model[x64_probe_start:]
    x64_probe = x64_probe[:x64_probe.index(newline + "    func ")]
    if ('workingDirectory: runtime.guestWorkingDirectory' not in x64_probe
            and 'workingDirectory: "/mnt/drive_d/.boxedvn-x64-diagnostics"'
            not in x64_probe):
        raise SystemExit(
            "the x86-64 graphics probe must pass the shared guest working "
            "directory to BoxedWine -w"
        )
    x64_desktop_start = app_model.index("func launchX64Desktop(")
    x64_desktop = app_model[x64_desktop_start:]
    x64_desktop = x64_desktop[:x64_desktop.index(newline + "    func ")]         if newline + "    func " in x64_desktop else x64_desktop
    if 'workingDirectory: runtime.guestWorkingDirectory' not in x64_desktop:
        raise SystemExit(
            "the 64-bit desktop must pass the shared guest working directory "
            "to BoxedWine -w"
        )
    if 'workingDirectory: "d:' in x64_probe.lower():
        raise SystemExit(
            "the x86-64 graphics probe must not pass a Windows path as the "
            "BoxedWine working directory"
        )
    # The executable argument stays a Windows path: Wine is what reads it.
    if 'd:' + chr(92) + chr(92) + '.boxedvn-x64-diagnostics' + chr(92) + chr(92) + \
            'boxedvn-d3d11-cube-x64.exe' not in x64_probe:
        raise SystemExit(
            "the x86-64 graphics probe must still pass its executable as a "
            "Windows path"
        )

    # ------------------------------------------------------------------ #
    # Wine's PE ntdll reaches its Unix side through per-service thunks that   #
    # fall into a raw SYSCALL while KUSER_SHARED_DATA's SystemCall flag is    #
    # clear. RAX then holds a Windows NT ordinal, not a Linux syscall         #
    # number. The interpreter recognised these; FEX, which is what actually   #
    # runs x86-64, did not, and NT 227 returned -ENOSYS to a caller that      #
    # retried it 3,215,735 times.                                            #
    # ------------------------------------------------------------------ #
    nt_stub_header = read(repository / "include/wine_nt_syscall_stub.h")
    require_ordered(
        nt_stub_header,
        [
            "#define K_WINE_KUSER_SYSTEM_CALL_FLAG 0x7ffe0308ULL",
            "#define K_WINE_KUSER_SYSCALL_DISPATCHER 0x7ffe1000ULL",
            "inline bool matchWineNtSyscallStub(",
            "if ((rax >> 32) != 0) {",
            "at(-18) != 0x4C || at(-17) != 0x8B || at(-16) != 0xD1",
            "at(-15) != 0xB8",
            "ordinal != (uint32_t)rax",
            "at(-10) != 0xF6 || at(-9) != 0x04 || at(-8) != 0x25",
            "(uint64_t)dword(-7) != K_WINE_KUSER_SYSTEM_CALL_FLAG",
            "at(-2) != 0x75",
            "branchTarget != syscallAddress + 3",
            "at(0) != 0x0F || at(1) != 0x05",
            "at(2) != 0xC3",
            "const uint8_t tailSelector = at(3);",
            # The packaged layout: eb 01 hops the padding ret to the call.
            "if (tailSelector == 0xEB) {",
            "bridgeTarget != syscallAddress + 6",
            "if (tailAt(5) != 0xC3) {",
            "tailAt(6) != 0xFF || tailAt(7) != 0x14 || tailAt(8) != 0x25",
            "(uint64_t)tailDword(9) != K_WINE_KUSER_SYSCALL_DISPATCHER",
            "if (tailAt(13) != 0xC3) {",
            # The direct layout, validated to the same depth.
            "} else if (tailSelector == 0xFF) {",
            "tailAt(4) != 0x14 || tailAt(5) != 0x25",
            "(uint64_t)tailDword(6) != K_WINE_KUSER_SYSCALL_DISPATCHER",
            "if (tailAt(10) != 0xC3) {",
            # Anything else is not a thunk this code knows how to redirect.
            "} else {",
            "dispatcher == 0 || dispatcher == ~(uint64_t)0",
        ],
        "BoxedVN Wine NT stub recognition contract",
    )
    # The layout constants have to describe the thunk that actually ships.
    for required in ("inline constexpr int kWineNtStubFirstOffset = -18;",
                     "inline constexpr int kWineNtStubLastOffset = 13;",
                     "inline constexpr unsigned kWineNtStubLength = 32;"):
        if required not in nt_stub_header:
            raise SystemExit(
                "the Wine NT stub layout constants must describe the packaged "
                "32-byte thunk: " + required
            )
    # Resuming anywhere but the jne target would be guessing at Wine's own
    # control flow instead of following it.
    if "out.indirectPath = branchTarget;" not in nt_stub_header:
        raise SystemExit(
            "the redirect must resume on the validated jne target"
        )
    # Recognition must be by instruction shape. An address range is what broke
    # last time: ntdll moved and the check silently stopped matching. Comments
    # may still recount that history; code may not act on it.
    def code_lines(text):
        for line in text.split(newline):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or                     stripped.startswith("#"):
                continue
            yield stripped

    for line in code_lines(nt_stub_header):
        for banned in ("0x170000000", "0x170400000"):
            if banned in line:
                raise SystemExit(
                    "the Wine NT stub matcher must not depend on where ntdll "
                    "is mapped: " + line
                )

    nt_memory_header = read(repository / "include/wine_nt_syscall_memory.h")
    require_ordered(
        nt_memory_header,
        [
            '#include "wine_nt_syscall_stub.h"',
            "inline bool readGuestBytesForNtStub(",
            "if (!memory->isPageMapped(page)) {",
            "inline bool matchWineNtSyscallStubInGuest(",
            "inline void setWineNtSystemCallFlag(",
            "inline void reportWineNtSyscallRedirect(",
            "BOXEDWINE_X64_NT_REDIRECT pid=%u tid=%u nt=%u stub=0x%llx ",
        ],
        "BoxedVN Wine NT stub guest binding",
    )

    # Both CPU backends must reach the same recognition, or they will disagree
    # about what a thunk is.
    interpreter = read(repository / "source/emulation/cpu/cpu64.cpp")
    adapter = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    for name, source in (("interpreter", interpreter), ("FEX adapter", adapter)):
        for required in ('#include "wine_nt_syscall_memory.h"',
                         "boxedvn::matchWineNtSyscallStubInGuest(",
                         "boxedvn::setWineNtSystemCallFlag(",
                         "boxedvn::reportWineNtSyscallRedirect("):
            if required not in source:
                raise SystemExit(
                    "the " + name + " must use the shared Wine NT stub "
                    "recognition: " + required
                )
    # The obsolete image-base gate is what stopped the interpreter matching
    # this Wine at all.
    for line in code_lines(interpreter):
        for banned in ("0x170000000", "0x170400000"):
            if banned in line:
                raise SystemExit(
                    "the interpreter must not gate the NT redirect on an "
                    "ntdll address range: " + line
                )

    # The redirect has to happen before the Linux dispatcher sees the ordinal.
    handler_start = adapter.index(
        "extern " + chr(34) + "C" + chr(34) +
        " uint64_t BVNFEXCPU64AdapterHandleSyscall(")
    handler = adapter[handler_start:]
    match_at = handler.index("boxedvn::matchWineNtSyscallStubInGuest(")
    ksyscall_at = handler.index("ksyscall64(cpu);")
    if match_at > ksyscall_at:
        raise SystemExit(
            "the FEX adapter must recognise a Wine NT thunk before calling "
            "ksyscall64"
        )
    redirect = handler[match_at:ksyscall_at]
    for required in (
        # RAX carries the NT ordinal into Wine's dispatcher.
        "cpu->reg[X64_RAX].setU64(ntStub.ntOrdinal);",
        # The indirect path is a call, so RCX keeps the NT call's first
        # argument, which the thunk's own `mov r10, rcx` preserved.
        "cpu->reg[X64_RCX].setU64(arguments[4]);",
        # Resume on the validated indirect path, not on a guessed offset.
        "cpu->rip = ntStub.indirectPath;",
        "BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)",
        "adapter->lastAction = BVNFEXCPU64AdapterActionYield;",
        "return ntStub.ntOrdinal;",
    ):
        if required not in redirect:
            raise SystemExit(
                "the FEX NT redirect is incomplete: " + required
            )

    # FEX's generic-host SYSCALL lowering does not end a translated block. A
    # syscall that changes RIP (most importantly rt_sigreturn) has to leave the
    # block, or execution falls through after the SYSCALL in Wine's restorer
    # and the restored RIP never takes effect.
    syscall_tail = handler[ksyscall_at:]
    require_ordered(
        syscall_tail,
        [
            "ksyscall64(cpu);",
            "fexSyscallMustLeaveCurrentBlock(postSyscallRip, cpu->rip)",
            "BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)",
            "BOXEDWINE_FEX64_SYSCALL_REDIRECT pid=%d tid=%d",
            "adapter->lastAction = execSucceeded",
            "? BVNFEXCPU64AdapterActionExec",
            ": BVNFEXCPU64AdapterActionYield;",
        ],
        "FEX syscall control-flow replacement contract",
    )

    # Why Yield rather than a bare RIP change: in the pinned FEX, SYSCALL is
    # not a block-ending instruction off Windows, so SyscallOp never reloads
    # RIP from the state and a changed frame RIP would simply be ignored. If a
    # future pin changes that, this redirect should be revisited rather than
    # silently keeping the more expensive exit.
    fex_tables = read(fex / "FEXCore/Source/Interface/Core/X86Tables/X86Tables.h")
    expected_flags = (
        "#ifndef _WIN32" + newline +
        "  constexpr uint32_t DEFAULT_SYSCALL_FLAGS = FLAGS_NO_OVERLAY;"
    )
    if expected_flags not in fex_tables:
        raise SystemExit(
            "the pinned FEX no longer omits FLAGS_BLOCK_END from SYSCALL off "
            "Windows; the Wine NT redirect's use of a Yield re-entry must be "
            "re-checked against the new block-exit behaviour"
        )
    fex_dispatcher = read(
        fex / "FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp")
    require_ordered(
        fex_dispatcher,
        [
            "void OpDispatchBuilder::SyscallOp(",
            "StoreGPRRegister(X86State::REG_RAX, SyscallOp);",
            "if (Op->TableInfo->Flags & X86Tables::InstFlags::FLAGS_BLOCK_END) {",
        ],
        "pinned FEX syscall block-exit contract",
    )

    # Unsupported-syscall diagnostics must be bounded independently of the
    # semantic fix above.
    limiter_header = read(repository / "include/bounded_syscall_report.h")
    require_ordered(
        limiter_header,
        [
            "class BoundedSyscallReportLimiter {",
            "static constexpr unsigned kSlots =",
            "static constexpr unsigned kReportsPerKey =",
            "static constexpr unsigned kTotalReports =",
            "Outcome record(",
            "Slot slots_[kSlots]",
        ],
        "BoxedVN bounded syscall report contract",
    )
    # A fixed-size table, not a map keyed on guest-controlled values.
    for banned in ("std::map", "std::unordered_map", "std::vector"):
        if banned in limiter_header:
            raise SystemExit(
                "the unsupported-syscall limiter must use bounded storage, "
                "not " + banned
            )

    syscall_dispatch = read(repository / "source/kernel/syscall64.cpp")
    require_ordered(
        syscall_dispatch,
        [
            "cpu->unsupportedSyscallReports.record(",
            "UnsupportedDecision::Detailed",
            "UnsupportedDecision::Repeat",
            "UnsupportedDecision::Suppress",
            "further reports for this syscall are suppressed",
            "unsupported.decision == UnsupportedDecision::Detailed &&",
            'getenv("BW64_SCDUMP")',
            "ret = (U64)-K_ENOSYS;",
        ],
        "BoxedVN unsupported syscall reporting bound",
    )
    # The register dump is the most verbose part of the report and has to be
    # inside the same bound, not merely behind the environment variable.
    scdump_at = syscall_dispatch.index(
        "unsupported.decision == UnsupportedDecision::Detailed &&" + newline)
    scdump_tail = syscall_dispatch[scdump_at:scdump_at + 400]
    if 'getenv("BW64_SCDUMP")' not in scdump_tail:
        raise SystemExit(
            "BW64_SCDUMP must be gated by the report limiter as well as by "
            "its environment variable"
        )

    # ------------------------------------------------------------------ #
    # Wine reserves inaccessible arenas at high addresses the identity        #
    # window cannot host. Refusing them made it retry without end: 8,916,993  #
    # mmaps in one device run, 8,916,842 rejected, ~98% of a core.            #
    # ------------------------------------------------------------------ #
    placement = read(repository / "include/guest_mmap_placement.h")
    require_ordered(
        placement,
        [
            "ReserveSparse = 5,",
            "bool anonymous = false;",
            "if (request.nativeIdentity && !request.exactRangeAllowed) {",
            "if (request.anonymous && request.protection == 0) {",
            "return GuestMmapPlacement::ReserveSparse;",
            # Accessible or file-backed: no host memory can be provided at
            # that address, and the range is empty, so it is -ENOMEM.
            "return GuestMmapPlacement::FailNoMemory;",
        ],
        "BoxedVN sparse reservation placement contract",
    )
    # A reservation is a grant, not a failure: nothing may skip the address
    # space for it.
    failure_start = placement.index("inline bool guestMmapPlacementIsFailure(")
    failure_body = placement[failure_start:placement.index("}", failure_start)]
    if "ReserveSparse" in failure_body:
        raise SystemExit(
            "a granted sparse reservation must not be classified as a failure"
        )

    memory_header = read(repository / "include/kmemory64.h")
    require_ordered(
        memory_header,
        [
            "U64 reserveSparseNoReplace(U64 addr, U64 len);",
            "bool sparseReservationOverlaps(U64 addr, U64 len) const;",
            "bool releaseSparseReservation(U64 addr, U64 len);",
            "U64 sparseReservationCount() const;",
            # The const reservation queries take this lock, so it has to
            # be mutable the way pagesMutex already is.
            "mutable BOXEDWINE_MUTEX mmapMutex;",
            "std::map<U64, U64> sparseReservations;",
        ],
        "BoxedVN sparse reservation address-space contract",
    )

    memory = read(repository / "source/kernel/kmemory64.cpp")
    require_ordered(
        memory,
        [
            "U64 KMemory64::reserveSparseNoReplace(",
            "return (U64)-K_EEXIST;",
            "bool KMemory64::sparseReservationOverlaps(",
            "bool KMemory64::releaseSparseReservation(",
        ],
        "BoxedVN sparse reservation implementation contract",
    )
    # The whole point is that a reservation costs one interval. It must not
    # walk pages, map anything, or touch the native layer.
    reserve_start = memory.index("U64 KMemory64::reserveSparseNoReplace(")
    reserve_body = memory[reserve_start:memory.index(
        newline + "}" + newline, reserve_start)]
    for line in code_lines(reserve_body):
        for banned in ("getOrAllocPage", "commitPageLocked",
                       "mmapAnonymousFixed", "nativeMapAnonymous",
                       "mmapReserveAndMap", "new K64Page"):
            if banned in line:
                raise SystemExit(
                    "a sparse reservation must allocate no backing store: " +
                    line
                )
    # munmap has to release the interval before the native window guard, or the
    # address could never be reused.
    munmap_start = memory.index("U64 KMemory64::munmap(")
    munmap_body = memory[munmap_start:munmap_start + 2000]
    release_at = munmap_body.index("releaseSparseReservation(")
    guard_at = munmap_body.index("reject native munmap outside guest window")
    if release_at > guard_at:
        raise SystemExit(
            "munmap must release a sparse reservation before the native "
            "window guard refuses its address"
        )
    if "sparseReservations = from->sparseReservations;" not in memory:
        raise SystemExit(
            "a cloned address space must inherit its sparse reservations"
        )

    syscall_mmap = read(repository / "source/kernel/syscall64.cpp")
    require_ordered(
        syscall_mmap,
        [
            "request.anonymous = true;",
            "cpu->memory->sparseReservationOverlaps(alignedAddr, mapLen)",
            "case boxedvn::GuestMmapPlacement::ReserveSparse:",
            "cpu->memory->reserveSparseNoReplace(alignedAddr, mapLen);",
        ],
        "BoxedVN sparse reservation syscall contract",
    )
    # Summaries are bounded, not merely strided: one line per 4096 calls still
    # produced 2,177 lines against nine million mmaps.
    if "kGuestMmapSummaryEvery" in syscall_mmap:
        raise SystemExit(
            "mmap summaries must be bounded rather than emitted on a fixed "
            "stride"
        )
    require_ordered(
        syscall_mmap,
        [
            "bool guestMmapSummaryDue(U64 ordinal) {",
            "(ordinal & (ordinal - 1)) == 0;",
            'reportGuestMmapSummary("milestone");',
        ],
        "BoxedVN bounded mmap summary contract",
    )

    # ------------------------------------------------------------------ #
    # An indirect exit must never branch to a null host target. FEX caches   #
    # {HostCode, GuestCode} in a zero-initialised direct-mapped table and    #
    # marks an entry dead by zeroing its GuestCode, so an EMPTY slot reads   #
    # {0, 0}. The emitted lookup in DEF_OP(ExitFunction) compared only the   #
    # guest key, which made slot zero a hit for a dynamic target of zero and #
    # `ret TMP2` a branch to host address zero: the device crashed with host #
    # PC 0 and fault address 0 out of the `leave; mov eax, edx; ret`         #
    # epilogue of __libc_sigaction at guest 0x7a4004d4b9.                    #
    # ------------------------------------------------------------------ #
    null_exit_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-null-exit-target.patch")
    require_ordered(
        null_exit_patch,
        [
            "FEXCore/Source/Interface/Core/JIT/BranchOps.cpp",
            # The prediction path: a zeroed pair pushed by a call with no known
            # return block must not be believed for a target of zero.
            "PredictionUnusable",
            # The L1 path: the key alone is not a hit.
            "ARMEmitter::ForwardLabel L1Unusable;",
            "cbnz(ARMEmitter::Size::i64Bit, TMP1, &L1Unusable)",
            "cbnz(ARMEmitter::Size::i64Bit, TMP2, &SkipFullLookup)",
            # The witness, named by the block that produced the null target.
            "LoadConstant(ARMEmitter::Size::i64Bit, TMP1, Entry)",
            "BoxedWineNullExitSource",
            "Pointers.DispatcherLoopTop",
        ],
        "BoxedVN null indirect-exit guard",
    )
    # The same rule on the C++ side of the cache, and no dead code pointer left
    # behind by invalidation for a zero key to match.
    require_ordered(
        null_exit_patch,
        [
            "FEXCore/Source/Interface/Core/LookupCache.h",
            "if (L1Entry.GuestCode == Address && L1Entry.HostCode) {",
            "L1Entry.GuestCode = 0;",
            "L1Entry.HostCode = 0;",
            # Publication order: the host pointer before the key it belongs to.
            "L1Entry.HostCode = Entry.HostCode;",
            "L1Entry.GuestCode = Address;",
        ],
        "BoxedVN lookup cache null-host contract",
    )
    # The witness slot, and the bounded reporter that consumes it.
    require_ordered(
        null_exit_patch,
        [
            "FEXCore/include/FEXCore/Core/CoreState.h",
            "uint64_t BoxedWineNullExitSource {};",
            "<= 32760",
        ],
        "BoxedVN null-exit witness slot",
    )
    require_ordered(
        null_exit_patch,
        [
            "FEXCore/Source/Interface/Core/Core.cpp",
            "BoxedWineNullExitReportLimit",
            "ContextImpl::BoxedWineReportNullExitTarget",
            "Frame->BoxedWineNullExitSource = 0;",
            "L1Pointer",
            "BOXEDWINE_FEX64_NULL_EXIT_TARGET",
            "BoxedWineReportNullExitTarget(Frame, GuestRIP);",
        ],
        "BoxedVN null-exit witness report",
    )
    for field in ("source_block=", "guest_target=", "host_target=", "l1_key=",
                  "l1_host=", "rsp_after_pop=", "popped_slot=",
                  "popped_slot_host=", "return_slot_host=", "path=",
                  "repeat="):
        if field not in null_exit_patch:
            raise SystemExit(
                "the null indirect-exit witness must carry " + field
            )
    # The witness must stay bounded: no unconditional per-exit logging.
    if "BoxedWineNullExitReports.fetch_add" not in null_exit_patch:
        raise SystemExit(
            "the null indirect-exit witness must be bounded by a counter"
        )

    # It is a MAINTAINED patch, applied by both the device build and the
    # translator conformance probe, not an edit to the checked-out tree.
    fex_build_script = read(repository / "scripts/build-fex64-fex.sh")
    require_ordered(
        fex_build_script,
        [
            "apply_patch fex-boxedwine-low-address-alias.patch",
            "apply_patch fex-boxedwine-null-exit-target.patch",
        ],
        "BoxedVN null-exit patch is applied by the device build",
    )
    vixl_probe = read(repository / "scripts/run-fex64-vixl-probe.sh")
    require_ordered(
        vixl_probe,
        [
            "fex-boxedwine-low-address-alias.patch",
            "fex-boxedwine-null-exit-target.patch",
        ],
        "BoxedVN null-exit patch is applied by the VIXL probe",
    )

    # The behavioural regression: nested CALL / LEAVE / RET on both relocated
    # stacks, run cold (every return site an L1 miss) and warm (the hit path).
    highstack = read(
        repository / "scripts/guest-probes/fex64-highstack-callret.asm")
    require_ordered(
        highstack,
        [
            # Both lanes BoxedWine relocates.
            "0x7ffc0000",
            "0x7ffffe000000",
            # The device frame shape.
            "push rbp",
            "mov rbp, rsp",
            # Each call site names the return address it pushes, so a callee
            # reached from several sites checks the one that applies.
            "cmp r8, rdi",
            # A helper boundary whose return block is unknown at compile time.
            "call rax",
            "leave",
            "ret",
        ],
        "BoxedVN high-stack call/ret fixture",
    )
    if "qword [rbp + 8]" not in highstack:
        raise SystemExit(
            "the high-stack fixture must read the pushed return address back "
            "through an ordinary translated load"
        )
    require_ordered(
        vixl_probe,
        [
            "fixture_highstack_callret=",
            'prepare_fixture "${fixture_highstack_callret}" '
            "fex64-highstack-callret",
            "run_one x64-highstack-callret",
        ],
        "BoxedVN high-stack call/ret probe wiring",
    )
    # The fixture runs with the alias on, which is the device configuration.
    highstack_runs = [
        line for line in vixl_probe.splitlines()
        if "fex64-highstack-callret.config.bin" in line
    ]
    if len(highstack_runs) < 3:
        raise SystemExit(
            "the high-stack fixture must run in single-block and multiblock "
            "modes"
        )
    for line in highstack_runs:
        if not line.rstrip().endswith('"" 1'):
            raise SystemExit(
                "the high-stack fixture must run with BoxedWine's address "
                "translation enabled: " + line.strip()
            )

    # Every maintained FEX patch stays part of the CI cache key, and so does
    # the new fixture.
    workflow = read(repository / ".github/workflows/build-ios.yml")
    if "scripts/fex64-patches/**" not in workflow:
        raise SystemExit(
            "the maintained FEX patches must be part of the CI cache key"
        )
    if "scripts/guest-probes/fex64-highstack-callret.asm" not in workflow:
        raise SystemExit(
            "the high-stack call/ret fixture must be part of the CI cache key"
        )

    # ------------------------------------------------------------------ #
    # A CALL's return-address push reads its own store back.               #
    #                                                                      #
    # Device evidence at 59fe87a2: the guest stopped with RIP zero out of   #
    # the epilogue of __libc_sigaction, and a checked re-read of its stack   #
    # showed the frame chain intact -- every saved frame pointer correct --  #
    # while every return-address slot above it held zero. LEAVE and RET both #
    # did the right arithmetic, so either the CALL never stored the address  #
    # or something zeroed the slot before the RET. Nothing observable at RET #
    # time separates those, which is what this witness exists to do.        #
    # ------------------------------------------------------------------ #
    call_witness_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-call-return-witness.patch")
    require_ordered(
        call_witness_patch,
        [
            "FEXCore/include/FEXCore/Core/CoreState.h",
            # The frame carries only a pointer. The ring itself must live
            # outside it: CpuStateFrame is addressed by JIT load/store
            # immediates whose range depends on access width, and a kilobyte of
            # growth pushed an existing byte-sized access past its 4095-byte
            # limit -- caught by the conformance probe, not by a device.
            "uint64_t BoxedWineCallRingPointer {};",
            "struct BoxedWineCallRecord {",
            "uint64_t IntendedReturn;",
            "uint64_t SlotGuest;",
            "uint64_t SlotHost;",
            "uint64_t Readback;",
            "uint64_t StackBefore;",
            # The record size and ring count must stay shiftable/maskable,
            # because the JIT scales the index rather than multiplying.
            "sizeof(BoxedWineCallRecord) == 64",
            "offsetof(BoxedWineCallRing, Records) == 0",
            "BoxedWineCallRing::Count - 1)) == 0",
        ],
        "BoxedVN CALL history ring",
    )
    # The ring must not be a member of the frame.
    if "BoxedWineCallRing[" in call_witness_patch:
        raise SystemExit(
            "the CALL ring must not be embedded in CpuStateFrame; the frame's "
            "load/store immediates cannot absorb its size"
        )
    require_ordered(
        call_witness_patch,
        [
            "FEXCore/include/FEXCore/Debug/InternalThreadState.h",
            "FEXCore::Core::BoxedWineCallRing BoxedWineCallRing {};",
        ],
        "BoxedVN CALL ring lives beside the thread",
    )
    if "Thread->CurrentFrame->BoxedWineCallRingPointer = reinterpret_cast<uint64_t>"             not in call_witness_patch:
        raise SystemExit(
            "the CALL ring pointer must be published when the thread is created"
        )
    require_ordered(
        call_witness_patch,
        [
            "FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp",
            # Only a CALL's return-address push is recorded; a `push rbp` is
            # not, so an ordinary push costs nothing.
            "bool Arm64JITCore::IsCallReturnPush(",
            "OP_ENTRYPOINTOFFSET",
            "void Arm64JITCore::EmitCallReturnWitness(",
            # One pointer load, then everything is relative to the ring base.
            "BoxedWineCallRingPointer));",
            "cbz(ARMEmitter::Size::i64Bit, TMP2, &NoRing)",
            # The read-back has to go through the SAME host address the store
            # used, or it proves nothing about that store.
            "ldur(TMP3, SlotHost, 0);",
            "offsetof(Record, Readback)",
            "offsetof(Ring, Anomalies)",
            "IsCallReturnPush(Op->Value)",
        ],
        "BoxedVN CALL return-address read-back",
    )
    # The witness must be armed by the integration, not always on.
    if "GetBoxedWineCallWitness()" not in call_witness_patch:
        raise SystemExit(
            "the CALL witness must be gated on an explicitly armed flag"
        )
    require_ordered(
        read(repository / "scripts/build-fex64-fex.sh"),
        [
            "apply_patch fex-boxedwine-null-exit-target.patch",
            "apply_patch fex-boxedwine-call-return-witness.patch",
            "apply_patch fex-boxedwine-inline-call-return.patch",
        ],
        "BoxedVN CALL witness patch is applied by the device build",
    )
    require_ordered(
        read(repository / "scripts/run-fex64-vixl-probe.sh"),
        [
            "fex-boxedwine-null-exit-target.patch",
            "fex-boxedwine-call-return-witness.patch",
            "fex-boxedwine-inline-call-return.patch",
        ],
        "BoxedVN CALL witness patch is applied by the VIXL probe",
    )
    # It is armed on the device path, with an off switch that needs no rebuild.
    backend_source = read(repository / "ios/runtime/src/BVNFEXBackend.mm")
    require_ordered(
        backend_source,
        [
            "SetGuestTopClearMask(boxedvn::kGuestTopClearMask);",
            'getenv("BW64_NO_CALL_WITNESS")',
            "SetBoxedWineCallWitness(callWitness);",
            "BOXEDWINE_FEX64_CALL_WITNESS armed=",
        ],
        "BoxedVN CALL witness arming",
    )

    # The ring is retained, not narrated: it prints only when a return has
    # already failed.
    require_ordered(
        call_witness_patch,
        [
            "BoxedWineCallRingDumpLimit",
            "ContextImpl::BoxedWineDumpCallRing",
            "BOXEDWINE_FEX64_CALL_RING",
            "BOXEDWINE_FEX64_CALL_RECORD",
        ],
        "BoxedVN CALL ring dump",
    )
    # A device run settled the first question: across 617,677 recorded calls
    # the read-back matched every time and anomalies were zero, including for
    # the very slot the failing RET popped. The push is exonerated, so the
    # witness now has to say what the slot holds AT THE FAILURE and whether a
    # neighbouring region went with it -- one zeroed qword and a zeroed run
    # need different fixes.
    for field in ("current=", "intact=", "BOXEDWINE_FEX64_NULL_EXIT_WINDOW",
                  "qwords="):
        if field not in call_witness_patch:
            raise SystemExit(
                "the CALL witness must report " + field
            )
    for reason in ('"null-exit-target"', '"compile-low-rip"'):
        if reason not in call_witness_patch:
            raise SystemExit(
                "the CALL ring must be dumped for " + reason
            )

    # ------------------------------------------------------------------ #
    # The null-exit marker has to fire on the TARGET, not on which branch   #
    # of the cache lookup happened to miss. Keying it on "the cached entry   #
    # matched but its host half was null" made it unreachable once any block #
    # occupied slot zero: two device runs reached CompileBlock with RIP zero #
    # and neither produced a line.                                          #
    # ------------------------------------------------------------------ #
    require_ordered(
        call_witness_patch,
        [
            "FEXCore/Source/Interface/Core/JIT/BranchOps.cpp",
            # The dynamic target is copied once, before any scratch register
            # is written, and every later use reads the copy. A device run
            # reached this lowering with a target of zero while the slot it was
            # popped from held the right address throughout -- the value was
            # correct when popped and became zero before State.rip was stored.
            "const auto TargetReg = TMP3;",
            "mov(ARMEmitter::Size::i64Bit, TargetReg.R(), RipReg);",
            "ARMEmitter::ForwardLabel TargetIsUsable;",
            "cbnz(ARMEmitter::Size::i64Bit, TargetReg, &TargetIsUsable)",
            "BoxedWineNullExitSource",
        ],
        "BoxedVN null-exit marker keyed on the target",
    )
    # Nothing after the copy may re-read the original register: the point of
    # the copy is that a clobber between the pop and any of these uses cannot
    # decide what the guest runs.
    exit_start = call_witness_patch.index("const auto TargetReg = TMP3;")
    exit_end = call_witness_patch.index("Bind(&SkipFullLookup)", exit_start)
    for line in call_witness_patch[exit_start:exit_end].splitlines():
        if line.startswith("+") and "RipReg" in line and "TargetReg" not in line:
            raise SystemExit(
                "the exit lowering re-reads the dynamic target's original "
                "register after a scratch write: " + line.strip()
            )
    # And the invariant that makes TMP3 usable is stated, not assumed.
    if "must not be allocated to a scratch temporary" not in call_witness_patch:
        raise SystemExit(
            "the exit lowering must assert that its target register is not a "
            "scratch temporary"
        )

    # ------------------------------------------------------------------ #
    # A guest target the host can never execute must become a guest fault,  #
    # and no path may hand the dispatcher a null host pointer to branch to.  #
    # ------------------------------------------------------------------ #
    low_rip_hunk = call_witness_patch.split("low/invalid RIP")[-1].split(
        "diff --git")[0]
    # Only ADDED lines describe the resulting code; the removed `return 0;`
    # is the whole point of the change.
    for line in low_rip_hunk.splitlines():
        if line.startswith("+") and "return 0;" in line:
            raise SystemExit(
                "a refused low guest RIP must not be returned to the "
                "dispatcher as a null host pointer"
            )
    if "-    return 0;" not in call_witness_patch:
        raise SystemExit(
            "the low guest RIP refusal must stop returning a null host pointer"
        )
    require_ordered(
        call_witness_patch,
        [
            "FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp",
            "ARMEmitter::ForwardLabel NullCompiledBlock;",
            "cbz(ARMEmitter::Size::i64Bit, TMP1, &NullCompiledBlock)",
            "Pointers.ThreadStopHandlerSpillSRA",
        ],
        "BoxedVN dispatcher never branches to a null host target",
    )
    # The interrupt-fault-page store has to pick its access width from the
    # offset. InterruptFaultPage is page-aligned inside InternalThreadState, so
    # its distance from BaseFrameState jumps a page whenever CpuStateFrame
    # crosses a page boundary -- and a byte store's unsigned immediate stops at
    # 4095. Two witness fields were enough to push it over, and the emitter
    # refused the store. JIT.cpp's copy of the same address already chose its
    # width this way; the dispatcher's did not.
    require_ordered(
        call_witness_patch,
        [
            "constexpr size_t InterruptPageOffset =",
            "InterruptPageOffset % 8 == 0",
            "if constexpr (InterruptPageOffset <= 4095) {",
            "strb(ARMEmitter::XReg::zr, STATE, InterruptPageOffset);",
            "InterruptPageOffset <= 32760",
            "str(ARMEmitter::XReg::zr, STATE, InterruptPageOffset);",
        ],
        "BoxedVN interrupt fault page store width",
    )

    # A CALL has no guest instruction between its architectural Push and the
    # ExitFunction transfer. Reasserting the return qword there makes the slot
    # authoritative across a second lowering seam, and gives the witness a
    # reliable hook even when IR canonicalisation has changed the Push value's
    # node type before MemoryOps sees it.
    require_ordered(
        call_witness_patch,
        [
            "DEF_OP(ExitFunction) {",
            "Op->Hint == IR::BranchHint::Call",
            "!Op->CallReturnAddress.IsInvalid()",
            "const auto GuestStack = StaticRegisters[X86State::REG_RSP];",
            "const auto HostStack = AliasGuestAddress(GuestStack);",
            "stur(ReturnAddress.X(), HostStack, 0);",
            "EmitCallReturnWitness(ReturnAddress, GuestStack, HostStack,",
            "ResetStack();",
        ],
        "BoxedVN CALL exit-slot repair and witness",
    )

    # Build 137 proved that re-storing the register-allocated return value is
    # not independent repair: one indirect CALL recorded, wrote and read back
    # an intended return of zero. The exit must receive its own inline return
    # PC metadata, materialise it after RA, and preserve it until the witness
    # has stored IntendedReturn.
    inline_return_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-inline-call-return.patch")
    for patch_driver in (
        read(repository / "scripts/build-fex64-fex.sh"),
        read(repository / "scripts/run-fex64-vixl-probe.sh"),
    ):
        if "fex-boxedwine-inline-call-return.patch" not in patch_driver or \
                "apply_options=(apply)" not in patch_driver or \
                "apply_options+=(--unidiff-zero)" not in patch_driver or \
                '"${apply_options[@]}"' not in patch_driver:
            raise SystemExit(
                "the zero-context inline-return patch must be applied with "
                "a Bash-3.2 nounset-safe git apply --unidiff-zero command")
    require_ordered(
        inline_return_patch,
        [
            "FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp",
            "auto InlineReturnPC = _InlineEntrypointOffset(",
            "ExitRelocatedPC(Op, TargetOffset, BranchHint::Call, InlineReturnPC",
            "ExitFunction(JMPPCOffset, BranchHint::Call, InlineReturnPC",
            "FEXCore/Source/Interface/Core/JIT/BranchOps.cpp",
            "IsInlineEntrypointOffset(Op->CallReturnAddress",
            "InsertGuestRIPMove(TMP1, BoxedWineInlineCallReturn)",
            "stur(TMP1, HostStack, 0);",
            "EmitCallReturnWitness(TMP1, GuestStack, HostStack",
            "FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp",
            "str(Value.X(), TMP3, offsetof(Record, IntendedReturn));",
            "LoadConstant(ARMEmitter::Size::i64Bit, TMP1, Entry);",
        ],
        "BoxedVN inline CALL return-PC repair",
    )
    added_inline_return_lines = [
        line[1:] for line in inline_return_patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    ]
    if "add(ARMEmitter::Size::i64Bit, TMP1, TMP2, TMP3);" in added_inline_return_lines:
        raise SystemExit(
            "the witness must not overwrite a TMP1 return value while forming "
            "the record pointer")

    # ------------------------------------------------------------------ #
    # A fatal translator ending and a guest exit_group are different        #
    # outcomes. A contained host fault unwinds cleanly and returns true, so  #
    # the return value alone cannot tell them apart -- gating on it left the #
    # process alive with no exit status while the session showed a loading   #
    # overlay, and gating on the action alone would overwrite a clean guest  #
    # exit with a failure.                                                  #
    # ------------------------------------------------------------------ #
    backend_header = read(repository / "ios/runtime/include/BVNFEXBackend.h")
    require_ordered(
        backend_header,
        [
            "BVNFEXCPU64AdapterActionProcessExit = 4,",
            "BVNFEXCPU64AdapterActionFatalExit = 5,",
            "BVNFEXCPU64RunOutcomeYield = 0,",
            "BVNFEXCPU64RunOutcomeGuestExit = 1,",
            "BVNFEXCPU64RunOutcomeFatal = 2,",
            "bool BVNFEXCPU64Run(void* process, void* thread,",
        ],
        "BoxedVN explicit translator run outcome",
    )
    adapter_fatal = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    if "adapter->lastAction = BVNFEXCPU64AdapterActionFatalExit;" not in adapter_fatal:
        raise SystemExit(
            "a contained host fault must report a fatal ending, not a guest "
            "process exit"
        )
    # exit_group keeps its own, non-fatal action.
    if "syscallNumber == 231" not in adapter_fatal:
        raise SystemExit("exit_group handling went missing")
    exit_group_at = adapter_fatal.index("syscallNumber == 231")
    exit_group_tail = adapter_fatal[exit_group_at:exit_group_at + 200]
    if "BVNFEXCPU64AdapterActionProcessExit" not in exit_group_tail:
        raise SystemExit(
            "a guest exit_group must stay an ordinary process exit"
        )
    require_ordered(
        backend_source,
        [
            "BVNFEXCPU64AdapterActionFatalExit ||",
            "const bool fatal =",
            "publish(fatal ? BVNFEXCPU64RunOutcomeFatal",
        ],
        "BoxedVN fatal outcome is derived from the action",
    )
    platform_fatal = read(
        repository / "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")
    require_ordered(
        platform_fatal,
        [
            "BVNFEXCPU64RunOutcome outcome =",
            "BVNFEXCPU64Run(process.get(), cpu->thread, &outcome);",
            "if (outcome == BVNFEXCPU64RunOutcomeFatal) {",
            'kfatalProcessExit64(cpu64, 127,',
        ],
        "BoxedVN fatal-only exit routing",
    )
    # A guest exit must never be converted into a fatal one.
    fatal_at = platform_fatal.index("if (outcome == BVNFEXCPU64RunOutcomeFatal) {")
    fatal_block = platform_fatal[fatal_at:fatal_at + 600]
    if "BVNFEXCPU64RunOutcomeGuestExit" in fatal_block:
        raise SystemExit(
            "a normal guest exit must not reach the fatal exit path"
        )

    # The fixture covers the lowering shapes a device register allocation can
    # take, and the return address read straight off the stack on entry.
    highstack_shapes = read(
        repository / "scripts/guest-probes/fex64-highstack-callret.asm")
    require_ordered(
        highstack_shapes,
        [
            "push_shapes:",
            # The immediate read-back, guest side.
            "mov r8, qword [rsp]",
            # Value register is the address register.
            "push rsp",
            # Adjacent pushes, which the allocator fuses into a paired store.
            "push r8",
            "push r9",
            # A dispatcher boundary between the CALL and its RET.
            "jmp rax",
            "leave",
            "ret",
        ],
        "BoxedVN push overlap shape coverage",
    )

    # ------------------------------------------------------------------ #
    # The fault report has to say what the guest stack held, read back    #
    # independently of whatever the translated code loaded from it.       #
    # ------------------------------------------------------------------ #
    adapter_source = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    require_ordered(
        adapter_source,
        [
            "static GuestStackWord readGuestStackWord(",
            # Translation only. The canonical address is never dereferenced.
            "k64GuestToHostAddress(guestAddress)",
            # The same checked read the host-code capture uses.
            "vm_read_overwrite(",
            "BOXEDWINE_FEX64_FAULT_STACK",
        ],
        "BoxedVN guest stack fault snapshot",
    )
    for field in ('"rsp-16"', '"rsp-8"', '"rsp"', '"rsp+8"', '"rbp"',
                  '"rbp+8"'):
        if field not in adapter_source:
            raise SystemExit(
                "the guest stack snapshot must cover " + field
            )
    if "guest=0x%llx host=0x%llx read=%d value=0x%llx" not in adapter_source:
        raise SystemExit(
            "each guest stack word must report its canonical address, the host "
            "address it was read through, whether the read succeeded, and the "
            "value"
        )
    stack_reader_start = adapter_source.index(
        "static GuestStackWord readGuestStackWord(")
    stack_reader_body = adapter_source[
        stack_reader_start:adapter_source.index(
            newline + "}" + newline, stack_reader_start)]
    for banned in ("malloc", "new ", "lock", "printf("):
        if banned in stack_reader_body:
            raise SystemExit(
                "the guest stack snapshot runs in a signal handler and must "
                "not " + banned.strip()
            )

    # ------------------------------------------------------------------ #
    # A fatal translator failure ends the guest PROCESS the way exit_group  #
    # does. Retiring only the thread left the pid with no exit status, no    #
    # lifecycle marker, and a session that went on presenting a loading      #
    # overlay while helper processes kept the emulator alive.                #
    # ------------------------------------------------------------------ #
    syscall_header = read(repository / "include/syscall64.h")
    if "void kfatalProcessExit64(CPU64* cpu, U32 status, const char* reason);" \
            not in syscall_header:
        raise SystemExit(
            "a fatal guest process exit needs one shared entry point"
        )
    syscall_source = read(repository / "source/kernel/syscall64.cpp")
    require_ordered(
        syscall_source,
        [
            "void kfatalProcessExit64(CPU64* cpu, U32 status, const char* reason) {",
            "BOXEDWINE_X64_FATAL_EXIT",
            # The same two markers a guest exit_group emits.
            "CPU64: exit_group syscall, status=%u",
            "BOXEDWINE_X64_PROC_EXIT",
            # The same completion.
            "process->exitgroup(cpu->thread, status);",
            # The launched program's death ends the session; a helper's does not.
            "KSystem::shutingDown = true;",
            "KNativeSystem::postQuit();",
        ],
        "BoxedVN fatal guest process exit",
    )
    if "const bool sessionRoot = parentId <= 1;" not in syscall_source:
        raise SystemExit(
            "only the launched program's death may stop the emulator"
        )
    platform_source = read(
        repository / "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp")
    require_ordered(
        platform_source,
        [
            "BVNFEXCPU64Run(process.get(), cpu->thread, &outcome);",
            "if (outcome == BVNFEXCPU64RunOutcomeFatal) {",
            'kfatalProcessExit64(cpu64, 127,',
            "cpu->thread->terminating = true;",
        ],
        "BoxedVN fatal translator failure routing",
    )

    # ------------------------------------------------------------------ #
    # Detail budgeting keyed by address space. One global ordinal spent    #
    # the whole budget on the loader and the short-lived helpers, so the   #
    # 4,193,283 refused requests the failing process issued after the      #
    # Windows PE handoff were never described at all.                      #
    # ------------------------------------------------------------------ #
    mmap_diagnostics = read(repository / "include/guest_mmap_diagnostics.h")
    require_ordered(
        mmap_diagnostics,
        [
            "struct GuestMmapSignature {",
            "std::uint64_t addressSpace = 0;",
            "class GuestMmapDetailBudget {",
            "kLinesPerAddressSpace",
            "kTotalLines",
            "bool take(std::uint64_t processId,",
            "class GuestMmapRepeatTracker {",
            "kReportsPerSignature",
            "kTotalReports",
            "Outcome record(const GuestMmapSignature& signature) noexcept {",
            "static bool shouldReport(std::uint64_t seen) noexcept {",
            "return (seen & (seen - 1)) == 0;",
        ],
        "BoxedVN bounded mmap diagnostics contract",
    )
    # Both bounds must be real: fixed storage, no allocation, no growth.
    for banned in ("std::map", "std::vector", "std::unordered_map", "new "):
        if banned in mmap_diagnostics:
            raise SystemExit(
                "mmap diagnostics must use fixed storage keyed on nothing the "
                "guest controls: " + banned
            )
    # An exec is a new address space, so the same pid before and after the
    # Windows handoff cannot share a budget or a repeat count.
    memory_header_text = read(repository / "include/kmemory64.h")
    if "U64 addressSpaceGeneration() const" not in memory_header_text:
        raise SystemExit(
            "diagnostics must be able to tell one address space from the one "
            "an exec replaced"
        )
    if "generation = g_addressSpaceGeneration.fetch_add(" not in memory:
        raise SystemExit(
            "every address space must take its own generation at construction"
        )
    require_ordered(
        syscall_mmap,
        [
            '#include "guest_mmap_diagnostics.h"',
            "boxedvn::GuestMmapDetailBudget gGuestMmapDetailBudget;",
            "boxedvn::GuestMmapRepeatTracker gGuestMmapRepeats;",
            "signature.addressSpace = cpu->memory->addressSpaceGeneration();",
        ],
        "BoxedVN mmap diagnostics routing",
    )

    # ------------------------------------------------------------------ #
    # CMPXCHG16B: Wine's ntdll reaches f0 49 0f c7 08 and the interpreter      #
    # stopped there with "unimpl opcode".                                     #
    # ------------------------------------------------------------------ #
    cmpxchg = read(repository / "include/cmpxchg16b.h")
    require_ordered(
        cmpxchg,
        [
            "inline Cmpxchg16bResult evaluateCmpxchg16b(",
            "if (memoryLow == rax && memoryHigh == rdx) {",
            "result.writeLow = rbx;",
            "result.writeHigh = rcx;",
            "result.zeroFlag = true;",
            "result.raxAfter = memoryLow;",
            "result.rdxAfter = memoryHigh;",
            "result.zeroFlag = false;",
            "inline bool isCmpxchg16bEncoding(",
            "return (regField & 7) == 1 && rexW && !registerForm;",
        ],
        "BoxedVN CMPXCHG16B semantics contract",
    )
    interpreter64 = read(repository / "source/emulation/cpu/cpu64.cpp")
    require_ordered(
        interpreter64,
        [
            '#include "cmpxchg16b.h"',
            "if (op2 == 0xC7 && opSize == 8) {",
            "boxedvn::isCmpxchg16bEncoding(m.regField, opSize == 8,",
            "goto unhandled;",
            "cpu64AtomicLockFor(m.effAddr)",
            "boxedvn::evaluateCmpxchg16b(memLow, memHigh,",
            "if (swap.succeeded) {",
            "memory->writeq(m.effAddr, swap.writeLow);",
            "memory->writeq(m.effAddr + 8, swap.writeHigh);",
            "reg[X64_RAX].setU64(swap.raxAfter);",
            "reg[X64_RDX].setU64(swap.rdxAfter);",
            "if (swap.zeroFlag) rflags |= X64_ZF;",
            "U32 used = opOff + 2 + m.length;",
        ],
        "BoxedVN CMPXCHG16B interpreter contract",
    )
    # ZF is the architectural result; the SDM leaves the rest undefined, and
    # flagsSub would invent them.
    cmpxchg_start = interpreter64.index("if (op2 == 0xC7 && opSize == 8) {")
    cmpxchg_body = interpreter64[cmpxchg_start:cmpxchg_start + 2400]
    if "flagsSub" in cmpxchg_body:
        raise SystemExit(
            "CMPXCHG16B must not synthesise the flags the SDM leaves undefined"
        )

    # ------------------------------------------------------------------ #
    # The Wine C: drive link, and the diagnostics it used to drown.           #
    # ------------------------------------------------------------------ #
    prefix_header = read(repository / "include/guest_wine_prefix.h")
    require_ordered(
        prefix_header,
        [
            '#define K_GUEST_WINE_DRIVE_C "drive_c"',
            '#define K_GUEST_WINE_DOSDEVICES "dosdevices"',
            '#define K_GUEST_WINE_C_LINK "c:"',
            '#define K_GUEST_WINE_C_LINK_TARGET "../drive_c"',
            "inline GuestWinePrefixSetup planGuestWinePrefixSetup(",
            "setup.createDriveCLink = !driveCLinkExists;",
        ],
        "BoxedVN Wine prefix completion contract",
    )
    startup = read(repository / "source/sdl/startupArgs.cpp")
    require_ordered(
        startup,
        [
            'const BString wineDriveC = winePrefix + "/" K_GUEST_WINE_DRIVE_C;',
            "boxedvn::planGuestWinePrefixSetup(",
            "if (prefixSetup.createDriveC) {",
            "if (prefixSetup.createDosDevices) {",
            "if (prefixSetup.createDriveCLink) {",
            "Fs::addFileNode(wineDriveCLink, B(K_GUEST_WINE_C_LINK_TARGET),",
            "BOXEDWINE_X64_PREFIX path=%s arch=%s dosdevices=%s ",
            "drive_c=%s c_link=%s cwd=%s",
        ],
        "BoxedVN Wine prefix startup contract",
    )
    # The prefix is the resolved one, never a hardcoded .wine64.
    for line in code_lines(startup):
        if ".wine64" in line:
            raise SystemExit(
                "prefix setup must use the resolved WINEPREFIX, not a "
                "hardcoded .wine64: " + line
            )

    require_ordered(
        syscall_mmap,
        [
            "cpu->failedOpenReports.record(",
            "FailedOpenDecision::Detailed",
            "FailedOpenDecision::Repeat",
            "FailedOpenDecision::Suppress",
            "further reports for this path are suppressed",
        ],
        "BoxedVN bounded failed-open reporting contract",
    )
    # The bounded unsupported-syscall limiter stays exactly as it is: the
    # device logs show it holding a run to about 270 KB.
    require_ordered(
        read(repository / "include/cpu64.h"),
        [
            "boxedvn::BoundedSyscallReportLimiter unsupportedSyscallReports;",
            "boxedvn::BoundedSyscallReportLimiter failedOpenReports;",
        ],
        "BoxedVN diagnostic limiter contract",
    )

    # ------------------------------------------------------------------ #
    # Wine's top-down arena. Wine reserves inside [.., 0x7fffffff0000) and    #
    # commits accessible subranges; every one was refused as outside the      #
    # identity lane, and the search that followed reached 8,388,609 mmaps at  #
    # about 98% of a core. The arena was then 32 MiB and Wine's own           #
    # try_map_free_area walked off the bottom of it; the base is now as far   #
    # down as clearing the mask can reach without leaving the alias block.    #
    # ------------------------------------------------------------------ #
    alias_header = read(repository / "include/guest_low_alias.h")
    require_ordered(
        alias_header,
        [
            "inline constexpr std::uint64_t kGuestTopBase = 0x7FFF80000000ULL;",
            "inline constexpr std::uint64_t kGuestTopEnd = 0x7FFFFFFF0000ULL;",
            "inline constexpr std::uint64_t kGuestTopClearMask = 0x7F8000000000ULL;",
            "kGuestTopBase & ~kGuestTopClearMask;",
            # The invariants that make clearing a bit field exact.
            "(kGuestTopBase & kGuestTopClearMask) == kGuestTopClearMask,",
            "((kGuestTopEnd - 1) & kGuestTopClearMask) == kGuestTopClearMask,",
            "(kGuestHighEnd & kGuestTopClearMask) == 0,",
            "((kGuestLowLimit - 1) & kGuestTopClearMask) == 0,",
            "((kGuestLowAliasEnd - 1) & kGuestTopClearMask) == 0,",
            "((kGuestAliasBlockEnd - 1) & kGuestTopClearMask) == 0,",
            "kGuestTopHostBase >= kGuestHighEnd,",
            # Injectivity: the relocated image has to stay inside the block, or
            # two arena addresses would collide on one host address.
            "static_assert(kGuestTopHostBase >= kGuestLowAliasBase &&",
            "kGuestTopHostEnd <= kGuestAliasBlockEnd,",
            "static_assert(kGuestTopHostBase == kGuestHighEnd,",
            "(kGuestTopHostEnd - 1) < kGuestTopBase,",
            "inline constexpr bool isTopArenaGuestAddress(",
            "return (guestAddress | kGuestLowAliasBase) & ~kGuestTopClearMask;",
            "inline constexpr bool isTopArenaHostAddress(",
            "return hostAddress | kGuestTopClearMask;",
            "return address >= kGuestTopBase && end <= kGuestTopEnd;",
        ],
        "BoxedVN guest top-arena alias contract",
    )

    # ------------------------------------------------------------------ #
    # The identity lane, and the block that bounds every lane.             #
    #                                                                      #
    # The lane used to stop one kGuestLowLimit above its base because the   #
    # invariant was written as "must not cross an alias-base bit boundary", #
    # which is sufficient for a lane of exactly that size and refuses every  #
    # larger one that is equally exact. The invariant that actually holds is #
    # containment in the block that begins at the alias base and spans its    #
    # LOWEST set bit: adding less than that bit cannot clear a base bit, so    #
    # the OR is the identity for every address in it.                          #
    # ------------------------------------------------------------------ #
    require_ordered(
        alias_header,
        [
            "inline constexpr std::uint64_t kGuestAliasBlockSpan =",
            "kGuestLowAliasBase & (~kGuestLowAliasBase + 1);",
            "inline constexpr std::uint64_t kGuestAliasBlockEnd =",
            "kGuestLowAliasBase + kGuestAliasBlockSpan;",
            "inline constexpr std::uint64_t kGuestHighEnd = 0x7F80000000ULL;",
            "static_assert(kGuestLowAliasBase != 0,",
            "static_assert((kGuestLowAliasBase & (kGuestAliasBlockSpan - 1)) "
            "== 0,",
            "static_assert(kGuestHighBase >= kGuestLowAliasBase &&",
            "kGuestHighBase < kGuestHighEnd &&",
            "kGuestHighEnd <= kGuestAliasBlockEnd,",
        ],
        "BoxedVN identity lane containment contract",
    )
    # The superseded test must be gone, not merely joined by its replacement:
    # leaving it in place would refuse the very lane its replacement admits.
    if "(kGuestHighBase ^ (kGuestHighEnd - 1)) < kGuestLowLimit" in alias_header:
        raise SystemExit(
            "the identity lane's crossing test was replaced by block "
            "containment; leaving it in refuses the enlarged lane"
        )

    memory_header = read(repository / "include/kmemory64.h")
    require_ordered(
        memory_header,
        [
            "#define K64_NATIVE_ALIAS_BLOCK_SPAN   (K64_NATIVE_LOW_ALIAS_BASE &",
            "#define K64_NATIVE_ALIAS_BLOCK_END    (K64_NATIVE_LOW_ALIAS_BASE +",
            "#define K64_NATIVE_TOP_GUEST_BASE     0x7FFF80000000ULL",
            "#define K64_NATIVE_TOP_GUEST_END      0x7FFFFFFF0000ULL",
            "#define K64_NATIVE_TOP_CLEAR_MASK     0x7F8000000000ULL",
            # The identity lane's end is derived from the arena's host block,
            # so the two cannot be moved independently and the block cannot
            # acquire a gap the guest is never offered.
            "#define K64_NATIVE_GUEST_HIGH_END     K64_NATIVE_TOP_HOST_BASE",
            # Mirrored constants must be asserted equal, not merely similar.
            "K64_NATIVE_TOP_GUEST_BASE == boxedvn::kGuestTopBase,",
            "K64_NATIVE_TOP_GUEST_END == boxedvn::kGuestTopEnd,",
            "K64_NATIVE_TOP_CLEAR_MASK == boxedvn::kGuestTopClearMask,",
            "K64_NATIVE_TOP_HOST_BASE == boxedvn::kGuestTopHostBase,",
            "K64_NATIVE_TOP_HOST_BASE >= K64_NATIVE_GUEST_HIGH_END,",
            "K64_NATIVE_TOP_HOST_END <= K64_NATIVE_GUEST_WINDOW_END,",
            "static_assert(K64_NATIVE_TOP_HOST_BASE >= K64_NATIVE_LOW_ALIAS_BASE &&",
            "K64_NATIVE_TOP_HOST_END <= K64_NATIVE_ALIAS_BLOCK_END,",
            "static_assert(K64_NATIVE_TOP_HOST_BASE == K64_NATIVE_GUEST_HIGH_END,",
            "static inline U64 k64GuestToHostAddress(U64 guestAddress) {",
            "return (guestAddress | K64_NATIVE_LOW_ALIAS_BASE) &",
            "~K64_NATIVE_TOP_CLEAR_MASK;",
            "if (k64IsTopArenaHostAddress(hostAddress)) {",
            "return hostAddress | K64_NATIVE_TOP_CLEAR_MASK;",
        ],
        "BoxedVN address-space top-arena contract",
    )

    memory = read(repository / "source/kernel/kmemory64.cpp")
    require_ordered(
        memory,
        [
            "bool KMemory64::nativeGuestRangeAllowed(",
            "end <= K64_NATIVE_GUEST_HIGH_END) {",
            "return addr >= K64_NATIVE_TOP_GUEST_BASE &&",
            "end <= K64_NATIVE_TOP_GUEST_END;",
        ],
        "BoxedVN top-arena hostability contract",
    )
    # Every native operation names host addresses through the translation, and
    # the guest page map through canonical ones. Mixing the two looked up an
    # unrelated page for anything served through an alias.
    for required in (
        # The union over a host page's guest subpages, and the reconciliation
        # that applies it, both name the subpages canonically.
        "const U64 guestOfHostPage = k64HostToGuestAddress(hostPageStart);",
        "const U64 guestOfHostPage = k64HostToGuestAddress(host);",
        # And the reconciliation touches a host page only when the round trip
        # proves it is the image of a guest address this space serves.
        "if (k64GuestToHostAddress(k64HostToGuestAddress(host)) != host ||",
        "k64NativeAlignDown(k64GuestToHostAddress(addr), hostPage);",
    ):
        if required not in memory:
            raise SystemExit(
                "a native operation still confuses host and guest addresses: " +
                required
            )
    for line in code_lines(memory):
        for banned in ("k64NativeAlignDown(addr,", "k64NativeAlignUp(addr +"):
            if banned in line:
                raise SystemExit(
                    "a host page range must be computed from the translated "
                    "address: " + line
                )

    # The translator has to be told about the lane, or a block would
    # dereference the canonical arena at an address the host cannot map.
    backend = read(repository / "ios/runtime/src/BVNFEXBackend.mm")
    require_ordered(
        backend,
        [
            "bundle->context->SetGuestLowAlias(boxedvn::kGuestLowAliasBase,",
            "bundle->context->SetGuestTopClearMask(boxedvn::kGuestTopClearMask);",
            "BOXEDWINE_FEX64_TOP_ALIAS guest=[0x%llx,0x%llx) ",
            "bundle->context->InitCore()",
        ],
        "BoxedVN translator top-alias publication contract",
    )

    alias_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-low-address-alias.patch")
    require_ordered(
        alias_patch,
        [
            "+  void SetGuestTopClearMask(uint64_t Mask) override {",
            "+    GuestTopClearMask = Mask;",
            "+  uint64_t GetGuestTopClearMask() const {",
            "+    return (GuestAddress | GuestLowAliasBase) & ~GuestTopClearMask;",
            "+  uint64_t GuestTopClearMask {};",
            # The patch is generated file by file: the JIT class declaration
            # precedes its definition, and the public virtual comes last.
            "+  void ApplyGuestTopRelocation(ARMEmitter::Register Reg);",
            "+void Arm64JITCore::ApplyGuestTopRelocation(ARMEmitter::Register Reg) {",
            "+  bic(ARMEmitter::Size::i64Bit, Reg, Reg, ClearMask);",
            "+  FEX_DEFAULT_VISIBILITY virtual void SetGuestTopClearMask(uint64_t Mask) {",
        ],
        "BoxedVN FEX top-arena translation contract",
    )
    # Every path that ORs the alias base itself must relocate the same
    # register too, or that path alone keeps pointing at the unmappable
    # canonical arena. This is checked per register rather than by counting
    # one temporary: MemSet and MemCpy use TMP2/TMP3, and a TMP4-only count
    # is exactly why their missing relocation reached a device, where the
    # vectorised copy loop faulted dereferencing 0x7fffffa10188.
    patch_lines = alias_patch.split(newline)
    aliased = 0
    for index, line in enumerate(patch_lines):
        if not line.startswith("+"):
            continue
        if "orr(ARMEmitter::Size::i64Bit, TMP" not in line:
            continue
        if "AliasBase)" not in line:
            continue
        register = line.split("TMP", 1)[1].split(",")[0]
        register = "TMP" + register.strip()
        aliased += 1
        # The relocation may follow another OR for a second pointer, so look
        # a few lines ahead rather than only at the next one.
        window = newline.join(patch_lines[index + 1:index + 6])
        if "ApplyGuestTopRelocation(" + register + ")" not in window:
            raise SystemExit(
                "an alias OR on " + register + " is not followed by the top "
                "relocation, so that path would dereference the canonical "
                "arena: " + line.strip())
    if aliased < 5:
        raise SystemExit(
            "expected every aliasing memory path to be covered; found only " +
            str(aliased))
    # A compare inside operand generation would corrupt guest flags, which FEX
    # keeps in the host NZCV register.
    reloc_start = alias_patch.index(
        "+void Arm64JITCore::ApplyGuestTopRelocation(")
    reloc_body = alias_patch[reloc_start:reloc_start + 500]
    for banned in ("tst(", "cmp(", "csel(", "ands("):
        if banned in reloc_body:
            raise SystemExit(
                "the top relocation must not touch the flags FEX keeps guest "
                "state in: " + banned)

    # Behaviour, not source text: the emitter check drives the real emitter and
    # the guest fixture runs the real JIT.
    emitter_check = read(
        repository / "scripts/guest-probes/fex64-emitter-encoding-check.cpp")
    require_ordered(
        emitter_check,
        [
            "void checkGuestAddressTranslationEncodings() {",
            "constexpr uint64_t kGuestLowAliasBase = 0x7800000000ULL;",
            "constexpr uint64_t kGuestTopClearMask = 0x7F8000000000ULL;",
            "emitter.orr(ARMEmitter::Size::i64Bit, ARMEmitter::Register(13),",
            "emitter.bic(ARMEmitter::Size::i64Bit, ARMEmitter::Register(13),",
            # stp/ldp are IndexType templates with no plain overload, a fact
            # invisible in the signature. Drive the real emitter with the
            # exact calls the aliased stack paths make, and prove the
            # addressing mode carries no writeback.
            "void checkStackAccessAddressingModes() {",
            "emitter.stp<ARMEmitter::IndexType::OFFSET>(",
            "emitter.ldp<ARMEmitter::IndexType::OFFSET>(",
            "emitter.stur(ARMEmitter::XRegister(4), ARMEmitter::Register(13), 0);",
            "emitter.ldur(ARMEmitter::XRegister(4), ARMEmitter::Register(13), 0);",
            "checkGuestAddressTranslationEncodings();",
            "checkStackAccessAddressingModes();",
        ],
        "BoxedVN emitter translation encoding contract",
    )
    # The copies in the emitter tool have to match the runtime's own values.
    for constant in ("0x7800000000ULL", "0x7F8000000000ULL"):
        if constant not in alias_header:
            raise SystemExit(
                "the emitter check's constant is not the runtime's: " +
                constant)

    # The stack ops used ARM64 pre/post-indexed forms on the guest-visible
    # address register. That writeback cannot be aliased: it would put a host
    # address into RSP. They kept dereferencing the canonical stack instead,
    # and Wine's ntdll spun on a `push rbp` at guest 0x7a402602e4 writing
    # 0x7ffcfc78, ninety-three million handled faults.
    require_ordered(
        alias_patch,
        [
            "+  if (CTX->GetGuestLowAliasBase()) {",
            "+    sub(AddrSize, Dst, AddrSrc, ValueSize);",
            "+    const auto HostAddr = AliasGuestAddress(Dst);",
            "+    case 8: stur(Value.X(), HostAddr, 0); break;",
            '+    LOGMAN_THROW_A_FMT(Dst != Src1 && Dst != Src2, "PushTwo address must not overlap its values");',
            "+    sub(ARMEmitter::Size::i64Bit, Dst, Dst, 2 * ValueSize);",
            "+    case 8: stp<ARMEmitter::IndexType::OFFSET>(Src1.X(), Src2.X(), HostAddr, 0); break;",
            "+    const auto HostAddr = AliasGuestAddress(Addr);",
            "+    case 8: ldur(Dst.X(), HostAddr, 0); break;",
            "+    add(ARMEmitter::Size::i64Bit, Addr, Addr, Size);",
            "+    case 8: ldp<ARMEmitter::IndexType::OFFSET>(Dst1.X(), Dst2.X(), HostAddr, 0); break;",
            "+    add(ARMEmitter::Size::i64Bit, Addr, Addr, 2 * Size);",
        ],
        "BoxedVN aliased stack operation contract",
    )
    # Whatever the aliased path does, it must not index off the canonical
    # address register: the writeback is the whole problem.
    added = [line for line in alias_patch.split(newline) if line.startswith("+")]
    depth = 0
    inside = False
    for line in added:
        body = line[1:]
        if not inside and "if (CTX->GetGuestLowAliasBase()) {" in body:
            inside = True
            depth = 0
        if not inside:
            continue
        depth += body.count("{") - body.count("}")
        for banned in ("IndexType::PRE", "IndexType::POST"):
            if banned in body:
                raise SystemExit(
                    "an aliased stack path must not use an indexed form whose "
                    "writeback would put a host address in the guest's stack "
                    "pointer: " + body.strip())
        if depth <= 0:
            inside = False
    # And each of the four has to reach the alias at all.
    aliasedStackPaths = alias_patch.count(
        "+  if (CTX->GetGuestLowAliasBase()) {")
    if aliasedStackPaths != 4:
        raise SystemExit(
            "Push, PushTwo, Pop and PopTwo each need an aliased path; found " +
            str(aliasedStackPaths))

    # A fixture that runs with the translation disabled cannot catch a
    # missing translation. The alias-enabled harness path exists for exactly
    # that, and the values it publishes must be the runtime's own.
    harness_patch = read(
        repository / "scripts/fex64-patches/fex-boxedwine-harness-alias.patch")
    require_ordered(
        harness_patch,
        [
            '+  const char* const BoxedWineAliasEnv = std::getenv("FEX_BOXEDWINE_ALIAS");',
            "+  constexpr uint64_t BoxedWineLowAliasBase = 0x7800000000ULL;",
            "+  constexpr uint64_t BoxedWineLowGuestLimit = 0x200000000ULL;",
            "+  constexpr uint64_t BoxedWineTopClearMask = 0x7f8000000000ULL;",
            "+    CTX->SetGuestLowAlias(BoxedWineLowAliasBase, BoxedWineLowGuestLimit);",
            "+    CTX->SetGuestTopClearMask(BoxedWineTopClearMask);",
            "   if (!CTX->InitCore()) {",
            "+    auto MapForFixture = [&](uint64_t Address, size_t Size) -> void* {",
            "+          DoMmap(Host, Size);",
            "+    if (!Loader.MapMemory(MapForFixture)) {",
        ],
        "BoxedVN alias-enabled harness contract",
    )
    # Off by default, so every other harness run is unchanged.
    if "BoxedWineAliasEnv[0] != 0" not in harness_patch:
        raise SystemExit(
            "an empty FEX_BOXEDWINE_ALIAS must leave the harness unchanged")
    # The harness's copies of the constants have to be the runtime's.
    for constant in ("0x7800000000ULL", "0x200000000ULL", "0x7f8000000000ULL"):
        if constant.lower() not in alias_header.lower():
            raise SystemExit(
                "the harness's alias constant is not the runtime's: " +
                constant)
    builder = read(repository / "scripts/build-fex64-fex.sh")
    if "apply_patch fex-boxedwine-harness-alias.patch" not in builder:
        raise SystemExit(
            "every maintained patch must be applied by the FEX builder")

    probe = read(repository / "scripts/run-fex64-vixl-probe.sh")
    workflow_key = read(repository / ".github/workflows/build-ios.yml")
    require_ordered(
        probe,
        [
            'fixture_top_alias="${root}/scripts/guest-probes/fex64-top-alias-contract.asm"',
            'fixture_top_alias_repmov="${root}/scripts/guest-probes/fex64-top-alias-repmov.asm"',
            'prepare_fixture "${fixture_top_alias}" fex64-top-alias',
            'prepare_fixture "${fixture_top_alias_repmov}" fex64-top-alias-repmov',
            'run_one x64-top-alias "${tmp_dir}/fex64-top-alias.bin"',
            'run_one x64-top-alias-repmov "${tmp_dir}/fex64-top-alias-repmov.bin"',
            'run_one x64-top-alias-stack "${tmp_dir}/fex64-top-alias-stack.bin"',
        ],
        "BoxedVN top-arena VIXL probe contract",
    )
    stack_fixture = read(
        repository / "scripts/guest-probes/fex64-top-alias-stack.asm")
    for required in ("push rbp", "push rsp", "pop rcx", "call callee", "ret",
                     "%define LOW_STACK_TOP 0x7ffcfc80",
                     "%define TOP_STACK_TOP 0x7ffffe008000"):
        if required not in stack_fixture:
            raise SystemExit(
                "the stack fixture must exercise the operations that spun on "
                "device: " + required)
    # The harness maps the canonical page and its alias, so a push proved by
    # its own pop proves nothing. Each side has to cross into the other world.
    for required in ("mov rcx, qword [r10]", "mov qword [r10], rdx"):
        if required not in stack_fixture:
            raise SystemExit(
                "the stack fixture must check a push with a translated load "
                "and feed a pop with a translated store: " + required)
    if "scripts/guest-probes/fex64-top-alias-stack.asm" not in workflow_key:
        raise SystemExit(
            "the stack fixture must participate in the VIXL cache key")
    # The alias is turned on for that fixture and nothing else.
    if 'FEX_BOXEDWINE_ALIAS="${boxedwine_alias}"' not in probe:
        raise SystemExit(
            "the probe must pass the alias mode explicitly rather than rely "
            "on a temporary assignment in front of a shell function")
    for line in probe.split(newline):
        if "run_one " not in line or "repmov" in line:
            continue
        if line.rstrip().endswith(" 1") and line.count('"') == 2:
            raise SystemExit(
                "only the alias-enabled fixture may request the alias: " +
                line.strip())
    repmov = read(
        repository / "scripts/guest-probes/fex64-top-alias-repmov.asm")
    for required in ("rep movsq", "rep movsb", "rep stosq", "rep stosb",
                     "std", "cld",
                     "%define ARENA_SRC 0x7ffffe000000",
                     "%define ARENA_DST 0x7ffffe010000",
                     "%define LOW_BUF   0x100000"):
        if required not in repmov:
            raise SystemExit(
                "the alias-enabled fixture must exercise the string ops that "
                "faulted: " + required)
    if "scripts/guest-probes/fex64-top-alias-repmov.asm" not in workflow_key:
        raise SystemExit(
            "the alias-enabled fixture must participate in the VIXL cache key")
    fixture = read(
        repository / "scripts/guest-probes/fex64-top-alias-contract.asm")
    for required in ("mov r10, ARENA", "lock xadd", "lock cmpxchg",
                     "%define ARENA 0x7ffffe000000",
                     "mov r10, 0x7ffffffefff8"):
        if required not in fixture:
            raise SystemExit(
                "the top-arena fixture must exercise real accesses: " +
                required)
    workflow = read(repository / ".github/workflows/build-ios.yml")
    if "scripts/guest-probes/fex64-top-alias-contract.asm" not in workflow:
        raise SystemExit(
            "the top-arena fixture must participate in the VIXL cache key")

    # ------------------------------------------------------------------ #
    # The guest uses 4 KiB pages and iOS uses 16 KiB, so a guest mapping is   #
    # rounded out to the enclosing host pages and routinely shares one with   #
    # the request before it. glibc's heap does this on every brk growth, and  #
    # the mixture of tracked and untracked host pages was refused outright.   #
    # ------------------------------------------------------------------ #
    plan_header = read(repository / "include/native_map_plan.h")
    require_ordered(
        plan_header,
        [
            "struct NativeHostRun {",
            "bool reused = false;",
            "GapOccupied = 2,",
            "bool exactHostCover = false;",
            "std::vector<NativeHostRun> runs;",
            "inline NativeMapPlan planNativeAnonymousMap(",
            "const bool isReused = covered(context, page, page + hostPageSize);",
            "plan.status = NativeMapPlanStatus::GapOccupied;",
            "plan.status = NativeMapPlanStatus::Ok;",
        ],
        "BoxedVN native map planning contract",
    )

    memory_impl = read(repository / "source/kernel/kmemory64.cpp")
    require_ordered(
        memory_impl,
        [
            '#include "native_map_plan.h"',
            "bool KMemory64::nativeMapAnonymous(",
            "boxedvn::planNativeAnonymousMap(",
            "for (const boxedvn::NativeHostRun& run : plan.runs) {",
            "if (run.reused) {",
            "k64NativeReserveHostRange(run.start, run.length)",
            "::mmap((void*)(uintptr_t)run.start, (size_t)run.length,",
            "::memset((void*)(uintptr_t)hostAddr, 0, (size_t)len);",
            # The interval is recorded read/write and the CALLER narrows it per
            # host page from the page map. Choosing a protection here could
            # only be done from plan.exactHostCover, a property of the whole
            # interval, which is wrong for every host page it shares.
            "nativeTrackRangeLocked(hostStart, hostLength, 0x3u);",
            "BOXEDWINE_X64_NATIVE_EXTEND guest=[0x%llx,0x%llx) ",
        ],
        "BoxedVN partial native map contract",
    )
    # The refusal this replaces is gone, and no single MAP_FIXED may span the
    # whole mixed interval any more.
    if "refusing partially tracked native map" in memory_impl:
        raise SystemExit(
            "a partially tracked host interval must be planned, not refused")
    map_start = memory_impl.index("bool KMemory64::nativeMapAnonymous(")
    map_end = memory_impl.index(newline + "bool KMemory64::nativeMapKuserAlias(",
                                map_start)
    map_body = memory_impl[map_start:map_end]
    if "::mmap((void*)(uintptr_t)hostStart" in map_body:
        raise SystemExit(
            "the whole enclosing interval must never be mapped in one "
            "destructive MAP_FIXED: it would replace the tracked pages it "
            "was asked to preserve")
    # Only the requested guest bytes are zeroed. Zeroing the enclosing host
    # pages would destroy the neighbouring guest subpages.
    if "::memset((void*)(uintptr_t)hostStart" in map_body:
        raise SystemExit(
            "only the requested guest bytes may be zeroed, never the "
            "enclosing host pages")
    # A failure must undo only what this call created.
    for required in ("auto rollback = [&created]()", "rollback();",
                     "it->mapped", "it->reserved"):
        if required not in map_body:
            raise SystemExit(
                "a failed partial map must roll back only its own "
                "reservations and mappings: " + required)
    # An existing neighbour keeps its access, and this is no longer decided
    # here. plan.exactHostCover is a property of the WHOLE interval: applying
    # it left a PROT_NONE reservation whose length was not a multiple of the
    # host page read/write everywhere, and applied the request's protection to
    # host pages whose other guest subpages were never named. The mapping path
    # now leaves the interval read/write and the caller narrows it host page by
    # host page from the page map, which is the only value that is right for
    # all four guest pages of a host page at once.
    if "exactHostCover ?" in map_body:
        raise SystemExit(
            "a host page's protection must be the union of its guest "
            "subpages, not a property of the whole request")
    reconcile_start = memory_impl.index(
        "void KMemory64::nativeReconcileHostProt(")
    reconcile_end = memory_impl.index(newline + "}" + newline, reconcile_start)
    reconcile_body = memory_impl[reconcile_start:reconcile_end]
    for required in (
        # One host page at a time, from the shared union.
        "for (U64 host = hostStart; host < hostEnd; host += hostPage) {",
        "nativeHostPageProtLocked(host, hostPage,",
        "k64NativeProt(runProt)",
        # An untracked host page is skipped, never a reason to fail the call:
        # refusing the whole request left the guest pages it named holding the
        # rights of the reservation they were being committed out of.
        "!nativeRangeCovers(host, host + hostPage)",
    ):
        if required not in reconcile_body:
            raise SystemExit(
                "the host protection must be reconciled per host page from "
                "the page map: " + required)
    union_start = memory_impl.index("U32 KMemory64::nativeHostPageProtLocked(")
    union_end = memory_impl.index(newline + "}" + newline, union_start)
    union_body = memory_impl[union_start:union_end]
    for required in (
        "U32 unionProt = 0;",
        "unionProt |= k64GuestProtFromPageFlags(it->second->flags);",
        "if (it->second->dataShared) unionProt |= 0x3u;",
    ):
        if required not in union_body:
            raise SystemExit(
                "the union over a host page's guest subpages lost a term: " +
                required)
    # A guest page's rights may only be written by a call that names it, so
    # nothing may assign K64Page::flags outside its single writer.
    for banned in ("->flags = ", "->flags |= ", "->flags &= "):
        if banned in memory_impl:
            raise SystemExit(
                "a guest page's flags must be written only through "
                "K64Page::writeFlags: " + banned)

    # Linux brk semantics are unchanged: the new break is returned as asked,
    # not rounded out to a host page.
    syscalls = read(repository / "source/kernel/syscall64.cpp")
    brk_start = syscalls.index("U64 sys_brk64(")
    brk_body = syscalls[brk_start:brk_start + 4000]
    for banned in ("k64NativeAlignUp(newBrk", "k64NativeHostPageSize()"):
        if banned in brk_body:
            raise SystemExit(
                "sys_brk64 must return the requested break, not one rounded "
                "to the host page: " + banned)

    # ------------------------------------------------------------------ #
    # A guest that exits non-zero after a blocked read leaves nothing behind.  #
    # The device shows the main process parked in read(fd=9, count=16) -- the  #
    # Wine-server reply header -- and then exiting 1, with no record of what   #
    # that read returned. The tail is collected always and printed only on a   #
    # failing exit.                                                            #
    # ------------------------------------------------------------------ #
    tail_header = read(repository / "include/syscall_tail_ring.h")
    require_ordered(
        tail_header,
        [
            "enum class SyscallTailState : std::uint8_t {",
            "Pending = 1,",
            "Completed = 2,",
            "Restarted = 3,",
            "static constexpr unsigned kCapacity = 96;",
            "std::uint64_t begin(",
            "record.state = SyscallTailState::Pending;",
            "void complete(",
            "void restart(",
            "void forEach(",
            "bool claimDump() {",
            "if (record.sequence != token) {",
        ],
        "BoxedVN syscall tail ring contract",
    )
    # Fixed storage: a guest making millions of syscalls must cost a fixed
    # amount of memory and produce a fixed number of lines.
    for banned in ("std::vector", "std::deque", "std::map", "new "):
        if banned in tail_header:
            raise SystemExit(
                "the syscall tail must use fixed storage, not " + banned)

    syscalls = read(repository / "source/kernel/syscall64.cpp")
    require_ordered(
        syscalls,
        [
            "static void dumpX64ExitDiagnostics(CPU64* cpu, U64 status) {",
            "if (!process->syscallTail.claimDump()) return;",
            "X64_EXIT_DIAG pid=%u status=%llu exe='%s' cmd='%s' ",
            "rip=0x%llx syscall_rip=0x%llx rsp=0x%llx rax=0x%llx ",
            "fs=0x%llx gs=0x%llx",
            "X64_EXIT_TAIL pid=%u recorded=%llu (oldest first)",
            'crashRingDump("non-zero exit");',
            "dumpX64DescendantSnapshot(process);",
            "tailToken = tailProcess->syscallTail.begin(",
            "cpu->thread->process->syscallTail.restart(tailToken);",
            "cpu->thread->process->syscallTail.complete(tailToken, (S64)ret);",
        ],
        "BoxedVN exit diagnostic contract",
    )
    # Only a failing exit prints anything.
    exit_start = syscalls.index("static U64 sys_exit64(")
    exit_body = syscalls[exit_start:exit_start + 3000]
    # The guard has to be on the call itself, not merely somewhere nearby.
    guarded = ("    if (status != 0) {" + newline +
               "        dumpX64ExitDiagnostics(cpu, status);" + newline +
               "    }")
    if guarded not in exit_body:
        raise SystemExit(
            "the exit diagnostic must fire only for a non-zero exit status")
    # And it must run before anything is torn down.
    diag_at = exit_body.index("dumpX64ExitDiagnostics(cpu, status);")
    teardown_at = exit_body.index("process->exitgroup(cpu->thread")
    if diag_at > teardown_at:
        raise SystemExit(
            "the exit diagnostic must run before the process is torn down, "
            "while its descendants and register state are still real")
    # A pending record must never be printed with a result.
    tail_dump_start = syscalls.index("X64_EXIT_TAIL pid=%u recorded=")
    tail_dump = syscalls[tail_dump_start:tail_dump_start + 2600]
    if "state == boxedvn::SyscallTailState::Completed" not in tail_dump or             "-> none state=%s" not in tail_dump:
        raise SystemExit(
            "a syscall that never returned must not be printed as a completed "
            "result: an invented zero reads as EOF")

    # Both outcomes of a read reach the socket ring, not only a success.
    read_start = syscalls.index("static U64 sys_read64(")
    read_body = syscalls[read_start:syscalls.index("static U64 sys_openat64(",
                                                   read_start)]
    for required in ("crashRingRecordRead('E'", "crashRingRecordRead('R'"):
        if required not in read_body:
            raise SystemExit(
                "the read recorder must capture an error and a success alike, "
                "or it cannot say what the reply-header read returned: " +
                required)

    # The bounded lifecycle markers, so a forked daemon's scheduling, first
    # syscall and exit can be matched up.
    for marker, where in (
        ("BOXEDWINE_X64_PROC_FIRST_SYSCALL pid=%u parent=%u tid=%u ",
         "source/kernel/syscall64.cpp"),
        ("BOXEDWINE_X64_PROC_EXIT pid=%u parent=%u status=%lld ",
         "source/kernel/syscall64.cpp"),
        ("BOXEDWINE_X64_PROC_SCHEDULED pid=%u parent=%u tid=%u ",
         "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp"),
    ):
        if marker not in read(repository / where):
            raise SystemExit(
                "a process lifecycle marker is missing from " + where + ": " +
                marker)
    # Bounded by a per-process flag, not by a counter that could run away.
    process_header = read(repository / "include/kprocess.h")
    for required in ("boxedvn::SyscallTailRing syscallTail;",
                     "bool scheduleReported = false;",
                     "bool firstSyscallReported = false;"):
        if required not in process_header:
            raise SystemExit(
                "the lifecycle markers must be one-shot per process: " +
                required)

    # The descendant snapshot stays generic and bounded: no observed pid may
    # be written into the emulator.
    snapshot_start = syscalls.index("static void dumpX64DescendantSnapshot(")
    snapshot = syscalls[snapshot_start:syscalls.index(
        "static void dumpX64ExitDiagnostics(", snapshot_start)]
    if "kMaxProcesses" not in snapshot:
        raise SystemExit("the descendant snapshot must be bounded")
    if "child->parentId != parent->id" not in snapshot:
        raise SystemExit(
            "the descendant snapshot must be found by parentage, never by a "
            "hardcoded pid")

    # Only the x86-64 graphics probe turns the socket rings on, and it uses
    # the bounded ones rather than the unbounded firehose.
    runtime = read(repository / "ios/runtime/src/BVNRuntime.mm")
    require_ordered(
        runtime,
        [
            "if (launch.useFEX64 && launch.useDXMT) {",
            'setenv("BW64_CRASHRING", "1", 1);',
            'setenv("BW64_WSREAD", "1", 1);',
        ],
        "BoxedVN probe diagnostic contract",
    )
    # A comment may explain why the firehose was not chosen; enabling it is a
    # different matter.
    if 'setenv("BW64_IPCDUMP"' in runtime:
        raise SystemExit(
            "the probe must not enable the unbounded message dump")

    # ---------------------------------------------------------------------
    # The loader handoff: the Wine-server reply that admits the process, the
    # bounded trace armed over the transition that follows it, and the
    # descriptor table the transition indexes into.
    # ---------------------------------------------------------------------

    # The exchange is recognised by its opcode and the reply's size, never by
    # a pid or an entry address one log happened to contain.
    require_ordered(
        syscalls,
        [
            "boxedvn::wineServerRequestOpcode(",
            "lastServerRequestOpcode = opcode;",
            "K_WINE_REQ_INIT_PROCESS_DONE",
            "got == K_WINE_SERVER_MESSAGE_BYTES",
            "boxedvn::decodeWineInitProcessDoneReply(",
            "BOXEDWINE_X64_INIT_DONE_REPLY pid=%u error=%u ",
            "suspend=%d",
            "BVNFEXBackendArmHandoffTrace(",
        ],
        "init_process_done reply contract",
    )
    # The detection is bounded to the region that decodes the reply, and
    # nothing in it may compare against a value a log happened to contain: a
    # detector keyed on one run's pid or entry point proves nothing about the
    # next one.
    detection_start = syscalls.index("boxedvn::decodeWineInitProcessDoneReply(")
    detection = syscalls[syscalls.rindex("{", 0, detection_start) - 400:
                         syscalls.index("BVNFEXBackendArmHandoffTrace(",
                                        detection_start)]
    for forbidden in ("->id ==", "pid ==", "0x1400013d0", "entry ==",
                      "== 0x1400"):
        if forbidden in detection:
            raise SystemExit(
                "the handoff detection must not name a pid or an entry point "
                "from a log: " + forbidden)

    # One marker per process, so a process that loops through the exchange
    # cannot grow the log.
    if "bool initProcessDoneReported = false;" not in process_header:
        raise SystemExit(
            "the init_process_done marker must be one-shot per process")
    if "replyProcess->initProcessDoneReported = true;" not in syscalls:
        raise SystemExit(
            "the init_process_done marker must set its one-shot flag")

    # The trace is a bounded phase with a budget, and it stops at the first
    # NT redirect rather than running to the end of the budget.
    require_ordered(
        backend,
        [
            "constexpr unsigned kHandoffTraceBlockBudget = ",
            "BVNFEXBackendArmHandoffTrace(unsigned processId)",
            "FEXCore::Context::BoxedWineArmBlockTrace(processId,",
            "kHandoffTraceBlockBudget",
            "BOXEDWINE_FEX64_HANDOFF_TRACE_ARMED pid=%u budget=%u",
            "BVNFEXBackendDisarmHandoffTrace(unsigned processId)",
            "FEXCore::Context::BoxedWineDisarmBlockTrace(processId)",
            "BOXEDWINE_FEX64_HANDOFF_TRACE_DONE pid=%u reason=nt-redirect",
        ],
        "bounded handoff trace contract",
    )
    budget_line = backend.split(
        "constexpr unsigned kHandoffTraceBlockBudget = ", 1)[1].split(";", 1)[0]
    budget = int(budget_line.strip())
    if not 64 <= budget <= 128:
        raise SystemExit(
            "the handoff trace budget must stay between 64 and 128 blocks, "
            "got " + str(budget))

    adapter = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    require_ordered(
        adapter,
        [
            "reportWineNtSyscallRedirect(",
            "BVNFEXBackendDisarmHandoffTrace(",
        ],
        "handoff trace stop contract",
    )

    # The budget is spent by the translator, so an armed phase cannot outlive
    # it even if nothing disarms it.
    block_patch = read(
        repository
        / "scripts/fex64-patches/fex-boxedwine-block-diagnostics.patch"
    )
    require_ordered(
        block_patch,
        [
            "BoxedWineBlockTraceBudget",
            "BOXEDWINE_FEX64_IRET_PRE rip=",
            "uint64_t BoxedWineTakeGuestIRETNote() {",
            "bool BoxedWineTakeBlockTraceCredit() {",
            "Remaining - 1",
            "BoxedWinePhase = FEXCore::Context::BoxedWineTakeBlockTraceCredit();",
            "BOXEDWINE_FEX64_IRET_POST from=",
        ],
        "translator block-trace budget contract",
    )
    if "BoxedWineNoteGuestIRET(Op->PC)" not in block_patch:
        raise SystemExit(
            "the iretq marker must be taken at the translated instruction's "
            "own guest address")

    # The descriptor table Wine's dispatcher return indexes into. The
    # selectors it loads are indices 6 and 5, so a table shorter than FEX's
    # own is an out-of-bounds read, not a smaller table.
    segment_header = read(repository / "include/guest_segment_table.h")
    if "#define K_GUEST_SEGMENT_TABLE_ENTRIES 32" not in segment_header:
        raise SystemExit(
            "the descriptor table must hold as many entries as FEX's own")
    thread_manager = read(
        fex / "Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.cpp"
    )
    if "sizeof(ThreadStateObject->gdt) == (8 * 32)" not in thread_manager:
        raise SystemExit(
            "FEX's own per-thread descriptor table is no longer 32 entries; "
            "K_GUEST_SEGMENT_TABLE_ENTRIES was matched to it deliberately")
    require_ordered(
        backend,
        [
            "publishGuestDescriptorTable(",
            "SEGMENT_ARRAY_INDEX_GDT",
            "SEGMENT_ARRAY_INDEX_LDT",
            "gdt[K_GUEST_SEGMENT_TABLE_ENTRIES]",
        ],
        "guest descriptor table contract",
    )
    if "gdt_segment gdt[1]" in backend:
        raise SystemExit(
            "a one-entry descriptor table cannot hold the selectors Wine's "
            "dispatcher return loads")

    # The descriptor table is host memory. A translated read of it under the
    # guest alias lands in neither domain, so the long-mode path must not
    # emit that load at all -- and must still emit it for FS and GS, whose
    # bases are real.
    segment_patch = read(
        repository
        / "scripts/fex64-patches/fex-boxedwine-longmode-segment-base.patch"
    )
    require_ordered(
        segment_patch,
        [
            "UpdatePrefixFromSegment",
            "Is64BitMode && CTX->GetGuestLowAliasBase() != 0",
            "SegmentReg != FEXCore::X86Tables::DecodeFlags::FLAG_FS_PREFIX",
            "SegmentReg != FEXCore::X86Tables::DecodeFlags::FLAG_GS_PREFIX",
            "if (HostSideDescriptorTable) {",
            "NewSegment = _Constant(0);",
            "_LoadMemGPR(OpSize::i64Bit, SegmentBase, SegmentOffset",
        ],
        "long-mode segment base contract",
    )

    # Per-block decode mode. Wine's WoW64 layer far-jumps a 64-bit thread into
    # a 32-bit code segment and back, so the width a block is decoded in is a
    # property of the block, not of the context. The translator already derived
    # it that way -- from the L bit of the descriptor CS names -- and only two
    # assertions tied it to the context's creation-time mode.
    mode_patch = read(
        repository
        / "scripts/fex64-patches/fex-boxedwine-per-block-decode-mode.patch"
    )
    require_ordered(
        mode_patch,
        [
            # The gate. Nothing may diverge unless the host armed it.
            "void SetGuestPerBlockDecodeMode(bool Enabled) override {",
            "bool GetGuestPerBlockDecodeMode() const {",
            # The witness the next device run is read for.
            "BOXEDWINE_FEX64_MODE_SWITCH rip={:#x} cs={:#x} mode={}",
            # The block cache is keyed by RIP alone, so a page that changes
            # bitness has to be invalidated before it is compiled again.
            "bool ContextImpl::BoxedWineClaimDecodeModePage(",
            "BOXEDWINE_FEX64_MODE_PAGE_CONFLICT",
            "SyscallHandler->InvalidateGuestCodeRange(Frame->Thread, Page, "
            "FEXCore::Utils::FEX_PAGE_SIZE);",
            # The frontend's assertion becomes conditional, not deleted: a
            # divergence with the gate closed is still a hard error.
            "BlockInfo.Is64BitMode = CSSegment->L == 1;",
            "LOGMAN_THROW_A_FMT(CTX->GetGuestPerBlockDecodeMode(), "
            "\"Expected operating mode to not change at runtime!\");",
            "CTX->BoxedWineNoteDecodeMode(PC, "
            "Thread->CurrentFrame->State.cs_idx, BlockInfo.Is64BitMode);",
            # The same relaxation in the dispatcher, which is handed the
            # decoder's per-block answer already.
            "LOGMAN_THROW_A_FMT(CTX->GetGuestPerBlockDecodeMode(), "
            "\"Expected operating mode to not change at runtime!\");",
            # And the public arming hook the integration calls.
            "virtual void SetGuestPerBlockDecodeMode(bool Enabled)",
        ],
        "per-block decode mode contract",
    )
    # The descriptor table is host memory in a 32-bit block exactly as it is in
    # a 64-bit one, so the substituted base must no longer be conditional on
    # the width -- and FS and GS must still be excluded from it and served by
    # the host trap in both modes.
    for required in (
        "const bool HostSideDescriptorTable = CTX->GetGuestLowAliasBase() "
        "!= 0 &&",
        "if (!Is64BitMode && CTX->GetGuestLowAliasBase() == 0) {",
    ):
        if required not in mode_patch:
            raise SystemExit(
                "the per-block decode mode patch must keep the host-side "
                "descriptor table decision on the address space rather than "
                "the width: " + required)
    require_ordered(
        read(repository / "scripts/build-fex64-fex.sh"),
        [
            "apply_patch fex-boxedwine-low-address-alias.patch",
            "apply_patch fex-boxedwine-longmode-segment-base.patch",
            "apply_patch fex-boxedwine-longmode-segment-selector-write.patch",
            "apply_patch fex-boxedwine-far-transfer-witness.patch",
            "apply_patch fex-boxedwine-per-block-decode-mode.patch",
            "apply_patch fex-boxedwine-host-served-segment-base.patch",
        ],
        "per-block decode mode patch order",
    )

    # FS and GS are the only segments with a base worth having, and the
    # descriptor that names it is host memory. Two opcodes can write them --
    # `mov fs/gs, r/m16` and `pop fs/gs`, since FEX leaves LFS and LGS
    # unimplemented -- so both must reach the host, and the descriptor load
    # must not be emitted for either register while the alias is armed.
    host_served = read(
        repository
        / "scripts/fex64-patches/fex-boxedwine-host-served-segment-base.patch"
    )
    require_ordered(
        host_served,
        [
            # `pop fs` / `pop gs` take the same host trap as the `mov` form.
            "SegmentReg == FEXCore::X86Tables::DecodeFlags::FLAG_FS_PREFIX",
            "CTX->GetGuestLowAliasBase() != 0",
            ".TrapNumber = X86State::X86_TRAPNO_GP,",
            ".si_code = 0x80,",
            # And nothing may reach the untranslatable descriptor load for
            # FS or GS any more.
            "UpdatePrefixFromSegment",
            "return;",
        ],
        "host-served segment base contract",
    )
    if "FEXCore::Core::CPUState, rip" not in host_served:
        raise SystemExit(
            "the popped selector write must publish the faulting instruction's "
            "own address, or the host cannot advance past it")
    adapter = read(repository / "ios/runtime/src/BVNFEXCPU64Adapter.mm")
    for required in ("bytes[i + 1] == 0xa1", "bytes[i + 1] == 0xa9"):
        if required not in adapter:
            raise SystemExit(
                "the host must serve `pop fs` / `pop gs` as well as the `mov` "
                "form of a selector write: " + required)

    # A signal handler runs in the 64-bit code segment. The translator takes a
    # block's decode width from the descriptor CS names, so a handler entered
    # with the 32-bit code selector still loaded is decoded as 32-bit code.
    require_ordered(
        adapter,
        [
            "cpu->seg.cs = frame->State.cs_idx;",
            "cpu->seg.valid = true;",
            "if (cpu->seg.valid) {",
            "frame->State.cs_idx = cpu->seg.cs;",
        ],
        "selector round-trip contract",
    )
    signals = read(repository / "source/kernel/syscall64.cpp")
    require_ordered(
        signals,
        [
            # The frame carries the interrupted pair, not a constant.
            "(U64)cpu->seg.cs | ((U64)cpu->seg.ss << 48)",
            # The witness the next device run is read for.
            "BOXEDWINE_X64_SIGNAL_CS %s pid=%d tid=%d from_cs=0x%x to_cs=0x%x",
            # Delivery switches to the 64-bit pair.
            "static void enterSignalHandlerSegments(CPU64* cpu) {",
            "cpu->seg.cs = K_WINE_X64_CODE_SELECTOR;",
            "cpu->seg.ss = K_WINE_X64_DATA_SELECTOR;",
            # And sigreturn puts back whatever the frame holds.
            "static void restoreSignalFrameSegments(CPU64* cpu, U64 csgsfs) {",
        ],
        "signal code segment contract",
    )
    if ("restoreSignalFrameSegments(\n        cpu, cpu->memory->readq(gregsPtr "
            "+ 8 * X64_GREG_CSGSFS));") not in signals:
        raise SystemExit(
            "rt_sigreturn must resume in the code segment the frame names, or "
            "a WoW64 thread can never get back to 32-bit code")
    # Order matters: the frame has to record the interrupted segments before
    # the thread is switched to the handler's.
    for builder, entry in (
            ("U64 uctxPtr = buildSignalFrame(cpu, frameBase);\n"
             "    enterSignalHandlerSegments(cpu);", "deliverSignalSync"),
            ("U64 uctxPtr = buildSignalFrame(this, frameBase);\n"
             "    enterSignalHandlerSegments(this);", "raiseSyncFault")):
        if builder not in signals:
            raise SystemExit(
                "the signal frame must be built before the code segment is "
                "switched, in " + entry)
    # The host half of the same switch: the one descriptor whose L bit decides
    # a decode, and the arming that lets the translator act on it.
    backend = read(repository / "ios/runtime/src/BVNFEXBackend.mm")
    require_ordered(
        backend,
        [
            "initialiseGuestDescriptorTable(",
            "bool wow64ModeSwitch",
            "table[wow64Code].L = 0;",
            "table[wow64Code].D = 1;",
            "guestWow64ModeSwitchEnabled()",
            "SetGuestPerBlockDecodeMode(perBlockDecodeMode)",
        ],
        "WoW64 32-bit code descriptor contract",
    )
    if "K_WINE_X86_CODE_SELECTOR 0x23" not in read(
            repository / "include/guest_segment_table.h"):
        raise SystemExit(
            "the 32-bit code selector a WoW64 thread enters compatibility "
            "mode with must be named where the table arithmetic lives")

    # The dispatcher-return fixture runs through the real translator, with the
    # alias on as well as off, because the alias is the device's configuration.
    probe = read(repository / "scripts/run-fex64-vixl-probe.sh")
    require_ordered(
        probe,
        [
            "fixture_dispatcher_return=",
            "fex-boxedwine-longmode-segment-base.patch",
            "prepare_fixture \"${fixture_dispatcher_return}\"",
            "run_one x64-dispatcher-return ",
            "run_one x64-dispatcher-return-alias ",
        ],
        "dispatcher-return fixture contract",
    )
    fixture = read(
        repository / "scripts/guest-probes/fex64-dispatcher-return.asm")
    for required in ("WINE_CS 0x33", "WINE_SS 0x2b", "pushfq"):
        if required not in fixture:
            raise SystemExit(
                "the dispatcher-return fixture must reproduce Wine's frame: "
                + required)
    # An `iretq` named in a comment is not an `iretq` executed. Require the
    # instruction itself, on its own line.
    instructions = [line.split(";", 1)[0].strip()
                    for line in fixture.splitlines()]
    if "iretq" not in instructions:
        raise SystemExit(
            "the dispatcher-return fixture must actually execute an iretq")
    if "hlt" not in instructions:
        raise SystemExit("the dispatcher-return fixture must terminate")

    # The descendant snapshot says what it measured. A daemon that reparented
    # itself away is not a direct child, and its absence here is not evidence
    # that it died.
    if "no live DIRECT child of pid=%u" not in syscalls:
        raise SystemExit(
            "the descendant snapshot must not claim more than it measured")

    # No maintained patch may open a NAMED namespace.
    #
    # Core.cpp closes and reopens `namespace FEXCore::Context` around
    # ContextImpl. A patch that writes that opener again lands inside the one
    # already open, nesting FEXCore::Context::FEXCore::Context -- which
    # introduces the name `FEXCore` inside FEXCore::Context, so every later
    # `FEXCore::X86State` in the file resolves against the nearer one and the
    # translation unit stops compiling, hundreds of lines away from the patch.
    #
    # These patches add code to namespaces the pinned source already has. An
    # anonymous namespace is fine: it cannot be reopened by name, so it cannot
    # shadow anything.
    for patch_path in sorted(
            (repository / "scripts/fex64-patches").glob("*.patch")):
        for number, line in enumerate(
                read(patch_path).splitlines(), 1):
            if line.startswith("+namespace ") and line != "+namespace {":
                raise SystemExit(
                    "{}:{}: a maintained patch must not open a named "
                    "namespace -- the pinned source already provides them, "
                    "and reopening one nests it: {}".format(
                        patch_path.name, number, line[1:]))

    print("FEX exit-dispatch and loader-boundary contracts verified")


if __name__ == "__main__":
    main()
