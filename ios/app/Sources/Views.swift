/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  The library UI.  Deliberately plain: until Wine Notepad and one visual
 *  novel actually run, effort spent here is effort not spent on the runtime.
 *  Every control below performs a real operation.
 */

import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// `[.zip]` alone leaves real ZIP files greyed out and unselectable in the
/// system picker for a meaningful fraction of real-world files - most often
/// ones that arrived via AirDrop, where the sending side's declared UTI
/// doesn't always resolve cleanly to `public.zip-archive` on the receiving
/// end (large files in particular). `.data` is the generic "some bytes"
/// UTType that essentially everything conforms to, so adding it as a second
/// allowed type makes the file selectable regardless of how its UTI got
/// tagged in transit. This is a selectability gate only, not a validator -
/// both call sites verify the picked file is actually a usable ZIP
/// afterwards (BVNZipInspect via GameLibrary.importGame, and directly in
/// AppModel.importRootFilesystem), so a non-ZIP selection is still caught
/// and reported clearly rather than silently accepted.
let zipImportContentTypes: [UTType] = [.zip, .data]
let windowsInstallerContentTypes: [UTType] = {
    guard let executable = UTType(filenameExtension: "exe") else {
        return [.data]
    }
    return [executable, .data]
}()

/// A UIKit import-mode picker rather than SwiftUI's `.fileImporter`.
///
/// `.fileImporter` opens the original URL and returns a security-scoped
/// reference. On the physical test device its Files row highlights but that
/// open-in-place hand-off never completes. `asCopy: true` asks Files to copy
/// the selection into BoxedVN's sandbox and uses an explicit delegate, which
/// is the modern equivalent of the old UIDocumentPickerModeImport contract.
struct DocumentImportPicker: UIViewControllerRepresentable {
    let contentTypes: [UTType]
    let onResult: (Result<URL, Error>) -> Void
    let onCancel: () -> Void

    init(contentTypes: [UTType],
         onResult: @escaping (Result<URL, Error>) -> Void,
         onCancel: @escaping () -> Void) {
        self.contentTypes = contentTypes
        self.onResult = onResult
        self.onCancel = onCancel
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(onResult: onResult, onCancel: onCancel)
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let identifiers = contentTypes.map(\.identifier).joined(separator: ", ")
        Log.write("Presenting UIKit import-copy picker for \(identifiers)",
                  category: "import")
        let picker = UIDocumentPickerViewController(
            forOpeningContentTypes: contentTypes,
            asCopy: true)
        picker.delegate = context.coordinator
        picker.allowsMultipleSelection = false
        picker.shouldShowFileExtensions = true
        return picker
    }

    func updateUIViewController(_ controller: UIDocumentPickerViewController,
                                context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onResult: (Result<URL, Error>) -> Void
        let onCancel: () -> Void

        init(onResult: @escaping (Result<URL, Error>) -> Void,
             onCancel: @escaping () -> Void) {
            self.onResult = onResult
            self.onCancel = onCancel
        }

        func documentPicker(_ controller: UIDocumentPickerViewController,
                            didPickDocumentsAt urls: [URL]) {
            guard let url = urls.first else {
                onResult(.failure(DocumentImportError.emptySelection))
                return
            }
            onResult(.success(url))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            Log.write("Document import picker cancelled", category: "import")
            onCancel()
        }
    }
}

enum DocumentImportError: LocalizedError {
    case emptySelection

    var errorDescription: String? {
        "The Files picker returned without a selected file."
    }
}

struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        NavigationStack {
            LibraryView()
                .navigationTitle("BoxedVN")
        }
        .alert("BoxedVN", isPresented: .constant(model.alertMessage != nil)) {
            Button("OK") { model.alertMessage = nil }
        } message: {
            Text(model.alertMessage ?? "")
        }
    }
}

// MARK: - Library

struct LibraryView: View {
    @EnvironmentObject private var model: AppModel
    @State private var showingContainerPrompt = false
    @State private var containerName = ""

