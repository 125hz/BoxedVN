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
    var createdAt: Date

    init(id: String, name: String, windowsVersion: String = "win10",
         renderer: String = "automatic", width: UInt32 = 800,
         height: UInt32 = 600, sharedDriveLetter: String = "e",
         createdAt: Date = Date()) {
        self.id = id
        self.name = name
        self.windowsVersion = windowsVersion
        self.renderer = renderer
        self.width = width
        self.height = height
        self.sharedDriveLetter = sharedDriveLetter
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
        width = try values.decodeIfPresent(UInt32.self, forKey: .width) ?? 800
        height = try values.decodeIfPresent(UInt32.self, forKey: .height) ?? 600
        sharedDriveLetter = try values.decodeIfPresent(
            String.self, forKey: .sharedDriveLetter) ?? "e"
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
                    .map { ContainerProgram(drive: "c", root: driveC,
                                            executable: $0) }
            }
        }
        return programs.filter(\.executable.runnable).sorted {
            $0.executable.relativePath.localizedCaseInsensitiveCompare(
                $1.executable.relativePath) == .orderedAscending
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
