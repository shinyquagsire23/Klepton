//
//  KleptonControllers.swift — the controller seam, P5b/M7 on visionOS.
//
//  The guest wants two Oculus Touch controllers: poses for nodes 3 and 4, and
//  `ovrpButton`/`ovrpTouch` raw bits, an index trigger, a hand trigger and a
//  thumbstick per hand (`kl_ovrp_set_hand_pose`, `kl_ovrp_set_controller_input`).
//  This platform can answer that two ways, and which one is available is a
//  runtime fact, not a build-time one:
//
//    * **PSVR2 Sense controllers**, when paired. Poses come from ARKit's
//      `AccessoryTrackingProvider`, buttons from GameController. This is the
//      real answer — it is the only source that has buttons at all, the only
//      one whose pose the platform publishes as a proper `.grip`, and the only
//      one that reports velocity.
//
//  **Every pose here is predicted to the frame's presentation time**, the same
//  instant `queryDeviceAnchor(atTimestamp:)` gives the head. Reading the last
//  anchor an async stream delivered instead — which is what this did — caps the
//  controllers at the tracker's delivery rate rather than the display's, and a
//  pose repeated across three frames of a 90 Hz picture does not read as
//  latency, it reads as the hands running at 30 Hz.
//    * **Hands**, always. `HandTrackingProvider`'s skeleton supplies the grip
//      pose (the middle-finger metacarpal, so a hilt is gripped rather than
//      swinging off the wrist), and the index trigger comes from the system's
//      own pinch recogniser through `LayerRenderer.onSpatialEvent` — not from
//      our arithmetic on two fingertip joints, which was tried twice and was
//      unstable both times. The fingertip distance survives as the trigger's
//      analog value and as the fallback if no spatial event ever arrives.
//      No other button exists on a hand, but a trigger is the one a menu
//      clicks with.
//
//  So this file is a merge, not a switch: Sense wins per hand where it is
//  present, hands fill in the rest, and a Sense controller that is paired for
//  the right hand only still leaves the left hand tracking. Per *hand*, not
//  per session, because that is how they actually arrive — one at a time,
//  mid-run, and either one can drop out.
//
//  Nothing below the seam changes. kl_ovrp still reports node 9 for the head
//  and 3/4 for the hands, and the SDL viewer on the host still drives the same
//  two functions (kl_view.c) — which is what makes the host a usable A/B for
//  anything that looks like an input bug here.
//
import Foundation
import ARKit
import CoreHaptics
import GameController
import SwiftUI
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
}

/// `ovrpButton` **raw** bits.
///
/// Raw, not virtual: `ControllerState4.Buttons` carries `OVRInput.RawButton`,
/// and OVRInput maps raw→virtual itself through its own buttonMap. Emitting
/// virtual values is a specific, silent bug the host already hit — 0x2000 is
/// `PrimaryIndexTrigger` in virtual space and `RThumbstickDown` in raw, so
/// every "trigger press" became a stick flick and no press ever reached a UI.
///
/// The eight marked (v) are the ones `runtime/kl_view.c` drives on the host and
/// which have been seen to work end to end in gameplay. The rest are the
/// standard raw layout, consistent with those eight, and are **not** yet
/// confirmed against the guest's own metadata — if a menu or stick click does
/// nothing on device, suspect these before suspecting the plumbing.
enum OVRPRawButton {
    static let a: UInt32              = 0x0000_0001   // (v)
    static let b: UInt32              = 0x0000_0002   // (v)
    static let rThumbstick: UInt32    = 0x0000_0004
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

/// The wrist frame ARKit reports, expressed in the grip frame the guest wants.
///
/// **These are not the same basis, and nothing above this line converts them.**
/// `HandAnchor.originFromAnchorTransform` is the wrist joint, whose axes are
/// anatomical and mirrored between hands; the guest wants an Oculus Touch grip
/// pose — right-handed, +Y up out of the top of the hilt, **-Z along the
/// direction the hilt points**. Handing the wrist frame over unconverted is
/// what put the laser out of the side of the wrist rather than out of the fist.
///
/// The mapping is ALVR's, from `WorldTracker.swift`'s
/// `rightHandOrientationCorrection` — the same anchor, the same provider, the
/// same OS — and it is observed correct on device for the RIGHT hand:
///
///   grip X = wrist +Y, grip Y = wrist +Z, grip Z = wrist +X
///
/// **The LEFT hand is `R · Rx(180)`**, and four playtests force that — no free
/// parameters are left:
///
/// | left constant | the hilt |
/// |---|---|
/// | A = `R · Ry(180)` (ALVR's mirrored pair) | base at the pinky |
/// | B = `R` (the right hand's own) | "about how I would want it, **if it were just rotated 180**" |
/// | C = `R · Rz(180)` | base at the pinky again — indistinguishable from A |
///
/// A and C look the same, and that is the measurement that closes it. They
/// differ by exactly `Rx(180)` (`Rx·Ry = Rz` for half-turns about orthogonal
/// axes), so a feature invariant between them is invariant under `Rx(180)` —
/// which means **the visible hilt axis is the grip's X**, not its Z. (It is not
/// our Z because the game does not point a saber down the raw grip axis:
/// `IVRPlatformHelper.AdjustControllerTransform` rotates a Touch controller by
/// a large device-specific offset before rendering anything.)
///
/// B reverses the hilt relative to A and C — it flips X — and B is the one whose
/// direction was right. So the fix must keep B's X and turn the hilt over about
/// it: `Rx(180)`, the only half-turn that preserves X.
///
/// Which is also why the two earlier attempts failed the same way. Both reasoned
/// about *which axis of Apple's wrist frame means what*, and that has no ground
/// truth to check against — the relationship between the hands' frames is a
/// convention that is not written down, and cannot be derived (a joint transform
/// is a rotation, determinant +1; a mirror is not, so something must differ
/// between the hands and which axis differs was a free choice). The playtests do
/// have ground truth. Reading them eliminated Y, then eliminated Z, and X is what
/// remains.
///
/// A/Bs, since this is still inference from four samples:
///   * `KL_HAND_ANATOMICAL=1` derives the left frame from joint **positions**
///     instead (`anatomicalFrame`) — no convention needed at all, but it has
///     never been confirmed on device.
///   * `KL_HAND_MIRROR=1` restores row A.
///   * `KL_HAND_ROT_L="180,0,0"` / `"0,180,0"` / `"0,0,180"` sweeps the axes
///     from here with no rebuild.
private let klGripFromWrist: [simd_quatf] = {
    let r = simd_quatf(ix: 0.5, iy: 0.5, iz: 0.5, r: 0.5)
    if ProcessInfo.processInfo.environment["KL_HAND_MIRROR"] == "1" {
        NSLog("[cp] KL_HAND_MIRROR: ALVR's mirrored pair")
        return [simd_quatf(ix: 0.5, iy: -0.5, iz: -0.5, r: 0.5), r]
    }
    // r * Rx(180), multiplied out, so the constant in the source and the
    // constant in a log line are the same object.
    return [simd_quatf(ix: -0.5, iy: -0.5, iz: 0.5, r: 0.5), r]
}()

/// `KL_HAND_ROT="x,y,z"` (degrees) and `KL_HAND_POS="x,y,z"` (metres): a further
/// rotation and offset in the *grip's own* frame, applied to every **hand** pose
/// after the correction above. `KL_HAND_ROT_L` / `KL_HAND_ROT_R` (and the `POS`
/// pair) override it for one hand.
///
/// **Hands only.** This used to be applied to the Sense controllers' poses as
/// well, and that was wrong in a way that is easy to miss: the default offset
/// below is a *hand* correction — the metacarpal midpoint is not where a
/// controller's tracked origin sits — while `AccessoryAnchor`'s `.grip` already
/// **is** "where the hand holds this", published by the platform. Sharing the
/// knob therefore pushed every Sense pose 4 cm down its own hilt for a reason
/// that does not apply to it, and made the two paths impossible to tune
/// independently. The Sense path has `KL_SENSE_ROT` / `KL_SENSE_POS`.
///
/// Per-hand, because the errors that are left are not guaranteed to be
/// symmetric — the shared correction above is a claim about a convention, and
/// if it turns out to be half right the fix is 180 degrees on one hand only,
/// which a shared knob cannot express.
///
/// This exists because the remaining error is a comfort angle, not a basis
/// error, and a comfort angle can only be found by wearing the thing: Beat
/// Saber's own `IVRPlatformHelper.AdjustControllerTransform` tilts a Touch
/// controller by a device-specific offset we cannot read, so even a perfect
/// grip pose does not put the saber where a Quest player's muscle memory
/// expects. One device run per candidate angle, with no rebuild between them.
private struct KLHandTune {
    var rot = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    /// 4 cm out along the hilt axis by default (`-Z`, the direction the blade
    /// leaves the fist). The metacarpal midpoint is where the hand *holds* the
    /// hilt, which is not where a Touch controller's tracked origin sits — the
    /// controller extends past the fist — so a hilt pinned to the palm reads as
    /// sliding back down its own pointing ray. Directionally right, not gripped
    /// right. `KL_HAND_POS` replaces this outright.
    var pos = SIMD3<Float>(0, 0, -0.04)

