import SwiftUI
import Foundation
import CompositorServices
import UIKit
import WebKit
import Network

/// The local-network permission, asked for while a window can show the question.
///
/// visionOS grants the local network per app, after an alert the user answers,
/// and nothing a guest does on its own raises that alert here. Steam Link's
/// shell finds and pairs with the PC through a dual-stack socket, as IPv4-mapped
/// addresses, which pass without the permission and do not raise the alert. Its
/// VR part then streams over a plain IPv4 socket from inside the immersive
/// space: every datagram is refused ("Broken pipe", then "No route to host" —
/// 193,000 in one minute), no alert comes up there, the app is not even listed
/// under Settings > Privacy & Security > Local Network, and the PC sits at
/// "waiting for connection". A sweep of the subnet used to raise the alert by
/// accident; on a network too large to sweep (a /16) nothing did, so a new
/// install could pair and never stream (seen 2026-10-13 with second installs
/// beside a working one, on three toolchain states alike).
///
/// Plain datagrams sent from the window did not raise the alert either, nor did
/// a restart of the headset. A Bonjour browse does, at once: the alert comes
/// with the window, the user allows, and the stream runs. The service browsed
/// for is our own name and nobody offers it (NSBonjourServices in Info.plist
/// has to list it); the browse is there to be seen by the system, not to find
/// anything.
enum KLLocalNetwork {
    private static var browser: NWBrowser?
    static func ask() {
        guard browser == nil else { return }
        let b = NWBrowser(for: .bonjour(type: "_klepton-lan._udp", domain: nil), using: .udp)
        b.stateUpdateHandler = { NSLog("[net] local-network browse: \(String(describing: $0))") }
        b.browseResultsChangedHandler = { _, _ in }
        b.start(queue: .main)
        browser = b
    }
}

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

    // Every game opens a MIXED space, and black is drawn wherever it shows no
    // picture (mixedBlack, KleptonCompositor) — the owner's call, 2026-10-12,
    // for all games: "lieber mit 100 FPS und schwarzem Hintergrund arbeiten als
    // mit den 120". A fully immersive space runs this headset's panel at up to
    // 120 Hz, and visionOS halves that to 60 for the rest of a level once the
    // game cannot keep up (seen with Beat Saber at full resolution, although the
    // compositor itself missed no deadline); a mixed one runs at up to 100 and
    // stays there. What the full space gave besides — black instead of the room
    // behind loading screens and between levels — the black backdrop gives.
    //
    // Before this the targets that are in-world titles opened .full
    // (kl_app_target_wants_full) and the rest .mixed with the room showing.
    // KL_FULL=1 is the full space again, for any target; KL_MIXED_BLACK=0 shows
    // the room in a mixed one, which is what judging how a scene sits wants.
    //
    // The full space is also a SETTING since ("Frame rate: max. 120" in the
    // start window, KleptonRendering.maxRate), off unless chosen and not
    // offered to a mixed-reality game. KL_FULL in the environment still decides
    // either way.
    static var mixed: Bool {
        // Space Atlas VR must hide the real room in every direction from the
        // first immersive frame. A mixed-space black quad is not that guarantee.
        // KL_FULL=0 remains an explicit A/B override; other titles keep their default.
        let spaceAtlas = String(cString: kl_app_target_name_or_default()) == "x8016534201745429"
        return !klEnvOn("KL_FULL", default: spaceAtlas || KleptonRendering.shared.wantsFullSpace)
    }
    static var mixedBlack: Bool { mixed && klEnvOn("KL_MIXED_BLACK", default: true) }
    /// ...as it stood when the game was started: the compositor reads this from
    /// its own thread, and the choice is made once per run anyway.
    nonisolated(unsafe) static var mixedBlackAtBoot = false
    /// ...and whether the space is a mixed one at all (the play area's see-through setting needs one).
    nonisolated(unsafe) static var mixedAtBoot = true

    // How many times the CompositorLayer closure has been entered. SwiftUI may
    // re-evaluate a scene, and a second layer would mean the render loop the log
    // describes is not the one on screen.
    nonisolated(unsafe) private static var closures = 0
    static func bump() -> Int { closures += 1; return closures }

    /// Whether the system's persistent overlays — the Home indicator, and the
    /// hand-gesture affordance that fades in beneath it — stay on top of the
    /// guest's picture.
    ///
    /// Hidden by default except for BRINK's real-hand interface. The overlay is
    /// drawn by the system *over* the
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
        // BRINK uses the user's real hands: retain visionOS's palm-up Home
        // indicator and palm-flip Control Center gestures. Other targets keep
        // their existing preference; an explicit KL_OVERLAYS override wins.
        let brink = (Bundle.main.bundleIdentifier ?? "").hasSuffix(".x3635172946605196")
        return klEnvOn("KL_OVERLAYS", default: brink) ? .automatic : .hidden
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
        // The crown press delivers .inactive at once but .background only when
        // the system gets round to it (measured ~30 s). The music belongs to
        // the immersive moment: go silent on the first, and still exit on the
        // second. .active restores — the boot transition passes through a
        // 2 s .inactive on the way in, which this round-trips harmlessly.
        switch phase {
        case .inactive: KleptonAudio.setSilenced(true)
        case .active:   KleptonAudio.setSilenced(false)
        default: break
        }
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
        if KLRealityPresent.switching {
            NSLog("[app] backgrounded while the picture moves between spaces — staying")
            return
        }
        guard klEnvOn("KL_EXIT_ON_BACKGROUND", default: true) else {
            NSLog("[app] backgrounded; KL_EXIT_ON_BACKGROUND=0, staying alive")
            return
        }
        NSLog("[app] backgrounded — exiting (KL_EXIT_ON_BACKGROUND)")
        leave()
    }

    /// The end of the process, and what is written down before it. Being
    /// backgrounded comes here, and so does a game that quit by itself
    /// (`kl_app_guest_asked_quit`, from the compositor): the same leaving,
    /// whoever asked for it. On the main thread.
    static func leave() -> Never {
        kl_app_exit_report()
        KleptonRendering.shared.rememberRun()
        klAlyxRememberRun()
        // Flush before exit rather than relying on it. The guest's log is a
        // file in the container (kl_app.c redirects stdout/stderr there) and it
        // is the only account of the run that survives; `exit` does flush
        // stdio, but it also runs atexit handlers and static destructors inside
        // a guest whose threads are still live, and one of those blocking would
        // turn a clean exit into a watchdog kill with a truncated log.
        fflush(nil)
        // A guest with a quit of its own gets to take it first: its shutdown is
        // where an engine saves the player's settings, and it ends the process
        // itself when it is through. Bounded — a guest that does not finish in
        // time is ended as before.
        if kl_app_guest_request_quit() != 0 {
            NSLog("[app] asked the guest to quit; giving it up to 2.5 s to save and leave")
            fflush(nil)
            // Its main returning is the end of its shutdown (Xash returns from
            // SDL_main rather than calling exit), so that is the cue to go.
            var waited = 0.0
            while waited < 2.5 && kl_app_guest_quit_done() == 0 {
                Thread.sleep(forTimeInterval: 0.05)
                waited += 0.05
            }
            NSLog(kl_app_guest_quit_done() != 0
                  ? "[app] the guest finished its shutdown after \(Int(waited * 1000)) ms — exiting"
                  : "[app] the guest did not finish in time — exiting")
            fflush(nil)
        }
        exit(0)
    }
}

/// Publishes the Vision Pro's real battery state through the one battery seam
/// used by both Android BatteryManager and OVRPlugin.  Steam Link reads that
/// telemetry and forwards it to SteamVR as the HMD battery percentage.
///
/// UIDevice monitoring is public UIKit API and needs no entitlement.  It is
/// disabled by default, so enable it once for the process and keep the two
/// notification tokens alive for the app's lifetime.  Explicit KL_BATTERY_*
/// values remain useful for host tests and one-off diagnostics and therefore
/// win independently for level and charging state.
@MainActor
private final class KleptonBattery {
    static let shared = KleptonBattery()

    private var started = false
    private var observers: [NSObjectProtocol] = []
    private let overrideLevel = ProcessInfo.processInfo.environment["KL_BATTERY_LEVEL"] != nil
    private let overrideCharging = ProcessInfo.processInfo.environment["KL_BATTERY_CHARGING"] != nil

    func start() {
        guard !started else { return }
        started = true

        let device = UIDevice.current
        device.isBatteryMonitoringEnabled = true
        publish()

        let center = NotificationCenter.default
        for name in [UIDevice.batteryLevelDidChangeNotification,
                     UIDevice.batteryStateDidChangeNotification] {
            observers.append(center.addObserver(forName: name, object: device, queue: .main) {
                _ in Task { @MainActor in KleptonBattery.shared.publish() }
            })
        }
    }

    private func publish() {
        let device = UIDevice.current
        let rawLevel = device.batteryLevel
        var levelDescription = "override"
        var stateDescription = "override"

        if !overrideLevel {
            if rawLevel >= 0 {
                let percent = Int((rawLevel * 100).rounded())
                kl_ovrp_set_battery_level(Int32(percent))
                levelDescription = "\(percent)%"
            } else {
                levelDescription = "unknown (keeping fallback)"
            }
        }

        if !overrideCharging {
            switch device.batteryState {
            case .charging:
                kl_ovrp_set_battery_charging(1)
                stateDescription = "charging"
            case .full:
                kl_ovrp_set_battery_charging(1)
                stateDescription = "full"
            case .unplugged:
                kl_ovrp_set_battery_charging(0)
                stateDescription = "unplugged"
            case .unknown:
                stateDescription = "unknown (keeping fallback)"
            @unknown default:
                stateDescription = "unknown (keeping fallback)"
            }
        }

        NSLog("[battery] HMD level \(levelDescription), state \(stateDescription)")
    }
}

