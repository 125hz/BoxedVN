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
import UniformTypeIdentifiers

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
    @State private var showingGameImporter = false
    @State private var showingFolderImporter = false
    @State private var pendingImportURL: URL?
    @State private var pendingTitle = ""
    @State private var showingTitlePrompt = false

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
                LabeledContent("Session", value: model.runtimeState.rawValue)
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
            }

            Section("Actions") {
                Button("Import game from ZIP…") { showingGameImporter = true }
                    .disabled(model.isImporting)
                Button("Import game from folder…") { showingFolderImporter = true }
                    .disabled(model.isImporting)
                Button("Run Wine Notepad") { model.launchWineNotepad() }
                    .disabled(model.rootFilesystem == nil)
                if model.runtimeState == .running {
                    Button("Quit running session", role: .destructive) {
                        model.requestShutdown()
                    }
                }
            }

            Section {
                NavigationLink("Settings") { SettingsView() }
                NavigationLink("Logs") { LogView() }
            }
        }
        .fileImporter(isPresented: $showingGameImporter,
                      allowedContentTypes: [.zip]) { result in
            handleImport(result)
        }
        .fileImporter(isPresented: $showingFolderImporter,
                      allowedContentTypes: [.folder]) { result in
            handleImport(result)
        }
        .alert("Name this game", isPresented: $showingTitlePrompt) {
            TextField("Title", text: $pendingTitle)
            Button("Import") {
                if let url = pendingImportURL {
                    model.importGame(from: url, title: pendingTitle)
                }
                pendingImportURL = nil
            }
            Button("Cancel", role: .cancel) { pendingImportURL = nil }
        }
    }

    private func handleImport(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            pendingImportURL = url
            pendingTitle = url.deletingPathExtension().lastPathComponent
            showingTitlePrompt = true
        case .failure(let error):
            model.alertMessage = error.localizedDescription
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
    @State private var workingDirectory = ""
    @State private var resolution = "default"
    @State private var saveError: String?

    private let resolutions = ["default", "640x480", "800x600", "1024x576",
                               "1280x720", "1920x1080"]

    var body: some View {
        List {
            Section("Executable") {
                if executables.isEmpty {
                    Text("Scanning…").foregroundStyle(.secondary)
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
                TextField("Working directory (inside the game)",
                          text: $workingDirectory)
                Picker("Resolution", selection: $resolution) {
                    ForEach(resolutions, id: \.self) { Text($0) }
                }
                Button("Save") { save() }
                if let saveError {
                    Text(saveError).foregroundStyle(.red).font(.caption)
                }
            }

            Section {
                Button("Launch") { save(); model.launch(game) }
                    .disabled(selected.isEmpty
                              || !(executables.first { $0.relativePath == selected }?
                                    .runnable ?? false))
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

    private func save() {
        let arguments = argumentsText
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }

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
                workingDirectory: workingDirectory, arguments: arguments,
                width: width, height: height)
            saveError = nil
            model.reloadGames()
        } catch {
            saveError = error.localizedDescription
        }
    }
}

// MARK: - Status

struct StatusView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        List {
            Section("JIT") {
                LabeledContent("Status") {
                    Text(model.jit.isUsable ? "Available" : "Unavailable")
                        .foregroundStyle(model.jit.isUsable ? .green : .orange)
                }
                LabeledContent("ARM64 JIT compiled in",
                               value: model.jit.jitCompiledIn ? "yes" : "no")
                LabeledContent("Debugger attached (CS_DEBUGGED)") {
                    Text(model.jit.debuggerAttached ? "yes" : "no")
                        .foregroundStyle(model.jit.debuggerAttached ? .green : .secondary)
                }
                LabeledContent("Executable memory",
                               value: model.jit.executableMemoryAvailable ? "yes" : "no")
                Text(model.jit.detail).font(.caption).foregroundStyle(.secondary)
                Button("Re-check") { model.refreshJIT() }
            }

            Section {
                // This updates live (every half second) without needing
                // "Re-check" - the button above exists for an immediate,
                // deliberate read, not because the badge is otherwise stale.
                if model.jit.debuggerAttached && !model.jit.executableMemoryAvailable {
                    Text("A debugger is attached, but executable memory is "
                         + "still unavailable. This points at the app's own "
                         + "signature rather than StikDebug - see the detail "
                         + "above.")
                        .foregroundStyle(.orange)
                } else if !model.jit.debuggerAttached {
                    Text("BoxedVN cannot enable JIT itself. Attach StikDebug or "
                         + "another JIT enabler to this app while it is "
                         + "running; the status above updates automatically.")
                        .font(.caption)
                }
            }

            Section("Runtime") {
                LabeledContent("Boxedwine core", value: Session.boxedwineVersion)
                LabeledContent("Session", value: model.runtimeState.rawValue)
                if let code = Session.lastExitCode {
                    LabeledContent("Last exit code", value: String(code))
                }
                if !Session.lastError.isEmpty {
                    Text(Session.lastError).font(.caption).foregroundStyle(.red)
                }
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
    @State private var showingRootFilesystemImporter = false

    var body: some View {
        List {
            Section {
                Text(model.rootFilesystemDescription)
                    .font(.callout)
                Button("Import root filesystem ZIP…") {
                    showingRootFilesystemImporter = true
                }
            } header: {
                Text("Root filesystem")
            } footer: {
                Text("Boxedwine needs a Linux/Wine root filesystem archive. "
                     + "See docs/BUILD_IOS.md for the pinned version BoxedVN "
                     + "is tested against.")
            }

            Section("Storage") {
                if let games = Storage.games {
                    LabeledContent("Games", value: games.path)
                        .font(.caption)
                }
                if let prefixes = Storage.winePrefixes {
                    LabeledContent("Wine prefixes", value: prefixes.path)
                        .font(.caption)
                }
                if let logs = Storage.logs {
                    LabeledContent("Logs", value: logs.path)
                        .font(.caption)
                }
            }
        }
        .navigationTitle("Settings")
        .fileImporter(isPresented: $showingRootFilesystemImporter,
                      allowedContentTypes: [.zip]) { result in
            switch result {
            case .success(let url): model.importRootFilesystem(from: url)
            case .failure(let error): model.alertMessage = error.localizedDescription
            }
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