    /// Indexed by hand: 0 = left, 1 = right.
    static let shared: [KLHandTune] = [make(0), make(1)]

    private static func make(_ hand: Int) -> KLHandTune {
        var t = KLHandTune()
        if let (d, k) = klEnvTriple("KL_HAND_ROT", hand) {
            t.rot = klEulerXYZ(d)
            NSLog("[cp] \(k): hand \(hand) extra grip rotation \(d) degrees")
        }
        if let (p, k) = klEnvTriple("KL_HAND_POS", hand) {
            t.pos = p
            NSLog("[cp] \(k): hand \(hand) extra grip offset \(p) m")
        }
        return t
    }
}

/// `"x,y,z"` from `<key><_L|_R>` or `<key>`, whichever is set — the per-hand key
/// wins. Shared by both tuning structs so a knob means the same thing on either
/// path.
private func klEnvTriple(_ key: String, _ hand: Int) -> (SIMD3<Float>, String)? {
    let env = ProcessInfo.processInfo.environment
    for k in [key + (hand == 0 ? "_L" : "_R"), key] {
        guard let s = env[k] else { continue }
        let f = s.split(separator: ",")
            .compactMap { Float($0.trimmingCharacters(in: .whitespaces)) }
        guard f.count == 3 else {
            NSLog("[cp] \(k)=\(s) is not three comma-separated numbers — ignored")
            continue
        }
        return (SIMD3<Float>(f[0], f[1], f[2]), k)
    }
    return nil
}

/// Degrees to a quaternion, X then Y then Z in the frame being corrected.
private func klEulerXYZ(_ degrees: SIMD3<Float>) -> simd_quatf {
    let r = degrees * .pi / 180
    return simd_quatf(angle: r.x, axis: [1, 0, 0])
         * simd_quatf(angle: r.y, axis: [0, 1, 0])
         * simd_quatf(angle: r.z, axis: [0, 0, 1])
}

/// The Sense controllers' own correction — `KL_SENSE_ROT` / `KL_SENSE_POS`, in
/// the grip's frame, per hand with `_L`/`_R`.
///
/// Separate from `KLHandTune` because the two paths start from different
/// places. `AccessoryAnchor`'s `.grip` is the platform's own answer to "where
/// does a hand hold this and which way does it point", so it needs no
/// hand-shaped compensation at all — what it needs is the gap between Apple's
/// grip convention and the pose an *Oculus Touch* controller would have
/// reported, which is the thing the guest is actually built against and which
/// nothing on this platform can tell us.
///
/// Two of the three defaults are ALVR's, from `WorldTracker.swift`
/// (`controllerToAlvrDeviceMotion`) — the same controller, the same anchor, the
/// same OS:
///
///   * **5.037 degrees about X.** Not a guess: it is the tilt in SteamVR's own
///     PSVR2 controller model JSON, i.e. the offset between the tracked frame
///     and the handle a user perceives.
///   * **(0.002, 0, -0.01) m**, the origin nudge ALVR applies with it.
///
/// The third is this guest's, and it is the larger one:
///
///   * **`hiltPitch`, -35 degrees about X.** Measured the only way it can be —
///     by playing, and reading the number back off Beat Saber's own in-game
///     controller adjustment, where 35 degrees on X is what put the hilts
///     straight. It is a *convention* gap rather than a comfort angle: the
///     platform's `.grip` is where the hand holds the controller, and what the
///     guest is built against is the pose an Oculus Touch would have reported,
///     which is not the same frame and cannot be derived from here.
///
///     **Negative**, because the first device run at +35 pitched the hilts
///     backward — the game's adjustment is applied in Unity's left-handed
///     frame and ours is not, so the magnitude carries over and the sign does
///     not.
///
/// Which leaves the two constants pulling opposite ways, for -29.96 total. If
/// the residual after a playtest looks like about five degrees *forward*, the
/// likely reading is that ALVR's PSVR2 tilt wants this frame's sign too: that
/// would make -40.04, and Beat Saber's own
/// `OculusVRHelper.AdjustControllerTransform` pitches a Touch controller by 40
/// degrees. `KL_SENSE_PITCH=-45.037` is that experiment in one run.
private struct KLSenseTune {
    /// ALVR's PSVR2 comfort tilt and this guest's hilt pitch, kept apart so the
    /// log line and the source agree on where each degree came from.
    static let psvr2Tilt: Float = 5.037
    static let hiltPitch: Float =
        ProcessInfo.processInfo.environment["KL_SENSE_PITCH"].flatMap { Float($0) } ?? -35

    var rot = klEulerXYZ(SIMD3<Float>(psvr2Tilt + hiltPitch, 0, 0))
    var pos = SIMD3<Float>(0.002, 0, -0.01)

    static let shared: [KLSenseTune] = [make(0), make(1)]

    private static func make(_ hand: Int) -> KLSenseTune {
        var t = KLSenseTune()
        if hand == 0 {
            NSLog(String(format: "[cp] Sense grip pitch %.3f deg about X "
                         + "(%.3f PSVR2 tilt + %.3f hilt; KL_SENSE_PITCH, "
                         + "KL_SENSE_ROT override)",
                         psvr2Tilt + hiltPitch, psvr2Tilt, hiltPitch))
        }
        if let (d, k) = klEnvTriple("KL_SENSE_ROT", hand) {
            t.rot = klEulerXYZ(d)
            NSLog("[cp] \(k): hand \(hand) Sense grip rotation \(d) degrees")
        }
        if let (p, k) = klEnvTriple("KL_SENSE_POS", hand) {
            t.pos = p
            NSLog("[cp] \(k): hand \(hand) Sense grip offset \(p) m")
        }
        return t
    }

    /// `KL_SENSE_VEL_FRAME=world` — treat `AccessoryAnchor.velocity` as already
    /// being in tracking space instead of the accessory's own frame.
    ///
    /// The default follows ALVR, which rotates both velocities by the grip
    /// orientation before using them, i.e. reads them as controller-local.
    /// Apple documents neither frame, and the two differ only in *direction*
    /// (the magnitude is the same either way), so this is the knob that settles
    /// it on device: shake the controller along one axis and see which reading
    /// stays pointed the way your hand went.
    static let velocityIsLocal =
        ProcessInfo.processInfo.environment["KL_SENSE_VEL_FRAME"] != "world"
}

final class KleptonControllers {
    // 0 = left, 1 = right, matching kl_ovrp's hand indices (nodes 3 and 4).
    private var state: [KleptonHandState] = [KleptonHandState(), KleptonHandState()]

    // Poses arrive asynchronously from AccessoryTrackingProvider's anchor
    // stream, while buttons are polled synchronously on the render thread, so
    // the two halves genuinely do cross threads and the lock is not defensive.
    private let lock = NSLock()

    /// The most recent anchor per hand — **the object, not the pose it carried**.
    ///
    /// This used to be the resolved `(position, orientation)` pair, and that is
    /// what capped the controllers at the anchor stream's own rate. The stream
    /// is a *sensor* feed: it delivers when the tracker has something new, which
    /// on this platform is far below display rate, so a render loop reading the
    /// last delivered pose repeats each sample for several frames. Hands do not
    /// hold still for two frames at a time, so what that looks like is not
    /// smooth-but-late motion, it is a controller stepping — 30 Hz motion inside
    /// a 90 Hz picture.
    ///
    /// Keeping the anchor keeps `AccessoryTrackingProvider.predictAnchor(for:at:)`
    /// available, which is the platform's own extrapolator: ask it for the
    /// frame's *presentation* time and every frame gets its own pose, forward to
    /// where the controller will be when the light reaches the eye. That is the
    /// same thing `queryDeviceAnchor(atTimestamp:)` already does for the head —
    /// the controllers were simply never given the equivalent.
    private var accessoryAnchor: [Int: AccessoryAnchor] = [:]
    /// Held so the render thread can predict against it. `makeAccessoryProvider`
    /// is the only writer, and it runs before the session that feeds it.
    private var accessoryProvider: AccessoryTrackingProvider?

    private(set) var senseConnected = false
    private var tracked: [GCController] = []

    // MARK: - Discovery

    /// The spatial controllers paired right now.
    static func spatialControllers() -> [GCController] {
        guard #available(visionOS 26.0, *) else { return [] }
        return GCController.controllers().filter {
            $0.productCategory == GCProductCategorySpatialController
        }
    }

