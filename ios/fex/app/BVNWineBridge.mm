/*
 * BoxedVN fex64 - minimal embedded Wine bootstrap for iPhoneOS.
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#import <Foundation/Foundation.h>
#import <os/log.h>

#include "BVNWineBridge.h"
#include "BVNFexBridge.h"
#include "BVNExecMemory.h"
#include "BVNJitArenaPlan.h"
#include "IOSDisplayShim.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#if BVN_WINE_BOOT_ENABLED

#include <cerrno>
#include <csetjmp>
#include <cstdlib>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

extern "C" {
int wineserver_main(int argc, char* argv[]);
void wineserver_inject_client_fd(int fd);
void wineserver_set_nls_dir(const char* path);
void wineserver_log_set_file(const char* path);
void wine_log_set_file(const char* path);
void __wine_main(int argc, char* argv[]);

extern int foreground;

// Implemented in C so the TLS ABI exactly matches Wine's C translation units;
// Objective-C++ accesses it only through these non-TLS functions.
jmp_buf* BVNWineExitJumpBuffer(void);
void BVNWinePrepareExitTrap(void);
int BVNWineExitCode(void);
void BVNWineClearExitTrap(void);

// Read by the patched wineserver event loop.
volatile int g_wineserver_should_stop = 0;
}

#endif

namespace {

std::atomic<BVNWineStage> g_stage {
#if BVN_WINE_BOOT_ENABLED
    BVNWineStageIdle
#else
    BVNWineStageUnavailable
#endif
};
std::mutex g_reportMutex;
std::mutex g_snapshotMutex;
std::string g_report;
std::string g_logPath;
std::atomic<BVNWineTarget> g_target {BVNWineTargetX64};

NSString* diagnosticLogPath() {
    NSString* documents = [NSFileManager.defaultManager
        URLsForDirectory:NSDocumentDirectory
               inDomains:NSUserDomainMask].firstObject.path;
    return [documents stringByAppendingPathComponent:@"fex64-wine.log"];
}

void appendDiagnosticLine(const std::string& path, const char* line) {
    if (path.empty()) return;
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor == -1) return;
    const size_t length = strlen(line);
    (void)write(descriptor, line, length);
    (void)write(descriptor, "\n", 1);
    close(descriptor);
}

void reportf(const char* format, ...) {
    char line[2048];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    std::string logPath;
    {
        std::lock_guard<std::mutex> guard(g_reportMutex);
        if (!g_report.empty()) g_report.push_back('\n');
        g_report += line;
        logPath = g_logPath;
    }
    appendDiagnosticLine(logPath, line);
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT,
                     "[BoxedVN Wine] %{public}s", line);
}

#if BVN_WINE_BOOT_ENABLED

std::atomic<bool> g_starting {false};
std::atomic<bool> g_serverRunning {false};
std::string g_prefix;
void* g_winePoolExecutable = nullptr;
void* g_winePoolWritable = nullptr;
size_t g_winePoolBytes = 0;

bool prepareWineJitPool() {
    if (g_winePoolExecutable != nullptr) return true;
    if (BVNFexStageReached() != BVNFexStageExecuted) {
        reportf("the FEX probe must complete before Wine can lease an executable pool");
        return false;
    }

    // The arena is prepared in independent segments. FEX owns the first one;
    // Wine leases the next so its PE-image copier and FEX's allocator can
    // never advance into each other's executable pages. This has to match the
    // arena segment size exactly: a lease is first fit within one segment and
    // never spans two, so asking for more than a segment fails no matter how
    // much of the arena is free.
    constexpr size_t poolBytes = kBVNJitArenaSegmentBytes;
    void* executable = BVNExecMemAlloc(poolBytes);
    if (executable == nullptr) {
        size_t capacity = 0;
        size_t available = 0;
        size_t segments = 0;
        BVNExecMemArenaStatus(&capacity, &available, &segments);
        reportf("could not lease Wine's executable pool (%zu MiB free of %zu MiB across %zu segments)",
                available / (1024 * 1024), capacity / (1024 * 1024), segments);
        return false;
    }
    void* writable = BVNExecMemWritableAddress(executable, poolBytes);
    if (writable == nullptr) {
        BVNExecMemFree(executable, poolBytes);
        reportf("Wine's executable pool has no writable alias");
        return false;
    }

    g_winePoolExecutable = executable;
    g_winePoolWritable = writable;
    g_winePoolBytes = poolBytes;

    char rx[32];
    char rw[32];
    char size[32];
    snprintf(rx, sizeof(rx), "0x%llx",
             (unsigned long long)(uintptr_t)g_winePoolExecutable);
    snprintf(rw, sizeof(rw), "0x%llx",
             (unsigned long long)(uintptr_t)g_winePoolWritable);
    snprintf(size, sizeof(size), "0x%llx",
             (unsigned long long)g_winePoolBytes);
    setenv("WINE_IOS_JIT_RX", rx, 1);
    setenv("WINE_IOS_JIT_RW", rw, 1);
    setenv("WINE_IOS_JIT_SIZE", size, 1);

    reportf("Wine executable pool %zu MiB at rx=%p rw=%p (rw-rx=%+lld)",
            g_winePoolBytes / (1024 * 1024), g_winePoolExecutable,
            g_winePoolWritable,
            (long long)((intptr_t)g_winePoolWritable -
                        (intptr_t)g_winePoolExecutable));
    return true;
}

bool prepareDiagnostics() {
    NSString* path = diagnosticLogPath();
    {
        std::lock_guard<std::mutex> guard(g_reportMutex);
        g_report.clear();
        g_logPath = path.fileSystemRepresentation;
    }

    // O_APPEND is load-bearing, not tidiness. dup2 below makes stderr share
    // this open file description, including its file OFFSET, which sits just
    // past the separator. reportf appends through a separate descriptor, so
    // without O_APPEND every reportf line is silently overwritten by the next
    // Wine stderr write, which resumes from the stale offset. Logs 10 and 11
    // begin with the separator followed immediately by Wine output: every
    // app-side line, including the layer report, had been written and then
    // clobbered. O_APPEND makes each write land at end-of-file instead.
    const int descriptor = open(path.fileSystemRepresentation,
                                O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (descriptor == -1) {
        os_log_error(OS_LOG_DEFAULT,
                     "[BoxedVN Wine] could not open persistent log: %{public}s",
                     strerror(errno));
        return false;
    }

    static const char separator[] =
        "\n===== BoxedVN fex64 Wine session =====\n";
    (void)write(descriptor, separator, sizeof(separator) - 1);

    // Wine's iOS bridge already emits its lowest-level breadcrumbs to stderr.
    // Redirect before either native thread starts so a fatal signal still
    // leaves evidence even when Swift and os_log never get another turn.
    const bool redirected = dup2(descriptor, STDERR_FILENO) != -1 &&
                            dup2(descriptor, STDOUT_FILENO) != -1;
    close(descriptor);
    return redirected;
}

NSString* bundled(NSString* name) {
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:name];
}

bool isDirectory(NSString* path) {
    BOOL directory = NO;
    return [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] &&
           directory;
}

// Defined below, beside the rest of the user-supplied-executable handling;
// preparePrefix calls it so a start always republishes the links.
int scanInstalled();

bool preparePrefix() {
    NSFileManager* files = NSFileManager.defaultManager;
    NSString* documents = [files URLsForDirectory:NSDocumentDirectory
                                         inDomains:NSUserDomainMask].firstObject.path;
    NSString* prefix = [documents stringByAppendingPathComponent:@"fex64-wine"];
    NSString* prefixTemplate = bundled(@"prefix-template");
    NSString* marker = [prefix stringByAppendingPathComponent:@".update-timestamp"];
    NSError* error = nil;

    if (![files fileExistsAtPath:marker]) {
        if (![files fileExistsAtPath:prefixTemplate]) {
            reportf("prefix template is missing from the app bundle");
            return false;
        }

        NSString* staging = [documents stringByAppendingPathComponent:@"fex64-wine-staging"];
        [files removeItemAtPath:staging error:nil];
        if (![files copyItemAtPath:prefixTemplate toPath:staging error:&error]) {
            reportf("could not stage the Wine prefix: %s",
                    error.localizedDescription.UTF8String);
            return false;
        }

        // Never discard a partial prefix automatically: it may already hold
        // user-installed files even if the bootstrap marker is absent.
        if ([files fileExistsAtPath:prefix]) {
            [files removeItemAtPath:staging error:nil];
            reportf("an incomplete Wine prefix already exists; preserve or remove it before retrying");
            return false;
        }
        if (![files moveItemAtPath:staging toPath:prefix error:&error]) {
            reportf("could not install the Wine prefix: %s",
                    error.localizedDescription.UTF8String);
            return false;
        }
        reportf("installed the bundled Wine prefix template");
    }

    NSString* system32 = [prefix stringByAppendingPathComponent:
                           @"drive_c/windows/system32"];
    if (![files createDirectoryAtPath:system32
          withIntermediateDirectories:YES attributes:nil error:&error]) {
        reportf("could not create system32: %s", error.localizedDescription.UTF8String);
        return false;
    }

    // Translated and graphics probes use ARM64EC so xtajit64 can hand x64
    // code to FEX. The native probe keeps the aarch64 runtime available as a
    // control without requiring a different IPA.
    //
    // The desktop probe runs in the aarch64 world. Wine builds programs for
    // the native architecture only, so explorer exists there and nowhere
    // else, and the app build stages an aarch64 wineios.drv beside it because
    // the integration ships that driver for ARM64EC alone. This world cannot
    // run x86, which the translation target covers separately.
    // Recreate every symlink because the app bundle path changes when a
    // sideloader reinstalls the IPA.
    const BVNWineTarget target = g_target.load(std::memory_order_acquire);
    const bool nativeTarget = target == BVNWineTargetNative ||
                              target == BVNWineTargetDesktop;
    NSString* runtimeName = nativeTarget ? @"aarch64-windows" : @"arm64ec-windows";
    NSString* runtime = bundled(runtimeName);
    NSArray<NSString*>* names = [files contentsOfDirectoryAtPath:runtime error:&error];
    if (!names) {
        reportf("could not read the %s Wine runtime: %s",
                runtimeName.UTF8String,
                error.localizedDescription.UTF8String);
        return false;
    }
    NSUInteger linked = 0;
    for (NSString* name in names) {
        NSString* source = [runtime stringByAppendingPathComponent:name];
        NSString* destination = [system32 stringByAppendingPathComponent:name];
        [files removeItemAtPath:destination error:nil];
        if ([files createSymbolicLinkAtPath:destination
                        withDestinationPath:source error:nil]) {
            linked++;
        }
    }

    NSString* dosdevices = [prefix stringByAppendingPathComponent:@"dosdevices"];
    [files createDirectoryAtPath:dosdevices
      withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* driveC = [dosdevices stringByAppendingPathComponent:@"c:"];
    [files removeItemAtPath:driveC error:nil];
    [files createSymbolicLinkAtPath:driveC
                withDestinationPath:@"../drive_c" error:nil];

    g_prefix = prefix.fileSystemRepresentation;
    reportf("prefix ready with %lu %s runtime links", (unsigned long)linked,
            runtimeName.UTF8String);
    // Republished here as well as on demand: the container path changes when
    // a sideloader reinstalls the IPA, and a start is the last moment the
    // links can be corrected before something tries to open through them.
    scanInstalled();
    g_stage.store(BVNWineStagePrefixReady, std::memory_order_release);
    return linked != 0;
}

// --- user-supplied executables ---------------------------------------------
//
// Everything else this app can start ships inside the IPA. This is the path
// for a program the user copied in themselves, which means two problems the
// bundled targets never have: where it lives, and where it runs from.
//
// Where it lives. The drop directory is Documents/games, which iOS shows in
// Files because the Info.plist asks for it, and which sits outside the Wine
// prefix. Outside matters: the prefix is disposable and is reinstalled from
// the bundled template whenever that template changes, and nothing the user
// spent an hour copying over a cable should be inside something disposable.
// Each top-level entry is then published as a symbolic link under
// drive_c/Games, so Wine addresses it as C:\Games\... - an ordinary drive C
// path, resolved the same way as everything else. That is deliberate rather
// than a second DOS drive: the integration's ntdll notes that
// unix_to_nt_file_name cannot resolve drives through dosdevices on iOS, so a
// D: mapping would look right and fail at the first file open. Links inside
// drive_c are already the mechanism the runtime DLLs use.
//
// Where it runs from. A game finds its data relative to the working
// directory, so the process has to start in the executable's own folder, and
// on both sides: chdir for the unix half, and MYTHIC_INITIAL_CWD for the
// Windows half, which env_ios.c reads because the usual conversion goes
// through the same dosdevices path that does not work here.

std::mutex g_installedMutex;
std::vector<std::string> g_installedWindows;   // "Games\\Some Game\\game.exe"
std::vector<std::string> g_installedUnixDirs;  // absolute, the exe's own folder
std::vector<std::string> g_installedUnixPaths; // absolute path to the PE image
std::atomic<int> g_installedIndex {0};
std::string g_installRoot;

NSString* documentsDirectory() {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                inDomains:NSUserDomainMask]
        .firstObject.path;
}

// Redistributable payloads sit beside a game and are full of installers -
// DirectX, the VC runtimes, .NET. Every one of them is an .exe, and listing
// them would bury the two or three that are actually the program. Publishers
// mark these directories by convention; the leading underscore is the most
// common spelling and the named ones cover the rest.
bool isRedistributableDirectory(NSString* name) {
    if ([name hasPrefix:@"_"]) return true;
    static NSArray<NSString*>* const known = @[
        @"redist", @"redists", @"commonredist", @"directx", @"vcredist",
        @"dotnet", @"drivers", @"support"
    ];
    for (NSString* candidate in known) {
        if ([name caseInsensitiveCompare:candidate] == NSOrderedSame) return true;
    }
    return false;
}

void collectExecutables(NSString* root, NSString* relative, int depth,
                        std::vector<std::string>& windows,
                        std::vector<std::string>& unixDirs,
                        std::vector<std::string>& unixPaths) {
    // Three levels below the drop directory reaches <game>/<subdir>/<subdir>,
    // which is past where a launcher has ever been found. Deeper is engine
    // data, and walking it on the main thread is what the user would feel.
    if (depth > 3) return;

    NSFileManager* files = NSFileManager.defaultManager;
    NSString* here = relative.length
        ? [root stringByAppendingPathComponent:relative] : root;
    NSArray<NSString*>* entries = [files contentsOfDirectoryAtPath:here error:nil];

    for (NSString* name in [entries sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
        if ([name hasPrefix:@"."]) continue;
        NSString* childRelative = relative.length
            ? [relative stringByAppendingPathComponent:name] : name;
        NSString* child = [root stringByAppendingPathComponent:childRelative];

        // Follows links on purpose: every entry directly below the root is one.
        BOOL directory = NO;
        if (![files fileExistsAtPath:child isDirectory:&directory]) continue;

        if (directory) {
            if (isRedistributableDirectory(name)) continue;
            collectExecutables(root, childRelative, depth + 1, windows, unixDirs,
                               unixPaths);
            continue;
        }
        if ([name.pathExtension caseInsensitiveCompare:@"exe"] != NSOrderedSame) continue;

        std::string windowsPath = std::string("Games\\") + childRelative.UTF8String;
        for (char& character : windowsPath) {
            if (character == '/') character = '\\';
        }
        windows.push_back(std::move(windowsPath));
        unixDirs.push_back(child.stringByDeletingLastPathComponent.fileSystemRepresentation);
        unixPaths.push_back(child.fileSystemRepresentation);
    }
}

int scanInstalled() {
    NSFileManager* files = NSFileManager.defaultManager;
    NSString* documents = documentsDirectory();
    NSString* drop = [documents stringByAppendingPathComponent:@"games"];
    NSString* prefix = [documents stringByAppendingPathComponent:@"fex64-wine"];
    NSString* marker = [prefix stringByAppendingPathComponent:@".update-timestamp"];

    // The drop directory is created unconditionally so it appears in Files
    // before Wine has ever run - that is where the user has to put something
    // for any of this to have an answer.
    [files createDirectoryAtPath:drop
     withIntermediateDirectories:YES attributes:nil error:nil];

    // Publishing is the part that touches the prefix, and it waits for the
    // prefix to exist. Creating drive_c early would be worse than useless:
    // preparePrefix refuses to install over a directory it did not create,
    // reading it as a half-finished prefix that might hold user files, so a
    // scan before the first start would wedge the first start permanently.
    NSUInteger published = 0;
    if ([files fileExistsAtPath:marker]) {
        NSString* publish = [prefix stringByAppendingPathComponent:@"drive_c/Games"];
        [files createDirectoryAtPath:publish
         withIntermediateDirectories:YES attributes:nil error:nil];

        // Relinked every time. A sideloader reinstalling the IPA moves the
        // container, which leaves every absolute link pointing at a path that
        // no longer exists - the same reason the runtime links are recreated
        // on every prefix preparation.
        for (NSString* stale in [files contentsOfDirectoryAtPath:publish error:nil]) {
            [files removeItemAtPath:[publish stringByAppendingPathComponent:stale]
                              error:nil];
        }
        for (NSString* name in [files contentsOfDirectoryAtPath:drop error:nil]) {
            if ([name hasPrefix:@"."]) continue;
            if ([files createSymbolicLinkAtPath:[publish stringByAppendingPathComponent:name]
                            withDestinationPath:[drop stringByAppendingPathComponent:name]
                                          error:nil]) {
                published++;
            }
        }
    }

    // Listed from the drop directory rather than through the links, so the
    // interface can show what is there before the prefix exists. Each
    // top-level entry is published under the same name, so the path below is
    // the path Wine will see either way.
    std::vector<std::string> windows;
    std::vector<std::string> unixDirs;
    std::vector<std::string> unixPaths;
    collectExecutables(drop, @"", 0, windows, unixDirs, unixPaths);

    int count;
    {
        std::lock_guard<std::mutex> guard(g_installedMutex);
        g_installRoot = drop.fileSystemRepresentation;
        g_installedWindows = std::move(windows);
        g_installedUnixDirs = std::move(unixDirs);
        g_installedUnixPaths = std::move(unixPaths);
        count = (int)g_installedWindows.size();
        if (g_installedIndex.load(std::memory_order_relaxed) >= count) {
            g_installedIndex.store(0, std::memory_order_relaxed);
        }
    }
    reportf("Documents/games: %d executable(s) found, %lu folder(s) published to C:\\Games%s",
            count, (unsigned long)published,
            [files fileExistsAtPath:marker] ? "" : " (deferred: no prefix yet)");
    return count;
}

bool selectedInstalled(std::string& windowsPath, std::string& unixDir) {
    std::lock_guard<std::mutex> guard(g_installedMutex);
    const int index = g_installedIndex.load(std::memory_order_relaxed);
    if (index < 0 || index >= (int)g_installedWindows.size()) return false;
    windowsPath = "C:\\" + g_installedWindows[(size_t)index];
    unixDir = g_installedUnixDirs[(size_t)index];
    return true;
}

bool selectedInstalledUnixPath(std::string& unixPath) {
    std::lock_guard<std::mutex> guard(g_installedMutex);
    const int index = g_installedIndex.load(std::memory_order_relaxed);
    if (index < 0 || index >= (int)g_installedUnixPaths.size()) return false;
    unixPath = g_installedUnixPaths[(size_t)index];
    return true;
}

uint16_t peMachine(const std::string& path) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor == -1) return 0;

    uint8_t dosHeader[64] {};
    if (pread(descriptor, dosHeader, sizeof(dosHeader), 0) != sizeof(dosHeader) ||
        dosHeader[0] != 'M' || dosHeader[1] != 'Z') {
        close(descriptor);
        return 0;
    }
    const uint32_t peOffset = (uint32_t)dosHeader[0x3c] |
                              ((uint32_t)dosHeader[0x3d] << 8) |
                              ((uint32_t)dosHeader[0x3e] << 16) |
                              ((uint32_t)dosHeader[0x3f] << 24);
    uint8_t signatureAndMachine[6] {};
    const bool valid = peOffset <= 64 * 1024 * 1024 &&
        pread(descriptor, signatureAndMachine, sizeof(signatureAndMachine), peOffset) ==
            sizeof(signatureAndMachine) &&
        signatureAndMachine[0] == 'P' && signatureAndMachine[1] == 'E' &&
        signatureAndMachine[2] == 0 && signatureAndMachine[3] == 0;
    close(descriptor);
    if (!valid) return 0;
    return (uint16_t)signatureAndMachine[4] |
           ((uint16_t)signatureAndMachine[5] << 8);
}

void* serverThread(void*) {
    @autoreleasepool {
        setenv("WINEPREFIX", g_prefix.c_str(), 1);
        setenv("HOME", g_prefix.c_str(), 1);

        NSString* nls = bundled(@"nls");
        wineserver_set_nls_dir(nls.fileSystemRepresentation);
        wineserver_log_set_file(g_logPath.c_str());
        foreground = 1;
        g_serverRunning.store(true, std::memory_order_release);
        reportf("entering the embedded wineserver");

        char serverName[] = "wineserver";
        char foregroundArgument[] = "--foreground";
        char* arguments[] = {serverName, foregroundArgument, nullptr};
        const int result = wineserver_main(2, arguments);
        g_serverRunning.store(false, std::memory_order_release);
        reportf("wineserver returned %d", result);
    }
    return nullptr;
}

void* processThread(void*) {
    @autoreleasepool {
        setenv("WINEPREFIX", g_prefix.c_str(), 1);
        setenv("HOME", g_prefix.c_str(), 1);
        setenv("WINELOADERNOEXEC", "1", 1);
        setenv("WINEDLLPATH", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);
        // warn+module on purpose: the unixlib resolver in ntdll's virtual_ios.c
        // announces which table each builtin got through WARN_(module), and
        // with err+all alone every one of those lines is invisible. A device
        // log then cannot distinguish "win32u's unix side is active" from
        // "it is linked but dormant" from "the resolver never ran" - which is
        // exactly the ambiguity that cost a build. Thirteen modules load, so
        // the added volume is a dozen lines.
        setenv("WINEDEBUG", "err+all,warn+module", 1);
        setenv("MYTHIC_TEST_VAR", "steam-s1", 1);

        // Activate win32u's unix side.
        //
        // scripts/build-fex64-win32u.sh builds libwin32u_unix.a and the app
        // link pulls it in, but linking it is not enough: load_builtin_unixlib
        // in ntdll's virtual_ios.c calls win32u_unix_lib_init() only when this
        // variable is set, and takes a "linked but dormant" branch otherwise.
        // Dormant means slot 1 of KeServiceDescriptorTable is never populated,
        // so every NtUser/NtGdi entry point the guest reaches through
        // user32 dispatches into an unregistered table.
        //
        // That is what the last device run shows. The guest's message loop
        // calls PeekMessage, is told a message is waiting, and gets a MSG that
        // is entirely zero; it then calls DispatchMessage, user32 reaches its
        // window-procedure call site (user32+0x60528, the __os_arm64x_check_icall
        // sequence) and dispatches to x64 through the exit thunk at
        // user32+0x949b4 with a target of 0x700880f7d0 - the address of the
        // guest's own MSG local. FEX finds that page non-executable and raises,
        // which is the "Unhandled page fault on execute access" the run ends on.
        //
        // The integration's own bridge sets this (app/Mythic/WineProcessBridge.m).
        setenv("MYTHIC_WIN32U", "1", 1);

        const int64_t writeOffset =
            static_cast<int64_t>(static_cast<uint8_t*>(g_winePoolWritable) -
                                 static_cast<uint8_t*>(g_winePoolExecutable));
        if (writeOffset != 0) {
            char value[32];
            snprintf(value, sizeof(value), "0x%llx", (unsigned long long)writeOffset);
            setenv("MYTHIC_JIT_WRITE_OFFSET", value, 1);
            reportf("published the runtime JIT alias offset");
        }

        wine_log_set_file(g_logPath.c_str());

        BVNWinePrepareExitTrap();
        g_stage.store(BVNWineStageProcessStarted, std::memory_order_release);
        reportf("entering Wine through native ntdll");

        char loader[] = "wine";
        char nativeTarget[] = "C:\\windows\\system32\\child-test.exe";
        char x64Target[] = "C:\\windows\\system32\\fib-x64.exe";
        char dxmtTarget[] = "C:\\windows\\system32\\cube-x64.exe";
        /* The desktop target is a native ARM64 PE, so it exercises Wine's own
         * window, graphics and server stack without going through translation
         * at all -- the part of the road to a visible desktop that the x64
         * acceptance programs never touch. explorer needs the /desktop switch
         * or it attaches to a shell that does not exist here. */
        char desktopTarget[] = "C:\\windows\\system32\\explorer.exe";
        char desktopArgument[] = "/desktop=shell,1024x768";
        char* target = x64Target;
        char* extraArgument = nullptr;
        // Outlives the argument vector: __wine_main is handed the pointer.
        std::string installedPath;
        std::string installedDir;
        switch (g_target.load(std::memory_order_acquire)) {
            case BVNWineTargetNative: target = nativeTarget; break;
            case BVNWineTargetX64: target = x64Target; break;
            case BVNWineTargetDXMT: target = dxmtTarget; break;
            case BVNWineTargetDesktop:
                target = desktopTarget;
                extraArgument = desktopArgument;
                break;
            case BVNWineTargetInstalled:
                if (!selectedInstalled(installedPath, installedDir)) {
                    reportf("no executable is selected. Copy a game folder into "
                            "%s using Files, then rescan.",
                            BVNWineInstallRoot());
                    g_stage.store(BVNWineStageFailed, std::memory_order_release);
                    g_starting.store(false, std::memory_order_release);
                    return nullptr;
                }
                target = const_cast<char*>(installedPath.c_str());
                break;
        }
        reportf("selected acceptance target: %s",
                BVNWineTargetName(g_target.load(std::memory_order_relaxed)));

        // A game reads its data relative to the working directory, so the
        // process has to start in the executable's own folder. Both halves
        // need telling: chdir for the unix side, MYTHIC_INITIAL_CWD for the
        // Windows side, because the integration's get_initial_directory
        // cannot derive one from the other on iOS.
        if (!installedDir.empty()) {
            const std::string::size_type separator = installedPath.rfind('\\');
            const std::string windowsDir = installedPath.substr(0, separator + 1);

            if (chdir(installedDir.c_str()) != 0) {
                reportf("could not enter %s (errno %d); the program will start "
                        "in the prefix root and probably not find its data",
                        installedDir.c_str(), errno);
            } else {
                setenv("PWD", installedDir.c_str(), 1);
                setenv("MYTHIC_INITIAL_CWD", windowsDir.c_str(), 1);
                // Storefront shims shipped beside a game treat this as the
                // asset root and ask for it repeatedly during start-up. It
                // costs nothing when no such shim is present.
                setenv("SteamAppPath", windowsDir.substr(0, windowsDir.size() - 1).c_str(), 1);
                reportf("running %s from %s", target, windowsDir.c_str());
            }
        }
        char* arguments[] = {loader, target, extraArgument, nullptr};
        const int argumentCount = extraArgument ? 3 : 2;

        if (setjmp(*BVNWineExitJumpBuffer()) == 0) {
            __wine_main(argumentCount, arguments);
            reportf("Wine returned normally");
        } else {
            reportf("Wine exited with code %d", BVNWineExitCode());
        }
        BVNWineClearExitTrap();
        g_stage.store(BVNWineStageExited, std::memory_order_release);
        g_starting.store(false, std::memory_order_release);
    }
    return nullptr;
}

