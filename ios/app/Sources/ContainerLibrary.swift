/*
 * BoxedVN - persistent Wine containers.
 * GPLv2; see license.txt.
 */

import Foundation

struct WineContainer: Codable, Identifiable, Hashable, Sendable {
    var id: String
    var name: String
    var windowsVersion: String
    var renderer: String
    var width: UInt32
    var height: UInt32
    var sharedDriveLetter: String
    var showWindowsPrograms: Bool
    var createdAt: Date

    init(id: String, name: String, windowsVersion: String = "win10",
         renderer: String = "automatic", width: UInt32 = 1280,
         height: UInt32 = 720, sharedDriveLetter: String = "e",
         showWindowsPrograms: Bool = false,
         createdAt: Date = Date()) {
        self.id = id
        self.name = name
        self.windowsVersion = windowsVersion
        self.renderer = renderer
        self.width = width
        self.height = height
        self.sharedDriveLetter = sharedDriveLetter
        self.showWindowsPrograms = showWindowsPrograms
        self.createdAt = createdAt
    }

    // Defaults make old container files forward-compatible as settings are
    // added. A missing new key must not make the entire container disappear.
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        name = try values.decodeIfPresent(String.self, forKey: .name) ?? id
        windowsVersion = try values.decodeIfPresent(
            String.self, forKey: .windowsVersion) ?? "win10"
        renderer = try values.decodeIfPresent(
            String.self, forKey: .renderer) ?? "automatic"
        width = try values.decodeIfPresent(UInt32.self, forKey: .width) ?? 1280
        height = try values.decodeIfPresent(UInt32.self, forKey: .height) ?? 720
        sharedDriveLetter = try values.decodeIfPresent(
            String.self, forKey: .sharedDriveLetter) ?? "e"
        showWindowsPrograms = try values.decodeIfPresent(
            Bool.self, forKey: .showWindowsPrograms) ?? false
        createdAt = try values.decodeIfPresent(
            Date.self, forKey: .createdAt) ?? Date()
    }

    var prefixName: String { "container-" + id }
}

struct ContainerProgram: Identifiable, Hashable {
    var id: String { drive + ":" + executable.relativePath }
    var drive: String
    var root: URL
    var executable: ExecutableDescription
}

/// A program on one of a container's drives, with the two paths a 64-bit
/// launch needs: the Windows path Wine reads, and the program's own folder as
/// a guest Linux path, which is what BoxedWine's `-w` takes. The device log
/// showed why the second one is not a Windows path: given one, the process
/// started with no valid current directory at all (`open(".") -> -2`).
struct ContainerX64Program: Identifiable, Hashable, Sendable {
    /// The guest drive letter the folder is mounted as: "d" for the
    /// container's Files folder, "c" for its 64-bit prefix drive C.
    var drive: String
    /// Path below that drive's root, "/"-separated, as discovery reports it.
    var relativePath: String
    /// Host location, for the compatibility scan the launch does.
    var root: URL
    var executable: ExecutableDescription

    var id: String { drive + ":" + relativePath }

    /// Just the file name, for a row that has to fit on a phone.
    var name: String {
        relativePath.split(separator: "/").last.map { String($0) }
            ?? relativePath
    }

    /// The folder the program sits in, as the user sees it: "D: · Folder".
    var location: String {
        let folder = relativePath.split(separator: "/").dropLast()
            .joined(separator: "\\")
        return drive.uppercased() + ":" + (folder.isEmpty ? "" : " · " + folder)
    }

    /// `D:\Folder\Program.exe` - Wine reads this one, so it stays a Windows
    /// path.
    var guestExecutablePath: String {
        drive.uppercased() + ":\\"
            + relativePath.replacingOccurrences(of: "/", with: "\\")
    }

