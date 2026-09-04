/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "startupArgs.h"

#include "guest_wine_prefix.h"
#include "guest_wine64_layout.h"

#include "devtty.h"
#include "devurandom.h"
#include "devnull.h"
#include "devzero.h"
#include "devinput.h"
#include "devdsp.h"
#include "procselfexe.h"
#include "cpuinfo.h"
#include "bufferaccess.h"
#include "meminfo.h"
#include "procstat.h"

#include "uptime.h"
#include "devmixer.h"
#include "devsequencer.h"
#include "devfb.h"
#include "mainloop.h"
#include "../io/fsfilenode.h"
#include "../io/fszip.h"
#include "loader.h"
#include "kstat.h"
#include "knativesystem.h"
#include "knativeinput.h"
#include "knativeaudio.h"
#include "knativesocket.h"

#ifdef __TEST
#include "../test/cpu/testCPU.h"
#endif

#ifndef BOXEDWINE_DISABLE_UI
#include "../ui/data/globalSettings.h"
#endif

#include MKDIR_INCLUDE
#include CURDIR_INCLUDE

void gl_init(BString allowExtensions);
void vulkan_init();
void x11_init();
void createSysfs(const std::shared_ptr<FsNode> rootNode);

U32 StartUpArgs::uiType;

static bool hasEnvValue(const std::vector<BString>& envValues, const char* name) {
    BString prefix = BString::copy(name);
    prefix += "=";
    for (auto& value : envValues) {
        if (value.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

static void addDefaultEnvValue(std::vector<BString>& envValues, const char* value) {
    const char* equals = strchr(value, '=');
    if (!equals) {
        return;
    }
    BString name = BString::copy(value, (int)(equals - value));
    if (!hasEnvValue(envValues, name.c_str())) {
        envValues.push_back(BString::copy(value));
    }
}

// The guest Wine prefix this launch actually uses.
//
// WINEPREFIX in the guest environment is authoritative, because it is what
// Wine itself reads. An x86-64 launch sets it to .wine64: the bundled
// /home/username/.wine is a 32-bit installation and Wine64 refuses to run in
// it. The last assignment wins, which is what the guest would see. With no
// WINEPREFIX, or one that is not a usable absolute guest path, this is the
// unchanged 32-bit default, so IA-32 launches behave exactly as before.
static BString guestWinePrefixFromEnv(const std::vector<BString>& envValues) {
    const char* selected = nullptr;
    for (const BString& entry : envValues) {
        const char* value = boxedvn::guestWinePrefixAssignment(entry.c_str());
        if (value) {
            selected = value;
        }
    }
    return BString::copy(boxedvn::resolveGuestWinePrefix(selected).c_str());
}

static bool guestUsesFex64(const std::vector<BString>& envValues) {
    for (const BString& entry : envValues) {
        if (entry == "BOXEDWINE_CPU64=fex") {
            return true;
        }
    }
    return false;
}

// Whether this launch asked for DXVK's 32-bit d3d9 over Wine's own.
//
// Wine's d3d9 is wined3d, and wined3d needs OpenGL or Vulkan: there is no
// OpenGL on iOS, and the Vulkan route only exists once the 64-bit guest ICD
// is staged and the bridge answers. DXVK's d3d9 needs Vulkan only, and the
// binary is the one the app already ships and the IA-32 lane already uses.
//
// Opt-in, and only on the exact value: an unset variable, an empty one, or
// any other spelling keeps Wine's own d3d9, so a Vulkan path that does not
// work cannot regress a lane that currently reaches its own message box.
static bool guestWantsWow64Dxvk(const std::vector<BString>& envValues) {
    const BString prefix = B(K_X64_WOW64_D3D9_ENV "=");
    bool wanted = false;
    for (const BString& entry : envValues) {
        if (entry.startsWith(prefix)) {
            wanted = (entry == prefix + B(K_X64_WOW64_D3D9_DXVK));
        }
    }
    return wanted;
}

// Whether this launch asked for Wine's relay trace of the guest's Windows API
// calls, restricted to the modules K_X64_WINE_RELAY_INCLUDE names.
//
// Opt-in and only on the exact value, like the DXVK d3d9 projection above: an
// unset variable, an empty one or any other spelling leaves the prefix's Debug
// key alone, so a session that did not ask for the trace cannot acquire one
// from a session that did.
static bool guestWantsWineRelayTrace(const std::vector<BString>& envValues) {
    const BString prefix = B(K_X64_WINE_TRACE_ENV "=");
    bool wanted = false;
    for (const BString& entry : envValues) {
        if (entry.startsWith(prefix)) {
            wanted = (entry == prefix + B(K_X64_WINE_TRACE_RELAY));
        }
    }
    return wanted;
}

// Make every process of an x86-64 session resolve glibc's IFUNC string and
// memory routines to non-VEX implementations.
//
// See K_X64_GUEST_GLIBC_HWCAPS in guest_wine64_layout.h for why: only the
// launched process runs on FEX, every forked child runs on the SSE2-only
// interpreter, and glibc chose its routines once, from the FEX CPUID, before
// the first fork. GLIBC_TUNABLES is read by ld.so in each process, so the
// choice is remade -- the same way -- in the children as well.
//
// A caller-supplied GLIBC_TUNABLES is kept and appended to; a caller that
// already named glibc.cpu.hwcaps keeps its own list untouched. Only the last
// assignment is what the guest would see, so that is the one this reads and
// the one it replaces.
static void addGuestGlibcTunables(std::vector<BString>& envValues) {
    const BString prefix = B(K_X64_GUEST_GLIBC_TUNABLES_NAME "=");
    BString existing;
    int replaceAt = -1;
    for (int i = 0; i < (int)envValues.size(); i++) {
        if (envValues[i].startsWith(prefix)) {
            existing = envValues[i].substr(prefix.length());
            replaceAt = i;
        }
    }
    const std::string composed =
        boxedvn::guestGlibcTunablesValue(std::string(existing.c_str()));
    const bool callerSetHwcaps =
        replaceAt >= 0 &&
        boxedvn::glibcTunablesSetHwcaps(std::string(existing.c_str()));
    const BString assignment = prefix + BString::copy(composed.c_str());
    if (replaceAt >= 0) {
        envValues[replaceAt] = assignment;
    } else {
        envValues.push_back(assignment);
    }
    // One line naming the value the guest will actually carry, so a log shows
    // whether the children were launched with the tunables at all rather than
    // leaving it to be inferred from which routines they died in.
    klog_fmt("BOXEDWINE_X64_GLIBC_TUNABLES value=%s status=%s",
             composed.c_str(),
             callerSetHwcaps ? "caller-set-hwcaps"
                             : (replaceAt >= 0 ? "appended" : "applied"));
}

// Give the projected DXVK d3d9 the WINEDLLOVERRIDES entry it needs, merged
// into whatever the launch already set.
//
// Declining when the variable was already present is what used to happen, and
// it was the branch that ran in practice: the app always sets
// WINEDLLOVERRIDES for the DXMT modules, so the projected DXVK image sat in
// syswow64 while the loader bound Wine's own i386-windows/d3d9.dll and the
// device log said only "override-kept". Wine's separator between entries is
// ';', so the caller's entry keeps everything it named and gains one more.
//
// Only the last assignment is what the guest would see, so that is the one
// this reads and the one it rewrites.
static void mergeWow64DxvkD3d9Override(std::vector<BString>& envValues) {
    const BString prefix = B(K_WINE_DLL_OVERRIDES_NAME "=");
    BString existing;
    int replaceAt = -1;
    for (int i = 0; i < (int)envValues.size(); i++) {
        if (envValues[i].startsWith(prefix)) {
            existing = envValues[i].substr(prefix.length());
            replaceAt = i;
        }
    }
    const std::string previous(existing.c_str());
    const std::string merged = boxedvn::wineDllOverridesWithDxvkD3d9(previous);
    const bool alreadyNamed =
        replaceAt >= 0 && boxedvn::wineDllOverridesNameModule(previous, "d3d9");
    const BString assignment = prefix + BString::copy(merged.c_str());
    if (replaceAt >= 0) {
        envValues[replaceAt] = assignment;
    } else {
        envValues.push_back(assignment);
    }
    klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=dxvk-i386 status=%s "
             "WINEDLLOVERRIDES=%s",
             alreadyNamed ? "override-kept"
                          : (replaceAt >= 0 ? "override-merged"
                                            : "override-applied"),
             merged.c_str());
}

// Wine normally installs small builtin placeholders into a new prefix's
// system32 directory. On the BoxedWine x64 path wineboot completes with status
// 0 but leaves that directory empty, and the FEX-translated parent then cannot
// perform Wine's late Unix-side builtin fallback. Project the mounted PE tree
// into system32 as guest links before any process starts. This is an in-memory
// union: a real prefix file is never replaced, and the packaged module remains
// the single backing copy.
// Project DXMT's Wine-builtin modules over the packaged Wine module root.
//
// The DXMT DLLs carry Wine's builtin marker so winemetal.dll can reach its
// winemetal.so through __wine_unix_call. Wine treats a builtin-marked PE found
// anywhere but its module tree as a stale installed copy and loads its own
// builtin of that name instead: a device run found d3d11.dll and dxgi.dll
// beside the executable, resolved wined3d and opengl32, and never looked for
// winemetal.dll, so D3D11CreateDevice failed with DXGI_ERROR_UNSUPPORTED.
//
// Replacing the module-root entries makes DXMT the builtin. The packaged
// archive is untouched: this is the same in-memory union the system32
// projection uses, applied after it so the system32 links, which name the
// module-root guest paths, resolve to the DXMT copies as well.
//
// That last sentence only holds for a name the packaged tree already carried.
// The system32 projection runs first and links what it finds; a module DXMT
// adds afterwards -- winemetal.dll, which Wine does not ship -- is in the
// module root and nowhere in the prefix. Wine's loader never looks in the
// module root by itself: it searches the DOS directories, finds the prefix's
// entry for a builtin name, and only then follows it to the module tree. With
// no prefix entry the search ends at the four DOS directories, and a device
// run showed exactly that -- 'system32/winemetal.dll' ENOENT after a scan of
// all 962 system32 entries, no probe of the module root at any point, and
// dxgi.dll (which imports it) failing with STATUS_DLL_NOT_FOUND, which
// LdrInitializeThunk then returned for the whole process. So each projected
// module is linked into the prefix as well, under the same non-destructive
// rule the system32 projection uses: a real prefix file always wins.
static void overlayX64WineModules(const BString& overlayDir,
                                  const BString& winePrefix) {
    std::shared_ptr<FsNode> peDir =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_WINE_PE_DIR), true);
    if (!peDir || !peDir->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY dir=%s status=no-module-root",
                 overlayDir.c_str());
        return;
    }
    const BString system32 = winePrefix + "/" K_GUEST_WINE_DRIVE_C "/" +
                             K_GUEST_WINE_WINDOWS "/" K_GUEST_WINE_SYSTEM32;
    std::shared_ptr<FsNode> system32Directory =
        Fs::getNodeFromLocalPath(B(""), system32, true);
    const bool system32Ready =
        system32Directory != nullptr && system32Directory->isDirectory();
    for (const std::string& name : boxedvn::x64DxmtModuleNames()) {
        const BString sourcePath = overlayDir + "/" + name.c_str();
        std::shared_ptr<FsNode> source =
            Fs::getNodeFromLocalPath(B(""), sourcePath, true);
        if (!boxedvn::shouldOverlayX64WineModule(
                source != nullptr, source && source->isDirectory())) {
            klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY name=%s source=%s status=missing",
                     name.c_str(), sourcePath.c_str());
            continue;
        }
        const BString destination = B(K_X64_WINE_PE_DIR) + "/" + name.c_str();
        Fs::addFileNode(destination, B(""), source->nativePath, false, peDir);
        // The prefix half. "present" is the ordinary case for a name the
        // packaged tree already had -- the system32 projection linked it, and
        // that link names the module-root path this loop just rewrote, so it
        // already resolves to the DXMT copy. "linked" is the name that had no
        // prefix entry at all and is the one this exists for.
        const char* system32Status = "unavailable";
        if (system32Ready) {
            const BString prefixPath = system32 + "/" + name.c_str();
            const bool prefixExists =
                Fs::getNodeFromLocalPath(B(""), prefixPath, false) != nullptr;
            if (boxedvn::shouldProjectGuestWineSystemModule(true, false,
                                                            prefixExists)) {
                Fs::addFileNode(prefixPath, destination, B(""), false,
                                system32Directory);
                system32Status = "linked";
            } else {
                system32Status = "present";
            }
        }
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY name=%s source=%s destination=%s "
                 "status=projected system32=%s",
                 name.c_str(), sourcePath.c_str(), destination.c_str(),
                 system32Status);
    }
}

