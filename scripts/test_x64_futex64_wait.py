"""BoxedVN - contract tests for the x86-64 guest's futex table.

A device run launched a 32-bit program through Wine's WoW64 lane. It loaded,
probed the adapter through the Vulkan bridge, created its main window -- and
stopped there. The window was never mapped and nothing was ever presented. The
stalled thread was sitting in the futex syscall:

    BOXEDWINE_FEX64_STALL pid=10 tid=43 state=waiting guest_rip=0x7a4013338b
    BOXEDWINE_FEX64_STALL_GPRS_A live_mask=0x0 syscall_info=0xffff rax=0xca
                                 rcx=0x7a4013338d rdx=0x0 rsi=0x80
    BOXEDWINE_FEX64_STALL_GPRS_B r10=0x7ffffe4fe9f0 r11=0x246
    BOXEDWINE_FEX64_STALL_HOST_IMAGE symbol=__psynch_cvwait

rax 0xca is futex, rsi 0x80 is FUTEX_WAIT | FUTEX_PRIVATE_FLAG, and r10 is a
timespec on the thread's own stack. That log could not say why the wait never
ended, because the only futex diagnostic in the tree -- KThread::logFutexSnapshot
-- was wired solely to the Vulkan first-frame watchdog, which never arms for a
program that wedges before it presents. These tests pin the fixes that are
provable from the source and the witnesses that will answer the rest.

WHICH BRANCH RUNS WHERE. KThread::futex64's wait has two halves. Getting this
backwards wastes a device run, so it is pinned below:

  * CMakeLists.txt puts BOXEDWINE_MAC_JIT in the core's compile definitions,
    and include/boxedwine.h turns BOXEDWINE_MAC_JIT plus TARGET_CPU_ARM64 into
    BOXEDWINE_MULTI_THREADED. iOS is arm64, so the branch this target compiles
    is the #ifdef BOXEDWINE_MULTI_THREADED half: it blocks the host thread
    inside the syscall on the futex's own condition, and it already consumed
    the wake flag, already computed a signed remaining and returned -ETIMEDOUT,
    and already handled pending signals before any of this work.
  * The #else half is the cooperative build (Emscripten and the other
    single-threaded targets): it cannot block, so it parks through the
    scheduler and the syscall is re-executed. That half really was missing all
    three of those things. The corrections to it are real but they are NOT what
    the device is running, and no contract below claims otherwise.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and there is no host compiler here.
"""

from __future__ import annotations

import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KTHREAD = REPO / "source" / "kernel" / "kthread.cpp"
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"
BOXEDWINE_H = REPO / "include" / "boxedwine.h"
CMAKELISTS = REPO / "CMakeLists.txt"
FEX_BACKEND = REPO / "ios" / "runtime" / "src" / "BVNFEXBackend.mm"
KMEMORY64_H = REPO / "include" / "kmemory64.h"
MT_PLATFORM = (REPO / "source" / "emulation" / "cpu" / "normal" /
               "normalPlatformMultiThreaded.cpp")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def futex64_body(source: str) -> str:
    """The text of KThread::futex64, to its closing brace."""
    start = source.index("S64 KThread::futex64(")
    # The first `#endif` inside is the BOXEDWINE_MULTI_THREADED one; the
    # function ends at the brace in column one that follows it.
    return source[start:source.index("\n}\n#endif", start)]


def futex64_wait_branch(source: str) -> str:
    body = futex64_body(source)
    start = body.index("if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET) {")
    end = body.index("if (command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET) {")
    return body[start:end]


def threaded_half(source: str) -> str:
    """The half this target compiles."""
    wait = futex64_wait_branch(source)
    return wait[wait.index("#ifdef BOXEDWINE_MULTI_THREADED"):wait.index("\n#else")]


def cooperative_half(source: str) -> str:
    """The half the single-threaded targets compile."""
    wait = futex64_wait_branch(source)
    return wait[wait.index("\n#else"):]


def wake_helper(source: str) -> str:
    """The body of wakeFutexes64, which sits above KThread::futex64."""
    start = source.index("static U32 wakeFutexes64(")
    return source[start:source.index("\n}", start)]


def resume_prologue(source: str) -> str:
    """The entry code shared by both halves, before the timeout is read."""
    wait = futex64_wait_branch(source)
    return wait[:wait.index("if (!wait && timeoutAddress) {")]


