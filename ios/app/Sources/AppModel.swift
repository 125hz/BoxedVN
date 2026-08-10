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
    // .probeStatus(), never .probeExecuteUnsafe(): this runs at app launch,
    // before the user has done anything, and the unsafe probe can crash the
    // process with no recovery possible. See Runtime.swift / BVNRuntime.h.
    @Published private(set) var jit: JITReport = .probeStatus()
    @Published private(set) var memory: MemoryReport = .probe()
    @Published private(set) var runtimeState: RuntimeState = .idle
    @Published private(set) var isImporting = false
    @Published var importProgressMessage = ""
    @Published private(set) var isInstallingRootFilesystem = false
    @Published var alertMessage: String?

    private var pollTimer: Timer?
    private var pollCount = 0

    init() {
        reloadGames()
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

        let writableRoot = prefixes.appendingPathComponent(game.winePrefix)
        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: writableRoot,
                gameDirectory: game.contentDirectory,
                executablePath: game.guestExecutablePath,
                arguments: GameLibrary.arguments(for: game),
                environment: [],
                workingDirectory: game.guestWorkingDirectory,
                width: game.requestedWidth,
                height: game.requestedHeight,
                soundEnabled: true,
                runThroughWine: true
            )
        } catch {
            alertMessage = error.localizedDescription
        }
    }

    /// Runs Wine's desktop shell: a real Windows desktop with a taskbar, a
    /// Start menu and Explorer, in the same emulated environment a game gets.
    ///
    /// 1280x720 rather than the 800x600 a game defaults to. 16:9 matches the
    /// phone closely enough that the letterbox is thin, which keeps the
    /// desktop's own edges - where a taskbar and window controls live - away
    /// from the Dynamic Island instead of underneath it.
    func launchWineDesktop() {
        guard let rootFilesystem else {
            alertMessage = "No root filesystem is installed. Import "
                         + "Boxedwine's root filesystem archive in Settings first."
            return
        }
        guard let prefixes = Storage.winePrefixes else {
            alertMessage = "Could not create the Wine prefix directory."
            return
        }

        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: prefixes.appendingPathComponent("desktop"),
                gameDirectory: nil,
                executablePath: "explorer",
                // Wine's own switch for "give me a desktop window with a
                // shell in it" rather than rootless windows on the X root.
                // The desktop must be called "shell": that is the name Wine's
                // explorer treats as "run the shell in here", which is what
                // puts a taskbar, a Start menu and desktop icons on it. Any
                // other name gives a bare desktop window - which is the white
                // rectangle and nothing else that build 70 produced.
                // Run winefile *inside* the desktop rather than relying on
                // the shell alone. Build 71 gave a bare white rectangle: Wine's
                // explorer logged "NtUserChangeDisplaySettings ... returned -2"
                // and "Failed to set primary display settings", so its shell -
                // taskbar, Start menu, icons - never came up and the desktop
                // window was all that was left. A file manager launched into
                // the same desktop does not depend on that, and browsing files
                // is the point of this mode.
                arguments: ["/desktop=shell,1280x720", "winefile"],
                environment: [],
                workingDirectory: nil,
                width: 1280,
                height: 720,
                soundEnabled: true,
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
        guard let prefixes = Storage.winePrefixes else {
            alertMessage = "Could not create the Wine prefix directory."
            return
        }

        do {
            try Session.launch(
                rootFilesystem: rootFilesystem,
                writableRoot: prefixes.appendingPathComponent("default"),
                gameDirectory: nil,
                executablePath: "notepad",
                arguments: [],
                environment: [],
                workingDirectory: nil,
                width: 800,
                height: 600,
                soundEnabled: true,
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
                }
                self.refreshJIT()
                self.pollCount += 1
                if self.pollCount % 10 == 0 {
                    self.memory = .probe()
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
}