// The 32-bit half of the same projection. New WoW64 keeps the i386 PE builtins
// in a third tree under the packaged module root, and Wine looks for them in
// the prefix's syswow64 rather than system32; a prefix whose syswow64 is empty
// gives a 32-bit image nothing to bind to, which Wine reports only as a
// failure to start it.
//
// Silent when the tree is not mounted: a 64-bit-only runtime layer is still a
// working 64-bit lane, so the absence is logged and the launch continues.
static void projectX64WinePe32Modules(const BString& winePrefix) {
    const BString syswow64 = winePrefix + "/" K_GUEST_WINE_DRIVE_C "/" +
                             K_GUEST_WINE_WINDOWS "/syswow64";

    std::shared_ptr<FsNode> pe32Directory =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_WINE_PE32_DIR), true);
    if (!pe32Directory || !pe32Directory->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=i386-windows status=missing source=%s",
                 K_X64_WINE_PE32_DIR);
        return;
    }
    Fs::makeLocalDirs(syswow64);
    std::shared_ptr<FsNode> pe32Destination =
        Fs::getNodeFromLocalPath(B(""), syswow64, true);
    if (!pe32Destination || !pe32Destination->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=i386-windows status=missing destination=%s",
                 syswow64.c_str());
        return;
    }

    std::vector<std::shared_ptr<FsNode> > pe32Modules;
    pe32Directory->getAllChildren(pe32Modules);
    U32 pe32Projected = 0;
    pe32Destination->reserveChildren(pe32Modules.size());
    for (const std::shared_ptr<FsNode>& source : pe32Modules) {
        if (!source || source->name.isEmpty()) {
            continue;
        }
        const BString destination = syswow64 + "/" + source->name;
        const bool destinationExists =
            Fs::getNodeFromLocalPath(B(""), destination, false) != nullptr;
        if (!boxedvn::shouldProjectGuestWineSystemModule(
                source != nullptr, source && source->isDirectory(),
                destinationExists)) {
            continue;
        }
        Fs::addFileNode(destination, source->path, B(""), false,
                        pe32Destination);
        ++pe32Projected;
    }
    klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=i386-windows status=projected destination=%s projected=%u",
             syswow64.c_str(), pe32Projected);

    // What the projection produced, checked against the import chain every
    // 32-bit Windows program walks before its own entry point runs. A device
    // run projected 784 modules and still had no zlib1.dll, which 32-bit
    // wined3d imports: the loader searched syswow64, system, windows and the
    // program's own directory, found nothing, and ended the process with
    // STATUS_DLL_NOT_FOUND. Nothing in that sequence names the missing module
    // -- the search trace is hundreds of stat lines and the program simply
    // never appears -- so name it here, once, before any process starts.
    //
    // The list is the budget: at most one line per required module and one
    // summary, whatever the state of the tree.
    const std::vector<std::string> lanePe32Modules =
        boxedvn::x64Wow64LanePe32ModuleNames();
    U32 pe32Missing = 0;
    for (const std::string& name : lanePe32Modules) {
        const BString probe = syswow64 + "/" + name.c_str();
        if (Fs::getNodeFromLocalPath(B(""), probe, false) != nullptr) {
            continue;
        }
        ++pe32Missing;
        klog_fmt("BOXEDWINE_X64_PE32_GAP name=%s destination=%s status=missing",
                 name.c_str(), probe.c_str());
    }
    klog_fmt("BOXEDWINE_X64_PE32_GAP tree=i386-windows required=%u missing=%u",
             (U32)lanePe32Modules.size(), pe32Missing);
}

// The 32-bit DXVK override, projected over the syswow64 entry the i386
// projection just made.
//
// A 32-bit Direct3D 9 program on this lane resolves d3d9.dll out of syswow64
// and gets Wine's own, which is wined3d over OpenGL or Vulkan. A device run
// showed both dlopens failing -- there is no OpenGL on iOS and the 64-bit
// lane had no Vulkan client library at all -- and Direct3DCreate9 returning
// E_FAIL into a message box. DXVK's d3d9 needs Vulkan only, and now that the
// lane has a Vulkan ICD it is the renderer worth trying.
//
// The files are staged apart, in K_X64_WINE_DXVK_PE32_DIR, and this runs only
// when the launch set BOXEDVN_WOW64_D3D9=dxvk. Everything about the default
// path is therefore unchanged: a runtime carrying DXVK and a launch that does
// not ask for it behave identically to one that carries none.
static void projectX64WineDxvkD3d9(const BString& winePrefix) {
    const BString syswow64 = winePrefix + "/" K_GUEST_WINE_DRIVE_C "/" +
                             K_GUEST_WINE_WINDOWS "/syswow64";
    std::shared_ptr<FsNode> dxvkDirectory =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_WINE_DXVK_PE32_DIR), true);
    if (!dxvkDirectory || !dxvkDirectory->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=dxvk-i386 status=missing source=%s",
                 K_X64_WINE_DXVK_PE32_DIR);
        return;
    }
    std::shared_ptr<FsNode> destination =
        Fs::getNodeFromLocalPath(B(""), syswow64, true);
    if (!destination || !destination->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=dxvk-i386 status=missing destination=%s",
                 syswow64.c_str());
        return;
    }
    const std::vector<std::string> modules = boxedvn::x64DxvkPe32ModuleNames();
    U32 projected = 0;
    for (const std::string& name : modules) {
        const BString sourcePath =
            B(K_X64_WINE_DXVK_PE32_DIR) + "/" + name.c_str();
        std::shared_ptr<FsNode> source =
            Fs::getNodeFromLocalPath(B(""), sourcePath, true);
        if (!source || source->isDirectory()) {
            klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY name=%s source=%s status=missing",
                     name.c_str(), sourcePath.c_str());
            continue;
        }
        const BString overlay = syswow64 + "/" + name.c_str();
        const bool replaced =
            Fs::getNodeFromLocalPath(B(""), overlay, false) != nullptr;
        // Destructive on purpose, unlike the i386 projection: the whole point
        // is to replace Wine's d3d9 with DXVK's, and the entry already there
        // is the one that projection made a moment ago. FsNode::addChild sets
        // by name, so adding over it is the replacement.
        Fs::addFileNode(overlay, sourcePath, source->nativePath, false,
                        destination);
        ++projected;
        klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY name=%s source=%s destination=%s "
                 "status=projected tree=dxvk-i386 replaced=%d",
                 name.c_str(), sourcePath.c_str(), overlay.c_str(),
                 replaced ? 1 : 0);
    }
    klog_fmt("BOXEDWINE_X64_MODULE_OVERLAY tree=dxvk-i386 status=projected "
             "destination=%s required=%u projected=%u",
             syswow64.c_str(), (U32)modules.size(), projected);
}

// ---------------------------------------------------------------------------
// Audio. See docs/PLAN_X64_AUDIO.md.
//
// The route is Wine's OSS driver over BoxedWine's own /dev/dsp, which is
// already emulated for the IA-32 lane and already mixes through SDL on the
// host. Two things have to be true before Wine will use it, and neither was
// checked anywhere: the driver pair has to be in the packaged module tree,
// and mmdevapi has to be pointed at it.
//
// The pair is a unit. wineoss.drv (PE) dlopens wineoss.so (ELF) across Wine's
// private, unversioned __wine_unix_call boundary, so half a pair is not a
// degraded driver, it is undefined behaviour -- which is why "packaged" here
// means both halves or neither, and why a half-present tree is reported as a
// packaging bug rather than quietly used.
static bool x64WineOssDriverPackaged(bool& unixHalf, bool& peHalf) {
    const BString unixPath = B(K_X64_WINE_UNIX_DIR) + "/wineoss.so";
    const BString pePath = B(K_X64_WINE_PE_DIR) + "/wineoss.drv";
    std::shared_ptr<FsNode> unixNode =
        Fs::getNodeFromLocalPath(B(""), unixPath, true);
    std::shared_ptr<FsNode> peNode =
        Fs::getNodeFromLocalPath(B(""), pePath, true);
    unixHalf = unixNode != nullptr && !unixNode->isDirectory();
    peHalf = peNode != nullptr && !peNode->isDirectory();
    return unixHalf && peHalf;
}

// Insert or correct one string value in a Wine user.reg, in the file's own
// format: a section header line "[Software\\Wine\\Drivers] <seconds>", an
// optional "#time=" line, then quoted "name"="value" lines until the next
// section. Returns false when nothing had to change.
//
// This is deliberately the smallest possible editor rather than a registry
// parser: it is inserting a single known value into a file Wine itself
// rewrites, and anything it does not recognise it leaves alone.
static bool setGuestWineRegistryValue(std::string& contents,
                                      const std::string& section,
                                      const std::string& name,
                                      const std::string& quotedValue) {
    const std::string header = "[" + section + "]";
    const std::string assignment = "\"" + name + "\"=" + quotedValue;
    const std::string namePrefix = "\"" + name + "\"=";

    size_t sectionStart = contents.find(header);
    while (sectionStart != std::string::npos && sectionStart != 0 &&
           contents[sectionStart - 1] != '\n') {
        // Only a header at the start of a line counts.
        sectionStart = contents.find(header, sectionStart + 1);
    }
    if (sectionStart == std::string::npos) {
        // Wine accepts a section with a zero timestamp; it rewrites the value
        // with a real one the next time it touches the key.
        if (!contents.empty() && contents.back() != '\n') {
            contents += "\n";
        }
        contents += "\n" + header + " 0\n" + assignment + "\n";
        return true;
    }
    size_t sectionEnd = contents.find("\n[", sectionStart);
    if (sectionEnd == std::string::npos) {
        sectionEnd = contents.length();
    } else {
        sectionEnd++;
    }
    // Look for the value inside this section only.
    size_t cursor = contents.find('\n', sectionStart);
    while (cursor != std::string::npos && cursor < sectionEnd) {
        const size_t lineStart = cursor + 1;
        size_t lineEnd = contents.find('\n', lineStart);
        if (lineEnd == std::string::npos || lineEnd > sectionEnd) {
            lineEnd = sectionEnd;
        }
        if (contents.compare(lineStart, namePrefix.length(), namePrefix) == 0) {
            std::string existing =
                contents.substr(lineStart, lineEnd - lineStart);
            // A registry Wine wrote uses LF, but tolerate CRLF so a value
            // that is already correct is not rewritten on every launch.
            if (!existing.empty() && existing.back() == '\r') {
                existing.pop_back();
            }
            if (existing == assignment) {
                return false;
            }
            contents.replace(lineStart, lineEnd - lineStart, assignment);
            return true;
        }
        if (lineEnd >= sectionEnd) {
            break;
        }
        cursor = lineEnd;
    }
    // Not present: insert immediately after the header line, which is where
    // Wine's own writer puts a new value.
    size_t insertAt = contents.find('\n', sectionStart);
    if (insertAt == std::string::npos) {
        contents += "\n" + assignment + "\n";
        return true;
    }
    insertAt++;
    // Keep a "#time=" line directly under its header.
    if (contents.compare(insertAt, 6, "#time=") == 0) {
        const size_t afterTime = contents.find('\n', insertAt);
        if (afterTime != std::string::npos && afterTime + 1 <= contents.length()) {
            insertAt = afterTime + 1;
        }
    }
    contents.insert(insertAt, assignment + "\n");
    return true;
}

// mmdevapi picks its backend from HKCU\Software\Wine\Drivers value "Audio",
// and with the value absent it tries its built-in order -- pulse, alsa, oss,
// coreaudio -- so a guest with no PulseAudio daemon and no /dev/snd pays a
// startup delay for two backends that cannot work before reaching the one
// that can. Set it, but only when the driver is actually packaged: forcing a
// driver that is not there leaves the process with no audio backend at all
// rather than falling back.
static void configureX64AudioDriver(const BString& winePrefix) {
    bool unixHalf = false;
    bool peHalf = false;
    const bool packaged = x64WineOssDriverPackaged(unixHalf, peHalf);
    const char* packagedStatus = packaged
        ? "yes"
        : (unixHalf || peHalf ? "half" : "no");
    const char* registryStatus = "skipped";

    if (packaged) {
        const BString userRegistry = winePrefix + "/user.reg";
        std::shared_ptr<FsNode> node =
            Fs::getNodeFromLocalPath(B(""), userRegistry, true);
        BString nativePath = node ? node->nativePath : B("");
        if (!node || node->isDirectory() || nativePath.isEmpty()) {
            registryStatus = "no-user-reg";
        } else {
            std::string contents;
            bool readable = true;
            {
                std::ifstream in(nativePath.c_str(), std::ios::binary);
                if (!in) {
                    readable = false;
                } else {
                    in.seekg(0, std::ios::end);
                    const std::streamoff size = in.tellg();
                    in.seekg(0, std::ios::beg);
                    if (size > 0) {
                        contents.resize((size_t)size);
                        in.read(&contents[0], (std::streamsize)contents.size());
                        readable = in.good();
                    }
                }
            }
            if (!readable) {
                registryStatus = "unreadable";
            } else {
                // The doubled backslashes are the registry file's own
                // escaping, not C escaping twice over: user.reg spells this
                // section "[Software\\Wine\\Drivers]" on disk. The host-side
                // helper in ios/support/src/wine_prefix.cpp reaches the same
                // bytes by escaping a single-backslash name itself.
                if (!setGuestWineRegistryValue(contents, "Software\\\\Wine\\\\Drivers",
                                               "Audio", "\"oss\"")) {
                    registryStatus = "present";
                } else {
                    // Write through a sibling temp file and rename, so a
                    // process killed mid-write cannot leave the prefix with a
                    // truncated user.reg -- which Wine treats as an empty
                    // registry and silently reinitialises.
                    const BString tempPath = nativePath + ".boxedvn-audio";
                    bool wrote = false;
                    {
                        std::ofstream out(tempPath.c_str(), std::ios::binary | std::ios::trunc);
                        if (out) {
                            out.write(contents.data(), (std::streamsize)contents.size());
                            wrote = out.good();
                        }
                    }
                    std::error_code ec;
                    if (wrote) {
                        std::filesystem::rename(tempPath.c_str(), nativePath.c_str(), ec);
                    }
                    if (!wrote || ec) {
                        std::filesystem::remove(tempPath.c_str(), ec);
                        registryStatus = "unwritable";
                    } else {
                        registryStatus = "oss";
                    }
                }
            }
        }
    }

    klog_fmt("BOXEDWINE_X64_AUDIO_DRIVER packaged=%s registry=%s unix=%d pe=%d "
             "prefix=%s",
             packagedStatus, registryStatus, unixHalf ? 1 : 0, peHalf ? 1 : 0,
             winePrefix.c_str());
    if (!packaged) {
        klog(unixHalf || peHalf
             ? "BOXEDWINE_X64_AUDIO_DRIVER status=half-packaged: wineoss.drv and "
               "wineoss.so are one unit across Wine's private unix-call boundary; "
               "ship both or neither"
             : "BOXEDWINE_X64_AUDIO_DRIVER status=absent: no OSS driver in the "
               "packaged Wine tree, so mmdevapi keeps its built-in backend order "
               "and the 64-bit lane has no audio device");
    }
}