class ThisTargetIsMultiThreaded(unittest.TestCase):
    """Pin the derivation, so nobody re-diagnoses against the wrong branch.

    Grepping the build files for the literal token BOXEDWINE_MULTI_THREADED
    finds nothing, which is how it gets missed: the flag is derived in a header
    from a different flag the build does set.
    """

    def test_the_build_sets_the_flag_that_derives_it(self) -> None:
        self.assertIn("BOXEDWINE_MAC_JIT", read(CMAKELISTS))

    def test_the_header_derives_multi_threaded_on_arm64(self) -> None:
        source = read(BOXEDWINE_H)
        block = source[source.index("#ifdef BOXEDWINE_MAC_JIT"):]
        block = block[:block.index("#if TARGET_OS_IPHONE")]
        self.assertIn("#if TARGET_CPU_ARM64", block)
        self.assertIn("#define BOXEDWINE_MULTI_THREADED", block)

    def test_the_scheduler_that_ships_is_the_multi_threaded_one(self) -> None:
        self.assertIn("#if defined(BOXEDWINE_MULTI_THREADED)", read(MT_PLATFORM))


class TheBranchThisTargetCompiles(unittest.TestCase):
    """The multi-threaded half. It was already correct; keep it that way."""

    def setUp(self) -> None:
        self.half = threaded_half(read(KTHREAD))

    def test_it_consumes_a_claimed_wakeup(self) -> None:
        self.assertIn("if (wait->wake) {", self.half)
        self.assertIn("return 0;", self.half)

    def test_it_can_time_out_with_a_signed_remaining(self) -> None:
        self.assertIn("S32 remaining = wait->expireTimeInMillies -", self.half)
        self.assertIn("if (remaining <= 0) {", self.half)
        self.assertIn("return -K_ETIMEDOUT;", self.half)

    def test_it_delivers_pending_signals(self) -> None:
        self.assertIn("if (this->pendingSignals) {", self.half)
        self.assertIn("return -K_EINTR;", self.half)

    def test_every_exit_releases_the_entry(self) -> None:
        # This is what makes the wake-flag skip in the wake loop harmless here:
        # a freed entry has no thread, so the wake loop skips it on that test
        # instead, and allocFutex re-initialises the flag on reuse.
        self.assertEqual(self.half.count("freeFutex(wait);"),
                         self.half.count("return "),
                         "an exit from the threaded wait does not free its entry")

    def test_a_long_wait_is_named_from_inside_this_half(self) -> None:
        # The witness has to fire on the branch that ships, not only on the
        # cooperative one, or the next device log says nothing new.
        self.assertIn(
            'reportLongFutexWait(wait, KSystem::getMilliesSinceStart(), "park");',
            self.half)


class TheCooperativeHalfOtherTargetsCompile(unittest.TestCase):
    """Not what the device runs. Corrected because it is wrong on its own terms.

    It parks through the scheduler and returns K_FUTEX64_PARKED, and the
    syscall is re-executed when the thread runs again. It had no -ETIMEDOUT, it
    computed its remaining time with an unsigned subtraction, and it never
    looked at the wake flag -- so a timed wait could not expire and a woken
    waiter re-parked into an entry that FUTEX_WAKE would thereafter skip.
    """

    def setUp(self) -> None:
        self.half = cooperative_half(read(KTHREAD))

    def test_an_expired_deadline_reports_etimedout(self) -> None:
        self.assertIn("return -K_ETIMEDOUT;", self.half)

    def test_the_remaining_time_is_signed(self) -> None:
        self.assertIn("const S32 remaining =", self.half)
        self.assertIn("if (remaining <= 0) {", self.half)

    def test_it_still_parks_and_asks_for_re_execution(self) -> None:
        self.assertIn("return K_FUTEX64_PARKED;", self.half)
        source = read(SYSCALL64)
        self.assertIn("if (r == K_FUTEX64_PARKED) {", source)
        self.assertIn("cpu->reExecuteSyscall = true;", source)


class AReExecutedWaitIsTheSameWait(unittest.TestCase):
    """Shared entry code. Only the cooperative half can reach it with an entry
    already present, but the code is compiled into both."""

    def setUp(self) -> None:
        self.prologue = resume_prologue(read(KTHREAD))
        self.wait = futex64_wait_branch(read(KTHREAD))

    def test_a_claimed_wakeup_is_consumed_before_anything_else(self) -> None:
        self.assertIn("if (wait->wake) {", self.prologue)
        self.assertIn("return 0;", self.prologue)

    def test_the_deadline_is_the_one_the_first_call_computed(self) -> None:
        self.assertIn("if (!wait && timeoutAddress) {", self.wait)
        self.assertIn("expires = wait->expireTimeInMillies;", self.prologue)
        self.assertNotIn("wait->expireTimeInMillies = expires;", self.wait,
                         "a resumed wait must not overwrite its own deadline")

    def test_a_resumed_entry_must_belong_to_this_call(self) -> None:
        self.assertIn("wait->expectedValue != value || wait->operation != command",
                      self.prologue)