    /// Has the paired set changed since the last time this was asked?
    ///
    /// Polled rather than driven off `GCControllerDidConnect`, because the set
    /// is what the provider is built from and a notification would only send us
    /// back here to read it anyway. Cheap: an array compare of a list that is
    /// almost always empty or two long.
    func spatialControllersChanged() -> Bool {
        let now = Self.spatialControllers()
        guard now != tracked else { return false }
        tracked = now
        return true
    }

    /// Build the accessory-tracking provider for whatever is paired now.
    ///
    /// Built **unconditionally** when the capability exists, including with an
    /// empty accessory list. Skipping it while nothing is paired was the wrong
    /// instinct: the provider set is fixed when `ARKitSession.run` is called,
    /// so a run that omitted it could never pick a controller up, which is what
    /// made reconnects look like a platform limitation. They are not — the
    /// session is simply re-`run()` with a rebuilt list when the set changes
    /// (`spatialControllersChanged`), without stopping it first.
    func makeAccessoryProvider() async -> (any DataProvider)? {
        guard #available(visionOS 26.0, *), AccessoryTrackingProvider.isSupported else { return nil }
        let spatial = Self.spatialControllers()
        tracked = spatial

        var accessories: [Accessory] = []
        for c in spatial {
            // One failure must not take the other controller with it — they are
            // paired independently and a half-working pair is the normal state
            // while one is waking up.
            do { accessories.append(try await Accessory(device: c)) }
            catch { NSLog("[cp] accessory init failed for \(c.vendorName ?? "?"): \(error)") }
        }
        senseConnected = !accessories.isEmpty
        NSLog("[cp] spatial controllers: \(accessories.map { $0.name })")
        for c in spatial { Self.census(c) }
        let p = AccessoryTrackingProvider(accessories: accessories)
        lock.lock(); accessoryProvider = p; lock.unlock()
        return p
    }

    /// Every element name a controller carries, once, when it attaches.
    ///
    /// `pollButtons` reads elements by name, and a name this hardware does not
    /// publish is indistinguishable from a button nobody pressed — the lookup
    /// returns nil, `?? false` makes it a release, and the control is silently
    /// dead for the whole run. Two bugs of exactly that shape were live at once:
    /// the thumbstick was read as two AXIS elements when `GCInput` models a
    /// stick as a DPAD, and the PS button (`GCInputButtonHome`) may not be
    /// delivered to an app at all on this OS. Neither can be seen from the host
    /// and neither raises anything.
    ///
    /// Three collections, because which one an element lives in is exactly what
    /// was wrong. One line per controller at attach.
    private static func census(_ c: GCController) {
        guard #available(visionOS 26.0, *) else { return }
        let p = c.physicalInputProfile
        NSLog("[cp] \(c.vendorName ?? "?") elements: "
              + "buttons \(p.buttons.keys.sorted()) "
              + "axes \(p.axes.keys.sorted()) "
              + "dpads \(p.dpads.keys.sorted())")
    }

    /// Element names seen pressed at least once, across every controller.
    nonisolated(unsafe) private static var pressSeen = Set<String>()

    /// Name each button element the first time it is actually pressed.
    ///
    /// The census says what exists; this says what the hardware sends when a
    /// person presses it, and only the pair settles a mapping. It is what will
    /// answer the PS button question on the next device run — an element that
    /// exists and never presses is the system eating it, an element that
    /// presses under a name we do not read is a one-line fix, and neither looks
    /// different from the other in any log we had.
    ///
    /// Bounded by construction: one line per distinct name, ever.
    private static func notePress(_ name: String) {
        if pressSeen.insert(name).inserted {
            NSLog("[cp] button element pressed for the first time: \"\(name)\"")
        }
    }

    /// Drop the pose for any hand whose controller has gone away, so a
    /// disconnect cannot leave the guest holding the last pose forever. The
    /// hand-tracking fallback takes over on the next frame.
    func forgetAccessoryPoses() {
        // The haptics engines belong to the GCControllers that are going away.
        // Kept past a disconnect they are engines on a dead device, and the
        // next `makePlayer` throws rather than returning nil — which is a
        // caught error per frame for the rest of the run.
        releaseHaptics()
        lock.lock()
        accessoryAnchor.removeAll()
        // The provider goes too: predicting against a provider whose session is
        // being torn down and rebuilt is the one way a stale anchor could still
        // produce a fresh-looking pose after the controller is gone.
        accessoryProvider = nil
        lock.unlock()
        for i in 0...1 {
            lock.lock(); state[i] = KleptonHandState(); lock.unlock()
        }
    }

    /// Consume the accessory anchor stream. Runs for the life of the session.
    func consumeAccessoryAnchors(_ provider: any DataProvider) async {
        guard #available(visionOS 26.0, *),
              let p = provider as? AccessoryTrackingProvider else { return }
        for await update in p.anchorUpdates {
            let anchor = update.anchor
            // An unspecified chirality is a controller that has not decided
            // which hand it is in. Treated as the right hand, which is the one
            // a single controller is overwhelmingly likely to be, and is what
            // ALVR does.
            let hand: Int
            switch anchor.accessory.inherentChirality {
            case .left:  hand = 0
            case .right: hand = 1
            default:     hand = 1
            }
            // The anchor itself is stored, not the pose read off it: the pose
            // this frame wants is the *predicted* one, and only the anchor can
            // be predicted. See `accessoryAnchor` and `sensePose`.
            //
            // Through a non-async helper: taking a lock directly inside an
            // async function is an error in Swift 6 mode, and the fix is not to
            // suppress it — the critical section genuinely must not span a
            // suspension point, and pushing it into a synchronous function is
            // what guarantees that.
            store(hand: hand, removed: update.event == .removed, anchor: anchor)
        }
    }

    private func store(hand: Int, removed: Bool, anchor: AccessoryAnchor) {
        lock.lock()
        defer { lock.unlock() }
        accessoryAnchor[hand] = removed ? nil : anchor
    }

    /// This hand's Sense controller, predicted to `time` and corrected: grip
    /// pose plus the motion that goes with it, all in tracking space.
    ///
    /// `time` is a `CACurrentMediaTime()`-domain instant — the frame's
    /// presentation time, the same one `queryDeviceAnchor(atTimestamp:)` gets
    /// for the head, so the hands and the head describe one instant rather than
    /// two. Falling back to the raw anchor if prediction refuses keeps a
    /// controller that has just appeared (or one the tracker has lost) visible
    /// instead of snapping it to the synthetic head-relative pose.
    private func sensePose(hand: Int, at time: TimeInterval)
        -> (SIMD3<Float>, simd_quatf, SIMD3<Float>, SIMD3<Float>)? {
        lock.lock()
        let stored = accessoryAnchor[hand]
        let provider = accessoryProvider
        lock.unlock()
        guard let stored, let provider else { return nil }
        let a = provider.predictAnchor(for: stored, at: time) ?? stored

        // `.grip` is the location the platform defines for "where a hand holds
        // this" — the anchor's own transform is the middle of the plastic, and
        // pointing a saber out of that is the same class of bug as pointing one
        // out of the wrist. A controller that publishes no grip (a stylus)
        // falls back to its base.
        let m = a.accessory.locations.contains(.grip)
            ? a.coordinateSpace(for: .grip, correction: .rendered)
                .ancestorFromSpaceTransformFloat().matrix
            : a.originFromAnchorTransform
        let raw = simd_quatf(m)
        let t = KLSenseTune.shared[hand]
        let q = simd_normalize(raw * t.rot)
        let p = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z) + raw.act(t.pos)

        // Motion, rotated out of the accessory's own frame if that is what it
        // is in — by the UNCORRECTED orientation, because the correction above
        // is a change of convention applied to the pose we report, not a claim
        // about the frame the tracker measured in.
        var v = a.velocity, w = a.angularVelocity
        if KLSenseTune.velocityIsLocal { v = raw.act(v); w = raw.act(w) }
        return (p, q, v, w)
    }

    /// Apply the KL_HAND_ROT / KL_HAND_POS tuning, in the grip's own frame.
    private static func tuned(_ p: SIMD3<Float>, _ q: simd_quatf,
                              hand: Int) -> (SIMD3<Float>, simd_quatf) {
        let t = KLHandTune.shared[hand]
        let q2 = q * t.rot
        return (p + q2.act(t.pos), q2)
    }


