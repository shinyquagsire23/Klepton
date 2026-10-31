// Independent Surreal Touch interoperability implementation. See docs/SURREAL.md.
// No OpenSurreal code or dependency is included in this runtime.
import Foundation
import simd

// Unknown/ambiguous device names never silently become a right controller.
enum SurrealHand: Int, CaseIterable {
    case left = 0, right = 1
    init?(name: String) {
        let words = Set(name.uppercased().split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init))
        guard words.contains("SURREAL"), words.contains("TOUCH") else { return nil }
        let left = !words.isDisjoint(with: ["L", "LEFT"])
        let right = !words.isDisjoint(with: ["R", "RIGHT"])
        guard left != right else { return nil }
        self = left ? .left : .right
    }
}

struct SurrealInputSettings: Equatable {
    static func stored(defaults: UserDefaults = .standard) -> Self {
        let scale = defaults.double(forKey: "klepton.surreal.analogScale")
        let pitch = defaults.double(forKey: "klepton.surreal.gripPitch")
        return Self(analogScale: scale == 255 ? 255 : 127,
                    invertY: defaults.bool(forKey: "klepton.surreal.invertY"),
                    gripPitch: pitch.isFinite ? Float(min(90, max(-90, pitch))) : 0,
                    positionFromHands: defaults.bool(forKey: "klepton.surreal.positionFromHands"),
                    aimOffsets: (0...1).map { SurrealAimOffset.stored(hand: $0, defaults: defaults) })
    }
    var analogScale: Float = 127
    var deadZone: Float = 0.12
    var invertY = false
    // Body-to-grip correction is separate from the world registration. Tune on hardware.
    var gripPitch: Float = 0
    var positionFromHands = false
    var aimOffsets = [SurrealAimOffset(), SurrealAimOffset()]
    var bodyFromGrip: simd_float4x4 {
        simd_float4x4(simd_quatf(angle: gripPitch * .pi / 180, axis: [1, 0, 0]))
    }
}

enum SurrealWire {
    static let service = "6E401001-B5A3-F393-E0A9-E50E24DCCA9E"
    static let pose = "6E401002-B5A3-F393-E0A9-E50E24DCCA9E"
    static let input = "6E401003-B5A3-F393-E0A9-E50E24DCCA9E"
    static let haptic = "6E401004-B5A3-F393-E0A9-E50E24DCCA9E"
    // The haptic characteristic is the device's TLV control channel; replies arrive on 1006.
    // Message types per github.com/shinyquagsire23/SurrealTouch_re (dm_common_enums.py).
    static let controlResponse = "6E401006-B5A3-F393-E0A9-E50E24DCCA9E"
    static let modeSet: UInt8 = 0x0C       // 1 u8 arg: 0 = controller mode, 1 = mouse (cursor) mode
    static let modeRequest: UInt8 = 0x0D
    static let modeSetResponse: UInt8 = 0x0E
    static let controllerMode: UInt8 = 0x00

    /// One control message: type, u16 length (sequence + data), u16 sequence, data.
    static func tlv(type: UInt8, sequence: UInt16, data: [UInt8] = []) -> Data {
        let length = UInt16(data.count + 2)
        var bytes: [UInt8] = [type, UInt8(length & 0xFF), UInt8(length >> 8),
                              UInt8(sequence & 0xFF), UInt8(sequence >> 8)]
        bytes.append(contentsOf: data)
        return Data(bytes)
    }

    /// A reply from 1006 as (type, sequence, data), or nil when it is shorter than its header says.
    static func parseTLV(_ data: Data) -> (type: UInt8, sequence: UInt16, data: [UInt8])? {
        let b = [UInt8](data)
        guard b.count >= 5 else { return nil }
        let size = Int(integer(b, 1, 2))
        guard size >= 2, b.count >= 3 + size else { return nil }
        return (b[0], UInt16(integer(b, 3, 2)), Array(b[5..<(3 + size)]))
    }

