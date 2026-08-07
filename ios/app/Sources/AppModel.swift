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
    @Published private(set) var jit: JITReport = .probe()
    @Published private(set) var runtimeState: RuntimeState = .idle
    @Published private(set) var isImporting = false
    @Published var importProgressMessage = ""
    @Published var alertMessage: String?

    private var pollTimer: Timer?

    init() {
        reloadGames()
        startPolling()
        Log.write("BoxedVN frontend initialised; Boxedwine core "
                  + Session.boxedwineVersion, category: "app")
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
        return "\(url.lastPathComponent) (\(sizeText))"
    }

    func importRootFilesystem(from url: URL) {
        guard let destination = Storage.rootFilesystems else {
            alertMessage = "Could not create the root filesystem directory."
            return
        }
        let needsScope = url.startAccessingSecurityScopedResource()
        defer { if needsScope { url.stopAccessingSecurityScopedResource() } }

        let target = destination.appendingPathComponent("boxedwine.zip")
        do {
            if FileManager.default.fileExists(atPath: target.path) {
                try FileManager.default.removeItem(at: target)
            }
            try FileManager.default.copyItem(at: url, to: target)
            Log.write("Root filesystem installed from \(url.lastPathComponent)",
                      category: "rootfs")
            objectWillChange.send()
        } catch {
            alertMessage = "Could not install the root filesystem: "
                         + error.localizedDescription
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
        // itself is one mmap+munmap of a single page: cheap enough to just
        // keep it live rather than rely on the user noticing it's stale.
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
            }
        }
    }

    func refreshJIT() {
        let updated = JITReport.probe()
        // Comparing before assigning avoids an @Published fire (and a
        // SwiftUI re-render) on every tick when nothing actually changed.
        if updated.status != jit.status ||
            updated.debuggerAttached != jit.debuggerAttached ||
            updated.executableMemoryAvailable != jit.executableMemoryAvailable {
            jit = updated
        }
    }
}
