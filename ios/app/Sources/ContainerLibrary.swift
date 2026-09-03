/*
 * BoxedVN - persistent Wine containers.
 * GPLv2; see license.txt.
 */

import Foundation

struct WineContainer: Codable, Identifiable, Hashable {
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
struct ContainerX64Program: Identifiable, Hashable {
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

    /// Every Windows program on the container's two 64-bit drives, newest
    /// scan each time, for the "Run program…" picker.
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
    static func x64Programs(in container: WineContainer) -> [ContainerX64Program] {
        let files = filesDirectory(for: container)
        var programs = discoverX64Programs(in: files, drive: "d")
        let driveC = x64DriveC(for: container)
        if FileManager.default.fileExists(atPath: driveC.path) {
            programs += discoverX64Programs(in: driveC, drive: "c").filter {
                if container.showWindowsPrograms { return true }
                let path = $0.relativePath.lowercased()
                return path != "windows" && !path.hasPrefix("windows/")
            }
        }
        return programs.sorted {
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

    private static func discoverX64Programs(in root: URL, drive: String)
        -> [ContainerX64Program] {
        Executables.discover(in: root)
            .filter { executable in
                let path = executable.relativePath
                    .replacingOccurrences(of: "\\", with: "/")
                guard path.lowercased().hasSuffix(".exe") else { return false }
                // A dot-directory is BoxedVN's own or the system's.
                return !path.split(separator: "/").contains { $0.hasPrefix(".") }
            }
            .map { executable in
                ContainerX64Program(
                    drive: drive,
                    relativePath: executable.relativePath
                        .replacingOccurrences(of: "\\", with: "/"),
                    root: root,
                    executable: executable)
            }
    }

    private static func containerDirectory(for container: WineContainer) throws
        -> URL {
        guard let root = Storage.containers else {
            throw ContainerLibraryError.storageUnavailable
        }
        return root.appendingPathComponent(container.id, isDirectory: true)
    }
}