#endif

} // namespace

#if BVN_WINE_BOOT_ENABLED

// The embedded server cannot terminate the iOS process on a fatal error.
extern "C" void fatal_error(const char* format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    reportf("wineserver fatal error: %s", message);
    g_stage.store(BVNWineStageFailed, std::memory_order_release);
    pthread_exit(nullptr);
}

#endif

extern "C" bool BVNWineAvailable(void) {
#if BVN_WINE_BOOT_ENABLED
    return isDirectory(bundled(@"aarch64-windows")) &&
           isDirectory(bundled(@"arm64ec-windows")) &&
           isDirectory(bundled(@"nls")) &&
           isDirectory(bundled(@"prefix-template"));
#else
    return false;
#endif
}

extern "C" bool BVNWineSetTarget(BVNWineTarget target) {
    if (target < BVNWineTargetNative || target > BVNWineTargetInstalled) return false;
#if BVN_WINE_BOOT_ENABLED
    if (g_starting.load(std::memory_order_acquire)) return false;
#endif
    g_target.store(target, std::memory_order_release);
    return true;
}

extern "C" BVNWineTarget BVNWineSelectedTarget(void) {
    return g_target.load(std::memory_order_acquire);
}

extern "C" const char* BVNWineTargetName(BVNWineTarget target) {
    switch (target) {
        case BVNWineTargetNative: return "native control";
        case BVNWineTargetX64: return "x64 translation";
        case BVNWineTargetDXMT: return "x64 graphics";
        case BVNWineTargetDesktop: return "Wine desktop";
        case BVNWineTargetInstalled: return "installed program";
    }
    return "unknown";
}