    static func integer(_ bytes: [UInt8], _ offset: Int, _ count: Int) -> UInt64 {
        (0..<count).reduce(0) { $0 | UInt64(bytes[offset + $1]) << (8 * $1) }
    }
    static func axis(_ value: UInt8) -> Float { max(-1, (Float(value) - 128) / 127) }
    static func stick(_ x: UInt8, _ y: UInt8, settings: SurrealInputSettings) -> SIMD2<Float> {
        let v = SIMD2(axis(x), axis(y) * (settings.invertY ? -1 : 1))
        let length = simd_length(v), dead = min(0.9, max(0, settings.deadZone))
        guard length > dead else { return .zero }
        return v / length * ((min(1, length) - dead) / (1 - dead))
    }
    static func buttons(_ data: Data, hand: SurrealHand, settings: SurrealInputSettings) -> KleptonHandState? {
        guard data.count == 13 else { return nil }
        let b = Array(data), left = hand == .left
        var result = KleptonHandState()
        result.fromController = true
        result.indexTrigger = min(1, Float(b[9]) / max(1, settings.analogScale))
        result.handTrigger = min(1, Float(b[10]) / max(1, settings.analogScale))
        result.stick = stick(b[11], b[12], settings: settings)
        let mapping: [(UInt8, UInt32)] = [
            (1, left ? OVRPRawButton.x : OVRPRawButton.a),
            (2, left ? OVRPRawButton.y : OVRPRawButton.b),
            (4, left ? OVRPRawButton.start : OVRPRawButton.back),
            (8, left ? OVRPRawButton.lThumbstick : OVRPRawButton.rThumbstick)]
        for (flag, bit) in mapping where b[8] & flag != 0 { result.buttons |= bit }
        if result.indexTrigger >= 0.55 { result.buttons |= left ? OVRPRawButton.lIndexTrigger : OVRPRawButton.rIndexTrigger }
        if result.handTrigger >= 0.55 { result.buttons |= left ? OVRPRawButton.lHandTrigger : OVRPRawButton.rHandTrigger }
        if result.stick.y > 0.7 { result.buttons |= left ? OVRPRawButton.lThumbstickUp : OVRPRawButton.rThumbstickUp }
        if result.stick.y < -0.7 { result.buttons |= left ? OVRPRawButton.lThumbstickDown : OVRPRawButton.rThumbstickDown }
        if result.stick.x < -0.7 { result.buttons |= left ? OVRPRawButton.lThumbstickLeft : OVRPRawButton.rThumbstickLeft }
        if result.stick.x > 0.7 { result.buttons |= left ? OVRPRawButton.lThumbstickRight : OVRPRawButton.rThumbstickRight }
        // The documented packet has no capacitive-touch fields. Only touches the input
        // itself proves are reported: a pressed face button or stick, and a stick pushed
        // past the dead zone. Half-Life: Alyx's teleport is a SteamVR dpad with sub_mode
        // "touch", which ignores a pushed stick unless /input/thumbstick/touch is set.
        let stickBit = left ? OVRPRawButton.lThumbstick : OVRPRawButton.rThumbstick
        let touchable = stickBit | (left ? OVRPRawButton.x | OVRPRawButton.y : OVRPRawButton.a | OVRPRawButton.b)
        result.touches = result.buttons & touchable
        if result.stick != .zero { result.touches |= stickBit }
        return result
    }
    static func vibration(sequence: UInt16, amplitude: Float, duration: Float, frequency: Float = 170) -> Data {
        let a = amplitude.isFinite ? min(1, max(0, amplitude)) : 0
        let d = duration.isFinite ? min(0.25, max(0.03, duration)) : 0.03
        let f = frequency.isFinite ? min(300, max(20, frequency)) : 170
        var bytes: [UInt8] = [0x18]
        for (number, count) in [(UInt64(10), 2), (UInt64(sequence), 2), (UInt64(a * 10000), 2),
                                (UInt64(f), 2), (UInt64((d * 1000).rounded()), 4)] {
            for i in 0..<count { bytes.append(UInt8(truncatingIfNeeded: number >> (i * 8))) }
        }
        return Data(bytes)
    }
}

