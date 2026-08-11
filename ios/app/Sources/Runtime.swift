/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  Swift-side wrappers over the C ABI in BVNRuntime.h and BVNImport.h.  Every
 *  call here reaches real emulator or filesystem code; nothing is simulated.
 */

import Foundation

// MARK: - JIT

struct JITReport {
    enum Status {
        case unknown
        case available        // BVNJITProbeExecute() actually ran generated code
        case likelyAvailable  // CS_DEBUGGED is set; execution has not been risked
        case unavailable
    }

    var status: Status
    var jitCompiledIn: Bool
    var debuggerAttached: Bool
    var executableMemoryAvailable: Bool
    var detail: String

    var isUsable: Bool { status == .available || status == .likelyAvailable }

    private static func convert(_ report: BVNJITReport) -> JITReport {
        let status: Status
        switch report.status {
        case BVNJITStatusAvailable: status = .available
        case BVNJITStatusLikelyAvailable: status = .likelyAvailable
        case BVNJITStatusUnavailable: status = .unavailable
        default: status = .unknown
        }
        return JITReport(
            status: status,
            jitCompiledIn: report.jitCompiledIn,
            debuggerAttached: report.debuggerAttached,
            executableMemoryAvailable: report.executableMemoryAvailable,
            detail: report.detail.map(String.init(cString:)) ?? ""
        )
    }

    /// Safe: never executes generated code, so it can never crash the
    /// process. This is the only probe automatic code (app launch, the
    /// polling timer) may ever call.
    static func probeStatus() -> JITReport {
        convert(BVNJITProbeStatus())
    }

    /// UNSAFE. Actually maps, writes and calls a small test function; if
    /// executable memory is not really available, iOS can deliver an
    /// uncatchable SIGKILL right here with no way to recover in-process.
    /// Only call this from a handler for a specific, explicit user action
    /// where a resulting crash is an acceptable, explainable consequence of
    /// what they just did — see BVNRuntime.h for the reasoning. Session.launch
    /// already triggers Boxedwine's own equivalent check before starting a
    /// guest; do not add another automatic call site for this.
    static func probeExecuteUnsafe() -> JITReport {
        convert(BVNJITProbeExecute())
    }
}

// MARK: - Memory entitlement

struct MemoryReport {
    enum EntitlementStatus {
        case unknown
        case disabled
        case enabled
    }

    var entitlement: EntitlementStatus
    var availableBytes: UInt64
    var physicalMemoryBytes: UInt64
    var detail: String

    var statusText: String {
        switch entitlement {
        case .enabled: return "Increased limit signed"
        case .disabled: return "Standard limit"
        case .unknown: return "Could not verify"
        }
    }

    var availableText: String {
        ByteCountFormatter.string(fromByteCount: Int64(availableBytes),
                                  countStyle: .memory)
    }

    static func probe() -> MemoryReport {
        let report = BVNMemoryProbe()
        let status: EntitlementStatus
        switch report.increasedMemoryLimit {
        case BVNMemoryEntitlementEnabled: status = .enabled
        case BVNMemoryEntitlementDisabled: status = .disabled
        default: status = .unknown
        }
        return MemoryReport(
            entitlement: status,
            availableBytes: report.availableBytes,
            physicalMemoryBytes: report.physicalMemoryBytes,
            detail: report.detail.map(String.init(cString:)) ?? ""
        )
    }
}

// MARK: - Version

/// Identifies exactly which build is installed.
///
/// BoxedVN is sideloaded, so there is no App Store version history and no
/// update prompt: the only way to know whether the IPA on the device is the
/// newest one is for the app to say so itself.  The build number rises with
/// every packaged IPA (scripts/bump-build.sh) and the revision is the git
/// commit the binary was compiled from (stamped by scripts/build-ios.sh).
enum AppVersion {
    static var marketing: String {
        string(for: "CFBundleShortVersionString") ?? "0.0.0"
    }

    static var build: String { string(for: "CFBundleVersion") ?? "0" }

    /// "unknown" for anything not built through scripts/build-ios.sh - see the
    /// BVN_BUILD_REVISION default in ios/project.yml.  Reported as-is rather
    /// than hidden, because a build whose provenance cannot be established is
    /// exactly the case worth seeing.
    static var revision: String { string(for: "BVNBuildRevision") ?? "unknown" }

    /// e.g. "0.1.0 (2) · 769e6334".  The revision is omitted when unknown
    /// rather than printed as the word "unknown" beside a real version.
    static var display: String {
        let base = "\(marketing) (\(build))"
        return revision == "unknown" || revision.isEmpty
            ? base
            : "\(base) · \(revision)"
    }

