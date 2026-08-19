// Chroma keying and hand matting — the two things that decide how much of the
// ROOM shows through the guest's picture, made adjustable while wearing the
// headset and remembered across launches.
//
// **Ported from VisionOSALVRClient with its maths and its defaults intact.**
// The two distance dials are numbers a person finds by looking at a matte, not
// values that can be derived, so keeping them identical means a setting found
// in that client still means the same thing here. `kl_reproject.c` carries the
// shader half and the same argument.
//
// Unlike the controller tuning beside it, these are PERSISTED: a key colour is
// a property of the room the person is sitting in, not of a debugging session,
// and re-finding it every launch is the thing that would stop it being used.
import SwiftUI
import simd

/// Everything the two panels below own. `Codable` because it is written to
/// UserDefaults as JSON — a struct rather than seven separate defaults keys so
/// a settings file can never be half-migrated, and so adding a dial later is a
/// field with a default rather than a new key nothing writes.
struct KLChromaSettings: Codable, Equatable {
    var enabled = false
    var color   = SIMD3<Float>(16.0 / 255.0, 124.0 / 255.0, 16.0 / 255.0)
    var distMin: Float = 0.35
    var distMax: Float = 0.7
    /// Whether the system mattes the user's own hands and arms OVER the guest's
    /// picture. On by default, which is what visionOS does in `.mixed` and what
    /// a person reaching for a real object wants; off is what a guest that
    /// draws its own hands wants, because two pairs of hands in one place is
    /// worse than either alone.
    var handMatting = true

    /// SIMD3 is not Codable, so the colour rides as three keyed floats. Spelled
    /// out rather than reached for a wrapper: this is the file format, and a
    /// format that is legible in the JSON is one a person can fix by hand.
    private enum CodingKeys: String, CodingKey {
        case enabled, distMin, distMax, handMatting
        case colorR, colorG, colorB
    }

    init() {}

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        // decodeIfPresent throughout, so a settings blob written by an older
        // build is READ rather than rejected: a missing field keeps this
        // struct's default instead of failing the whole decode and silently
        // resetting every other dial with it.
        enabled     = try c.decodeIfPresent(Bool.self,  forKey: .enabled)     ?? enabled
        distMin     = try c.decodeIfPresent(Float.self, forKey: .distMin)     ?? distMin
        distMax     = try c.decodeIfPresent(Float.self, forKey: .distMax)     ?? distMax
        handMatting = try c.decodeIfPresent(Bool.self,  forKey: .handMatting) ?? handMatting
        color = SIMD3<Float>(try c.decodeIfPresent(Float.self, forKey: .colorR) ?? color.x,
                             try c.decodeIfPresent(Float.self, forKey: .colorG) ?? color.y,
                             try c.decodeIfPresent(Float.self, forKey: .colorB) ?? color.z)
    }

    func encode(to e: Encoder) throws {
        var c = e.container(keyedBy: CodingKeys.self)
        try c.encode(enabled,     forKey: .enabled)
        try c.encode(distMin,     forKey: .distMin)
        try c.encode(distMax,     forKey: .distMax)
        try c.encode(handMatting, forKey: .handMatting)
        try c.encode(color.x, forKey: .colorR)
        try c.encode(color.y, forKey: .colorG)
        try c.encode(color.z, forKey: .colorB)
    }
}

final class KleptonChroma: ObservableObject {
    static let shared = KleptonChroma()

    private static let defaultsKey = "klepton.chroma"

    @Published var settings = KLChromaSettings() { didSet { publish(); save() } }

    private init() {
        // **The environment wins where it is explicitly set, then the saved
        // settings, then the defaults**, and that order is the one that stays
        // predictable. Saved-wins would make a `KL_CHROMA=1` on a launch line
        // do nothing as soon as anything had ever been saved, which reads as
        // the knob being broken; environment-always-wins would make the panel
        // unable to turn off something a script had turned on.
        var s = KLChromaSettings()
        if let d = UserDefaults.standard.data(forKey: Self.defaultsKey),
           let saved = try? JSONDecoder().decode(KLChromaSettings.self, from: d) {
            s = saved
            NSLog("[chroma] restored: \(Self.describe(s))")
        }
        let env = ProcessInfo.processInfo.environment
        if let v = env["KL_CHROMA"]       { s.enabled = v != "0" }
        if let v = env["KL_CHROMA_COLOR"] {
            let f = v.split(separator: ",").compactMap { Float($0) }
            if f.count == 3 { s.color = SIMD3<Float>(f[0], f[1], f[2]) }
        }
        if let v = env["KL_CHROMA_RANGE"] {
            let f = v.split(separator: ",").compactMap { Float($0) }
            if f.count == 2 { s.distMin = f[0]; s.distMax = f[1] }
        }
        if let v = env["KL_HAND_MATTING"] { s.handMatting = v != "0" }
        settings = s          // didSet publishes it to the shader and re-saves
    }

