/*
 * BoxedWine - which side-by-side assembly a guest process actually activated.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A device log named `microsoft.windows.common-controls.dll` at a crash and
 * carried thirty-eight probe lines for that name, all of them misses. What it
 * did not carry was an answer: whether the program got version 6 of the common
 * controls, version 5, or nothing at all. That had to be reconstructed from the
 * shape of the misses, and the reconstruction was only possible because Wine's
 * lookup order is known.
 *
 * The order is worth writing down, because it is what this witness reads.
 * ntdll's `lookup_assembly` (dlls/ntdll/actctx.c) tries exactly two places:
 *
 *   1. `lookup_winsxs` -- <windows dir>\winsxs\manifests, searched with a
 *      wildcard on <arch>_<name>_<key>_<major>.<minor>.*_<lang>_deadbeef;
 *   2. only if that returns STATUS_NO_SUCH_FILE, the private-assembly probes:
 *      <dir>\<name>.dll, <dir>\<name>.manifest, <dir>\<name>\<name>.dll and
 *      <dir>\<name>\<name>.manifest, first for the application directory and
 *      then for the directory of the manifest the dependency came from.
 *
 * So a `<dir>\<name>.manifest` probe is proof that step 1 found nothing, and a
 * successful open under winsxs\manifests is proof that it did -- and the file
 * name says which version, because Wine encodes the resolved version in it.
 * Those two facts are the whole witness. Both are visible from the syscall
 * side, which matters here: device sessions have carried no Wine debug channel
 * output at all, so `FIXME: Could not find dependent assembly` is not a line
 * this log can rely on.
 *
 * Bounded like the module-search trace beside it: one line per assembly per
 * process, a fixed number of assemblies remembered, and no line at all for a
 * process that never asks about one. It is not budgeted against the module
 * trace, because the answer must survive a spent budget -- that is exactly the
 * point at which a log stops saying anything.
 */

#ifndef __GUEST_SXS_ACTIVATION_H__
#define __GUEST_SXS_ACTIVATION_H__

// The directory `lookup_winsxs` searches, as it appears inside a guest path,
// and the trailer `append_manifest_filename` gives every manifest Wine writes.
// Matched as path text rather than reconstructed from the prefix: the same
// process reaches this path through dosdevices/c: and through drive_c, and the
// witness has to recognise both.
#define K_SXS_MANIFEST_DIR_MARKER "winsxs/manifests/"
#define K_SXS_MANIFEST_SUFFIX ".manifest"

// An assembly name is long: "microsoft.windows.common-controls" is 33
// characters and the versioned CRT names are longer. A name that does not fit
// is reported truncated rather than dropped -- the point of the line is to say
// which assembly, and a truncated name still says that.
#define K_SXS_ASSEMBLY_NAME_MAX 96
#define K_SXS_ASSEMBLY_VERSION_MAX 32
#define K_SXS_ASSEMBLY_ARCH_MAX 16

// How many distinct assemblies one process reports on. A program's manifest
// names one or two; Wine's own helpers name one. Eight is room for a program
// that asks for the CRT, GDI+ and the common controls together and still
// bounds the whole thing to eight lines per process.
#define K_SXS_ASSEMBLY_SLOTS 8

#if defined(__cplusplus)
#include <atomic>
#include <cstddef>

