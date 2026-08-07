/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  The game library.  Every entry is backed by a manifest.json on disk written
 *  by the import path; nothing here is fabricated or seeded.
 */

import Foundation

struct Game: Identifiable, Hashable {
    var id: String
    var title: String
    var directory: URL          // <Documents>/Games/<id>
    var contentDirectory: URL   // <directory>/content
    var manifestURL: URL        // <directory>/manifest.json
    var selectedExecutable: String
    var workingDirectory: String
    var winePrefix: String
    var requestedWidth: UInt32
    var requestedHeight: UInt32
    var importedAt: Date

    var hasRunnableExecutable: Bool { !selectedExecutable.isEmpty }

    /// The guest path passed to Wine.  The game directory is mounted as D:.
    var guestExecutablePath: String {
        "d:\\" + selectedExecutable.replacingOccurrences(of: "/", with: "\\")
    }

    var guestWorkingDirectory: String? {
        guard !workingDirectory.isEmpty else { return nil }
        // -w takes an emulated *Linux* path; mount_drive links d: to
        // ~/.wine/dosdevices/d:
        return "/home/username/.wine/dosdevices/d:/" + workingDirectory
    }
}

enum GameLibraryError: LocalizedError {
    case noGamesDirectory
    case importFailed(String)
    case manifestFailed(String)

    var errorDescription: String? {
        switch self {
        case .noGamesDirectory:
            return "BoxedVN could not create its Games directory in Documents."
        case .importFailed(let detail):
            return detail
        case .manifestFailed(let detail):
            return detail
        }
    }
}