struct SurrealPose {
    var ticks: UInt64
    var time: Double = 0
    var transform: simd_float4x4
    var confidence: Float
    var linear: SIMD3<Float>
    var angular: SIMD3<Float>
    init?(_ data: Data) {
        guard data.count == 76 else { return nil }
        let bytes = Array(data)
        let values = (0..<17).map { Float(bitPattern: UInt32(SurrealWire.integer(bytes, 8 + $0 * 4, 4))) }
        guard values.allSatisfy(\.isFinite) else { return nil }
        let q = SIMD4<Float>(-values[3], -values[4], values[5], values[6])
        guard simd_length(q) > 0.5, simd_length(q) < 1.5 else { return nil }
        ticks = SurrealWire.integer(bytes, 0, 8)
        transform = simd_float4x4(simd_quatf(vector: simd_normalize(q)))
        transform.columns.3 = [values[0], values[1], -values[2], 1]
        confidence = values[7]
        linear = [values[8], values[9], -values[10]]
        // Angular velocity is an axial vector under the Z reflection.
        angular = [-values[11], -values[12], values[13]]
    }
    func predicted(at target: Double) -> simd_float4x4 {
        let dt = Float(min(0.035, max(0, target - time)))
        let speed = simd_length(angular)
        var q = simd_quatf(transform)
        if speed > 0.00001 { q = simd_quatf(angle: speed * dt, axis: angular / speed) * q }
        var output = simd_float4x4(q)
        output.columns.3 = transform.columns.3 + SIMD4<Float>(linear * dt, 0)
        return output
    }
}

/// Constant-memory lower-envelope clock estimator, nanosecond firmware ticks -> media time.
/// BLE latency cannot be measured one-way; the fastest observed delivery is the reference.
struct SurrealClock {
    var offset: Double?
    var lastTicks: UInt64?
    mutating func stamp(_ ticks: UInt64, arrival: Double) -> (time: Double, reset: Bool) {
        let reboot = lastTicks.map { ticks < $0 } ?? false
        let seconds = Double(ticks) * 1e-9, observed = arrival - Double(ticks) * 1e-9
        let reset = reboot || offset.map({ abs(observed - $0) > 0.5 }) == true
        if reset { offset = nil }
        // Slow upward drift allowance; downward changes immediately track a faster packet.
        offset = offset.map { min(observed, $0 + 0.000002) } ?? observed
        lastTicks = ticks
        let estimated = seconds + offset!
        return (arrival - estimated <= 0.25 ? min(arrival, estimated) : arrival, reset)
    }
}

/// Average a short, stable orientation reference, then let the caller latch it.
/// An interrupted grip or noisy reference starts a fresh window instead of
/// calibrating from the first frame in which a hand happens to become visible.
struct SurrealOrientationCalibration {
    private var window: (start: Double, last: Double, first: simd_quatf, mean: simd_quatf, count: Int)?
    mutating func sample(_ candidate: simd_quatf, at time: Double) -> simd_quatf? {
        if let window, time == window.last { return nil }
        guard var next = window, time > next.last, time - next.last < 0.1,
              abs(simd_dot(next.first.vector, candidate.vector)) >= cos(5 * Float.pi / 360) else {
            window = (time, time, candidate, candidate, 1)
            return nil
        }
        next.count += 1
        next.mean = simd_slerp(next.mean, candidate, 1 / Float(next.count)).normalized
        next.last = time
        if time - next.start >= 0.35 {
            window = nil
            return next.mean
        }
        window = next
        return nil
    }
}

