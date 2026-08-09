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

/// A KL_* knob read by value, not by presence — the Swift twin of
/// `kl_env_on()`. Both defaults below are ON, so `KL_FOO=0` has to be a real
/// way to say no rather than a second way to say yes.
func klEnvOn(_ name: String, default dflt: Bool) -> Bool {
    guard let v = ProcessInfo.processInfo.environment[name] else { return dflt }
    return !["", "0", "no", "off", "false"].contains(v.lowercased())
}

/// The same for a scalar — `kl_env_float()`'s twin. An unparseable value falls
/// back to the default rather than to zero: a typo in a gain knob that silently
/// means "off" is the kind of thing that gets diagnosed as broken hardware.
func klEnvFloat(_ name: String, _ dflt: Float) -> Float {
    guard let v = ProcessInfo.processInfo.environment[name], let f = Float(v) else { return dflt }
    return f
}

enum Immersive {
    static let id = "KleptonImmersive"
    // Default ON. Klepton runs a VR title, so the immersive space is the app —
    // launching from the Home View and getting a window with a Boot button is
    // not a sensible product, it is a test harness. P4's window-and-report
    // shape is still exactly one knob away (KL_IMMERSIVE=0) and has to stay so,
    // because it is the measurement that localises a device regression.
    static var wanted: Bool { klEnvOn("KL_IMMERSIVE", default: true) }

    // .mixed by default now that the picture works. The guest renders an opaque
    // world so passthrough shows through nowhere it matters, and being able to
    // see the room is worth a great deal while the thing being judged is how the
    // scene SITS — scale, distance and IPD are all much easier to call against
    // real surroundings than against a black void. KL_FULL=1 restores .full,
    // which is what shipping a VR title eventually wants.
    static var mixed: Bool { !klEnvOn("KL_FULL", default: false) }

    // How many times the CompositorLayer closure has been entered. SwiftUI may
    // re-evaluate a scene, and a second layer would mean the render loop the log
    // describes is not the one on screen.
    nonisolated(unsafe) private static var closures = 0
    static func bump() -> Int { closures += 1; return closures }
}

@main
struct KleptonApp: App {
    var body: some Scene {
        WindowGroup { BootView() }
            .defaultSize(width: 1100, height: 900)

        // The floor test — KL_TEMPLATE=1. Its own space and its own renderer,
        // sharing nothing with the one below, so a picture here says the
        // platform and the app are fine and a black here says they are not.
        // See KleptonTemplate.swift.
        // KL_TPL_MIXED=1 puts the floor test in .mixed instead of .full, and it
        // is the more informative of the two right now. In .full, "our content
        // is not compositing" and "our content is black" look identical — both
        // are a black field, which is exactly the ambiguity that has cost this
        // hunt several runs. In .mixed the room shows through, so an opaque blue
        // clear is unmistakable: blue means the layer composites, passthrough
        // means it does not, and there is no third reading.
        // The template's own scene shape: a CompositorContent struct, not a bare
        // CompositorLayer in this closure. See KleptonTemplate.swift.
        ImmersiveSpace(id: Template.id) {
            TemplateImmersiveContent()
        }
        .immersionStyle(selection: .constant(Template.mixed ? .mixed : .full),
                        in: .mixed, .full)

        ImmersiveSpace(id: Immersive.id) {
            CompositorLayer(configuration: KleptonStageConfiguration()) { layerRenderer in
                // How many times this closure runs, and for which renderer. If
                // it runs twice, the loop we watch in the log is not necessarily
                // the layer being displayed — which would explain a correct pass
                // that nobody sees.
                NSLog("[cp] CompositorLayer closure #\(Immersive.bump()) "
                      + "renderer=\(ObjectIdentifier(layerRenderer))")
                KleptonCompositor(layerRenderer).startRenderLoop()
            }
        }
        // .mixed by default — see Immersive.mixed. The guest renders an opaque
        // world, so passthrough only shows where Beat Saber's own sky is, which
        // is nowhere; what it buys is being able to see the room while judging
        // how the scene sits. KL_FULL=1 goes back.
        .immersionStyle(selection: .constant(Immersive.mixed ? .mixed : .full),
                        in: .mixed, .full)
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
        // Boots on its own. Tapping an app and then tapping Boot is a harness,
        // not a product — and the scripted paths (`visionos/run.sh`) wanted
        // this anyway, which is what KL_AUTOBOOT was for. `KL_AUTOBOOT=0`
        // restores the button-only shape for hand-driven debugging, where the
        // point is to attach or start a capture before the guest runs.
        .task {
            // The floor test runs INSTEAD of booting. No guest is loaded at
            // all, so a black result here cannot be blamed on anything the
            // guest, ANGLE or the runtime did — which is the whole reason it
            // exists. See KleptonTemplate.swift.
            if Template.wanted {
                status = "template immersive space"
                log = "KL_TEMPLATE=1 — the floor test. No guest is booted.\n"
                // Configure but do NOT boot. This only resolves the two roots
                // and redirects stderr into Documents/klepton-boot.log, which is
                // where every [tpl] line has to land to be readable afterwards —
                // NSLog alone would only reach the system log. No guest library
                // is touched, so the floor stays a floor.
                _ = kl_app_configure(Paths.resources, Paths.container)
                // ...and open the log, which kl_app_boot would normally do.
                // Without this the container keeps the previous run's file and
                // the floor test looks like it produced nothing.
                _ = kl_app_open_log()
                NSLog("[tpl] KL_TEMPLATE=1 — floor test, no guest will be booted")
                let r = await openImmersiveSpace(id: Template.id)
                NSLog("[tpl] openImmersiveSpace -> \(r)")
                return
            }
            if klEnvOn("KL_AUTOBOOT", default: true) { boot() }
        }
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

            // The audio session, and the placement is deliberate on both sides.
            //
            // AFTER kl_app_boot, because boot is what opens (and truncates)
            // Documents/klepton-boot.log — anything this prints earlier goes to
            // a stderr nobody can retrieve from a headset. The first device run
            // did exactly that: the session was configured correctly and the two
            // lines saying so were invisible, which is the wrong way round for
            // the one subsystem whose failure mode is silence.
            //
            // BEFORE the lifecycle, because that is when FMOD opens its OpenSL
            // player, and kl_audio builds its output unit against the session's
            // measured sample rate. A unit initialised against a guess starts
            // successfully and produces nothing, with no error code anywhere in
            // the path. There is a wide margin here: boot ends at initJni and
            // the first [sl] line is thousands of log lines later.
            KleptonAudio.start()
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
                    // The result is logged rather than discarded: `.error` and
                    // `.userCancelled` both leave the app alive with no
                    // compositor, which is indistinguishable in the log from a
                    // compositor that came up and died — and those want
                    // completely different next moves.
                    Task {
                        let r = await openImmersiveSpace(id: Immersive.id)
                        NSLog("[cp] openImmersiveSpace -> \(r)")
                    }
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