extern "C" int BVNWineScanInstalled(void) {
#if BVN_WINE_BOOT_ENABLED
    @autoreleasepool {
        return scanInstalled();
    }
#else
    return 0;
#endif
}

extern "C" int BVNWineInstalledCount(void) {
#if BVN_WINE_BOOT_ENABLED
    std::lock_guard<std::mutex> guard(g_installedMutex);
    return (int)g_installedWindows.size();
#else
    return 0;
#endif
}

extern "C" const char* BVNWineInstalledName(int index) {
#if BVN_WINE_BOOT_ENABLED
    std::lock_guard<std::mutex> guard(g_installedMutex);
    if (index < 0 || index >= (int)g_installedWindows.size()) return nullptr;
    // Below the publishing directory, so the "Games\" the Windows path needs
    // is noise on screen.
    return g_installedWindows[(size_t)index].c_str() + strlen("Games\\");
#else
    (void)index;
    return nullptr;
#endif
}

extern "C" bool BVNWineSelectInstalled(int index) {
#if BVN_WINE_BOOT_ENABLED
    if (g_starting.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> guard(g_installedMutex);
    if (index < 0 || index >= (int)g_installedWindows.size()) return false;
    g_installedIndex.store(index, std::memory_order_relaxed);
    return true;
#else
    (void)index;
    return false;
#endif
}

extern "C" int BVNWineSelectedInstalled(void) {
#if BVN_WINE_BOOT_ENABLED
    return g_installedIndex.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" const char* BVNWineInstallRoot(void) {
#if BVN_WINE_BOOT_ENABLED
    {
        std::lock_guard<std::mutex> guard(g_installedMutex);
        if (!g_installRoot.empty()) return g_installRoot.c_str();
    }
    @autoreleasepool {
        NSString* drop = [documentsDirectory() stringByAppendingPathComponent:@"games"];
        std::lock_guard<std::mutex> guard(g_installedMutex);
        g_installRoot = drop.fileSystemRepresentation;
        return g_installRoot.c_str();
    }
#else
    return "";
#endif
}

extern "C" bool BVNWineStart(void) {
#if BVN_WINE_BOOT_ENABLED
    bool expected = false;
    if (!g_starting.compare_exchange_strong(expected, true)) {
        reportf("Wine bootstrap is already running");
        return false;
    }
    if (!prepareDiagnostics()) {
        reportf("persistent logging could not redirect stdout and stderr");
    } else {
        reportf("persistent log ready at Documents/fex64-wine.log");
    }

    // The shim registers its layer when SwiftUI builds the view, which is
    // before this log exists and before prepareDiagnostics truncates it, so
    // the shim's own stderr line can never appear here and its absence
    // proves nothing. Report the latched state instead: a run that would
    // have produced no picture says so before the graphics stack loads.
    reportf("display layer registered before start: %s",
            bvn_display_has_layer() ? "yes" : "no - swapchains will fail");
    if (!BVNWineAvailable()) {
        reportf("the linked build is missing one or more Wine resources");
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    // Checked before anything is leased or detached, because the debugger
    // detach below is one-way: a run that stops after it cannot be retried
    // without relaunching the app, and "you did not pick a program" is not
    // worth spending that on.
    if (g_target.load(std::memory_order_acquire) == BVNWineTargetInstalled) {
        std::string windowsPath;
        std::string unixDir;
        if (!selectedInstalled(windowsPath, unixDir)) {
            reportf("no executable is selected. Copy a folder into %s with the "
                    "Files app, then rescan.", BVNWineInstallRoot());
            g_stage.store(BVNWineStageFailed, std::memory_order_release);
            g_starting.store(false);
            return false;
        }
        std::string unixPath;
        if (selectedInstalledUnixPath(unixPath)) {
            const uint16_t machine = peMachine(unixPath);
            if (machine == 0x014c) {
                reportf("the selected executable is 32-bit x86 (PE machine 0x014c); "
                        "this build contains only the x86-64 translator. Select an "
                        "x86-64 executable.");
                g_stage.store(BVNWineStageFailed, std::memory_order_release);
                g_starting.store(false);
                return false;
            }
        }
    }
    if (!prepareWineJitPool()) {
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    reportf("detaching the debugger after executable pool preparation");
    if (!BVNExecMemDetachDebugger()) {
        reportf("could not complete the guarded debugger detach request");
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    setenv("MYTHIC_DETACHED", "1", 1);
    reportf("debugger detach request completed");
    g_stage.store(BVNWineStageResourcesReady, std::memory_order_release);
    reportf("Wine runtime resources found");

    if (!preparePrefix()) {
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }

    pthread_t server;
    const int serverResult = pthread_create(&server, nullptr, serverThread, nullptr);
    if (serverResult != 0) {
        reportf("could not create the wineserver thread: %s", strerror(serverResult));
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    pthread_detach(server);

    // The running flag is set immediately before wineserver_main. A bounded
    // wait avoids racing socket injection with its global initialization.
    for (unsigned int attempt = 0; attempt != 200 && !g_serverRunning.load(); ++attempt) {
        usleep(10 * 1000);
    }
    if (!g_serverRunning.load()) {
        reportf("the wineserver thread did not enter its main function");
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    g_stage.store(BVNWineStageServerStarted, std::memory_order_release);

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        reportf("socketpair failed: %s", strerror(errno));
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    char socketValue[24];
    snprintf(socketValue, sizeof(socketValue), "%d", sockets[1]);
    setenv("WINESERVERSOCKET", socketValue, 1);
    wineserver_inject_client_fd(sockets[0]);
    reportf("connected native ntdll to the embedded wineserver");

    pthread_t process;
    const int processResult = pthread_create(&process, nullptr, processThread, nullptr);
    if (processResult != 0) {
        close(sockets[0]);
        close(sockets[1]);
        reportf("could not create the Wine process thread: %s", strerror(processResult));
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
    pthread_detach(process);
    return true;
#else
    reportf("this build does not contain the Wine runtime");
    return false;
#endif
}

extern "C" BVNWineStage BVNWineStageReached(void) {
    return g_stage.load(std::memory_order_acquire);
}

extern "C" const char* BVNWineStageName(BVNWineStage stage) {
    switch (stage) {
        case BVNWineStageUnavailable: return "not included in this build";
        case BVNWineStageIdle: return "ready";
        case BVNWineStageResourcesReady: return "runtime resources found";
        case BVNWineStagePrefixReady: return "prefix ready";
        case BVNWineStageServerStarted: return "wineserver started";
        case BVNWineStageProcessStarted: return "Wine entered";
        case BVNWineStageExited: return "Wine exited";
        case BVNWineStageFailed: return "failed";
    }
    return "unknown";
}

extern "C" const char* BVNWineReport(void) {
    static std::string snapshot;
    std::lock_guard<std::mutex> guard(g_reportMutex);
    snapshot = g_report;
    return snapshot.c_str();
}

extern "C" const char* BVNWineLogPath(void) {
    static std::string snapshot;
    std::lock_guard<std::mutex> guard(g_snapshotMutex);
    snapshot = diagnosticLogPath().fileSystemRepresentation;
    return snapshot.c_str();
}

extern "C" const char* BVNWinePersistentLog(void) {
    static std::string snapshot;
    constexpr NSUInteger maximumBytes = 256 * 1024;
    NSData* data = [NSData dataWithContentsOfFile:diagnosticLogPath()];
    if (data.length > maximumBytes) {
        data = [data subdataWithRange:NSMakeRange(data.length - maximumBytes,
                                                  maximumBytes)];
    }
    NSString* contents = data.length == 0
        ? @""
        : [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!contents && data.length != 0) {
        contents = [[NSString alloc] initWithData:data
                                         encoding:NSISOLatin1StringEncoding];
    }

    std::lock_guard<std::mutex> guard(g_snapshotMutex);
    snapshot = contents ? contents.UTF8String : "";
    return snapshot.c_str();
}
