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
    var renderer: String
    var requestedWidth: UInt32
    var requestedHeight: UInt32
    var importedAt: Date

    var hasRunnableExecutable: Bool { !selectedExecutable.isEmpty }

    /// Installer-created games live inside their persistent Wine C: drive.
    /// Imported archives are external content mounted as D:.
    var isInstalledInWinePrefix: Bool {
        contentDirectory.standardizedFileURL.path
            .replacingOccurrences(of: "\\", with: "/")
            .hasSuffix("/home/username/.wine/drive_c")
    }

    private var guestDriveLetter: String {
        isInstalledInWinePrefix ? "c" : "d"
    }

    /// The guest path passed to Wine. Imported content is mounted as D:;
    /// installer-created content retains its original C: path.
    var guestExecutablePath: String {
        guestDriveLetter + ":\\"
            + selectedExecutable.replacingOccurrences(of: "/", with: "\\")
    }

    var guestWorkingDirectory: String {
        // Older Windows games commonly open data files relative to their
        // process working directory. An empty manifest value means "use the
        // executable's folder", not Boxedwine's unrelated Linux default.
        let normalizedExecutable = selectedExecutable
            .replacingOccurrences(of: "\\", with: "/")
        let executableParts = normalizedExecutable.split(separator: "/")
        let inferredDirectory = executableParts.dropLast().joined(separator: "/")
        let relativeDirectory = workingDirectory.isEmpty
            ? inferredDirectory
            : workingDirectory.replacingOccurrences(of: "\\", with: "/")
        let trimmedDirectory = relativeDirectory.trimmingCharacters(
            in: CharacterSet(charactersIn: "/"))

        // -w takes an emulated Linux path. Imported content is linked at d:;
        // an installer-created game stays at its original c: location.
        let driveRoot = "/home/username/.wine/dosdevices/"
            + guestDriveLetter + ":/"
        return trimmedDirectory.isEmpty
            ? driveRoot
            : driveRoot + trimmedDirectory
    }
}

struct PendingGameInstallation: Hashable {
    var id: String
    var title: String
    var directory: URL
    var stagingDirectory: URL
    var installerURL: URL
    var manifestURL: URL
    var winePrefixRoot: URL

    var guestInstallerPath: String {
        "d:\\" + installerURL.lastPathComponent
    }

    var installedContentDirectory: URL {
        winePrefixRoot
            .appendingPathComponent("home", isDirectory: true)
            .appendingPathComponent("username", isDirectory: true)
            .appendingPathComponent(".wine", isDirectory: true)
            .appendingPathComponent("drive_c", isDirectory: true)
    }
}

enum GameLibraryError: LocalizedError {
    case noGamesDirectory
    case noWinePrefixDirectory
    case importFailed(String)
    case manifestFailed(String)

    var errorDescription: String? {
        switch self {
        case .noGamesDirectory:
            return "BoxedVN could not create its Games directory in Documents."
        case .noWinePrefixDirectory:
            return "BoxedVN could not create its Wine prefix directory."
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

            games.append(game(from: entry, summary: &summary))
        }
        return games.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    /// Imports a ZIP archive or an already-extracted folder.
    /// Blocking; call off the main thread.
    static func importGame(from source: URL, title: String) throws -> Game {
        guard let gamesRoot = Storage.games else {
            throw GameLibraryError.noGamesDirectory
        }

        let identifier = uniqueIdentifier(for: title, in: gamesRoot)

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
            renderer: "automatic",
            requestedWidth: 0,
            requestedHeight: 0,
            importedAt: Date()
        )
    }

