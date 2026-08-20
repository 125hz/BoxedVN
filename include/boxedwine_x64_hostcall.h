/*
 * BoxedWine x86-64 guest private host-call ABI.
 *
 * This header is intentionally C-compatible and has no BoxedWine types.  It
 * is included by both the 64-bit guest unixlib shim and the native x64 syscall
 * dispatcher.  The number is above the Linux x86-64 syscall table and is
 * reserved for calls that are implemented by the BoxedWine host process.
 */
#ifndef BOXEDWINE_X64_HOSTCALL_H
#define BOXEDWINE_X64_HOSTCALL_H

/* __wine_unix_call() bridge for the native Metal unix-call table. */
#define BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL 0x7fff0001ULL

/* Number of entries in the pinned DXMT winemetal unix-call ABI. */
#define BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL_COUNT 127U

/* NTSTATUS returned when the native table has a NULL entry. */
#define BOXEDWINE_X64_HOSTCALL_STATUS_NOT_IMPLEMENTED 0xc0000002U

#endif /* BOXEDWINE_X64_HOSTCALL_H */
