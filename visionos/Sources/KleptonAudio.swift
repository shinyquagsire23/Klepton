import AVFAudio
import Foundation

/// The audio session — the one part of playback that CoreAudio alone will not
/// give you on this platform.
///
/// `runtime/kl_audio.c` owns the output unit and the ring, and it is plain C
/// that compiles unchanged for macOS and visionOS. What does *not* port is the
/// session: on macOS there is none, and on visionOS an output unit will
/// initialise, start, report success and produce nothing at all unless an
/// `AVAudioSession` has been configured and activated first. That failure has
/// no error code anywhere in it, which is exactly why this file exists and why
/// it logs what it did.
///
/// The second job here is keeping the stream. visionOS inherits iPadOS's view
/// that audio is a resource the system lends out: an interruption, a route
/// change, a media-services reset or a trip through the background can all
/// take the unit away, and the unit does not always notice — it stays nominally
/// started and simply stops calling back. So there are two independent
/// recoveries, and they are meant to overlap:
///
///   * these notifications, which are precise and occasionally do not arrive;
///   * kl_audio.c's own watchdog on the producer side, which notices a render
///     callback that has stopped arriving and rebuilds the unit regardless.
///
/// Either alone has been enough to lose audio for a whole session on this
/// family of OS. Together, a missed notification costs a half-second gap.
enum KleptonAudio {
    nonisolated(unsafe) private static var started = false
    nonisolated(unsafe) private static var observers: [NSObjectProtocol] = []

    /// Configure and activate the session, then tell the C side what the
    /// hardware rate turned out to be. Safe to call more than once.
    static func start() {
        guard !started else { return }
        started = true

        let session = AVAudioSession.sharedInstance()
        do {
            // .playback, not .ambient: Beat Saber's music is the point of the
            // app, not a decoration over someone else's audio, and .ambient is
            // silenced by the ringer switch on this OS family. .default mode
            // keeps the system's own spatialisation out of the way — the guest
            // mixes its own stereo and anything we add on top is a second
            // opinion about a scene we already rendered.
            try session.setCategory(.playback, mode: .default)
            // Ask for the guest's rate. If the system grants it the resampler
            // in kl_audio.c degenerates to a copy; if it does not, kl_audio
            // measures what it actually got and resamples. Either way this is a
            // request, never an assumption — a unit built against an assumed
            // rate is the failure that reports success and stays silent.
            try session.setPreferredSampleRate(48000)
            try session.setPreferredIOBufferDuration(0.010)
            try session.setActive(true)
            // After activation, which is the order ALVR uses and the order the
            // session's own routing decisions are made in.
            try Self.directStereo(session)
        } catch {
            NSLog("[au] AVAudioSession setup failed: \(error) — expect silence")
        }

        NSLog("[au] session active: \(session.sampleRate) Hz, "
              + "\(session.outputNumberOfChannels) ch out, "
              + "IO buffer \(String(format: "%.1f", session.ioBufferDuration * 1000)) ms")
        kl_audio_session_ready(session.sampleRate)

        observe(AVAudioSession.interruptionNotification) { note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt ?? 0
            let began = AVAudioSession.InterruptionType(rawValue: raw) == .began
            if !began {
                // The session is deactivated for us on the way in but NOT
                // reactivated on the way out — an interruption that ends leaves
                // an inactive session, and a unit started against one produces
                // silence with no error. Reactivate before telling C to rebuild.
                try? session.setActive(true)
            }
            kl_audio_interrupted(began ? 1 : 0)
        }

        // A route change is the headphones going in or out, and on this device
        // also the transition into and out of an immersive space. The hardware
        // sample rate can change with it, which is why the C side rebuilds the
        // unit rather than restarting it: a unit initialised against the old
        // rate starts happily and never calls back.
        observe(AVAudioSession.routeChangeNotification) { note in
            let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt ?? 0
            let reason = AVAudioSession.RouteChangeReason(rawValue: raw)
            NSLog("[au] route change (reason \(raw)), now \(session.sampleRate) Hz")
            switch reason {
            case .oldDeviceUnavailable, .newDeviceAvailable, .override,
                 .routeConfigurationChange, .categoryChange:
                try? session.setActive(true)
                _ = kl_audio_restart()
            default:
                break
            }
        }

        // Rare, and total: every audio object in the process is invalid. Nothing
        // survives it except a full rebuild, and the session has to be
        // configured again from scratch first.
        observe(AVAudioSession.mediaServicesWereResetNotification) { _ in
            NSLog("[au] media services were reset — reconfiguring from scratch")
            try? session.setCategory(.playback, mode: .default)
            try? session.setActive(true)
            // A reset invalidates every audio object in the process, and the
            // spatial experience is one of them — without this the sound comes
            // back correct in every respect except where it is.
            try? Self.directStereo(session)
            kl_audio_session_ready(session.sampleRate)
            _ = kl_audio_restart()
        }
    }

    /// Take the system's spatial audio out of the path: two channels, played
    /// where the guest mixed them.
    ///
    /// **This is why the sound followed the window.** visionOS spatialises app
    /// audio by default, and the sound stage it spatialises *into* is anchored
    /// to the app's scene — so a stereo mix that the guest has already panned
    /// for a head-mounted listener gets panned a second time, towards a window,
    /// by a system that has no idea where anything in the guest's world is. It
    /// is not a mixing bug and no gain change fixes it; the whole scene simply
    /// sits wherever the window is.
    ///
    /// `.bypassed` is the escape hatch, and it is the same one ALVR uses
    /// (`EventHandler.fixAudioForDirectStereo`) for the same reason: the audio
    /// arrives already spatialised by something that knows the scene, so the
    /// only correct thing the OS can do with it is play it. Beat Saber's FMOD
    /// mix is exactly that — `kl_opensl.c` hands us a finished stereo buffer.
    ///
    /// `setPreferredOutputNumberOfChannels(2)` goes with it: bypassing the
    /// spatialiser on a route that has offered more than two channels would
    /// otherwise leave the mix to be spread across them by whatever downmix
    /// happens to be in the way.
    ///
    /// ALVR's other two calls are deliberately NOT imported. It takes
    /// `.playAndRecord` and `.voiceChat` because it needs the microphone for
    /// SteamVR; we do not, and `.playAndRecord` would hand the ringer switch a
    /// veto over the music this app exists to play.
    ///
    /// `KL_AUDIO_SPATIAL=1` leaves the system's spatialiser in, which is the
    /// A/B if the sound is ever wrong in a way that is not "in the wrong
    /// place".
    private static func directStereo(_ session: AVAudioSession) throws {
        guard ProcessInfo.processInfo.environment["KL_AUDIO_SPATIAL"] != "1" else {
            NSLog("[au] KL_AUDIO_SPATIAL=1 — leaving the system spatialiser in the path")
            return
        }
        try session.setPreferredOutputNumberOfChannels(2)
        try session.setIntendedSpatialExperience(.bypassed)
        NSLog("[au] direct stereo: system spatialisation bypassed, 2 ch preferred")
    }

    private static func observe(_ name: Notification.Name,
                                _ body: @escaping (Notification) -> Void) {
        observers.append(NotificationCenter.default.addObserver(
            forName: name, object: AVAudioSession.sharedInstance(),
            queue: .main, using: body))
    }
}
