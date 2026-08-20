//  BoxedVN fex64 - the application shell for the native-ARM64 stack.
//  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
//  Deliberately one screen. This branch has no games to list and no prefixes
//  to manage; what it has is one question - does translated x86-64 run from a
//  debugger-prepared page on this device - and an interface any bigger than
//  that would imply otherwise.

import Metal
import QuartzCore
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
    @State private var wineTarget = BVNWineTargetX64
    @State private var wineElapsed = 0.0
    @State private var winePollTick = 0
    @State private var wineMonitoringTimedOut = false
    @State private var installedPrograms: [String] = []
    @State private var installedIndex = 0

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

                    Picker("Acceptance target", selection: $wineTarget) {
                        Text("Native control").tag(BVNWineTargetNative)
                        Text("x64 translation").tag(BVNWineTargetX64)
                        Text("x64 graphics").tag(BVNWineTargetDXMT)
                        Text("Wine desktop").tag(BVNWineTargetDesktop)
                        Text("Installed program").tag(BVNWineTargetInstalled)
                    }
                    .disabled(wineRunning)
                    .onChange(of: wineTarget) { _, target in
                        if target == BVNWineTargetInstalled { scanInstalled() }
                    }

                    if wineTarget == BVNWineTargetInstalled {
                        installedSection
                    }

                    // DXMT presents onto this layer directly. Shown for the
                    // two paths that render; the rest report through the log.
                    if wineTarget == BVNWineTargetDXMT || wineTarget == BVNWineTargetInstalled {
                        BVNMetalSurface()
                            .frame(height: 220)
                            .background(Color.black)
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
                              wineStage == BVNWineStageProcessStarted ||
                              stage != BVNFexStageExecuted)

                    if wineRunning {
                        Text("Monitoring for \(Int(wineElapsed))s")
                            .font(.caption)
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    } else if wineMonitoringTimedOut {
                        Text("The 30-second monitoring window ended. The process is still wedged; export the log, force-close, and relaunch for the next test.")
                            .font(.footnote)
                            .foregroundStyle(.orange)
                    }

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
                        Text("The on-screen view keeps a recent tail responsive while the full persistent log remains exportable.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text("Persistent log: \(String(cString: BVNWineLogPath()))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Wine bootstrap")
                } footer: {
                    Text("One IPA contains a native control, an x64 translation check, an x64 graphics check, and whatever you copy in yourself. Select one before starting Wine; restart the app to run another target.")
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
                wineElapsed += 0.75
                winePollTick += 1
                wineStage = BVNWineStageReached()
                // Replacing a 24k-character Text every timer tick repeatedly
                // invalidates the List while the user is dragging it. Three
                // seconds is still live enough for bring-up and leaves scroll
                // tracking responsive.
                if winePollTick.isMultiple(of: 4) {
                    refreshWineDiagnostics()
                }
                if wineStage == BVNWineStageExited || wineStage == BVNWineStageFailed {
                    refreshWineDiagnostics()
                    wineRunning = false
                } else if wineElapsed >= 30 {
                    refreshWineDiagnostics()
                    wineMonitoringTimedOut = true
                    wineRunning = false
                }
            }
        }
    }

    private var wineDisplayText: String {
        let contents = winePersistentLog.isEmpty ? wineReport : winePersistentLog
        let limit = 24_000
        guard contents.count > limit else { return contents }
        return "… showing the most recent \(limit) characters …\n" + String(contents.suffix(limit))
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

    /// The list of executables found under the drop directory, plus enough
    /// instruction to get one there. Copying a folder in through Files takes
    /// minutes and there is no feedback while it happens, so the empty state
    /// says exactly where to put it rather than only that nothing was found.
    @ViewBuilder
    private var installedSection: some View {
        if installedPrograms.isEmpty {
            Text("No executable found. In Files, open On My iPhone → BoxedVNFex → games and copy the program's whole folder in, then rescan.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        } else {
            Picker("Program", selection: $installedIndex) {
                ForEach(installedPrograms.indices, id: \.self) { index in
                    Text(installedPrograms[index]).tag(index)
                }
            }
            .disabled(wineRunning)
        }

        Button {
            scanInstalled()
        } label: {
            Label("Rescan", systemImage: "arrow.clockwise")
        }
        .disabled(wineRunning)

        // A game folder holds several executables and only one of them is the
        // right one. Nothing here can tell which, so say what decides it.
        Text("Pick the build that uses Direct3D 11 — the graphics path here translates D3D11 to Metal, so a D3D9 or D3D12 build will not start. 32-bit programs will not start either; this stack runs x86-64 only.")
            .font(.caption)
            .foregroundStyle(.secondary)
    }

    private func scanInstalled() {
        _ = BVNWineScanInstalled()
        let count = Int(BVNWineInstalledCount())
        installedPrograms = (0..<count).compactMap { index in
            guard let name = BVNWineInstalledName(Int32(index)) else { return nil }
            return String(cString: name)
        }
        if installedIndex >= installedPrograms.count { installedIndex = 0 }
    }

    private func startWine() {
        wineRunning = true
        wineElapsed = 0
        winePollTick = 0
        wineMonitoringTimedOut = false
        guard BVNWineSetTarget(wineTarget) else {
            wineRunning = false
            return
        }
        // Refused here rather than in the bridge because starting is one-way:
        // the debugger detach that precedes Wine cannot be undone, so a run
        // that fails for want of a selection costs a relaunch.
        if wineTarget == BVNWineTargetInstalled {
            guard !installedPrograms.isEmpty,
                  BVNWineSelectInstalled(Int32(installedIndex)) else {
                wineRunning = false
                return
            }
        }
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

    private func refreshWineDiagnostics() {
        let latestReport = String(cString: BVNWineReport())
        let latestLog = String(cString: BVNWinePersistentLog())
        if latestReport != wineReport { wineReport = latestReport }
        if latestLog != winePersistentLog { winePersistentLog = latestLog }
    }
}

/// A CAMetalLayer-backed view handed to DXMT.
///
/// DXMT presents D3D11 frames straight onto this layer through the macdrv
/// shim, so nothing here draws: the view exists to own a layer and hand it
/// over before Wine starts. Without it the swapchain has nowhere to present
/// and the run produces no picture however far it otherwise gets.
struct BVNMetalSurface: UIViewRepresentable {
    func makeUIView(context: Context) -> BVNMetalView {
        let view = BVNMetalView()
        bvn_display_set_layer(view.metalLayer)
        return view
    }

    func updateUIView(_ uiView: BVNMetalView, context: Context) {}
}

final class BVNMetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        metalLayer.device = MTLCreateSystemDefaultDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
    }

    required init?(coder: NSCoder) { fatalError("not used") }
}
