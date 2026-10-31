// Surreal Touch through Surreal Interactive's own visionOS SDK
// (github.com/surreal-interactive/SwiftSDKBinary, Apache-2.0), linked into a
// Surreal build by gen_xcodeproj.py (KL_SURREAL_SDK; see kl_surreal_sdk.h).
import CoreBluetooth
import Foundation
import QuartzCore
import simd

/// The pose source for Surreal mode when Surreal's SDK is linked. KleptonSurrealBackend reads
/// the controllers' own tracking and can align it to the headset only from a grip hold; on
/// hardware that drifted 15–45 cm near the head and 20–30 cm in fast swings, enough to miss
/// Half-Life: Alyx's backpack and to throw sideways. The SDK solves that alignment itself —
/// clock sync, latency, the controller-to-world and hand-to-controller transforms, whether
/// the controller is held — with its own ARKit hand tracking and its own Bluetooth
/// connection, so KleptonSurrealBackend must not be connected while it runs.
final class KleptonSurrealSDK: @unchecked Sendable {
    static let shared = KleptonSurrealSDK()

    /// Whether this app was built with the SDK — decided when the game is built for Surreal
    /// Touch (gen_xcodeproj.py), never at run time. Only consulted in Surreal mode
    /// (KleptonControllers.usesSurrealSDK).
    static var enabled: Bool {
        #if KL_SURREAL_SDK
        return true
        #else
        return false
        #endif
    }

    private let lock = NSLock()
    private var input = [KleptonHandState(), KleptonHandState()]   // under lock; the SDK's thread writes
    private var settings = SurrealInputSettings()                   // under lock
    private var started = false                                     // under lock
    private var gotButtons = false                                  // under lock
    private var gesture = SurrealResetGesture()                     // under lock; the SDK's thread
    private var toggleRequested = false                             // under lock
    private var paused = false                                      // under lock; true while restarting
    private var clearPolls = false                                  // under lock
    // Render thread only.
    private var pendingPolls: [[Int64]] = [[], []]
    private var wasConnected = [false, false]
    private var nextPulse: [Double] = [0, 0]

    private let logQueue = DispatchQueue(label: "klepton.surreal.sdk.log", qos: .utility)
    private var log: FileHandle?
    private var logBytes = 0

    private init() {}

    /// Connection events and the first buttons only, to Documents/surreal-sdk.log; written on
    /// its own queue, never on the render thread, like KleptonSurrealBackend's surreal-ble.log.
    fileprivate func trace(_ message: String) {
        NSLog("[surreal-sdk] %@", message)
        let stamp = CACurrentMediaTime()
        logQueue.async { [self] in
            guard logBytes < 256 * 1024 else { return }
            if log == nil {
                let file = URL.documentsDirectory.appendingPathComponent("surreal-sdk.log")
                FileManager.default.createFile(atPath: file.path, contents: nil)
                log = try? FileHandle(forWritingTo: file)
            }
            let data = Data((String(format: "%.3f ", stamp) + message + "\n").utf8)
            try? log?.write(contentsOf: data)
            logBytes += data.count
        }
    }

    func start(settings: SurrealInputSettings) {
        #if KL_SURREAL_SDK
        lock.lock()
        self.settings = settings
        let first = !started
        started = true
        lock.unlock()
        guard first else { return }
        launch()
        #endif
    }

    #if KL_SURREAL_SDK
    private func launch() {
        // Take the controllers out of cursor mode BEFORE the SDK connects: once it holds them,
        // a second connection from this app stays in "connecting" (hardware, 2026-10-29).
        SurrealModeKicker.shared.prepare(timeout: 3) { [self] in
            trace("starting Surreal Interactive's SDK")
            // Start first, as Surreal's Unity integration does: SVRStart makes a new provider
            // (make_shared<dm::SubsystemProvider>), and a callback appended before it is dropped
            // with the old one — on hardware no button ever arrived.
            lock.lock(); gotButtons = false; lock.unlock()   // log this start's first buttons
            SVRStart()
            SVRSetButtonCallback { _, chirality, buttons in
                KleptonSurrealSDK.shared.receive(chirality: Int(chirality), buttons: buttons)
            }
            lock.lock(); paused = false; lock.unlock()
        }
    }
    #endif

