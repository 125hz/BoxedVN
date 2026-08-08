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

// MARK: - Storage

enum Storage {
    static var rootFilesystems: URL? { directory(BVNPathRootFilesystems()) }
    static var winePrefixes: URL? { directory(BVNPathWinePrefixes()) }
    static var games: URL? { directory(BVNPathGames()) }
    static var logs: URL? { directory(BVNPathLogs()) }
    static var caches: URL? { directory(BVNPathCaches()) }

    /// The root filesystem archive shipped inside the app bundle, if the build
    /// included one.  A development build made without it returns nil and the
    /// user is asked to import an archive instead.
    static var bundledRootFilesystem: URL? {
        directory(BVNPathBundledRootFilesystemZip())
    }

    /// The root filesystem archive BoxedVN will actually use: an imported one
    /// if present, otherwise the bundled one.
    static var activeRootFilesystem: URL? {
        if let imported = importedRootFilesystem { return imported }
        return bundledRootFilesystem
    }

    static var importedRootFilesystem: URL? {
        guard let root = rootFilesystems else { return nil }
        let candidate = root.appendingPathComponent("boxedwine.zip")
        return FileManager.default.fileExists(atPath: candidate.path) ? candidate : nil
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
                        withOptionalCString(workingDirectory) { workPath in
                            var request = BVNLaunchRequest()
                            request.rootFilesystemZipPath = rootPath
                            request.writableRootPath = writablePath
                            request.gameDirectoryHostPath = gamePath
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

    static func recentText() -> String {
        let needed = BVNLogCopyRecent(nil, 0)
        guard needed > 0 else { return "" }
        var buffer = [CChar](repeating: 0, count: needed + 1)
        _ = BVNLogCopyRecent(&buffer, buffer.count)
        return String(cString: buffer)
    }
}