class APlainWaitIsABitsetWait(unittest.TestCase):
    """Shared code, both lanes, every target.

    The kernel runs FUTEX_WAIT as FUTEX_WAIT_BITSET with FUTEX_BITSET_MATCH_ANY
    and FUTEX_WAKE as FUTEX_WAKE_BITSET with the same mask. Recording a zero
    mask on a plain wait hid it from every bitset wake.
    """

    def setUp(self) -> None:
        self.source = read(KTHREAD)

    def test_the_match_any_mask_is_named(self) -> None:
        self.assertIn("#define FUTEX_BITSET_MATCH_ANY\t0xffffffff", self.source)

    def test_the_64_bit_wait_records_it(self) -> None:
        self.assertRegex(
            futex64_wait_branch(self.source),
            r"wait->mask\s*=\s*\(command == FUTEX_WAIT_BITSET\)\s*\?\s*val3\s*"
            r":\s*FUTEX_BITSET_MATCH_ANY;")

    def test_the_32_bit_wait_records_it(self) -> None:
        self.assertRegex(
            self.source,
            r"f->mask\s*=\s*\(cmd == FUTEX_WAIT_BITSET\)\s*\?\s*val3\s*"
            r":\s*FUTEX_BITSET_MATCH_ANY;")

    def test_a_plain_wake_matches_every_bit(self) -> None:
        self.assertRegex(
            futex64_body(self.source),
            r"command == FUTEX_WAKE_BITSET\s*\n?\s*\?\s*val3\s*:\s*"
            r"FUTEX_BITSET_MATCH_ANY")


class NoOperationAnswersNobodyWasWaiting(unittest.TestCase):
    """Shared code. A silent 0 is indistinguishable from a lost wakeup."""

    def setUp(self) -> None:
        self.body = futex64_body(read(KTHREAD))

    def test_the_requeue_family_is_named_rather_than_numbered(self) -> None:
        self.assertNotIn("if (command == 3 || command == 4 || command == 5) {",
                         self.body)
        self.assertIn("command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE",
                      self.body)
        self.assertIn("command == FUTEX_WAKE_OP", self.body)

    def test_a_requeue_wakes_the_waiters_it_cannot_move(self) -> None:
        # The second word is the syscall's fifth argument and is not carried
        # this far, so the waiters that would be requeued are woken instead --
        # always permitted, since the kernel may wake spuriously and every
        # caller re-tests its predicate after its wait returns.
        self.assertIn("toWake += timeoutAddress;", self.body)
        self.assertIn("wakeFutexes64(this, isPrivate, addressSpace, addr,", self.body)

    def test_cmp_requeue_still_compares(self) -> None:
        self.assertIn(
            "if (command == FUTEX_CMP_REQUEUE && guestMemory->readd(addr) != val3) {",
            self.body)

    def test_an_unserved_operation_is_reported(self) -> None:
        self.assertIn("BOXEDWINE_FUTEX64_UNSERVED", read(KTHREAD))
        self.assertIn("reportUnservedFutex64(this, command, addr, value,",
                      self.body)


class AReleasedEntryOwnsNothing(unittest.TestCase):
    """Shared code. The threaded half never depended on this; the cooperative
    half does, and a stale wake flag on a reusable slot is worth nothing."""

    def setUp(self) -> None:
        source = read(KTHREAD)
        start = source.index("void freeFutex(struct futex* f) {")
        self.body = source[start:source.index("\n}", start)]

    def test_the_wake_flag_is_cleared(self) -> None:
        self.assertIn("f->wake = false;", self.body)

    def test_the_waiting_flag_is_cleared(self) -> None:
        self.assertIn("f->waiting = false;", self.body)