    /// Copies an installer into a new game's staging directory. The caller
    /// launches it with `winePrefixRoot` as the writable Wine prefix, then
    /// calls `finishInstaller` after the guest session exits. Blocking; call
    /// off the main thread.
    static func prepareInstaller(from source: URL, title: String) throws
        -> PendingGameInstallation {
        let title = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty else {
            throw GameLibraryError.importFailed(
                "Enter a name for the installed game.")
        }
        guard source.pathExtension.lowercased() == "exe" else {
            throw GameLibraryError.importFailed(
                "Choose a Windows .exe installer. MSI packages are not supported yet.")
        }
        let inspection = Executables.inspect(at: source)
        guard inspection.runnable else {
            throw GameLibraryError.importFailed(
                "\(source.lastPathComponent) cannot run in BoxedVN: "
                + inspection.diagnostic)
        }
        guard let gamesRoot = Storage.games else {
            throw GameLibraryError.noGamesDirectory
        }
        guard let prefixesRoot = Storage.winePrefixes else {
            throw GameLibraryError.noWinePrefixDirectory
        }

        let identifier = uniqueIdentifier(for: title, in: gamesRoot)
        let gameDirectory = gamesRoot.appendingPathComponent(identifier)
        let stagingDirectory = gameDirectory.appendingPathComponent("installer")
        let installerURL = stagingDirectory.appendingPathComponent(
            source.lastPathComponent)
        do {
            try FileManager.default.createDirectory(
                at: stagingDirectory, withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: source, to: installerURL)
        } catch {
            try? FileManager.default.removeItem(at: gameDirectory)
            throw GameLibraryError.importFailed(
                "Could not stage the installer: \(error.localizedDescription)")
        }

        return PendingGameInstallation(
            id: identifier,
            title: title,
            directory: gameDirectory,
            stagingDirectory: stagingDirectory,
            installerURL: installerURL,
            manifestURL: gameDirectory.appendingPathComponent("manifest.json"),
            winePrefixRoot: prefixesRoot.appendingPathComponent(identifier))
    }

    /// Scans the installer's persistent C: drive and creates the library
    /// manifest. The manifest deliberately points at that drive rather than
    /// copying files elsewhere: registry entries, DLLs and absolute paths all
    /// remain in the exact prefix in which the installer created them.
    static func finishInstaller(_ installation: PendingGameInstallation) throws
        -> Game {
        var contentDirectory = installation.installedContentDirectory
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: contentDirectory.path, isDirectory: &isDirectory),
            isDirectory.boolValue else {
            throw GameLibraryError.importFailed(
                "The installer exited without creating a Windows C: drive.")
        }

        var discovered = Executables.discover(in: contentDirectory).filter {
            $0.runnable && BVNLooksLikeInstalledGameExecutable($0.relativePath)
        }
        // A few installers let the user choose D:. D: is the staging folder
        // during installation, so retain and launch that directory when it
        // contains the game instead of assuming every installer used C:.
        if discovered.isEmpty {
            let stagedDiscoveries = Executables.discover(
                in: installation.stagingDirectory).filter {
                    $0.runnable &&
                    $0.relativePath.caseInsensitiveCompare(
                        installation.installerURL.lastPathComponent)
                        != .orderedSame &&
                    BVNLooksLikeInstalledGameExecutable($0.relativePath)
                }
            if !stagedDiscoveries.isEmpty {
                contentDirectory = installation.stagingDirectory
                discovered = stagedDiscoveries
            }
        }
        guard !discovered.isEmpty else {
            throw GameLibraryError.importFailed(
                "No compatible 32-bit game executable was found in the "
                + "installer's C: drive. The installation may have been "
                + "cancelled, or the game may be 64-bit only.")
        }
        try writeManifest(
            to: installation.manifestURL,
            identifier: installation.id,
            title: installation.title,
            contentDirectory: contentDirectory,
            discovered: discovered)

        // The imported EXE was only a staging copy. Remove it when the program
        // lives in drive_c; retain the directory when the user installed to D:.
        if contentDirectory != installation.stagingDirectory {
            try? FileManager.default.removeItem(at: installation.stagingDirectory)
        }

