/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

import Combine
import Foundation

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var games: [Game] = []
    @Published private(set) var containers: [WineContainer] = []
    // .probeStatus(), never .probeExecuteUnsafe(): this runs at app launch,
    // before the user has done anything, and the unsafe probe can crash the
    // process with no recovery possible. See Runtime.swift / BVNRuntime.h.
    @Published private(set) var jit: JITReport = .probeStatus()
    @Published private(set) var memory: MemoryReport = .probe()
    @Published private(set) var fexBackend: FEXBackendReport = .status()
    @Published private(set) var isProbingFEX = false
    @Published private(set) var lowAddressProbe: LowAddressProbeReport = .notRun
    @Published private(set) var isProbingLowAddresses = false
    @Published private(set) var runtimeState: RuntimeState = .idle
    @Published private(set) var isImporting = false
    @Published var importProgressMessage = ""
    @Published private(set) var isInstallingGame = false
    @Published var installerProgressMessage = ""
    @Published private(set) var isInstallingRootFilesystem = false
    @Published var alertMessage: String?

    private var pollTimer: Timer?
    private var pollCount = 0
    private var pendingGameInstallation: PendingGameInstallation?

    init() {
        reloadGames()
        reloadContainers()
        startPolling()
        // After the banner, so a session that starts with logging off still
        // records which build turned it off.
        defer { Log.applyStoredPreference() }
        Log.write("BoxedVN \(AppVersion.display) initialised; Boxedwine core "
                  + Session.boxedwineVersion, category: "app")
        Log.write("Memory status: \(memory.statusText); "
                  + "\(memory.availableText) available before process limit. "
                  + memory.detail, category: "memory")
        if Storage.bundledRootFilesystem != nil {
            let ignoredImport = Storage.importedRootFilesystem == nil
                ? ""
                : " An older imported archive is present but intentionally ignored."
            Log.write("Bundled root filesystem selected as the authoritative "
                      + "runtime for this build." + ignoredImport,
                      category: "rootfs")
        }
    }

    deinit {
        pollTimer?.invalidate()
    }

    // MARK: - Library

    func reloadGames() {
        games = GameLibrary.load()
    }

    func reloadContainers() {
        containers = ContainerLibrary.load()
    }

    func createContainer(named name: String) {
        do {
            _ = try ContainerLibrary.create(name: name)
            reloadContainers()
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    func updateContainer(_ container: WineContainer) {
        do {
            try ContainerLibrary.save(container)
            reloadContainers()
        } catch {
            alertMessage = "Could not save \(container.name): \(error.localizedDescription)"
        }
    }

    func deleteContainer(_ container: WineContainer) {
        do {
            try ContainerLibrary.delete(container)
            reloadContainers()
            reloadGames()
        } catch {
            alertMessage = "Could not delete \(container.name): \(error.localizedDescription)"
        }
    }

    func addShortcut(_ program: ContainerProgram, from container: WineContainer,
                     title: String) {
        do {
            _ = try GameLibrary.createContainerShortcut(
                title: title, contentDirectory: program.root,
                executable: program.executable,
                winePrefix: container.prefixName,
                renderer: container.renderer,
                width: container.width, height: container.height)
            reloadGames()
        } catch {
            alertMessage = "Could not add the shortcut: \(error.localizedDescription)"
        }
    }

    func importGame(from url: URL, title: String) {
        guard !isImporting else { return }
        isImporting = true
        importProgressMessage = "Reading \(url.lastPathComponent)…"

        // Security-scoped access is required for anything the Files picker
        // hands back from outside the app container.
        let needsScope = url.startAccessingSecurityScopedResource()

        Task.detached(priority: .userInitiated) {
            defer { if needsScope { url.stopAccessingSecurityScopedResource() } }
            do {
                let game = try GameLibrary.importGame(from: url, title: title)
                await MainActor.run {
                    self.isImporting = false
                    self.importProgressMessage = ""
                    self.reloadGames()
                    if !game.hasRunnableExecutable {
                        self.alertMessage = Self.noRunnableExecutableMessage(for: game)
                    }
                }
            } catch {
                await MainActor.run {
                    self.isImporting = false
                    self.importProgressMessage = ""
                    self.alertMessage = error.localizedDescription
                    Log.write(error.localizedDescription, category: "import",
                              level: BVNLogLevelError)
                }
            }
        }
    }

    private static func noRunnableExecutableMessage(for game: Game) -> String {
        let discovered = Executables.discover(in: game.contentDirectory)
        if discovered.isEmpty {
            return "No .exe files were found in \(game.title). The import "
                 + "succeeded, but there is nothing to run."
        }
        let reasons = discovered.prefix(3)
            .map { "• \($0.relativePath): \($0.diagnostic)" }
            .joined(separator: "\n")
        return "None of the executables in \(game.title) can be run:\n\n\(reasons)"
    }

    func deleteGame(_ game: Game) {
        do {
            try GameLibrary.delete(game)
            reloadGames()
        } catch {
            alertMessage = "Could not delete \(game.title): \(error.localizedDescription)"
        }
    }

    func installGame(from url: URL, title: String) {
        guard !isInstallingGame else { return }
        guard runtimeState != .starting && runtimeState != .running &&
              runtimeState != .stopping else {
            alertMessage = "Quit the running session before starting an installer."
            return
        }
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed. Import Boxedwine's "
                         + "root filesystem archive in Settings first."
            return
        }

        isInstallingGame = true
        installerProgressMessage = "Preparing \(url.lastPathComponent)..."
        let needsScope = url.startAccessingSecurityScopedResource()

        Task.detached(priority: .userInitiated) {
            defer { if needsScope { url.stopAccessingSecurityScopedResource() } }
            do {
                let installation = try GameLibrary.prepareInstaller(
                    from: url, title: title)
                await MainActor.run {
                    self.pendingGameInstallation = installation
                    self.installerProgressMessage = "Installer running..."
                    do {
                        try Session.launch(
                            rootFilesystem: rootFilesystem,
                            writableRoot: installation.winePrefixRoot,
                            gameDirectory: installation.stagingDirectory,
                            sharedDirectory: Storage.sharedFiles,
                            executablePath: installation.guestInstallerPath,
                            arguments: [],
                            environment: [],
                            workingDirectory:
                                "/home/username/.wine/dosdevices/d:/",
                            width: 1280,
                            height: 720,
                            soundEnabled: Preferences.soundEnabled,
                            runThroughWine: true)
                    } catch {
                        self.pendingGameInstallation = nil
                        self.isInstallingGame = false
                        self.installerProgressMessage = ""
                        GameLibrary.discardInstaller(installation)
                        self.alertMessage = error.localizedDescription
                    }
                }
            } catch {
                await MainActor.run {
                    self.isInstallingGame = false
                    self.installerProgressMessage = ""
                    self.alertMessage = error.localizedDescription
                    Log.write(error.localizedDescription, category: "installer",
                              level: BVNLogLevelError)
                }
            }
        }
    }

    // MARK: - Root filesystem

    var rootFilesystem: URL? { Storage.activeRootFilesystem }

    var rootFilesystemDescription: String {
        guard let url = rootFilesystem else {
            return "Not installed. BoxedVN needs Boxedwine's Linux/Wine root "
                 + "filesystem archive before it can run anything."
        }
        let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size]
            as? UInt64) ?? nil
        let sizeText = size.map {
            ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file)
        } ?? "unknown size"
        let source = Storage.bundledRootFilesystem == nil
            ? "Imported runtime"
            : "Bundled runtime"
        return "\(source) · \(url.lastPathComponent) (\(sizeText))"
    }

    /// - Parameter movingSource: true when `url` is already inside the app's
    ///   own sandbox (the Documents folder), where the file can simply be
    ///   moved instead of copied - instant on the same volume, and it avoids
    ///   leaving a second ~150 MB copy behind. Never true for a URL that came
    ///   from the document picker: those live outside the sandbox and must be
    ///   copied, not moved.
    func importRootFilesystem(from url: URL, movingSource: Bool = false) {
        guard !isInstallingRootFilesystem else { return }
        guard let destination = Storage.rootFilesystems else {
            alertMessage = "Could not create the root filesystem directory."
            return
        }

        isInstallingRootFilesystem = true
        // Security-scoped access is required for anything the Files picker
        // hands back from outside the app container; the picker's own
        // allowedContentTypes is a selectability hint only (some real ZIPs,
        // especially ones that arrived via AirDrop, don't resolve cleanly to
        // public.zip-archive and would otherwise be greyed out and
        // unselectable), so the actual file is verified as a real ZIP here
        // before anything is copied.
        let needsScope = url.startAccessingSecurityScopedResource()

        Task.detached(priority: .userInitiated) {
            defer { if needsScope { url.stopAccessingSecurityScopedResource() } }

            var listing = BVNZipListing()
            BVNZipInspect(url.path, &listing)
            guard listing.ok else {
                let reason = cString(&listing.error, Int(BVN_MAX_DIAGNOSTIC))
                await MainActor.run {
                    self.isInstallingRootFilesystem = false
                    self.alertMessage = "'\(url.lastPathComponent)' is not a "
                                       + "valid ZIP archive: \(reason)"
                }
                return
            }

            let target = destination.appendingPathComponent("boxedwine.zip")
            do {
                if FileManager.default.fileExists(atPath: target.path) {
                    try FileManager.default.removeItem(at: target)
                }
                if movingSource {
                    try FileManager.default.moveItem(at: url, to: target)
                } else {
                    try FileManager.default.copyItem(at: url, to: target)
                }
                Log.write("Root filesystem installed from \(url.lastPathComponent)"
                          + (movingSource ? " (moved from Documents)" : ""),
                          category: "rootfs")
                await MainActor.run {
                    self.isInstallingRootFilesystem = false
                    self.objectWillChange.send()
                }
            } catch {
                await MainActor.run {
                    self.isInstallingRootFilesystem = false
                    self.alertMessage = "Could not install the root filesystem: "
                                       + error.localizedDescription
                }
            }
        }
    }

    // MARK: - Launching

    func launch(_ game: Game) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed. Import "
                         + "Boxedwine's root filesystem archive in Settings first."
            return
        }
        guard let prefixes = Storage.winePrefixes else {
            alertMessage = "Could not create the Wine prefix directory."
            return
        }
        guard game.hasRunnableExecutable else {
            alertMessage = "No runnable executable is selected for \(game.title)."
            return
        }

        // The game's OWN prefix, not a shared one. A Windows game writes its
        // saves inside the prefix - drive_c/users/username/... or beside the
        // executable on the emulated C: drive - so the prefix a game launches
        // into IS its save file. Build 73 moved every game onto one shared
        // prefix to make Notepad and the file browser see each other's files,
        // and the immediate consequence was that every existing save vanished:
        // still on disk, in the per-game prefix, but not in the prefix the
        // game was now booting from.
        let writableRoot = prefixes.appendingPathComponent(game.winePrefix)
        let container = containers.first { $0.prefixName == game.winePrefix }
        // A shortcut copies its container defaults when it is created, after
        // which its launch settings are intentionally per-game. Do not let a
        // later container edit silently override an explicit game renderer or
        // resolution. "Automatic" is the one value that may inherit the live
        // container preference.
        let selectedRenderer = game.renderer == "automatic"
            ? (container?.renderer ?? game.renderer)
            : game.renderer
        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: game.contentDirectory,
                sharedDirectory: Storage.sharedFiles,
                executablePath: game.guestExecutablePath,
                arguments: GameLibrary.arguments(for: game),
                environment: GameLibrary.environment(for: game),
                workingDirectory: game.guestWorkingDirectory,
                width: game.requestedWidth,
                height: game.requestedHeight,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                wineRenderer: Self.wineRenderer(for: selectedRenderer),
                sharedDriveLetter:
                    container?.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container?.windowsVersion,
                compatibilityDirectory: game.compatibilityDirectory
            )
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    /// Opens a persistent Wine shell. D: is the container's Files directory
    /// (visible through the iOS Files app), C: is its private Wine prefix, and
    /// the chosen shared letter points at Documents/Shared.
    func launchDesktop(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let writableRoot = ContainerLibrary.prefixRoot(for: container) else {
            alertMessage = "Could not create the container prefix."
            return
        }
        let files = ContainerLibrary.filesDirectory(for: container)
        do {
            try FileManager.default.createDirectory(
                at: files, withIntermediateDirectories: true)
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "explorer",
                arguments: ["/desktop=shell,\(container.width)x\(container.height)",
                            "winefile", "D:\\"],
                environment: [],
                workingDirectory: "/home/username/.wine/dosdevices/d:/",
                width: container.width,
                height: container.height,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                wineRenderer: Self.wineRenderer(for: container.renderer),
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion)
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    /// Installs an official Wine Mono MSI into this exact container prefix.
    /// Launching an MSI from Winefile depends on shell file associations that
    /// are incomplete in the bundled shell, so invoke Wine's installer host
    /// directly and keep the selected package on the container's D: drive.
    func installWineMono(from source: URL, in container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let writableRoot = ContainerLibrary.prefixRoot(for: container) else {
            alertMessage = "Could not create the container prefix."
            return
        }
        let lowerName = source.lastPathComponent.lowercased()
        guard source.pathExtension.lowercased() == "msi",
              lowerName.hasPrefix("wine-mono") else {
            alertMessage = "Select an official wine-mono-*-x86.msi package."
            return
        }

        let accessed = source.startAccessingSecurityScopedResource()
        defer {
            if accessed { source.stopAccessingSecurityScopedResource() }
        }
        let files = ContainerLibrary.filesDirectory(for: container)
        let target = files.appendingPathComponent("wine-mono.msi")
        do {
            try FileManager.default.createDirectory(
                at: files, withIntermediateDirectories: true)
            if source.standardizedFileURL != target.standardizedFileURL {
                if FileManager.default.fileExists(atPath: target.path) {
                    try FileManager.default.removeItem(at: target)
                }
                try FileManager.default.copyItem(at: source, to: target)
            }
            Log.write("Launching Wine Mono installer in \(container.name): "
                      + target.lastPathComponent, category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "msiexec",
                arguments: ["/i", "d:\\wine-mono.msi"],
                environment: ["WINEDEBUG=warn+msi"],
                workingDirectory: "/home/username/.wine/dosdevices/d:/",
                width: container.width,
                height: container.height,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                wineRenderer: Self.wineRenderer(for: container.renderer),
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion)
        } catch {
            alertMessage = "Wine Mono could not be installed: "
                         + error.localizedDescription
        }
    }

    /// Wine's bundled DXDiag is the first renderer smoke test: it exercises
    /// DirectDraw/Direct3D discovery inside the selected prefix and reports
    /// the adapter, driver and feature levels without importing a game.
    func launchDirect3DTest(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let writableRoot = ContainerLibrary.prefixRoot(for: container) else {
            alertMessage = "Could not create the container prefix."
            return
        }
        let files = ContainerLibrary.filesDirectory(for: container)
        do {
            // Keep diagnostics mounted to the container's D: drive, but scan
            // an empty directory for compatibility. Otherwise one unrelated
            // NW.js game anywhere under Files makes dxdiag inherit Chromium
            // switches and anonymous-executable interpretation.
            let compatibilityDirectory = files.appendingPathComponent(
                ".boxedvn-diagnostics", isDirectory: true)
            try FileManager.default.createDirectory(
                at: compatibilityDirectory,
                withIntermediateDirectories: true)
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "dxdiag",
                arguments: ["/dontskip"],
                environment: ["WINEDEBUG=warn+d3d_shader,-d3d"],
                workingDirectory: nil,
                width: container.width,
                height: container.height,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                wineRenderer: Self.wineRenderer(for: container.renderer),
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: compatibilityDirectory)
        } catch {
            alertMessage = "Direct3D diagnostics could not start: "
                         + error.localizedDescription
        }
    }

    /// Runs the app-bundled IA-32 D3D9 cube through the same persistent Wine
    /// prefix and Vulkan-backed WineD3D path used by classic 3D software.
    /// Runs the bundled IA-32 Direct3D 9 probe through Wine64's WoW64 layer
    /// on the translator lane: the same glibc, Wine64 and DXMT layers as the
    /// 64-bit cube plus the 32-bit PE layer the user places in the container
    /// folder. This is the phase 2 device probe: until the CPU backend hands
    /// 32-bit code to a 32-bit translator context, the run is expected to
    /// stop at the first mode switch, and the log of that stop is the input
    /// for the next step.
    func launchGraphicsProbe(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let source = Bundle.main.url(
            forResource: "boxedvn-d3d9-cube", withExtension: "exe") else {
            alertMessage = "This build does not include the 32-bit graphics probe."
            return
        }
        guard let runtime = prepareX64Runtime(for: container) else { return }
        // The cube is a 32-bit PE, so it takes the WoW64 lane and needs a
        // complete 32-bit layer, not merely a present one.
        guard allowWoW64Launch(with: runtime) else { return }
        let target = runtime.diagnostics.appendingPathComponent(source.lastPathComponent)
        do {
            if FileManager.default.fileExists(atPath: target.path) {
                try FileManager.default.removeItem(at: target)
            }
            try FileManager.default.copyItem(at: source, to: target)
            Log.write("Launching bundled IA-32 Direct3D 9 graphics probe through "
                      + "Wine64 WoW64, BoxedWine FEX and DXMT", category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                rootFilesystemOverlays: runtime.overlays,
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                executablePath:
                    "d:\\.boxedvn-x64-diagnostics\\boxedvn-d3d9-cube.exe",
                arguments: [],
                environment: X64Runtime.withVerboseTrace(
                    X64Runtime.wow64Environment,
                    enabled: Preferences.verboseWineTrace),
                workingDirectory: runtime.guestWorkingDirectory,
                width: container.width,
                height: container.height,
                soundEnabled: false,
                runThroughWine: true,
                useFEX64: true,
                useDXMT: true,
                winePrefixDriveC: runtime.driveC,
                wineRenderer: BVNWineRendererAutomatic,
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: runtime.diagnostics)
        } catch {
            alertMessage = "The 32-bit graphics probe could not start: "
                         + error.localizedDescription
        }
    }

    /// The bundled resources every x86-64 (FEX + DXMT) launch needs, and the
    /// per-container locations they are staged into. The DXMT PE modules are
    /// copied into a hidden directory on the container's D: drive, which the
    /// runtime projects over Wine's module root (`-x64modules`); the 64-bit
    /// prefix is kept apart from the 32-bit one because the two Wine builds
    /// cannot share a prefix.
    private struct X64Runtime {
        let glibc: URL
        let wine64: URL
        let dxmt: URL
        let writableRoot: URL
        let files: URL
        /// The 64-bit prefix's C: drive, kept beside the container's Files
        /// folder so it shows in the Files app under the container.
        let driveC: URL
        let diagnostics: URL
        /// The 32-bit PE layer (`wine64-pe32.zip`) when the user has placed
        /// it in the container folder or in Documents. It is not bundled: the
        /// IPA stays small for everyone who runs only 64-bit programs, and it
        /// mounts as a third read-only layer when present.
        let pe32: URL?
        var overlays: [URL] { [glibc, wine64] + (pe32.map { [$0] } ?? []) }
        static let pe32ArchiveName = "wine64-pe32.zip"

        /// Where inside the archive the 32-bit builtins live. The build
        /// script archives the staged tree from its root, so the entries are
        /// `usr/lib/x86_64-linux-gnu/wine/i386-windows/<module>` - the guest
        /// path K_X64_WINE_PE32_DIR names, with no leading slash.
        static let pe32ModuleDirectory = "/i386-windows/"

        /// The 32-bit builtins the WoW64 lane's own import chain reaches
        /// before a Windows program runs an instruction of its own. Kept in
        /// step with K_X64_WOW64_LANE_PE32_MODULE_NAMES in
        /// include/guest_wine64_layout.h, which is where the list came from:
        /// every name was observed being resolved by a device run.
        ///
        /// The archive carries no version stamp of its own - the build writes
        /// its manifest beside the archive, not inside it - so this list *is*
        /// the version check. A copy made before a packaging fix is exactly a
        /// copy that does not carry all fifteen, and the device log of one
        /// said so: `BOXEDWINE_X64_PE32_GAP tree=i386-windows required=14
        /// missing=1`, followed by a process that exited 0xC0000135 with no
        /// window and no message.
        ///
        /// libgcc_s_dw2-1.dll is the fifteenth and the newest. It is a mingw
        /// runtime DLL that Wine's mingw-built i386 modules import and that
        /// neither Wine tree builds; an archive predating that fix loses
        /// whichever builtin imports it - Direct3D disappears while the
        /// program keeps running - so the miss never reaches a status code
        /// and this list is the only thing that can name it.
        static let pe32RequiredModules = [
            "ntdll.dll", "kernel32.dll", "kernelbase.dll", "advapi32.dll",
            "sechost.dll", "msvcrt.dll", "ucrtbase.dll", "gdi32.dll",
            "user32.dll", "win32u.dll", "opengl32.dll", "wined3d.dll",
            "d3d9.dll", "zlib1.dll", "libgcc_s_dw2-1.dll",
        ]

        /// The guest path D: resolves to for `diagnostics`. BoxedWine -w takes
        /// a guest Linux directory, not a Windows path: the Windows form left
        /// the process with no valid current directory at all (the device log
        /// showed open(".") -> -2).
        var guestWorkingDirectory: String {
            "/mnt/drive_d/.boxedvn-x64-diagnostics"
        }

        static let dxmtModules = ["d3d11.dll", "dxgi.dll", "d3d10core.dll",
                                  "winemetal.dll"]
        static let environment = [
            // winedevice and the mount manager trace why the 64-bit
            // desktop's drive links never appear; both are quiet at boot.
            //
            // msgbox is the one channel that carries text this platform
            // cannot see any other way. A program that refuses to start
            // paints its reason into a window, and Wine rasterises that text
            // in-process and hands the X server pixels, so the server has no
            // string to log; the caption reaches it as WM_NAME and is
            // recorded (include/guest_dialog_trace.h), but the body does not.
            // Wine's own user32 message box writes the body to this channel
            // and to nothing else. It is one line per message box, so the
            // volume is the number of times a program stops to talk.
            //
            // Whether any of this arrives is a separate question and an open
            // one: three 64-bit captures contain no Wine debug output at all,
            // not even the err and fixme classes that are on by default,
            // while 32-bit captures of the same build are full of them.
            // sys_execve64 now prints WINEDEBUG beside WINELOADER for every
            // exec in the chain, so the next capture says whether the value
            // below reached the process that was supposed to honour it.
            "WINEDEBUG=warn+module,warn+seh,+winedevice,+mountmgr,+msgbox",
            "WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n,b",
            // DXMT's own logging. It is not wined3d, so no WINEDEBUG channel
            // reaches it: `+d3d11` and `+dxgi` name Wine's implementations,
            // which these overrides replace. DXMT reads DXMT_LOG_LEVEL
            // instead and defaults to info, which is why a device run carried
            // one line ("Failed to set Metal cache path") and nothing else -
            // everything below info was discarded before it was written.
            // trace is affordable here: the pinned source has eleven TRACE
            // and three DEBUG call sites in total, so this adds lines at the
            // rate of a program's D3D entry points, not its frames.
            "DXMT_LOG_LEVEL=trace",
            // Send it to Wine's debug output only. DXMT writes to a file as
            // well unless told otherwise, and the file it picks is relative
            // to the working directory - which on this lane is a read-only
            // projection. The capture already carries the guest's stderr as
            // `[guest fd=2 pid=...]`, so the file has no reader anyway.
            "DXMT_LOG_PATH=none",
        ]

        /// The environment for a launch that will enter 32-bit code.
        ///
        /// Wine's own 32-bit d3d9 is wined3d, which needs OpenGL or Vulkan.
        /// There is no OpenGL on iOS, and a device run showed both dlopens
        /// failing and `Direct3DCreate9` returning E_FAIL into a message box.
        /// DXVK's d3d9 needs Vulkan only, and the runtime now stages both the
        /// 32-bit DXVK image and the 64-bit Vulkan client library the chain
        /// ends at. This variable is what turns that projection on; the
        /// emulator reads it in `source/sdl/startupArgs.cpp` and does nothing
        /// unless the value is exactly "dxvk", so a 64-bit launch and a build
        /// whose layer carries no DXVK are both unaffected.
        static let wow64Environment =
            environment + ["BOXEDVN_WOW64_D3D9=dxvk"]

        /// The WINEDEBUG channels Settings' "Verbose Wine trace" adds, and
        /// the variable that tells the emulator to write the relay filter
        /// into the prefix before Wine opens it.
        ///
        /// relay is the only thing on this platform that can see a guest's
        /// Windows API calls at all: the syscall trace sees file opens and
        /// the X bridge sees windows, so a startup check that reads the
        /// registry, asks for a named object or looks at an environment
        /// variable is invisible - a device capture of exactly that shape
        /// carried no guest activity whatever between the program's imports
        /// finishing and its error dialog appearing. loaddll names each
        /// module as it attaches and debugstr carries whatever the program
        /// passes to OutputDebugString, both one line per event.
        ///
        /// Which modules relay covers is not settable from the environment:
        /// Wine reads RelayInclude and RelayExclude from
        /// HKCU\Software\Wine\Debug and from nowhere else. The variable
        /// below is what makes the emulator write them, and the list itself
        /// lives beside them in include/guest_wine64_layout.h
        /// (K_X64_WINE_TRACE_CHANNELS, K_X64_WINE_RELAY_INCLUDE) so the two
        /// halves can be compared by a test rather than by memory.
        static let verboseTraceChannels = "+relay,+loaddll,+debugstr,+seh,+uniscribe"
        static let verboseTraceAssignment = "BOXEDVN_X64_WINE_TRACE=relay"
        static let wineDebugAssignmentPrefix = "WINEDEBUG="

        /// `base` with the verbose trace folded in, or `base` unchanged when
        /// the setting is off.
        ///
        /// The channels are appended to whichever WINEDEBUG the lane already
        /// sets rather than replacing it: msgbox is what carries the text of
        /// the dialog the trace is being turned on to explain, and losing it
        /// would trade the answer for the evidence. A caller that supplied a
        /// WINEDEBUG of its own still wins over all of this, in
        /// BVNLaunchArguments.
        static func withVerboseTrace(_ base: [String],
                                     enabled: Bool) -> [String] {
            guard enabled else { return base }
            return base.map {
                $0.hasPrefix(wineDebugAssignmentPrefix)
                    ? $0 + "," + verboseTraceChannels : $0
            } + [verboseTraceAssignment]
        }
    }

    /// Locates the bundled Wine64, glibc and DXMT resources for `container`
    /// and stages the DXMT modules. Every resource is checked here so a
    /// packaging omission becomes an actionable alert instead of a guest
    /// loader failure several seconds later. Returns nil after setting the
    /// alert when something is missing.
    private func prepareX64Runtime(for container: WineContainer) -> X64Runtime? {
        guard let prefixes = Storage.winePrefixes else {
            alertMessage = "Could not create the Wine prefix directory."
            return nil
        }
        guard let glibcPointer = BVNPathBundledWine64GlibcZip(),
              let winePointer = BVNPathBundledWine64Zip(),
              let dxmtPointer = BVNPathBundledDXMTDirectory() else {
            alertMessage = "This build does not contain the validated Wine64 "
                         + "and DXMT resources."
            return nil
        }
        let files = ContainerLibrary.filesDirectory(for: container)
        let containerFolder = files.deletingLastPathComponent()
        let pe32Candidates = [
            containerFolder.appendingPathComponent(X64Runtime.pe32ArchiveName),
            Storage.documents?.appendingPathComponent(X64Runtime.pe32ArchiveName),
        ].compactMap { $0 }
        let pe32 = pe32Candidates.first { FileManager.default.fileExists(atPath: $0.path) }
        Log.write(pe32.map { "32-bit PE layer found at \($0.path)" }
                      ?? "No 32-bit PE layer: looked for \(X64Runtime.pe32ArchiveName) in "
                         + pe32Candidates.map(\.path).joined(separator: ", "),
                  category: "container")
        let runtime = X64Runtime(
            glibc: URL(fileURLWithPath: String(cString: glibcPointer)),
            wine64: URL(fileURLWithPath: String(cString: winePointer)),
            dxmt: URL(fileURLWithPath: String(cString: dxmtPointer)),
            writableRoot: prefixes.appendingPathComponent(
                container.prefixName + "-x64", isDirectory: true),
            files: files,
            driveC: ContainerLibrary.x64DriveC(for: container),
            diagnostics: files.appendingPathComponent(
                ".boxedvn-x64-diagnostics", isDirectory: true),
            pe32: pe32)
        do {
            try FileManager.default.createDirectory(
                at: runtime.diagnostics, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(
                at: runtime.driveC, withIntermediateDirectories: true)
            try migrateX64DriveC(from: runtime.writableRoot, to: runtime.driveC)
            for name in X64Runtime.dxmtModules {
                let source = runtime.dxmt.appendingPathComponent(name)
                let target = runtime.diagnostics.appendingPathComponent(name)
                guard FileManager.default.fileExists(atPath: source.path) else {
                    throw LaunchFailure(message: "The bundled DXMT runtime is "
                        + "missing \(name).")
                }
                if FileManager.default.fileExists(atPath: target.path) {
                    try FileManager.default.removeItem(at: target)
                }
                try FileManager.default.copyItem(at: source, to: target)
            }
        } catch {
            alertMessage = "The 64-bit runtime could not be staged: "
                         + error.localizedDescription
            return nil
        }
        return runtime
    }

    // MARK: - The 32-bit runtime layer

    /// What one reading of a `wine64-pe32.zip` found, remembered so the same
    /// archive is not re-read on every launch.
    private struct Pe32Inspection {
        /// Path, size and modification date. A replaced archive gets a new
        /// one and is read again; the same file never is.
        var identity: String
        /// Required modules the archive's central directory does not list, or
        /// lists as an empty entry.
        var missingModules: [String]
        /// False when the file could not be read as a ZIP at all.
        var readable: Bool
    }

    private var pe32Inspections: [String: Pe32Inspection] = [:]

    /// Checks the 32-bit runtime layer a WoW64 launch is about to mount, and
    /// returns the message to show when it cannot carry one.
    ///
    /// The archive is not opened or inflated: only its central directory is
    /// read, which is the last few tens of kilobytes of the file whatever its
    /// size, so this costs the same on a 500 MB layer as on a small one.
    ///
    /// Why it exists: the layer is a file the user downloads and copies into
    /// the container folder by hand, so the copy on a device can predate any
    /// packaging fix, and nothing on either side notices. One did, and the
    /// only symptom was a program that never opened a window - the loader
    /// searched for a builtin the archive did not carry and ended the process
    /// with STATUS_DLL_NOT_FOUND before its entry point. Naming the gap and
    /// the fix here turns that into a sentence the user can act on.
    private func pe32LayerProblem(at archive: URL) -> String? {
        let attributes = try? FileManager.default.attributesOfItem(
            atPath: archive.path)
        let size = (attributes?[.size] as? NSNumber)?.int64Value ?? -1
        let modified = (attributes?[.modificationDate] as? Date)?
            .timeIntervalSince1970 ?? -1
        let identity = "\(archive.path)|\(size)|\(modified)"

        var inspection = pe32Inspections[archive.path]
        if inspection?.identity != identity {
            inspection = inspect(pe32Archive: archive, identity: identity)
            pe32Inspections[archive.path] = inspection
        }
        guard let inspection else { return nil }

        let replace = "Download \(X64Runtime.pe32ArchiveName) again from the "
            + "build's release page and replace the copy in this container's "
            + "folder (next to Files), or in On My iPhone > BoxedVN."
        if !inspection.readable {
            return "The 32-bit runtime layer at \(archive.path) could not be "
                + "read as a ZIP archive, so a 32-bit program cannot be "
                + "started. " + replace
        }
        guard !inspection.missingModules.isEmpty else { return nil }
        let names = inspection.missingModules.joined(separator: ", ")
        return "The 32-bit runtime layer in this container is out of date: "
            + "\(X64Runtime.pe32ArchiveName) is missing "
            + "\(inspection.missingModules.count) of the "
            + "\(X64Runtime.pe32RequiredModules.count) modules a 32-bit "
            + "Windows program loads before it runs - \(names). A program "
            + "started with this layer exits with status 0xC0000135 and no "
            + "window. " + replace
    }

    private func inspect(pe32Archive archive: URL, identity: String)
        -> Pe32Inspection {
        guard let entries = ZipArchive.listEntries(at: archive) else {
            Log.write("The 32-bit PE layer at \(archive.path) has no readable "
                      + "ZIP central directory", category: "container",
                      level: BVNLogLevelError)
            return Pe32Inspection(identity: identity, missingModules: [],
                                  readable: false)
        }
        var present: Set<String> = []
        for entry in entries where entry.uncompressedSize > 0 {
            let name = entry.name.lowercased()
            guard name.contains(X64Runtime.pe32ModuleDirectory) else { continue }
            present.insert(entry.fileName.lowercased())
        }
        let missing = X64Runtime.pe32RequiredModules.filter {
            !present.contains($0)
        }
        Log.write("32-bit PE layer \(archive.lastPathComponent): "
                  + "\(entries.count) entries, "
                  + "\(present.count) i386-windows modules, "
                  + (missing.isEmpty
                        ? "all \(X64Runtime.pe32RequiredModules.count) "
                          + "required modules present"
                        : "missing " + missing.joined(separator: ", ")),
                  category: "container",
                  level: missing.isEmpty ? BVNLogLevelInfo
                                         : BVNLogLevelWarning)
        return Pe32Inspection(identity: identity, missingModules: missing,
                              readable: true)
    }

    /// The guard both WoW64 launches share: the layer has to be there, and it
    /// has to be complete. Returns false after setting the alert.
    private func allowWoW64Launch(with runtime: X64Runtime) -> Bool {
        guard let pe32 = runtime.pe32 else {
            alertMessage = "A 32-bit program runs through the 64-bit Wine, "
                + "which needs the 32-bit PE layer. Download "
                + "\(X64Runtime.pe32ArchiveName) from the build's release page "
                + "and put it in this container's folder (next to Files) or in "
                + "On My iPhone > BoxedVN, then try again."
            return false
        }
        if let problem = pe32LayerProblem(at: pe32) {
            alertMessage = problem
            return false
        }
        return true
    }

    /// Moves an existing prefix's drive_c out of the private writable root
    /// into the Files-visible folder the first time that folder is mounted,
    /// so Program Files and the user directories survive the move. Entries
    /// already present in the visible folder are kept; the old directory is
    /// removed only once it is empty.
    private func migrateX64DriveC(from writableRoot: URL, to driveC: URL) throws {
        let manager = FileManager.default
        let legacy = writableRoot.appendingPathComponent(
            "home/username/.wine64/drive_c", isDirectory: true)
        guard manager.fileExists(atPath: legacy.path) else { return }
        let present = Set(try manager.contentsOfDirectory(atPath: driveC.path))
        var moved = 0
        for name in try manager.contentsOfDirectory(atPath: legacy.path) {
            guard !present.contains(name) else { continue }
            try manager.moveItem(at: legacy.appendingPathComponent(name),
                                 to: driveC.appendingPathComponent(name))
            moved += 1
        }
        if try manager.contentsOfDirectory(atPath: legacy.path).isEmpty {
            try manager.removeItem(at: legacy)
        }
        if moved > 0 {
            Log.write("Moved \(moved) drive C entries of the 64-bit prefix into "
                      + "the Files-visible folder", category: "container")
        }
    }

    /// Runs the bundled AMD64 Direct3D 11 probe through Wine64 and the
    /// BoxedWine-owned FEX/DXMT path.
    func launchX64GraphicsProbe(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let probePointer = BVNPathBundledX64GraphicsProbe() else {
            alertMessage = "This build does not contain the AMD64 graphics probe."
            return
        }
        guard let runtime = prepareX64Runtime(for: container) else { return }
        let probe = URL(fileURLWithPath: String(cString: probePointer))

        do {
            let probeTarget = runtime.diagnostics.appendingPathComponent(
                "boxedvn-d3d11-cube-x64.exe")
            if FileManager.default.fileExists(atPath: probeTarget.path) {
                try FileManager.default.removeItem(at: probeTarget)
            }
            try FileManager.default.copyItem(at: probe, to: probeTarget)

            Log.write("Launching bundled AMD64 Direct3D 11 graphics probe "
                      + "through BoxedWine FEX and DXMT", category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                rootFilesystemOverlays: runtime.overlays,
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                // The executable stays a Windows path because Wine is what
                // reads that one.
                executablePath:
                    "d:\\.boxedvn-x64-diagnostics\\boxedvn-d3d11-cube-x64.exe",
                arguments: [],
                environment: X64Runtime.withVerboseTrace(
                    X64Runtime.environment,
                    enabled: Preferences.verboseWineTrace),
                workingDirectory: runtime.guestWorkingDirectory,
                width: container.width,
                height: container.height,
                soundEnabled: false,
                runThroughWine: true,
                useFEX64: true,
                useDXMT: true,
                winePrefixDriveC: runtime.driveC,
                wineRenderer: BVNWineRendererAutomatic,
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: runtime.diagnostics)
        } catch {
            alertMessage = "The 64-bit graphics probe could not start: "
                         + error.localizedDescription
        }
    }

    /// Runs a program the user picked from the container's drives on exactly
    /// the path the 64-bit cube takes: Wine64 through FEX, DXMT presenting
    /// Direct3D, one translated process - the one launched here.
    ///
    /// Two things differ from the cube, and both matter. The program runs
    /// from its own folder, so a DLL beside it and the data it opens by
    /// relative path resolve (a device session's interpreter child, started
    /// from the file manager, died with STATUS_DLL_NOT_FOUND while resolving
    /// its imports). And because the working directory is no longer where
    /// the DXMT modules are staged, the staging directory is named
    /// separately: it is what Wine's module root is overlaid from, and taking
    /// it from the working directory would have projected nothing.
    ///
    /// Starting the program here rather than double-clicking it in the file
    /// manager is what gives it the translator. A process the guest creates
    /// runs on the interpreter with no DXMT - exactly one process per session
    /// is translated (docs/KNOWN_LIMITATIONS_IOS.md).
    func launchX64Program(_ program: ContainerX64Program,
                          in container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let runtime = prepareX64Runtime(for: container) else { return }
        // A program whose PE header says i386 takes Wine's WoW64 lane, which
        // is served entirely out of the 32-bit layer the user supplies. The
        // picker already read that header, so the launch knows which lane it
        // is on before it starts anything.
        let isWoW64Program =
            program.executable.architecture == Self.x86_32ArchitectureName
        if isWoW64Program, !allowWoW64Launch(with: runtime) {
            return
        }
        rememberX64Program(program, for: container)
        do {
            Log.write("Launching \(program.guestExecutablePath) through "
                      + "BoxedWine FEX and DXMT, working directory "
                      + program.guestWorkingDirectory, category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                rootFilesystemOverlays: runtime.overlays,
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: program.guestExecutablePath,
                arguments: [],
                // Only a 32-bit program gets the DXVK d3d9 projection: it is
                // the WoW64 lane's renderer and has nothing to do with the
                // 64-bit lane, which reaches Direct3D 11 through DXMT.
                // Settings' "Verbose Wine trace" applies here and only
                // here: this is the launch that runs a program whose startup
                // can fail for a reason of its own, and the trace is far too
                // expensive to leave on for the probes and the desktop.
                environment: X64Runtime.withVerboseTrace(
                    isWoW64Program ? X64Runtime.wow64Environment
                                   : X64Runtime.environment,
                    enabled: Preferences.verboseWineTrace),
                workingDirectory: program.guestWorkingDirectory,
                width: container.width,
                height: container.height,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                useFEX64: true,
                useDXMT: true,
                winePrefixDriveC: runtime.driveC,
                wineRenderer: BVNWineRendererAutomatic,
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: program.hostDirectory,
                // Where prepareX64Runtime staged the DXMT modules, which is
                // no longer the working directory.
                dxmtModuleDirectory: runtime.guestWorkingDirectory)
        } catch {
            alertMessage = "\(program.name) could not start: "
                         + error.localizedDescription
        }
    }

    /// The program a container last ran, as `ContainerX64Program.id`. Kept so
    /// the row can say what it would run again; the picker still lists
    /// everything, because a remembered program can be deleted or renamed
    /// between sessions.
    func lastX64ProgramID(for container: WineContainer) -> String? {
        UserDefaults.standard.string(forKey: Self.x64ProgramKey(container))
    }

    private func rememberX64Program(_ program: ContainerX64Program,
                                    for container: WineContainer) {
        UserDefaults.standard.set(program.id,
                                  forKey: Self.x64ProgramKey(container))
    }

    private static func x64ProgramKey(_ container: WineContainer) -> String {
        "BoxedVN.x64Program." + container.id
    }

    /// What `architectureName` in Runtime.swift calls an i386 PE. Matched
    /// rather than re-derived so the two never drift apart silently.
    private static let x86_32ArchitectureName = "x86 32-bit"

    /// Wine's builtin explorer, named the way Wine's unix loader can resolve
    /// it without falling back to `start.exe`. See `launchX64Desktop`.
    private static let x64DesktopExplorerPath = "C:\\windows\\system32\\explorer.exe"

    /// Opens the container's desktop on the x86-64 lane: Wine64 through FEX,
    /// with the DXMT Direct3D modules projected in, so a 64-bit program
    /// started from the file manager renders the way the cube probe does. The
    /// desktop itself is the same explorer/winefile pair the 32-bit desktop
    /// uses, in the container's own resolution and its separate 64-bit
    /// prefix.
    ///
    /// The explorer image is named by its full Windows path rather than as
    /// the bare word `explorer`, so the process the launcher starts is the
    /// desktop itself. Wine's unix loader resolves argv[1] as a path first; a
    /// bare `explorer` is no path, so it falls back to loading `start.exe` as
    /// the main image and hands it `/exec explorer ...`, which leaves an
    /// extra process asleep in the middle of the session for no purpose.
    ///
    /// The desktop shell does NOT get the translator. Exactly one process per
    /// session can be translated, because only one can own the identity
    /// mapping of the guest address space, and the shell is the wrong one to
    /// give it to: it makes no Direct3D call all session, while every program
    /// it starts is refused for want of native memory and dies at once (log
    /// 2026-09-03, `BOXEDWINE_DXMT_RETURN ... reason=native-memory pid=45`,
    /// then `status=0xC0000005`). The kernel therefore leaves the role unheld
    /// for a launch whose command line names Wine's own infrastructure, and
    /// the first top-level Windows program started from the desktop takes it
    /// at its own exec - see `KProcess::execve`. The shell and the file
    /// manager run on the interpreter, which is what they already did on the
    /// 32-bit lane. The 32-bit lane keeps the bare name: it has no
    /// per-process translator to place.
    func launchX64Desktop(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let runtime = prepareX64Runtime(for: container) else { return }
        do {
            Log.write("Launching the 64-bit desktop through BoxedWine FEX and DXMT",
                      category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                rootFilesystemOverlays: runtime.overlays,
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: Self.x64DesktopExplorerPath,
                arguments: ["/desktop=shell,\(container.width)x\(container.height)",
                            "winefile", "D:\\"],
                environment: X64Runtime.withVerboseTrace(
                    X64Runtime.environment,
                    enabled: Preferences.verboseWineTrace),
                workingDirectory: runtime.guestWorkingDirectory,
                width: container.width,
                height: container.height,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true,
                useFEX64: true,
                useDXMT: true,
                winePrefixDriveC: runtime.driveC,
                wineRenderer: BVNWineRendererAutomatic,
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: runtime.diagnostics)
        } catch {
            alertMessage = "The 64-bit desktop could not start: "
                         + error.localizedDescription
        }
    }

    /// Browse the emulated PC: Wine's file manager, in the tools prefix that
    /// Notepad also runs in, so a file saved from one is visible in the other.
    /// Games keep their own prefixes - see `launch(_:)` for why.
    ///
    /// Keep the shared file-browser desktop at 1280x720. Imported games use a
    /// 1366x1024 default virtual monitor so their decorations fit too, while
    /// an older non-DPI-aware game can explicitly select a different desktop
    /// in its own launch settings.
    ///
    /// Two builds tried to run this inside Wine's virtual desktop
    /// (`explorer /desktop=shell,1280x720`) so it would look like Windows.
    /// Both produced a white rectangle behind the file manager, and the log
    /// says why: `NtUserChangeDisplaySettings ... returned -2` followed by
    /// `Failed to set primary display settings`. Wine's explorer cannot resize
    /// the emulated display, so its shell - taskbar, Start menu, icons - never
    /// starts and all that is left of the desktop is its blank window, sized
    /// to something other than the 1280x720 that was asked for. That is the
    /// white box.
    ///
    /// Without `/desktop` the file manager is a normal window on the X root,
    /// which is the same dark background the letterbox uses, and browsing
    /// files - the point of this mode - is unaffected.
    func launchWineDesktop() {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed. Import "
                         + "Boxedwine's root filesystem archive in Settings first."
            return
        }
        guard let writableRoot = Storage.toolsWinePrefix else {
            alertMessage = "Could not create the Wine prefix directory."
            return
        }

        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: nil,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "winefile",
                arguments: ["E:\\"],
                environment: [],
                workingDirectory: nil,
                width: 1280,
                height: 720,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true
            )
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    /// Runs Wine's own Notepad from the root filesystem, with no game mounted.
    /// This is the smoke test for "does the emulated environment boot at all".
    func launchWineNotepad() {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let writableRoot = Storage.toolsWinePrefix else {
            alertMessage = "Could not create the Wine prefix directory."
            return
        }

        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: nil,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "notepad",
                arguments: [],
                environment: [],
                workingDirectory: nil,
                width: 800,
                height: 600,
                soundEnabled: Preferences.soundEnabled,
                runThroughWine: true
            )
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    func requestShutdown() {
        // The runtime's own state, read straight out of C, not the published
        // copy. The published copy is refreshed from inside a
        // `Task { @MainActor }`, and while a guest runs the main actor is the
        // guest, so that copy is frozen at whatever it held when the launch
        // button was tapped - `idle`. Deciding anything from it meant the stop
        // button answering "No guest session is running" to the user of a
        // session that was very much running, over a page still showing it.
        //
        // BVNRuntimeGetState is a mutex read with no actor hop, so it is the
        // truth at the moment of the tap whatever the main actor is doing.
        let live = RuntimeState.current
        // Published now rather than at the next poll: the poll cannot run
        // until the guest has finished unwinding, so on device the page kept
        // saying "running" - and the whole UI looked frozen - for as long as
        // that took.
        if live == .running || live == .starting {
            runtimeState = .stopping
        } else {
            runtimeState = live
        }
        // Never an alert. A stop tap on a page that shows a session is always
        // meant, and the runtime answers a request it cannot serve by doing
        // nothing, which is the right answer here too.
        //
        // SDL_PushEvent takes the event queue's lock, which the guest loop
        // holds for stretches of its own, so the tap itself is answered off
        // the main thread.
        Task.detached(priority: .userInitiated) {
            Session.requestShutdown()
        }
    }

    // MARK: - Polling

    private func startPolling() {
        // The runtime state lives in C and changes on the main thread inside
        // BVNGuestMain, so it is polled rather than pushed.  Half a second is
        // enough for a state label and costs nothing.
        //
        // JIT is polled on the same timer, deliberately not just once at
        // launch.  A JIT enabler like StikDebug attaches asynchronously,
        // usually AFTER the app has already started, so a one-shot probe at
        // cold launch shows "unavailable" forever even once JIT genuinely
        // becomes available - the user has to know to dig into Runtime
        // status and tap "Re-check" for the badge to ever catch up. The probe
        // itself only reads csops/CS_DEBUGGED: cheap and safe enough to keep
        // live rather than rely on the user noticing it's stale.
        // .common, not the default mode this timer would otherwise get: the
        // guest runs on the main thread and the run loop spends its time in
        // tracking and other modes, where a default-mode timer does not fire
        // at all. This one still cannot outrun a blocked main actor - see
        // GuestPerformanceReadout, which samples its own numbers for that
        // reason - but it does keep the library screen's status live.
        pollTimer = Timer(timeInterval: 0.5, repeats: true) {
            [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let state = RuntimeState.current
                if state != self.runtimeState {
                    self.runtimeState = state
                    if state == .failed {
                        let message = Session.lastError
                        if !message.isEmpty {
                            self.alertMessage = message
                        }
                    }
                    self.finishInstallerIfNeeded(afterEntering: state)
                }
                self.refreshJIT()
                self.pollCount += 1
                if self.pollCount % 10 == 0 {
                    self.memory = .probe()
                }
            }
        }
        if let pollTimer {
            RunLoop.main.add(pollTimer, forMode: .common)
        }
    }

    private static func wineRenderer(for value: String) -> BVNWineRenderer {
        switch value {
        case "wined3d": return BVNWineRendererWineD3D
        case "dxvk": return BVNWineRendererDXVK
        default: return BVNWineRendererAutomatic
        }
    }

    private func finishInstallerIfNeeded(afterEntering state: RuntimeState) {
        guard state == .stopped || state == .failed,
              let installation = pendingGameInstallation else { return }
        pendingGameInstallation = nil
        installerProgressMessage = "Finding the installed game..."
        let runtimeFailure = state == .failed ? Session.lastError : ""

        Task.detached(priority: .userInitiated) {
            do {
                let game = try GameLibrary.finishInstaller(installation)
                await MainActor.run {
                    self.isInstallingGame = false
                    self.installerProgressMessage = ""
                    self.reloadGames()
                    self.alertMessage = "\(game.title) was installed and added "
                                      + "to the game list."
                    Log.write("Installed \(game.title); selected "
                              + game.selectedExecutable,
                              category: "installer")
                }
            } catch {
                GameLibrary.discardInstaller(installation)
                await MainActor.run {
                    self.isInstallingGame = false
                    self.installerProgressMessage = ""
                    let prefix = runtimeFailure.isEmpty
                        ? "The installer closed, but BoxedVN could not add the game."
                        : "The installer session failed: \(runtimeFailure)"
                    self.alertMessage = prefix + "\n\n" + error.localizedDescription
                    Log.write(self.alertMessage ?? error.localizedDescription,
                              category: "installer", level: BVNLogLevelError)
                }
            }
        }
    }

    /// Safe, timer-driven refresh - never risks executing generated code.
    /// See Runtime.swift's probeStatus()/probeExecuteUnsafe() split.
    func refreshJIT() {
        let updated = JITReport.probeStatus()
        // Comparing before assigning avoids an @Published fire (and a
        // SwiftUI re-render) on every tick when nothing actually changed.
        if updated.status != jit.status ||
            updated.debuggerAttached != jit.debuggerAttached ||
            updated.executableMemoryAvailable != jit.executableMemoryAvailable {
            jit = updated
        }
    }

    /// Deliberate device-only translator test. Like the executable-memory
    /// probe, this can terminate the process if JIT setup is invalid, so it is
    /// never run by startup or polling code.
    func runFEXBackendProbe() {
        guard fexBackend.built, !isProbingFEX else { return }
        isProbingFEX = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = FEXBackendReport.executeProbe()
            DispatchQueue.main.async {
                self.fexBackend = result
                self.isProbingFEX = false
            }
        }
    }

    func runLowAddressProbe() {
        guard !isProbingLowAddresses else { return }
        isProbingLowAddresses = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = LowAddressProbeReport.execute()
            DispatchQueue.main.async {
                self.lowAddressProbe = result
                self.isProbingLowAddresses = false
            }
        }
    }
}
