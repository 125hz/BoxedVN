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
    func launchGraphicsProbe(_ container: WineContainer) {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed."
            return
        }
        guard let writableRoot = ContainerLibrary.prefixRoot(for: container) else {
            alertMessage = "Could not create the container prefix."
            return
        }
        guard let source = Bundle.main.url(
            forResource: "boxedvn-d3d9-cube", withExtension: "exe") else {
            alertMessage = "This build does not include the 32-bit graphics probe."
            return
        }

        let files = ContainerLibrary.filesDirectory(for: container)
        let diagnostics = files.appendingPathComponent(
            ".boxedvn-diagnostics", isDirectory: true)
        let target = diagnostics.appendingPathComponent(source.lastPathComponent)
        do {
            try FileManager.default.createDirectory(
                at: diagnostics, withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: target.path) {
                try FileManager.default.removeItem(at: target)
            }
            try FileManager.default.copyItem(at: source, to: target)
            Log.write("Launching bundled IA-32 Direct3D 9 graphics probe",
                      category: "container")
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: files,
                sharedDirectory: Storage.sharedFiles,
                executablePath:
                    "d:\\.boxedvn-diagnostics\\boxedvn-d3d9-cube.exe",
                arguments: [],
                environment: ["WINEDEBUG=warn+d3d_shader,-d3d"],
                workingDirectory: "d:\\.boxedvn-diagnostics\\",
                width: container.width,
                height: container.height,
                soundEnabled: false,
                runThroughWine: true,
                wineRenderer: BVNWineRendererWineD3D,
                sharedDriveLetter:
                    container.sharedDriveLetter.lowercased().first ?? "e",
                windowsVersion: container.windowsVersion,
                compatibilityDirectory: diagnostics)
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
            "WINEDEBUG=warn+module,warn+seh",
            "WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n,b",
        ]
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
        let runtime = X64Runtime(
            glibc: URL(fileURLWithPath: String(cString: glibcPointer)),
            wine64: URL(fileURLWithPath: String(cString: winePointer)),
            dxmt: URL(fileURLWithPath: String(cString: dxmtPointer)),
            writableRoot: prefixes.appendingPathComponent(
                container.prefixName + "-x64", isDirectory: true),
            files: files,
            driveC: files.deletingLastPathComponent()
                .appendingPathComponent("Drive C (64-bit)", isDirectory: true),
            diagnostics: files.appendingPathComponent(
                ".boxedvn-x64-diagnostics", isDirectory: true))
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
                rootFilesystemOverlays: [runtime.glibc, runtime.wine64],
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                // The executable stays a Windows path because Wine is what
                // reads that one.
                executablePath:
                    "d:\\.boxedvn-x64-diagnostics\\boxedvn-d3d11-cube-x64.exe",
                arguments: [],
                environment: X64Runtime.environment,
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

    /// Opens the container's desktop on the x86-64 lane: Wine64 through FEX,
    /// with the DXMT Direct3D modules projected in, so a 64-bit program
    /// started from the file manager renders the way the cube probe does. The
    /// desktop itself is the same explorer/winefile pair the 32-bit desktop
    /// uses, in the container's own resolution and its separate 64-bit
    /// prefix.
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
                rootFilesystemOverlays: [runtime.glibc, runtime.wine64],
                writableRoot: runtime.writableRoot,
                gameDirectory: runtime.files,
                sharedDirectory: Storage.sharedFiles,
                executablePath: "explorer",
                arguments: ["/desktop=shell,\(container.width)x\(container.height)",
                            "winefile", "D:\\"],
                environment: X64Runtime.environment,
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
        if !Session.requestShutdown() {
            alertMessage = "No guest session is running."
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
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
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
