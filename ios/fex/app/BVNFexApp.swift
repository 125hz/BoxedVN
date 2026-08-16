//  BoxedVN fex64 - the application shell for the native-ARM64 stack.
//  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
//  Deliberately one screen. This branch has no games to list and no prefixes
//  to manage; what it has is one question - does translated x86-64 run from a
//  debugger-prepared page on this device - and an interface any bigger than
//  that would imply otherwise.

import SwiftUI

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
                } footer: {
                    // Executing generated code is the one step that can take
                    // the process down with no recovery, so it stays behind a
                    // deliberate tap rather than running at launch.
                    Text("Prepares the executable arena, points FEX's allocator at it, and creates a translator context. Needs BoxedVN to be running through StikDebug with universal.js assigned.")
                }

                if !report.isEmpty {
                    Section("What happened") {
                        Text(report)
                            .font(.system(.footnote, design: .monospaced))
                            .textSelection(.enabled)
                    }
                }
            }
            .navigationTitle("BoxedVN fex64")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    ShareLink(item: report.isEmpty ? "no report yet" : report)
                        .disabled(report.isEmpty)
                }
            }
        }
        .onAppear(perform: refresh)
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
                refresh()
            }
        }
    }

    private func refresh() {
        stage = BVNFexStageReached()
        report = String(cString: BVNFexReport())

        var bytes = 0
        var used = 0
        pool = BVNFexPoolStatus(&bytes, &used) ? (bytes, used) : nil
    }
}
