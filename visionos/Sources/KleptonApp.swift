import SwiftUI
import Foundation
import CompositorServices

// The visionOS host app (Swift for the platform layer).
//
// The boot gate is deliberately a plain WindowGroup and nothing else: the gate is "the
// guest boots inside an app bundle under AMFI, and a veneer executes", and an
// ImmersiveSpace would have put Compositor Services into the picture before
// there was anything to present, so a failure in either half would have read
// as a failure of the other.
//
// The immersive path sits BESIDE it rather than in place of it, behind
// KL_IMMERSIVE. The window still boots and still reports, so the boot measurement
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
    // Default ON for Beat Saber, OFF for Steam Link, and the difference is not a
    // preference — it is what each app can currently put on screen.
    //
    // Klepton runs a VR title, so the immersive space IS the app: launching from
    // the Home View and getting a window with a Boot button is a test harness,
    // not a product. The window-and-report shape stays exactly one knob away
    // (KL_IMMERSIVE=0) because it is the measurement that localises a device
    // regression.
    //
    // Steam Link is the other way round until its 2D shell has a window. Its VR
    // half cannot draw anything without an authorized session, and a session
    // arrives by hand (KL_SLINK_SARGS) — so the default launch opens an
    // immersive space that is black by construction and hides the one surface
    // with information on it. KL_IMMERSIVE=1 turns it on for a run that HAS a
    // session, which is the run that wants it.
    //
    // Reads kl_app_target_is_steamlink(), so it is only meaningful after
    // kl_app_configure. Both call sites are inside boot(), after configure has
    // returned; the scene body reads `mixed`, not this.
    static var wanted: Bool {
        klEnvOn("KL_IMMERSIVE", default: kl_app_target_is_steamlink() == 0)
    }

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

    /// Whether the system's persistent overlays — the Home indicator, and the
    /// hand-gesture affordance that fades in beneath it — stay on top of the
    /// guest's picture.
    ///
    /// Hidden by default. The overlay is drawn by the system *over* the
    /// immersive scene, and both guests here put their own interactive content
    /// exactly where it lands: Beat Saber's lower menu row and Steam Link's
    /// dashboard toolbar are both near the bottom of the field, so the overlay
    /// sits on the controls rather than beside them. It also reappears on every
    /// hand raise, which for a title driven entirely by raised hands is
    /// continuous.
    ///
    /// `.hidden` is a request, not a guarantee — the system still shows the
    /// indicator at moments it considers mandatory (the first seconds of a
    /// space, a pending system alert), which is why this is the same `Visibility`
    /// ALVR passes rather than a claim that it is gone. `KL_OVERLAYS=1` puts it
    /// back, which is what a run wants when the question is whether the system
    /// still thinks our space is on screen at all.
    static var systemOverlays: Visibility {
        klEnvOn("KL_OVERLAYS", default: false) ? .automatic : .hidden
    }
}

/// What backgrounding means for this process: the end of it.
///
/// ALVR's shape, and for the same reason. Everything the guest is holding when
/// the app goes away is either unresumable or expensive to re-establish — the
/// ARKit session, the Compositor Services layer, the ANGLE context and the eye
/// swapchain behind it, FMOD's OpenSL player, and a Unity engine that has been
/// told it is on a Quest and never expects the display to leave. Resuming that
/// correctly is a project of its own; resuming it *incorrectly* is a class of
/// bug that reports itself as "the second run is broken" long after the cause.
/// Exiting makes every launch the first launch.
///
/// `KL_EXIT_ON_BACKGROUND=0` keeps the old behaviour, which is what a debugging
/// session wants when the headset comes off with a capture still open.
enum Lifecycle {
    static func scenePhaseChanged(to phase: ScenePhase) {
        NSLog("[app] scene phase -> \(phase)")
        // Coming back is the audio's cue, and it needs one: this OS silently
        // stops calling CoreAudio's render callback across a scene transition —
        // the boot window being closed while the immersive space runs is one,
        // a Digital Crown press to passthrough is another — with no error and
        // no interruption notification. See kl_audio_resume; the compositor
        // hooks the immersive half of the same transition, and kl_audio's
        // heartbeat catches whatever neither of them sees.
        //
        // Unconditional rather than "only if we were away". A rebuild of a
        // healthy unit costs a few milliseconds of silence and cannot go wrong;
        // the state that would let us skip it is precisely the state this
        // platform lies about.
        if phase == .active { kl_audio_resume() }
        guard phase == .background else { return }
        guard klEnvOn("KL_EXIT_ON_BACKGROUND", default: true) else {
            NSLog("[app] backgrounded; KL_EXIT_ON_BACKGROUND=0, staying alive")
            return
        }
        NSLog("[app] backgrounded — exiting (KL_EXIT_ON_BACKGROUND)")
        // Flush before exit rather than relying on it. The guest's log is a
        // file in the container (kl_app.c redirects stdout/stderr there) and it
        // is the only account of the run that survives; `exit` does flush
        // stdio, but it also runs atexit handlers and static destructors inside
        // a guest whose threads are still live, and one of those blocking would
        // turn a clean exit into a watchdog kill with a truncated log.
        fflush(nil)
        exit(0)
    }
}