    private static func string(for key: String) -> String? {
        guard let value = Bundle.main.object(forInfoDictionaryKey: key) as? String,
              !value.isEmpty else {
            return nil
        }
        return value
    }
}

// MARK: - Storage

enum Storage {
    static var rootFilesystems: URL? { directory(BVNPathRootFilesystems()) }
    static var winePrefixes: URL? { directory(BVNPathWinePrefixes()) }

    /// The prefix the built-in tools - the file browser and Notepad - run in.
    ///
    /// Build 73 pointed *everything*, games included, at one shared prefix so
    /// that a file saved from Notepad was visible to the file browser. That is
    /// true of these two and worth keeping, but a game's saves live inside its
    /// prefix under drive_c/users, so moving games onto a new prefix hid every
    /// save the player had ever made. Games keep their own prefix - the one
    /// their saves are already in - and only the tools share.
    static var toolsWinePrefix: URL? {
        winePrefixes?.appendingPathComponent(toolsWinePrefixName)
    }

    /// Files deliberately shared by all otherwise-isolated Wine prefixes.
    /// Mounted as E: in games, Winefile and Notepad.
    static var sharedFiles: URL? {
        guard let documents else { return nil }
        let shared = documents.appendingPathComponent("Shared", isDirectory: true)
        do {
            try FileManager.default.createDirectory(
                at: shared, withIntermediateDirectories: true)
            return shared
        } catch {
            Log.write("Could not create shared files directory: \(error)",
                      category: "storage", level: BVNLogLevelError)
            return nil
        }
    }

    /// Deliberately the name build 72 and earlier gave desktop mode, so the
    /// file browser opens on the prefix it has always used.
    static let toolsWinePrefixName = "desktop"

    /// Build 73's shared prefix. Nothing launches into it any more, but a
    /// player who saved during build 73 has that progress here and not in the
    /// game's own prefix, so it is never deleted.
    static let build73SharedWinePrefixName = "shared"
    static var games: URL? { directory(BVNPathGames()) }
    static var logs: URL? { directory(BVNPathLogs()) }
    static var caches: URL? { directory(BVNPathCaches()) }

    /// The root filesystem archive shipped inside the app bundle, if the build
    /// included one.  A development build made without it returns nil and the
    /// user is asked to import an archive instead.
    static var bundledRootFilesystem: URL? {
        directory(BVNPathBundledRootFilesystemZip())
    }

    /// The root filesystem archive BoxedVN will actually use. A deliberately
    /// bundled runtime is authoritative for that build; otherwise use the
    /// archive imported by the user. This ordering is important for runtime
    /// migrations: an older imported Wine 10 archive must not silently mask a
    /// Wine 11 archive included to fix a known guest compatibility failure.
    static var activeRootFilesystem: URL? {
        if let bundled = bundledRootFilesystem { return bundled }
        return importedRootFilesystem
    }

    static var importedRootFilesystem: URL? {
        guard let root = rootFilesystems else { return nil }
        let candidate = root.appendingPathComponent("boxedwine.zip")
        return FileManager.default.fileExists(atPath: candidate.path) ? candidate : nil
    }

    /// The app's own Documents folder. Info.plist sets UIFileSharingEnabled
    /// and LSSupportsOpeningDocumentsInPlace, so this shows up in the Files
    /// app under "On My iPhone > BoxedVN" and the user can copy files
    /// straight into it.
    static var documents: URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
    }

    /// ZIP archives sitting directly in the app's Documents folder.
    ///
    /// This exists as a way to get a root filesystem (or a game) in without
    /// going through UIDocumentPickerViewController at all. The picker is an
    /// out-of-process remote view controller and has proven unreliable in
    /// this app on at least one physical device - rows highlight on tap but
    /// the selection never completes - so relying on it as the only import
    /// route is a single point of failure. Copying a file into the app's own
    /// Documents folder in Files needs no picker, no security-scoped URL and
    /// no cross-process handshake: the file is simply already inside the
    /// sandbox.
    static func documentsZipCandidates() -> [URL] {
        guard let documents else { return [] }
        let entries = (try? FileManager.default.contentsOfDirectory(
            at: documents,
            includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey],
            options: [.skipsHiddenFiles])) ?? []

        return entries
            .filter { $0.pathExtension.lowercased() == "zip" }
            .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }
    }

    private static func directory(_ pointer: UnsafePointer<CChar>?) -> URL? {
        guard let pointer else { return nil }
        return URL(fileURLWithPath: String(cString: pointer))
    }
}

// MARK: - Executable inspection

struct ExecutableDescription: Identifiable, Hashable {
    var id: String { relativePath }
    var relativePath: String
    var architecture: String
    var format: String
    var backend: String
    var runnable: Bool
    var diagnostic: String
    var subsystem: UInt16

