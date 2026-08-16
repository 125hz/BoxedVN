//  BoxedVN fex64 - the application shell for the native-ARM64 stack.
//  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
//  Deliberately one screen. This branch has no games to list and no prefixes
//  to manage; what it has is one question - does translated x86-64 run from a
//  debugger-prepared page on this device - and an interface any bigger than
//  that would imply otherwise.

import SwiftUI
import UIKit

@main
struct BoxedVNFexApp: App {
    var body: some Scene {
        WindowGroup {
            ProbeView()
        }
    }
}

struct ProbeView: View {
    @State private var stage = BVNFexStageIdle
    @State private var report = ""
    @State private var running = false
    @State private var pool: (bytes: Int, used: Int)?
    @State private var step = ""
    @State private var stepSeconds = 0.0
    @State private var copied = false
    @State private var wineStage = BVNWineStageUnavailable
    @State private var wineReport = ""
    @State private var winePersistentLog = ""
    @State private var wineRunning = false

    // The steps that talk to StikDebug cannot be cancelled and can wait
    // forever, so the interface polls instead of waiting for a return value.
    private let tick = Timer.publish(every: 0.75, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            List {
                Section("Stage") {
                    LabeledContent("Reached") {
                        Text(String(cString: BVNFexStageName(stage)))
                            .foregroundStyle(stage == BVNFexStageIdle ? .secondary : .primary)
                    }
                    if let pool {
                        LabeledContent("Arena pool") {
                            Text("\(pool.used / 1024) KiB of \(pool.bytes / 1_048_576) MiB")
                                .monospacedDigit()
                        }
                    }
                }

                Section {
                    Button {
                        run()
                    } label: {
                        HStack {
                            Text(running ? "Running…" : "Run the probe")
                            Spacer()
                            if running { ProgressView() }
                        }
                    }
                    .disabled(running)

                    if running && !step.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(step)
                                .font(.footnote)
                            Text(String(format: "%.0fs", stepSeconds))
                                .font(.caption)
                                .monospacedDigit()
                                .foregroundStyle(.secondary)
                        }
                    }

                    // Naming the deadline matters more than enforcing one:
                    // nothing here can interrupt a debugger request, so the
                    // useful thing is to say which step is stuck and why.
                    if running && stepSeconds > 15 {
                        Text(stallHint)
                            .font(.footnote)
                            .foregroundStyle(.orange)
                    }
                } footer: {
                    // Executing generated code is the one step that can take
                    // the process down with no recovery, so it stays behind a
                    // deliberate tap rather than running at launch.
                    Text("Prepares the executable arena, points FEX's allocator at it, and creates a translator context. Needs BoxedVN to be running through StikDebug with universal.js assigned.")
                }