/// All calls made while the backend's lock is held. No Bluetooth calls under that lock.
/// Five grip squeezes on BOTH controllers within three seconds: the in-game way to throw
/// away registration when the controllers have drifted, without going back to the launcher.
/// A squeeze is a press above 0.55 after a release below 0.35, so a held grip counts once.
struct SurrealResetGesture {
    static let squeezes = 5
    static let window: Double = 3
    private var pressed = [false, false]
    private var presses: [[Double]] = [[], []]

    /// Feed every input packet; true once, when the gesture completes.
    mutating func update(hand: Int, grip: Float, at time: Double) -> Bool {
        guard (0...1).contains(hand), grip.isFinite else { return false }
        if pressed[hand] {
            if grip < 0.35 { pressed[hand] = false }
            return false
        }
        guard grip > 0.55 else { return false }
        pressed[hand] = true
        presses[hand].append(time)
        for h in 0...1 { presses[h].removeAll { time - $0 > Self.window } }
        guard presses.allSatisfy({ $0.count >= Self.squeezes }) else { return false }
        presses = [[], []]
        return true
    }
}

struct SurrealHandCache {
    var deviceID: UUID?
    var connected = false
    var connectionRequested = false
    var input = KleptonHandState()
    var pose: SurrealPose?
    var clock = SurrealClock()
    var receivedAt: Double = 0
    var hasInput = false
    var registration: simd_float4x4?
    var lastCalibration: Double = 0
    var status = "Disconnected"
    var battery: Int?
    private var handPosition: SIMD3<Float>?
    private var handPositionTime: Double = 0
    private var handWasVisible = false
    private var orientationCalibration = SurrealOrientationCalibration()

