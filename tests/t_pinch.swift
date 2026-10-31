// Exercise the same filter shipped in the app with timed hand-input sequences.
@main
struct PinchTests {
    static func expect(_ condition: @autoclosure () -> Bool, _ description: String) {
        precondition(condition(), description)
    }

    static func sequence(at hz: Double) {
        var pinch = KleptonPinch()
        var time = 0.0
        var previous: Float = 0
        var presses = 0
        var previousSystem = false
        @discardableResult
        func sample(_ value: Float?, system: Bool = false,
                    distance: Bool = true, enabled: Bool = true) -> Float {
            time += 1 / hz
            let started = system && !previousSystem
            previousSystem = system
            let result = pinch.update(value: value, systemDown: system,
                                      systemStarted: started,
                                      distanceEnabled: distance, enabled: enabled, at: time)
            expect(result == 0 || result == 1, "No partial trigger value may reach the game")
            if previous == 0 && result == 1 { presses += 1 }
            previous = result
            return result
        }
        func hold(_ value: Float?, seconds: Double) {
            for _ in 0..<Int((seconds * hz).rounded(.up)) { sample(value) }
        }

        hold(1, seconds: 0.2)
        expect(presses == 0, "Entering view already pinched must not select anything")
        hold(0, seconds: 0.1)
        for value: Float in [0.1, 0.25, 0.5, 0.54] {
            hold(value, seconds: 0.1)
        }
        expect(presses == 0, "Partial finger closure must not count as a click")
        for _ in 0..<10 { sample(0.8); sample(0.4) }
        expect(presses == 0, "Single-frame fluctuations must not click")
        hold(0.8, seconds: 0.1)
        expect(presses == 1 && previous == 1, "A deliberate pinch must press")
        for _ in 0..<10 { sample(0.45); sample(0.8) }
        expect(presses == 1 && previous == 1, "A held pinch must not repeat")
        pinch.reset() // The controller adapter resets when the whole hand is lost.
        expect(sample(nil) == 0, "Whole-hand loss must release immediately")
        hold(1, seconds: 0.3)
        expect(presses == 1 && previous == 0, "Reappearing while pinched must not click again")
        hold(0, seconds: 0.1)
        hold(1, seconds: 0.1)
        expect(presses == 2 && previous == 1, "Opening then pinching must recover normally")
        expect(sample(0) == 0, "A released pinch must release immediately")
        hold(1, seconds: 0.1)
        expect(presses == 2, "A one-frame opening must not re-arm a second click")
        hold(0, seconds: 0.1)
        hold(1, seconds: 0.1)
        expect(presses == 3, "A full second click must work")
        expect(sample(.nan) == 0, "Invalid tracking must release")
        hold(1, seconds: 0.1)
        expect(presses == 3, "Invalid tracking must require reopening too")

        // Source selection still works; disabling pinches discards held state.
        hold(0, seconds: 0.1)
        for _ in 0..<Int(hz / 10) { sample(1, distance: false) }
        expect(presses == 3, "System-only mode must ignore distance")
        for _ in 0..<Int(hz / 10) { sample(0, system: true, distance: false) }
        expect(presses == 4 && previous == 1, "System events can still drive a pinch")
        expect(sample(0, system: true, enabled: false) == 0, "Disabling input must release")
        hold(1, seconds: 0.1)
        expect(presses == 4, "Re-enabling input mid-pinch must not click")
    }

    static func fingertipGaps() {
        var pinch = KleptonPinch()
        func at(_ time: Double, _ value: Float?) -> Float {
            pinch.update(value: value, systemDown: false, distanceEnabled: true,
                         enabled: true, at: time)
        }
        _ = at(0, 0); _ = at(0.03, 0); _ = at(0.06, 0)
        _ = at(0.07, 1); _ = at(0.09, 1) // 20 ms of measured closure
        expect(at(0.11, nil) == 0, "Missing tips cannot start a press")
        expect(at(0.13, nil) == 0, "Invisible time cannot complete closure")
        expect(at(0.14, 1) == 0, "Returning tips must resume, not skip, debounce")
        expect(at(0.17, 1) == 1, "A brief gap must not discard a deliberate pinch")
        expect(at(0.19, nil) == 1, "A held pinch survives a brief tip occlusion")
        expect(at(0.22, 1) == 1, "Returning closed must continue the same press")
        expect(at(0.23, 0) == 0, "Observed opening still releases immediately")
        _ = at(0.25, 0)
        _ = at(0.27, nil); _ = at(0.29, nil)
        _ = at(0.30, 0) // Only 20 ms of measured opening so far
        expect(at(0.31, 1) == 0, "Invisible time cannot re-arm a click")
        expect(at(0.36, 1) == 0, "A short reopening must not produce repeats")
        _ = at(0.37, 0); _ = at(0.40, 0); _ = at(0.43, 0)
        _ = at(0.44, 1)
        expect(at(0.49, 1) == 1, "Reopening and a fresh pinch must work")
        expect(at(0.58, nil) == 0, "Longer tip loss must release and disarm")
        _ = at(0.59, 1)
        expect(at(0.65, 1) == 0, "A long dropout cannot create a replacement press")
        _ = at(0.66, 0); _ = at(0.69, 0); _ = at(0.72, 0)
        _ = at(0.73, 1)
        expect(at(1.0, 1) == 0, "A stalled frame loop must not finish a pinch")
        _ = at(1.01, 0); _ = at(1.04, 0); _ = at(1.07, 0)
        _ = at(1.08, 1)
        expect(at(1.13, 1) == 1, "Recovery from a stalled loop must work")
        expect(at(1.0, 1) == 0, "A backwards clock must clear held state")
    }