// Read one string value out of a Wine user.reg, in the same file format the
// writer above edits. Returns the empty string when the section or the value
// is absent, which is the state that matters for the renderer: wined3d reads
// an absent value as WINED3D_RENDERER_AUTO and picks its OpenGL adapter.
//
// Deliberately the mirror of setGuestWineRegistryValue and no larger. It
// recognises the same three shapes -- a header at the start of a line, an
// optional "#time=" line, quoted assignments until the next section -- and
// claims to understand nothing else.
static std::string getGuestWineRegistryValue(const std::string& contents,
                                             const std::string& section,
                                             const std::string& name) {
    const std::string header = "[" + section + "]";
    const std::string namePrefix = "\"" + name + "\"=";

    size_t sectionStart = contents.find(header);
    while (sectionStart != std::string::npos && sectionStart != 0 &&
           contents[sectionStart - 1] != '\n') {
        sectionStart = contents.find(header, sectionStart + 1);
    }
    if (sectionStart == std::string::npos) {
        return std::string();
    }
    size_t sectionEnd = contents.find("\n[", sectionStart);
    if (sectionEnd == std::string::npos) {
        sectionEnd = contents.length();
    } else {
        sectionEnd++;
    }
    size_t cursor = contents.find('\n', sectionStart);
    while (cursor != std::string::npos && cursor < sectionEnd) {
        const size_t lineStart = cursor + 1;
        size_t lineEnd = contents.find('\n', lineStart);
        if (lineEnd == std::string::npos || lineEnd > sectionEnd) {
            lineEnd = sectionEnd;
        }
        if (contents.compare(lineStart, namePrefix.length(), namePrefix) == 0) {
            std::string value =
                contents.substr(lineStart + namePrefix.length(),
                                lineEnd - lineStart - namePrefix.length());
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            // A string value carries the file's own quotes. Anything else --
            // a dword, say -- is not this value's type and is returned as it
            // stands rather than guessed at.
            if (value.size() >= 2 && value.front() == '"' &&
                value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        if (lineEnd >= sectionEnd) {
            break;
        }
        cursor = lineEnd;
    }
    return std::string();
}

// The ELF class of one guest file, asked of the mounted filesystem rather
// than of the build. A packaging listing is not what the guest loader sees:
// on this lane the file that answers a soname is routinely the IA-32 lane's
// shim from the root filesystem, which no 64-bit packaging step ever placed.
static const char* x64GuestFileElfClass(const BString& path) {
    std::shared_ptr<FsNode> node = Fs::getNodeFromLocalPath(B(""), path, true);
    if (!node || node->isDirectory()) {
        return "none";
    }
    FsOpenNode* probe = node->open(K_O_RDONLY);
    if (!probe) {
        return "unreadable";
    }
    U8 ident[5] = {};
    const U32 got = probe->readNative(ident, (U32)sizeof(ident));
    probe->close();
    delete probe;
    return boxedvn::guestElfClassName(ident, got);
}

// One line naming what a 64-bit guest process gets when it asks its loader for
// the OpenGL client library, so a log answers that without Wine's wgl channel
// being turned on -- and answers it with a path and an ELF class rather than
// with a sentence that reads identically whether the file is absent or
// present and unusable.
//
// A device run reported only "Failed to load libGL: libGL.so.1: wrong ELF
// class: ELFCLASS32" followed by "OpenGL support is disabled", which does not
// say which of eight candidate paths answered, and would have said the same
// had a usable library been staged one directory earlier. See
// K_X64_GUEST_OPENGL_LIB_PATH in include/guest_wine64_layout.h for why the
// expected answer is the IA-32 lane's shim and why it is left where it is.
static void reportX64GuestOpenGl() {
    const char* firstClass = "none";
    BString firstPath = B("(none)");
    BString bindsPath = B("(none)");
    bool binds = false;
    for (const std::string& candidate :
         boxedvn::guestLibrarySearchPaths(K_X64_GUEST_OPENGL_SONAME)) {
        const BString path = BString::copy(candidate.c_str());
        const char* elfClass = x64GuestFileElfClass(path);
        if (std::string(elfClass) == "none") {
            continue;
        }
        if (std::string(firstClass) == "none") {
            firstClass = elfClass;
            firstPath = path;
        }
        // The loader does not stop at the first file it opens; it stops at
        // the first one it can use. Both are reported, because on this lane
        // they are different files whenever they exist at all.
        if (boxedvn::guestElfClassIsUsable(elfClass)) {
            bindsPath = path;
            binds = true;
            break;
        }
    }
    klog_fmt("BOXEDWINE_X64_OPENGL soname=%s found=%s class=%s binds=%s "
             "lane64=%s",
             K_X64_GUEST_OPENGL_SONAME, firstPath.c_str(), firstClass,
             bindsPath.c_str(),
             x64GuestFileElfClass(B(K_X64_GUEST_OPENGL_LIB_PATH)));
    if (!binds) {
        klog("BOXEDWINE_X64_OPENGL status=absent: this build defines no GL "
             "backend, the host is Metal-only and the 64-bit X11 bridge serves "
             "no GLX, so wined3d's OpenGL adapter cannot be built and every "
             "Direct3D route has to be Vulkan or Metal; see "
             "docs/KNOWN_LIMITATIONS_IOS.md section 3");
    }
}

// wined3d chooses its backend once, from HKCU\Software\Wine\Direct3D value
// "renderer", and Wine 9 reads an absent value as WINED3D_RENDERER_AUTO, which
// is its OpenGL adapter. On a lane with no OpenGL that default is the one
// choice guaranteed to fail, and it fails late: a device run of a 32-bit
// Direct3D 11 program loaded Wine's own d3d11, which loaded wined3d, which
// loaded opengl32, which reached for the library the witness above reports,
// and only then said "Failed to get a GL context for adapter".
//
// So name the adapter the lane actually packages. Written here rather than by
// the iOS prefix policy because that policy prepares the IA-32 lane's prefix
// only -- the 64-bit lane's prefix is this launch's, the same one the audio
// driver value above is written into, and the write follows the same rule as
// that one: only when the backend is really there. A prefix that has not
// booted yet has no user.reg to edit and gets the value on its next launch,
// which is the behaviour the audio value has had since it was added.
static void configureX64WineD3dRenderer(const BString& winePrefix) {
    const char* vulkanClass =
        x64GuestFileElfClass(B(K_X64_GUEST_VULKAN_LIB_PATH));
    const bool vulkanUsable = boxedvn::guestElfClassIsUsable(vulkanClass);
    const char* registryStatus = "no-user-reg";
    std::string renderer;

    const BString userRegistry = winePrefix + "/user.reg";
    std::shared_ptr<FsNode> node =
        Fs::getNodeFromLocalPath(B(""), userRegistry, true);
    BString nativePath = node ? node->nativePath : B("");
    if (node && !node->isDirectory() && !nativePath.isEmpty()) {
        std::string contents;
        bool readable = true;
        {
            std::ifstream in(nativePath.c_str(), std::ios::binary);
            if (!in) {
                readable = false;
            } else {
                in.seekg(0, std::ios::end);
                const std::streamoff size = in.tellg();
                in.seekg(0, std::ios::beg);
                if (size > 0) {
                    contents.resize((size_t)size);
                    in.read(&contents[0], (std::streamsize)contents.size());
                    readable = in.good();
                }
            }
        }
        if (!readable) {
            registryStatus = "unreadable";
        } else {
            renderer = getGuestWineRegistryValue(
                contents, K_X64_WINED3D_REGISTRY_SECTION,
                K_X64_WINED3D_RENDERER_NAME);
            if (!boxedvn::shouldConfigureX64WineD3dVulkan(vulkanUsable)) {
                registryStatus = "skipped";
            } else if (!setGuestWineRegistryValue(
                           contents, K_X64_WINED3D_REGISTRY_SECTION,
                           K_X64_WINED3D_RENDERER_NAME,
                           "\"" K_X64_WINED3D_RENDERER_VULKAN "\"")) {
                registryStatus = "present";
                renderer = K_X64_WINED3D_RENDERER_VULKAN;
            } else {
                // Sibling temp file and rename, as the audio value does: a
                // process killed mid-write must not leave a truncated
                // user.reg, which Wine treats as an empty registry and
                // silently reinitialises.
                const BString tempPath = nativePath + ".boxedvn-d3d";
                bool wrote = false;
                {
                    std::ofstream out(tempPath.c_str(),
                                      std::ios::binary | std::ios::trunc);
                    if (out) {
                        out.write(contents.data(),
                                  (std::streamsize)contents.size());
                        wrote = out.good();
                    }
                }
                std::error_code ec;
                if (wrote) {
                    std::filesystem::rename(tempPath.c_str(),
                                            nativePath.c_str(), ec);
                }
                if (!wrote || ec) {
                    std::filesystem::remove(tempPath.c_str(), ec);
                    registryStatus = "unwritable";
                } else {
                    registryStatus = K_X64_WINED3D_RENDERER_VULKAN;
                    renderer = K_X64_WINED3D_RENDERER_VULKAN;
                }
            }
        }
    }

    klog_fmt("BOXEDWINE_X64_WINED3D_RENDERER vulkan_client=%s registry=%s "
             "renderer=%s adapter=%s prefix=%s",
             vulkanClass, registryStatus,
             renderer.empty() ? "(unset)" : renderer.c_str(),
             boxedvn::wined3dAdapterForRenderer(renderer),
             winePrefix.c_str());
    if (!vulkanUsable) {
        klog("BOXEDWINE_X64_WINED3D_RENDERER status=no-vulkan-client: the "
             "64-bit Vulkan client library is missing or is the IA-32 shim, so "
             "the renderer is left as it stands rather than naming a backend "
             "this lane cannot reach");
    }
}

// Relay tracing is restricted from the registry and from nowhere else: Wine
// reads HKCU\Software\Wine\Debug values RelayInclude and RelayExclude once,
// when the first traced call is made, and neither has an environment variable.
// So a launch that asked for the trace writes both here, into the prefix it is
// about to open, by exactly the route the audio driver and the wined3d
// renderer take -- read, edit, sibling temp file, rename -- and for the same
// reason: a process killed mid-write must not leave a truncated user.reg,
// which Wine treats as an empty registry and silently reinitialises.
//
// Both values are written together or not at all. The include list without
// the exclude list is the flood the include list exists to prevent, and a
// prefix carrying only half of the pair would produce a log nobody can read
// while looking exactly like one that was configured.
//
// A prefix that has not booted yet has no user.reg to edit and gets the values
// on its next launch, which is the behaviour the other two registry values
// here have had since they were added.
static void configureX64WineRelayFilter(const BString& winePrefix) {
    const char* registryStatus = "no-user-reg";

    const BString userRegistry = winePrefix + "/user.reg";
    std::shared_ptr<FsNode> node =
        Fs::getNodeFromLocalPath(B(""), userRegistry, true);
    BString nativePath = node ? node->nativePath : B("");
    if (node && !node->isDirectory() && !nativePath.isEmpty()) {
        std::string contents;
        bool readable = true;
        {
            std::ifstream in(nativePath.c_str(), std::ios::binary);
            if (!in) {
                readable = false;
            } else {
                in.seekg(0, std::ios::end);
                const std::streamoff size = in.tellg();
                in.seekg(0, std::ios::beg);
                if (size > 0) {
                    contents.resize((size_t)size);
                    in.read(&contents[0], (std::streamsize)contents.size());
                    readable = in.good();
                }
            }
        }
        if (!readable) {
            registryStatus = "unreadable";
        } else {
            // Both edits are made before either is judged: |= on a bool, not
            // ||, so the second call is not skipped once the first has already
            // reported a change.
            bool changed = setGuestWineRegistryValue(
                contents, K_X64_WINE_DEBUG_REGISTRY_SECTION,
                K_X64_WINE_RELAY_INCLUDE_NAME,
                "\"" K_X64_WINE_RELAY_INCLUDE "\"");
            changed |= setGuestWineRegistryValue(
                contents, K_X64_WINE_DEBUG_REGISTRY_SECTION,
                K_X64_WINE_RELAY_EXCLUDE_NAME,
                "\"" K_X64_WINE_RELAY_EXCLUDE "\"");
            if (!changed) {
                registryStatus = "present";
            } else {
                const BString tempPath = nativePath + ".boxedvn-relay";
                bool wrote = false;
                {
                    std::ofstream out(tempPath.c_str(),
                                      std::ios::binary | std::ios::trunc);
                    if (out) {
                        out.write(contents.data(),
                                  (std::streamsize)contents.size());
                        wrote = out.good();
                    }
                }
                std::error_code ec;
                if (wrote) {
                    std::filesystem::rename(tempPath.c_str(),
                                            nativePath.c_str(), ec);
                }
                if (!wrote || ec) {
                    std::filesystem::remove(tempPath.c_str(), ec);
                    registryStatus = "unwritable";
                } else {
                    registryStatus = "written";
                }
            }
        }
    }

    klog_fmt("BOXEDWINE_X64_WINE_RELAY registry=%s channels=%s include='%s' "
             "exclude='%s' prefix=%s",
             registryStatus, K_X64_WINE_TRACE_CHANNELS,
             K_X64_WINE_RELAY_INCLUDE, K_X64_WINE_RELAY_EXCLUDE,
             winePrefix.c_str());
}

// The two OSS nodes Wine's driver opens, taken through the guest filesystem
// after every overlay is mounted. They are registered unconditionally in
// buildVirtualFileSystem() and shared with the IA-32 lane, so the interesting
// question is not whether the code ran but whether a 64-bit process can see
// and open them.
static void reportX64AudioDeviceNodes() {
    // Presence and mode only. Opening /dev/dsp here would construct a DevDsp
    // and with it a KDspAudio voice, which is a side effect a diagnostic has
    // no business having; whether Wine's open succeeds is answered by
    // BOXEDWINE_X64_DSP on the first ioctl anyway.
    for (const char* path : {"/dev/dsp", "/dev/mixer"}) {
        std::shared_ptr<FsNode> node =
            Fs::getNodeFromLocalPath(B(""), BString::copy(path), true);
        klog_fmt("BOXEDWINE_X64_AUDIO_DEVNODE path=%s present=%d mode=0%o "
                 "sound=%d",
                 path, node ? 1 : 0,
                 node ? (unsigned)(node->getMode() & 0777) : 0u,
                 KSystem::soundEnabled ? 1 : 0);
    }
}

// Wine strips "64" from its own loader name to find the loader for a 32-bit
// image and hands the image to start.exe when that yields a name; start.exe
// then spawns that loader, which the layers do not ship. Upstream's WoW64
// layout names the loader wine, so the lane launches through that name and
// the packaged wine64 binaries are aliased under it here. A link node with a
// relative target resolves inside the module directory.
// A link is not a binary. "status=linked" said only that a guest link node was
// created, and for wine-preloader that read as though a preloader were
// available: the packaged wine64 layer has never carried wine64-preloader,
// scripts/validate-wine64-runtime.sh does not check for it, and the link
// therefore names a file that does not resolve. Wine's preloader_exec()
// answers a failed exec of it by running the loader directly
// (execv(argv[1], argv + 1)), which is visible in every device log as a
// wine-preloader execve immediately followed by a loader execve with one
// argument fewer and no BOXEDWINE_X64_EXEC line between them.
//
// That is not a defect -- ntdll's mmap_init reserves its own arena precisely
// when no preloader ran, and that is the branch this lane wants -- but it
// decides where the guest's address space comes from, so the line has to say
// it rather than leave it to be inferred from a pair of execve traces.
static void aliasX64WineLoader(const char* aliasPath, const char* targetName) {
    std::shared_ptr<FsNode> moduleRoot =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_WINE_MODULE_ROOT), true);
    const bool targetPresent =
        moduleRoot &&
        Fs::getNodeFromLocalPath(B(""),
                                 B(K_X64_WINE_MODULE_ROOT) + "/" +
                                     BString::copy(targetName),
                                 true) != nullptr;
    if (Fs::getNodeFromLocalPath(B(""), BString::copy(aliasPath), false)) {
        klog_fmt("BOXEDWINE_X64_LOADER_ALIAS alias=%s target=%s status=present "
                 "target_present=%d",
                 aliasPath, targetName, targetPresent ? 1 : 0);
        return;
    }
    if (!moduleRoot || !moduleRoot->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_LOADER_ALIAS alias=%s target=%s status=no-module-root",
                 aliasPath, targetName);
        return;
    }
    Fs::addFileNode(BString::copy(aliasPath), BString::copy(targetName), B(""),
                    false, moduleRoot);
    klog_fmt("BOXEDWINE_X64_LOADER_ALIAS alias=%s target=%s status=linked "
             "target_present=%d",
             aliasPath, targetName, targetPresent ? 1 : 0);
}

