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
//      real answer — it is the only source that has buttons at all.
//    * **Hands**, always. `HandTrackingProvider`'s wrist anchor stands in for
//      the grip pose. Poses only: a hand has no trigger, so nothing clicks.
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
import GameController
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

final class KleptonControllers {
    // 0 = left, 1 = right, matching kl_ovrp's hand indices (nodes 3 and 4).
    private var state: [KleptonHandState] = [KleptonHandState(), KleptonHandState()]

    // Poses arrive asynchronously from AccessoryTrackingProvider's anchor
    // stream, while buttons are polled synchronously on the render thread, so
    // the two halves genuinely do cross threads and the lock is not defensive.
    private let lock = NSLock()
    private var accessoryPose: [Int: (SIMD3<Float>, simd_quatf)] = [:]

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
        return AccessoryTrackingProvider(accessories: accessories)
    }

    /// Drop the pose for any hand whose controller has gone away, so a
    /// disconnect cannot leave the guest holding the last pose forever. The
    /// hand-tracking fallback takes over on the next frame.
    func forgetAccessoryPoses() {
        lock.lock(); accessoryPose.removeAll(); lock.unlock()
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
            let m = anchor.originFromAnchorTransform
            // Through a non-async helper: taking a lock directly inside an
            // async function is an error in Swift 6 mode, and the fix is not to
            // suppress it — the critical section genuinely must not span a
            // suspension point, and pushing it into a synchronous function is
            // what guarantees that.
            store(hand: hand, removed: update.event == .removed, transform: m)
        }
    }

    private func store(hand: Int, removed: Bool, transform m: simd_float4x4) {
        lock.lock()
        defer { lock.unlock() }
        accessoryPose[hand] = removed ? nil
            : (SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z), simd_quatf(m))
    }

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
        }
        lock.unlock()

        for c in GCController.controllers()
            where c.productCategory == GCProductCategorySpatialController {

            // Chirality by name, because GCController itself does not carry the
            // hand — Accessory does, and the two objects are not joined here.
            // The suffix is what the vendor string actually contains:
            // "PlayStation VR2 Sense Controller (L)".
            let name = c.vendorName ?? ""
            let hand = name.hasSuffix("(L)") ? 0 : 1

            c.input.inputStateQueueDepth = 1
            guard let s = c.input.nextInputState() else { continue }
            let b = s.buttons, ax = s.axes

            // The PSVR2 Sense element names, per controller. Both controllers
            // use the SAME unprefixed names — the hand is the device, not the
            // element — which is why this is keyed off `hand` rather than off
            // "Left …"/"Right …" strings.
            func pressed(_ n: String) -> Bool { b[n]?.pressedInput.isPressed ?? false }
            func value(_ n: String) -> Float { b[n]?.pressedInput.value ?? 0 }
            func touched(_ n: String) -> Bool { b[n]?.touchedInput?.isTouched ?? false }
            func axis(_ n: String) -> Float { ax[n]?.absoluteInput?.value ?? 0 }

            var st = KleptonHandState()
            st.fromController = true
            st.indexTrigger = value("Trigger")
            st.handTrigger  = pressed("Grip") ? 1 : 0
            st.stick = SIMD2<Float>(axis("Thumbstick X Axis"), axis("Thumbstick Y Axis"))

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
                if pressed("Button Menu") { bits |= OVRPRawButton.start }
            } else {
                if pressed("Button A") { bits |= OVRPRawButton.a }
                if pressed("Button B") { bits |= OVRPRawButton.b }
                if touched("Button A") { touch |= OVRPRawButton.a }
                if touched("Button B") { touch |= OVRPRawButton.b }
                if pressed("Thumbstick Button") { bits |= OVRPRawButton.rThumbstick }
                if st.indexTrigger > 0 { bits |= OVRPRawButton.rIndexTrigger }
                if st.handTrigger  > 0 { bits |= OVRPRawButton.rHandTrigger }
                if pressed("Button Menu") { bits |= OVRPRawButton.start }
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

            lock.lock()
            if let p = accessoryPose[hand] { st.position = p.0; st.orientation = p.1 }
            state[hand] = st
            lock.unlock()
        }
    }

    // MARK: - The seam

    /// Merge every source and push the result across to kl_ovrp.
    ///
    /// `handAnchors` is the hand-tracking fallback: used for a hand's *pose*
    /// only, and only where a Sense controller has not already supplied one.
    func update(leftHand: simd_float4x4?, rightHand: simd_float4x4?) {
        pollButtons()

        for hand in 0...1 {
            lock.lock()
            var st = state[hand]
            let accPose = accessoryPose[hand]
            lock.unlock()

            if let accPose {
                st.position = accPose.0; st.orientation = accPose.1
            } else if let m = (hand == 0 ? leftHand : rightHand) {
                st.position = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
                st.orientation = simd_quatf(m)
                // A hand has no buttons; pollButtons() has already cleared them
                // for any hand no controller wrote this frame, so there is
                // nothing to undo here.
            } else {
                // Nothing tracked this hand. Leave kl_ovrp's own synthesised
                // head-relative hand alone rather than pushing a stale pose —
                // it at least keeps the controllers inside the frustum.
                continue
            }

            if let p = st.position, let q = st.orientation {
                kl_ovrp_set_hand_pose(Int32(hand), p.x, p.y, p.z,
                                      q.imag.x, q.imag.y, q.imag.z, q.real)
            }
            kl_ovrp_set_controller_input(Int32(hand), st.buttons, st.touches,
                                         st.indexTrigger, st.handTrigger,
                                         st.stick.x, st.stick.y)

            lock.lock(); state[hand] = st; lock.unlock()
        }
    }
}