    static func nativePinches() {
        var pinch = KleptonPinch()
        func at(_ time: Double, _ value: Float?, down: Bool = false,
                started: Bool = false, distance: Bool = true, enabled: Bool = true) -> Float {
            pinch.update(value: value, systemDown: down, systemStarted: started,
                         distanceEnabled: distance, enabled: enabled, at: time)
        }
        _ = at(0, nil); _ = at(2, nil)
        expect(at(2.01, nil, down: true, started: true) == 1,
               "A new native pinch works without fingertip tracking or a measured opening")
        expect(at(2.02, nil) == 0, "A short native pinch releases without a second debounce")
        expect(at(2.03, nil, down: true, started: true) == 1, "A new event can select again")
        expect(at(3, nil, down: true) == 1, "An active native gesture does not need fingertip samples")
        pinch.reset() // Whole-hand loss consumes the current event in the adapter.
        expect(at(3.01, nil, down: true) == 0, "An old active event cannot restart after hand loss")
        expect(at(3.02, 1, down: true) == 0, "Closed fingertips cannot restart the old event either")
        expect(at(3.03, 1, down: true, started: true) == 1, "A genuinely new system event can recover")
        expect(at(3.04, 1) == 0, "System release overrides lagging closed fingertips")
        expect(at(3.05, 1) == 0, "Closed geometry must not restart a completed gesture")
        expect(at(3.06, nil, down: true, started: true, distance: false) == 1,
               "System-only mode works without the distance source")
        expect(at(3.07, 1, distance: false) == 0, "System-only release ignores closed fingertips")
        expect(at(3.08, nil, down: true, started: true, enabled: false) == 0,
               "A disabled source cannot select")
        expect(at(3.09, nil, down: true) == 0, "Re-enabling cannot replay a native event")
        expect(at(3.10, .nan, down: true, started: true) == 1,
               "Invalid fingertip geometry cannot veto a fresh trusted system gesture")
        expect(at(3.11, .nan) == 0, "Invalid geometry cannot hold a finished system gesture")
    }

    static func main() {
        for hz in [60.0, 90.0, 100.0] { sequence(at: hz) }
        fingertipGaps()
        nativePinches()
        systemEventLifetimes()
        print("PASS: repeated native gesture lifetimes, reused IDs, unattributed releases, authoritative release, no stale replay, distance debounce, tracking gaps/loss and source selection.")
    }

    static func systemEventLifetimes() {
        var events = KleptonSystemPinches<Int>()
        var pinch = KleptonPinch()
        func sample(_ time: Double) -> Float {
            let event = events.sample(hand: 1, at: time)
            return pinch.update(value: nil, systemDown: event.down,
                                systemStarted: event.started, distanceEnabled: true,
                                enabled: true, at: time)
        }
        // Several complete pinches, including a reused ID and a terminal event
        // whose chirality is absent, must each produce a press and a release.
        for (index, id) in [10, 11, 11, 12].enumerated() {
            let t = Double(index)
            expect(events.receive(id: id, hand: 1, active: true, at: t) == 1,
                   "A new attributed event has an owner")
            expect(sample(t + 0.01) == 1, "Each new system pinch must press")
            _ = events.receive(id: id, hand: nil, active: true, at: t + 0.02)
            expect(sample(t + 0.03) == 1, "A held update must stay held")
            expect(events.receive(id: id, hand: nil, active: false, at: t + 0.04) == 1,
                   "An unattributed release must use the original gesture owner")
            expect(sample(t + 0.05) == 0, "Every completed gesture must release")
        }
        expect(events.receive(id: 99, hand: nil, active: true, at: 4) == nil,
               "Unknown unattributed events must not invent a hand")
        _ = events.receive(id: 20, hand: 1, active: true, at: 4.1)
        expect(sample(4.11) == 1, "Pinches continue after an unknown event")
        _ = events.receive(id: 21, hand: 1, active: true, at: 4.12)
        expect(events.receive(id: 20, hand: nil, active: false, at: 4.13) == nil,
               "A late old release must not clear a newer event")
        expect(sample(4.14) == 1, "Newer event remains held")
        events.discard(hand: 1); pinch.reset()
        _ = events.receive(id: 21, hand: 1, active: true, at: 4.15)
        expect(sample(4.16) == 0, "An active update after tracking loss must not replay")
        _ = events.receive(id: 21, hand: nil, active: false, at: 4.17)
        _ = events.receive(id: 21, hand: 1, active: true, at: 4.18)
        expect(sample(4.19) == 1, "A completed then restarted gesture can recover")
        events.discard(hand: 1); pinch.reset()
        _ = events.receive(id: 22, hand: 1, active: true, at: 5)
        expect(sample(5.3) == 0, "An event missed during a long stall must not replay")
        _ = events.receive(id: 30, hand: 0, active: true, at: 5.4)
        expect(events.sample(hand: 0, at: 5.41).started,
               "Each hand has an independent gesture lifetime")
        _ = events.receive(id: 30, hand: nil, active: false, at: 5.42)
        expect(!events.sample(hand: 0, at: 5.43).down, "Left unattributed release works too")
    }
}
