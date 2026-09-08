#include "boxedwine_x64_hostcall.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    for (uint64_t ordinal = 0; ordinal < 4096; ++ordinal) {
        const bool expected = ordinal == 36 || ordinal == 37 || ordinal == 38;
        assert(bool(boxedwineDxmtRecordsCommands(ordinal)) == expected);
        assert(bool(boxedwineDxmtRecordsCommands(ordinal | 0x80000000ull)) == expected);
        assert(!boxedwineDxmtRecordsCommands(ordinal | 0x100000000ull));
    }
    assert(!boxedwineDxmtRecordsCommands(~0ull));
    puts("PASS Metal recording allowlist: native, WoW64 and malformed ordinals");
}