class TheTableIsKeyedTheWayLinuxKeysIt(unittest.TestCase):
    """A private futex is (address space, guest address); a shared one is the
    host pointer, standing in for Linux's inode and offset.

    The 64-bit lane used to key EVERY wait on the host pointer, which made a
    parked waiter depend on that pointer not moving. It can move: getRamPtr
    pins the page against K64Page::decommit, but K64Page::adoptNative and
    K64Page::demoteNativeShared re-point what hostData() returns for an
    already-pinned page, and the mmap paths call them. A waiter that registered
    before such a change and a waker resolving after it compared unequal and
    the wake was dropped in silence. Private waits -- nearly everything glibc
    and Wine do -- no longer resolve a host pointer to be matched at all.
    """

    def setUp(self) -> None:
        self.source = read(KTHREAD)
        start = self.source.index("static bool futexKeyMatches(")
        self.matcher = self.source[start:self.source.index("\n}", start)]

    def test_a_private_wait_is_keyed_on_the_address_space_and_guest_address(self) -> None:
        self.assertIn("if (isPrivate) {", self.matcher)
        self.assertIn("return f->addressSpace == addressSpace &&", self.matcher)
        self.assertIn("f->guestAddress == guestAddress;", self.matcher)

    def test_a_shared_wait_is_keyed_on_the_host_pointer(self) -> None:
        self.assertIn("return f->address == ramAddress;", self.matcher)

    def test_private_and_shared_are_different_futexes(self) -> None:
        # The kernel never matches a FUTEX_WAIT against a FUTEX_WAIT_PRIVATE on
        # the same word, and neither does this.
        self.assertIn("if (f->isPrivate != isPrivate) {", self.matcher)
        self.assertIn("return false;", self.matcher)

    def test_a_wake_from_another_process_cannot_reach_a_private_waiter(self) -> None:
        # Two processes have two KMemory objects, so the address-space half of
        # the key can never compare equal across them.
        self.assertIn("const void* addressSpace = nullptr;", self.source)
        self.assertIn("memory(process->memory),", self.source)

    def test_the_64_bit_lane_derives_private_from_the_flag(self) -> None:
        body = futex64_body(self.source)
        self.assertIn("const bool isPrivate = (op & FUTEX_PRIVATE_FLAG) != 0;",
                      body)
        self.assertIn("const void* const addressSpace = guestMemory;", body)

    def test_the_32_bit_lane_passes_its_own_address_space(self) -> None:
        self.assertIn("f = getFutex(this, isPrivate, memory, addr, ramAddress);",
                      self.source)
        self.assertIn("futexKeyMatches(f, isPrivate, memory, addr,",
                      self.source)

    def test_the_old_process_id_test_is_gone(self) -> None:
        self.assertNotIn(
            "bool processCheck = (!isPrivate || f->thread->process->id ==",
            self.source)

    def test_both_lookups_carry_the_whole_key(self) -> None:
        for signature in (
                "struct futex* getFutex(KThread* thread, bool isPrivate,",
                "struct futex* allocFutex(KThread* thread, U64 address, U64 guestAddress,",
                "static U32 wakeFutexes64(KThread* waker, bool isPrivate,"):
            self.assertIn(signature, self.source)

    def test_a_shared_futex_still_crosses_processes(self) -> None:
        # The shared key is the host pointer, and every process that maps the
        # same file page borrows the same buffer from the shared registry --
        # which is what makes it stand in for Linux's inode and offset.
        memory = read(KMEMORY64_H)
        self.assertIn("owned by g_sharedFileRegistry and aliased by every process",
                      memory)


