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
    pair_mask_patch = read(
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
            "BVNFEXCPU64AdapterBindFEX(",
            "threadState->fexThread = replacement;",
            "retiredBundle = std::move(processState->fex);",
            "processState->fex = std::move(replacementBundle);",
            "retiredBundle->context->DestroyThread(retired);",
        ],
        "BoxedVN loader execution-epoch replacement",
    )
    require_ordered(
        pair_mask_patch,
        [
            "void LoadStorePair(",
            "Instr |= (Imm & 0b111'1111) << 15;",
        ],
        "FEX ARM64 pair-immediate encoding",
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

    print("FEX exit-dispatch and loader-boundary contracts verified")


if __name__ == "__main__":
    main()