// The side-by-side assembly store, projected the way system32 is.
//
// Wine ships no winsxs tree and wine.inf never mentions one. A real prefix gets
// its winsxs from wineboot's fake-DLL install: install_fake_dll calls
// register_fake_dll for every builtin it copies in, which enumerates that
// module's RT_MANIFEST resources and writes
// windows\winsxs\manifests\<arch>_<name>_<key>_<version>_<lang>_deadbeef.manifest
// plus windows\winsxs\<same stem>\<file> for each resource NAMED WINE_MANIFEST
// (dlls/setupapi/fakedll.c). This lane never gets that pass -- system32 here is
// the in-memory projection below rather than a directory wineboot filled in --
// so a prefix has no winsxs at all.
//
// Nothing reports that, which is why it survived. ntdll's lookup_winsxs is the
// FIRST thing lookup_assembly tries; with no manifests directory it returns
// STATUS_NO_SUCH_FILE and the loader falls through to the private-assembly
// probes beside the program -- <dir>\<name>.dll, <dir>\<name>.manifest and the
// two <name>\<name> forms. A device log shows exactly those four probes missing
// for Microsoft.Windows.Common-Controls, in the launched program and in Wine's
// own helpers. When they miss, parse_depend_manifests fails the whole
// activation context with STATUS_SXS_CANT_GEN_ACTCTX, and the program runs with
// none: a manifest asking for common controls version 6 gets version 5.
//
// The tree is staged at K_X64_GUEST_WINSXS_DIR by the runtime builder, from the
// packaged modules' own WINE_MANIFEST resources, with the assembly directories
// holding guest links to the packaged modules rather than copies. Both
// architectures stage at that one path -- the 64-bit half in wine64.zip and the
// 32-bit half in the PE32 archive -- so what is projected here is their union.
//
// Two levels deep and no deeper: that is the shape Wine creates, and a generic
// recursive copy would follow whatever a future staging put there.
static void projectX64WineSxsAssemblies(const BString& winePrefix) {
    const BString winsxs = winePrefix + "/" K_GUEST_WINE_DRIVE_C "/" +
                           K_GUEST_WINE_WINDOWS "/" K_GUEST_WINE_WINSXS;
    std::shared_ptr<FsNode> stagedRoot =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_GUEST_WINSXS_DIR), true);
    if (!stagedRoot || !stagedRoot->isDirectory()) {
        // A runtime assembled before this staging existed. Named rather than
        // silent: this is the state in which every program gets version 5.
        klog_fmt("BOXEDWINE_X64_SXS_OVERLAY source=%s destination=%s "
                 "manifests=0 assemblies=0 status=no-staged-tree",
                 K_X64_GUEST_WINSXS_DIR, winsxs.c_str());
        return;
    }
    Fs::makeLocalDirs(winsxs);
    std::shared_ptr<FsNode> destinationRoot =
        Fs::getNodeFromLocalPath(B(""), winsxs, true);
    if (!destinationRoot || !destinationRoot->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_SXS_OVERLAY source=%s destination=%s "
                 "manifests=0 assemblies=0 status=unavailable",
                 K_X64_GUEST_WINSXS_DIR, winsxs.c_str());
        return;
    }

    U32 manifests = 0;
    U32 assemblyFiles = 0;
    U32 preserved = 0;
    std::vector<std::shared_ptr<FsNode> > stagedChildren;
    stagedRoot->getAllChildren(stagedChildren);
    destinationRoot->reserveChildren(stagedChildren.size());
    for (const std::shared_ptr<FsNode>& child : stagedChildren) {
        if (!child || child->name.isEmpty() || !child->isDirectory()) {
            continue;
        }
        const BString childDestination = winsxs + "/" + child->name;
        Fs::makeLocalDirs(childDestination);
        std::shared_ptr<FsNode> childDestinationNode =
            Fs::getNodeFromLocalPath(B(""), childDestination, true);
        if (!childDestinationNode || !childDestinationNode->isDirectory()) {
            continue;
        }
        const bool isManifestDirectory =
            child->name == K_X64_GUEST_WINSXS_MANIFEST_SUBDIR;
        std::vector<std::shared_ptr<FsNode> > entries;
        child->getAllChildren(entries);
        childDestinationNode->reserveChildren(entries.size());
        for (const std::shared_ptr<FsNode>& entry : entries) {
            if (!entry || entry->name.isEmpty()) {
                continue;
            }
            const BString destination = childDestination + "/" + entry->name;
            const bool destinationExists =
                Fs::getNodeFromLocalPath(B(""), destination, false) != nullptr;
            // The same non-destructive rule system32 uses: a real file an
            // installer put in the prefix always wins. A third-party assembly
            // installed into this prefix is exactly that case.
            if (!boxedvn::shouldProjectGuestWineSystemModule(
                    true, entry->isDirectory(), destinationExists)) {
                if (destinationExists) {
                    ++preserved;
                }
                continue;
            }
            Fs::addFileNode(destination, entry->path, B(""), false,
                            childDestinationNode);
            if (isManifestDirectory) {
                ++manifests;
            } else {
                ++assemblyFiles;
            }
        }
    }
    // The witness for the prefix: how many assemblies this launch made
    // activatable at all. Zero is the state the private-assembly probes in an
    // older log were the only evidence of.
    klog_fmt("BOXEDWINE_X64_SXS_OVERLAY source=%s destination=%s manifests=%u "
             "assemblies=%u preserved=%u status=ready",
             K_X64_GUEST_WINSXS_DIR, winsxs.c_str(), manifests, assemblyFiles,
             preserved);
}

static void projectX64WineSystemModules(const BString& winePrefix) {
    const BString system32 = winePrefix + "/" K_GUEST_WINE_DRIVE_C "/" +
                             K_GUEST_WINE_WINDOWS "/" K_GUEST_WINE_SYSTEM32;
    Fs::makeLocalDirs(system32);
    // The 32-bit builtins go to syswow64, beside this; Wine keeps the two
    // architectures in separate prefix directories.
    projectX64WinePe32Modules(winePrefix);

    std::shared_ptr<FsNode> sourceDirectory =
        Fs::getNodeFromLocalPath(B(""), B(K_X64_WINE_PE_DIR), true);
    std::shared_ptr<FsNode> destinationDirectory =
        Fs::getNodeFromLocalPath(B(""), system32, true);
    if (!sourceDirectory || !sourceDirectory->isDirectory() ||
        !destinationDirectory || !destinationDirectory->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_SYSTEM32_OVERLAY source=%s destination=%s "
                 "projected=0 preserved=0 status=unavailable",
                 K_X64_WINE_PE_DIR, system32.c_str());
        return;
    }

    std::vector<std::shared_ptr<FsNode> > sourceModules;
    sourceDirectory->getAllChildren(sourceModules);
    U32 projected = 0;
    U32 preserved = 0;
    U32 skipped = 0;
    destinationDirectory->reserveChildren(sourceModules.size());
    for (const std::shared_ptr<FsNode>& source : sourceModules) {
        if (!source || source->name.isEmpty()) {
            ++skipped;
            continue;
        }
        const BString destination = system32 + "/" + source->name;
        const bool destinationExists =
            Fs::getNodeFromLocalPath(B(""), destination, false) != nullptr;
        if (!boxedvn::shouldProjectGuestWineSystemModule(
                source != nullptr, source && source->isDirectory(),
                destinationExists)) {
            if (destinationExists) {
                ++preserved;
            } else {
                ++skipped;
            }
            continue;
        }
        Fs::addFileNode(destination, source->path, B(""), false,
                        destinationDirectory);
        ++projected;
    }
    klog_fmt("BOXEDWINE_X64_SYSTEM32_OVERLAY source=%s destination=%s "
             "projected=%u preserved=%u skipped=%u status=ready",
             K_X64_WINE_PE_DIR, system32.c_str(), projected, preserved,
             skipped);

    // The kernel drivers go one level down. Wine's packaged builtin tree
    // is flat, but the services wine.inf registers name their binary as
    // "%11%\drivers\<name>.sys", so a driver projected only beside the
    // DLLs is not where winedevice.exe looks for it. Four device runs
    // logged the consequence: stat 'system32/drivers' -> ENOENT (three
    // times, Wine's case-insensitive retry re-reading all 961 system32
    // entries), then 'system32/drivers/mountmgr.sys' -> ENOENT, and then
    // winedevice.exe and services.exe both exiting 0 seconds after boot.
    // Nothing else publishes the \DosDevices drive links the shell
    // enumerates, which is why the desktop listed no drives at all, not
    // even C:.
    //
    // wineboot --init would create the directory as part of its fake-dll
    // install, but it is still reading wine.inf when services.exe starts
    // winedevice.exe, so the prefix has to carry the directory before the
    // first boot rather than during it.
    const BString drivers = system32 + "/" K_GUEST_WINE_DRIVERS;
    Fs::makeLocalDirs(drivers);
    std::shared_ptr<FsNode> driversDirectory =
        Fs::getNodeFromLocalPath(B(""), drivers, true);
    if (!driversDirectory || !driversDirectory->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_DRIVERS_OVERLAY destination=%s projected=0 "
                 "mountmgr=0 status=unavailable", drivers.c_str());
        return;
    }
    U32 driversProjected = 0;
    U32 mountManagerProjected = 0;
    for (const std::shared_ptr<FsNode>& source : sourceModules) {
        if (!source || source->name.isEmpty()) {
            continue;
        }
        if (!boxedvn::isGuestWineKernelDriverModule(source->name.c_str())) {
            continue;
        }
        const BString driverDestination = drivers + "/" + source->name;
        const bool driverExists =
            Fs::getNodeFromLocalPath(B(""), driverDestination, false) != nullptr;
        if (!boxedvn::shouldProjectGuestWineSystemModule(
                source != nullptr, source && source->isDirectory(),
                driverExists)) {
            continue;
        }
        Fs::addFileNode(driverDestination, source->path, B(""), false,
                        driversDirectory);
        ++driversProjected;
        if (source->name.toLowerCase() == "mountmgr.sys") {
            mountManagerProjected = 1;
        }
    }
    // The witness for the next device log: mountmgr=1 says the mount
    // manager has a binary to load, which is why the drives come back.
    klog_fmt("BOXEDWINE_X64_DRIVERS_OVERLAY destination=%s projected=%u "
             "mountmgr=%u status=ready",
             drivers.c_str(), driversProjected, mountManagerProjected);
}