    var subsystemName: String {
        switch subsystem {
        case 2: return "Windows GUI"
        case 3: return "Windows console"
        default: return "unknown"
        }
    }
}

private func architectureName(_ value: BVNGuestArchitecture) -> String {
    switch value {
    case BVNGuestArchitectureX86_16: return "x86 16-bit"
    case BVNGuestArchitectureX86_32: return "x86 32-bit"
    case BVNGuestArchitectureX86_64: return "x86-64"
    default: return "unknown"
    }
}

private func describe(_ info: BVNExecutableInfo, relativePath: String)
    -> ExecutableDescription {
    var info = info
    let format = withUnsafePointer(to: &info.format) {
        $0.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_SHORT)) {
            String(cString: $0)
        }
    }
    let backend = withUnsafePointer(to: &info.backend) {
        $0.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_SHORT)) {
            String(cString: $0)
        }
    }
    let diagnostic = withUnsafePointer(to: &info.diagnostic) {
        $0.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_DIAGNOSTIC)) {
            String(cString: $0)
        }
    }
    return ExecutableDescription(
        relativePath: relativePath,
        architecture: architectureName(info.architecture),
        format: format,
        backend: backend,
        runnable: info.runnable,
        diagnostic: diagnostic,
        subsystem: info.subsystem
    )
}

enum Executables {
    /// Inspects a single file.
    static func inspect(at url: URL) -> ExecutableDescription {
        var info = BVNExecutableInfo()
        BVNInspectExecutable(url.path, &info)
        return describe(info, relativePath: url.lastPathComponent)
    }

    /// Scans a directory recursively.  Blocking; call off the main thread.
    static func discover(in directory: URL) -> [ExecutableDescription] {
        let capacity = 256
        var buffer = [BVNDiscoveredExecutable](
            repeating: BVNDiscoveredExecutable(), count: capacity)
        let total = BVNDiscoverExecutables(directory.path, &buffer, capacity)
        let count = min(Int(total), capacity)

        return (0..<count).map { index in
            var entry = buffer[index]
            let path = withUnsafePointer(to: &entry.relativePath) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_PATH)) {
                    String(cString: $0)
                }
            }
            return describe(entry.info, relativePath: path)
        }
    }

    static func unsupportedMessage(for architecture: String) -> String {
        var buffer = [CChar](repeating: 0, count: Int(BVN_MAX_DIAGNOSTIC))
        let value: BVNGuestArchitecture =
            architecture == "x86-64" ? BVNGuestArchitectureX86_64
                                     : BVNGuestArchitectureUnknown
        BVNUnsupportedArchitectureMessage(value, &buffer, buffer.count)
        return String(cString: buffer)
    }
}

// MARK: - Backends

struct BackendDescription: Identifiable {
    var id: String { identifier }
    var identifier: String
    var implemented: Bool
    var requiresJIT: Bool
    var hasInterpreterFallback: Bool
    var architectures: [String]
}

enum Backends {
    static func all() -> [BackendDescription] {
        let capacity = 8
        var buffer = [BVNBackendDescription](
            repeating: BVNBackendDescription(), count: capacity)
        let total = BVNCopyBackendDescriptions(&buffer, capacity)
        let count = min(Int(total), capacity)

        return (0..<count).map { index in
            var entry = buffer[index]
            let identifier = withUnsafePointer(to: &entry.identifier) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(BVN_MAX_SHORT)) {
                    String(cString: $0)
                }
            }
            var architectures: [String] = []
            if entry.runsX86_16 { architectures.append("x86 16-bit") }
            if entry.runsX86_32 { architectures.append("x86 32-bit") }
            if entry.runsX86_64 { architectures.append("x86-64") }
            return BackendDescription(
                identifier: identifier,
                implemented: entry.implemented,
                requiresJIT: entry.requiresJIT,
                hasInterpreterFallback: entry.hasInterpreterFallback,
                architectures: architectures
            )
        }
    }
}

// MARK: - Session control

enum RuntimeState: String {
    case idle, starting, running, stopping, stopped, failed, unknown

    static var current: RuntimeState {
        RuntimeState(rawValue: String(cString: BVNRuntimeStateName(BVNRuntimeGetState())))
            ?? .unknown
    }
}

struct LaunchFailure: LocalizedError {
    var message: String
    var errorDescription: String? { message }
}