@main
struct KleptonApp: App {
    @Environment(\.scenePhase) private var scenePhase
    /// Observed so the Matting panel's hand-matting toggle re-evaluates the
    /// immersive scene's `.upperLimbVisibility` live. The object is the shared
    /// singleton the panel edits, not a second copy.
    @ObservedObject private var chroma = KleptonChroma.shared

    // Before anything here has asked Metal for a shader (kl_app.c).
    init() {
        kl_app_shader_cache_test()
        // Before the start window reads the picture settings.
        if klIsAlyx() { klAlyxMigrateDials() }
        // The device's language, for a guest that takes its own from the
        // device (kl_jni_services.c reads it where that is switched on).
        if let first = Locale.preferredLanguages.first {
            let loc = Locale(identifier: first)
            if let lang = loc.language.languageCode?.identifier {
                let region = loc.region?.identifier ?? Locale.current.region?.identifier
                setenv("KL_DEVICE_LANG", region.map { "\(lang)_\($0)" } ?? lang, 0)
            }
        }
        // The game's files, which the installer can put back, out of the
        // device's backup (KleptonBackup.swift).
        KLBackup.keepGameFilesOut()
    }

    var body: some Scene {
        WindowGroup(id: SurrealAimGuide.launcherID) { BootView() }
            .defaultSize(width: 1280, height: 800)
            // On the app's phase, not this window's: closing the boot window
            // while the immersive space is up is not backgrounding, and must
            // not be treated as it. The log line above every decision is what
            // makes that distinction checkable on a device rather than assumed.
            .onChange(of: scenePhase) { _, phase in Lifecycle.scenePhaseChanged(to: phase) }

        // The on-demand text-entry window (KL_KBD_WINDOW). Additive: it exists in
        // the scene graph but nothing opens it unless the flag is on and the guest
        // asks for text, so the default keyboard path (BootView's hidden field) is
        // untouched. visionOS centres a freshly opened window on the viewer's gaze,
        // which is the whole point — the hidden field is anchored to a 2D window
        // off to the side of an immersive guest, so its keyboard is never in view.
        WindowGroup(id: "kbentry") { KeyboardEntryView() }
            .defaultSize(width: 560, height: 240)
            .windowResizability(.contentSize)

        // A web page the guest asked to open (see KLRun.watchKeyboard). One
        // window per URL; closing it is the way back to the game. Never brought
        // back at launch: visionOS restores the windows that were open when an
        // app ended, and a web window restored ALONE meant the start window — the
        // one that boots the game — never came up (DreamSpace, 2026-10-16).
        WindowGroup(id: "web", for: URL.self) { $url in KleptonWebWindow(url: url) }
            .defaultSize(width: 1100, height: 760)
            .restorationBehavior(.disabled)
            .defaultLaunchBehavior(.suppressed)

        ImmersiveSpace(id: SurrealAimGuide.spaceID) { KleptonSurrealAimSpace() }
            .immersionStyle(selection: .constant(.mixed), in: .mixed)

        // "Measure room" and the room check before a start (KleptonRoomSpace.swift).
        ImmersiveSpace(id: KLRoomFlow.spaceID) { KleptonRoomSpace() }
            .immersionStyle(selection: .constant(.mixed), in: .mixed)

        // Shown for the moment the picture moves to the RealityKit space, so
        // the app always has a scene and is not sent to the background.
        WindowGroup(id: KLRealityPresent.switchWindowID) { KLRealitySwitchView() }
            .defaultSize(width: 420, height: 120)

        // KL_RK_PRESENT, a proof of concept: the same game, its picture shown
        // through RealityKit so the system's eye-tracked foveation applies
        // (KleptonRealityPresent.swift).
        ImmersiveSpace(id: KLRealityPresent.spaceID) { KleptonRealitySpace() }
            .immersionStyle(selection: .constant(.mixed), in: .mixed)
            .persistentSystemOverlays(Immersive.systemOverlays)
            .upperLimbVisibility(chroma.upperLimbVisibility)

        ImmersiveSpace(id: Immersive.id) {
            CompositorLayer(configuration: KleptonStageConfiguration()) { layerRenderer in
                // How many times this closure runs, and for which renderer. If
                // it runs twice, the loop we watch in the log is not necessarily
                // the layer being displayed — which would explain a correct pass
                // that nobody sees.
                NSLog("[cp] CompositorLayer closure #\(Immersive.bump()) "
                      + "renderer=\(ObjectIdentifier(layerRenderer))")
                KleptonCompositor.attach(layer: layerRenderer)
            }
        }
        // .mixed vs .full is read from the observed chroma object (set by boot
        // once the target is known — see KleptonChroma.immersionFull and
        // Immersive.mixed), NOT from a static read here: the scene graph is built
        // before kl_app_configure, so a static read always saw the pre-target
        // default. Observing the object means .full takes hold reactively before
        // the space opens, for the native VR kind (GTA Vice City). KL_FULL wins.
        .immersionStyle(selection: .constant(chroma.immersionFull ? .full : .mixed),
                        in: .mixed, .full)
        // See Immersive.systemOverlays. On the scene, not on a view inside it:
        // a CompositorLayer has no view hierarchy for the View-level modifier to
        // attach to, so the Scene-level one is the only one that applies here.
        .persistentSystemOverlays(Immersive.systemOverlays)
        // Whether the system mattes the user's own hands and arms over the
        // guest's picture. Read from the observed settings rather than from the
        // environment directly, so the toggle in the Matting panel takes effect
        // without a relaunch — which is the whole reason these live in a window
        // that stays open beside the space.
        .upperLimbVisibility(chroma.upperLimbVisibility)
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

/// What outlives the start window of an installed game.
///
/// A product build closes its window when the guest's picture is up
/// (`BootView.productStart`), and a window that is opened again later — tapping
/// the app in the Home View does that — is a NEW view with fresh state. So the
/// two things the window used to be the keeper of live here instead: whether the
/// guest has been started (it boots once per process), and the poll that raises
/// a keyboard when the guest asks for text.
@MainActor
final class KLRun: ObservableObject {
    static let shared = KLRun()
    @Published var started = false
    @Published var adjustingControllers = false
    @Published var up = false
    private var openWindow: OpenWindowAction?
    private var keyboard: Task<Void, Never>?

    /// The guest's text-entry requests, served without the start window: the
    /// dedicated entry window is opened in front of the viewer instead.
    func watchKeyboard(_ open: OpenWindowAction) {
        openWindow = open
        guard keyboard == nil else { return }
        keyboard = Task { @MainActor in
            while !Task.isCancelled {
                if kl_mono_text_input_wanted() != 0 {
                    kl_mono_set_text_input(0)
                    if !KbState.shared.open { self.openWindow?(id: "kbentry") }
                }
                // A web page the guest asked for (an Android VIEW intent, mapped in
                // kl_jni_android's startActivity) opens in a window of OUR OWN
                // ("web", KleptonWebWindow), not in Safari: handing it to Safari
                // sent this app to the background, which ends it
                // (KL_EXIT_ON_BACKGROUND) — DreamSpace closed the moment its
                // Spotify button opened the page (2026-10-16). A window beside the
                // immersive space keeps the game running behind it.
                var buf = [CChar](repeating: 0, count: 2048)
                if kl_mono_take_open_url(&buf, Int32(buf.count)) != 0,
                   let s = String(validatingUTF8: buf), let given = URL(string: s),
                   let url = KleptonWebWindow.allowed(given, navigation: false) {
                    NSLog("[app] opening \(KleptonWebWindow.brief(url)) in a web window")
                    self.openWindow?(id: "web", value: url)
                }
                try? await Task.sleep(for: .milliseconds(200))
            }
        }
    }
}

/// The guest's web page, in a web view of our own. Every navigation in it — the
/// first load, links, redirects, frames, pages that ask for a new window — goes
/// through kl_mono_web_url, the rule the page was opened by: https on the
/// listed hosts only, an http address upgraded. A refused one is cancelled and
/// logged; the window stays on the page it had.
struct KleptonWebWindow: UIViewRepresentable {
    let url: URL?

    func makeCoordinator() -> Navigation { Navigation() }

    func makeUIView(context: Context) -> WKWebView {
        let view = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())
        // A long press shows no preview: a preview loads the link outside the
        // navigation rule below.
        view.allowsLinkPreview = false
        view.navigationDelegate = context.coordinator
        view.uiDelegate = context.coordinator
        if let url, let ok = KleptonWebWindow.allowed(url, navigation: false) {
            view.load(URLRequest(url: ok))
        }
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {}

    /// kl_mono_web_url for a URL: the address to load, or nil (refused, logged).
    static func allowed(_ url: URL, navigation: Bool) -> URL? {
        var out = [CChar](repeating: 0, count: 4096)
        var why: UnsafePointer<CChar>? = nil
        guard kl_mono_web_url(url.absoluteString, &out, UInt(out.count),
                              navigation ? 1 : 0, &why) != 0 else {
            NSLog("[app] web window: refused \(brief(url)): \(why.map { String(cString: $0) } ?? "?")")
            return nil
        }
        return URL(string: String(cString: out))
    }

    /// A URL as the log may show it: scheme and host only, as the runtime logs
    /// one (klj_uri_brief). Path and query can carry a token or an account.
    static func brief(_ url: URL) -> String {
        "\(url.scheme ?? "?")://\(url.host ?? "")"
    }

    @MainActor
    final class Navigation: NSObject, WKNavigationDelegate, WKUIDelegate {
        // Asked for every navigation, server redirects included.
        func webView(_ webView: WKWebView,
                     decidePolicyFor action: WKNavigationAction) async -> WKNavigationActionPolicy {
            // Nothing is downloaded: a link marked as a download is refused here,
            // anything served as one below.
            if action.shouldPerformDownload { return .cancel }
            guard let url = action.request.url,
                  let ok = KleptonWebWindow.allowed(url, navigation: true) else { return .cancel }
            if url.scheme?.lowercased() == "http" {
                // Upgraded: the https address is loaded instead (in the main frame).
                if action.targetFrame?.isMainFrame ?? true { webView.load(URLRequest(url: ok)) }
                return .cancel
            }
            return .allow
        }

        // A response the window cannot show, or one sent as an attachment, would
        // become a download: refused.
        func webView(_ webView: WKWebView,
                     decidePolicyFor response: WKNavigationResponse) async -> WKNavigationResponsePolicy {
            if !response.canShowMIMEType { return .cancel }
            if let http = response.response as? HTTPURLResponse,
               let disposition = http.value(forHTTPHeaderField: "Content-Disposition"),
               disposition.lowercased().hasPrefix("attachment") { return .cancel }
            return .allow
        }

        // ...and should one start all the same, it is stopped at once.
        func webView(_ webView: WKWebView, navigationAction: WKNavigationAction,
                     didBecome download: WKDownload) { download.cancel(nil) }
        func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse,
                     didBecome download: WKDownload) { download.cancel(nil) }

        // No camera and no microphone for a page, whatever it asks
        // (requestMediaCapturePermissionForOrigin, in its async Swift form).
        func webView(_ webView: WKWebView, decideMediaCapturePermissionsFor origin: WKSecurityOrigin,
                     initiatedBy frame: WKFrameInfo,
                     type: WKMediaCaptureType) async -> WKPermissionDecision { .deny }

        // A redirect is also checked where it lands, in case one got past the above.
        func webView(_ webView: WKWebView,
                     didReceiveServerRedirectForProvisionalNavigation navigation: WKNavigation!) {
            if let url = webView.url, KleptonWebWindow.allowed(url, navigation: true) == nil {
                webView.stopLoading()
            }
        }

        // target=_blank and window.open: no second window; the page opens here,
        // by the same rule.
        func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                     for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
            // navigation: false, as for the first page: what is asked for here
            // replaces the page in the main frame, so it must be a real https page
            // on the list. The about:blank/about:srcdoc allowance is for frames a
            // page makes inside itself; a window.open("about:blank") the opener
            // then writes into would be a page outside the rule.
            if let url = action.request.url, let ok = KleptonWebWindow.allowed(url, navigation: false) {
                webView.load(URLRequest(url: ok))
            }
            return nil
        }
    }
}

