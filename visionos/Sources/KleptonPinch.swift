// Track the complete system-gesture lifetime independently of frame sampling.
// A terminal event may lack chirality: its ID still identifies the hand that
// started it. Keep that ownership after discarding input on tracking loss, so
// another active update cannot replay the same gesture.
struct KleptonSystemPinches<ID: Equatable> {
    private var ids: [ID?] = [nil, nil]
    private var down = [false, false]
    private var generation: [UInt64] = [0, 0]
    private var consumed: [UInt64] = [0, 0]
    private var startedAt = [0.0, 0.0]

    func owns(id: ID) -> Bool { ids.contains { $0 == id } }

    mutating func receive(id: ID, hand: Int?, active: Bool, at time: Double) -> Int? {
        let owner = ids.firstIndex { $0 == id }
        // Existing ownership is stronger than optional event chirality.
        guard let h = owner ?? hand, (0..<2).contains(h) else { return nil }
        if active {
            guard time.isFinite else { return nil }
            if ids[h] != id {
                ids[h] = id
                down[h] = true
                generation[h] &+= 1
                startedAt[h] = time
            }
        } else {
            // A late release of an older gesture must not end a newer one.
            guard ids[h] == id else { return nil }
            ids[h] = nil
            down[h] = false
        }
        return h
    }

    mutating func discard(hand: Int) {
        down[hand] = false
        consumed[hand] = generation[hand]
    }

    mutating func sample(hand: Int, at time: Double) -> (down: Bool, started: Bool) {
        let age = time - startedAt[hand]
        let fresh = down[hand] && generation[hand] != consumed[hand]
            && age.isFinite && age >= 0 && age < 0.2
        consumed[hand] = generation[hand]
        return (down[hand], fresh)
    }
}

// A discrete trigger for games that interpret ANY nonzero analog value as a
// click. Kept independent of ARKit so recorded input sequences can be tested.
struct KleptonPinch {
    private var pressed = false
    private var armed = false
    private var openDuration = 0.0
    private var closeDuration = 0.0
    private var wasOpen = false
    private var wasClosed = false
    private var lastTime: Double?
    private var lastTrackedTime: Double?
    private var previousTracked = false
    private var systemEngaged = false

    mutating func reset() {
        pressed = false
        armed = false
        openDuration = 0
        closeDuration = 0
        wasOpen = false
        wasClosed = false
        lastTime = nil
        lastTrackedTime = nil
        previousTracked = false
        systemEngaged = false
    }

    // The caller resets immediately for whole-hand loss. A nil value here is
    // a missing fingertip on an otherwise tracked hand: tolerate up to 80 ms,
    // but do not count invisible time toward a new press or re-arm.
    mutating func update(value: Float?, systemDown: Bool, systemStarted: Bool = false,
                         distanceEnabled: Bool,
                         enabled: Bool, at time: Double) -> Float {
        guard enabled, time.isFinite else {
            reset()
            return 0
        }
        if let lastTime, time < lastTime { reset(); return 0 }
        // visionOS already recognized this gesture. Do not debounce it again
        // or require visible fingertips. Only a NEW event may engage it: a
        // stale active event after hand loss must never create another click.
        if systemStarted && systemDown {
            systemEngaged = true
            pressed = true
            armed = false
            openDuration = 0; closeDuration = 0
            wasOpen = false; wasClosed = false
        }
        if systemEngaged {
            if systemDown {
                lastTime = time
                previousTracked = value?.isFinite == true
                if previousTracked { lastTrackedTime = time }
                return 1
            }
            systemEngaged = false
            // The recognizer has ended/cancelled this gesture. Geometry can
            // lag or remain partly closed; it must not keep the button held
            // across the next recognized pinch. Distance input re-arms after
            // a measured reopening, while a fresh native event can press again.
            reset()
            return 0
        }
        guard value?.isFinite != false else { reset(); return 0 }
        if let lastTrackedTime, time - lastTrackedTime > 0.08 { reset() }
        let dt = previousTracked ? max(0, time - (lastTime ?? time)) : 0
        lastTime = time
        guard let value else {
            previousTracked = false
            return pressed ? 1 : 0
        }
        previousTracked = true
        lastTrackedTime = time
        let open = !distanceEnabled || value <= 0.30
        if open {
            pressed = false
            closeDuration = 0
            wasClosed = false
            openDuration = wasOpen ? openDuration + dt : 0
            wasOpen = true
            if openDuration >= 0.05 { armed = true }
            return 0
        }
        openDuration = 0
        wasOpen = false
        if pressed { return 1 }

        // Keep the existing distance thresholds. The important change is that
        // partial values never escape as clicks, and a brief spike cannot fire.
        let closed = distanceEnabled && value > 0.55
        guard armed && closed else {
            closeDuration = 0
            wasClosed = false
            return 0
        }
        closeDuration = wasClosed ? closeDuration + dt : 0
        wasClosed = true
        guard closeDuration >= 0.04 else { return 0 }
        pressed = true
        armed = false
        closeDuration = 0
        wasClosed = false
        return 1
    }
}