                Section {
                    LabeledContent("Reached") {
                        Text(String(cString: BVNWineStageName(wineStage)))
                            .foregroundStyle(wineStage == BVNWineStageFailed ? Color.red : Color.primary)
                    }

                    Button {
                        startWine()
                    } label: {
                        HStack {
                            Text(wineRunning ? "Starting Wine…" : "Start Wine")
                            Spacer()
                            if wineRunning { ProgressView() }
                        }
                    }
                    .disabled(!BVNWineAvailable() || wineRunning ||
                              stage != BVNFexStageExecuted)

                    if !BVNWineAvailable() {
                        Text("This IPA contains the FEX probe only. Build the Wine-enabled target to run this stage.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } else if stage != BVNFexStageExecuted {
                        Text("Complete the FEX probe first so Wine can lease a separate executable arena segment.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }

                    if !wineReport.isEmpty {
                        diagnosticPane(wineDisplayText, maximumHeight: 320)
                    } else if !winePersistentLog.isEmpty {
                        diagnosticPane(wineDisplayText, maximumHeight: 320)
                    }

                    if !winePersistentLog.isEmpty {
                        ShareLink(item: wineLogURL) {
                            Label("Export Wine log", systemImage: "square.and.arrow.up")
                        }
                    }

                    if BVNWineAvailable() {
                        Text("Persistent log: \(String(cString: BVNWineLogPath()))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Wine bootstrap")
                } footer: {
                    Text("Starts the embedded wineserver and enters native Wine with a pre-seeded prefix. The ARM64EC runtime remains bundled for the translated executable path after this first boot succeeds.")
                }

                if !report.isEmpty {
                    Section("What happened") {
                        diagnosticPane(report, maximumHeight: 420)
                    }
                }
            }
            .navigationTitle("BoxedVN fex64")
            .toolbar {
                if !diagnosticsText.isEmpty {
                    ToolbarItem(placement: .primaryAction) {
                        ShareLink(item: diagnosticsText) {
                            Label("Share diagnostics", systemImage: "square.and.arrow.up")
                        }
                    }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        UIPasteboard.general.string = diagnosticsText
                        copied = true
                    } label: {
                        Label(copied ? "Copied" : "Copy diagnostics",
                              systemImage: copied ? "checkmark" : "doc.on.doc")
                    }
                    .disabled(diagnosticsText.isEmpty)
                }
            }
        }
        .onAppear(perform: refresh)
        .onReceive(tick) { _ in
            if running {
                step = String(cString: BVNFexCurrentStep())
                stepSeconds = BVNFexCurrentStepSeconds()
                report = String(cString: BVNFexReport())
                copied = false
            }
            if wineRunning {
                wineStage = BVNWineStageReached()
                let latestReport = String(cString: BVNWineReport())
                let latestLog = String(cString: BVNWinePersistentLog())
                if latestReport != wineReport { wineReport = latestReport }
                if latestLog != winePersistentLog { winePersistentLog = latestLog }
                if wineStage == BVNWineStageExited || wineStage == BVNWineStageFailed {
                    wineRunning = false
                }
            }
        }
    }

    private var wineDisplayText: String {
        winePersistentLog.isEmpty ? wineReport : winePersistentLog
    }

    private var wineLogURL: URL {
        URL(fileURLWithPath: String(cString: BVNWineLogPath()))
    }

    private var diagnosticsText: String {
        var sections: [String] = []
        if !report.isEmpty {
            sections.append("FEX probe\n\(report)")
        }
        if !wineDisplayText.isEmpty {
            sections.append("Wine bootstrap\n\(wineDisplayText)")
        }
        return sections.joined(separator: "\n\n")
    }

    private func diagnosticPane(_ contents: String,
                                maximumHeight: CGFloat) -> some View {
        ScrollView(.vertical) {
            Text(contents)
                .font(.system(.footnote, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.vertical, 4)
        }
        .frame(maxHeight: maximumHeight)
        .accessibilityLabel("Diagnostic output")
    }

    // The previous version said the same thing whichever step was stuck, which
    // sent a run that stalled inside the translator looking at StikDebug.
    private var stallHint: String {
        if step.contains("StikDebug") {
            return "This step has not returned. It asks the debugger to prepare every page and then waits; if the StikDebug session ended, relaunch through it and try again."
        }
        if step.contains("confirmation") {
            return "This step has not returned. It executes code from the arena, so a stall here is about the device rather than the debugger — capture this report."
        }
        return "This step has not returned. Nothing here can interrupt it; capture this report with the share button and reopen the app."
    }

    private func run() {
        running = true
        // Off the main thread: preparing the arena talks to the debugger and
        // blocks for as long as that takes.
        DispatchQueue.global(qos: .userInitiated).async {
            let reached = BVNFexProbe()
            DispatchQueue.main.async {
                stage = reached
                running = false
                step = ""
                stepSeconds = 0
                refresh()
            }
        }
    }

    private func startWine() {
        wineRunning = true
        DispatchQueue.global(qos: .userInitiated).async {
            let started = BVNWineStart()
            DispatchQueue.main.async {
                if !started { wineRunning = false }
                wineStage = BVNWineStageReached()
                wineReport = String(cString: BVNWineReport())
                winePersistentLog = String(cString: BVNWinePersistentLog())
            }
        }
    }

    private func refresh() {
        stage = BVNFexStageReached()
        report = String(cString: BVNFexReport())
        wineStage = BVNWineStageReached()
        wineReport = String(cString: BVNWineReport())
        winePersistentLog = String(cString: BVNWinePersistentLog())

        var bytes = 0
        var used = 0
        pool = BVNFexPoolStatus(&bytes, &used) ? (bytes, used) : nil
    }
}