    /// Five grip squeezes on both controllers since the last call (SurrealResetGesture).
    func takeToggleRequest() -> Bool {
        lock.lock(); defer { lock.unlock() }
        let requested = toggleRequested
        toggleRequested = false
        return requested
    }

    func note(_ message: String) { trace(message) }

    /// Throws away the SDK's alignment by restarting it, so it finds the controllers again
    /// where they are now (after hands mode they came back misaligned, 2026-10-28). The
    /// render thread stops calling the SDK first; the controllers are parked meanwhile.
    func realign() {
        #if KL_SURREAL_SDK
        lock.lock()
        guard started, !paused else { lock.unlock(); return }
        paused = true; clearPolls = true; gotButtons = false
        input = [KleptonHandState(), KleptonHandState()]
        lock.unlock()
        trace("re-aligning: restarting Surreal Interactive's SDK")
        // Two frames for the render thread to see `paused`, and for the last pulse to play.
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.3) { [self] in
            SVRStop()
            DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.5) { [self] in launch() }
        }
        #endif
    }

    #if KL_SURREAL_SDK
    /// The SDK's thread. The same 13-byte packet KleptonSurrealBackend decodes, so the Touch
    /// mapping, analog scale, dead zone and stick touch are the same with either source.
    private func receive(chirality: Int, buttons b: SVRButtons) {
        guard (0...1).contains(chirality) else { return }
        let flags = (b.primary_button != 0 ? 1 : 0) | (b.secondary_button != 0 ? 2 : 0)
            | (b.menu_button != 0 ? 4 : 0) | (b.stick_z_button != 0 ? 8 : 0)
        let packet = Data([UInt8](repeating: 0, count: 8)
                          + [UInt8(flags), b.trigger_button_value, b.grip_button_value, b.stick_x_value, b.stick_y_value])
        lock.lock()
        let first = !gotButtons
        gotButtons = true
        var toggled = false
        if let decoded = SurrealWire.buttons(packet, hand: chirality == 0 ? .left : .right, settings: settings) {
            input[chirality] = decoded
            if gesture.update(hand: chirality, grip: decoded.handTrigger, at: CACurrentMediaTime()) {
                toggleRequested = true; toggled = true
            }
        }
        lock.unlock()
        if first { trace("first buttons from the SDK (hand \(chirality))") }
        if toggled { trace("\(SurrealResetGesture.squeezes) grip squeezes on both controllers: switching hands/controllers") }
    }
    #endif

    /// Render thread, once per hand per frame.
    func sample(hand: Int) -> KleptonHandState {
        var released = KleptonHandState()
        released.position = [0, -10, 0]; released.orientation = simd_quatf(angle: 0, axis: [0, 1, 0])
        #if KL_SURREAL_SDK
        lock.lock()
        let isPaused = paused
        // After a restart: no stale polls, and its connections are logged again.
        if clearPolls { pendingPolls = [[], []]; wasConnected = [false, false]; clearPolls = false }
        lock.unlock()
        guard !isPaused else { return released }
        let chirality = Int32(hand)
        let now = SVRTimeNow()
        // As the SDK's own Unity integration does: the poll three frames back is the one
        // whose frame is finishing now, which is how the SDK learns the render latency.
        if pendingPolls[hand].count >= 3 {
            SVRRenderFinish(chirality, pendingPolls[hand].removeFirst(), now)
        }
        var pose = SVRPose(), linear = SVRVector3f(), angular = SVRVector3f()
        let connected = SVRIsConnected(chirality)
        let tracked = connected && SVRPollDevicePose(chirality, now, &pose, &linear, &angular, nil)
        pendingPolls[hand].append(now)
        if connected != wasConnected[hand] {
            wasConnected[hand] = connected
            trace("hand \(hand) \(connected ? "connected" : "disconnected")")
            if connected {
                // Cursor mode is handled before SVRStart (SurrealModeKicker.prepare): once the SDK
                // holds a controller, a second connection from this app never completes.
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 4) { [self] in
                    lock.lock(); let got = gotButtons; lock.unlock()
                    if !got { trace("no buttons 4 s after connecting hand \(hand)") }
                }
            }
        }
        // Until it has aligned a controller the SDK answers (100, 100, 100): not a pose.
        guard tracked, max(abs(pose.x), abs(pose.y), abs(pose.z)) < 50 else { return released }
        lock.lock()
        var result = input[hand]
        lock.unlock()
        // The C API is the one Surreal's Unity SDK hands straight to Unity: left-handed,
        // +Z forward. ARKit is right-handed with -Z forward, so Z is mirrored — position
        // and linear velocity negate z, and a rotation (an axial quantity) negates x and y.
        // On hardware, as-is gave correlation -0.99 against the visible hand along Z and
        // inverted pitch; mirrored, the median distance to the hand was 5–7 cm.
        result.fromController = true
        result.position = SIMD3(pose.x, pose.y, -pose.z)
        result.orientation = simd_quatf(ix: -pose.qx, iy: -pose.qy, iz: pose.qz, r: pose.qw).normalized
        result.linearVelocity = SIMD3(linear.x, linear.y, -linear.z)
        result.angularVelocity = SIMD3(-angular.x, -angular.y, angular.z)
        return result
        #else
        return released
        #endif
    }

    func vibrate(hand: Int, amplitude: Float, duration: Float) {
        #if KL_SURREAL_SDK
        guard (0...1).contains(hand), amplitude > 0, amplitude.isFinite, duration.isFinite else { return }
        // The runtime's level arrives every frame; one pulse per 20 ms is plenty.
        let t = CACurrentMediaTime()
        guard t >= nextPulse[hand] else { return }
        nextPulse[hand] = t + 0.02
        _ = SVRHapticContinuous(Int32(hand), min(1, amplitude), 170, Double(min(0.25, max(0.03, duration))))
        #endif
    }
}