// WINEARCH, for the launch marker only. Empty means the caller left it to
// Wine, which is the ordinary 32-bit case.
static BString guestWineArchFromEnv(const std::vector<BString>& envValues) {
    BString selected;
    for (const BString& entry : envValues) {
        if (entry.startsWith("WINEARCH=")) {
            selected = entry.substr(9);
        }
    }
    return selected;
}

static void addDefaultUtf8LocaleEnv(std::vector<BString>& envValues, bool guestHasUtf8Locale) {
    if (!guestHasUtf8Locale) {
        return;
    }
    addDefaultEnvValue(envValues, "LANG=en_US.UTF-8");
    addDefaultEnvValue(envValues, "LC_ALL=en_US.UTF-8");
}

static bool guestHasUtf8Locale() {
    const char* localePaths[] = {
        "/usr/lib/locale/locale-archive",
        "/usr/lib/locale/en_US.UTF-8",
        "/usr/lib/locale/en_US.utf8",
        "/usr/lib/locale/C.UTF-8",
        "/usr/lib/locale/C.utf8",
    };

    for (const char* path : localePaths) {
        if (Fs::getNodeFromLocalPath(B(""), BString::copy(path), false)) {
            return true;
        }
    }
    return false;
}

#ifdef __TEST
static bool hasExactEnvValue(const std::vector<BString>& envValues, const char* value) {
    for (auto& envValue : envValues) {
        if (envValue == value) {
            return true;
        }
    }
    return false;
}

void testStartupArgsDefaultUtf8LocaleEnvironment() {
    std::vector<BString> unsupportedEnvValues;
    addDefaultUtf8LocaleEnv(unsupportedEnvValues, false);

    if (unsupportedEnvValues.size() != 0) {
        testFail("default UTF-8 locale env values were added without guest locale support");
    }

    std::vector<BString> envValues;
    addDefaultUtf8LocaleEnv(envValues, true);

    if (!hasExactEnvValue(envValues, "LANG=en_US.UTF-8")) {
        testFail("default LANG was not added");
    }
    if (!hasExactEnvValue(envValues, "LC_ALL=en_US.UTF-8")) {
        testFail("default LC_ALL was not added");
    }

    std::vector<BString> explicitEnvValues;
    explicitEnvValues.push_back(B("LANG=C"));
    explicitEnvValues.push_back(B("LC_ALL=C"));
    addDefaultUtf8LocaleEnv(explicitEnvValues, true);

    if (explicitEnvValues.size() != 2) {
        testFail("explicit locale env values were not preserved");
    }
    if (!hasExactEnvValue(explicitEnvValues, "LANG=C")) {
        testFail("explicit LANG was overwritten");
    }
    if (!hasExactEnvValue(explicitEnvValues, "LC_ALL=C")) {
        testFail("explicit LC_ALL was overwritten");
    }
}
#endif

FsOpenNode* openKernelCommandLine(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new BufferAccess(node, flags, B(""));
}

// This parses a resolution given as a string in the format of: '800x600'
// with the width being the first number
//
// This logic taken from WINE source code in desktop.c
int StartUpArgs::parse_resolution(const char *resolutionString, U32 *width, U32 *height)
{
    // Moving pointer for where the character parsing completes at
    char *end;

    // Parse the width
    *width = (U32)strtoul(resolutionString, &end, 10);

    // Width parsing failed, pointer not moved
    if (end == resolutionString) 
        return false;
	
    // If the next character is not an 'x' then it is an improper resolution format
    if (*end != 'x') 
        return false;

    // Advance the string to beyond the 'x'
    resolutionString = end + 1;

    // Attempt to parse the height
    *height = (U32)strtoul(resolutionString, &end, 10);

    // Height parsing failed, character not null (end of string)
    if (*end)
        return false;

    // Made it!  Full string was parsed
    return true;
}