    var body: some View {
        List {
            Section("Runtime") {
                NavigationLink {
                    StatusView()
                } label: {
                    LabeledContent("Status") {
                        HStack(spacing: 6) {
                            Circle()
                                .fill(model.jit.isUsable ? Color.green : Color.orange)
                                .frame(width: 8, height: 8)
                            Text(model.jit.isUsable ? "JIT ready" : "JIT unavailable")
                        }
                    }
                }
                LabeledContent("Memory") {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(model.memory.entitlement == .enabled
                                  ? Color.green : Color.secondary)
                            .frame(width: 8, height: 8)
                        Text(model.memory.statusText)
                    }
                }
                LabeledContent("Session", value: model.runtimeState.rawValue)
                // Deliberately on the first screen, not buried in an About
                // page: BoxedVN is sideloaded, so this is the only way to tell
                // whether the installed IPA is the newest build.
                LabeledContent("Version", value: AppVersion.display)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Section {
                ForEach(model.containers) { container in
                    NavigationLink {
                        ContainerDetailView(container: container)
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(container.name)
                            Text("\(container.windowsVersion) · "
                                 + "\(container.width)x\(container.height) · "
                                 + container.renderer)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                .onDelete { offsets in
                    for index in offsets {
                        model.deleteContainer(model.containers[index])
                    }
                }
                Button {
                    containerName = ""
                    showingContainerPrompt = true
                } label: {
                    Label("New container", systemImage: "plus.rectangle.on.folder")
                }
            } header: {
                Text("Containers")
            }

            Section {
                if model.games.isEmpty {
                    Text("No games imported yet.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(model.games, id: \.id) { game in
                        NavigationLink {
                            GameDetailView(game: game)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(game.title)
                                Text(game.hasRunnableExecutable
                                     ? game.selectedExecutable
                                     : "No runnable executable")
                                    .font(.caption)
                                    .foregroundStyle(game.hasRunnableExecutable
                                                     ? Color.secondary : Color.red)
                            }
                        }
                    }
                    .onDelete { offsets in
                        for index in offsets {
                            model.deleteGame(model.games[index])
                        }
                    }
                }
            } header: {
                Text("Games")
            } footer: {
                if model.isImporting {
                    HStack {
                        ProgressView()
                        Text(model.importProgressMessage)
                    }
                }
                if model.isInstallingGame {
                    HStack {
                        ProgressView()
                        Text(model.installerProgressMessage)
                    }
                }
            }

            Section {
                NavigationLink("Settings") { SettingsView() }
                NavigationLink("Logs") { LogView() }
                if model.runtimeState == .running {
                    Button("Quit running session", role: .destructive) {
                        model.requestShutdown()
                    }
                }
            }
        }
        .alert("New container", isPresented: $showingContainerPrompt) {
            TextField("Name", text: $containerName)
            Button("Create") { model.createContainer(named: containerName) }
            Button("Cancel", role: .cancel) {}
        }
    }

}

// MARK: - Container detail

struct ContainerDetailView: View {
    @EnvironmentObject private var model: AppModel
    @State private var container: WineContainer
    @State private var programs: [ContainerProgram] = []
    @State private var isScanning = false
    @State private var shortcutProgram: ContainerProgram?
    @State private var shortcutTitle = ""
    @State private var showingShortcutPrompt = false

    private let resolutions = ["640x480", "800x600", "1024x768",
                               "1280x720", "1280x960", "1366x1024",
                               "1600x1200", "1920x1080"]

    init(container: WineContainer) {
        _container = State(initialValue: container)
    }