@main
struct KleptonApp: App {
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup { BootView() }
            .defaultSize(width: 1280, height: 800)
            // On the app's phase, not this window's: closing the boot window
            // while the immersive space is up is not backgrounding, and must
            // not be treated as it. The log line above every decision is what
            // makes that distinction checkable on a device rather than assumed.
            .onChange(of: scenePhase) { _, phase in Lifecycle.scenePhaseChanged(to: phase) }

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
        // See Immersive.systemOverlays. On the scene, not on a view inside it:
        // a CompositorLayer has no view hierarchy for the View-level modifier to
        // attach to, so the Scene-level one is the only one that applies here.
        .persistentSystemOverlays(Immersive.systemOverlays)
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
    // The flat guest's picture, and the handoff that ends it. Polled rather
    // than pushed: both are C state written by a guest thread, and a callback
    // out of one into SwiftUI would be a main-actor hop from a thread that is
    // mid-frame. A timer at 5 Hz costs nothing and cannot deadlock.
    @State private var showShell = false
    @State private var handedOff = false
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    var body: some View {
        Group {
            if showShell { ShellWindow() } else { bootReport }
        }
        .task { await watchPresentation() }
    }

    /// Which of the two things the window can be showing, decided by what the
    /// guest is actually producing rather than by a knob.
    ///
    /// kl_present observes the mode — a window surface was created, or an eye
    /// texture was set up — so a guest that surprises us is described correctly
    /// rather than according to a flag someone remembered to set. The 2D->VR
    /// handoff is the transition kl_present.h was written for.
    private func watchPresentation() async {
        while !Task.isCancelled {
            let mono = kl_present_mode_now() == KL_PRESENT_MONO
            if mono != showShell { showShell = mono }

            if kl_app_vrlink_pending() != 0, !handedOff {
                handedOff = true
                NSLog("[app] 2D -> VR handoff: \(String(cString: kl_app_vrlink_sargs()))")
                // Unconditionally, not behind Immersive.wanted: that default
                // says what a LAUNCH should open, and this is a run that has
                // just been handed the one thing the VR half cannot start
                // without. openedSpace is shared with boot() so the two paths
                // cannot both open it.
                if !openedSpace {
                    openedSpace = true
                    let r = await openImmersiveSpace(id: Immersive.id)
                    NSLog("[cp] openImmersiveSpace (after handoff) -> \(r)")
                }
            }
            try? await Task.sleep(for: .milliseconds(200))
        }
    }

    private var bootReport: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Klepton").font(.largeTitle.bold())
                // The guest, by name. Two apps are built from this tree and they
                // look identical from the front; a boot log that does not say
                // which one produced it is a log that can be read as the other's.
                Text(String(cString: kl_app_target_name()))
                    .foregroundStyle(.secondary)
                Spacer()
                if finished {
                    Label(succeeded ? "initJni completed" : status,
                          systemImage: succeeded ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(succeeded ? .green : .red)
                }
            }

            ScrollView {
                Text(log.isEmpty ? "Press Boot to load the guest chain." : log)
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
                // To the SYSTEM log as well as the window, because this is the
                // one failure that happens before there is a klepton-boot.log to
                // write into — kl_app_boot opens that file, and configure runs
                // first. A scripted run therefore saw no output at all and read
                // as a launch that never reached our code; the reason was
                // sitting in a SwiftUI label nobody was looking at.
                NSLog("[app] configure failed: \(String(cString: kl_app_status()))")
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
            // Carry on into the Android lifecycle when asked, in the same
            // process and on the same thread. Only after boot has reported, so a
            // lifecycle failure cannot be mistaken for a boot failure — and only
            // when asked, because the gate is a boot that stops at initJni and
            // it must stay possible to take exactly that measurement.
            //
            // This is where libil2cpp (66 MB, 3,083 x18 veneers) first loads
            // under AMFI, and where the synthetic /proc is first read on device.
            //
            // Skipped under KL_IMMERSIVE: there the compositor is the frame
            // clock and drives kl_app_lifecycle_begin/_frame itself, and both
            // entries are once-per-process, so running the pump here as well
            // would take the lifecycle's only turn.
            //
            // Which knob asks for it depends on the guest, because a frame
            // budget is meaningless to one that owns its own frame loop. Beat
            // Saber's is KL_FRAMES — nativeRender calls, counted. Steam Link's
            // is KL_SLINK_WAIT — seconds of looper pump, exactly as it is on the
            // command line. Gating both on KL_FRAMES was the first version and
            // it made the window path on that target load the chain, report, and
            // then never call ANativeActivity_onCreate at all: a run that looks
            // finished and never started the activity.
            // ...and whether it is asked for AT ALL differs too. Beat Saber's
            // window path stops at initJni unless KL_FRAMES says otherwise,
            // because continuing is what the gate must not do. Steam Link's has
            // no such reason: its chain report is printed and complete BEFORE
            // the activity starts, so that measurement stays takeable from any
            // run, and stopping there instead would mean the default launch
            // loads the guest and then does nothing at all. So it always runs,
            // bounded by KL_SLINK_WAIT (30 s by default) — bounded rather than
            // open-ended because the media/audio/XR/GL report is written at the
            // END of the pump, and that is the report a working run has no other
            // way of producing.
            //
            // The Unreal guest is the Steam Link arm's shape for the Steam Link
            // arm's reasons: its budget is KL_UE4_WAIT in seconds (a frame count
            // means nothing to a guest that owns its frame loop), and its report
            // is written at the END of the pump, so stopping at the chain would
            // mean the default launch loads the guest and then does nothing.
            let env = ProcessInfo.processInfo.environment
            let ownsLoop = kl_app_target_owns_frame_loop() != 0
            if result == 0, !Immersive.wanted,
               ownsLoop || env["KL_FRAMES"] != nil {
                result = kl_app_lifecycle(UInt32(env["KL_FRAMES"] ?? "") ?? 1)
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

            KLEPTON_TARGET=\(String(cString: kl_app_target_name())) \
                visionos/stage_assets.sh <device-udid>

        from the repo root with the device paired and unlocked.
        """
    }
}