    /// The program's own folder as a guest Linux directory, so a DLL beside
    /// the program and the data it opens by relative path both resolve.
    var guestWorkingDirectory: String {
        let mount = drive == "c" ? ContainerLibrary.x64GuestDriveCPath
                                 : ContainerLibrary.x64GuestDriveDPath
        let folder = relativePath.split(separator: "/").dropLast()
            .joined(separator: "/")
        return folder.isEmpty ? mount : mount + "/" + folder
    }

    /// The host directory the program is in, which is what the launch scans
    /// to pick a renderer and recognise an engine.
    var hostDirectory: URL {
        root.appendingPathComponent(relativePath).deletingLastPathComponent()
    }
}

enum ContainerLibraryError: LocalizedError {
    case storageUnavailable
    case invalidName

    var errorDescription: String? {
        switch self {
        case .storageUnavailable:
            return "BoxedVN could not create its Containers directory."
        case .invalidName:
            return "Enter a name for the container."
        }
    }
}

enum ContainerLibrary {
    static func load() -> [WineContainer] {
        guard let root = Storage.containers else { return [] }
        let entries = (try? FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles])) ?? []
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return entries.compactMap { directory in
            let url = directory.appendingPathComponent("container.json")
            guard let data = try? Data(contentsOf: url) else { return nil }
            do {
                return try decoder.decode(WineContainer.self, from: data)
            } catch {
                Log.write("Skipping container \(directory.lastPathComponent): \(error)",
                          category: "container", level: BVNLogLevelWarning)
                return nil
            }
        }.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    static func create(name: String) throws -> WineContainer {
        guard let root = Storage.containers else {
            throw ContainerLibraryError.storageUnavailable
        }
        let cleanName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanName.isEmpty else { throw ContainerLibraryError.invalidName }
        var buffer = [CChar](repeating: 0, count: Int(BVN_MAX_SHORT))
        BVNMakeIdentifier(cleanName, &buffer, buffer.count)
        let base = String(cString: buffer)
        var id = base
        var suffix = 2
        while FileManager.default.fileExists(
            atPath: root.appendingPathComponent(id).path) {
            id = base + "-\(suffix)"
            suffix += 1
        }
        let container = WineContainer(id: id, name: cleanName)
        try FileManager.default.createDirectory(
            at: filesDirectory(for: container), withIntermediateDirectories: true)
        try save(container)
        return container
    }

    static func save(_ container: WineContainer) throws {
        let directory = try containerDirectory(for: container)
        try FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(container).write(
            to: directory.appendingPathComponent("container.json"),
            options: .atomic)
    }

    static func delete(_ container: WineContainer) throws {
        for shortcut in GameLibrary.load() where
            shortcut.winePrefix == container.prefixName {
            try? GameLibrary.delete(shortcut)
        }
        if let prefixes = Storage.winePrefixes {
            let prefix = prefixes.appendingPathComponent(container.prefixName)
            if FileManager.default.fileExists(atPath: prefix.path) {
                try FileManager.default.removeItem(at: prefix)
            }
        }
        let directory = try containerDirectory(for: container)
        if FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
    }

    static func filesDirectory(for container: WineContainer) -> URL {
        (try? containerDirectory(for: container))?
            .appendingPathComponent("Files", isDirectory: true)
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
    }

    static func prefixRoot(for container: WineContainer) -> URL? {
        Storage.winePrefixes?.appendingPathComponent(
            container.prefixName, isDirectory: true)
    }

    static func programs(in container: WineContainer) -> [ContainerProgram] {
        var programs = Executables.discover(in: filesDirectory(for: container))
            .map { ContainerProgram(drive: "d", root: filesDirectory(for: container),
                                    executable: $0) }
        if let prefix = prefixRoot(for: container) {
            let driveC = prefix
                .appendingPathComponent("home/username/.wine/drive_c",
                                        isDirectory: true)
            if FileManager.default.fileExists(atPath: driveC.path) {
                programs += Executables.discover(in: driveC)
                    .filter { executable in
                        if container.showWindowsPrograms { return true }
                        let path = executable.relativePath
                            .replacingOccurrences(of: "\\", with: "/")
                            .lowercased()
                        return path != "windows" && !path.hasPrefix("windows/")
                    }
                    .map { ContainerProgram(drive: "c", root: driveC,
                                            executable: $0) }
            }
        }
        return programs.filter(\.executable.runnable).sorted {
            $0.executable.relativePath.localizedCaseInsensitiveCompare(
                $1.executable.relativePath) == .orderedAscending
        }
    }

    /// Where the container's Files folder is mounted inside the guest. The
    /// launch passes it as drive D:, and BoxedWine mounts a lettered drive at
    /// `/mnt/drive_<letter>` (see startupArgs.cpp).
    static let x64GuestDriveDPath = "/mnt/drive_d"

    /// Where the 64-bit prefix's drive C is mounted: the prefix path from
    /// guest_wine_prefix.h, which the launch mounts `x64DriveC` over.
    static let x64GuestDriveCPath = "/home/username/.wine64/drive_c"

    /// The 64-bit lane's C: drive. It sits beside the container's Files
    /// folder rather than inside the private writable root so that Program
    /// Files and the user directories are reachable from the Files app; the
    /// 64-bit prefix is kept apart from the 32-bit one because the two Wine
    /// builds cannot share a prefix.
    static func x64DriveC(for container: WineContainer) -> URL {
        filesDirectory(for: container).deletingLastPathComponent()
            .appendingPathComponent("Drive C (64-bit)", isDirectory: true)
    }

    // MARK: - The "Run program…" scan

    /// How deep below a drive root the scan looks. A program installed by a
    /// Windows installer sits three or four folders down
    /// (`Program Files/Vendor/Product/bin`); nothing that far past that is a
    /// program the user is looking for, and the folders that go deeper -
    /// asset trees, package caches - are where the time went.
    static let x64ScanMaximumDepth = 8

    /// Folder names never worth descending into, matched case-insensitively
    /// against a single path component. These are Windows' own or the
    /// installer machinery's: none of them holds a program the user chose to
    /// put on the drive, and between them they hold most of the files.
    static let x64ScanSkippedFolders: Set<String> = [
        "winsxs", "installer", "assembly", "servicing", "softwaredistribution",
        "package cache", "packages", "$recycle.bin",
        "system volume information", "temp", "tmp", "catroot", "catroot2",
        "driverstore", "logfiles", "prefetch", "symbols",
    ]

    /// One update from a running scan. Results arrive as they are found: the
    /// first program is on screen long before the last folder is read, which
    /// is the difference between a picker that works and one the user waits
    /// out.
    struct X64ScanProgress: Sendable {
        /// Everything found so far, in the order the picker shows it.
        var programs: [ContainerX64Program]
        /// Folders read so far, for the progress line.
        var foldersScanned: Int
        var finished: Bool
        /// True when this arrived from the cache rather than from a walk.
        var fromCache: Bool
    }

    /// Every Windows program on the container's two 64-bit drives, delivered
    /// incrementally, for the "Run program…" picker.
    ///
    /// Only `.exe` files: the picker starts a process, and the loader is what
    /// decides whether a given image runs. Both architectures are listed -
    /// the 64-bit prefix has a 32-bit lane when the PE32 layer is present -
    /// and the row says which one each is, so a program that cannot start
    /// says so before it is picked rather than after.
    ///
    /// BoxedVN's own staging directory is hidden: the DXMT modules and the
    /// bundled probes live there, and neither is a program the user put on
    /// the drive.
    ///
    /// The walk happens on a detached task; nothing here touches the main
    /// thread. Cancelling the stream (dismissing the sheet) stops the walk at
    /// the next folder.
    static func x64ProgramScan(for container: WineContainer)
        -> AsyncStream<X64ScanProgress> {
        AsyncStream<X64ScanProgress> { continuation in
            let task = Task.detached(priority: .userInitiated) {
                if let cached = cachedX64Programs(for: container) {
                    continuation.yield(X64ScanProgress(
                        programs: cached, foldersScanned: 0, finished: true,
                        fromCache: true))
                    continuation.finish()
                    return
                }
                var found: [ContainerX64Program] = []
                var visited: [String: Date] = [:]
                var folders = 0
                var lastYield = Date.distantPast

                let roots: [(URL, String)] = [
                    (filesDirectory(for: container), "d"),
                    (x64DriveC(for: container), "c"),
                ]
                for (root, drive) in roots {
                    guard FileManager.default.fileExists(atPath: root.path)
                    else { continue }
                    folders += walkX64Programs(
                        root: root, drive: drive,
                        showWindowsPrograms: container.showWindowsPrograms,
                        visited: &visited) { program, folderCount in
                            found.append(program)
                            // Batched by time, not by count: a burst of
                            // twenty programs in one folder is one update,
                            // and a slow tree still refreshes twice a second.
                            let now = Date()
                            guard now.timeIntervalSince(lastYield) > 0.4 else {
                                return
                            }
                            lastYield = now
                            continuation.yield(X64ScanProgress(
                                programs: sortedX64Programs(found),
                                foldersScanned: folderCount, finished: false,
                                fromCache: false))
                        }
                    if Task.isCancelled { break }
                }
                if Task.isCancelled {
                    continuation.finish()
                    return
                }
                let programs = sortedX64Programs(found)
                storeX64Programs(programs, directories: visited,
                                 for: container)
                continuation.yield(X64ScanProgress(
                    programs: programs, foldersScanned: folders,
                    finished: true, fromCache: false))
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    /// Discards the remembered scan for one container, so the picker's
    /// "Rescan" really does.
    static func forgetX64ProgramCache(for container: WineContainer) {
        x64CacheLock.lock()
        x64Cache.removeValue(forKey: x64CacheKey(for: container))
        x64CacheLock.unlock()
    }

    /// The container, and the one setting that changes what a scan finds.
    /// Turning "Show Windows system programs" on has to produce a new scan
    /// rather than hand back the previous one's results.
    private static func x64CacheKey(for container: WineContainer) -> String {
        container.id + (container.showWindowsPrograms ? "|windows" : "")
    }

    private static func sortedX64Programs(_ programs: [ContainerX64Program])
        -> [ContainerX64Program] {
        programs.sorted {
            // Anything that can start comes first; the rest stay visible,
            // because "my program is not in the list" is worse than a row
            // that explains itself.
            if $0.executable.runnable != $1.executable.runnable {
                return $0.executable.runnable
            }
            return $0.id.localizedCaseInsensitiveCompare($1.id)
                == .orderedAscending
        }
    }

    /// Reads one drive tree, calling `found` for every program as it is
    /// inspected and recording the modification date of every folder it
    /// entered, which is what tells a later scan whether anything changed.
    /// Returns the number of folders read.
    @discardableResult
    private static func walkX64Programs(
        root: URL, drive: String, showWindowsPrograms: Bool,
        visited: inout [String: Date],
        found: (ContainerX64Program, Int) -> Void
    ) -> Int {
        let manager = FileManager.default
        let keys: [URLResourceKey] = [.isDirectoryKey]
        var queue: [(URL, String, Int)] = [(root, "", 0)]
        // A Wine drive_c is full of symbolic links, several of which point
        // back up the tree. Entering a folder once is both the loop guard and
        // the reason the same file is never inspected twice.
        var entered: Set<String> = [root.resolvingSymlinksInPath().path]
        var folders = 0
        while !queue.isEmpty {
            if Task.isCancelled { return folders }
            let (directory, relative, depth) = queue.removeFirst()
            guard let entries = try? manager.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: keys,
                options: [.skipsHiddenFiles]) else { continue }
            folders += 1
            // Only a folder whose date could actually be read is remembered:
            // a placeholder date would make every later scan believe the tree
            // had changed, which is the cache never being used at all.
            if let modified = (try? directory.resourceValues(
                forKeys: [.contentModificationDateKey]))?
                .contentModificationDate {
                visited[directory.path] = modified
            }

            for entry in entries {
                let name = entry.lastPathComponent
                // A dot-directory is BoxedVN's own or the system's.
                if name.hasPrefix(".") { continue }
                let entryRelative = relative.isEmpty ? name
                                                     : relative + "/" + name
                var isDirectory = false
                if let resources = try? entry.resourceValues(forKeys: Set(keys)),
                   let flag = resources.isDirectory {
                    isDirectory = flag
                }
                if isDirectory {
                    guard depth < x64ScanMaximumDepth,
                          shouldEnterX64ScanFolder(
                            entryRelative, drive: drive,
                            showWindowsPrograms: showWindowsPrograms),
                          entered.insert(
                            entry.resolvingSymlinksInPath().path).inserted
                    else { continue }
                    queue.append((entry, entryRelative, depth + 1))
                    continue
                }
                guard name.lowercased().hasSuffix(".exe") else { continue }
                found(ContainerX64Program(
                    drive: drive, relativePath: entryRelative, root: root,
                    executable: Executables.inspect(
                        at: entry, relativePath: entryRelative)),
                      folders)
            }
        }
        return folders
    }

    /// Whether a folder at `relativePath` below a drive root is worth reading.
    private static func shouldEnterX64ScanFolder(
        _ relativePath: String, drive: String, showWindowsPrograms: Bool
    ) -> Bool {
        let lower = relativePath.lowercased()
        guard let name = lower.split(separator: "/").last else { return false }
        if x64ScanSkippedFolders.contains(String(name)) { return false }
        // Wine's shared-component and redistributable trees hold hundreds of
        // DLLs and no program anyone launches.
        if lower.hasPrefix("program files/common files")
            || lower.hasPrefix("program files (x86)/common files") {
            return false
        }
        if drive == "c" && !showWindowsPrograms {
            if lower == "windows" || lower.hasPrefix("windows/") {
                return false
            }
        }
        return true
    }

    // MARK: - Remembering a scan

    private struct X64ProgramCacheEntry {
        var programs: [ContainerX64Program]
        /// Every folder the scan entered, with the modification date it had.
        /// A file added, removed or renamed anywhere in the tree changes the
        /// date of the folder it is in, so re-reading these dates - one stat
        /// each, no directory listing - is a complete check for "did anything
        /// change" at a fraction of the cost of the scan itself.
        var directories: [String: Date]
    }

    private static let x64CacheLock = NSLock()
    private static var x64Cache: [String: X64ProgramCacheEntry] = [:]

    private static func cachedX64Programs(for container: WineContainer)
        -> [ContainerX64Program]? {
        x64CacheLock.lock()
        let entry = x64Cache[x64CacheKey(for: container)]
        x64CacheLock.unlock()
        guard let entry, !entry.directories.isEmpty else { return nil }
        for (path, recorded) in entry.directories {
            let url = URL(fileURLWithPath: path)
            guard let values = try? url.resourceValues(
                    forKeys: [.contentModificationDateKey]),
                  let current = values.contentModificationDate,
                  abs(current.timeIntervalSince(recorded)) < 0.5 else {
                return nil
            }
        }
        return entry.programs
    }

    private static func storeX64Programs(_ programs: [ContainerX64Program],
                                         directories: [String: Date],
                                         for container: WineContainer) {
        x64CacheLock.lock()
        x64Cache[x64CacheKey(for: container)] = X64ProgramCacheEntry(
            programs: programs, directories: directories)
        x64CacheLock.unlock()
    }

    private static func containerDirectory(for container: WineContainer) throws
        -> URL {
        guard let root = Storage.containers else {
            throw ContainerLibraryError.storageUnavailable
        }
        return root.appendingPathComponent(container.id, isDirectory: true)
    }
}