    var body: some View {
        List {
            Section("Desktop") {
                Button {
                    save()
                    model.launchDesktop(container)
                } label: {
                    Label("Open desktop", systemImage: "macwindow")
                }
                .disabled(model.rootFilesystem == nil || sessionIsBusy)
                Picker("Resolution", selection: resolutionBinding) {
                    ForEach(resolutions, id: \.self) { Text($0) }
                    if !resolutions.contains(resolutionBinding.wrappedValue) {
                        Text(resolutionBinding.wrappedValue)
                            .tag(resolutionBinding.wrappedValue)
                    }
                }
                HStack {
                    TextField("Width", value: $container.width, format: .number)
                        .keyboardType(.numberPad)
                    Text("×").foregroundStyle(.secondary)
                    TextField("Height", value: $container.height, format: .number)
                        .keyboardType(.numberPad)
                }
                Picker("Windows version", selection: $container.windowsVersion) {
                    Text("Windows 10").tag("win10")
                    Text("Windows 7").tag("win7")
                    Text("Windows XP").tag("winxp")
                }
                Picker("Direct3D renderer", selection: $container.renderer) {
                    Text("Automatic").tag("automatic")
                    Text("WineD3D (Vulkan)").tag("wined3d")
                    Text("DXVK (Vulkan)").tag("dxvk")
                }
                Picker("Shared folder drive", selection: $container.sharedDriveLetter) {
                    ForEach(["e", "f", "g", "h", "i", "j", "k", "l", "m",
                             "n", "o", "p", "q", "r", "s", "t", "u", "v",
                             "w", "x", "y", "z"], id: \.self) { letter in
                        Text(letter.uppercased() + ":").tag(letter)
                    }
                }
                Button("Save settings") { save() }
            }

            Section("Programs") {
                if isScanning {
                    HStack { ProgressView(); Text("Scanning C: and D:…") }
                } else if programs.isEmpty {
                    Text("No compatible 32-bit programs found.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(programs) { program in
                        Button {
                            shortcutProgram = program
                            shortcutTitle = URL(fileURLWithPath:
                                program.executable.relativePath)
                                .deletingPathExtension().lastPathComponent
                            showingShortcutPrompt = true
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(program.executable.relativePath)
                                Text(program.drive.uppercased() + ": · Add to Home")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                Button("Scan again") { scan() }
                    .disabled(isScanning)
            }

            Section("Files") {
                LabeledContent("Container", value: "Containers/\(container.id)")
                LabeledContent("Import folder", value: "Files")
                LabeledContent("Container files", value: "D:")
                LabeledContent("Shared files",
                               value: container.sharedDriveLetter.uppercased() + ":")
            }
        }
        .navigationTitle(container.name)
        .task { scan() }
        .alert("Add shortcut", isPresented: $showingShortcutPrompt) {
            TextField("Title", text: $shortcutTitle)
            Button("Add to Home") {
                if let shortcutProgram {
                    model.addShortcut(shortcutProgram, from: container,
                                      title: shortcutTitle)
                }
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    private var sessionIsBusy: Bool {
        model.runtimeState == .starting || model.runtimeState == .running ||
        model.runtimeState == .stopping
    }

    private var resolutionBinding: Binding<String> {
        Binding(
            get: { "\(container.width)x\(container.height)" },
            set: { value in
                let pieces = value.split(separator: "x")
                if pieces.count == 2,
                   let width = UInt32(pieces[0]), let height = UInt32(pieces[1]) {
                    container.width = width
                    container.height = height
                }
            })
    }

    private func save() {
        container.width = min(max(container.width, 320), 3840)
        container.height = min(max(container.height, 240), 2160)
        model.updateContainer(container)
    }

    private func scan() {
        guard !isScanning else { return }
        isScanning = true
        let current = container
        Task {
            programs = await Task.detached(priority: .userInitiated) {
                ContainerLibrary.programs(in: current)
            }.value
            isScanning = false
        }
    }
}

// MARK: - Game detail

struct GameDetailView: View {
    @EnvironmentObject private var model: AppModel
    let game: Game

    @State private var executables: [ExecutableDescription] = []
    @State private var selected: String = ""
    @State private var argumentsText = ""
    @State private var environmentText = ""
    @State private var workingDirectory = ""
    @State private var resolution = "default"
    @State private var renderer = "automatic"
    @State private var saveError: String?

    private let resolutions = ["default", "640x480", "800x600", "1024x576",
                               "1280x720", "1280x960", "1366x1024",
                               "1600x1200", "1920x1080"]
    private let renderers = ["automatic", "wined3d", "dxvk"]

    var body: some View {
        List {
            Section("Executable") {
                if executables.isEmpty {
                    // Name the program the manifest already chose rather than
                    // showing only "Scanning…". Launch works during the scan,
                    // so the row has to say what it would launch.
                    if !selected.isEmpty {
                        LabeledContent("Program", value: selected)
                    }
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Looking for other programs…")
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Picker("Program", selection: $selected) {
                        ForEach(executables) { executable in
                            Text(executable.relativePath)
                                .tag(executable.relativePath)
                        }
                    }
                    if let current = executables.first(where: {
                        $0.relativePath == selected
                    }) {
                        LabeledContent("Architecture", value: current.architecture)
                        LabeledContent("Format", value: current.format)
                        LabeledContent("Subsystem", value: current.subsystemName)
                        LabeledContent("Backend", value: current.backend)
                        Text(current.diagnostic)
                            .font(.caption)
                            .foregroundStyle(current.runnable ? Color.secondary : Color.red)
                    }
                }
            }

            Section("Launch settings") {
                TextField("Arguments (one per line)", text: $argumentsText,
                          axis: .vertical)
                    .lineLimit(1...4)
                if !misplacedEnvironmentLines.isEmpty {
                    Text("\(misplacedEnvironmentLines.joined(separator: ", ")) "
                         + "looks like an environment variable. Arguments are "
                         + "passed to the program, so this would have no "
                         + "effect. Move it to Environment below.")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
                TextField("Working directory (inside the game)",
                          text: $workingDirectory)
                TextField("Environment (NAME=VALUE, one per line)",
                          text: $environmentText, axis: .vertical)
                    .lineLimit(1...4)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                Picker("Virtual desktop", selection: $resolution) {
                    ForEach(resolutions, id: \.self) { Text($0) }
                }
                Picker("Direct3D renderer", selection: $renderer) {
                    Text("Automatic (recommended)").tag("automatic")
                    Text("WineD3D Vulkan (D3D8/9)").tag("wined3d")
                    Text("DXVK Vulkan (D3D10/11)").tag("dxvk")
                }
                Button("Save") { save() }
                if let saveError {
                    Text(saveError).foregroundStyle(.red).font(.caption)
                }
            }

            Section {
                Button("Launch") {
                    if save(),
                       let updated = model.games.first(where: { $0.id == game.id }) {
                        model.launch(updated)
                    }
                }
                    .disabled(!canLaunchSelection)
            } footer: {
                if !model.jit.isUsable {
                    Text("JIT is not available, so launching will fail. "
                         + model.jit.detail)
                        .foregroundStyle(.orange)
                }
            }

            Section("Storage") {
                LabeledContent("Content", value: game.contentDirectory.lastPathComponent)
                LabeledContent("Wine prefix", value: game.winePrefix)
                LabeledContent("Imported", value: game.importedAt.formatted(date: .abbreviated,
                                                                            time: .shortened))
            }
        }
        .navigationTitle(game.title)
        .task {
            selected = game.selectedExecutable
            workingDirectory = game.workingDirectory
            argumentsText = GameLibrary.arguments(for: game).joined(separator: "\n")
            environmentText =
                GameLibrary.environment(for: game).joined(separator: "\n")
            renderer = renderers.contains(game.renderer)
                ? game.renderer : "automatic"
            if game.requestedWidth > 0 && game.requestedHeight > 0 {
                resolution = "\(game.requestedWidth)x\(game.requestedHeight)"
            }
            let directory = game.contentDirectory
            executables = await Task.detached {
                Executables.discover(in: directory)
            }.value
            if selected.isEmpty {
                selected = executables.first(where: { $0.runnable })?.relativePath
                    ?? executables.first?.relativePath ?? ""
            }
        }
    }

    /// Whether Launch can act on the current selection.
    ///
    /// Opening a game re-scans its whole content directory and inspects every
    /// executable it finds, which for a game shipping tens of thousands of
    /// asset files takes long enough to notice. While that ran, `executables`
    /// was empty, so the old condition - "is the selected entry marked
    /// runnable" - was false and Launch sat greyed out for seconds every
    /// single time, including for a game the user has already played.
    ///
    /// The scan exists so the user can *change* which program runs. It is not
    /// what makes the current one launchable: import already inspected that
    /// executable and wrote its verdict into the manifest, and the manifest's
    /// choice is exactly what Launch is about to use. So the button waits for
    /// the scan only when the selection has been changed to something the
    /// scan has not judged yet.
    private var canLaunchSelection: Bool {
        if selected.isEmpty {
            return false
        }
        guard let scanned = executables.first(where: {
            $0.relativePath == selected
        }) else {
            return selected == game.selectedExecutable
        }
        return scanned.runnable
    }

    /// Argument lines that are really environment assignments. A Windows
    /// program argument does not look like `NAME=VALUE` with no leading dash
    /// or slash, and an entry typed here instead of in Environment reaches the
    /// game as a command-line word it ignores - silently, which is how a
    /// diagnostic run gets thrown away.
    private var misplacedEnvironmentLines: [String] {
        argumentsText
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { line in
                guard let equals = line.firstIndex(of: "="),
                      equals != line.startIndex,
                      !line.hasPrefix("-"), !line.hasPrefix("/") else {
                    return false
                }
                let name = line[line.startIndex..<equals]
                return name.allSatisfy {
                    $0.isLetter || $0.isNumber || $0 == "_"
                } && (name.first?.isNumber == false)
            }
            .map { String($0.prefix(while: { $0 != "=" })) }
    }

    @discardableResult
    private func save() -> Bool {
        let arguments = argumentsText
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        // An entry without '=' is not an assignment. The manifest writer drops
        // it, so dropping it here too keeps the field showing what was saved.
        let environment = environmentText
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { $0.contains("=") }

        var width: UInt32 = 0
        var height: UInt32 = 0
        if resolution != "default" {
            let parts = resolution.split(separator: "x")
            if parts.count == 2 {
                width = UInt32(parts[0]) ?? 0
                height = UInt32(parts[1]) ?? 0
            }
        }

        do {
            try GameLibrary.updateLaunchSettings(
                for: game, selectedExecutable: selected,
                workingDirectory: workingDirectory, renderer: renderer,
                arguments: arguments, environment: environment,
                width: width, height: height)
            saveError = nil
            model.reloadGames()
            return true
        } catch {
            saveError = error.localizedDescription
            return false
        }
    }
}

// MARK: - Status

/// Status text/colour and the JIT section body are factored out into their
/// own small helpers/view: a single large `body` mixing this many
/// conditionals made the Swift type-checker time out in Release builds
/// ("unable to type-check this expression in reasonable time").
private func jitStatusText(_ jit: JITReport) -> String {
    jit.debuggerAttached ? "Confirmed" : "Unavailable"
}

private func jitStatusColor(_ jit: JITReport) -> Color {
    jit.debuggerAttached ? .green : .orange
}

/// Every row and the footer are their own small views with an explicit
/// return type, rather than one large inferred Section closure - see the
/// comment above jitStatusText for why.
private struct JITStatusRow: View {
    let jit: JITReport

    var body: some View {
        LabeledContent("Status") {
            Text(jitStatusText(jit))
                .foregroundStyle(jitStatusColor(jit))
        }
    }
}

private struct JITDebuggerRow: View {
    let jit: JITReport

    var body: some View {
        LabeledContent("Debugger attached (CS_DEBUGGED)") {
            Text(jit.debuggerAttached ? "yes" : "no")
                .foregroundStyle(jit.debuggerAttached ? Color.green : Color.secondary)
        }
    }
}

private struct JITFooter: View {
    var body: some View {
        Text("A debugger-attached (CS_DEBUGGED) process is shown as confirmed. "
             + "This safe check never issues StikDebug's breakpoint request or "
             + "executes generated code. StikDebug's assigned universal script "
             + "still has to prepare each executable region when a guest "
             + "starts.")
    }
}

private struct JITStatusSection: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Section {
            JITStatusRow(jit: model.jit)
            LabeledContent("ARM64 JIT compiled in",
                           value: model.jit.jitCompiledIn ? "yes" : "no")
            JITDebuggerRow(jit: model.jit)
            Text(model.jit.detail).font(.caption).foregroundStyle(.secondary)
            Button("Re-check") { model.refreshJIT() }
        } header: {
            Text("JIT")
        } footer: {
            JITFooter()
        }
    }
}

private struct JITHintSection: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Section {
            // This updates live (every half second) without needing
            // "Re-check" - the button above exists for an immediate,
            // deliberate read, not because the badge is otherwise stale.
            if !model.jit.debuggerAttached {
                Text("BoxedVN cannot enable JIT itself. In current StikDebug, "
                     + "enable Advanced Options, long-press BoxedVN, assign "
                     + "universal.js, and launch BoxedVN with that script "
                     + "kept active. The status above updates automatically.")
                    .font(.caption)
            }
        }
    }
}

struct StatusView: View {
    @EnvironmentObject private var model: AppModel
    @State private var loggingEnabled = Log.isEnabled

    var body: some View {
        List {
            JITStatusSection()
            JITHintSection()

            Section {
                LabeledContent("Increased-memory entitlement",
                               value: model.memory.statusText)
                LabeledContent("Available before memory limit",
                               value: model.memory.availableText)
                Text(model.memory.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Memory")
            } footer: {
                Text("The entitlement value is read from this installed app's "
                     + "signed code signature. Available memory is a live "
                     + "dirty-memory headroom snapshot, so compare it soon "
                     + "after launch before and after applying GetMoreRam.")
            }

            Section("Runtime") {
                LabeledContent("BoxedVN build", value: AppVersion.display)
                    .textSelection(.enabled)
                LabeledContent("Boxedwine core", value: Session.boxedwineVersion)
                LabeledContent("Session", value: model.runtimeState.rawValue)
                if let code = Session.lastExitCode {
                    LabeledContent("Last exit code", value: String(code))
                }
                if !Session.lastError.isEmpty {
                    Text(Session.lastError).font(.caption).foregroundStyle(.red)
                }
            }

            Section {
                Toggle("Session logging", isOn: Binding(
                    get: { loggingEnabled },
                    set: { loggingEnabled = $0; Log.isEnabled = $0 }))
            } header: {
                Text("Diagnostics")
            } footer: {
                Text("On by default. The emulator logs from hot paths, so "
                     + "turning this off can help a slow game — but nothing "
                     + "can be diagnosed from a session that was not recorded.")
            }

            Section("Backends") {
                ForEach(Backends.all()) { backend in
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text(backend.identifier)
                            Spacer()
                            Text(backend.implemented ? "implemented" : "not implemented")
                                .font(.caption)
                                .foregroundStyle(backend.implemented ? .green : .secondary)
                        }
                        if backend.architectures.isEmpty {
                            Text("Runs nothing yet")
                                .font(.caption).foregroundStyle(.secondary)
                        } else {
                            Text("Runs: " + backend.architectures.joined(separator: ", "))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
        .navigationTitle("Runtime status")
    }
}

// MARK: - Settings

struct SettingsView: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage("BoxedVN.preferredOrientation")
    private var preferredOrientation = 1
    @AppStorage(Preferences.soundEnabledKey)
    private var soundEnabled = true
    @State private var showingRootFilesystemImporter = false

    var body: some View {
        List {
            Section {
                Picker("App orientation", selection: $preferredOrientation) {
                    Text("Portrait").tag(0)
                    Text("Landscape").tag(1)
                    Text("Landscape flipped").tag(2)
                }
                .onChange(of: preferredOrientation) { _ in
                    BVNApplyPreferredOrientation()
                }
            } header: {
                Text("Display")
            } footer: {
                Text("BoxedVN stays in this orientation in the library and "
                     + "while Wine is running. Change it here before "
                     + "launching a game.")
            }

            Section {
                Toggle("Sound", isOn: $soundEnabled)
            } footer: {
                Text("Off tells the guest there is no audio device at all, "
                     + "rather than muting one. Turn it off to find out "
                     + "whether a game that crashes is crashing in its audio.")
            }

            Section {
                Text(model.rootFilesystemDescription)
                    .font(.callout)
                if model.isInstallingRootFilesystem {
                    HStack {
                        ProgressView()
                        Text("Installing…")
                    }
                } else {
                    Button("Import root filesystem ZIP…") {
                        showingRootFilesystemImporter = true
                    }
                }
            } header: {
                Text("Root filesystem")
            } footer: {
                Text("Boxedwine needs a Linux/Wine root filesystem archive. "
                     + "See docs/BUILD_IOS.md for the pinned version BoxedVN "
                     + "is tested against.")
            }

            Section {
                if let games = Storage.games {
                    LabeledContent("Games", value: games.path)
                        .font(.caption)
                }
                if let prefixes = Storage.winePrefixes {
                    LabeledContent("Wine prefixes", value: prefixes.path)
                        .font(.caption)
                }
                if let shared = Storage.sharedFiles {
                    LabeledContent("Shared E: drive", value: shared.path)
                        .font(.caption)
                }
                if let fonts = Storage.fonts {
                    LabeledContent("Fonts", value: fonts.path)
                        .font(.caption)
                }
                if let logs = Storage.logs {
                    LabeledContent("Logs", value: logs.path)
                        .font(.caption)
                }
            } header: {
                Text("Storage")
            } footer: {
                // Reading Storage.fonts above is what creates the folder. Say
                // what it is for: a missing font is the one absence that ends
                // a session outright rather than degrading it, and nothing
                // else in the app would ever prompt a user to fill this in.
                Text("Put .ttf, .ttc or .otf files in the Fonts folder to make "
                     + "them available to every game. Games built on a browser "
                     + "engine stop at startup when they can find no font at "
                     + "all, and a Japanese game needs a CJK face to avoid "
                     + "mojibake. BoxedVN ships no fonts of its own.")
            }
        }
        .navigationTitle("Settings")
        .sheet(isPresented: $showingRootFilesystemImporter) {
            DocumentImportPicker(
                contentTypes: zipImportContentTypes,
                onResult: { result in
                    showingRootFilesystemImporter = false
                    switch result {
                    case .success(let url):
                        Log.write("Root filesystem import-copy picker selected "
                                  + url.lastPathComponent, category: "rootfs")
                        model.importRootFilesystem(from: url)
                    case .failure(let error):
                        Log.write("Root filesystem import-copy picker failed: "
                                  + error.localizedDescription,
                                  category: "rootfs", level: BVNLogLevelError)
                        model.alertMessage = error.localizedDescription
                    }
                },
                onCancel: { showingRootFilesystemImporter = false })
        }
    }

}

// MARK: - Logs

struct LogView: View {
    @State private var text = ""
    @State private var generation: UInt64 = 0
    @State private var showingShareSheet = false

    private let refresh = Timer.publish(every: 1, on: .main, in: .common)
        .autoconnect()

    var body: some View {
        ScrollView {
            Text(text.isEmpty ? "No log output yet." : text)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
        }
        .navigationTitle("Logs")
        .toolbar {
            if let file = Log.currentFile {
                ShareLink(item: file) { Image(systemName: "square.and.arrow.up") }
            }
        }
        .onAppear { text = Log.recentText() }
        .onReceive(refresh) { _ in
            let current = Log.generation
            if current != generation {
                generation = current
                text = Log.recentText()
            }
        }
    }
}