    /// The hand's **anatomical** frame, built out of joint positions rather than
    /// out of anyone's frame convention — OpenXR's grip basis, by its own
    /// definition:
    ///
    ///   * `-Z` is "the ray through the tube formed by the non-thumb fingers,
    ///     in the direction of little finger to thumb" — so `+Z` runs index
    ///     knuckle to little knuckle.
    ///   * `+X` is the palm normal: *into* the palm for the right hand, *away
    ///     from* it for the left.
    ///   * `+Y` completes it.
    ///
    /// **Why this is worth computing rather than converting.** Two attempts at
    /// converting the wrist frame both got the two hands' relationship wrong,
    /// in opposite directions, because that relationship is a convention nobody
    /// documents and it cannot be derived from anatomy: a joint transform is a
    /// rotation (determinant +1) and a mirror is not, so *something* must differ
    /// between the hands and which axis differs is a free choice Apple made.
    /// Positions have no such freedom. Built from three joint positions, the
    /// frame comes out consistent between hands by construction.
    ///
    /// The chirality that looks like it should need a special case cancels:
    /// `n = (I-W) x (L-W)` points *out of* the palm on the right hand and *into*
    /// it on the left, and OpenXR wants `+X` into the palm on the right and out
    /// of it on the left — so `+X = -n` on both. That cancellation is the whole
    /// reason OpenXR defines `+X` with an explicit per-hand flip: it makes a
    /// symmetric controller held the same way produce the same grip rotation in
    /// either hand, which is exactly the property that has been missing here.
    private static func anatomicalFrame(_ anchor: HandAnchor) -> simd_quatf? {
        guard let sk = anchor.handSkeleton else { return nil }
        let w = sk.joint(.wrist), i = sk.joint(.indexFingerKnuckle),
            l = sk.joint(.littleFingerKnuckle)
        guard w.isTracked, i.isTracked, l.isTracked else { return nil }
        // Anchor-local is enough: the frame is then rotated into the world by
        // the anchor, exactly as the joints are.
        func p(_ j: HandSkeleton.Joint) -> SIMD3<Float> {
            let c = j.anchorFromJointTransform.columns.3
            return SIMD3<Float>(c.x, c.y, c.z)
        }
        let W = p(w), I = p(i), L = p(l)
        let n = simd_cross(I - W, L - W)
        // A flat or degenerate triangle means the joints are stacked and the
        // normal is noise. Refuse rather than emit a frame that spins.
        guard simd_length(n) > 1e-6, simd_length(L - I) > 1e-4 else { return nil }
        let z = simd_normalize(L - I)
        let x = simd_normalize(-n)
        let y = simd_cross(z, x)            // unit already: x and z are orthogonal
        let local = simd_float3x3(columns: (x, y, z))
        return simd_quatf(anchor.originFromAnchorTransform) * simd_quatf(local)
    }

    /// The grip frame expressed in the anatomical frame — the one constant that
    /// relates the two, and the same for both hands by the argument above.
    ///
    /// **Learned from the right hand, applied to the left.** The right hand's
    /// wrist-based conversion is the one that has been observed correct on
    /// device, so it stays untouched and is the reference: whenever the right
    /// hand is tracked, `K = A_right⁻¹ · W_right · R` is measured and latched,
    /// and the left hand is then `A_left · K` — which is the left hand's
    /// anatomically equivalent grip, whatever Apple's wrist convention happens
    /// to be. Zero risk to the hand that already works, and no third guess at
    /// the hand that does not.
    ///
    /// Until the right hand has been seen once, the left falls back to the
    /// wrist conversion, i.e. today's behaviour. `KL_HAND_MIRROR=1` disables
    /// the whole mechanism and restores ALVR's mirrored pair.
    private var gripFromAnatomical: simd_quatf?