class TheWitnessesThatWillAnswerTheNextRun(unittest.TestCase):
    """The device log had no futex evidence at all. These add it."""

    def setUp(self) -> None:
        self.source = read(KTHREAD)

    def test_a_64_bit_futex_address_is_not_truncated(self) -> None:
        # Wine's arena starts at 0x7a40000000. Storing the guest address in a
        # U32 made every diagnostic name an address that does not exist.
        self.assertIn("U64 guestAddress = 0;", self.source)
        self.assertNotIn("U32 guestAddress = 0;", self.source)
        self.assertIn(
            "wait = allocFutex(this, ramAddress, addr, isPrivate, addressSpace,",
            self.source)

    def test_the_long_wait_witness_names_the_word(self) -> None:
        self.assertIn("BOXEDWINE_FUTEX64_LONG_WAIT", self.source)
        for field in ("guest=0x%llx", "host=0x%llx", "expected=0x%x",
                      "actual=0x%x", "age=%ums", "resumes=%u",
                      "wake_attempts=%u", "last_wake=%s"):
            self.assertIn(field, self.source, f"the witness omits {field}")

    def test_the_wake_loop_records_why_a_waiter_was_skipped(self) -> None:
        body = wake_helper(self.source)
        self.assertIn("wait->wakeAttempts++;", body)
        for outcome in ('"count-exhausted"', '"already-woken"',
                        '"not-parked-yet"', '"mask-miss"', '"woken"'):
            self.assertIn(outcome, body)

    def test_an_address_split_is_detected_and_named(self) -> None:
        # getRamPtr pins the page, which stops K64Page::decommit from moving
        # the buffer -- but adoptNative and demoteNativeShared re-point the
        # shared-file indirection slot for a page that is already pinned. The
        # keying change makes that harmless for a private wait; the witness
        # stays so we learn whether it happens at all.
        self.assertIn("BOXEDWINE_FUTEX64_ADDRESS_SPLIT", self.source)
        self.assertIn("private=%u outcome=%s", self.source)
        # Two distinct things to report, and they mean opposite things.
        self.assertIn('"repointed"', self.source)   # key matched, pointer moved
        self.assertIn('"unreachable"', self.source)  # key did not match: dropped
        memory = read(KMEMORY64_H)
        self.assertIn("void adoptNative(U8* native)", memory)
        self.assertIn("void demoteNativeShared()", memory)

    def test_every_witness_is_bounded(self) -> None:
        self.assertIn("#define FUTEX64_MAX_DIAGNOSTIC_LINES\t16", self.source)
        self.assertIn("#define FUTEX64_LONG_WAIT_MILLIES\t4000", self.source)
        self.assertEqual(
            self.source.count(">= FUTEX64_MAX_DIAGNOSTIC_LINES"), 3,
            "each of the three witnesses needs its own budget")

    def test_no_diagnostic_dereferences_a_stored_host_pointer(self) -> None:
        # The stored host pointer going stale is the failure these witnesses
        # exist to catch, so it is the one pointer they must not follow: it may
        # no longer be mapped, and a diagnostic must never be able to fault.
        # The word is re-resolved from the GUEST address through the owning
        # process's page table -- which is also how a waker resolves it, so the
        # value reported is the value a wake would compare against.
        self.assertNotIn("reinterpret_cast<const void*>(f->address)",
                         self.source)
        self.assertIn("static bool futexWordValue(struct futex* f, U32* value) {",
                      self.source)
        self.assertIn("memory->isPageMapped(f->guestAddress >> K64_PAGE_SHIFT)",
                      self.source)
        self.assertIn("*value = memory->readd(f->guestAddress);", self.source)
        # A private 32-bit futex stores a GUEST address in `address`, so it has
        # no host word to report at all.
        self.assertIn("bool hostAddress = false;", self.source)
        self.assertIn("!f->hostAddress", self.source)
        self.assertIn("wait->hostAddress = true;",
                      futex64_wait_branch(self.source))

    def test_an_unreadable_word_says_so_rather_than_reporting_zero(self) -> None:
        self.assertIn("actual_read=%u", self.source)
        self.assertEqual(self.source.count("readable ? 1 : 0"), 2,
                         "both witnesses must say whether the word was read")

    def test_the_snapshot_reports_expected_against_actual(self) -> None:
        start = self.source.index("void KThread::logFutexSnapshot() {")
        body = self.source[start:self.source.index("\n}", start)]
        self.assertIn("BOXEDWINE_FUTEX_SNAPSHOT", body)
        self.assertIn("actual=%08X", body)
        self.assertIn("wake_attempts=%u", body)
        self.assertIn("guest=%llX", body)


class TheSnapshotRunsWhenAThreadStalls(unittest.TestCase):
    """It used to run only from the Vulkan first-frame watchdog, which never
    arms for a program that wedges before it presents -- which is why the
    device log carried no futex evidence at all."""

    def setUp(self) -> None:
        self.source = read(FEX_BACKEND)

    def test_the_stall_report_dumps_the_futex_table(self) -> None:
        stall = self.source[self.source.index('reportf("BOXEDWINE_FEX64_STALL pid='):]
        stall = stall[:stall.index("BOXEDWINE_FEX64_STALL_GPRS_A")]
        self.assertIn("KThread::logFutexSnapshot();", stall)

    def test_it_is_bounded(self) -> None:
        self.assertIn("static std::atomic<unsigned> futexSnapshots {0};",
                      self.source)
        self.assertIn("futexSnapshots.fetch_add(1, std::memory_order_relaxed) < 8",
                      self.source)

    def test_it_runs_after_the_sampled_threads_are_resumed(self) -> None:
        # Taking the futex locks while a guest thread is suspended holding one
        # would deadlock the sampler.
        resume = self.source.index("const kern_return_t resumeResult = thread_resume(")
        snapshot = self.source.index("KThread::logFutexSnapshot();")
        self.assertLess(resume, snapshot)


if __name__ == "__main__":
    unittest.main()