// Coordinates the on-demand keyboard window. It is a separate Scene from BootView
// and cannot share @State, so a shared singleton carries the one bit BootView
// needs — is it already open — to avoid opening a second copy on the next request.
final class KbState: ObservableObject {
    static let shared = KbState()
    @Published var open = false
}

// The on-demand text-entry window (KL_KBD_WINDOW=1). Opened in front of the viewer
// when the guest asks for text; it raises the system keyboard and feeds what is
// typed to the guest through the SAME path as BootView's hidden field
// (kl_mono_commit_text for characters, KEYCODE_DEL for backspace, KEYCODE_ENTER on
// submit). Dismissing it puts the keyboard away.
struct KeyboardEntryView: View {
    private static func backspace(_ n: Int) {
        for _ in 0..<n {
            let c = kl_mono_keycode_for_char(8); kl_mono_key(1, c); kl_mono_key(0, c)
        }
    }
    @State private var text = ""
    @FocusState private var focused: Bool
    @Environment(\.dismissWindow) private var dismissWindow
    @ObservedObject private var kb = KbState.shared

    var body: some View {
        VStack(spacing: 14) {
            Text("Game text entry").font(.headline)
            Text(klT("Type here — it goes straight to the game. “Send” presses Enter for you; a name box in a menu only needs “Close”.",
                     "Hier tippen – es geht direkt ans Spiel. „Abschicken“ drückt die Eingabetaste für dich; ein Namensfeld in einem Menü braucht nur „Schließen“."))
                .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
            TextField("", text: $text)
                .focused($focused)
                .textFieldStyle(.roundedBorder)
                .onChange(of: text) { old, new in
                    if new.count < old.count {
                        for _ in 0..<(old.count - new.count) {
                            let c = kl_mono_keycode_for_char(8); kl_mono_key(1, c); kl_mono_key(0, c)
                        }
                    } else if new.count > old.count {
                        String(new.dropFirst(old.count)).withCString { kl_mono_commit_text($0) }
                    }
                    if new.count > 128 { text = "" }   // a keyboard, not a buffer
                }
                .onSubmit {
                    let c = kl_mono_keycode_for_char(10); kl_mono_key(1, c); kl_mono_key(0, c)  // Enter
                }
            // What is already in the game's field was not typed here, so this
            // field starts empty and its delete key has nothing to remove: a name
            // box that opens with "Player" in it could only be added to. These two
            // send the game's own backspace, whatever this field holds.
            HStack(spacing: 12) {
                // With text in this field its own change handler sends the backspace.
                Button(klT("Delete one", "Ein Zeichen löschen")) {
                    if text.isEmpty { Self.backspace(1) } else { text.removeLast() }
                }
                Button(klT("Clear the game's field", "Feld im Spiel leeren")) { Self.backspace(64); text = "" }
            }
            // A game that reads a line – a chat message, a value for a setting – acts on it only when the Enter
            // key arrives; closing this window is not that. The field's own Return key sends it too, but with a
            // virtual keyboard that key is easy to miss, so here it is as a button that also closes the window.
            HStack(spacing: 12) {
                Button(klT("Send (Enter)", "Abschicken (Eingabetaste)")) {
                    let c = kl_mono_keycode_for_char(10); kl_mono_key(1, c); kl_mono_key(0, c)
                    dismissWindow(id: "kbentry")
                }
                Button(klT("Close", "Schließen")) { dismissWindow(id: "kbentry") }
            }
        }
        .padding(28)
        .frame(minWidth: 480)
        .onAppear { text = ""; focused = true; kb.open = true }
        .onDisappear { kb.open = false }
    }
}

