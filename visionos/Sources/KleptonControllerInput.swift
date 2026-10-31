import Foundation
import simd

/// One hand's worth of state, in the guest's own units.
struct KleptonHandState {
    var position: SIMD3<Float>?
    var orientation: simd_quatf?
    var buttons: UInt32 = 0
    var touches: UInt32 = 0
    var indexTrigger: Float = 0
    var handTrigger: Float = 0
    var stick: SIMD2<Float> = .zero
    /// Tracking-space linear (m/s) and angular (rad/s) velocity, for the
    /// ovrpPoseStatef fields libunity forwards to `XRNodeState.velocity`.
    /// Only the Sense controllers report these — a `HandAnchor` carries no
    /// motion at all, and differentiating a pose stream to invent one produces
    /// a number that looks physical and is mostly tracker noise.
    var linearVelocity: SIMD3<Float> = .zero
    var angularVelocity: SIMD3<Float> = .zero
    /// Whether a Sense controller supplied this, as opposed to a hand anchor.
    /// Only a Sense controller can press anything, so this is also "are the
    /// buttons meaningful".
    var fromController = false
    /// The bits this hand's menu button (Create / Options) stands for while it
    /// is down, and the hand's other buttons and touches without them. Kept
    /// apart because the metrics panel may hold that one button back
    /// (`applyPanelChord`); `buttons` and `touches` are what the guest gets.
    var pill: UInt32 = 0
    var otherButtons: UInt32 = 0
    var otherTouches: UInt32 = 0
}

/// `ovrpButton` **raw** bits.
///
/// Raw, not virtual: `ControllerState4.Buttons` carries `OVRInput.RawButton`,
/// and OVRInput maps raw→virtual itself through its own buttonMap. Emitting
/// virtual values is a specific, silent bug the host already hit — 0x2000 is
/// `PrimaryIndexTrigger` in virtual space and `RThumbstickDown` in raw, so
/// every "trigger press" became a stick flick and no press ever reached a UI.
///
/// The eight marked (v) are the ones `runtime/gfx/kl_view.c` drives on the host and
/// which have been seen to work end to end in gameplay. The rest are the
/// standard raw layout, consistent with those eight, and are **not** yet
/// confirmed against the guest's own metadata — if a menu or stick click does
/// nothing on device, suspect these before suspecting the plumbing.
enum OVRPRawButton {
    static let a: UInt32              = 0x0000_0001   // (v)
    static let b: UInt32              = 0x0000_0002   // (v)
    static let rThumbstick: UInt32    = 0x0000_0004
    static let lThumbstickUp: UInt32   = 0x0000_0010
    static let lThumbstickDown: UInt32 = 0x0000_0020
    static let lThumbstickLeft: UInt32 = 0x0000_0040
    static let lThumbstickRight: UInt32 = 0x0000_0080
    static let x: UInt32              = 0x0000_0100   // (v)
    static let y: UInt32              = 0x0000_0200   // (v)
    static let lThumbstick: UInt32    = 0x0000_0400
    static let rThumbstickUp: UInt32  = 0x0000_1000   // (v)
    static let rThumbstickDown: UInt32 = 0x0000_2000  // (v)
    static let rThumbstickLeft: UInt32 = 0x0000_4000  // (v)
    static let rThumbstickRight: UInt32 = 0x0000_8000 // (v)
    static let start: UInt32          = 0x0010_0000
    static let back: UInt32           = 0x0020_0000
    static let rIndexTrigger: UInt32  = 0x0400_0000   // (v)
    static let rHandTrigger: UInt32   = 0x0800_0000   // (v)
    static let lIndexTrigger: UInt32  = 0x1000_0000   // (v)
    static let lHandTrigger: UInt32   = 0x2000_0000   // (v)
}

/// One Euro filter (Casiez, Roussel & Vogel, CHI 2012) for a rigid pose: a low-pass whose
/// cutoff rises with speed, so a still hand's tracker noise is smoothed away while a fast
/// motion passes with little lag. Position cutoff in Hz = minCutoff + beta * speed (m/s);
/// rotation the same with rad/s. Used for hand tracking in Surreal's hands mode, where the
/// predicted ARKit wrist shook the hands and fingers in Half-Life: Alyx (2026-10-29).
struct KleptonPoseSmoother {
    var minCutoff: Float = 1.0, beta: Float = 8.0
    var rotMinCutoff: Float = 1.0, rotBeta: Float = 1.5
    var derivativeCutoff: Float = 1.0
    private var last: (time: Double, p: SIMD3<Float>, q: simd_quatf, v: Float, w: Float)?

    mutating func reset() { last = nil }

    private static func alpha(_ cutoff: Float, _ dt: Float) -> Float {
        let tau = 1 / (2 * Float.pi * cutoff)
        return 1 / (1 + tau / dt)
    }

    /// The smoothed pose at `time`. A gap over 0.1 s (a hand lost) starts again from the sample.
    mutating func filter(position p: SIMD3<Float>, orientation raw: simd_quatf, at time: Double) -> (SIMD3<Float>, simd_quatf) {
        guard let prev = last, time > prev.time, time - prev.time < 0.1,
              p.x.isFinite, p.y.isFinite, p.z.isFinite else {
            last = (time, p, raw.normalized, 0, 0)
            return (p, raw.normalized)
        }
        let dt = Float(max(1.0 / 240, time - prev.time))
        // Same hemisphere as the last output, so the slerp takes the short way.
        let q = simd_dot(raw.vector, prev.q.vector) < 0 ? simd_quatf(vector: -raw.vector) : raw
        let speed = simd_length(p - prev.p) / dt
        let v = prev.v + (speed - prev.v) * Self.alpha(derivativeCutoff, dt)
        let outP = prev.p + (p - prev.p) * Self.alpha(minCutoff + beta * v, dt)
        let delta = (q * prev.q.inverse).normalized
        let angle = 2 * acos(min(1, abs(delta.real)))
        let w = prev.w + (angle / dt - prev.w) * Self.alpha(derivativeCutoff, dt)
        let outQ = simd_slerp(prev.q, q, Self.alpha(rotMinCutoff + rotBeta * w, dt)).normalized
        last = (time, outP, outQ, v, w)
        return (outP, outQ)
    }
}