namespace boxedvn {

// What one line says. Fixed buffers so a report can be taken under the lock
// and printed outside it.
struct SxsActivationReport {
    char assembly[K_SXS_ASSEMBLY_NAME_MAX] = {0};
    char version[K_SXS_ASSEMBLY_VERSION_MAX] = {0};
    char arch[K_SXS_ASSEMBLY_ARCH_MAX] = {0};
    // "winsxs" when the prefix's assembly store answered, "none" when it did
    // not and the loader fell through to the private-assembly probes.
    const char* source = "none";
    // "activated" or "not-found".
    const char* status = "not-found";
};

inline char sxsLower(char value) {
    return (value >= 'A' && value <= 'Z') ? (char)(value - 'A' + 'a') : value;
}

inline bool sxsIsSeparator(char value) {
    return value == '/' || value == '\\';
}

inline std::size_t sxsLength(const char* text) {
    std::size_t length = 0;
    while (text != nullptr && text[length] != 0) {
        ++length;
    }
    return length;
}

// Case-insensitive, separator-insensitive substring test. `marker` is spelled
// with '/' and is matched against either separator, because a guest path
// reaches here in Unix form while the same lookup is spelled with backslashes
// on the Windows side.
inline bool sxsPathContains(const char* path, const char* marker) {
    if (path == nullptr || marker == nullptr) {
        return false;
    }
    for (std::size_t at = 0; path[at] != 0; ++at) {
        std::size_t i = 0;
        for (;; ++i) {
            if (marker[i] == 0) {
                return true;
            }
            const char left = path[at + i];
            if (left == 0) {
                break;
            }
            const char right = marker[i];
            if (right == '/') {
                if (!sxsIsSeparator(left)) {
                    break;
                }
            } else if (sxsLower(left) != sxsLower(right)) {
                break;
            }
        }
    }
    return false;
}

inline bool sxsPathEndsWith(const char* path, const char* suffix) {
    const std::size_t pathLength = sxsLength(path);
    const std::size_t suffixLength = sxsLength(suffix);
    if (suffixLength == 0 || pathLength < suffixLength) {
        return false;
    }
    for (std::size_t i = 0; i < suffixLength; ++i) {
        if (sxsLower(path[pathLength - suffixLength + i]) !=
            sxsLower(suffix[i])) {
            return false;
        }
    }
    return true;
}

// The last path component, without a trailing ".manifest".
inline bool sxsManifestStem(const char* path, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return false;
    }
    out[0] = 0;
    if (!sxsPathEndsWith(path, K_SXS_MANIFEST_SUFFIX)) {
        return false;
    }
    const std::size_t length =
        sxsLength(path) - sxsLength(K_SXS_MANIFEST_SUFFIX);
    std::size_t start = length;
    while (start > 0 && !sxsIsSeparator(path[start - 1])) {
        --start;
    }
    const std::size_t stemLength = length - start;
    if (stemLength == 0) {
        return false;
    }
    std::size_t copied = 0;
    for (; copied < stemLength && copied + 1 < capacity; ++copied) {
        out[copied] = path[start + copied];
    }
    out[copied] = 0;
    return true;
}

inline void sxsCopy(char* out, std::size_t capacity, const char* value,
                    std::size_t length) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    std::size_t copied = 0;
    for (; copied < length && copied + 1 < capacity; ++copied) {
        out[copied] = value[copied];
    }
    out[copied] = 0;
}

// Split a Wine manifest stem into its parts.
//
// The stem is <arch>_<name>_<key>_<version>_<lang>_deadbeef, and it is parsed
// from the RIGHT because only the tail has a fixed field count: the assembly
// name is the one field that may itself contain an underscore, so anything
// between the first field and the last four belongs to it.
inline bool sxsParseManifestStem(const char* stem, SxsActivationReport& out) {
    const std::size_t length = sxsLength(stem);
    if (length == 0) {
        return false;
    }
    // Offsets of the underscores, from the right.
    std::size_t marks[5];
    unsigned found = 0;
    std::size_t at = length;
    while (at > 0 && found < 5) {
        --at;
        if (stem[at] == '_') {
            marks[found++] = at;
        }
    }
    if (found < 5) {
        return false;
    }
    // marks[0] precedes the trailer, marks[1] the language, marks[2] the
    // version, marks[3] the public key; marks[4] is the last underscore that
    // could still belong to the name, so the architecture ends at the FIRST
    // underscore in the stem instead.
    std::size_t archEnd = 0;
    while (archEnd < length && stem[archEnd] != '_') {
        ++archEnd;
    }
    if (archEnd == 0 || archEnd >= marks[3]) {
        return false;
    }
    sxsCopy(out.arch, sizeof(out.arch), stem, archEnd);
    sxsCopy(out.assembly, sizeof(out.assembly), stem + archEnd + 1,
            marks[3] - archEnd - 1);
    sxsCopy(out.version, sizeof(out.version), stem + marks[2] + 1,
            marks[1] - marks[2] - 1);
    return out.assembly[0] != 0 && out.version[0] != 0;
}

