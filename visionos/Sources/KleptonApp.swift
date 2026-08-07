import SwiftUI
import Foundation
import CompositorServices

// The visionOS host app (PLANNING §12.6 — Swift for the platform layer).
//
// P4 was deliberately a plain WindowGroup and nothing else: the gate is "the
// guest boots inside an app bundle under AMFI, and a veneer executes", and an
// ImmersiveSpace would have put Compositor Services into the picture before
// there was anything to present, so a failure in either half would have read
// as a failure of the other.
//
// P5b adds the immersive path *beside* it rather than in place of it, behind
// KL_IMMERSIVE. The window still boots and still reports, so P4's measurement
// stays takeable on the same binary — which matters because that measurement
// is how a device regression gets localised.

enum Immersive {
    static let id = "KleptonImmersive"
    static var wanted: Bool { ProcessInfo.processInfo.environment["KL_IMMERSIVE"] != nil }
}

@main
struct KleptonApp: App {
    var body: some Scene {
        WindowGroup { BootView() }
            .defaultSize(width: 1100, height: 900)

        ImmersiveSpace(id: Immersive.id) {
            CompositorLayer(configuration: KleptonStageConfiguration()) { layerRenderer in
                KleptonCompositor(layerRenderer).startRenderLoop()
            }
        }
        // .full, not .mixed: the guest is a VR title that renders its own
        // world, so passthrough behind it would only show through wherever
        // Beat Saber's own sky is, which is nowhere.
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}

/// Where the runtime's two roots live.
///
/// Guest code stays in the bundle and assets stay in the container, and the
/// split is not arbitrary: P3 established that AMFI accepts a `klepton-ld`
/// dylib *inside a bundle we signed*, and established nothing about one pushed
/// into Documents afterwards. The 2.3 GB of assets carry no code, so they go
/// the other way — staged once — because re-uploading them on every install
/// would make the iterate loop unusable.
enum Paths {
    static var resources: String { Bundle.main.bundlePath }
    static var container: String {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
    }
}

struct BootView: View {
    @State private var log = ""
    @State private var status = "idle"
    @State private var running = false
    @State private var finished = false
    @State private var succeeded = false
    @State private var openedSpace = false
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Klepton").font(.largeTitle.bold())
                Text("P4 — guest boot on visionOS").foregroundStyle(.secondary)
                Spacer()
                if finished {
                    Label(succeeded ? "initJni completed" : status,
                          systemImage: succeeded ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(succeeded ? .green : .red)
                }
            }

            ScrollView {
                Text(log.isEmpty ? "Press Boot to load libmain.so and run JNI_OnLoad." : log)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: .infinity)

            HStack(spacing: 16) {
                Button(running ? "Running…" : "Boot") { boot() }
                    .disabled(running)
                if running { ProgressView() }
                if finished, !log.isEmpty {
                    ShareLink(item: log) { Label("Export log", systemImage: "square.and.arrow.up") }
                }
                Spacer()
                Text(status).font(.footnote).foregroundStyle(.secondary)
            }
        }
        .padding(24)
        // KL_AUTOBOOT=1 starts the run without a tap, so the simulator and
        // device paths can be driven from a script (`visionos/run.sh`). The
        // button stays the interactive way in.
        .task { if ProcessInfo.processInfo.environment["KL_AUTOBOOT"] != nil { boot() } }
    }

    private func boot() {
        // Guarded on this side too, not only in kl_app.c. The guest boots
        // once per process — the runtime's JNI tables are process-global, so a
        // second run re-registers every native onto the same table and the
        // numbers stop being comparable to the host's. Disabling the button
        // while running is not enough: the run *finishes*, and the obvious
        // next thing to do with a finished run is press Boot again.
        guard !running, !finished else { return }
        running = true; log = ""

        // Off the main thread: the guest blocks — Unity's Baselib waits on
        // futexes, IL2CPP's GC suspends the world — and a blocked main thread
        // is a watchdog kill on this platform, which would present as a crash
        // with no report rather than as the hang it is.
        Thread.detachNewThread {
            let rc = kl_app_configure(Paths.resources, Paths.container)
            if rc != 0 {
                DispatchQueue.main.async {
                    status = String(cString: kl_app_status())
                    log = "configure failed: \(status)\n\n" + stagingHelp
                    running = false; finished = true; succeeded = false
                }
                return
            }

            let logPath = String(cString: kl_app_log_path())
            // The report is read back from the file rather than returned,
            // because an unimplemented JNI call aborts the process by design.
            // If this run dies, the next launch still shows how far it got.
            let poll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
                if let t = try? String(contentsOfFile: logPath, encoding: .utf8) {
                    DispatchQueue.main.async { log = t }
                }
            }
            RunLoop.current.add(poll, forMode: .common)

            var result = kl_app_boot()
            // P5.4: carry on into the Android lifecycle when asked, in the same
            // process and on the same thread. Only after boot has reported, so a
            // lifecycle failure cannot be mistaken for a boot failure — and only
            // when asked, because P4's gate is a boot that stops at initJni and
            // it must stay possible to take exactly that measurement.
            //
            // This is where libil2cpp (66 MB, 3,083 x18 veneers) first loads
            // under AMFI, and where the synthetic /proc is first read on device.
            //
            // Skipped under KL_IMMERSIVE: there the compositor is the frame
            // clock and drives kl_app_lifecycle_begin/_frame itself, and both
            // entries are once-per-process, so running the pump here as well
            // would take the lifecycle's only turn.
            if result == 0, !Immersive.wanted,
               let f = ProcessInfo.processInfo.environment["KL_FRAMES"] {
                result = kl_app_lifecycle(UInt32(f) ?? 1)
            }
            poll.invalidate()

            let text = (try? String(contentsOfFile: logPath, encoding: .utf8)) ?? ""
            DispatchQueue.main.async {
                log = text
                status = String(cString: kl_app_status())
                succeeded = (result == 0)
                running = false; finished = true
                // The space opens only after boot has succeeded, and from here
                // rather than from .task, because the compositor's very first
                // act is kl_app_lifecycle_begin() — which needs the UnityPlayer
                // that kl_app_boot creates. Opening it in parallel with boot
                // would be a race whose losing side reports "kl_app_boot must
                // run first" and reads like a missing binding.
                if result == 0, Immersive.wanted, !openedSpace {
                    openedSpace = true
                    Task { _ = await openImmersiveSpace(id: Immersive.id) }
                }
            }
        }
    }

    private var stagingHelp: String {
        """
        The APK assets are staged into the app's Documents container rather \
        than bundled, so they survive a reinstall and are uploaded once. Run:

            visionos/stage_assets.sh <device-udid>

        from the repo root with the device paired and unlocked.
        """
    }
}
