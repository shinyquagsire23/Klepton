import Foundation
import simd

@main
struct SurrealTests {
    static var checks = 0
    static let forward = simd_quatf(angle: 0, axis: [0, 1, 0])
    static func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1; precondition(condition(), message)
    }
    static func near(_ a: Float, _ b: Float, _ message: String) { expect(abs(a - b) < 0.0001, message) }
    static func buttonPacket(flags: UInt8 = 0, trigger: UInt8 = 0, grip: UInt8 = 0,
                             x: UInt8 = 128, y: UInt8 = 128) -> Data {
        Data([UInt8](repeating: 0, count: 8) + [flags, trigger, grip, x, y])
    }
    static func posePacket(ticks: UInt64 = 1_000_000_000, position: SIMD3<Float> = .zero,
                           q: SIMD4<Float> = [0, 0, 0, 1], confidence: Float = 1,
                           velocity: SIMD3<Float> = .zero, angular: SIMD3<Float> = .zero) -> Data {
        var result = Data()
        for i in 0..<8 { result.append(UInt8(truncatingIfNeeded: ticks >> (8 * i))) }
        for f in [position.x, position.y, position.z, q.x, q.y, q.z, q.w, confidence,
                  velocity.x, velocity.y, velocity.z, angular.x, angular.y, angular.z, 0, 0, 0] {
            for i in 0..<4 { result.append(UInt8(truncatingIfNeeded: f.bitPattern >> (8 * i))) }
        }
        return result
    }
    static func main() {
        let suite = "surreal-tests-\(UUID())", defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        expect(KleptonControllerMode.selected(defaults: defaults, bundleDefault: nil) == .standard, "Original builds default to Standard")
        expect(KleptonControllerMode.selected(defaults: defaults, bundleDefault: "surreal") == .surreal, "Mac default")
        KleptonControllerMode.standard.save(defaults: defaults)
        expect(KleptonControllerMode.selected(defaults: UserDefaults(suiteName: suite)!, bundleDefault: "surreal") == .standard, "Local preference survives relaunch and overrides updated bundle")
        KleptonControllerMode.surreal.save(defaults: defaults)
        expect(KleptonControllerMode.selected(defaults: defaults, bundleDefault: "standard") == .surreal, "Switch between launches")
        defaults.set("bad", forKey: KleptonControllerMode.preferenceKey)
        expect(KleptonControllerMode.selected(defaults: defaults, bundleDefault: "bad") == .standard, "Invalid settings fail safe")
        expect(KleptonControllerMode.surrealAvailable(nil) && KleptonControllerMode.surrealAvailable("YES")
               && !KleptonControllerMode.surrealAvailable("NO"), "Surreal Touch is there unless the build says NO")
        KleptonControllerMode.surreal.save(defaults: defaults)
        expect(KleptonControllerMode.selected(defaults: defaults, bundleDefault: "surreal", available: false) == .standard,
               "Without Surreal Touch support every build is Standard")
        expect(defaults.string(forKey: KleptonControllerMode.preferenceKey) == "standard",
               "A stored Surreal choice is put back to Standard where Surreal Touch is not available")
        expect(SurrealHand(name: "Surreal Touch L D5A8") == .left, "Left name with suffix")
        expect(SurrealHand(name: "surreal touch right 123") == .right, "Right identity")
        expect(SurrealHand(name: "Surreal Touch L R") == nil, "Ambiguous hands rejected")
        expect(SurrealHand(name: "Gamepad R") == nil, "Other devices rejected")
        let settings = SurrealInputSettings()
        expect(!settings.positionFromHands && !SurrealInputSettings.stored(defaults: defaults).positionFromHands,
               "Hand positioning is opt-in for fresh and existing installations")
        defaults.set(true, forKey: "klepton.surreal.positionFromHands")
        expect(SurrealInputSettings.stored(defaults: UserDefaults(suiteName: suite)!).positionFromHands,
               "Hand positioning survives relaunch")
        defaults.set(false, forKey: "klepton.surreal.positionFromHands")
        expect(!SurrealInputSettings.stored(defaults: defaults).positionFromHands, "Native positioning can be restored")
        for hand in SurrealHand.allCases {
            let st = SurrealWire.buttons(buttonPacket(flags: 15, trigger: 127, grip: 255), hand: hand, settings: settings)!
            // Literal Oculus RAW masks, deliberately independent of the implementation constants.
            expect(st.buttons == (hand == .left ? 0x30100700 : 0x0C200007), "Oculus raw masks for faces, menu, stick, trigger, grip")
            expect(st.touches == (hand == .left ? 0x700 : 0x7), "Pressed faces and stick are touched; menu and grip are not")
            let idle = SurrealWire.buttons(buttonPacket(trigger: 127, grip: 255), hand: hand, settings: settings)!
            expect(idle.touches == 0, "No invented touch without a press or a push")
            let pushed = SurrealWire.buttons(buttonPacket(y: 255), hand: hand, settings: settings)!
            expect(pushed.touches == (hand == .left ? 0x400 : 0x4), "A pushed stick is touched (Alyx teleport: dpad sub_mode touch)")
            let resting = SurrealWire.buttons(buttonPacket(x: 133, y: 124), hand: hand, settings: settings)!
            expect(resting.touches == 0, "A stick inside the dead zone is not touched")
            near(st.indexTrigger, 1, "Trigger full scale"); near(st.handTrigger, 1, "Grip clamps")
        }
        let half = SurrealWire.buttons(buttonPacket(trigger: 64, grip: 32), hand: .left, settings: settings)!
        near(half.indexTrigger, 64/127, "Trigger fractional value"); near(half.handTrigger, 32/127, "Grip analog value")
        expect(half.buttons == 0, "Digital threshold separate from analog")
        near(SurrealWire.buttons(buttonPacket(trigger: 127), hand: .right, settings: .init(analogScale: 255))!.indexTrigger, 127/255, "Alternate firmware scale")
        expect(SurrealWire.stick(128, 128, settings: settings) == .zero, "Centered")
        expect(SurrealWire.stick(133, 124, settings: settings) == .zero, "Dead zone")
        near(SurrealWire.stick(255, 128, settings: settings).x, 1, "Right")
        near(SurrealWire.stick(0, 128, settings: settings).x, -1, "Left")
        near(SurrealWire.stick(128, 255, settings: .init(invertY: true)).y, -1, "Y polarity override")
        near(simd_length(SurrealWire.stick(255, 255, settings: settings)), 1, "Radial diagonal limit")
        expect(SurrealWire.buttons(Data(count: 12), hand: .left, settings: settings) == nil, "Truncated button packet")
        expect(SurrealPose(Data(count: 75)) == nil, "Truncated pose")
        expect(SurrealPose(posePacket(q: .zero)) == nil, "Degenerate quaternion")
        expect(SurrealPose(posePacket(position: [.nan, 0, 0])) == nil, "Nonfinite pose")
        var pose = SurrealPose(posePacket(position: [1, 2, 3], q: [0.5, 0.5, 0.5, 0.5], velocity: [1, 2, 3], angular: [4, 5, 6]))!
        expect(pose.transform.columns.3 == [1, 2, -3, 1], "Polar vector reflection")
        expect(pose.linear == [1, 2, -3] && pose.angular == [-4, -5, 6], "Linear vs axial reflection")
        let reflection = simd_float3x3(diagonal: [1, 1, -1])
        let expected = reflection * simd_float3x3(simd_quatf(vector: [0.5, 0.5, 0.5, 0.5])) * reflection
        let actual = simd_float3x3(simd_quatf(pose.transform))
        expect(simd_length(actual.columns.0 - expected.columns.0) < 0.0001, "Quaternion basis conversion")
        pose.time = 10; pose.angular = .zero
        near(pose.predicted(at: 20).columns.3.x, 1.035, "Prediction bounded at 35ms")
        near(pose.predicted(at: 9).columns.3.x, 1, "Never predict backwards")
        var clock = SurrealClock()
        near(Float(clock.stamp(1_000_000_000, arrival: 10).time), 10, "Clock offset")
        near(Float(clock.stamp(1_010_000_000, arrival: 10.03).time), 10.010002, "Delivery jitter not baked into pose time")
        expect(clock.stamp(1, arrival: 11).reset, "Device reboot resets epoch")
        var cache = SurrealHandCache()
        cache.connected = true
        cache.input = SurrealWire.buttons(buttonPacket(flags: 1, trigger: 127, grip: 127), hand: .left, settings: settings)!
        cache.hasInput = true
        cache.receivePose(posePacket(), at: 10)
        var reference = matrix_identity_float4x4; reference.columns.3 = [1, 2, 3, 1]
        let calibrated = cache.sample(reference: reference, at: 10, now: 10, settings: settings, alignmentTarget: forward)
        expect(calibrated.position == [1, 2, 3], "Grip calibrates into world position")
        expect(calibrated.buttons & 0x100 != 0, "Mapped input reaches calibrated state")
        cache.receivePose(posePacket(ticks: 1_010_000_000, position: [0.1, 0, 0]), at: 10.01)
        let coasting = cache.sample(reference: nil, at: 10.01, now: 10.01, settings: settings, alignmentTarget: forward)
        near(coasting.position!.x, 1.1, "Native 6DoF continues without visible hands")
        let stale = cache.sample(reference: nil, at: 11, now: 11, settings: settings, alignmentTarget: forward)
        expect(stale.buttons == 0 && stale.indexTrigger == 0 && stale.handTrigger == 0 && stale.stick == .zero, "Lost tracking releases every input")
        expect(cache.registration == nil, "Stale tracking requires calibration")
        // Grip still held, no new input packet: the stream resumes and the grip is still down.
        cache.receivePose(posePacket(ticks: 2_000_000_000), at: 11.0)
        let resumed = cache.sample(reference: reference, at: 11.0, now: 11.0, settings: settings, alignmentTarget: forward)
        expect(resumed.buttons & 0x100 != 0 && resumed.handTrigger > 0.55,
               "A button held through a pose gap is still held when the stream resumes")
        cache.disconnect()
        let disconnected = cache.sample(reference: reference, at: 12, now: 12, settings: settings, alignmentTarget: forward)
        expect(disconnected.buttons == 0 && disconnected.position == [0, -10, 0], "Disconnect parks unused hand; never injects hand pinch")
        cache.connected = true; cache.receivePose(posePacket(), at: 12)
        expect(cache.sample(reference: reference, at: 12, now: 12, settings: settings, alignmentTarget: forward).buttons == 0, "Reconnect cannot resurrect held buttons")
        expect(cache.registration == nil, "Reconnect needs a new grip and input snapshot")
        let pulse = Array(SurrealWire.vibration(sequence: 0x1234, amplitude: 0.5, duration: 0.1))
        expect(pulse == [0x18, 10, 0, 0x34, 0x12, 0x88, 0x13, 170, 0, 100, 0, 0, 0], "Haptic wire format and units")
        let badPulse = SurrealWire.vibration(sequence: 0, amplitude: .nan, duration: .infinity)
        expect(badPulse.count == 13, "Invalid haptic values do not trap")
        testResetGesture()
        testPoseSmoother()
        expect(Array(SurrealWire.tlv(type: SurrealWire.modeRequest, sequence: 0)) == [0x0D, 2, 0, 0, 0], "Mode request wire format")
        expect(Array(SurrealWire.tlv(type: SurrealWire.modeSet, sequence: 0x0201, data: [SurrealWire.controllerMode]))
               == [0x0C, 3, 0, 0x01, 0x02, 0x00], "Set controller mode wire format")
        let reply = SurrealWire.parseTLV(Data([0x0E, 3, 0, 2, 0, 0x00]))
        expect(reply?.type == SurrealWire.modeSetResponse && reply?.sequence == 2 && reply?.data == [0x00], "Mode reply as seen on hardware")
        expect(SurrealWire.parseTLV(Data([0x0E, 3, 0, 2, 0])) == nil, "Truncated control reply")
        testHandPosition()
        testStableCalibrationWindow()
        testRotationCalibrationStaysFixed()
        testAimOffsets()
        testSensorOnlyOrientation()
        print("Surreal controller tests: \(checks) checks passed")
    }

    static func testHandPosition() {
        let settings = SurrealInputSettings(positionFromHands: true)
        var cache = SurrealHandCache()
        cache.connected = true
        cache.input = SurrealWire.buttons(buttonPacket(flags: 1, trigger: 127, grip: 127), hand: .left, settings: settings)!
        cache.hasInput = true
        // The device's native positional tracker is unavailable and drifts wildly.
        cache.receivePose(posePacket(position: [90, 80, 70], confidence: 0, velocity: [99, 0, 0]), at: 19.5)
        let unseen = cache.sample(reference: nil, at: 19.5, now: 19.5, settings: settings, alignmentTarget: forward)
        expect(unseen.position == [0, -10, 0] && unseen.buttons == 0, "A hand must be seen before hybrid input activates")
        var reference = matrix_identity_float4x4
        reference.columns.3 = [1, 2, 3, 1]
        for frame in 0..<40 {
            let time = 19.6 + Double(frame) * 0.01
            cache.receivePose(posePacket(ticks: 1_100_000_000 + UInt64(frame) * 10_000_000,
                                        position: [90, 80, 70], confidence: 0, velocity: [99, 0, 0]), at: time)
            _ = cache.sample(reference: reference, at: time, now: time, settings: settings, alignmentTarget: forward)
        }
        let initial = cache.sample(reference: reference, at: 20, now: 20, settings: settings, alignmentTarget: forward)
        expect(initial.position == [1, 2, 3], "Hand positioning works without native position confidence or velocity")
        expect(initial.buttons == cache.input.buttons && initial.indexTrigger == 1, "Physical buttons survive hand positioning")
        expect(initial.linearVelocity == .zero, "Initial acquisition cannot produce a velocity spike")

        cache.input = SurrealWire.buttons(buttonPacket(flags: 1, trigger: 127, x: 255), hand: .left, settings: settings)!
        let nativeRotation = simd_quatf(angle: .pi / 2, axis: [0, 0, 1])
        cache.receivePose(posePacket(ticks: 1_510_000_000, position: [-90, -80, -70],
                                    q: nativeRotation.vector, confidence: 0, velocity: [-99, 0, 0], angular: [0, 0, 0.4]), at: 20.01)
        reference = simd_float4x4(simd_quatf(angle: .pi / 3, axis: [1, 0, 0]))
        reference.columns.3 = [1.02, 2, 3, 1]
        let moved = cache.sample(reference: reference, at: 20.01, now: 20.01, settings: settings, alignmentTarget: forward)
        expect(moved.position == [1.02, 2, 3], "Position follows the hand despite opposing native motion")
        near(moved.linearVelocity.x, 2, "Linear velocity follows hand movement")
        near(abs(simd_dot(moved.orientation!.vector, nativeRotation.vector)), 1, "Rotation remains controller-driven after calibration")
        near(moved.angularVelocity.z, 0.4, "Angular velocity remains controller-driven")
        expect(moved.buttons == cache.input.buttons && moved.stick.x == 1, "Hand rotation never synthesizes or replaces buttons/sticks")

        let held = cache.sample(reference: nil, at: 20.02, now: 20.02, settings: settings, alignmentTarget: forward)
        expect(held.position == moved.position && held.linearVelocity == .zero, "Brief occlusion freezes position without native drift")
        let lost = cache.sample(reference: nil, at: 20.17, now: 20.17, settings: settings, alignmentTarget: forward)
        expect(lost.position == [0, -10, 0] && lost.buttons == 0 && lost.indexTrigger == 0 && lost.stick == .zero,
               "Sustained hand loss parks and releases inputs while BLE remains fresh")
        expect(cache.registration != nil, "Hand occlusion alone retains controller orientation alignment")
        // A physical release while hidden must not be resurrected on reacquisition.
        cache.input = SurrealWire.buttons(buttonPacket(), hand: .left, settings: settings)!
        reference.columns.3 = [5, 6, 7, 1]
        let returned = cache.sample(reference: reference, at: 20.18, now: 20.18, settings: settings, alignmentTarget: forward)
        expect(returned.position == [5, 6, 7] && returned.linearVelocity == .zero, "Reacquisition resumes without a throw velocity spike")
        expect(returned.buttons == 0 && returned.indexTrigger == 0, "Reacquisition uses the latest physical button snapshot")

        cache.resetTracking()
        expect(cache.sample(reference: nil, at: 20.19, now: 20.19, settings: settings, alignmentTarget: forward).position == [0, -10, 0],
               "Settings changes/recalibration discard the held hand position")
        cache.input.handTrigger = 1
        _ = cache.sample(reference: reference, at: 20.20, now: 20.20, settings: settings, alignmentTarget: forward)
        let stale = cache.sample(reference: reference, at: 20.4, now: 20.4, settings: settings, alignmentTarget: forward)
        expect(stale.position == [0, -10, 0] && stale.buttons == 0 && stale.handTrigger == 0,
               "A visible hand cannot keep a stale BLE controller active")
        expect(cache.registration == nil, "BLE timeout clears hybrid calibration")
        expect(cache.hasInput && cache.input.handTrigger == 1,
               "BLE timeout keeps the button snapshot; input is only notified on change")
        cache.disconnect()
        expect(cache.sample(reference: reference, at: 20.5, now: 20.5, settings: settings, alignmentTarget: forward).position == [0, -10, 0],
               "A visible hand cannot replace a disconnected controller")

        cache.connected = true; cache.input.handTrigger = 1; cache.hasInput = true
        cache.receivePose(posePacket(confidence: 0), at: 21)
        expect(cache.sample(reference: reference, at: 21, now: 21, settings: .init()).position == [0, -10, 0],
               "Native mode still rejects low-confidence device tracking")
    }

    static func testStableCalibrationWindow() {
        var calibration = SurrealOrientationCalibration()
        let identity = simd_quatf(angle: 0, axis: [0, 1, 0])
        expect(calibration.sample(identity, at: 0) == nil, "Calibration cannot latch a single noisy frame")
        var aligned: simd_quatf?
        for frame in 1...36 {
            let noise = simd_quatf(angle: (frame % 2 == 0 ? 1 : -1) * Float.pi / 180, axis: [1, 0, 0])
            // q and -q describe the same orientation; neither should restart the window.
            aligned = calibration.sample(simd_quatf(vector: frame % 2 == 0 ? -noise.vector : noise.vector), at: Double(frame) * 0.01)
            if aligned != nil { break }
        }
        expect(aligned != nil && abs(aligned!.angle) < 0.1 * .pi / 180, "Stable window averages small hand jitter across quaternion signs")
        calibration = SurrealOrientationCalibration()
        _ = calibration.sample(identity, at: 1)
        _ = calibration.sample(identity, at: 1.05)
        expect(calibration.sample(identity, at: 1.4) == nil, "Missing calibration frames cannot complete a hold")
        expect(calibration.sample(simd_quatf(angle: 0.3, axis: [0, 1, 0]), at: 1.45) == nil,
               "A large hand-reference change restarts calibration")

        var cache = SurrealHandCache()
        cache.connected = true; cache.hasInput = true; cache.input.handTrigger = 1
        let settings = SurrealInputSettings(positionFromHands: true)
        func feed(_ time: Double, reference: simd_float4x4? = matrix_identity_float4x4,
                  angular: SIMD3<Float> = .zero) {
            cache.receivePose(posePacket(ticks: UInt64(time * 1e9), angular: angular), at: time)
            _ = cache.sample(reference: reference, at: time, now: time, settings: settings, alignmentTarget: forward)
        }
        for frame in 0...20 { feed(Double(frame) * 0.01) }
        expect(cache.registration == nil, "An incomplete grip hold stays inactive")
        cache.input.handTrigger = 0; feed(0.21)
        cache.input.handTrigger = 1
        for frame in 22...45 { feed(Double(frame) * 0.01) }
        expect(cache.registration == nil, "Releasing grip restarts the stable hold")
        feed(0.46, reference: nil)
        for frame in 47...70 { feed(Double(frame) * 0.01) }
        expect(cache.registration == nil, "Losing the hand restarts the stable hold")
        feed(0.71, angular: [1, 0, 0])
        for frame in 72...95 { feed(Double(frame) * 0.01) }
        expect(cache.registration == nil, "Moving the controller restarts the stable hold")
        for frame in 96...112 { feed(Double(frame) * 0.01) }
        expect(cache.registration != nil, "A fresh uninterrupted stable hold completes calibration")
    }

    static func testRotationCalibrationStaysFixed() {
        let settings = SurrealInputSettings(positionFromHands: true)
        var cache = SurrealHandCache()
        cache.connected = true; cache.hasInput = true; cache.input.handTrigger = 1
        let alignment = simd_quatf(angle: 0.4, axis: [0, 1, 0])
        var reference = simd_float4x4(alignment)
        reference.columns.3 = [1, 2, 3, 1]
        // Give either calibration implementation a stable initial reference.
        for frame in 0...40 {
            let time = 30 + Double(frame) * 0.01
            cache.receivePose(posePacket(ticks: UInt64(frame) * 10_000_000), at: time)
            _ = cache.sample(reference: reference, at: time, now: time, settings: settings, alignmentTarget: alignment)
        }
        expect(cache.registration != nil, "Stable visible grip establishes rotation calibration")
        // A hand tracker estimate can change while a controller is gripped.
        // It must not pull on the controller's orientation after calibration.
        for (index, axis) in [SIMD3<Float>(1, 0, 0), [0, 1, 0], [0, 0, 1]].enumerated() {
            let deviceRotation = simd_quatf(angle: 0.2, axis: axis)
            let expected = alignment * deviceRotation
            let noisyHand = simd_quatf(angle: 0.3, axis: [0, 0, 1]) * expected
            reference = simd_float4x4(noisyHand)
            reference.columns.3 = [1.1, 2, 3, 1]
            let time = 30.41 + Double(index) * 0.01
            let wireQuaternion = SIMD4(-deviceRotation.imag.x, -deviceRotation.imag.y,
                                       deviceRotation.imag.z, deviceRotation.real)
            cache.receivePose(posePacket(ticks: 410_000_000 + UInt64(index) * 10_000_000,
                                        q: wireQuaternion, angular: [-axis.x * 0.2, -axis.y * 0.2, axis.z * 0.2]), at: time)
            let result = cache.sample(reference: reference, at: time, now: time, settings: settings, alignmentTarget: alignment)
            near(abs(simd_dot(result.orientation!.vector, expected.vector)), 1,
                 "Holding grip cannot recalibrate pitch/yaw/roll during play")
            expect(simd_length(result.angularVelocity - alignment.act(axis * 0.2)) < 0.0001,
                   "Hand reference jitter cannot add angular velocity")
            expect(result.position == [1.1, 2, 3] && result.handTrigger == 1,
                   "Fixed rotation calibration preserves live hand position and physical grip")
        }
    }

    static func testPoseSmoother() {
        // A still hand with ±3 mm / ±1° of tracker noise at 90 Hz: the output must shake far less.
        var s = KleptonPoseSmoother()
        var rng = SystemRandomNumberGenerator()
        var rawSpread: Float = 0, outSpread: Float = 0, rawTurn: Float = 0, outTurn: Float = 0
        var lastRaw = SIMD3<Float>(0, 1, 0), lastOut = lastRaw
        var lastRawQ = simd_quatf(angle: 0, axis: [0, 1, 0]), lastOutQ = lastRawQ
        for i in 0..<360 {
            let n = SIMD3<Float>(Float.random(in: -0.003...0.003, using: &rng), Float.random(in: -0.003...0.003, using: &rng),
                                 Float.random(in: -0.003...0.003, using: &rng))
            let rq = simd_quatf(angle: Float.random(in: -0.017...0.017, using: &rng), axis: [0, 1, 0])
            let (p, q) = s.filter(position: SIMD3(0, 1, 0) + n, orientation: rq, at: Double(i) / 90)
            if i > 30 {
                rawSpread += simd_length(SIMD3(0, 1, 0) + n - lastRaw); outSpread += simd_length(p - lastOut)
                rawTurn += abs((rq * lastRawQ.inverse).angle); outTurn += abs((q * lastOutQ.inverse).angle)
            }
            lastRaw = SIMD3(0, 1, 0) + n; lastOut = p; lastRawQ = rq; lastOutQ = q
        }
        expect(outSpread < rawSpread * 0.25, "Still-hand position noise is mostly smoothed away")
        expect(outTurn < rawTurn * 0.35, "Still-hand rotation noise is mostly smoothed away")
        // A 1.5 m/s sweep: the output keeps up within a few centimetres.
        var f = KleptonPoseSmoother()
        var out = SIMD3<Float>.zero
        for i in 0..<45 {
            out = f.filter(position: SIMD3(Float(i) * 1.5 / 90, 1, 0), orientation: simd_quatf(angle: 0, axis: [0, 1, 0]), at: Double(i) / 90).0
        }
        expect(abs(out.x - 44 * 1.5 / 90) < 0.04, "Fast motion follows with little lag")
        // A gap (hand lost) starts again from the new sample instead of sliding to it.
        _ = f.filter(position: [0, 1, 0], orientation: simd_quatf(angle: 0, axis: [0, 1, 0]), at: 10)
        expect(f.filter(position: [0.5, 1, 0], orientation: simd_quatf(angle: 0, axis: [0, 1, 0]), at: 10.5).0.x == 0.5,
               "After a gap the filter restarts at the sample")
    }


    static func testResetGesture() {
        // One squeeze = press then release; hands alternate 0.1 s apart.
        func squeeze(_ g: inout SurrealResetGesture, _ hand: Int, at t: Double) -> Bool {
            let fired = g.update(hand: hand, grip: 0.9, at: t)
            _ = g.update(hand: hand, grip: 0.1, at: t + 0.05)
            return fired
        }
        var g = SurrealResetGesture()
        var fired = 0
        for i in 0..<5 { for h in 0...1 { if squeeze(&g, h, at: Double(i) * 0.4 + Double(h) * 0.1) { fired += 1 } } }
        expect(fired == 1, "Five squeezes on both controllers reset once")
        var held = SurrealResetGesture()
        var heldFired = false
        for i in 0..<20 { heldFired = heldFired || held.update(hand: 0, grip: 0.9, at: Double(i) * 0.05)
                                   || held.update(hand: 1, grip: 0.9, at: Double(i) * 0.05) }
        expect(!heldFired, "A held grip is one squeeze, not five")
        var oneHand = SurrealResetGesture()
        var oneFired = false
        for i in 0..<10 { oneFired = squeeze(&oneHand, 0, at: Double(i) * 0.2) || oneFired }
        expect(!oneFired, "One controller alone does not reset")
        var slow = SurrealResetGesture()
        var slowFired = false
        for i in 0..<5 { for h in 0...1 { slowFired = squeeze(&slow, h, at: Double(i) * 1.0 + Double(h) * 0.1) || slowFired } }
        expect(!slowFired, "Squeezes spread over more than three seconds do not reset")
        var light = SurrealResetGesture()
        var lightFired = false
        for i in 0..<5 { for h in 0...1 { lightFired = light.update(hand: h, grip: 0.4, at: Double(i) * 0.4) || lightFired
                                          _ = light.update(hand: h, grip: 0.1, at: Double(i) * 0.4 + 0.05) } }
        expect(!lightFired, "Light squeezes below the press threshold do not count")
    }

    static func testAimOffsets() {
        let suite = "surreal-aim-\(UUID())", defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        expect(SurrealAimOffset.stored(hand: 0, defaults: defaults).degrees == .zero, "Aim adjustment defaults to no offset")
        let offset = SurrealAimOffset(degrees: [15, -20, 5])
        offset.save(hand: 0, defaults: defaults)
        expect(SurrealAimOffset.stored(hand: 0, defaults: UserDefaults(suiteName: suite)!) == offset, "Manual angles persist")
        expect(SurrealAimOffset.stored(hand: 1, defaults: defaults).degrees == .zero, "Each hand has independent angles")
        defaults.set([1000.0, -1000.0, 1.0], forKey: SurrealAimOffset.key(1))
        expect(SurrealAimOffset.stored(hand: 1, defaults: defaults).degrees == [90, -90, 1], "Loaded angles are bounded")
        defaults.set([1.0, 2.0], forKey: SurrealAimOffset.key(1))
        expect(SurrealAimOffset.stored(hand: 1, defaults: defaults).degrees == .zero, "Malformed angles reset safely")
        expect(SurrealInputSettings.stored(defaults: defaults).aimOffsets[0] == offset, "Stored aim reaches runtime settings")
        var cache = SurrealHandCache()
        cache.connected = true; cache.hasInput = true; cache.input.handTrigger = 1
        let settings = SurrealInputSettings(positionFromHands: true)
        var hand = matrix_identity_float4x4; hand.columns.3 = [1, 2, 3, 1]
        for frame in 0...40 {
            let time = 50 + Double(frame) * 0.01
            cache.receivePose(posePacket(ticks: UInt64(time * 1e9)), at: time)
            _ = cache.sample(reference: hand, at: time, now: time, settings: settings, aimOffset: offset.rotation, alignmentTarget: forward)
        }
        let before = cache.registration
        let output = cache.sample(reference: hand, at: 50.41, now: 50.41, settings: settings, aimOffset: offset.rotation)
        expect(SurrealControllerOrientation.angle(output.orientation!, offset.rotation) < 0.002,
               "Forward alignment preserves rather than cancels a saved manual offset")
        let changed = cache.sample(reference: hand, at: 50.42, now: 50.42, settings: settings, aimOffset: forward)
        expect(SurrealControllerOrientation.angle(changed.orientation!, forward) < 0.002 && cache.registration == before,
               "Manual reset updates direction immediately without recalibrating")
        expect(changed.position == [1, 2, 3] && changed.handTrigger == 1, "Angle adjustment preserves hand position and physical input")
    }

    static func testSensorOnlyOrientation() {
        let settings = SurrealInputSettings(positionFromHands: true)
        var cache = SurrealHandCache()
        cache.connected = true; cache.hasInput = true; cache.input.handTrigger = 1
        var hand = matrix_identity_float4x4
        hand.columns.3 = [1, 2, 3, 1]
        for frame in 0...40 {
            let time = 60 + Double(frame) * 0.01
            cache.receivePose(posePacket(ticks: UInt64(time * 1e9)), at: time)
            _ = cache.sample(reference: hand, at: time, now: time, settings: settings)
        }
        expect(cache.registration == nil, "A visible wrist cannot establish orientation without an explicit forward target")
        let desired = simd_quatf(angle: 0.5, axis: [0, 1, 0])
        for frame in 41...81 {
            let time = 60 + Double(frame) * 0.01
            hand = simd_float4x4(simd_quatf(angle: Float(frame) * 0.4, axis: [1, 0, 0]))
            hand.columns.3 = [1, 2, 3, 1]
            cache.receivePose(posePacket(ticks: UInt64(time * 1e9)), at: time)
            _ = cache.sample(reference: hand, at: time, now: time, settings: settings, alignmentTarget: desired)
        }
        expect(cache.registration != nil, "Wrist rotation cannot interrupt controller-only alignment")
        let time = 60.82
        cache.receivePose(posePacket(ticks: UInt64(time * 1e9)), at: time)
        hand.columns.3 = [3, 4, 5, 1]
        let output = cache.sample(reference: hand, at: time, now: time, settings: settings, alignmentTarget: forward)
        expect(SurrealControllerOrientation.angle(output.orientation!, desired) < 0.002,
               "Changing head direction and wrist rotation cannot steer an aligned stationary controller")
        expect(output.position == [3, 4, 5], "Only hand translation reaches the sensor-oriented controller")

        let head = simd_float4x4(desired * simd_quatf(angle: 0.6, axis: [1, 0, 0]) * simd_quatf(angle: 0.3, axis: [0, 0, 1]))
        expect(SurrealControllerOrientation.angle(SurrealControllerOrientation.forward(head: head)!, desired) < 0.002,
               "Explicit forward is level despite head pitch and roll")
        expect(SurrealControllerOrientation.forward(head: simd_float4x4(simd_quatf(angle: .pi/2, axis: [1, 0, 0]))) == nil,
               "Looking straight up cannot establish an arbitrary forward direction")
        let body = simd_quatf(angle: 0.7, axis: [1, 0, 0])
        let world = simd_quatf(angle: 0.9, axis: [0, 1, 0])
        let baseDevice = simd_quatf(angle: 0.4, axis: [0, 0, 1])
        let ray = SurrealControllerOrientation(device: baseDevice, target: world * baseDevice)
        for axis in [SIMD3<Float>(1, 0, 0), [0, 1, 0], [0, 0, 1]] {
            let device = simd_quatf(angle: 0.8, axis: axis) * baseDevice
            expect(SurrealControllerOrientation.angle(ray.rotation(device: device, offset: body), world * device * body) < 0.002,
                   "Live ray follows only BLE rotation on each axis with a fixed registration")
        }
    }

}