enum GameLibrary {
    /// Reads every manifest under <Documents>/Games.  A manifest that cannot
    /// be read is skipped and the reason is logged, rather than silently
    /// dropping the game.
    static func load() -> [Game] {
        guard let gamesRoot = Storage.games else { return [] }

        let entries = (try? FileManager.default.contentsOfDirectory(
            at: gamesRoot, includingPropertiesForKeys: [.isDirectoryKey])) ?? []

        var games: [Game] = []
        for entry in entries {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(
                atPath: entry.path, isDirectory: &isDirectory),
                isDirectory.boolValue else { continue }

            let manifestURL = entry.appendingPathComponent("manifest.json")
            guard FileManager.default.fileExists(atPath: manifestURL.path) else {
                continue
            }

            var summary = BVNManifestSummary()
            BVNManifestRead(manifestURL.path, &summary)
            guard summary.ok else {
                Log.write(
                    "Skipping \(entry.lastPathComponent): \(cString(&summary.error, Int(BVN_MAX_DIAGNOSTIC)))",
                    category: "library", level: BVNLogLevelWarning)
                continue
            }

            let identifier = cString(&summary.identifier, Int(BVN_MAX_SHORT))
            let title = cString(&summary.title, Int(BVN_MAX_PATH))
            games.append(Game(
                id: identifier,
                title: title.isEmpty ? identifier : title,
                directory: entry,
                contentDirectory: entry.appendingPathComponent("content"),
                manifestURL: manifestURL,
                selectedExecutable: cString(&summary.selectedExecutable, Int(BVN_MAX_PATH)),
                workingDirectory: cString(&summary.workingDirectory, Int(BVN_MAX_PATH)),
                winePrefix: cString(&summary.winePrefix, Int(BVN_MAX_SHORT)),
                requestedWidth: summary.requestedWidth,
                requestedHeight: summary.requestedHeight,
                importedAt: Date(timeIntervalSince1970: TimeInterval(summary.importedAtUnixSeconds))
            ))
        }
        return games.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    /// Imports a ZIP archive or an already-extracted folder.
    /// Blocking; call off the main thread.
    static func importGame(from source: URL, title: String) throws -> Game {
        guard let gamesRoot = Storage.games else {
            throw GameLibraryError.noGamesDirectory
        }

        var identifierBuffer = [CChar](repeating: 0, count: Int(BVN_MAX_SHORT))
        BVNMakeIdentifier(title, &identifierBuffer, identifierBuffer.count)
        var identifier = String(cString: identifierBuffer)

        // Never overwrite an existing game.
        var suffix = 2
        while FileManager.default.fileExists(
            atPath: gamesRoot.appendingPathComponent(identifier).path) {
            identifier = String(cString: identifierBuffer) + "-\(suffix)"
            suffix += 1
        }

        let gameDirectory = gamesRoot.appendingPathComponent(identifier)
        let contentDirectory = gameDirectory.appendingPathComponent("content")
        try FileManager.default.createDirectory(
            at: contentDirectory, withIntermediateDirectories: true)

        do {
            if source.pathExtension.lowercased() == "zip" {
                try extractArchive(source, into: contentDirectory)
            } else {
                try copyFolder(source, into: contentDirectory)
            }
        } catch {
            try? FileManager.default.removeItem(at: gameDirectory)
            throw error
        }

        let discovered = Executables.discover(in: contentDirectory)
        let manifestURL = gameDirectory.appendingPathComponent("manifest.json")
        try writeManifest(
            to: manifestURL, identifier: identifier, title: title,
            contentDirectory: contentDirectory, discovered: discovered)

        var summary = BVNManifestSummary()
        BVNManifestRead(manifestURL.path, &summary)

        return Game(
            id: identifier,
            title: title,
            directory: gameDirectory,
            contentDirectory: contentDirectory,
            manifestURL: manifestURL,
            selectedExecutable: cString(&summary.selectedExecutable, Int(BVN_MAX_PATH)),
            workingDirectory: cString(&summary.workingDirectory, Int(BVN_MAX_PATH)),
            winePrefix: identifier,
            requestedWidth: 0,
            requestedHeight: 0,
            importedAt: Date()
        )
    }

    static func updateLaunchSettings(
        for game: Game, selectedExecutable: String, workingDirectory: String,
        arguments: [String], width: UInt32, height: UInt32
    ) throws {
        var errorBuffer = [CChar](repeating: 0, count: Int(BVN_MAX_DIAGNOSTIC))
        let storage = arguments.map { strdup($0) }
        defer { storage.forEach { free($0) } }
        var pointers = storage.map { UnsafePointer<CChar>($0) }

        let ok = pointers.withUnsafeMutableBufferPointer { buffer in
            BVNManifestUpdateLaunchSettings(
                game.manifestURL.path, selectedExecutable, workingDirectory,
                buffer.baseAddress, buffer.count, width, height,
                &errorBuffer, errorBuffer.count)
        }
        if !ok {
            throw GameLibraryError.manifestFailed(String(cString: errorBuffer))
        }
    }

    /// The per-game arguments recorded in the manifest.  They cross the C
    /// boundary newline-separated; see BVNManifestCopyArgumentsJoined.
    static func arguments(for game: Game) -> [String] {
        var buffer = [CChar](repeating: 0, count: 8192)
        let written = BVNManifestCopyArgumentsJoined(
            game.manifestURL.path, &buffer, buffer.count)
        guard written > 0 else { return [] }
        return String(cString: buffer)
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map(String.init)
    }

    static func delete(_ game: Game) throws {
        try FileManager.default.removeItem(at: game.directory)
        if let prefixes = Storage.winePrefixes {
            let prefix = prefixes.appendingPathComponent(game.winePrefix)
            if FileManager.default.fileExists(atPath: prefix.path) {
                try? FileManager.default.removeItem(at: prefix)
            }
        }
    }

    // MARK: - Private

    private static func extractArchive(_ archive: URL, into destination: URL) throws {
        var listing = BVNZipListing()
        BVNZipInspect(archive.path, &listing)
        if !listing.ok {
            throw GameLibraryError.importFailed(
                cString(&listing.error, Int(BVN_MAX_DIAGNOSTIC)))
        }
        if listing.rejectedEntryCount > 0 {
            throw GameLibraryError.importFailed(
                "This archive contains \(listing.rejectedEntryCount) unsafe "
                + "entr\(listing.rejectedEntryCount == 1 ? "y" : "ies") and was "
                + "not extracted. First problem: "
                + cString(&listing.firstRejection, Int(BVN_MAX_DIAGNOSTIC)))
        }

        var result = BVNZipExtractionResult()
        BVNZipExtract(archive.path, destination.path, true, &result)
        if !result.ok {
            throw GameLibraryError.importFailed(
                cString(&result.error, Int(BVN_MAX_DIAGNOSTIC)))
        }
    }

    private static func copyFolder(_ source: URL, into destination: URL) throws {
        let entries = try FileManager.default.contentsOfDirectory(
            at: source, includingPropertiesForKeys: nil)
        for entry in entries {
            let target = destination.appendingPathComponent(entry.lastPathComponent)
            try FileManager.default.copyItem(at: entry, to: target)
        }
    }

    private static func writeManifest(
        to url: URL, identifier: String, title: String,
        contentDirectory: URL, discovered: [ExecutableDescription]
    ) throws {
        var entries = discovered.map { description -> BVNDiscoveredExecutable in
            var entry = BVNDiscoveredExecutable()
            withUnsafeMutablePointer(to: &entry.relativePath) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_PATH)) { raw in
                    _ = description.relativePath.withCString {
                        strlcpy(raw, $0, Int(BVN_MAX_PATH))
                    }
                }
            }
            entry.info.runnable = description.runnable
            entry.info.subsystem = description.subsystem
            entry.info.architecture = architectureValue(description.architecture)
            withUnsafeMutablePointer(to: &entry.info.format) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_SHORT)) { raw in
                    _ = description.format.withCString {
                        strlcpy(raw, $0, Int(BVN_MAX_SHORT))
                    }
                }
            }
            withUnsafeMutablePointer(to: &entry.info.diagnostic) { pointer in
                pointer.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_DIAGNOSTIC)) { raw in
                    _ = description.diagnostic.withCString {
                        strlcpy(raw, $0, Int(BVN_MAX_DIAGNOSTIC))
                    }
                }
            }
            return entry
        }

        var errorBuffer = [CChar](repeating: 0, count: Int(BVN_MAX_DIAGNOSTIC))
        let ok = entries.withUnsafeMutableBufferPointer { buffer in
            BVNManifestWriteForImport(
                url.path, identifier, title, contentDirectory.path, nil,
                buffer.baseAddress, buffer.count,
                Int64(Date().timeIntervalSince1970),
                &errorBuffer, errorBuffer.count)
        }
        if !ok {
            throw GameLibraryError.manifestFailed(String(cString: errorBuffer))
        }
    }

    private static func architectureValue(_ name: String) -> BVNGuestArchitecture {
        switch name {
        case "x86 16-bit": return BVNGuestArchitectureX86_16
        case "x86 32-bit": return BVNGuestArchitectureX86_32
        case "x86-64": return BVNGuestArchitectureX86_64
        default: return BVNGuestArchitectureUnknown
        }
    }
}

/// Reads a fixed-size C char array embedded in an imported struct.
func cString<T>(_ pointer: UnsafeMutablePointer<T>, _ capacity: Int) -> String {
    pointer.withMemoryRebound(to: CChar.self, capacity: capacity) {
        String(cString: $0)
    }
}