/// The three things a player may want to change, in words that do not need the
/// port explained: how the picture is rendered, how large, and whether their
/// own hands show. All of it is read once when the game starts, which the page
/// says once instead of under every control.
struct PlayerSettingsView: View {
    @AppStorage(KleptonControllerMode.preferenceKey) private var controllerMode = KleptonControllerMode.selected().rawValue
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var chroma = KleptonChroma.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            KleptonControllerSettingsView()
            // Alyx's page shows AlyxPanel instead of this view, with these in its own boxes.
            if !klIsAlyx() {
            GroupBox(klT("Picture", "Bild")) {
                VStack(alignment: .leading, spacing: 10) {
                    // A Vulkan game has the level as a choice (kl_fove); the
                    // switch is the GL path's and does nothing for it.
                    if rendering.offersVkFoveation {
                        HStack {
                            Text(klT("Foveated rendering", "Foveated Rendering"))
                            Picker(klT("Foveated rendering", "Foveated Rendering"), selection: $rendering.vkFoveation) {
                                Text(klT("Off", "Aus")).tag("off")
                                Text(klT("Light", "Leicht")).tag("low")
                                Text(klT("Medium", "Mittel")).tag("medium")
                                Text(klT("Strong", "Stark")).tag("high")
                                Text(klT("Very strong", "Sehr stark")).tag("ultra")
                            }
                            .pickerStyle(.segmented)
                            .labelsHidden()
                        }
                        Text(klT("Keeps the middle of the picture sharp and draws the edges more coarsely, so the game runs more smoothly. The stronger the level, the smaller the sharp middle and the coarser the edge.",
                                 "Hält die Bildmitte scharf und zeichnet die Ränder gröber, damit das Spiel flüssiger läuft. Je stärker die Stufe, desto kleiner die scharfe Mitte und desto gröber der Rand."))
                            .font(.callout).foregroundStyle(.secondary)
                    } else {
                    Toggle(klT("Foveated rendering", "Foveated Rendering"), isOn: $rendering.foveation)
                    Text(klT("Keeps the middle of the picture sharp and draws the edges more coarsely. Games run more smoothly with it.",
                             "Hält die Bildmitte scharf und zeichnet die Ränder gröber. Spiele laufen damit flüssiger."))
                        .font(.callout).foregroundStyle(.secondary)
                    }
                    if KleptonRendering.targetHasMSAA {
                        HStack {
                            Text(klT("Edge smoothing", "Kantenglättung"))
                            Picker(klT("Edge smoothing", "Kantenglättung"), selection: $rendering.msaa) {
                                Text(klT("Off", "Aus")).tag(0)
                                Text("2x").tag(2)
                                Text("4x").tag(4)
                            }
                            .pickerStyle(.segmented)
                            .labelsHidden()
                        }
                        Text(klT("Smooths jagged edges (MSAA). It costs frame rate: at full resolution the game can drop to 60 Hz with it, so lower the resolution along with it.",
                                 "Glättet gezackte Kanten (MSAA). Kostet Bildrate: Bei voller Auflösung kann das Spiel damit auf 60 Hz fallen, also die Auflösung dazu verringern.")
                             + (KleptonRendering.targetIsBeatSaber
                                ? klT(" From Beat Saber 1.45 on.", " Ab Beat Saber 1.45.") : ""))
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    // The same setting for a game that smooths its edges by
                    // itself, with fewer samples (a Vulkan game; see
                    // KleptonRendering.vkMsaa for when it is known).
                    if rendering.offersVkMsaa {
                        HStack {
                            Text(klT("Edge smoothing", "Kantenglättung"))
                            Picker(klT("Edge smoothing", "Kantenglättung"), selection: $rendering.vkMsaa) {
                                Text(klT("As the game (\(KleptonRendering.vkGuestSamples)x)",
                                         "Wie das Spiel (\(KleptonRendering.vkGuestSamples)x)")).tag(0)
                                Text("4x").tag(4)
                            }
                            .pickerStyle(.segmented)
                            .labelsHidden()
                        }
                        Text(klT("Smooths jagged edges more than the game does by itself (MSAA). It costs some graphics time and memory, and the first start after a change compiles the game's shaders again, which stutters for a while. If the picture shows faults, set it back.",
                                 "Glättet gezackte Kanten stärker, als das Spiel es von sich aus tut (MSAA). Kostet etwas Grafikzeit und Speicher, und beim ersten Start nach einer Änderung werden die Shader des Spiels neu übersetzt, was eine Weile ruckelt. Zeigt das Bild Fehler, wieder zurückstellen."))
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    PicturePrimary(hintFont: .callout)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            }
            // For every game that opens an immersive space, Alyx included —
            // except a mixed-reality one, whose room needs the mixed space.
            if Immersive.wanted, !KleptonRendering.targetIsMixedReality {
                GroupBox(klT("Frame rate", "Bildrate")) {
                    FrameRatePicker().frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            // Frame generation (KleptonFrameGen), for every game that shows a picture of its own – among
            // the main settings, where its owner wanted it ("nicht erst bei erweitert"). In the game a
            // tap of Create + Options with the performance panel shown cycles the three, to compare
            // (KleptonTapAction: unless the game's setting has the tap do something else). A mixed-reality
            // game too: it had been left out on the guess that a moved in-between moves the edges to the room
            // as well, never tried – they move with the picture's alpha, as the things in them do. Its owner
            // asked for it with LEGO Bricktales (2026-10-29).
            if Immersive.wanted {
                GroupBox(klT("Frame generation", "Zwischenbilder")) {
                    FrameGenPicker().frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            // Alyx hides the player's arms whatever this says (see
            // KleptonChroma.upperLimbVisibility), so the switch would be a lie there.
            if !klIsAlyx() {
                GroupBox(klT("Your hands", "Deine Hände")) {
                    VStack(alignment: .leading, spacing: 10) {
                        Toggle(klT("Show my real hands over the game", "Meine echten Hände über dem Spiel zeigen"),
                               isOn: $chroma.settings.handMatting)
                        Text(klT("Most games draw their own hands or controllers, so your real ones would be a second pair in the same place.",
                                 "Die meisten Spiele zeichnen eigene Hände oder Controller – deine echten wären dann ein zweites Paar an derselben Stelle."))
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            if controllerMode == KleptonControllerMode.standard.rawValue {
                GroupBox(klT("Game controller", "Gamecontroller")) { GamepadStatusView() }
            }
            Label(klT("Changes to the picture apply the next time you start the game.",
                      "Änderungen am Bild gelten ab dem nächsten Start des Spiels."),
                  systemImage: "info.circle")
                .font(.callout).foregroundStyle(.secondary)
        }
    }
}

/// The frame-rate choice (KleptonRendering.maxRate): its picker and what it does.
struct FrameRatePicker: View {
    @ObservedObject private var rendering = KleptonRendering.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker(klT("Frame rate", "Bildrate"), selection: $rendering.maxRate) {
                Text(klT("Up to 100", "Bis 100")).tag(100)
                Text(klT("Up to 120", "Bis 120")).tag(120)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            Text(klT("Up to 100 is the default. Up to 120 is smoother where the game manages it and the Vision Pro's display can do it. When a game cannot keep up, the Vision Pro halves the rate for a while: to 50 from 100, to 60 from 120.",
                     "Bis 100 ist der Standard. Bis 120 ist flüssiger, wo das Spiel es schafft und das Display der Vision Pro es kann. Kommt ein Spiel nicht mit, halbiert die Vision Pro die Rate eine Weile: von 100 auf 50, von 120 auf 60."))
                .font(.callout).foregroundStyle(.secondary)
        }
    }
}

/// The frame-generation choice (KleptonRendering.frameGen): its three modes and what they do.
struct FrameGenPicker: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker(klT("Frame generation", "Zwischenbilder"), selection: $rendering.frameGen) {
                Text(klT("Off", "Aus")).tag(KleptonFrameGen.Mode.off.rawValue)
                Text(klT("Ahead", "Vorausberechnet")).tag(KleptonFrameGen.Mode.ahead.rawValue)
                Text(klT("In between", "Dazwischen")).tag(KleptonFrameGen.Mode.between.rawValue)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            Text(klT("For a game that draws fewer pictures than the display shows. Ahead: where the game has no new picture, one is made from its last two, moved on to the moment it is shown. In between: the middle between the game's last two pictures, as DLSS makes them – smoother, and what the game draws comes 10 ms later; the head's movement stays as quick. With the performance panel shown (Create and Options held together), a short tap of the two cycles the three – what the tap does can be set under the panel's switch.",
                     "Für ein Spiel, das weniger Bilder zeichnet, als das Display zeigt. Vorausberechnet: Wo das Spiel kein neues Bild hat, wird eins aus seinen letzten beiden errechnet, bis zu dem Moment weitergerückt, in dem es gezeigt wird. Dazwischen: die Mitte zwischen den letzten beiden Bildern des Spiels, wie DLSS sie macht – weicher, und was das Spiel zeichnet, kommt 10 ms später; die Kopfbewegung bleibt so schnell. Bei eingeblendeter Leistungsanzeige (Create und Options zusammen gehalten) wechselt ein kurzer Tipp auf beide zwischen den dreien – was der Tipp tut, lässt sich unter dem Schalter der Anzeige einstellen."))
                .font(.callout).foregroundStyle(.secondary)
            // The compositor makes no in-between picture where MetalFX swapped the eye's picture
            // (encodeComposite): the two would fight over the swap.
            if rendering.frameGen != KleptonFrameGen.Mode.off.rawValue, metalfx.enabled {
                Text(klT("While MetalFX upscaling is on, no in-between pictures are made – choose one of the two.",
                         "Solange MetalFX-Hochskalieren an ist, entstehen keine Zwischenbilder – eins von beiden wählen."))
                    .font(.callout).foregroundStyle(.orange)
            }
        }
    }
}

/// Whether an ordinary game controller is connected, and which of its buttons stands for which.
/// Looked up again every two seconds: a controller is paired and switched on while the page is
/// open as often as before it.
struct GamepadStatusView: View {
    var body: some View {
        TimelineView(.periodic(from: .now, by: 2)) { _ in
            VStack(alignment: .leading, spacing: 10) {
                if let name = KleptonControllers.connectedGamepadName() {
                    Label(klT("\(name) is connected.", "\(name) ist verbunden."), systemImage: "gamecontroller.fill")
                } else {
                    Label(klT("No game controller connected. Pair one under Settings › Bluetooth on the Vision Pro.",
                              "Kein Gamecontroller verbunden. Koppeln unter Einstellungen › Bluetooth auf der Vision Pro."),
                          systemImage: "gamecontroller")
                        .foregroundStyle(.secondary)
                }
                // The four face buttons are read off this game's own
                // layout, so the sentence cannot say something the
                // controller does not do.
                let m = KleptonControllers.faceMap
                let faces = "✕ = \(m.cross.uppercased()), ○ = \(m.circle.uppercased()), □ = \(m.square.uppercased()), △ = \(m.triangle.uppercased())"
                Text(klT("An ordinary controller works together with your hands: your hands show the game where they are and pinch to grab, the controller adds the sticks and buttons. Both sticks work as they are. The buttons stand for those of the VR controllers: \(faces); L2 and R2 are the triggers, L1 and R1 the grip buttons, Options the menu.",
                         "Ein gewöhnlicher Controller arbeitet mit deinen Händen zusammen: Die Hände zeigen dem Spiel, wo sie sind, und greifen per Fingertipp; der Controller bringt Sticks und Tasten mit. Beide Sticks gelten unverändert. Die Tasten stehen für die der VR-Controller: \(faces); L2 und R2 sind die Trigger, L1 und R1 die Grifftasten, Options das Menü."))
                    .font(.callout).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

struct BootView: View {
    @State private var log = ""
    @AppStorage(KleptonDiagnostics.waitersKey) private var diagWaiters = false
    @State private var status = "idle"
    @State private var running = false
    @State private var finished = false
    @State private var succeeded = false
    @State private var openedSpace = false
    /// "Preparing graphics" (kl_app_prepare_progress): pipelines compiled of
    /// pipelines to compile, while a start is doing that; a total of 0 otherwise.
    @State private var prepDone = 0
    @State private var prepTotal = 0
    @State private var prepTimer: Timer?
    @ObservedObject private var steamCloud = KLSteamCloud.shared
    /// The guest's data is not in the container yet: show the data panel
    /// instead of booting into Unity's "not enough storage" three layers up.
    @State private var needsData = false
    // The flat guest's picture, and the handoff that ends it. Polled rather
    // than pushed: both are C state written by a guest thread, and a callback
    // out of one into SwiftUI would be a main-actor hop from a thread that is
    // mid-frame. A timer at 5 Hz costs nothing and cannot deadlock.
    @State private var showShell = false
    @State private var handedOff = false
    // The visionOS system keyboard for an immersive guest: there is no on-screen
    // keyboard in an immersive space, so when the guest asks for text entry
    // (kl_mono_text_input_wanted, set by SDLActivity.showTextInput) we focus a
    // hidden field to raise the system keyboard and feed what is typed back
    // through kl_mono_key. cs1 needs this to name a local server.
    @State private var kbTyped = ""
    @State private var kbSuppress = false
    @FocusState private var kbFocused: Bool
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    // The start screen of an installed game — see `startScreen`.
    // KL_START_SETTINGS=1 opens it on the settings page, so a scripted
    // launch can photograph the settings without a hand to tap the button.
    @State private var showSettings = klEnvOn("KL_START_SETTINGS", default: false)
    /// The window stays open beside the game with the hand-position sliders
    /// (HandTuneView) instead of going away once the picture is up.
    @State private var handTune = false
    /// KL_HAND_TUNE=1. Written for LES MILLS BODYCOMBAT, whose hands sat a
    /// little above the player's real ones (on device, 2026-10-15): its owner
    /// moved them live, in the game, and the values are its default now
    /// (KLSenseTune.targetHand).
    static let handTuneAfterStart: Bool = klEnvOn("KL_HAND_TUNE", default: false)
    @ObservedObject private var run = KLRun.shared
    /// The play area (KLRoomGuard): the room space's state, and "no play area yet".
    @ObservedObject private var roomFlow = KLRoomFlow.shared
    @State private var roomMissing = false
    @State private var roomFailed = ""
    /// Watched so that "Measure room" follows the choice under Start (RoomQuickSettings).
    @AppStorage(KLRoomGuard.enabledKey) private var roomOn = KLRoomGuard.installDefault
    @AppStorage(KLRoomGuard.styleKey) private var roomStyle = KLRoomGuard.Style.roomscale.rawValue
    /// "Measure room" is for room scale only. Reads the two settings above, so the
    /// button follows the choice at once (KL_ROOM_GUARD/KL_ROOM_STYLE still win).
    private var canMeasureRoom: Bool {
        _ = roomOn; _ = roomStyle
        return KLRoomGuard.enabled && KLRoomGuard.style == .roomscale
    }
    /// Whether this build fronts as a game (a picture, its name, Start and
    /// Settings) rather than as the boot report. Asked for by whoever built it
    /// (gen_xcodeproj.py, KLEPTON_START_SCREEN) and not by the target: the same
    /// guest is a product when an installer put it there and a subject of
    /// measurement when a developer did.
    static let productStart: Bool =
        (Bundle.main.object(forInfoDictionaryKey: "KLStartScreen") as? String) == "1"
    private static var productTitle: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
            ?? klLauncherTitle() ?? klTargetName()
    }
    // The game-facing builds (hl1/hl2/portal) show a launcher: a chosen game
    // folder plus per-title options, and boot on a button rather than on their own.
    // Observe the active files object so the Start button tracks file readiness;
    // the fallback is inert (no such bookmark) for the runtime-debugging targets.
    @ObservedObject private var files = klActiveFiles() ?? LauncherFiles(bookmarkKey: "none", expected: "")
    private var isLauncher: Bool { klIsLauncher() }
    /// Whether this target boots as soon as the window is up. See the `.task`.
    private static let targetAutoBoot: Bool =
        (Bundle.main.object(forInfoDictionaryKey: "KLAutoBootDefault") as? String) != "0"
    // The microphone dial only appears where voice is actually used: Steam Link's
    // stream and the Xash titles that name a local server. Every other build hides it.
    private var showMic: Bool {
        ["steamlink-vr", "hl1", "cs1"].contains(klTargetName())
    }
    // Walkabout's multiplayer rooms have voice chat (Photon Voice, AudioInAEC in
    // kl_jni_services.c), and there the switch is a player's, not a tinkerer's:
    // it sits with the game's own settings rather than under Advanced.
    private var micUpFront: Bool { klTargetName() == "walkabout" }

    var body: some View {
        Group {
            if showShell { ShellWindow() }
            else if Self.productStart { startScreen }
            else { bootReport }
        }
        .task { await watchPresentation() }
        // The room check before a start: recognised, or measured and saved, and the game starts.
        .onChange(of: roomFlow.phase) { _, phase in
            switch phase {
            case .done(let outcome):
                guard roomFlow.purpose == .check else { return }
                if outcome == .saved || outcome == .recognised || outcome == .stationary {
                    roomMissing = false
                    Task {
                        // The room space has to be gone before the game's can open: the
                        // boot takes about a second, and opening the game's space while
                        // the room's was still up failed three times and left the start
                        // hanging (2026-10-17). Closed from here and waited for.
                        await dismissImmersiveSpace()
                        try? await Task.sleep(for: .milliseconds(300))
                        boot()
                    }
                }
            case .failed(let message):
                roomFailed = message
            default:
                break
            }
        }
        // Steam Link streams over the local network; see KLLocalNetwork.
        .task { if klTargetName() == "steamlink-vr" { KLLocalNetwork.ask() } }
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

            // Raise the system keyboard on EACH text-entry request. The guest
            // calls showTextInput every time a field is clicked, so this is
            // edge-triggered: consume the flag (set it back to 0) and (re-)focus,
            // so the next click is a fresh request that re-raises the keyboard —
            // even after the person dismissed it, and for a second field while the
            // first is still up. Clearing kbTyped for the new field must not look
            // like a deletion to onChange, hence the suppress flag.
            if kl_mono_text_input_wanted() != 0 {
                kl_mono_set_text_input(0)
                if klEnvOn("KL_KBD_WINDOW", default: false) {
                    // Gaze-centred window path: open the dedicated entry window in
                    // front of the viewer (only if one is not already up). It raises
                    // the keyboard itself on appear. This is the fix for an immersive
                    // guest, whose 2D boot window (and its keyboard) is out of view.
                    if !KbState.shared.open { openWindow(id: "kbentry") }
                } else {
                    if !kbTyped.isEmpty { kbSuppress = true; kbTyped = "" }
                    // Force a focus TRANSITION. @FocusState set true==true is a no-op,
                    // so if the field is still state-focused but the keyboard was put
                    // away (tapped off / a second field click), re-setting true leaves
                    // the keyboard DOWN — the "worked once, then couldn't keep writing"
                    // flakiness. Drop focus, let SwiftUI render that, then re-take it so
                    // the system re-raises the keyboard every time.
                    if kbFocused {
                        kbFocused = false
                        try? await Task.sleep(for: .milliseconds(60))
                    }
                    kbFocused = true
                }
            }

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
                    // The same rule as boot()'s path: once the picture is up the
                    // start window has done its job. Without this the Steam Link
                    // card ("Starting…", Settings) stayed floating in front of
                    // the stream, because a handoff never passes through boot().
                    if case .opened = r, Self.productStart {
                        KLRun.shared.up = true
                        KLRun.shared.watchKeyboard(openWindow)
                        showSettings = false
                        dismissWindow()
                    }
                }
            }
            try? await Task.sleep(for: .milliseconds(200))
        }
    }

    // One typed character to the guest, as the Android keycode SDL expects. The
    // key list is kl_mono_keycode_for_char, shared with the flat-window shell.
    private func kbSend(_ ch: Int32) {
        let code = kl_mono_keycode_for_char(ch)
        guard code != 0 else { return }
        kl_mono_key(1, code)
        kl_mono_key(0, code)
    }

    /// The front of an installed game: its picture, its name, Start, Settings.
    ///
    /// Everything the boot report shows is still here — the same state, the same
    /// `boot()` — but behind the Settings button, because none of it is what a
    /// person who tapped a game wants to read. Two rules differ from the report:
    ///
    ///  - Games normally wait for Start so settings can be changed first.
    ///    BRINK's installed build auto-starts; KL_AUTOBOOT=0 restores its button.
    ///  - **The window goes away once the picture is up** — see where
    ///    `openImmersiveSpace` returns. It stays if the start fails, because
    ///    then it is the only place the reason can be read.
    private var startScreen: some View {
        Group {
            // In the window rather than over it: a sheet on visionOS is a
            // fixed, narrow card, and the dials in it were unreadably small.
            if handTune {
                HandTuneView(onClose: { handTune = false; dismissWindow() })
            } else if showSettings { settingsPage } else { startFront }
        }
        .task {
            // BRINK opens as a game from its Home icon; explicit overrides remain available.
            if !isLauncher, klEnvOn("KL_AUTOBOOT", default: klTargetName() == "x3635172946605196") { boot() }
        }
    }

    /// What is installed, for the corner of the start screen: the game's version,
    /// its version code and the toolchain the app was built with. The installer
    /// stamps the bundle "<version code>.<toolchain>" (KLEPTON_APP_BUILD); a
    /// build stamped any other way shows what it has. Asked for by the owner
    /// once a game could be installed in more than one version: "dann sieht man
    /// was man installiert hat".
    private static let versionStamp: String? = {
        let info = Bundle.main.infoDictionary
        guard let short = info?["CFBundleShortVersionString"] as? String, !short.isEmpty else { return nil }
        let build = (info?["CFBundleVersion"] as? String) ?? ""
        let parts = build.split(separator: ".")
        if parts.count == 2, parts.allSatisfy({ $0.allSatisfy(\.isNumber) }) {
            return "\(short) (\(parts[0])) · Toolchain \(parts[1])"
        }
        return build.isEmpty || build == short ? short : "\(short) (\(build))"
    }()

    private var startFront: some View {
        // Measured against the WINDOW, not against the content: the hero picture
        // fills the window by overflowing it (scaledToFill), which makes the
        // content wider than what is visible, and a corner of that is outside
        // the window. On the owner's headset the line ended at "Toolchain", the
        // number cut off, even after it had been moved in from the corner.
        GeometryReader { window in
            startFrontContent
                .frame(width: window.size.width, height: window.size.height)
                .clipped()
                // Small and out of the way: nobody starts a game to read this, but
                // it is the one place on the headset that says which version is on it.
                .overlay(alignment: .bottomTrailing) {
                    if let stamp = Self.versionStamp {
                        Text(stamp)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .fixedSize()
                            // In from the corner by more than its radius: a visionOS
                            // window's corners are rounded (about 46 pt).
                            .padding(.trailing, 48)
                            .padding(.bottom, 20)
                            .accessibilityLabel(klT("Installed version \(stamp)", "Installierte Fassung \(stamp)"))
                    }
                }
        }
    }

    private var startFrontContent: some View {
        ZStack(alignment: .bottom) {
            if let hero = UIImage(named: "StartHero") {
                Image(uiImage: hero)
                    .resizable()
                    .scaledToFill()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .clipped()
                    .overlay(LinearGradient(colors: [.clear, .black.opacity(0.8)],
                                            startPoint: .center, endPoint: .bottom))
            }
            VStack(spacing: 22) {
                Spacer()
                Text(Self.productTitle)
                    .font(.extraLargeTitle.bold())
                    .shadow(radius: 10)
                // Only when there is something to say: a start that failed, or
                // files that are not on the device.
                if finished, !succeeded {
                    // The sentence, not `status`: that is a path or an error
                    // code, as wide as the window and no use to a player. It
                    // is under Diagnostics, where the log beside it explains it.
                    Label(klT("The game could not be started.", "Das Spiel konnte nicht gestartet werden."),
                          systemImage: "exclamationmark.triangle.fill")
                        .font(.headline)
                        .foregroundStyle(.yellow)
                    Text(klT("Settings → Advanced → Diagnostics has the details. Close the app and open it again to retry.",
                             "Unter Einstellungen → Erweitert → Diagnose stehen die Einzelheiten. Für einen neuen Versuch die App schließen und wieder öffnen."))
                        .font(.callout).foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                if needsData, let files = KLGameFiles.current {
                    GameDataView(files: files) {
                        needsData = false
                        boot()
                    }
                }
                HStack(spacing: 18) {
                    // A window opened again while the guest runs has fresh state,
                    // so "already started" is asked of the process, not of it.
                    let started = running || finished || run.started
                    Button {
                        if isLauncher, !klLauncherApply() { return }
                        startOrCheckRoom()
                    } label: {
                        let failed = finished && !succeeded
                        Label(run.up ? klT("Running", "Läuft") : failed ? klT("Not started", "Nicht gestartet")
                                  : (started ? klT("Starting…", "Startet …") : klT("Start", "Start")),
                              systemImage: run.up ? "checkmark.circle.fill" : failed ? "xmark" : "play.fill")
                            .frame(minWidth: 180)
                            .foregroundStyle(started ? Color.secondary : Color.black)
                    }
                    // White, whatever the picture behind it: the one button
                    // that has to be found first on every game's art.
                    .buttonStyle(.borderedProminent)
                    .tint(.white)
                    .controlSize(.extraLarge)
                    .disabled(started || run.adjustingControllers || roomFlow.active
                              || (isLauncher && !files.canLaunch))
                    Button {
                        showSettings = true
                    } label: {
                        Label(klT("Settings", "Einstellungen"), systemImage: "gearshape")
                    }
                    .controlSize(.extraLarge)
                    .disabled(roomFlow.active)
                    // Beside Settings, as a Quest asks for its boundary before a game.
                    // Always there, so the row keeps its shape: greyed out while the
                    // game starts or runs, and when the play area is off or stationary
                    // (the owner, 2026-10-17: hidden on Start it looked broken).
                    Button {
                        openRoom(.setup)
                    } label: {
                        Label(klT("Measure room", "Raum vermessen"), systemImage: "square.dashed")
                    }
                    .controlSize(.extraLarge)
                    .disabled(!canMeasureRoom || started || roomFlow.active)
                    if running { ProgressView() }
                }
                // Room scale / stationary / off, and wall / fade out, without going into Settings.
                // Stays in place once Start is pressed, greyed out, so nothing jumps.
                RoomQuickSettings()
                    .disabled(running || finished || run.started || roomFlow.active)
                if roomFlow.active && roomFlow.quiet {
                    // The check before the start, finding the room: shown only here (KLRoomFlow.quiet).
                    VStack(spacing: 10) {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text(klT("Finding the room …", "Raum wird erkannt …"))
                                .font(.callout).foregroundStyle(.secondary)
                        }
                        HStack(spacing: 14) {
                            Button(klT("Play stationary / seated", "Stationär / sitzend spielen")) { roomFlow.useStationary() }
                            Button(klT("Cancel", "Abbrechen"), role: .cancel) { roomFlow.cancel() }
                        }
                        .controlSize(.small)
                    }
                } else if roomFlow.active {
                    Label(klT("The room is being measured — follow the card in the room.",
                              "Der Raum wird vermessen – folge der Karte im Raum."),
                          systemImage: "square.dashed")
                        .font(.callout).foregroundStyle(.secondary)
                } else if roomMissing {
                    VStack(spacing: 10) {
                        Label(klT("There is no play area yet. Set one up first — once, for all games — then the game starts.",
                                  "Es gibt noch keinen Spielbereich. Leg zuerst einen an – einmal, für alle Spiele –, dann startet das Spiel."),
                              systemImage: "exclamationmark.triangle.fill")
                            .font(.headline).foregroundStyle(.yellow)
                            .multilineTextAlignment(.center)
                        HStack(spacing: 14) {
                            Button(klT("Set up the play area", "Spielbereich anlegen")) { openRoom(.check) }
                                .buttonStyle(.borderedProminent)
                            // Standing on the spot or seated: nothing to measure.
                            Button(klT("Play stationary / seated", "Stationär / sitzend spielen")) {
                                KLRoomGuard.setStyle(.stationary)
                                roomMissing = false
                                boot()
                            }
                        }
                    }
                } else if !roomFailed.isEmpty {
                    Text(roomFailed).font(.callout).foregroundStyle(.yellow).multilineTextAlignment(.center)
                }
                KLSteamCloudStatus()
                if running, steamCloud.syncing {
                    Text(klT("Bringing your saves in step with Steam Cloud …",
                             "Spielstände werden mit der Steam Cloud abgeglichen …"))
                        .font(.callout).foregroundStyle(.secondary)
                }
                if running, prepTotal > 0 {
                    VStack(spacing: 6) {
                        ProgressView(value: Double(min(prepDone, prepTotal)), total: Double(prepTotal))
                            .frame(maxWidth: 420)
                        Text(klT("Preparing graphics: \(prepDone) of \(prepTotal). This takes a few minutes the first time and a few seconds after that; the game then starts by itself.",
                                 "Grafik wird vorbereitet: \(prepDone) von \(prepTotal). Das dauert beim ersten Mal einige Minuten, danach nur Sekunden; das Spiel startet anschließend von selbst."))
                            .font(.callout).foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                }
                if run.up {
                    Button(klT("Close this window", "Dieses Fenster schließen")) { dismissWindow() }
                        .font(.callout)
                }
            }
            .padding(40)
        }
    }

    /// What Settings opens: the dials that matter to a player first, the
    /// instruments after them, and the report last.
    private var settingsPage: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    if isLauncher { LauncherPanel() }
                    // Alyx's page is its own (AlyxPanel), down to its language, which
                    // the game picks itself (AlyxLanguage) and the general one does not reach.
                    if klIsAlyx() { AlyxPanel(started: running || finished || run.started) }
                    else { PlayerSettingsView() }
                    RoomSettingsView(measure: { showSettings = false; openRoom(.setup) })
                    PlayerNameView()
                    if !klIsAlyx() { GameLanguageView() }
                    WalkaboutUnlockView()
                    ModCreditsView()
                    if micUpFront { GroupBox { MicView() } }
                    // Everything a player does not need to understand to play: the
                    // dials of the port itself, and the report of this run.
                    DisclosureGroup(klT("Advanced", "Erweitert")) {
                        VStack(alignment: .leading, spacing: 14) {
                            if klIsAlyx() {
                                GroupBox(klT("Performance panel", "Leistungsanzeige")) { AlyxPerformancePanelView() }
                            }
                            GroupBox(klT("Picture, in detail", "Bild, im Einzelnen")) {
                                VStack(alignment: .leading, spacing: 14) {
                                    RenderingView()
                                    if klIsAlyx() { AlyxLeanDepthView() }
                                }
                            }
                            // Not for Alyx: its own KL_OVRP_EYE_MAX (klAlyxApply) would win over it.
                            if !klIsAlyx() {
                                GroupBox(klT("Max resolution per eye", "Maximale Auflösung je Auge")) { EyeMaxView() }
                            }
                            if KLRealityPresentSetting.offered {
                                GroupBox(klT("Eye-tracked sharpness", "Schärfe nach Blick")) { RealityPresentView() }
                            }
                            DisclosureGroup(klT("Passthrough and chroma key", "Durchsicht und Chroma-Key")) { ChromaView() }
                            DisclosureGroup(klT("Controller alignment", "Ausrichtung der Controller")) { TuningView() }
                            if showMic {
                                DisclosureGroup(klT("Microphone", "Mikrofon")) { MicView() }
                            }
                            if KleptonTestSwitches.available {
                                DisclosureGroup(klT("Test", "Test")) { TestSwitchesView() }
                            }
                            DisclosureGroup(klT("Diagnostics", "Diagnose")) {
                                VStack(alignment: .leading, spacing: 10) {
                                    Toggle(klT("Record what the game waits for", "Aufzeichnen, worauf das Spiel wartet"),
                                           isOn: $diagWaiters)
                                    Text(klT("For a game that stops in a loading screen. Every few seconds the log notes which parts of the game are waiting for what, and a report of all of them is written when you leave the game. Leave the game while it is stuck and pass the log on before you start it again. Takes effect the next time the app starts.",
                                             "Für ein Spiel, das in einem Ladebildschirm stehen bleibt. Alle paar Sekunden steht im Protokoll, welche Teile des Spiels worauf warten, und beim Verlassen wird ein Bericht über alle geschrieben. Das Spiel verlassen, während es hängt, und das Protokoll weitergeben, bevor es neu gestartet wird. Wirkt beim nächsten Start der App."))
                                        .font(.caption).foregroundStyle(.secondary)
                                    Text(status).font(.footnote).foregroundStyle(.secondary)
                                    if !log.isEmpty {
                                        ShareLink(item: log) {
                                            Label(klT("Export log", "Protokoll weitergeben"), systemImage: "square.and.arrow.up")
                                        }
                                    }
                                    Text(log.isEmpty ? klT("Nothing has been logged yet.", "Noch nichts protokolliert.") : String(log.suffix(6000)))
                                        .font(.system(.caption2, design: .monospaced))
                                        .textSelection(.enabled)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                }
                            }
                        }
                        .padding(.top, 10)
                    }
                }
                .padding(.horizontal, 40).padding(.vertical, 24)
            }
            .navigationTitle(Self.productTitle)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(klT("Done", "Fertig")) { showSettings = false }
                }
            }
        }
    }

    private var bootReport: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                if let title = klLauncherTitle() {
                    // A game build presents as the game, not the runtime.
                    Text(title).font(.largeTitle.bold())
                } else {
                    Text("Klepton").font(.largeTitle.bold())
                    // The guest, by name. Two apps are built from this tree and they
                    // look identical from the front; a boot log that does not say
                    // which one produced it is a log that can be read as the other's.
                    Text(klTargetName())
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if finished {
                    Label(succeeded ? (klIsAlyx() ? "started" : "initJni completed") : status,
                          systemImage: succeeded ? "checkmark.circle.fill" : "xmark.octagon.fill")
                        .foregroundStyle(succeeded ? .green : .red)
                }
            }

            if needsData, let files = KLGameFiles.current {
                GameDataView(files: files) {
                    needsData = false
                    boot()
                }
                Divider()
            }

            ScrollView {
                Text(log.isEmpty ? (klIsAlyx() ? "Choose a picture setting, then press Start."
                                               : "Press Boot to load the guest chain.") : log)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: .infinity)

            // The game build's launcher: a chosen game folder plus that title's
            // options (hl1's Xash panel; hl2/portal's thinner one). See
            // KleptonLauncher.swift / KleptonHL1.swift.
            if isLauncher { LauncherPanel() }

            // Half-Life: Alyx's picture-against-smoothness choice, above the Start
            // button because it is read once, when the run starts.
            if klIsAlyx() { AlyxPanel(started: running || finished) }

            // Resolution and MetalFX in view, as on a player's settings page:
            // the two dials of the picture's size, not folded away in Rendering.
            // Alyx has them under its Graphics' "Custom".
            if !klIsAlyx() {
                GroupBox(klT("Picture", "Bild")) { PicturePrimary(hintFont: .caption) }
                    .font(.callout)
            }

            // The controller-alignment sliders, in THIS window because it stays
            // open beside the immersive space — so the guest keeps rendering
            // while they move, which is the entire point of them. Collapsed by
            // default: it is a tuning instrument, not part of booting.
            DisclosureGroup("Controller alignment") { TuningView() }
                .font(.callout)

            // ...and the matting dials, the same way and in the same window.
            // Collapsed by default for the same reason: a key colour is found
            // once for a room and then left alone.
            DisclosureGroup("Matting") { ChromaView() }
                .font(.callout)

            // ...and foveated rendering, which is a preference rather than a
            // dial: off unless turned on, and read once at boot.
            DisclosureGroup("Rendering") { RenderingView() }
                .font(.callout)
            PlayerNameView()
            GameLanguageView()

            // ...and the microphone opt-in, same window, collapsed by default.
            // Off unless a person turns it on: it is the one dial that opens a
            // privacy surface and changes the audio session out from under the
            // music, so it never engages on its own. Hidden entirely except on the
            // targets that actually use voice — Steam Link and the Xash titles that
            // name a local server — so no other build shows a mic control it can't use.
            if showMic {
                DisclosureGroup("Microphone") { MicView() }
                    .font(.callout)
            }

            HStack(spacing: 16) {
                Button(running ? "Running…" : ((Self.targetAutoBoot && !klIsAlyx()) ? "Boot" : "Start")) {
                    // A game build gathers its settings first: fold them into env
                    // (+ commandline.txt for hl1), and only boot if the chosen
                    // folder is ready.
                    if isLauncher, !klLauncherApply() { return }
                    boot()
                }
                .disabled(running || (isLauncher && !files.canLaunch))
                if running { ProgressView() }
                if finished, !log.isEmpty {
                    ShareLink(item: log) { Label("Export log", systemImage: "square.and.arrow.up") }
                }
                Spacer()
                // The game builds don't surface the runtime's internal staging errors
                // ("missing staged assets …") — their file UI drives availability, so
                // only the folder-picker's own status is meaningful there.
                if !(isLauncher && status.hasPrefix("missing")) {
                    Text(status).font(.footnote).foregroundStyle(.secondary)
                }
            }
        }
        .padding(24)
        // The system-keyboard bridge for immersive text entry — hidden, and it
        // must stay in the hierarchy: dismissing the field is what tells the
        // system to put the keyboard away. Characters, not key events (SwiftUI
        // hands a windowed app the resulting STRING), translated to Android
        // keycodes the guest's SDL turns back into text via kl_mono_key.
        .overlay(alignment: .bottom) {
            TextField("", text: $kbTyped)
                .focused($kbFocused)
                .opacity(0.02)
                .frame(width: 1, height: 1)
                .onChange(of: kbTyped) { old, new in
                    // A programmatic reset for a new field is not typing — skip it,
                    // or the "" would read as deleting the previous field's text.
                    if kbSuppress { kbSuppress = false; return }
                    if new.count < old.count {
                        // Deletion: backspace is an edit key, not text — keep it on
                        // the key path (KEYCODE_DEL), which the field honours.
                        for _ in 0..<(old.count - new.count) { kbSend(8) }
                    } else if new.count > old.count {
                        // New characters must go through the IME commit path or SDL
                        // never raises SDL_TEXTINPUT and the field stays empty.
                        String(new.dropFirst(old.count)).withCString { kl_mono_commit_text($0) }
                    }
                    if new.count > 128 { kbSuppress = true; kbTyped = "" }   // a keyboard, not a buffer
                }
        }
        // Boots on its own. Tapping an app and then tapping Boot is a harness,
        // not a product — and the scripted paths (`visionos/run.sh`) wanted
        // this anyway, which is what KL_AUTOBOOT was for. `KL_AUTOBOOT=0`
        // restores the button-only shape for hand-driven debugging, where the
        // point is to attach or start a capture before the guest runs.
        .task {
            // A game build NEVER auto-boots — even though visionos/run.sh sets
            // KL_AUTOBOOT=1 for scripted launches — because it must let the player
            // choose the game folder, set options, and press "Boot" first.
            // Only the runtime-debugging targets honour KL_AUTOBOOT (default on).
            // ...and a target can ask for the same (targets.py "autoboot",
            // baked into Info.plist as KLAutoBootDefault): anything but "0"
            // boots by itself, as every build did before the key existed.
            //
            // Half-Life: Alyx waits for the button too when nobody asked otherwise:
            // its window has a setting that is read at the start of the run, and a
            // launch from the Home View that booted on its own would never let it
            // be changed. The scripts say KL_AUTOBOOT=1 and are unaffected.
            if !isLauncher, klEnvOn("KL_AUTOBOOT", default: Self.targetAutoBoot && !klIsAlyx()) {
                boot()
            }
        }
    }

    /// Start, with the play area first where it is switched on: no game
    /// without a play area for the room the player is in (KLRoomFlow, .check).
    private func startOrCheckRoom() {
        guard KLRoomGuard.enabled else { boot(); return }
        // Stationary needs no room: the circle is put round the player in the game.
        if KLRoomGuard.style == .stationary {
            NSLog("[room] Start: stationary play area — no room check")
            boot()
            return
        }
        if KLRoomStore.load().isEmpty {
            roomMissing = true
            NSLog("[room] Start: no play area measured yet — asked for one first")
            return
        }
        openRoom(.check)
    }

    private func openRoom(_ purpose: KLRoomFlow.Purpose) {
        guard !roomFlow.active else { return }
        roomFailed = ""
        roomFlow.begin(purpose)
        Task {
            let r = await openImmersiveSpace(id: KLRoomFlow.spaceID)
            NSLog("[room] openImmersiveSpace(room) -> \(r)")
            if case .opened = r { return }
            roomFlow.close()
            roomFailed = klT("The room space could not be opened. Close the app and open it again.",
                             "Der Raum konnte nicht geöffnet werden. App schließen und wieder öffnen.")
        }
    }

    private func boot() {
        // Guarded on this side too, not only in kl_app.c. The guest boots
        // once per process — the runtime's JNI tables are process-global, so a
        // second run re-registers every native onto the same table and the
        // numbers stop being comparable to the host's. Disabling the button
        // while running is not enough: the run *finishes*, and the obvious
        // next thing to do with a finished run is press Boot again.
        guard !running, !finished, !KLRun.shared.adjustingControllers else { return }
        // Before configure rather than out of its failure: configure only
        // checks the APK and assets/, and a split-binary guest without its OBB
        // would pass it and then fail far away.
        if !isLauncher, let files = KLGameFiles.current, !files.complete {
            needsData = true
            status = "game data missing"
            log = "The game's files are not on this device yet. Use the panel above."
            return
        }
        running = true; log = ""
        KLRun.shared.started = true
        // Before configure, which is where the runtime starts reading its
        // environment.
        klAlyxApply()
        // The test switches first: none of these overwrites a value that is set, so a switch under
        // Advanced › Test wins over the picture setting it retunes (the foveation profile, 2026-10-13).
        KleptonTestSwitches.applyEnvironment()
        KleptonBattery.shared.start()
        // The selected app configuration enables the original loader by default.
        // An explicit KL_SCOTLAND2=0 still permits a baseline comparison.
        if Bundle.main.object(forInfoDictionaryKey: "KLScotland2Test") as? String == "1" {
            let modernScotland2 = Bundle.main.object(forInfoDictionaryKey: "KLScotland2Profile") as? String == "1.45.3"
            setenv("KL_SCOTLAND2", "1", 0)
            let sc2Diagnostics = Bundle.main.object(forInfoDictionaryKey: "KLScotland2Diagnostics") as? String ?? "1"
            setenv("KL_SCOTLAND2_DIAGNOSTICS", sc2Diagnostics, 0)
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2Hooks") as? String == "1" {
                setenv("KL_SCOTLAND2_ABI", "1", 0)
                setenv("KL_SCOTLAND2_AOT_PROBE", "1", 0)
                setenv("KL_SCOTLAND2_FLAMINGO", "1", 0)
                if modernScotland2 { setenv("KL_SCOTLAND2_FLAMINGO_LIBRARY", "libflamingo.so", 0) }
                setenv("KL_SCOTLAND2_FLAMINGO_PROBE", "1", 0)
                setenv("KL_SCOTLAND2_SCENE_HOOK", "1", 0)
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2CustomTypes") as? String == "1" {
                setenv("KL_SCOTLAND2_CUSTOMTYPES_PROBE", "1", 0)
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2Lapiz") as? String == "1" {
                setenv("KL_SCOTLAND2_LAPIZ_PROBE", "1", 0)
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2BSML") as? String == "1" {
                setenv("KL_SCOTLAND2_BSML_PROBE", "1", 0)
                if !modernScotland2 { setenv("KL_SCOTLAND2_FILESYSTEM", "1", 0) }
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2SongCore") as? String == "1" {
                setenv("KL_SCOTLAND2_SONGCORE_PROBE", "1", 0)
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2MetaCore") as? String == "1" {
                setenv("KL_SCOTLAND2_NATIVE_GATES", "1", 0)
                setenv("KL_SCOTLAND2_METACORE_PROBE", "1", 0)
                if let ca = Bundle.main.path(forResource: "cacert", ofType: "pem", inDirectory: "Scotland2Metadata/certificates") {
                    setenv("KL_SCOTLAND2_CA_BUNDLE", ca, 0)
                }
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2SongDownloader") as? String == "1" {
                setenv("KL_SCOTLAND2_SONGDOWNLOADER_PROBE", "1", 0)
                if !modernScotland2 { setenv("KL_SCOTLAND2_DOWNLOAD_BOUNDARIES", "1", 0) }
            }
            if Bundle.main.object(forInfoDictionaryKey: "KLScotland2CoreMods") as? String == "1" {
                setenv("KL_SCOTLAND2_REMAINING_CORE", "1", 0)
            }
            // The mods' runtime additions (exact futex wake counts and the
            // extension's exports) are tested with the mods only.
            setenv("KL_FUTEX_EXACT", "1", 0)
            #if KL_SCOTLAND2
            // The song search's Apple Music history, written for the mod while the game starts. Only while the
            // mods really run: with KL_SCOTLAND2=0 (the baseline comparison) nothing asks for Apple Music.
            if klEnvOn("KL_SCOTLAND2", default: false) { KleptonAppleMusic.refreshInBackground() }
            #endif
            setenv("KL_SONGS", "0", 1)
            setenv("KL_GUEST_JIT", "0", 1)
        }
        // Before the picture settings: it turns the game's own foveation off.
        KLRealityPresentSetting.applyEnvironment()
        KleptonRendering.shared.applyEnvironment()
        KleptonDiagnostics.applyEnvironment()
        KleptonGuestInfo.applyEnvironment()
        // On the main run loop: the thread below is inside kl_app_boot for as
        // long as there is anything to show, and a timer of its own would not
        // fire until it came back.
        prepTimer?.invalidate()
        prepTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
            var d: UInt32 = 0, t: UInt32 = 0
            let on = kl_app_prepare_progress(&d, &t) != 0
            prepDone = Int(d)
            prepTotal = on ? Int(t) : 0
        }

        // Off the main thread: the guest blocks — Unity's Baselib waits on
        // futexes, IL2CPP's GC suspends the world — and a blocked main thread
        // is a watchdog kill on this platform, which would present as a crash
        // with no report rather than as the hang it is.
        Thread.detachNewThread {
            // Before anything of the game's is opened: its saves here and in
            // the player's Steam Cloud are brought in step (KleptonSteamCloud.swift).
            KLSteamCloud.syncBeforeStart()
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
                    if isLauncher {
                        // A game build has its own file UI — don't scare the player
                        // with the developer staging instructions, and let them pick
                        // a folder and press Boot again (boot never ran, so retry is
                        // safe — that is why finished stays false here).
                        log = "Choose your game folder below (or install it), then press Boot."
                        running = false; finished = false; succeeded = false
                    } else {
                        log = "configure failed: \(status)\n\n" + stagingHelp
                        running = false; finished = true; succeeded = false
                    }
                }
                return
            }

            // The target is resolved now, so the immersion default (which depends
            // on it — the native VR kind opens .full) is finally knowable. Publish
            // it on the main actor before the space opens; the scene observes it.
            DispatchQueue.main.async {
                KleptonChroma.shared.immersionFull = !Immersive.mixed
                Immersive.mixedBlackAtBoot = Immersive.mixedBlack
                Immersive.mixedAtBoot = Immersive.mixed
            }

            let logPath = String(cString: kl_app_log_path())
            // The report is read back from the file rather than returned,
            // because an unimplemented JNI call aborts the process by design.
            // If this run dies, the next launch still shows how far it got.
            let poll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
                let t = KleptonLogTail.read(logPath)
                DispatchQueue.main.async { if log != t { log = t } }
            }
            RunLoop.current.add(poll, forMode: .common)

            KleptonRendering.shared.apply()
            KleptonLanguage.shared.apply()
            KleptonPlayer.shared.apply()
            KleptonProgramCaches.dropIfBuildChanged()
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

            let text = KleptonLogTail.read(logPath)
            DispatchQueue.main.async {
                prepTimer?.invalidate(); prepTimer = nil; prepTotal = 0
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
                    // `.error` is retried, because it is not always final: a
                    // launch that follows a reinstall over a running instance
                    // got it once, nine seconds in, and the very next launch
                    // of the same binary opened. Left alone, that is an app
                    // sitting in its boot window with a booted guest and no
                    // way to show it. `.userCancelled` is an answer, not a
                    // failure, and is not asked again.
                    Task {
                        for attempt in 1...5 {
                            let r = await openImmersiveSpace(id: Immersive.id)
                            NSLog("[cp] openImmersiveSpace -> \(r)"
                                  + (attempt > 1 ? " (attempt \(attempt))" : ""))
                            // What moves the picture to RealityKit later on, if
                            // that is on (KLRealityPresent.switchAtStart).
                            if case .opened = r {
                                KLRealityPresent.capture(open: openImmersiveSpace, dismiss: dismissImmersiveSpace,
                                                         openWindow: openWindow, dismissWindow: dismissWindow)
                            }
                            // An installed game's window has done its job once
                            // the picture is up: hand the keyboard poll to
                            // something that outlives it, and go.
                            if case .opened = r, Self.productStart {
                                KLRun.shared.up = true
                                KLRun.shared.watchKeyboard(openWindow)
                                showSettings = false
                                if Self.handTuneAfterStart { handTune = true }
                                else if !klEnvOn("KL_KEEP_START_WINDOW", default: false) { dismissWindow() }
                            }
                            guard case .error = r, attempt < 5 else { break }
                            try? await Task.sleep(nanoseconds: 2_000_000_000)
                        }
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
