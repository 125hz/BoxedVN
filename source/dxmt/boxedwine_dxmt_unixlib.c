/*
 * 64-bit guest side of BoxedWine's DXMT unix-call bridge.
 *
 * Wine's winemetal PE thunks expect __wine_unix_call_funcs to contain one
 * NTSTATUS(void *) entry for each DXMT operation.  A normal Wine build would
 * put the implementation in an x86-64 ELF unix library.  Under BoxedWine the
 * implementation is native Objective-C/Metal code in the iOS host, so every
 * entry makes one private x86-64 syscall.  The identity-mapped FEX address
 * space makes the args pointer and object handles directly usable by the host
 * table after dispatch.
 *
 * Build this file for the guest x86-64 rootfs with
 * scripts/build-boxedwine-dxmt-unixlib.sh.  It is deliberately kept out of
 * the native BoxedWine core source list.
 */
#include <stdint.h>

#include "boxedwine_x64_hostcall.h"

#if !defined(__x86_64__)
#error "boxedwine_dxmt_unixlib.c must be compiled for an x86-64 guest"
#endif

typedef int32_t boxedwine_ntstatus;
typedef boxedwine_ntstatus (*boxedwine_unix_entry)(void *args);

static inline boxedwine_ntstatus boxedwine_dxmt_call(unsigned int code,
                                                      void *args)
{
    uint64_t syscall_number = BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL;
    uint64_t call_index = code;
    uint64_t call_args = (uint64_t)(uintptr_t)args;

    /* Keep this independent of the guest libc.  FEX executes the x86-64
     * syscall instruction and BoxedWine consumes the reserved number. */
    __asm__ volatile("syscall"
                     : "+a"(syscall_number)
                     : "D"(call_index), "S"(call_args)
                     : "rcx", "r11", "memory");
    return (boxedwine_ntstatus)(int64_t)syscall_number;
}

#define BOXEDWINE_DXMT_CALLS(X) \
    X(0)   X(1)   X(2)   X(3)   X(4)   X(5)   X(6)   X(7)   \
    X(8)   X(9)   X(10)  X(11)  X(12)  X(13)  X(14)  X(15)  \
    X(16)  X(17)  X(18)  X(19)  X(20)  X(21)  X(22)  X(23)  \
    X(24)  X(25)  X(26)  X(27)  X(28)  X(29)  X(30)  X(31)  \
    X(32)  X(33)  X(34)  X(35)  X(36)  X(37)  X(38)  X(39)  \
    X(40)  X(41)  X(42)  X(43)  X(44)  X(45)  X(46)  X(47)  \
    X(48)  X(49)  X(50)  X(51)  X(52)  X(53)  X(54)  X(55)  \
    X(56)  X(57)  X(58)  X(59)  X(60)  X(61)  X(62)  X(63)  \
    X(64)  X(65)  X(66)  X(67)  X(68)  X(69)  X(70)  X(71)  \
    X(72)  X(73)  X(74)  X(75)  X(76)  X(77)  X(78)  X(79)  \
    X(80)  X(81)  X(82)  X(83)  X(84)  X(85)  X(86)  X(87)  \
    X(88)  X(89)  X(90)  X(91)  X(92)  X(93)  X(94)  X(95)  \
    X(96)  X(97)  X(98)  X(99)  X(100) X(101) X(102) X(103) \
    X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
    X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
    X(120) X(121) X(122) X(123) X(124) X(125) X(126)

#define BOXEDWINE_DXMT_DECLARE(index) \
    static boxedwine_ntstatus boxedwine_dxmt_unix_call_##index(void *args) \
    { return boxedwine_dxmt_call(index, args); }
BOXEDWINE_DXMT_CALLS(BOXEDWINE_DXMT_DECLARE)
#undef BOXEDWINE_DXMT_DECLARE

#define BOXEDWINE_DXMT_ENTRY(index) boxedwine_dxmt_unix_call_##index,

/* The symbol name is the one Wine's builtin unixlib loader requests. */
__attribute__((visibility("default"), used))
const boxedwine_unix_entry __wine_unix_call_funcs[
    BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL_COUNT] = {
    BOXEDWINE_DXMT_CALLS(BOXEDWINE_DXMT_ENTRY)
};

#undef BOXEDWINE_DXMT_ENTRY
#undef BOXEDWINE_DXMT_CALLS
