/*
 * BoxedVN fex64 - minimal embedded Wine bootstrap for iPhoneOS.
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#import <Foundation/Foundation.h>
#import <os/log.h>

#include "BVNWineBridge.h"
#include "BVNFexBridge.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#if BVN_WINE_BOOT_ENABLED

#include <cerrno>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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
std::string g_report;

void reportf(const char* format, ...) {
    char line[2048];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    {
        std::lock_guard<std::mutex> guard(g_reportMutex);
        if (!g_report.empty()) g_report.push_back('\n');
        g_report += line;
    }
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT,
                     "[BoxedVN Wine] %{public}s", line);
}

#if BVN_WINE_BOOT_ENABLED

std::atomic<bool> g_starting {false};
std::atomic<bool> g_serverRunning {false};
std::string g_prefix;

NSString* bundled(NSString* name) {
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:name];
}

bool isDirectory(NSString* path) {
    BOOL directory = NO;
    return [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] &&
           directory;
}

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

    // Prefix initialization is native ARM64. Recreate every symlink because
    // the app bundle path changes when a sideloader reinstalls the IPA.
    NSString* runtime = bundled(@"aarch64-windows");
    NSArray<NSString*>* names = [files contentsOfDirectoryAtPath:runtime error:&error];
    if (!names) {
        reportf("could not read the native Wine runtime: %s",
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
    reportf("prefix ready with %lu native runtime links", (unsigned long)linked);
    g_stage.store(BVNWineStagePrefixReady, std::memory_order_release);
    return linked != 0;
}

void* serverThread(void*) {
    @autoreleasepool {
        setenv("WINEPREFIX", g_prefix.c_str(), 1);
        setenv("HOME", g_prefix.c_str(), 1);

        NSString* nls = bundled(@"nls");
        NSString* log = [NSHomeDirectory() stringByAppendingPathComponent:
                          @"Documents/fex64-wine.log"];
        wineserver_set_nls_dir(nls.fileSystemRepresentation);
        wineserver_log_set_file(log.fileSystemRepresentation);
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
        setenv("WINEDEBUG", "err+all", 1);

        const int64_t writeOffset = BVNFexWriteOffset();
        if (writeOffset != 0) {
            char value[32];
            snprintf(value, sizeof(value), "0x%llx", (unsigned long long)writeOffset);
            setenv("MYTHIC_JIT_WRITE_OFFSET", value, 1);
            reportf("published the runtime JIT alias offset");
        }

        NSString* log = [NSHomeDirectory() stringByAppendingPathComponent:
                          @"Documents/fex64-wine.log"];
        wine_log_set_file(log.fileSystemRepresentation);

        BVNWinePrepareExitTrap();
        g_stage.store(BVNWineStageProcessStarted, std::memory_order_release);
        reportf("entering Wine through native ntdll");

        char loader[] = "wine";
        char target[] = "C:\\windows\\system32\\wineboot.exe";
        char initialize[] = "--init";
        char* arguments[] = {loader, target, initialize, nullptr};

        if (setjmp(*BVNWineExitJumpBuffer()) == 0) {
            __wine_main(3, arguments);
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

extern "C" bool BVNWineStart(void) {
#if BVN_WINE_BOOT_ENABLED
    bool expected = false;
    if (!g_starting.compare_exchange_strong(expected, true)) {
        reportf("Wine bootstrap is already running");
        return false;
    }
    if (!BVNWineAvailable()) {
        reportf("the linked build is missing one or more Wine resources");
        g_stage.store(BVNWineStageFailed, std::memory_order_release);
        g_starting.store(false);
        return false;
    }
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