        var summary = BVNManifestSummary()
        BVNManifestRead(installation.manifestURL.path, &summary)
        guard summary.ok else {
            throw GameLibraryError.manifestFailed(
                cString(&summary.error, Int(BVN_MAX_DIAGNOSTIC)))
        }
        return game(from: installation.directory, summary: &summary)
    }

    static func discardInstaller(_ installation: PendingGameInstallation) {
        try? FileManager.default.removeItem(at: installation.directory)
        try? FileManager.default.removeItem(at: installation.winePrefixRoot)
    }

    static func updateLaunchSettings(
        for game: Game, selectedExecutable: String, workingDirectory: String,
        renderer: String, arguments: [String], environment: [String],
        width: UInt32, height: UInt32
    ) throws {
        var errorBuffer = [CChar](repeating: 0, count: Int(BVN_MAX_DIAGNOSTIC))
        let argumentStorage = arguments.map { strdup($0) }
        let environmentStorage = environment.map { strdup($0) }
        defer {
            argumentStorage.forEach { free($0) }
            environmentStorage.forEach { free($0) }
        }
        var argumentPointers = argumentStorage.map { UnsafePointer<CChar>($0) }
        var environmentPointers =
            environmentStorage.map { UnsafePointer<CChar>($0) }

        let ok = argumentPointers.withUnsafeMutableBufferPointer { args in
            environmentPointers.withUnsafeMutableBufferPointer { env in
                BVNManifestUpdateLaunchSettings(
                    game.manifestURL.path, selectedExecutable, workingDirectory,
                    renderer,
                    args.baseAddress, args.count,
                    env.baseAddress, env.count, width, height,
                    &errorBuffer, errorBuffer.count)
            }
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

    /// The per-game environment recorded in the manifest, each entry
    /// "NAME=VALUE".  Same newline encoding as the arguments above.
    ///
    /// These reach the guest as Boxedwine `-env` values, and a game's own
    /// entry wins over BoxedVN's defaults - including WINEDEBUG, which is how
    /// a title that starts but renders nothing can be made to say why without
    /// waiting for a new build.
    static func environment(for game: Game) -> [String] {
        var buffer = [CChar](repeating: 0, count: 8192)
        let written = BVNManifestCopyEnvironmentJoined(
            game.manifestURL.path, &buffer, buffer.count)
        guard written > 0 else { return [] }
        return String(cString: buffer)
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map(String.init)
    }

    static func delete(_ game: Game) throws {
        try FileManager.default.removeItem(at: game.directory)
        // The game's own prefix, which holds its saves. Never the tools prefix
        // or build 73's shared one: those belong to more than this game.
        if let prefixes = Storage.winePrefixes,
           game.winePrefix != Storage.toolsWinePrefixName,
           game.winePrefix != Storage.build73SharedWinePrefixName,
           !game.winePrefix.hasPrefix("container-") {
            let prefix = prefixes.appendingPathComponent(game.winePrefix)
            if FileManager.default.fileExists(atPath: prefix.path) {
                try? FileManager.default.removeItem(at: prefix)
            }
        }
    }

    /// Adds a launcher manifest for a program that remains inside a shared
    /// container. The shortcut owns only its manifest; deleting it must never
    /// remove the container prefix or the other programs installed there.
    static func createContainerShortcut(
        title: String, contentDirectory: URL,
        executable: ExecutableDescription, winePrefix: String,
        renderer: String, width: UInt32, height: UInt32
    ) throws -> Game {
        guard let gamesRoot = Storage.games else {
            throw GameLibraryError.noGamesDirectory
        }
        let identifier = uniqueIdentifier(for: title, in: gamesRoot)
        let directory = gamesRoot.appendingPathComponent(
            identifier, isDirectory: true)
        try FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true)
        let manifest = directory.appendingPathComponent("manifest.json")
        do {
            try writeManifest(
                to: manifest, identifier: identifier, title: title,
                contentDirectory: contentDirectory,
                discovered: [executable], winePrefix: winePrefix)
            var summary = BVNManifestSummary()
            BVNManifestRead(manifest.path, &summary)
            guard summary.ok else {
                throw GameLibraryError.manifestFailed(
                    cString(&summary.error, Int(BVN_MAX_DIAGNOSTIC)))
            }
            var game = game(from: directory, summary: &summary)
            try updateLaunchSettings(
                for: game, selectedExecutable: executable.relativePath,
                workingDirectory: "", renderer: renderer,
                arguments: [], environment: [], width: width, height: height)
            BVNManifestRead(manifest.path, &summary)
            game = game(from: directory, summary: &summary)
            return game
        } catch {
            try? FileManager.default.removeItem(at: directory)
            throw error
        }
    }

    // MARK: - Private

    private static func uniqueIdentifier(for title: String, in gamesRoot: URL)
        -> String {
        var identifierBuffer = [CChar](repeating: 0, count: Int(BVN_MAX_SHORT))
        BVNMakeIdentifier(title, &identifierBuffer, identifierBuffer.count)
        let base = String(cString: identifierBuffer)
        var identifier = base
        var suffix = 2
        while FileManager.default.fileExists(
            atPath: gamesRoot.appendingPathComponent(identifier).path) {
            identifier = base + "-\(suffix)"
            suffix += 1
        }
        return identifier
    }

    private static func game(from directory: URL,
                             summary: inout BVNManifestSummary) -> Game {
        let identifier = cString(&summary.identifier, Int(BVN_MAX_SHORT))
        let title = cString(&summary.title, Int(BVN_MAX_PATH))
        let winePrefix = cString(&summary.winePrefix, Int(BVN_MAX_SHORT))
        let storedContent = cString(
            &summary.contentDirectory, Int(BVN_MAX_PATH))
        let importedContent = directory.appendingPathComponent(
            "content", isDirectory: true)
        let stagedContent = directory.appendingPathComponent(
            "installer", isDirectory: true)
        let storedPath = storedContent.replacingOccurrences(
            of: "\\", with: "/")
        let contentDirectory: URL
        // Archive imports are self-contained beside the manifest. Prefer that
        // stable relative location even if an older manifest recorded the
        // app container's now-stale absolute UUID (for example after restore).
        if FileManager.default.fileExists(atPath: importedContent.path) {
            contentDirectory = importedContent
        } else if FileManager.default.fileExists(atPath: stagedContent.path) {
            contentDirectory = stagedContent
        } else if storedPath.hasSuffix("/home/username/.wine/drive_c"),
                  let prefixes = Storage.winePrefixes {
            contentDirectory = prefixes
                .appendingPathComponent(winePrefix, isDirectory: true)
                .appendingPathComponent("home", isDirectory: true)
                .appendingPathComponent("username", isDirectory: true)
                .appendingPathComponent(".wine", isDirectory: true)
                .appendingPathComponent("drive_c", isDirectory: true)
        } else if storedContent.isEmpty {
            contentDirectory = importedContent
        } else if NSString(string: storedContent).isAbsolutePath {
            contentDirectory = URL(
                fileURLWithPath: storedContent, isDirectory: true)
        } else {
            contentDirectory = directory.appendingPathComponent(
                storedContent, isDirectory: true)
        }
        return Game(
            id: identifier,
            title: title.isEmpty ? identifier : title,
            directory: directory,
            contentDirectory: contentDirectory,
            manifestURL: directory.appendingPathComponent("manifest.json"),
            selectedExecutable: cString(
                &summary.selectedExecutable, Int(BVN_MAX_PATH)),
            workingDirectory: cString(
                &summary.workingDirectory, Int(BVN_MAX_PATH)),
            winePrefix: winePrefix,
            renderer: cString(&summary.renderer, Int(BVN_MAX_SHORT)),
            requestedWidth: summary.requestedWidth,
            requestedHeight: summary.requestedHeight,
            importedAt: Date(timeIntervalSince1970: TimeInterval(
                summary.importedAtUnixSeconds)))
    }

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
        contentDirectory: URL, discovered: [ExecutableDescription],
        winePrefix: String? = nil
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
        let write: (UnsafePointer<CChar>?) -> Bool = { prefix in
            entries.withUnsafeMutableBufferPointer { buffer in
                BVNManifestWriteForImport(
                    url.path, identifier, title, contentDirectory.path, prefix,
                    buffer.baseAddress, buffer.count,
                    Int64(Date().timeIntervalSince1970),
                    &errorBuffer, errorBuffer.count)
            }
        }
        let ok = winePrefix?.withCString { write($0) } ?? write(nil)
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