enum Session {
    /// Starts a guest.  Throws with the runtime's own explanation when the
    /// request is refused; never reports a generic failure.
    static func launch(
        rootFilesystem: URL,
        writableRoot: URL,
        gameDirectory: URL?,
        sharedDirectory: URL?,
        executablePath: String,
        arguments: [String],
        environment: [String],
        workingDirectory: String?,
        width: UInt32,
        height: UInt32,
        soundEnabled: Bool,
        runThroughWine: Bool
    ) throws {
        var errorBuffer = [CChar](repeating: 0, count: 1024)

        // Every C string must outlive the call, so the buffers are held here
        // and only pointers are handed across.
        let argumentStorage = arguments.map { strdup($0) }
        let environmentStorage = environment.map { strdup($0) }
        defer {
            argumentStorage.forEach { free($0) }
            environmentStorage.forEach { free($0) }
        }

        // Immutable buffers, because BVNLaunchRequest's fields are
        // `const char* const*` and Swift will not narrow a mutable pointer on
        // assignment to a struct field the way it does for call arguments.
        let argumentPointers = argumentStorage.map { UnsafePointer<CChar>($0) }
        let environmentPointers = environmentStorage.map { UnsafePointer<CChar>($0) }

        let accepted = rootFilesystem.path.withCString { rootPath in
            writableRoot.path.withCString { writablePath in
                executablePath.withCString { exePath in
                    withOptionalCString(gameDirectory?.path) { gamePath in
                        withOptionalCString(sharedDirectory?.path) { sharedPath in
                          withOptionalCString(workingDirectory) { workPath in
                            var request = BVNLaunchRequest()
                            request.rootFilesystemZipPath = rootPath
                            request.writableRootPath = writablePath
                            request.gameDirectoryHostPath = gamePath
                            request.sharedDirectoryHostPath = sharedPath
                            request.executablePath = exePath
                            request.workingDirectory = workPath
                            request.width = width
                            request.height = height
                            request.bitsPerPixel = 32
                            request.soundEnabled = soundEnabled
                            request.runThroughWine = runThroughWine
                            request.argumentCount = argumentPointers.count
                            request.environmentCount = environmentPointers.count

                            return argumentPointers.withUnsafeBufferPointer { args in
                                environmentPointers.withUnsafeBufferPointer { env in
                                    request.arguments = args.baseAddress
                                    request.environment = env.baseAddress
                                    return BVNRuntimeRequestLaunch(
                                        &request, &errorBuffer, errorBuffer.count)
                                }
                            }
                          }
                        }
                    }
                }
            }
        }

        if !accepted {
            throw LaunchFailure(message: String(cString: errorBuffer))
        }
    }

    @discardableResult
    static func requestShutdown() -> Bool {
        BVNRuntimeRequestShutdown()
    }

    static var lastError: String {
        String(cString: BVNRuntimeLastError())
    }

    static var lastExitCode: Int32? {
        let code = BVNRuntimeLastExitCode()
        return code == Int32.min ? nil : code
    }

    static var boxedwineVersion: String {
        String(cString: BVNRuntimeBoxedwineVersion())
    }
}

private func withOptionalCString<Result>(
    _ value: String?,
    _ body: (UnsafePointer<CChar>?) -> Result
) -> Result {
    guard let value else { return body(nil) }
    return value.withCString { body($0) }
}

// MARK: - Logs

enum Log {
    static func write(_ message: String, category: String = "app",
                      level: BVNLogLevel = BVNLogLevelInfo) {
        BVNLogWrite(level, category, message)
    }

    static var currentFile: URL? {
        guard let pointer = BVNLogCurrentFilePath() else { return nil }
        return URL(fileURLWithPath: String(cString: pointer))
    }

    static var generation: UInt64 { BVNLogGeneration() }

    /// Session logging, persisted so it survives a relaunch. On by default:
    /// every diagnosis in this port has come from an exported log.
    static var isEnabled: Bool {
        get { BVNLogIsEnabled() }
        set {
            BVNLogSetEnabled(newValue)
            UserDefaults.standard.set(newValue, forKey: enabledDefaultsKey)
        }
    }

    static let enabledDefaultsKey = "BVNSessionLoggingEnabled"

    /// Applies the stored preference. Called once at startup, after the log
    /// file has been opened so that turning it off is itself recorded.
    static func applyStoredPreference() {
        guard UserDefaults.standard.object(forKey: enabledDefaultsKey) != nil
        else { return }
        BVNLogSetEnabled(UserDefaults.standard.bool(forKey: enabledDefaultsKey))
    }

    static func recentText() -> String {
        let needed = BVNLogCopyRecent(nil, 0)
        guard needed > 0 else { return "" }
        var buffer = [CChar](repeating: 0, count: needed + 1)
        _ = BVNLogCopyRecent(&buffer, buffer.count)
        return String(cString: buffer)
    }
}
