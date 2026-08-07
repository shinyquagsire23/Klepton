import SwiftUI
import Foundation

// The visionOS host app (PLANNING §12.6 — Swift for the platform layer).
//
// At P4 this is deliberately a plain WindowGroup and not an ImmersiveSpace.
// The gate is "the guest boots inside an app bundle under AMFI, and a veneer
// executes" — no rendering, no compositor, no ARKit. Adding an ImmersiveSpace
// now would put Compositor Services into the picture before there is anything
// to present, and a failure in either half would read as a failure of the
// other. The immersive path arrives at P5b, once ANGLE has somewhere to draw.

@main
struct KleptonApp: App {
    var body: some Scene {
        WindowGroup { BootView() }
            .defaultSize(width: 1100, height: 900)
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
            if result == 0, let f = ProcessInfo.processInfo.environment["KL_FRAMES"] {
                result = kl_app_lifecycle(UInt32(f) ?? 1)
            }
            poll.invalidate()

            let text = (try? String(contentsOfFile: logPath, encoding: .utf8)) ?? ""
            DispatchQueue.main.async {
                log = text
                status = String(cString: kl_app_status())
                succeeded = (result == 0)
                running = false; finished = true
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