    /// What the App reads to decide whether the system draws the user's hands
    /// over the guest. Published, so toggling it re-evaluates the scene rather
    /// than waiting for a relaunch.
    var upperLimbVisibility: Visibility { settings.handMatting ? .automatic : .hidden }

    private func publish() {
        var rgb = [settings.color.x, settings.color.y, settings.color.z]
        kl_reproject_set_chroma(settings.enabled ? 1 : 0, &rgb,
                                settings.distMin, settings.distMax)
    }

    private func save() {
        guard let d = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(d, forKey: Self.defaultsKey)
    }

    /// The environment lines that would reproduce this set, for the same reason
    /// the controller panel has one: a value found by dragging and not written
    /// down is a value found twice — and this one also has to survive being
    /// quoted into a `build_run_vpro.sh` invocation.
    static func describe(_ s: KLChromaSettings) -> String {
        String(format: "KL_CHROMA=%d KL_CHROMA_COLOR=\"%.4f,%.4f,%.4f\" "
               + "KL_CHROMA_RANGE=\"%.3f,%.3f\" KL_HAND_MATTING=%d",
               s.enabled ? 1 : 0, s.color.x, s.color.y, s.color.z,
               s.distMin, s.distMax, s.handMatting ? 1 : 0)
    }
}

/// The panel, a sibling of TuningView in the boot window — which stays open
/// beside the immersive space, so the guest keeps rendering while these move.
struct ChromaView: View {
    @ObservedObject private var chroma = KleptonChroma.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Matting").font(.headline)
            Text("What of the room shows through. The key colour is matched in "
                 + "HSV with hue weighted heaviest, so a green screen under "
                 + "uneven light still keys as one colour.")
                .font(.caption).foregroundStyle(.secondary)

            Toggle("Chroma key", isOn: $chroma.settings.enabled)
            ColorPicker("Key colour", selection: Binding(
                get: { Color(.sRGB,
                             red:   Double(chroma.settings.color.x),
                             green: Double(chroma.settings.color.y),
                             blue:  Double(chroma.settings.color.z)) },
                set: { c in
                    // resolve() gives the components in the picker's own space,
                    // which is what the shader compares against — see
                    // kl_reproject.c on which space the key test runs in.
                    let r = c.resolve(in: EnvironmentValues())
                    chroma.settings.color = SIMD3<Float>(r.red, r.green, r.blue)
                }))
            .disabled(!chroma.settings.enabled)

            // The fade band. `min` is where a pixel is fully keyed out and
            // `max` where it is fully kept, so min above max would invert the
            // ramp — the setters clamp rather than letting the sliders cross.
            dist("Key out below", $chroma.settings.distMin)
            dist("Keep above",    $chroma.settings.distMax)

            Divider()
            Toggle("Hand matting (system draws your hands over the guest)",
                   isOn: $chroma.settings.handMatting)
            Text("Turn off for a guest that draws its own hands — two pairs in "
                 + "one place is worse than either alone.")
                .font(.caption).foregroundStyle(.secondary)

            Divider()
            HStack {
                Button("Log as environment") {
                    NSLog("[chroma] \(KleptonChroma.describe(chroma.settings))")
                }
                Button("Reset") { chroma.settings = KLChromaSettings() }
            }
            Text(KleptonChroma.describe(chroma.settings))
                .font(.system(.caption2, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
        }
        .padding()
    }

    private func dist(_ label: String, _ value: Binding<Float>) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            Slider(value: Binding(
                get: { value.wrappedValue },
                set: { v in
                    value.wrappedValue = v
                    // Hold min <= max from whichever side moved, so the band
                    // can be dragged shut but never through itself.
                    if chroma.settings.distMin > chroma.settings.distMax {
                        if label.hasPrefix("Key out") {
                            chroma.settings.distMax = chroma.settings.distMin
                        } else {
                            chroma.settings.distMin = chroma.settings.distMax
                        }
                    }
                }), in: 0 ... 2, step: 0.01)
            Text(String(format: "%5.2f", value.wrappedValue))
                .font(.system(.body, design: .monospaced)).frame(width: 60)
        }
        .disabled(!chroma.settings.enabled)
    }
}