void StartUpArgs::buildVirtualFileSystem() {
    Fs::makeLocalDirs(B("/dev"));
    Fs::makeLocalDirs(B("/proc"));
    Fs::makeLocalDirs(B("/mnt"));
    Fs::makeLocalDirs(B("/etc"));

    std::shared_ptr<FsNode> rootNode = Fs::getNodeFromLocalPath(B(""), B("/"), true);
    std::shared_ptr<FsNode> devNode = Fs::addFileNode(B("/dev"), B(""), rootNode->nativePath.stringByApppendingPath("dev"), true, rootNode);
    std::shared_ptr<FsNode> inputNode = Fs::addFileNode(B("/dev/input"), B(""), B(""), true, devNode);
    KSystem::setProcNode(
        Fs::addFileNode(B("/proc"), B(""), B(""), true, rootNode));
    std::shared_ptr<FsNode> procSysNode = Fs::addFileNode(B("/proc/sys"), B(""), B(""), true, KSystem::procNode);
    std::shared_ptr<FsNode> procNetNode = Fs::addFileNode(B("/proc/net"), B(""), B(""), true, KSystem::procNode);
    std::shared_ptr<FsNode> procSysKernelNode = Fs::addFileNode(B("/proc/sys/kernel"), B(""), B(""), true, procSysNode);
    Fs::addVirtualFile(B("/proc/sys/kernel/ngroups_max"), [](const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
        return new BufferAccess(node, flags, B("65536"));
        }, K__S_IREAD, k_mdev(0, 0), procSysKernelNode);
    Fs::addVirtualFile(B("/proc/filesystems"), [](const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
        return new BufferAccess(node, flags, B("nodev\tsysfs\nnodev\trootfs\nnodev\tbdev\nnodev\tproc\nnodev\tcpuset\nnodev\tcgroup\nnodev\tcgroup2\nnodev\ttmpfs\nnodev\tdevtmpfs\nnodev\tdebugfs\nnodev\ttracefs\nnodev\tsecurityfs\nnodev\tsockfs\nnodev\tdax\nnodev\tbpf\nnodev\tpipefs\nnodev\thugetlbfs\nnodev\tdevpts\nodev\tmqueue\nnodev\tpstore\next3\next2\next4\nnodev\tautofs\nfuseblk\nnodev\tfuse\nnodev\tfusectl\n"));
        }, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addVirtualFile(B("/proc/net/dev"), openProcNetDev, K__S_IREAD, k_mdev(0, 0), procNetNode);

    std::shared_ptr<FsNode> etcNode = Fs::getNodeFromLocalPath(B(""), B("/etc"), true);
    createSysfs(rootNode);    

    Fs::addVirtualFile(B("/dev/tty0"), openDevTTY, K__S_IREAD|K__S_IWRITE|K__S_IFCHR, k_mdev(4, 0), devNode);
    Fs::addVirtualFile(B("/dev/tty"), openDevTTY, K__S_IREAD|K__S_IWRITE|K__S_IFCHR, k_mdev(4, 0), devNode);
    Fs::addVirtualFile(B("/dev/tty2"), openDevTTY, K__S_IREAD|K__S_IWRITE|K__S_IFCHR, k_mdev(4, 2), devNode); // used by XOrg
    Fs::addVirtualFile(B("/dev/urandom"), openDevURandom, K__S_IREAD|K__S_IFCHR, k_mdev(1, 9), devNode);
    Fs::addVirtualFile(B("/dev/random"), openDevURandom, K__S_IREAD|K__S_IFCHR, k_mdev(1, 8), devNode);
    Fs::addVirtualFile(B("/dev/null"), openDevNull, K__S_IREAD|K__S_IWRITE|K__S_IFCHR, k_mdev(1, 3), devNode);
    Fs::addVirtualFile(B("/dev/zero"), openDevZero, K__S_IREAD|K__S_IWRITE|K__S_IFCHR, k_mdev(1, 5), devNode);
    Fs::addVirtualFile(B("/proc/meminfo"), openMemInfo, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addVirtualFile(B("/proc/stat"), openProcStat, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addVirtualFile(B("/proc/uptime"), openUptime, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addVirtualFile(B("/proc/cpuinfo"), openCpuInfo, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addDynamicLinkFile(B("/proc/self"), k_mdev(0, 0), KSystem::procNode, true, [] {
        return BString::valueOf(KThread::currentThread()->process->id);
        });
    Fs::addVirtualFile(B("/proc/mounts"), [](const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
        return new BufferAccess(node, flags, B("proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n/dev/nvme0n1p5 / ext4 rw,relatime,errors=remount-ro 0 0\nudev /dev devtmpfs rw,nosuid,relatime,size=16371216k,nr_inodes=4092804,mode=755,inode64 0 0"));
        }, K__S_IREAD, k_mdev(0, 0), KSystem::procNode);
    Fs::addVirtualFile(B("/proc/cmdline"), openKernelCommandLine, K__S_IREAD, k_mdev(0, 0), KSystem::procNode); // kernel command line
    Fs::addVirtualFile(B("/dev/fb0"), openDevFB, K__S_IREAD | K__S_IWRITE | K__S_IFCHR, k_mdev(0x1d, 0), devNode);
    Fs::addVirtualFile(B("/dev/input/mice"), openDevInputTouch, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(0xd, 0x43), inputNode);
    Fs::addVirtualFile(B("/dev/input/event3"), openDevInputTouch, K__S_IWRITE|K__S_IREAD|K__S_IFCHR, k_mdev(0xd, 0x43), inputNode);
    Fs::addVirtualFile(B("/dev/input/event4"), openDevInputKeyboard, K__S_IWRITE|K__S_IREAD|K__S_IFCHR, k_mdev(0xd, 0x44), inputNode);
	Fs::addVirtualFile(B("/dev/dsp"), openDevDsp, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 3), devNode);
	Fs::addVirtualFile(B("/dev/mixer"), openDevMixer, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 0), devNode);
    Fs::addVirtualFile(B("/dev/sequencer"), openDevSequencer, K__S_IWRITE | K__S_IREAD | K__S_IFCHR, k_mdev(14, 1), devNode);    

    Fs::addVirtualFile(B("/etc/hostname"), openHostname, K__S_IREAD, k_mdev(0, 0), etcNode);
    Fs::addVirtualFile(B("/etc/hosts"), openHosts, K__S_IREAD, k_mdev(0, 0), etcNode);
}

std::vector<BString> StartUpArgs::buildArgs() {
    std::vector<BString> args;

    if (root.length()) {
        args.push_back(B("-root"));
        args.push_back(root);
    }
    for (auto& z : zips) {
        args.push_back(B("-zip"));
        args.push_back(z);
    }
    if (title.length()) {
        args.push_back(B("-title"));
        args.push_back(title);
    }
    if (userId != UID) {
        args.push_back(B("-uid"));
        args.push_back(BString::valueOf(userId));
    }
    if (effectiveUserId != UID) {
        args.push_back(B("-euid"));
        args.push_back(BString::valueOf(effectiveUserId));
    }
    if (nozip) {
        args.push_back(B("-nozip"));
    }
    if (workingDir.length()) {
        args.push_back(B("-w"));
        args.push_back(workingDir);
    }
    if (sdlFullScreen == FULLSCREEN_STRETCH) {
        args.push_back(B("-fullscreen"));
    }
    if (vsync != VSYNC_DEFAULT) {
        args.push_back(B("-vsync"));
        args.push_back(BString::valueOf(vsync));
    }
    if (sdlFullScreen == FULLSCREEN_ASPECT) {
        args.push_back(B("-fullscreenAspect"));
    }
    if (sdlScaleX != 100) {
        args.push_back(B("-scale"));
        args.push_back(BString::valueOf(sdlScaleX));
    }
    if (sdlScaleQuality.length() && sdlScaleQuality != "0") {
        args.push_back(B("-scale_quality"));
        args.push_back(sdlScaleQuality);
    }
    args.push_back(B("-resolution"));
    args.push_back(BString::valueOf(screenCx) + "x" + BString::valueOf(screenCy));
    if (screenBpp != 32) {
        args.push_back(B("-bpp"));
        args.push_back(BString::valueOf(screenBpp));
    }
    if (glExt.length()) {
        args.push_back(B("-glext"));
        args.push_back(glExt);
    }
    if (dpiAware) {
        args.push_back(B("-dpiAware"));
    }
    if (openGlLib.length()) {
        args.push_back(B("-opengl"));
        args.push_back(openGlLib);
    }
    if (ttyPrepend) {
        args.push_back(B("-ttyPrepend"));
    }
    if (recordAutomation.length()) {
        args.push_back(B("-record"));
        args.push_back(recordAutomation);
    }
    if (runAutomation.length()) {
        args.push_back(B("-automation"));
        args.push_back(runAutomation);
    }
    if (skipFrameFPS) {
        args.push_back(B("-skipFrameFPS"));
        args.push_back(BString::valueOf(skipFrameFPS));
    }
    if (cpuAffinity) {
        args.push_back(B("-cpuAffinity"));
        args.push_back(BString::valueOf(cpuAffinity));
    }
    if (pollRate > 0) {
        args.push_back(B("-pollRate"));
        args.push_back(BString::valueOf(this->pollRate));
    }
    for (auto& e : envValues) {
        args.push_back(B("-env"));
        args.push_back(e);
    }
    if (logPath.c_str()) {
        args.push_back(B("-log"));
        args.push_back(logPath);
    }
    for (auto& m : mountInfo) {
        if (m.wine) {
            args.push_back(B("-mount_drive"));
        } else {
            args.push_back(B("-mount"));
        }
        args.push_back(m.nativePath);
        args.push_back(m.localPath);
    }    
    if (!this->ddrawOverridePath.isEmpty()) {
        args.push_back(B("-ddrawOverride"));
        args.push_back(this->ddrawOverridePath);
    }
    args.push_back(B("-dxvk"));
    args.push_back(B(this->enableDXVK ? "1" : "0"));
    if (!this->x64ModuleOverlayPath.isEmpty()) {
        args.push_back(B("-x64modules"));
        args.push_back(this->x64ModuleOverlayPath);
    }
    if (this->disableHideCursor) {
        args.push_back(B("-disableHideCursor"));
    }
    if (this->forceRelativeMouse) {
        args.push_back(B("-forceRelativeMouse"));
    }
    if (this->cacheReads) {
        args.push_back(B("-cacheReads"));
    }
    if (this->disableWasmJitForWrittenCode) {
        args.push_back(B("-disableWasmJitForWrittenCode"));
    }
    if (this->interpreterAnonymousExecutable) {
        args.push_back(B("-interpreterAnonymousExecutable"));
    }
    for (const auto& module : this->interpreterModules) {
        args.push_back(B("-interpreterModule"));
        args.push_back(module);
    }
    for (const auto& range : this->interpreterRanges) {
        char encodedRange[18];
        snprintf(encodedRange, sizeof(encodedRange), "%08x-%08x",
                 range.first, range.second);
        args.push_back(B("-interpreterRange"));
        args.push_back(BString::copy(encodedRange));
    }
    for (auto& a : this->args) {
        args.push_back(a);
    }
    return args;
}

// The guest's own view of the packaged Wine64 module tree, taken through the
// guest filesystem after every ZIP overlay is mounted and before Wine starts.
// A packaged ZIP listing is not device acceptance: the archive that failed
// with STATUS_DLL_NOT_FOUND already contained kernel32.dll.
static void reportX64BuiltinPreflight(const char* path) {
    std::shared_ptr<FsNode> node =
        Fs::getNodeFromLocalPath(B(""), BString::copy(path), true);
    if (!node) {
        klog_fmt("BOXEDWINE_X64_BUILTIN_PREFLIGHT path=%s open=missing "
                 "size=0 mz=0", path);
        return;
    }
    if (node->isDirectory()) {
        klog_fmt("BOXEDWINE_X64_BUILTIN_PREFLIGHT path=%s open=directory "
                 "size=0 mz=0", path);
        return;
    }
    FsOpenNode* probe = node->open(K_O_RDONLY);
    if (!probe) {
        klog_fmt("BOXEDWINE_X64_BUILTIN_PREFLIGHT path=%s open=denied "
                 "size=%llu mz=0", path,
                 (unsigned long long)node->length());
        return;
    }
    U8 header[2] = {0, 0};
    const U32 got = probe->readNative(header, (U32)sizeof(header));
    const S64 size = probe->length();
    probe->close();
    delete probe;
    klog_fmt("BOXEDWINE_X64_BUILTIN_PREFLIGHT path=%s open=ok size=%lld mz=%d",
             path, (long long)size,
             boxedvn::looksLikePeImage(header, got) ? 1 : 0);
}

static void reportX64WineLayoutPreflight() {
    for (const char* path : {(const char*)K_X64_WINE_MODULE_ROOT,
                             (const char*)K_X64_WINE_PE_DIR,
                             (const char*)K_X64_WINE_LOADER}) {
        std::shared_ptr<FsNode> node =
            Fs::getNodeFromLocalPath(B(""), BString::copy(path), true);
        klog_fmt("BOXEDWINE_X64_WINE_LAYOUT path=%s present=%d kind=%s "
                 "size=%llu",
                 path, node ? 1 : 0,
                 !node ? "missing" : (node->isDirectory() ? "dir" : "file"),
                 (unsigned long long)(node ? node->length() : 0));
    }
}

bool StartUpArgs::apply() {
    KSystem::init();    
#ifdef BOXEDWINE_MULTI_THREADED
    KSystem::cpuAffinityCountForApp = this->cpuAffinity;
    if (KSystem::cpuAffinityCountForApp) {
        klog_fmt("CPU Affinity set to %d", KSystem::cpuAffinityCountForApp);
    }
#endif
    KSystem::disableHideCursor = this->disableHideCursor;
    KSystem::forceRelativeMouse = this->forceRelativeMouse;
    KSystem::cacheReads = this->cacheReads;
    KSystem::disableWasmJitForWrittenCode = this->disableWasmJitForWrittenCode;
    KSystem::interpreterAnonymousExecutable =
        this->interpreterAnonymousExecutable;
    KSystem::interpreterModules = this->interpreterModules;
    KSystem::interpreterRanges = this->interpreterRanges;
    for (const auto& module : KSystem::interpreterModules) {
        klog_fmt("Compatibility CPU profile: interpret modules matching '%s'; "
                 "JIT remains enabled for all other guest code",
                 module.c_str());
    }
    for (const auto& range : KSystem::interpreterRanges) {
        klog_fmt("Compatibility CPU profile: interpret guest range "
                 "%.8X-%.8X; JIT remains enabled outside it",
                 range.first, range.second);
    }
    if (KSystem::interpreterAnonymousExecutable) {
        klog("Compatibility CPU profile: interpret anonymous executable "
             "guest memory; JIT remains enabled for mapped ELF and PE code");
    }
    KSystem::pentiumLevel = this->pentiumLevel;
    KSystem::pollRate = this->pollRate;
    if (KSystem::pollRate < 0) {
        KSystem::pollRate = 0;
    }
    KSystem::openglLib = this->openGlLib;
    KSystem::ttyPrepend = this->ttyPrepend;
    KSystem::skipFrameFPS = this->skipFrameFPS;
    if (!KSystem::logFile.isOpen() && this->logPath.length()) {
        KSystem::logFile.createNew(this->logPath);
    }

    for (U32 f=0;f<nonExecFileFullPaths.size();f++) {
        FsFileNode::nonExecFileFullPaths.insert(nonExecFileFullPaths[f]);
    }
#ifdef BOXEDWINE_RECORDER
    if (this->recordAutomation.length()) {
        Recorder::start(this->recordAutomation);
    }
#endif

    klog_fmt("Using root directory: %s", root.c_str());
#ifdef BOXEDWINE_ZLIB
    std::vector<BString> fullfilled;
    for (auto& zip : zips) {
        BString fullfills;
        FsZip::readFileFromZip(zip, B("fullfills.txt"), fullfills);
        if (fullfills.length()) {
            fullfilled.push_back(fullfills);
        }
    }
    std::vector<BString> depends;
    for (auto& zip : zips) {
        BString depend;
        FsZip::readFileFromZip(zip, B("depends.txt"), depend);
        if (depend.length() && !vectorContainsIgnoreCase(depends, depend) && !vectorContainsIgnoreCase(zips, depend) && !vectorContainsIgnoreCase(fullfilled, depend)) {
            BString originalDepend = depend;
            if (!Fs::doesNativePathExist(depend)) {
                BString parentPath = Fs::getNativeParentPath(zip);
                depend = parentPath.stringByApppendingPath(depend);
            }
            if (!Fs::doesNativePathExist(depend)) {
                klog_fmt("%s depends on %s, and %s could not be found", zip.c_str(), originalDepend.c_str(), originalDepend.c_str());
            }
            depends.push_back(depend);
        }
    }
    if (depends.size()) {
        zips.insert(zips.end(), depends.begin(), depends.end());
    }
#endif
    for (auto& zip : zips) {
        klog_fmt("Using zip file system: %s", zip.c_str());
    }
    if (!Fs::initFileSystem(root)) {
        kwarn_fmt("root %s does not exist", root.c_str());
        return false;
    }
    if (this->resolutionSet) {
        klog_fmt("Resolution set to: %dx%d", this->screenCx, this->screenCy);
    }
#ifdef BOXEDWINE_ZLIB
    std::vector<std::shared_ptr<FsZip>> openZips;
    for (auto& zip : zips) {
        U64 startTime = KSystem::getMicroCounter();
        std::shared_ptr<FsZip> fsZip = std::make_shared<FsZip>();
        fsZip->init(zip, B(""));
        openZips.push_back(fsZip);
        U64 endTime = KSystem::getMicroCounter();
        klog_fmt("Loaded %s in %d ms", zip.c_str(), (U32)(endTime - startTime) / 1000);
    }

    std::shared_ptr<FsNode> wineVersionNode = Fs::getNodeFromLocalPath(B(""), B("/wineVersion.txt"), false);
    if (wineVersionNode) {
        FsOpenNode* openNode = wineVersionNode->open(K_O_RDONLY);
        if (openNode) {
            U8 tmp[64];
            U32 len = openNode->readNative(tmp, 64);
            if (len > 2) {
                BString wineVersion;
                wineVersion.append((char*)tmp, len);
                std::vector<BString> parts;
                wineVersion.split('.', parts);
                if (parts.size() == 2) {
                    KSystem::wineMajorVersion = parts[0].toInt();
                }
                if (KSystem::wineMajorVersion == 2 || KSystem::wineMajorVersion == 1) {
                    std::shared_ptr<FsNode> freeTypeNode = Fs::getNodeFromLocalPath(B(""), B("/usr/lib/i386-linux-gnu/libfreetype.so.6"), false);
                    if (freeTypeNode) {
                        freeTypeNode->link = B("libfreetype.so.6.12.3");
                    }
                }
            }
            openNode->close();
        }
    }
#endif
    KSystem::title = title;

    buildVirtualFileSystem();

#ifdef BOXEDWINE_RECORDER
    if (this->runAutomation.length()) {
        Player::start(this->runAutomation);
    }
    BOXEDWINE_RECORDER_INIT(this->root, this->zips, this->workingDir, this->args);
#endif

    envValues.push_back(B("HOME=/home/username"));
    envValues.push_back(B("LOGNAME=username"));
    envValues.push_back(B("USERNAME=username"));
    envValues.push_back(B("USER=username"));
    envValues.push_back("PWD="+this->workingDir);
    envValues.push_back(B("DISPLAY=:0"));
    envValues.push_back(B("WINE_FAKE_WAIT_VBLANK=60"));
    addDefaultUtf8LocaleEnv(envValues, guestHasUtf8Locale());

    // Everything below that used to spell out /home/username/.wine -- the
    // dosdevices drive links, the T: mount, and the ddraw and DXVK overlays --
    // has to agree with the prefix Wine will actually open. A link written
    // into a prefix the guest never opens is invisible to it, so D: and E:
    // would simply not exist for a 64-bit session.
    const BString winePrefix = guestWinePrefixFromEnv(this->envValues);
    const bool requestedFEX64 = guestUsesFex64(this->envValues);
    const BString wineDosDevices = winePrefix + "/dosdevices";
    // Wine64 creates its prefix on first boot, so these legitimately do not
    // exist yet and an otherwise-empty .wine64 is the expected state. Only
    // what is missing is created: for the bundled 32-bit prefix everything is
    // already there and none of this does anything at all.
    //
    // wineboot --init exits 0 without producing the C: drive link in a prefix
    // it did not create itself, and the guest then reopens the missing path
    // forever -- one device run logged 468,768 failed opens of
    // dosdevices/c:. Completing the prefix is what stops that, and nothing
    // valid is replaced: an existing c: is left exactly where it points.
    // A plain directory mount that lands inside the prefix (the app mounts a
    // Files-visible folder over drive_c) has to be attached before the prefix
    // is completed and the system modules are projected into it. Attached
    // afterwards, with the other mounts below, it replaced the drive_c node
    // together with the 959 projected modules, and the guest's first load
    // ended with STATUS_DLL_NOT_FOUND. Zip mounts and drive letters stay
    // below: the letter links need dosdevices, which does not exist yet.
    for (auto&& info : this->mountInfo) {
        if (info.wine || info.nativePath.length() < 4) continue;
        const BString ext = info.nativePath.substr(info.nativePath.length() - 4).toLowerCase();
        if (ext == ".zip") continue;
        Fs::makeLocalDirs(info.localPath);
        std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(B(""), Fs::getParentPath(info.localPath), true);
        Fs::addRootDirectoryNode(info.localPath, info.nativePath, parent);
        klog_fmt("BOXEDWINE_MOUNT_DIRECTORY guest=%s stage=before-prefix", info.localPath.c_str());
    }
    const BString wineDriveC = winePrefix + "/" K_GUEST_WINE_DRIVE_C;
    const BString wineDriveCLink = wineDosDevices + "/" K_GUEST_WINE_C_LINK;
    const boxedvn::GuestWinePrefixSetup prefixSetup =
        boxedvn::planGuestWinePrefixSetup(
            Fs::getNodeFromLocalPath(B(""), wineDriveC, true) != nullptr,
            Fs::getNodeFromLocalPath(B(""), wineDosDevices, true) != nullptr,
            Fs::getNodeFromLocalPath(B(""), wineDriveCLink, false) != nullptr);
    if (prefixSetup.createDriveC) {
        Fs::makeLocalDirs(wineDriveC);
    }
    if (prefixSetup.createDosDevices) {
        Fs::makeLocalDirs(wineDosDevices);
    }
    if (prefixSetup.createDriveCLink) {
        std::shared_ptr<FsNode> dosDevicesNode =
            Fs::getNodeFromLocalPath(B(""), wineDosDevices, true);
        if (dosDevicesNode) {
            // A relative target, the way Wine writes it, so the prefix stays
            // relocatable. The guest resolves it against the link's own
            // directory.
            Fs::addFileNode(wineDriveCLink, B(K_GUEST_WINE_C_LINK_TARGET),
                            B(""), false, dosDevicesNode);
        }
    }
    if (requestedFEX64) {
        aliasX64WineLoader(K_X64_WINE_LOADER, K_X64_WINE_LOADER64_NAME);
        aliasX64WineLoader(K_X64_WINE_PRELOADER, K_X64_WINE_PRELOADER64_NAME);
        projectX64WineSystemModules(winePrefix);
        // Beside system32, and before any process starts: ntdll builds a
        // process's activation context during LdrInitializeThunk, so a
        // winsxs that appears later is a winsxs no process ever saw.
        projectX64WineSxsAssemblies(winePrefix);
        // The guest ld-linux resolves winex11.so's libX11/libXext through
        // LD_LIBRARY_PATH before the multiarch directories. Put BoxedWine's
        // own x86-64 X11 client libraries first so the driver binds to the
        // bridge rather than to a distro libX11 that opens an X socket. A
        // caller-supplied value is kept, as with every other default here.
        addDefaultEnvValue(envValues, K_X64_GUEST_LIBRARY_PATH_ASSIGNMENT);
        // Before anything below can fork: the value has to be in the
        // environment the launched process starts with, because that is the
        // environment every descendant inherits.
        addGuestGlibcTunables(envValues);
        // The 32-bit Direct3D 9 renderer, when the launch asked for DXVK's.
        // This has to run after projectX64WinePe32Modules, which is what puts
        // Wine's own d3d9 into syswow64 in the first place.
        if (guestWantsWow64Dxvk(envValues)) {
            projectX64WineDxvkD3d9(winePrefix);
            // Native, not builtin: the projected file is a real PE32 DXVK
            // image and Wine has to load it rather than fall back to its own.
            mergeWow64DxvkD3d9Override(envValues);
        }
        // Audio, after the module projections so the packaged-driver test sees
        // the tree the guest will actually search. See docs/PLAN_X64_AUDIO.md.
        reportX64AudioDeviceNodes();
        configureX64AudioDriver(winePrefix);
        // Graphics, in the same place and for the same reason: both the GL
        // witness and the renderer decision have to see the mounted guest
        // filesystem, and the renderer has to be settled before wineserver
        // reads the prefix.
        reportX64GuestOpenGl();
        configureX64WineD3dRenderer(winePrefix);
        // The relay filter, when the launch asked for the trace. Last of the
        // three prefix edits for the same reason they are all here: wineserver
        // reads user.reg once, when the first process of the session reaches
        // it, so every value has to be in the file before that happens.
        if (guestWantsWineRelayTrace(envValues)) {
            configureX64WineRelayFilter(winePrefix);
        }
    }

    if (!this->ddrawOverridePath.isEmpty()) {
        envValues.push_back(B("WINEDLLOVERRIDES=ddraw=n,b"));
        std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(BString::empty, this->ddrawOverridePath, true);
        if (!parent) {
            klog_fmt("-ddrawOverride %s not found", this->ddrawOverridePath.c_str());
        }
        const BString ddrawDir = winePrefix + "/drive_c/ddraw";
        std::shared_ptr<FsNode> ddrawParent = Fs::getNodeFromLocalPath(BString::empty, ddrawDir, true);
        if (!ddrawParent) {
            klog_fmt("-ddrawOverride was specificied but %s was not found in the file system", ddrawDir.c_str());
        }
        if (parent && ddrawParent) {
            Fs::addFileNode(this->ddrawOverridePath + "/ddraw.dll", ddrawDir + "/ddraw.dll", ddrawParent->nativePath.stringByApppendingPath("ddraw.dll"), false, parent);
            Fs::addFileNode(this->ddrawOverridePath + "/ddraw.ini", ddrawDir + "/ddraw.ini", ddrawParent->nativePath.stringByApppendingPath("ddraw.ini"), false, parent);
        }
    }
    if (this->enableDXVK) {
        envValues.push_back(B("DXVK_LOG_LEVEL=warn"));
        envValues.push_back(B("WINEDLLOVERRIDES=d3d11,d3d10core,d3d9,d3d8,dxgi=n,b"));
        std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(BString::empty, winePrefix + "/drive_c/windows/system32", true);
        std::shared_ptr<FsNode> dxvkParent = Fs::getNodeFromLocalPath(BString::empty, winePrefix + "/drive_c/dxvk", true);
        if (!dxvkParent) {
            klog("-dxvk was enabled but not found in the file system");
        } else {
            for (const char* pName : { "d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll" }) {
                const BString sourcePath = dxvkParent->path + "/" + pName;
                const BString nativeSourcePath =
                    dxvkParent->nativePath.stringByApppendingPath(pName);
                if (!Fs::doesNativePathExist(nativeSourcePath)) {
                    klog_fmt("DXVK override not bundled: %s", pName);
                    continue;
                }
                Fs::addFileNode(parent->path + "/" + pName, sourcePath,
                                nativeSourcePath, false, parent);
                klog_fmt("DXVK override mounted: %s -> %s", pName,
                         sourcePath.c_str());
            }
        }
    }

    // automatically tell wine to use ddraw.dll if its in the same directory as the app being launched
    // this is a quality of life issue so that the user doesn't have to manually add this with winecfg
    // gog's hellfire uses this
    bool foundWineDllOverride = false;

    for (auto& value : envValues) {
        if (value.startsWith("WINEDLLOVERRIDES")) {
            foundWineDllOverride = true;
        }
    }
    if (!foundWineDllOverride) {
        if (args.size() >= 2 && args[0] == "/bin/wine") {
            std::shared_ptr<FsNode> appNode = Fs::getNodeFromLocalPath(workingDir, args[1], true);
            if (appNode) {
                std::shared_ptr<FsNode> parentNode = appNode->getParent().lock();
                if (parentNode && parentNode->getChildByNameIgnoreCase(B("ddraw.dll"))) {
                    envValues.push_back(B("WINEDLLOVERRIDES=ddraw=n,b"));
                    klog("automatically applied WINEDLLOVERRIDES=ddraw=n,b");
                }
            }
        }
    }
    //envValues.push_back(B("WINEDEBUG=+wgl"));
     
    if (userId==0) {
        envValues.push_back(B("PATH=/bin:/usr/bin:/usr/local/bin:/sbin:/usr/sbin:/usr/local/sbin"));
    } else {
        envValues.push_back(B("PATH=/bin:/usr/bin:/usr/local/bin"));
    }

    for(auto&& info: this->mountInfo) {
        if (info.wine) {
            std::shared_ptr<FsNode> mntDir = Fs::getNodeFromLocalPath(B(""), B("/mnt"), true);
            std::shared_ptr<FsNode> drive_d = Fs::addRootDirectoryNode("/mnt/drive_"+info.localPath, info.nativePath, mntDir);
            std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(B(""), wineDosDevices, true);
            if (parent) {
                Fs::addFileNode(wineDosDevices + "/" + info.localPath + ":", "/mnt/drive_"+info.localPath, B(""), false, parent);
            } else {
                klog_fmt("drive %s: not linked because %s is missing",
                         info.localPath.c_str(), wineDosDevices.c_str());
            }
        } else {
            BString ext = info.nativePath.substr(info.nativePath.length()-4).toLowerCase();
            if (ext == ".zip") {
    #ifdef BOXEDWINE_ZLIB
                U64 startTime = KSystem::getMicroCounter();
                std::shared_ptr<FsZip> fsZip = std::make_shared<FsZip>();
                if (!info.localPath.endsWith("/", false)) {
                    info.localPath= info.localPath+"/";
                }
                fsZip->init(info.nativePath, info.localPath);
                openZips.push_back(fsZip);
                U64 endTime = KSystem::getMicroCounter();
                klog_fmt("Mounted %s in %d ms", info.nativePath.c_str(), (U32)(endTime - startTime) / 1000);
    #else
                klog_fmt("% not mounted because zlib was not compiled in", info.nativePath.c_str());
    #endif
            }
            // Plain directories were attached before the prefix setup above.
        }
    }

    // The DXMT module overlay resolves its sources through the drive mounts
    // attached just above (the modules are staged on the game drive), so it
    // has to run after them. A device run that projected before the mounts
    // reported every module missing and Wine fell back to wined3d.
    if (requestedFEX64 && !this->x64ModuleOverlayPath.isEmpty()) {
        overlayX64WineModules(this->x64ModuleOverlayPath, winePrefix);
    }

    if (this->args.size()==0) {
        args.push_back(B("/bin/wine"));
        args.push_back(B("explorer"));
        args.push_back(B("/desktop=shell"));
    }
    if (this->args.size()) {
        std::shared_ptr<FsNode> node = Fs::getNodeFromLocalPath(workingDir, this->args[0], true);        

        bool validLinuxCommand = false;
        if (node) {
            BString interpreter;
            BString loader;
            std::vector<BString> interpreterArgs;
            std::vector<BString> args;

            FsOpenNode* openNode=ElfLoader::inspectNode(workingDir, node, loader, interpreter, interpreterArgs);
            if (openNode) {
                openNode->close();
                validLinuxCommand = true;
            }
        }
        if (!validLinuxCommand) {
            if (Fs::doesNativePathExist(args[0])) {
                BString dir = args[0];
                dir = Fs::trimTrailingSlash(dir);
                bool isDir = Fs::isNativePathDirectory(dir);
                std::shared_ptr<FsNode> mntDir = Fs::getNodeFromLocalPath(B(""), B("/mnt"), true);

                if (!isDir) {
                    dir = Fs::getNativeParentPath(dir);
                }
                
                std::shared_ptr<FsNode> drive_d = Fs::addRootDirectoryNode(B("/mnt/drive_t"), dir, mntDir);
                std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(B(""), wineDosDevices, true);
                if (parent) {
                    Fs::addFileNode(wineDosDevices + "/t:", B("/mnt/drive_t"), B(""), false, parent);
                }

                if (!workingDirSet) {
                    workingDir = B("/mnt/drive_t");
                }
            
                if (isDir) {
                    args.clear();
                    args.push_back(B("/bin/wine"));
                    args.push_back(B("explorer"));
                    args.push_back(B("t:\\"));
                } else {
                    BString fileName = Fs::getFileNameFromNativePath(args[0]);
                    args.erase(args.begin());
                    args.insert(args.begin(), "t:\\" + fileName);
                    if (fileName.endsWith(".msi", true)) {
                        args.insert(args.begin(), B("start"));
                    }
                    args.insert(args.begin(), B("/bin/wine"));
                }
            }
        }
    }

    // One line, after the prefix, the drive links and the working directory
    // are all settled. Which prefix a launch opened and whether its
    // dosdevices survived used to be unanswerable from a device log: a
    // 64-bit session that wrote its drive links into the 32-bit prefix
    // looks exactly like one that wrote them nowhere.
    {
        const BString wineArch = guestWineArchFromEnv(this->envValues);
        const bool dosDevicesReady =
            Fs::getNodeFromLocalPath(B(""), wineDosDevices, true) != nullptr;
        const bool driveCReady =
            Fs::getNodeFromLocalPath(B(""), wineDriveC, true) != nullptr;
        const bool driveCLinkReady =
            Fs::getNodeFromLocalPath(B(""), wineDriveCLink, false) != nullptr;
        klog_fmt("BOXEDWINE_X64_PREFIX path=%s arch=%s dosdevices=%s "
                 "drive_c=%s c_link=%s cwd=%s",
                 winePrefix.c_str(),
                 wineArch.length() ? wineArch.c_str() : "(default)",
                 dosDevicesReady ? "ready" : "missing",
                 driveCReady ? "ready" : "missing",
                 driveCLinkReady ? "ready" : "missing",
                 workingDir.c_str());
    }
    if (this->sdlFullScreen!=FULLSCREEN_NOTSET && !this->resolutionSet) {
        U32 width = 0;
        U32 height = 0;
        if (KNativeSystem::getScreenDimensions(&width, &height)) {
#ifdef __ANDROID__
            this->screenCx = width / 2;
            this->screenCy = height / 2;
#else
            this->screenCx = width;
            this->screenCy = height;
#endif
        }
    }
    KSystem::videoOption = this->videoOption;
#ifdef __EMSCRIPTEN__
    if (this->audioFreq) {
        dspSetMaxOutputFreq(this->audioFreq);
    }
#endif
    KSystem::soundEnabled = this->soundEnabled;
#ifdef __EMSCRIPTEN__
    if (KSystem::soundEnabled) {
        KSystem::soundEnabled = false;
        KSystem::enableSoundAfterMouseClick = true;
    }
#endif
    KNativeSystem::initWindow(this->screenCx, this->screenCy, this->screenBpp, this->sdlScaleX, this->sdlScaleY, this->sdlScaleQuality, this->sdlFullScreen, this->vsync);
    KNativeAudio::init();
#ifdef BOXEDWINE_OPENGL
    PlatformOpenGL::init();
    gl_init(this->glExt);        
#endif   
#ifdef BOXEDWINE_VULKAN
    vulkan_init();
#endif
    x11_init();

    if (this->args.size()) {
        klog_nonewline("Launching ");
        for (U32 i=0;i<this->args.size();i++) {
            klog_nonewline_fmt("\"%s\" ", this->args[i].c_str());
        }
        klog_nonewline("\n");
        bool result = false;
        {
            KProcessPtr process = KProcess::create();// keep in this small scope so we don't hold onto it for the life of the program
            if (requestedFEX64) {
                klog_fmt("BOXEDWINE_X64_LAUNCH request=fex executable=%s env_count=%u",
                         this->args[0].c_str(), (U32)this->envValues.size());
                // All overlays are mounted and no guest process has started,
                // so this reports the device-visible runtime rather than the
                // host bundle's archive listing.
                reportX64WineLayoutPreflight();
                reportX64BuiltinPreflight(K_X64_WINE_BUILTIN_PROBE);
            }
            KThread* thread = process->startProcess(this->workingDir, this->args, this->envValues, this->userId, this->groupId, this->effectiveUserId, this->effectiveGroupId);
            result = thread != nullptr;
        }
        if (result) {
            if (!doMainLoop()) {
                return false; // doMainLoop should have handled any cleanup, like SDL_Quit if necessary
            }
        }
    }
#ifdef GENERATE_SOURCE
    if (gensrc)
        writeSource();
#endif    
	KSystem::destroy();
    KNativeSystem::shutdown();
    KNativeAudio::shutdown();
    dspShutdown();

#ifdef BOXEDWINE_ZLIB
    openZips.clear();
#endif    
    return true;
}

bool StartUpArgs::loadDefaultResource(const char* app) {
    BString cmd = Platform::getResourceFilePath(B("cmd.txt"));
    static std::vector<BString> lines;
    lines.clear();
    lines.push_back(BString::copy(app));
    if (!cmd.isEmpty() && readLinesFromFile(cmd, lines)) {
        int count = (int)lines.size();
        const char** ppArgs = new const char*[count];
        for (int i=0;i<count;i++) {
            ppArgs[i] = lines[i].c_str();
            if (lines[i] == "-zip" && i+1<count) {
                if (!Fs::doesNativePathExist(lines[i+1])) {
                    BString zip = Platform::getResourceFilePath(lines[i+1]);
                    if (!zip.isEmpty() && Fs::doesNativePathExist(zip)) {
                        i++;
                        ppArgs[i] = strdup(zip.c_str()); // I'm not worried about leaking this
                    }
                }
            }
        }
        return parseStartupArgs((int)lines.size(), ppArgs);
    }
    return true;
}

bool StartUpArgs::parseStartupArgs(int argc, const char **argv) {
    int i;
    // look for -log as soon as possible so that logging is enabled as soon as possible
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-log") && i + 1 < argc) {
            this->logPath = BString::copy(argv[i + 1]);
            KSystem::logFile.createNew(logPath);
            i++;
        }
    }
    klog_nonewline("Command line arguments:");
    for (i = 0; i < argc; i++) {
        klog_nonewline_fmt(" \"%s\"", argv[i]);
    }
    klog_nonewline("\n");
    for (i=1;i<argc;i++) {
        if (!strcmp(argv[i], "-root") && i+1<argc) {
            this->setRoot(BString::copy(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i], "-zip") && i+1<argc) {
#ifdef BOXEDWINE_ZLIB
            this->addZip(BString::copy(argv[i+1]));
#else
            kwarn("BoxedWine wasn't compiled with zlib support");
#endif
            i++;
        } else if (!strcmp(argv[i], "-title") && i + 1 < argc) {
            this->title = BString::copy(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-nozip") && i + 1 < argc) {
            this->nozip = true;
        } else if (!strcmp(argv[i], "-m") && i+1<argc) {
            // no longer used
            i++;
        } else if (!strcmp(argv[i], "-uid") && i+1<argc) {
            this->userId = atoi(argv[i+1]);
            i++;
            if (!this->euidSet)
                this->effectiveUserId = this->userId;
        } else if (!strcmp(argv[i], "-gid") && i+1<argc) {
            this->groupId = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-euid") && i+1<argc) {
            this->effectiveUserId = atoi(argv[i+1]);
            i++;
            this->euidSet = true;
        } else if (!strcmp(argv[i], "-egid") && i+1<argc) {
            this->effectiveGroupId = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-w") && i+1<argc) {
            this->setWorkingDir(BString::copy(argv[i+1]));
            i++;
            this->workingDirSet = true;
        } else if (!strcmp(argv[i], "-nosound")) {
			this->soundEnabled = false;
#ifdef __EMSCRIPTEN__
        } else if (!strcmp(argv[i], "-audioFreq") && i+1<argc) {
            U32 audioFreq = atoi(argv[i+1]);
            if (audioFreq == 11025 || audioFreq == 22050) {
                this->audioFreq = audioFreq;
            } else {
                klog("-audioFreq must be 11025 or 22050");
            }
            i++;
#endif
        } else if (!strcmp(argv[i], "-novideo")) {
#ifdef BOXEDWINE_MSVC
            this->videoOption = VIDEO_HIDE_WINDOW;
#else
            this->videoOption = VIDEO_NO_WINDOW;
#endif
        } else if (!strcmp(argv[i], "-env")) {
			this->envValues.push_back(BString::copy(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i], "-fullscreen")) {
			this->setFullscreen(FULLSCREEN_STRETCH);
        } else if (!strcmp(argv[i], "-fullscreenAspect")) {
            this->setFullscreen(FULLSCREEN_ASPECT);
        } else if (!strcmp(argv[i], "-vsync")) {
            this->vsync = atoi(argv[i + 1]);
            i++;
            if (this->vsync < 0 || this->vsync > 2) {
                klog("-vsync must be 0, 1 or 2 (0=disabled, 1=enabled, 2=adaptive)");
                this->vsync = VSYNC_DEFAULT;
            }
        } else if (!strcmp(argv[i], "-scale")) {
			this->setScale(atoi(argv[i+1]));
            if (!this->resolutionSet) {
                this->screenCx = 640;
                this->screenCy = 480;
            }
            i++;
        } else if (!strcmp(argv[i], "-scale_quality")) {
			this->setScaleQuality(BString::copy(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i], "-resolution")) {
            setResolution(BString::copy(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i], "-bpp")) {
            setBpp(atoi(argv[i+1]));
            i++;
            if (this->screenBpp!=16 && this->screenBpp!=32 && this->screenBpp!=8) {
                klog("-bpp must be 8, 16 or 32");
                this->screenBpp = 32;
            }
        } else if (!strcmp(argv[i], "-noexecfiles")) {
            B(argv[i + 1]).split(':', this->nonExecFileFullPaths);        
            i++;
        } else if (!strcmp(argv[i], "-p2")) {
            this->pentiumLevel = 2;
        } else if (!strcmp(argv[i], "-p3")) {
            this->pentiumLevel = 3;
        } else if (!strcmp(argv[i], "-glext")) {
            this->setAllowedGlExtension(BString::copy(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i], "-rel_mouse_sensitivity")) {
            this->rel_mouse_sensitivity = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-mount_drive")) {
            if (strlen(argv[i+2])!=1) {
                klog("-mount_drive expects 2 parameters: <host directory to mount> <drive letter to use for wine>");
                klog("  example: -mount_drive \"c:\\my games\" d");
            } else {
                this->mountInfo.push_back(MountInfo(BString::copy(argv[i+2]), BString::copy(argv[i+1]), true));
            }
            i+=2;
        } else if (!strcmp(argv[i], "-mount")) {
            if (argv[i+2][0]!='/') {
                klog("-mount expects 2 parameters: <host directory to mount or zip file> <full path on root file>\n");
                klog("example: -mount \"c:\\my games\" \"/home/username/my games\"");
                klog("example: -mount_zip \"c:\\my games\\mygame.zip\" /mnt/game");
            } else {
                if (Fs::doesNativePathExist(BString::copy(argv[i + 2]))) {
                    klog_fmt("mount directory/file does not exist: %s", argv[i + 2]);
                    return false;
                }
                this->mountInfo.push_back(MountInfo(BString::copy(argv[i+2]), BString::copy(argv[i+1]), false));
            }
            i+=2;
        } else if (!strcmp(argv[i], "-showStartupWindow")) {
            // no longer used
        } else if (!strcmp(argv[i], "-ui")) {
            i++;
            if (!strcmp(argv[i], "opengl")) {
                this->uiType = UI_TYPE_OPENGL;
            }
        } else if (!strcmp(argv[i], "-dpiAware")) {
            dpiAware = true;
        } else if (!strcmp(argv[i], "-pollRate")) {
            this->pollRate = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-opengl")) {
            this->openGlLib = argv[i+1];
            i++;
        }
        else if (!strcmp(argv[i], "-ttyPrepend")) { // used to send tty back to WaitDlg for winetricks when BOXEDWINE_UI_LAUNCH_IN_PROCESS is not defined
            this->ttyPrepend = true;
        } else if (!strcmp(argv[i], "-cpuAffinity")) {
#ifdef BOXEDWINE_MULTI_THREADED
            this->cpuAffinity = atoi(argv[i+1]);
#else
            klog("ignoring -cpuAffinity");
#endif
            i++;
        } else if (!strcmp(argv[i], "-skipFrameFPS") && i+1<argc) {
            this->skipFrameFPS = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i], "-log") && i + 1 < argc) {
            this->logPath = BString::copy(argv[i + 1]);
            i++;
        } 
#ifdef BOXEDWINE_RECORDER
        else if (!strcmp(argv[i], "-record")) {
            if (!Fs::doesNativePathExist(BString::copy(argv[i+1]))) {
                static_cast<void>(MKDIR(argv[i+1])); // return result ignored
                if (!Fs::doesNativePathExist(BString::copy(argv[i+1]))) {
                    klog_fmt("-record path does not exist and could not be created: %s", argv[i+1]);
                    return false;
                }
            }
            this->recordAutomation = BString::copy(argv[i + 1]);
            i++;
        }  else if (!strcmp(argv[i], "-automation")) {
            if (!Fs::doesNativePathExist(BString::copy(argv[i+1]))) {
                klog_fmt("-automation directory does not exist %s", argv[i+1]);
                return false;
            }
            this->runAutomation = BString::copy(argv[i + 1]);
            i++;
        }  else if (!strcmp(argv[i], "-play")) {
            this->runAutomation = BString::copy(argv[i + 1]);
            i++;
        }
#endif
        else if (!strcmp(argv[i], "-ddrawOverride")) {
            this->ddrawOverridePath = argv[i + 1];
            i++;
        } else if (!strcmp(argv[i], "-disableHideCursor")) {
            this->disableHideCursor = true;
        } else if (!strcmp(argv[i], "-forceRelativeMouse")) {
            this->forceRelativeMouse = true;
        }  else if (!strcmp(argv[i], "-cacheReads")) {
            this->cacheReads = true;
        }  else if (!strcmp(argv[i], "-disableWasmJitForWrittenCode")) {
            this->disableWasmJitForWrittenCode = true;
        } else if (!strcmp(argv[i], "-interpreterAnonymousExecutable")) {
            this->interpreterAnonymousExecutable = true;
        } else if (!strcmp(argv[i], "-interpreterModule") && i + 1 < argc) {
            this->interpreterModules.push_back(BString::copy(argv[i + 1]));
            i++;
        } else if (!strcmp(argv[i], "-interpreterRange") && i + 1 < argc) {
            U32 start = 0;
            U32 end = 0;
            if (sscanf(argv[i + 1], "%x-%x", &start, &end) != 2 ||
                start >= end) {
                klog_fmt("Invalid -interpreterRange '%s'; expected "
                         "START-END with START below END", argv[i + 1]);
                return false;
            }
            this->interpreterRanges.emplace_back(start, end);
            i++;
        }
        else if (!strcmp(argv[i], "-x64modules")) {
            this->x64ModuleOverlayPath = argv[i + 1];
            i++;
        }
        else if (!strcmp(argv[i], "-dxvk")) {
            BString dxvk;
            dxvk = argv[i + 1];
            this->enableDXVK = dxvk.startsWith('t', true) || (dxvk == "1") || dxvk.startsWith('y', true);
            i++;
        } else {
            break;
        }
    } 
    char curdir[1024];
    char* base = getcwd(curdir, sizeof(curdir));
    char pathSeperator = '/';

    if (base!=nullptr && strchr(base, '\\') != nullptr) {
        pathSeperator = '\\';
    }
    if (KNativeSystem::getAppDirectory().length()) {
        BString base2 = KNativeSystem::getAppDirectory();
        base2 = base2.substr(0, base2.length()-1); 
        if (zips.size()==0 && !nozip) {
            std::vector<Platform::ListNodeResult> results;
            if (base) {
                Platform::listNodes(BString::copy(base), results);
                for (auto&& item : results) {
                    if (strstr(item.name.c_str(), "Wine") && strstr(item.name.c_str(), ".zip")) {
                        this->zips.push_back(base + pathSeperator + item.name);
                        break;
                    }
                }
            }
            if (zips.size()==0) {
                results.clear();
                Platform::listNodes(base2, results);
                for (auto&& item : results) {
                    if (strstr(item.name.c_str(), "Wine") && strstr(item.name.c_str(), ".zip")) {
                        this->zips.push_back(BString(base2) + pathSeperator + item.name);
                    }
                }
            }
        }
    }
    if (!this->root.length()) {
#ifdef __ANDROID__
        this->root=SDL_AndroidGetExternalStoragePath();
        this->root+="/root";
#else
        this->root=KNativeSystem::getLocalDirectory()+"root";
#endif
    }  

    argc = argc-i;
    argv = &argv[i];
        
    for (i=0;i<argc;i++) {
        args.push_back(BString::copy(argv[i]));
    }
    return true;
}

void StartUpArgs::setResolution(BString resolution) {
    U32 width;
    U32 height;

    int success = parse_resolution(resolution.c_str(), &width, &height);
    if (success) {
        this->screenCx = width;
        this->screenCy = height;
        this->resolutionSet = true;                
    }
}