// One process's side-by-side outcomes.
//
// Reports are queued rather than printed here: this is a header shared with
// host tests, and the emulator's logging belongs to the caller. The caller
// drains the queue after every path operation it already reports, so a line
// appears at the moment the outcome is known rather than at an exit the
// process may not reach.
class SxsActivationTrace final {
public:
    // Observe one path operation. `result` is the syscall's return: negative
    // means the path was not there.
    void noteResult(const char* path, long long result) {
        if (path == nullptr) {
            return;
        }
        const bool underWinsxs = sxsPathContains(path, K_SXS_MANIFEST_DIR_MARKER);
        char stem[K_SXS_ASSEMBLY_NAME_MAX + K_SXS_ASSEMBLY_VERSION_MAX +
                  K_SXS_ASSEMBLY_ARCH_MAX + 64];
        if (!sxsManifestStem(path, stem, sizeof(stem))) {
            return;
        }
        // `<module file name>.manifest` beside a module is not an assembly
        // lookup at all: it is get_manifest_in_associated_manifest, which the
        // loader tries for any image with no embedded manifest, and a device
        // log carries several of them (services.exe.manifest,
        // winedevice.exe.manifest). An assembly name never ends in .exe or
        // .dll, so the suffix is what separates the two.
        if (sxsPathEndsWith(stem, ".exe") || sxsPathEndsWith(stem, ".dll")) {
            return;
        }
        SxsActivationReport report;
        if (underWinsxs) {
            // A miss here says nothing on its own: Wine enumerates the
            // directory with a wildcard and opens only what it matched, so a
            // failed open is a manifest that went away between the two.
            if (result < 0) {
                return;
            }
            if (!sxsParseManifestStem(stem, report)) {
                return;
            }
            report.source = "winsxs";
            report.status = "activated";
        } else {
            // A private-assembly probe. Reaching one is proof that
            // lookup_winsxs already returned STATUS_NO_SUCH_FILE for this
            // assembly, whatever this probe goes on to do -- and a probe that
            // succeeds is a private assembly shipped beside the program, which
            // is a different answer and worth its own word.
            sxsCopy(report.assembly, sizeof(report.assembly), stem,
                    sxsLength(stem));
            if (report.assembly[0] == 0) {
                return;
            }
            sxsCopy(report.version, sizeof(report.version), "unknown", 7);
            sxsCopy(report.arch, sizeof(report.arch), "unknown", 7);
            report.source = result >= 0 ? "private-assembly" : "none";
            report.status = result >= 0 ? "activated" : "not-found";
        }
        queue(report);
    }

    // Takes one pending line, or returns false. Drains in the order the
    // outcomes were decided.
    bool takeReport(SxsActivationReport& out) {
        Guard guard(lock);
        if (pending == 0) {
            return false;
        }
        out = reports[0];
        for (unsigned i = 1; i < pending; ++i) {
            reports[i - 1] = reports[i];
        }
        --pending;
        return true;
    }

private:
    class Guard {
    public:
        explicit Guard(std::atomic<bool>& held) : held(held) {
            bool expected = false;
            while (!held.compare_exchange_weak(expected, true,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
                expected = false;
            }
        }
        ~Guard() { held.store(false, std::memory_order_release); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        std::atomic<bool>& held;
    };

    static bool sameAssembly(const char* left, const char* right) {
        for (std::size_t i = 0;; ++i) {
            if (sxsLower(left[i]) != sxsLower(right[i])) {
                return false;
            }
            if (left[i] == 0) {
                return true;
            }
        }
    }

    // At most one line per assembly per process. A "not-found" recorded first
    // is never replaced by a later answer for the same assembly, because
    // lookup_assembly cannot produce one: a winsxs hit short-circuits the
    // private probes, so the two outcomes are mutually exclusive.
    void queue(const SxsActivationReport& report) {
        Guard guard(lock);
        for (unsigned i = 0; i < reportedCount; ++i) {
            if (sameAssembly(reported[i], report.assembly)) {
                return;
            }
        }
        if (reportedCount >= K_SXS_ASSEMBLY_SLOTS) {
            return;
        }
        sxsCopy(reported[reportedCount], K_SXS_ASSEMBLY_NAME_MAX,
                report.assembly, sxsLength(report.assembly));
        ++reportedCount;
        if (pending >= K_SXS_ASSEMBLY_SLOTS) {
            return;
        }
        reports[pending] = report;
        ++pending;
    }

    mutable std::atomic<bool> lock {false};
    SxsActivationReport reports[K_SXS_ASSEMBLY_SLOTS];
    char reported[K_SXS_ASSEMBLY_SLOTS][K_SXS_ASSEMBLY_NAME_MAX] = {};
    unsigned reportedCount = 0;
    unsigned pending = 0;
};

} // namespace boxedvn

#endif // __cplusplus

#endif