    mutating func disconnect() { self = Self() }
    mutating func resetTracking() {
        registration = nil; lastCalibration = 0
        handPosition = nil; handPositionTime = 0; handWasVisible = false
        orientationCalibration = SurrealOrientationCalibration()
    }
    mutating func receivePose(_ data: Data, at now: Double) {
        guard let next = SurrealPose(data) else { return }
        receivePose(next, at: now)
    }
    /// True when the packet reset the clock (and so the registration).
    @discardableResult
    mutating func receivePose(_ decoded: SurrealPose, at now: Double) -> Bool {
        var next = decoded
        let stamp = clock.stamp(next.ticks, arrival: now)
        // Keep `input`: buttons are notified only on change, so a grip held through
        // the reset sends nothing new and would read as released until it moved.
        if stamp.reset { resetTracking() }
        next.time = stamp.time
        pose = next; receivedAt = now
        return stamp.reset
    }
    mutating func sample(reference: simd_float4x4?, at time: Double, now: Double,
                         settings: SurrealInputSettings,
                         aimOffset: simd_quatf = simd_quatf(angle: 0, axis: [0, 1, 0]),
                         alignmentTarget: simd_quatf? = nil) -> KleptonHandState {
        var released = KleptonHandState()
        released.position = [0, -10, 0]; released.orientation = simd_quatf(angle: 0, axis: [0, 1, 0])
        guard connected else { return released }
        // Hand positioning does not depend on the native positional tracker's
        // confidence, but still requires fresh packets with a valid quaternion.
        guard let pose, now - receivedAt < 0.25,
              settings.positionFromHands || pose.confidence > 0 else {
            // Released while lost, but the snapshot stays: input is notified only on
            // change, so clearing it lost a grip held through a short pose gap for good.
            // Disconnect still clears it.
            status = "Tracking lost — inputs released"
            resetTracking()
            return released
        }
        let body = pose.predicted(at: time)
        let bodyFromGrip = settings.positionFromHands ? matrix_identity_float4x4 : settings.bodyFromGrip
        var frameLinear = SIMD3<Float>.zero
        var frameAngular = SIMD3<Float>.zero
        if settings.positionFromHands {
            // Calibrate once in this mode. Holding/repressing grip during play
            // must not let the hand tracker alter gyro rotation or its velocity.
            if registration == nil {
                guard reference != nil, let alignmentTarget, hasInput, input.handTrigger > 0.55,
                      simd_length(pose.angular) < 0.35 else {
                    orientationCalibration = SurrealOrientationCalibration()
                    status = "Look forward, point both grips forward with sticks up; hold grip still for half a second"
                    return released
                }
                // The target is explicit forward alignment, never wrist rotation.
                let candidate = (alignmentTarget * simd_quatf(body * bodyFromGrip).inverse).normalized
                guard let aligned = orientationCalibration.sample(candidate, at: time) else {
                    status = "Calibrating rotation — keep still and hold grip"
                    return released
                }
                registration = simd_float4x4(aligned)
            }
        } else if let reference, hasInput, input.handTrigger > 0.15,
           simd_length(pose.linear) < 0.35,
           simd_length(pose.angular) < 1.5 {
            // Native positioning retains its existing continuous registration.
            let deviceGrip = body * settings.bodyFromGrip
            let candidate = reference * simd_inverse(deviceGrip)
            if let old = registration {
                let dt = max(0, min(0.1, time - lastCalibration))
                let alpha = Float(1 - exp(-dt / 0.18))
                var filtered = simd_float4x4(simd_slerp(simd_quatf(old), simd_quatf(candidate), alpha))
                filtered.columns.3 = old.columns.3 + (candidate.columns.3 - old.columns.3) * alpha
                if dt > 0.001, dt < 0.1 {
                    frameLinear = SIMD3<Float>(filtered.columns.3.x - old.columns.3.x,
                                               filtered.columns.3.y - old.columns.3.y,
                                               filtered.columns.3.z - old.columns.3.z) / Float(dt)
                    var delta = simd_quatf(filtered) * simd_quatf(old).inverse
                    if delta.real < 0 { delta = simd_quatf(vector: -delta.vector) }
                    if simd_length(delta.imag) > 0.00001 { frameAngular = delta.axis * delta.angle / Float(dt) }
                }
                registration = filtered
            } else { registration = candidate }
            lastCalibration = time
        }
        guard let registration else {
            status = "Hold still, hands visible, and squeeze grip to calibrate"
            return released
        }
        status = reference == nil ? "Tracking — hand out of view" : "Tracking"
        let world = registration * body * bodyFromGrip * simd_float4x4(aimOffset)
        var result = hasInput ? input : KleptonHandState()
        result.fromController = true
        result.position = SIMD3(world.columns.3.x, world.columns.3.y, world.columns.3.z)
        result.orientation = simd_quatf(world).normalized
        let rotation = simd_quatf(registration)
        let localPosition = SIMD3<Float>(body.columns.3.x, body.columns.3.y, body.columns.3.z)
        result.linearVelocity = rotation.act(pose.linear) + frameLinear + simd_cross(frameAngular, rotation.act(localPosition))
        result.angularVelocity = rotation.act(pose.angular) + frameAngular
        if settings.positionFromHands {
            // The compositor has already predicted this grip to presentationTime.
            // Never add native translation or predict the hand a second time.
            result.linearVelocity = .zero
            if let reference {
                let position = SIMD3(reference.columns.3.x, reference.columns.3.y, reference.columns.3.z)
                let dt = time - handPositionTime
                if handWasVisible, let previous = handPosition, dt > 0.001, dt < 0.1 {
                    result.linearVelocity = (position - previous) / Float(dt)
                }
                handPosition = position; handPositionTime = time; handWasVisible = true
                result.position = position
                status = "Tracking — position from hands"
            } else {
                handWasVisible = false
                guard let handPosition, time >= handPositionTime, time - handPositionTime <= 0.15 else {
                    status = "Hand tracking lost — inputs released"
                    return released
                }
                result.position = handPosition
                status = "Hand out of view — holding position briefly"
            }
        }
        return result
    }
}
