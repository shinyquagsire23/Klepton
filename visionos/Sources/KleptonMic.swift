// Microphone capture — a single opt-in toggle that lets a guest (Steam Link's
// voice chat) reach the headset's microphone. Modelled on KleptonChroma: a
// persisted Codable setting, an env override, and a small panel in the boot
// window, so it is remembered across launches and adjustable while wearing the
// headset.
//
// OFF by default, and deliberately so. A microphone is a privacy surface: the
// session category it forces (.playAndRecord) also hands the ringer switch a
// veto over the music this app exists to play, and touching the device at all
// makes visionOS show a permission prompt. Neither belongs in a title that
// never asks for the mic. So nothing here engages the microphone — no category
// change, no permission prompt, no capture device presented to the guest —
// until a person turns it on. The C half is kl_audio.c's capture path, armed by
// kl_audio_mic_set_enabled and read by kl_aaudio.c / kl_opensl.c when a guest
// opens an input stream.
import SwiftUI

/// The one field, kept as a struct for the same reason KLChromaSettings is: a
/// single JSON blob in UserDefaults migrates cleanly when a field is added
/// later, rather than a loose default key nothing writes.
struct KLMicSettings: Codable, Equatable {
    var enabled = false
}

final class KleptonMic: ObservableObject {
    static let shared = KleptonMic()

    private static let defaultsKey = "klepton.mic"

    @Published var settings = KLMicSettings() { didSet { publish(); save() } }

    private init() {
        // Same precedence as KleptonChroma: environment wins where explicitly
        // set, then the saved setting, then the default (off). KL_MIC=1 on a
        // launch line turns it on for that run without having to touch the panel.
        var s = KLMicSettings()
        if let d = UserDefaults.standard.data(forKey: Self.defaultsKey),
           let saved = try? JSONDecoder().decode(KLMicSettings.self, from: d) {
            s = saved
            NSLog("[mic] restored: \(Self.describe(s))")
        }
        let env = ProcessInfo.processInfo.environment
        if let v = env["KL_MIC"] { s.enabled = v != "0" }
        settings = s          // didSet arms the C side and (if on) the session
    }

    private func publish() {
        // Arm the capture side. It stays lazy — the microphone is not actually
        // opened until a guest asks for an input stream — but the session
        // category has to be right first, so both are done here.
        kl_audio_mic_set_enabled(settings.enabled ? 1 : 0)
        KleptonAudio.applyMicCategory(settings.enabled)
    }

    private func save() {
        guard let d = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(d, forKey: Self.defaultsKey)
    }

    /// The environment line that reproduces this, for pasting into a
    /// build_run_vpro.sh invocation — the same affordance the other panels have.
    static func describe(_ s: KLMicSettings) -> String {
        "KL_MIC=\(s.enabled ? 1 : 0)"
    }
}

/// The panel, a sibling of ChromaView in the boot window.
struct MicView: View {
    @ObservedObject private var mic = KleptonMic.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(klT("Microphone", "Mikrofon")).font(.headline)
            Text(klT("Let the game use the headset microphone — for voice chat, such as Steam "
                     + "Link's or the players' in a multiplayer room. Off by default. Turning it on asks for "
                     + "microphone permission and switches audio to play-and-record, "
                     + "which lets the silent switch mute app audio.",
                     "Lässt das Spiel das Mikrofon der Brille benutzen – für den Sprachchat, etwa in Steam "
                     + "Link oder mit den Mitspielern in einem Multiplayer-Raum. Standardmäßig aus. Beim Einschalten "
                     + "fragt visionOS nach dem Mikrofon, und der Ton läuft dann über Wiedergabe und Aufnahme, "
                     + "wodurch der Stummschalter den Ton der App stumm schalten kann."))
                .font(.caption).foregroundStyle(.secondary)

            Toggle(klT("Allow microphone", "Mikrofon erlauben"), isOn: $mic.settings.enabled)

            Text(KleptonMic.describe(mic.settings))
                .font(.system(.caption2, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
        }
        .padding()
    }
}