/// Takes Surreal Touch controllers out of cursor mode before Surreal's SDK connects them. Until
/// a control message arrives the controllers drive the visionOS pointer over HID and send no
/// buttons (see KleptonSurrealBackend.requestControllerMode). This opens this app's own link
/// to the controllers visionOS already has, subscribes only to the control replies (1006),
/// never to pose or input, writes the same mode request and set-controller-mode messages, and
/// lets go again before the SDK starts.
final class SurrealModeKicker: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
    static let shared = SurrealModeKicker()
    private let queue = DispatchQueue(label: "klepton.surreal.mode", qos: .userInitiated)
    private var central: CBCentralManager?
    private var pending: Set<Int> = []                 // hands asked for before Bluetooth was on
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var sides: [UUID: Int] = [:]
    private var control: [UUID: CBCharacteristic] = [:]
    private var sequence: UInt16 = 0x4000              // apart from the SDK's own numbers
    private var onReady: (() -> Void)?                 // starts the SDK; run once
    private var awaiting: Set<UUID> = []               // controllers whose set-mode reply is due
    private var setSequence: [UUID: UInt16] = [:]

    private func trace(_ message: String) { KleptonSurrealSDK.shared.trace("mode: " + message) }

    /// Before the SDK starts: send the messages to every Surreal Touch visionOS has connected,
    /// close this link, and call `ready` on the main queue once each has answered the set-mode
    /// message — or after `timeout`, or at once if none is connected yet.
    func prepare(timeout: Double, then ready: @escaping () -> Void) {
        queue.async { [self] in
            onReady = ready
            pending = [0, 1]
            if central == nil { central = CBCentralManager(delegate: self, queue: queue) }
            else if central?.state == .poweredOn { run() }
            queue.asyncAfter(deadline: .now() + timeout) { [self] in finish("no reply after \(timeout) s") }
        }
    }

    private func finish(_ reason: String) {
        guard let ready = onReady else { return }
        onReady = nil; awaiting.removeAll()
        // Let go first: with this link still open the SDK never connected (hardware, 2026-10-29).
        // The controller keeps controller mode (its light stays green) without it.
        for p in peripherals.values where p.state != .disconnected { central?.cancelPeripheralConnection(p) }
        control.removeAll()
        trace("ready (\(reason)); closed this link, starting the SDK in 0.5 s")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: ready)
    }

    private func run() {
        guard let central else { return }
        let found = central.retrieveConnectedPeripherals(withServices: [CBUUID(string: SurrealWire.service)])
        for hand in pending {
            guard let p = found.first(where: { SurrealHand(name: $0.name ?? "")?.rawValue == hand }) else {
                trace("hand \(hand): no connected Surreal Touch with that side among \(found.count)")
                continue
            }
            peripherals[p.identifier] = p; sides[p.identifier] = hand
            p.delegate = self
            if onReady != nil { awaiting.insert(p.identifier) }
            if p.state == .connected, let ch = control[p.identifier] { send(p, ch) }
            else if p.state == .connected {
                trace("hand \(hand): \(p.name ?? "unknown") connected here, looking for the control characteristic again")
                p.discoverServices([CBUUID(string: SurrealWire.service)])
            } else {
                trace("hand \(hand): connecting to \(p.name ?? "unknown") (state \(p.state.rawValue))")
                central.connect(p)
            }
        }
        pending.removeAll()
        if awaiting.isEmpty { finish("no Surreal Touch connected yet") }
    }

    private func send(_ p: CBPeripheral, _ ch: CBCharacteristic) {
        let steps: [(Double, UInt8, [UInt8], String)] = [
            (0.0, SurrealWire.modeRequest, [], "mode request"),
            (0.5, SurrealWire.modeSet, [SurrealWire.controllerMode], "set controller mode"),
        ]
        for (delay, type, data, label) in steps {
            queue.asyncAfter(deadline: .now() + delay) { [self] in
                guard p.state == .connected else { trace("\(label) skipped, \(p.name ?? "unknown") not connected"); return }
                let packet = SurrealWire.tlv(type: type, sequence: sequence, data: data)
                if type == SurrealWire.modeSet { setSequence[p.identifier] = sequence }
                sequence &+= 1
                trace("\(label) -> \(p.name ?? "unknown")")
                p.writeValue(packet, for: ch, type: ch.properties.contains(.write) ? .withResponse : .withoutResponse)
            }
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        trace("Bluetooth state \(central.state.rawValue)")
        if central.state == .poweredOn { run() }
    }
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        trace("connected to \(peripheral.name ?? "unknown"); discovering services")
        peripheral.discoverServices([CBUUID(string: SurrealWire.service)])
    }
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        trace("disconnected from \(peripheral.name ?? "unknown"): \(error?.localizedDescription ?? "no error")")
    }
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        trace("connect to \(peripheral.name ?? "unknown") failed: \(error?.localizedDescription ?? "unknown")")
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string: SurrealWire.service) }) else {
            trace("\(peripheral.name ?? "unknown"): Surreal service not found (\(error?.localizedDescription ?? "no error"))"); return
        }
        trace("\(peripheral.name ?? "unknown"): service found; discovering characteristics")
        peripheral.discoverCharacteristics([CBUUID(string: SurrealWire.haptic), CBUUID(string: SurrealWire.controlResponse)], for: service)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let chars = service.characteristics ?? []
        trace("\(peripheral.name ?? "unknown"): characteristics \(chars.map { "\($0.uuid):\($0.properties.rawValue)" }.joined(separator: ",")) error=\(error?.localizedDescription ?? "none")")
        if let replies = chars.first(where: { $0.uuid == CBUUID(string: SurrealWire.controlResponse) }),
           replies.properties.contains(.notify) { peripheral.setNotifyValue(true, for: replies) }
        guard let ch = chars.first(where: { $0.uuid == CBUUID(string: SurrealWire.haptic) }) else {
            trace("\(peripheral.name ?? "unknown"): control characteristic not found"); return
        }
        control[peripheral.identifier] = ch
        send(peripheral, ch)
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == CBUUID(string: SurrealWire.controlResponse), let data = characteristic.value,
              let reply = SurrealWire.parseTLV(data) else { return }
        let mode = reply.type == SurrealWire.modeSetResponse
            ? (reply.data.first == SurrealWire.controllerMode ? " (controller mode)" : " (cursor mode)") : ""
        trace("reply hand=\(sides[peripheral.identifier] ?? -1) type=0x\(String(format: "%02x", reply.type)) seq=\(reply.sequence)\(mode)")
        if setSequence[peripheral.identifier] == reply.sequence, awaiting.remove(peripheral.identifier) != nil, awaiting.isEmpty {
            finish("every connected controller answered")
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { trace("write to \(peripheral.name ?? "unknown") failed: \(error.localizedDescription)") }
    }
}