    /// The grip pose for a tracked hand: where a held hilt would be, and which
    /// way it would point.
    ///
    /// Position is the middle of the **middle-finger metacarpal** — the bone
    /// running through the palm — taken as the midpoint of its two ends. That
    /// is the bone a fist closes around, so a hilt placed there is a hilt being
    /// gripped rather than swinging about the wrist as the anchor origin does.
    ///
    /// **The offset is cached in the wrist's own frame, and that is not an
    /// optimisation.** Finger joints drop in and out of `isTracked` as the hand
    /// turns, and the old code fell back to the raw anchor origin whenever they
    /// did — a ~7 cm snap between two different definitions of "here", several
    /// times a second while rotating the hand. That is the hilts jumping
    /// around. Holding the last known wrist-local offset instead means losing
    /// the skeleton costs precision, not position.
    private func gripPose(_ anchor: HandAnchor, hand: Int) -> (SIMD3<Float>, simd_quatf) {
        let origin = anchor.originFromAnchorTransform
        let wristRot = simd_quatf(origin)

        // --- orientation.
        var q = wristRot * klGripFromWrist[hand]
        if Self.anatomical, let a = Self.anatomicalFrame(anchor) {
            if hand == 1 {
                // The reference hand. Measure the constant — and LOW-PASS it.
                // K is a constant of the two frames' relationship, so every
                // frame is another sample of the same number; taking the latest
                // one raw would pipe the right hand's joint noise straight into
                // the left hand's pose, which is a jitter with no visible
                // cause. Converges in about two seconds and is then steady.
                let sample = a.inverse * q
                if let k = gripFromAnatomical {
                    gripFromAnatomical = simd_slerp(k, sample, 0.02)
                } else {
                    gripFromAnatomical = sample
                    NSLog(String(format: "[cp] grip-from-anatomical latched from the right hand: "
                                 + "(%.4f, %.4f, %.4f, %.4f) — the left hand now follows it",
                                 sample.imag.x, sample.imag.y, sample.imag.z, sample.real))
                }
            } else if let k = gripFromAnatomical {
                q = a * k
            }
        }

        // --- position, in the wrist's frame so a dropout cannot move it.
        //
        // **Heavily smoothed, and frozen while the trigger is down.** In the
        // wrist's frame this offset is a bone: it is the same number every
        // frame for a given pair of hands, so anything that moves it is either
        // tracker noise or the hand changing shape — and the hand changing
        // shape is exactly what a pinch is. Letting it through means the
        // pointer's origin walks at the instant of the click, which is a ray
        // that leaves the button as you press it. That is indistinguishable
        // from an unreliable trigger and is not a trigger bug at all.
        if !pinchHeld[hand], let sk = anchor.handSkeleton {
            let base = sk.joint(.middleFingerMetacarpal)
            let tip  = sk.joint(.middleFingerKnuckle)
            if base.isTracked && tip.isTracked {
                let a = base.anchorFromJointTransform.columns.3
                let b = tip.anchorFromJointTransform.columns.3
                let m = SIMD3<Float>((a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5)
                // First sample lands, the rest converge — no visible settle,
                // and steady afterwards.
                gripOffset[hand] = gripOffset[hand].map { $0 + (m - $0) * 0.05 } ?? m
            }
        }
        let local = gripOffset[hand] ?? .zero
        let world = origin * SIMD4<Float>(local.x, local.y, local.z, 1)
        return Self.tuned(SIMD3<Float>(world.x, world.y, world.z), q, hand: hand)
    }

    /// Last known grip point in the wrist's own frame, per hand.
    private var gripOffset: [SIMD3<Float>?] = [nil, nil]

    /// KL_HAND_ANATOMICAL=1 — derive the left hand's frame from joint positions
    /// rather than from the constant above. Off by default: it is the more
    /// principled construction and it has never been confirmed on device, and a
    /// default nobody has looked at is not better than a constant three
    /// playtests point at.
    private static let anatomical =
        ProcessInfo.processInfo.environment["KL_HAND_ANATOMICAL"] == "1"

    // MARK: - Polling

    /// Poll the Sense controllers' buttons. Called once per frame from the
    /// render thread, before the guest's frame.
    private func pollButtons() {
        guard #available(visionOS 26.0, *) else { return }

        // Clear both hands *first*, then fill in whatever is connected.
        //
        // The two halves of this seam have independent lifecycles and they do
        // not agree on timing: GameController connect/disconnect is one event
        // stream, ARKit's accessory `anchorUpdates` is another, and either can
        // lead. Filling in place without clearing means a hand whose controller
        // has gone away is simply never written — so its last frame's bits stay
        // pinned, `fromController` stays true, and the fallback in update()
        // never fires. A controller unpaired mid-press would leave the guest
        // holding that trigger down for the rest of the run.
        //
        // Poses are deliberately NOT cleared here: they belong to the ARKit
        // stream, which reports its own `.removed`. Clearing them on the
        // GameController schedule would drop a pose that is still being
        // tracked.
        lock.lock()
        for i in 0...1 {
            state[i].buttons = 0; state[i].touches = 0
            state[i].indexTrigger = 0; state[i].handTrigger = 0
            state[i].stick = .zero; state[i].fromController = false
            // Only the Sense path writes these, and it rewrites them below;
            // a hand that falls back must report no motion rather than the
            // motion its controller had before it was put down.
            state[i].linearVelocity = .zero; state[i].angularVelocity = .zero
        }
        lock.unlock()

        for c in GCController.controllers()
            where c.productCategory == GCProductCategorySpatialController {

            // Chirality by name (`Self.hand(of:)`), because GCController itself
            // does not carry the hand — Accessory does, and the two objects are
            // not joined here. The suffix is what the vendor string actually
            // contains: "PlayStation VR2 Sense Controller (L)".
            let hand = Self.hand(of: c)

            c.input.inputStateQueueDepth = 1
            guard let s = c.input.nextInputState() else { continue }
            let b = s.buttons, ax = s.axes, dp = s.dpads

            // The PSVR2 Sense element names, per controller. Both controllers
            // use the SAME unprefixed names — the hand is the device, not the
            // element — which is why this is keyed off `hand` rather than off
            // "Left …"/"Right …" strings.
            func pressed(_ n: String) -> Bool { b[n]?.pressedInput.isPressed ?? false }
            func value(_ n: String) -> Float { b[n]?.pressedInput.value ?? 0 }
            func touched(_ n: String) -> Bool { b[n]?.touchedInput?.isTouched ?? false }
            func axis(_ n: String) -> Float { ax[n]?.absoluteInput?.value ?? 0 }

            // Every element name this controller reports pressed, once each.
            //
            // A name this hardware does not publish is INDISTINGUISHABLE from a
            // button nobody pressed: the lookup returns nil, `?? false` makes it
            // a release, and the control is silently dead for the whole run.
            // Both of this arc's input bugs are that shape — the thumbstick was
            // being read as two axis elements that do not exist, and the PS
            // button is being read as a name that may not either — and neither
            // has any other symptom. `census()` says what exists; this says what
            // the hardware actually sends when a person presses it, which is the
            // half that settles a mapping.
            // Every alias, not one name: the collection is a Swift Collection of
            // ELEMENTS (it has no `.keys`), and an element is reachable under
            // any of `aliases` — which is the whole point here, since the bug
            // being hunted is reading one under a name it does not answer to.
            for el in b where el.pressedInput.isPressed {
                for n in el.aliases { Self.notePress("\(n) [\(hand == 0 ? "L" : "R")]") }
            }

            // **The Oculus layout, laid over the Sense one.**
            //
            //   Menu  (GCInputButtonMenu)  -> Menu `menu/click`   (RAW_START)
            //                              -> AND Home `system/click` (RAW_BACK)
            //   Share (GCInputButtonShare) -> Menu, if it exists
            //   Home  (GCInputButtonHome)  -> Home, if it exists
            //
            // On an Oculus Touch pair the LEFT controller's Menu button is
            // `Start` and the RIGHT controller's Oculus button is `Home`, and
            // the Oculus button is the one that summons a dashboard. Steam Link
            // suggests `system/click` for it (SL-20 measured 39 of 41 bindings
            // taken, and this is one of them) — and nothing in this file set
            // RAW_BACK at all, so that action was live, active and permanently
            // false, which has no symptom: it decodes correctly and never fires.
            //
            // **`Button Menu` drives both, and that is ALVR's shape** (218ee37
            // moved to exactly this after having each hand's button on one bit).
            // The Sense pair reports only ONE non-game button per controller and
            // reports it under that one name, so mapping it to `system` alone
            // would take the menu button away to gain a dashboard button. Both
            // costs nothing: the Touch profile splits the pair across the hands
            // — LEFT has `menu/click` and no system, RIGHT has `system/click`
            // and no menu — so a hand with no binding for a control has nothing
            // to take the bit, and on the OVRPlugin path OVRInput ORs the two
            // controllers anyway.
            //
            // **The real PlayStation button may not reach us at all.** It is
            // `GCInputButtonHome`, and visionOS 26 hands it to the system the
            // way it hands over the Digital Crown; taking it needs
            // `GCControllerHomeButtonSetting.inAppAction`, which does not exist
            // in the XROS 26.0 SDK this tree builds against. Read anyway, so a
            // visionOS 27 build gets it the moment the system stops eating it —
            // and `notePresses` above is what will say when that happens,
            // which is worth more than a guessed selector.
            let menuish = pressed("Button Menu") || pressed("Button Share")
                       || pressed("Button Options")
            let homeish = pressed("Button Menu") || pressed("Button Home")

            var st = KleptonHandState()
            st.fromController = true
            st.indexTrigger = value("Trigger")
            st.handTrigger  = pressed("Grip") ? 1 : 0
            // **The thumbstick is a DPAD element, not two axes.** `GCInput`
            // models a stick as one `GCDirectionPadElement` carrying `xAxis` and
            // `yAxis` — `input.dpads[GCInputLeftThumbstick]`, and the header
            // says in as many words that looking a dpad up in `axes` fails. The
            // two names read here before ("Thumbstick X Axis" / "Thumbstick Y
            // Axis") are not element names on this hardware, so both lookups
            // returned nil, `?? 0` made that a centred stick, and the guest saw
            // a controller whose stick never moved — with the Up/Down/Left/Right
            // BUTTON bits working the whole time, which is why it read as an
            // analog problem rather than a naming one.
            //
            // The axis fallback is kept for anything that does publish them.
            // ALVR reads `dpads["Thumbstick"]` for the Sense pair; the prefixed
            // names are what a conventional gamepad uses.
            func stick2() -> SIMD2<Float> {
                for n in ["Thumbstick",
                          hand == 0 ? "Left Thumbstick" : "Right Thumbstick"] {
                    if let d = dp[n] { return SIMD2(d.xAxis.value, d.yAxis.value) }
                }
                return SIMD2(axis("Thumbstick X Axis"), axis("Thumbstick Y Axis"))
            }
            st.stick = stick2()

            var bits: UInt32 = 0, touch: UInt32 = 0
            if hand == 0 {
                // The Sense controllers name their face buttons A and B on both
                // hands; the guest's left hand calls them X and Y.
                if pressed("Button A") { bits |= OVRPRawButton.x }
                if pressed("Button B") { bits |= OVRPRawButton.y }
                if touched("Button A") { touch |= OVRPRawButton.x }
                if touched("Button B") { touch |= OVRPRawButton.y }
                if pressed("Thumbstick Button") { bits |= OVRPRawButton.lThumbstick }
                if st.indexTrigger > 0 { bits |= OVRPRawButton.lIndexTrigger }
                if st.handTrigger  > 0 { bits |= OVRPRawButton.lHandTrigger }
                if menuish { bits |= OVRPRawButton.start }
                if homeish { bits |= OVRPRawButton.back }
            } else {
                if pressed("Button A") { bits |= OVRPRawButton.a }
                if pressed("Button B") { bits |= OVRPRawButton.b }
                if touched("Button A") { touch |= OVRPRawButton.a }
                if touched("Button B") { touch |= OVRPRawButton.b }
                if pressed("Thumbstick Button") { bits |= OVRPRawButton.rThumbstick }
                if st.indexTrigger > 0 { bits |= OVRPRawButton.rIndexTrigger }
                if st.handTrigger  > 0 { bits |= OVRPRawButton.rHandTrigger }
                // Both bits on both hands. The Oculus Touch interaction profile
                // splits the pair — the LEFT carries `menu/click` and no
                // system, the RIGHT carries `system/click` and no menu — so a
                // hand whose profile does not have the control simply has no
                // binding to take the bit and cannot cross-fire; and on the
                // OVRPlugin path OVRInput ORs the two controllers anyway, so
                // which hand carried it was never observable there either.
                // Publishing both is what makes the mapping independent of
                // which Sense controller a given physical button sits on.
                if menuish { bits |= OVRPRawButton.start }
                if homeish { bits |= OVRPRawButton.back }
                // The guest reads stick *direction* bits as well as the axes,
                // and only the right hand has them in the raw enum.
                if pressed("Thumbstick Up")    { bits |= OVRPRawButton.rThumbstickUp }
                if pressed("Thumbstick Down")  { bits |= OVRPRawButton.rThumbstickDown }
                if pressed("Thumbstick Left")  { bits |= OVRPRawButton.rThumbstickLeft }
                if pressed("Thumbstick Right") { bits |= OVRPRawButton.rThumbstickRight }
            }
            // A pressed button is a touched one. The capacitive touch inputs
            // above are additive to that, not a replacement: OVRInput treats
            // touch as a superset of press and a press that is not also a touch
            // reads as a hardware fault.
            st.buttons = bits
            st.touches = touch | bits

            // Buttons only. The pose is resolved in update(), where the frame's
            // presentation time is known and the anchor can be predicted to it.
            lock.lock()
            state[hand] = st
            lock.unlock()
        }
    }

    // MARK: - Haptics

    //  The only seam here that runs OUT of the guest. Beat Saber queues an
    //  amplitude envelope through OVRPlugin's buffered haptics API and
    //  `kl_ovrp_haptics_pull` hands it over a pulse at a time (kl_ovrp.c holds
    //  the whole contract); this plays those pulses on the Sense controllers.
    //
    //  **A hand cannot be vibrated.** There is no actuator on the hand-tracking
    //  path and nothing on the headset stands in for one, so a hand-tracked run
    //  drains the queue and drops it. That is not a gap to fill later — it is
    //  what the hardware is.
    //
    //  The engine comes from ALVR (`GCController.haptics` →
    //  `createEngine(withLocality:)`), the *driving* does not, and the
    //  difference was forced by a headset. ALVR's source is discrete events
    //  from a server — one amplitude, one duration — so it plays each as its
    //  own short `.hapticContinuous`. Ours is a continuous 320 Hz envelope, and
    //  playing that as a run of short events is a restart per frame: a held
    //  buzz survives it, a note cut comes out as a blip with no decay.
    //
    //  So: **one long-lived looping player per hand, and the envelope is sent
    //  to it as a dynamic intensity parameter each frame.** No restarts, no
    //  gaps, and the shape of the clip is what reaches the hand. The player is
    //  paused after a second of silence and resumed on demand, so an idle run
    //  is not holding an actuator open.
    //
    //  `KL_HAPTICS_PULSE=1` restores the discrete shape (ALVR's, plus its 32 ms
    //  floor) — the A/B if the continuous player ever misbehaves on hardware,
    //  and the automatic fallback if the advanced player cannot be made.

    /// One engine and one player per hand, created on that hand's first pulse.
    /// Guarded by `lock` because `stoppedHandler` fires on CoreHaptics's own
    /// queue.
    private var hapticEngine: [Int: CHHapticEngine] = [:]
    private var hapticPlayer: [Int: CHHapticAdvancedPatternPlayer] = [:]
    /// Per hand: is the player started, is it paused, when did it last go
    /// silent, and the burst being logged.
    private var hapticRunning = [false, false]
    private var hapticSilentSince: [TimeInterval] = [0, 0]
    private var hapticBurstPeak: [Float] = [0, 0]
    private var hapticBurstStart: [TimeInterval] = [0, 0]
    private var hapticBursts = [0, 0]
    private var hapticPulseMode = [false, false]     // fell back to discrete?
    private var notedHaptics = Set<String>()

    /// `KL_HAPTICS_OFF=1` turns the path off outright — the pull still runs, so
    /// the guest's queue cannot back up, but nothing is played.
    private static let hapticsEnabled = !klEnvOn("KL_HAPTICS_OFF", default: false)
    /// `KL_HAPTICS_GAIN` scales every pulse. Default 0.25, which is ALVR's own
    /// PSVR2 scale (it uses 0.7 for everything else): the Sense actuators are
    /// strong enough that a guest amplitude of 1.0 played at full intensity is
    /// not what the guest meant by it.
    private static let gain = klEnvFloat("KL_HAPTICS_GAIN", 0.25)
    /// `KL_HAPTICS_SHARPNESS` — CoreHaptics's second axis, which OVRPlugin has
    /// no equivalent of (see kl_ovrp.h on why frequency is not in the seam).
    /// 1.0 is ALVR's constant: crisp, which is what a note cut is.
    private static let sharpness = klEnvFloat("KL_HAPTICS_SHARPNESS", 1.0)
    /// `KL_HAPTICS_PULSE=1` — drive with discrete per-frame events (ALVR's
    /// shape) instead of one continuous player. The A/B if the buzz is ever
    /// wrong in a way the envelope does not explain, and what a hand falls back
    /// to on its own if the continuous player cannot be made or started.
    private static let pulseMode = klEnvOn("KL_HAPTICS_PULSE", default: false)
    /// The floor a discrete event is never shorter than — ALVR's "controllers
    /// can't do 10ms vibrations". The continuous path gets the same floor from
    /// kl_ovrp's side, where it is a hold on the level instead.
    /// `KL_HAPTICS_MIN_MS` moves both.
    private static let minOn = klEnvFloat("KL_HAPTICS_MIN_MS", 32) / 1000

    /// Which hand a controller is, by vendor name — the same rule `pollButtons`
    /// uses, and in one place so the two cannot drift apart. GCController does
    /// not carry chirality itself; `Accessory` does, and the two objects are
    /// not joined here.
    static func hand(of c: GCController) -> Int {
        (c.vendorName ?? "").hasSuffix("(L)") ? 0 : 1
    }

    /// Follow both hands' envelopes. Called once a frame from the compositor,
    /// beside `update`.
    ///
    /// The pull is a LEVEL — what the hand should be feeling right now — not an
    /// event to schedule. Draining it unconditionally, even with playback off,
    /// keeps the two sides in step.
    func pumpHaptics() {
        let now = ProcessInfo.processInfo.systemUptime
        for hand in 0...1 {
            var amplitude: Float = 0, seconds: Float = 0
            let live = kl_ovrp_haptics_pull(Int32(hand), &amplitude, &seconds) != 0
            guard Self.hapticsEnabled else { continue }
            let level = live ? min(max(amplitude * Self.gain, 0), 1) : 0
            traceBurst(hand: hand, level: level, at: now)
            if Self.pulseMode || hapticPulseMode[hand] {
                if level > 0 { playPulse(hand: hand, level: level, seconds: seconds) }
            } else {
                drive(hand: hand, level: level, at: now)
            }
        }
    }

    /// The continuous path: hold one looping player per hand and move its
    /// intensity. Starting it costs a `start`; every frame after that is one
    /// `sendParameters`, which is what keeps the envelope unbroken.
    private func drive(hand: Int, level: Float, at now: TimeInterval) {
        if level <= 0 {
            guard hapticRunning[hand] else { return }
            // Silence first, then pause once it has been quiet a while: pausing
            // immediately would make every gap between two clips a stop/start
            // pair, which is the restart this design exists to avoid.
            send(hand: hand, level: 0)
            if hapticSilentSince[hand] == 0 { hapticSilentSince[hand] = now }
            if now - hapticSilentSince[hand] > 1.0 {
                try? hapticPlayer[hand]?.pause(atTime: CHHapticTimeImmediate)
                hapticRunning[hand] = false
                hapticSilentSince[hand] = 0
            }
            return
        }
        hapticSilentSince[hand] = 0
        if !hapticRunning[hand] {
            guard let player = hapticsPlayer(for: hand) else { return }
            do { try player.start(atTime: CHHapticTimeImmediate) }
            catch {
                NSLog("[cp] haptics: hand \(hand) player would not start: \(error)"
                      + " — falling back to discrete pulses")
                hapticPulseMode[hand] = true
                playPulse(hand: hand, level: level, seconds: Self.minOn)
                return
            }
            hapticRunning[hand] = true
        }
        send(hand: hand, level: level)
    }

    private func send(hand: Int, level: Float) {
        guard let player = hapticPlayer[hand] else { return }
        do {
            try player.sendParameters([CHHapticDynamicParameter(
                parameterID: .hapticIntensityControl, value: level, relativeTime: 0)],
                atTime: CHHapticTimeImmediate)
        } catch {
            NSLog("[cp] haptics: hand \(hand) parameter send failed: \(error)"
                  + " — dropping the engine")
            dropHapticsEngine(hand)
        }
    }

    /// ALVR's shape, kept as the fallback and the A/B: one short continuous
    /// event per frame, never shorter than the floor.
    private func playPulse(hand: Int, level: Float, seconds: Float) {
        guard let engine = hapticsEngine(for: hand) else { return }
        do {
            let event = CHHapticEvent(eventType: .hapticContinuous, parameters: [
                CHHapticEventParameter(parameterID: .hapticIntensity, value: level),
                CHHapticEventParameter(parameterID: .hapticSharpness, value: Self.sharpness),
            ], relativeTime: 0, duration: Double(max(seconds, Self.minOn)))
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            try engine.makePlayer(with: pattern).start(atTime: CHHapticTimeImmediate)
        } catch {
            // Dropped rather than retried: every failure mode here (the engine
            // stopped, the device went away, the pattern was rejected) repeats
            // on the next frame, so keeping it would be one caught error per
            // frame for the rest of the run. A rebuild is one lazy `createEngine`.
            NSLog("[cp] haptics: hand \(hand) pulse failed: \(error) — dropping the engine")
            dropHapticsEngine(hand)
        }
    }

    /// One line per BURST rather than per frame: at 90 Hz a held buzz would
    /// otherwise be ninety log lines a second, and what is worth reading is the
    /// shape — how loud it got and how long it lasted. The first burst per hand
    /// is the one that matters most; it is the only evidence the whole chain
    /// (guest clip → queue → pull → engine) reached an actuator.
    private func traceBurst(hand: Int, level: Float, at now: TimeInterval) {
        if level > 0 {
            if hapticBurstStart[hand] == 0 { hapticBurstStart[hand] = now; hapticBurstPeak[hand] = 0 }
            hapticBurstPeak[hand] = max(hapticBurstPeak[hand], level)
            return
        }
        guard hapticBurstStart[hand] != 0 else { return }
        let ms = (now - hapticBurstStart[hand]) * 1000
        hapticBursts[hand] += 1
        if hapticBursts[hand] <= 3 || hapticBursts[hand] % 100 == 0 {
            NSLog(String(format: "[cp] haptics: hand %d burst %d — peak %.2f over %.0f ms",
                         hand, hapticBursts[hand], hapticBurstPeak[hand], ms))
        }
        hapticBurstStart[hand] = 0
    }

    private func hapticsPlayer(for hand: Int) -> CHHapticAdvancedPatternPlayer? {
        if let existing = hapticPlayer[hand] { return existing }
        guard let engine = hapticsEngine(for: hand) else { return nil }
        do {
            // Intensity 1.0 in the pattern, because the dynamic parameter
            // SCALES it — an event authored quieter would cap everything below.
            // The event's own duration is irrelevant: the player loops.
            let event = CHHapticEvent(eventType: .hapticContinuous, parameters: [
                CHHapticEventParameter(parameterID: .hapticIntensity, value: 1.0),
                CHHapticEventParameter(parameterID: .hapticSharpness, value: Self.sharpness),
            ], relativeTime: 0, duration: 1.0)
            let player = try engine.makeAdvancedPlayer(
                with: CHHapticPattern(events: [event], parameters: []))
            player.loopEnabled = true
            hapticPlayer[hand] = player
            return player
        } catch {
            NSLog("[cp] haptics: hand \(hand) has no continuous player (\(error))"
                  + " — falling back to discrete pulses")
            hapticPulseMode[hand] = true
            return nil
        }
    }

    private func hapticsEngine(for hand: Int) -> CHHapticEngine? {
        lock.lock(); let existing = hapticEngine[hand]; lock.unlock()
        if let existing { return existing }
        guard #available(visionOS 26.0, *) else { return nil }
        guard let device = Self.spatialControllers().first(where: { Self.hand(of: $0) == hand })
        else { return nil }
        guard let haptics = device.haptics else {
            noteHaptics("[cp] haptics: \(device.vendorName ?? "controller") reports none")
            return nil
        }
        // One controller per hand, so the whole device is the locality. ALVR
        // asks for `.leftHandle`/`.rightHandle` first because its reference
        // device is one gamepad with two grips; a Sense controller *is* the
        // hand, and asking it for a handle it does not have returns nil.
        var made = haptics.createEngine(withLocality: .default)
        if made == nil { made = haptics.createEngine(withLocality: .all) }
        if made == nil, let any = haptics.supportedLocalities.first {
            made = haptics.createEngine(withLocality: any)
        }
        guard let engine = made else {
            noteHaptics("[cp] haptics: no engine for hand \(hand); "
                        + "localities \(haptics.supportedLocalities)")
            return nil
        }
        engine.stoppedHandler = { [weak self] reason in
            NSLog("[cp] haptics: hand \(hand) engine stopped (reason \(reason.rawValue))")
            self?.dropHapticsEngine(hand)
        }
        // A reset is the media server having gone away and come back: every
        // CoreHaptics object in the process is invalid and the engine has to be
        // started again. Weak on the engine as well as on self — the handler is
        // held BY the engine, so capturing it strongly is a cycle that outlives
        // the controller.
        engine.resetHandler = { [weak self, weak engine] in
            NSLog("[cp] haptics: hand \(hand) engine reset — restarting")
            do { try engine?.start() } catch { self?.dropHapticsEngine(hand) }
        }
        do { try engine.start() } catch {
            noteHaptics("[cp] haptics: hand \(hand) engine would not start: \(error)")
            return nil
        }
        NSLog("[cp] haptics: hand \(hand) engine up on \(device.vendorName ?? "?")")
        lock.lock(); hapticEngine[hand] = engine; lock.unlock()
        return engine
    }

    private func dropHapticsEngine(_ hand: Int) {
        lock.lock()
        let engine = hapticEngine.removeValue(forKey: hand)
        // The player belongs to the engine; keeping it past one would be a
        // player on a dead engine, which throws on every frame's parameter send.
        hapticPlayer.removeValue(forKey: hand)
        hapticRunning[hand] = false
        hapticSilentSince[hand] = 0
        lock.unlock()
        engine?.stop()
    }

    /// Every engine, for a disconnect or teardown.
    func releaseHaptics() {
        lock.lock()
        let engines = hapticEngine
        hapticEngine.removeAll()
        hapticPlayer.removeAll()
        hapticRunning = [false, false]
        hapticSilentSince = [0, 0]
        lock.unlock()
        for (_, e) in engines { e.stop() }
    }

    /// Once per distinct message. These are all "this device cannot do
    /// haptics" shapes, which is a fact about the run rather than an event —
    /// logged every frame it would drown the pinch trace.
    private func noteHaptics(_ msg: String) {
        lock.lock()
        let fresh = notedHaptics.insert(msg).inserted
        lock.unlock()
        if fresh { NSLog("%@", msg) }
    }

    // MARK: - The seam

    /// The index trigger, from a pinch.
    ///
    /// A hand has no buttons, which used to mean a hand-tracked run could reach
    /// the menu and then not click anything in it. The thumb-to-index distance
    /// is the one continuous quantity a hand does supply, and it maps onto a
    /// trigger the way a trigger already behaves.
    ///
    /// **It is a TWO-point calibration, and the closed end is the one that
    /// matters.** The first version was `1 - d / 5cm`, which reads as "touching
    /// is fully pressed" and is wrong about what touching means: `thumbTip` and
    /// `indexFingerTip` are skeletal joints inside fingers that have thickness,
    /// so a hard pinch bottoms out around 1.5 cm apart, not 0. That version
    /// therefore topped out near 0.7 — enough that the guest saw the axis move
    /// (the laser changed with it) and never enough for a full press. It read
    /// as a missing button bit and was a missing 30% of range.
    ///
    /// So: `KL_PINCH_CLOSED` metres (default 1.5 cm) or nearer is 1.0,
    /// `KL_PINCH_OPEN` (default 5 cm) or further is 0.0, linear between. Both
    /// are per-user — fingers differ — which is why they are knobs and why
    /// `KL_PINCH_TRACE=1` reports the distance actually measured.
    ///
    /// The *bit* gets hysteresis; the axis does not. `RawButton.RIndexTrigger`
    /// is what a UI click is edged off, and a bare threshold on a noisy
    /// millimetre of fingertip jitter emits a burst of press/release pairs at
    /// exactly the moment the user meant one click.
    private func pinchTrigger(_ anchor: HandAnchor, hand: Int) -> Float {
        guard let sk = anchor.handSkeleton else { pinchNoSkeleton[hand] += 1; return 0 }
        let thumb = sk.joint(.thumbTip), index = sk.joint(.indexFingerTip)
        guard thumb.isTracked && index.isTracked else {
            pinchNoSkeleton[hand] += 1; return 0
        }
        let o = anchor.originFromAnchorTransform
        let a = (o * thumb.anchorFromJointTransform).columns.3
        let b = (o * index.anchorFromJointTransform).columns.3
        let d = simd_length(SIMD3<Float>(a.x - b.x, a.y - b.y, a.z - b.z))
        let t = max(0, min(1, (Self.pinchOpen - d) / (Self.pinchOpen - Self.pinchClosed)))
        tracePinch(hand: hand, distance: d, value: t)
        return t
    }

    /// The distance that reads as a fully released trigger. KL_PINCH_OPEN.
    private static let pinchOpen: Float = envMetres("KL_PINCH_OPEN", 0.05)
    /// ...and the one that reads as fully pressed. KL_PINCH_CLOSED. Kept below
    /// `pinchOpen` whatever the environment says, because the two are divided.
    ///
    /// **3 cm, raised from the original 1.5.** A device playtest got through
    /// the menus only after raising this by hand, which is a measurement: these
    /// joints bottom out much further apart than the first guess assumed. It is
    /// a *floor*, not a threshold — anything closer also reads 1.0 — so erring
    /// wide costs a little sensitivity and erring narrow costs the click.
    private static let pinchClosed: Float = min(envMetres("KL_PINCH_CLOSED", 0.03),
                                                pinchOpen - 0.005)

    /// KL_PINCH_SRC — which source is allowed to press the button. Default
    /// `both`; `system` or `distance` isolates one when the question is which
    /// of the two is misbehaving.
    private enum PinchSource { case both, system, distance }
    private static let pinchSrc: PinchSource = {
        switch ProcessInfo.processInfo.environment["KL_PINCH_SRC"] {
        case "system":   NSLog("[cp] KL_PINCH_SRC=system — system pinch events only"); return .system
        case "distance": NSLog("[cp] KL_PINCH_SRC=distance — fingertip distance only"); return .distance
        default: return .both
        }
    }()

    /// Which source pressed, counted per report interval — so "the system is
    /// dropping presses" and "the distance never gets there" stop being one
    /// sentence of playtest feedback.
    private var pinchFromSystem = [0, 0]
    private var pinchFromDistance = [0, 0]
    /// Spatial events seen, and those the system could not attribute to a hand.
    private var spatialEvents = 0
    private var spatialNoChirality = 0

    private static func envMetres(_ key: String, _ fallback: Float) -> Float {
        let v = ProcessInfo.processInfo.environment[key].flatMap { Float($0) } ?? 0
        return (v > 0 && v < 0.5) ? v : fallback
    }

    /// The closest pinch seen per hand, the axis value it produced, and whether
    /// the bit was ever set — every two seconds.
    ///
    /// **On by default** (`KL_PINCH_TRACE=0` silences it), because two device
    /// playtests have now reported "the trigger does not click" and neither
    /// could say *which* of the four things in the chain was missing: the
    /// skeleton, the joints, the range, or the threshold. One line every two
    /// seconds distinguishes all four, and this is a bring-up seam where that
    /// is worth more than the log space. `pinches` counts frames where the bit
    /// was actually asserted, so "nothing reached the guest" and "the guest
    /// ignored it" stop looking the same.
    private var pinchMin: [Float] = [9, 9]
    private var pinchPeak: [Float] = [0, 0]
    private var pinchFires: [Int] = [0, 0]
    private var pinchNoSkeleton: [Int] = [0, 0]
    private var pinchNextLog: TimeInterval = 0
    private func tracePinch(hand: Int, distance: Float, value: Float) {
        pinchMin[hand] = min(pinchMin[hand], distance)
        pinchPeak[hand] = max(pinchPeak[hand], value)
    }
    private func reportPinch() {
        guard Self.pinchTrace else { return }
        let now = ProcessInfo.processInfo.systemUptime
        guard now >= pinchNextLog else { return }
        pinchNextLog = now + 2
        // A "closest" of 9 means the distance was never computed at all — no
        // skeleton, or the two fingertip joints were not tracked. That is a
        // different bug from a pinch that is measured and too wide, and it is
        // the one the value alone cannot show.
        func fmt(_ h: Int) -> String {
            pinchMin[h] > 8
                ? String(format: "no skeleton (%d frames)", pinchNoSkeleton[h])
                : String(format: "%.3f m -> %.2f, %d down (sys %d, dist %d)",
                         pinchMin[h], pinchPeak[h], pinchFires[h],
                         pinchFromSystem[h], pinchFromDistance[h])
        }
        NSLog("[cp] pinch: L \(fmt(0)) | R \(fmt(1)) "
              + String(format: "(open %.3f closed %.3f, %d spatial events, %d unattributed)",
                       Self.pinchOpen, Self.pinchClosed, spatialEvents, spatialNoChirality))
        pinchMin = [9, 9]; pinchPeak = [0, 0]
        pinchFires = [0, 0]; pinchNoSkeleton = [0, 0]
        pinchFromSystem = [0, 0]; pinchFromDistance = [0, 0]
        spatialEvents = 0; spatialNoChirality = 0
    }
    private static let pinchTrace =
        ProcessInfo.processInfo.environment["KL_PINCH_TRACE"] != "0"

    /// Debounced press state for the *fallback* pinch trigger, per hand.
    private var pinchHeld = [false, false]

    // MARK: - The system pinch

    /// Whether the system says each hand is currently pinching, and whether it
    /// has ever said anything at all.
    ///
    /// The second flag matters: until an event arrives we cannot tell "not
    /// pinching" from "this callback is never going to fire", and those need
    /// different behaviour. Before the first event the fingertip-distance
    /// fallback still drives the button; after it, the system is the only thing
    /// that presses, because two sources of truth for one bit is how a click
    /// ends up half-registered.
    private var systemPinch = [false, false]
    private var systemPinchSeen = false

    /// `LayerRenderer.onSpatialEvent`. Called on the system's own queue, hence
    /// the lock.
    ///
    /// Only `.indirectPinch` is taken. `.directPinch` is a finger touching a
    /// real surface and `.pointer`/`.touch` are other devices entirely; the
    /// guest's index trigger means "the user is selecting", which is what an
    /// indirect pinch is.
    func handleSpatialEvents(_ events: SpatialEventCollection) {
        for e in events {
            guard e.kind == .indirectPinch else { continue }
            // No chirality means the system could not attribute the pinch to a
            // hand. Attributing it to the right is the same choice the accessory
            // path already makes, and for the same reason: one-handed use is
            // overwhelmingly the right hand.
            let hand: Int
            var unattributed = false
            switch e.chirality {
            case .left:  hand = 0
            case .right: hand = 1
            default:
                hand = 1
                unattributed = true
                if !loggedNoChirality {
                    loggedNoChirality = true
                    NSLog("[cp] spatial pinch with no chirality — attributed to the right hand")
                }
            }
            let active = e.phase == .active
            lock.lock()
            spatialEvents += 1
            if unattributed { spatialNoChirality += 1 }
            if !systemPinchSeen {
                systemPinchSeen = true
                NSLog("[cp] system pinch events are live — the fingertip-distance "
                      + "fallback no longer presses anything")
            }
            systemPinch[hand] = active
            lock.unlock()
        }
    }
    private var loggedNoChirality = false

    /// Merge every source and push the result across to kl_ovrp.
    ///
    /// The hand anchors are the fallback: used for a hand's *pose* and its
    /// pinch trigger, and only where a Sense controller has not supplied one.
    ///
    /// `presentationTime` is when the frame being built will be shown. Every
    /// pose here is predicted to it, so the controllers move at display rate
    /// and describe the same instant the head pose does.
    func update(leftHand: HandAnchor?, rightHand: HandAnchor?,
                at presentationTime: TimeInterval) {
        pollButtons()

        for hand in 0...1 {
            lock.lock()
            var st = state[hand]
            lock.unlock()

            let accPose = sensePose(hand: hand, at: presentationTime)
            if let accPose {
                st.position = accPose.0; st.orientation = accPose.1
                st.linearVelocity = accPose.2; st.angularVelocity = accPose.3
            } else if let anchor = (hand == 0 ? leftHand : rightHand) {
                let (p, q) = gripPose(anchor, hand: hand)
                st.position = p; st.orientation = q
                // pollButtons() has already cleared this hand's bits (no Sense
                // controller wrote it), so everything the pinch sets is set
                // here and nothing has to be undone.
                let t = pinchTrigger(anchor, hand: hand)
                lock.lock()
                let sysSeen = systemPinchSeen, sysDown = systemPinch[hand]
                lock.unlock()

                // **Both sources, OR'd**, and that is deliberate rather than
                // indecisive. Each has now been observed unreliable on its own:
                // thresholding the fingertip distance chattered, and the
                // system's own recogniser drops presses in here too — plausibly
                // because `.mixed` immersion leaves the system competing for
                // the same gesture (try KL_FULL=1), or because a pinch it
                // cannot attribute to a hand is not one we can use. Their
                // failures are not correlated: the system misses presses the
                // distance sees, and the distance chatters where the system's
                // hysteresis is clean. A union is more reliable than either,
                // and for a *button* a union is also the right shape — the cost
                // of a spurious release is a dropped click, the cost of a
                // spurious press is nothing the user will notice.
                //
                // KL_PINCH_SRC=system|distance isolates one when the question
                // is which of them is misbehaving.
                let byDistance = Self.pinchSrc != .system
                    && (pinchHeld[hand] ? (t > 0.30) : (t > 0.55))
                let bySystem = Self.pinchSrc != .distance && sysSeen && sysDown
                let pressed = byDistance || bySystem
                if bySystem { pinchFromSystem[hand] += 1 }
                if byDistance { pinchFromDistance[hand] += 1 }

                // The analog value stays the measured distance — the guest's
                // laser reacts to it and a trigger that snaps 0->1 loses that —
                // but a press pins it to 1.0 so the axis can never disagree
                // with the bit.
                st.indexTrigger = pressed ? 1 : t
                pinchHeld[hand] = pressed
                if pressed {
                    pinchFires[hand] += 1
                    let bit = hand == 0 ? OVRPRawButton.lIndexTrigger
                                        : OVRPRawButton.rIndexTrigger
                    st.buttons |= bit; st.touches |= bit
                }
            } else {
                // Nothing tracked this hand. Leave kl_ovrp's own synthesised
                // head-relative hand alone rather than pushing a stale pose —
                // it at least keeps the controllers inside the frustum.
                continue
            }

            if let p = st.position, let q = st.orientation {
                kl_ovrp_set_hand_motion(Int32(hand), p.x, p.y, p.z,
                                        q.imag.x, q.imag.y, q.imag.z, q.real,
                                        st.linearVelocity.x, st.linearVelocity.y,
                                        st.linearVelocity.z,
                                        st.angularVelocity.x, st.angularVelocity.y,
                                        st.angularVelocity.z)
            }
            kl_ovrp_set_controller_input(Int32(hand), st.buttons, st.touches,
                                         st.indexTrigger, st.handTrigger,
                                         st.stick.x, st.stick.y)

            lock.lock(); state[hand] = st; lock.unlock()
        }
        reportPinch()
    }
}
