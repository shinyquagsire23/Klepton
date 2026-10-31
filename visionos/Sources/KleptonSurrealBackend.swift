import Foundation
import CoreBluetooth
import QuartzCore
import simd

/// One BLE delegate queue, a short lock for value snapshots, and no second ARKit session.
/// The renderer never dispatches synchronously to Bluetooth. No UI objects cross this seam.
final class KleptonSurrealBackend: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
    static let shared = KleptonSurrealBackend()
    struct Device: Identifiable {
        var id: UUID
        var name: String
        var hand: SurrealHand
    }
    struct Snapshot {
        var hands = [SurrealHandCache(), SurrealHandCache()]
        var devices: [Device] = []
        var bluetooth = "Bluetooth is idle"
        var scanning = false
        var settings = SurrealInputSettings()
        var simulated = false
        var lastHaptic: [String] = ["", ""]
    }
    private let lock = NSLock()
    private var state = Snapshot()
    private let queue = DispatchQueue(label: "klepton.surreal.ble", qos: .userInteractive)
    private var central: CBCentralManager!
    private var enabled = false
    private var wantsScan = false
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var sides: [UUID: SurrealHand] = [:]
    private var slots: [SurrealHand: UUID] = [:]
    private var desired: Set<UUID> = []
    private var haptics: [UUID: CBCharacteristic] = [:]
    private var notified: [UUID: Set<String>] = [:]
    private var sequence: UInt16 = 0
    private var writing: Set<UUID> = []
    private var nextPulse: [UUID: Double] = [:]
    // One pending haptic per hand, replaced rather than enqueued each render frame.
    private var pendingPulse: [(Float, Float)?] = [nil, nil]
    private var pulseScheduled = false
    private var diagnosticLog: FileHandle?
    private var diagnosticBytes = 0
    private var loggedPackets: Set<String> = []
    // Edge-triggered diagnostics, BLE queue only: grip press/release and pose confidence.
    private var gripDown = [false, false]
    private var poseConfident = [true, true]
    private var resetGesture = SurrealResetGesture()   // BLE queue only
    private var loggedDiscoveries: Set<UUID> = []

    /// Connection events and the first packet size only; never per-frame pose data.
    /// Called exclusively on the BLE queue, outside the renderer's snapshot lock.
    private func trace(_ message: String) {
        NSLog("[surreal] %@", message)
        guard diagnosticBytes < 256 * 1024 else { return }
        if diagnosticLog == nil {
            let file = URL.documentsDirectory.appendingPathComponent("surreal-ble.log")
            FileManager.default.createFile(atPath: file.path, contents: nil)
            diagnosticLog = try? FileHandle(forWritingTo: file)
        }
        let data = Data((String(format: "%.3f ", CACurrentMediaTime()) + message + "\n").utf8)
        try? diagnosticLog?.write(contentsOf: data)
        diagnosticBytes += data.count
    }

    #if targetEnvironment(simulator)
    private var demoTimer: DispatchSourceTimer?
    private var demoHands: Set<Int> = [0, 1]
    #endif

    private override init() { super.init() }
    func snapshot() -> Snapshot { lock.lock(); defer { lock.unlock() }; return state }
    private func edit(_ body: (inout Snapshot) -> Void) { lock.lock(); defer { lock.unlock() }; body(&state) }
    func setSettings(_ settings: SurrealInputSettings) {
        edit {
            guard $0.settings != settings else { return }
            let reset = $0.settings.positionFromHands != settings.positionFromHands
                || (!settings.positionFromHands && $0.settings.gripPitch != settings.gripPitch)
            $0.settings = settings
            if reset { for i in 0...1 { $0.hands[i].resetTracking() } }
        }
    }
    func setEnabled(_ on: Bool) {
        queue.async { [self] in
            enabled = on
            trace("backend enabled=\(on)")
            #if targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--surreal-demo") {
                if on { startDemo() } else {
                    demoTimer?.cancel(); demoTimer = nil
                    edit { $0.hands = [SurrealHandCache(), SurrealHandCache()] }
                }
                return
            }
            #endif
            if on {
                if central == nil { central = CBCentralManager(delegate: self, queue: queue) }
                else if central.state == .poweredOn { reconnectOnQueue() }
            } else if central != nil {
                wantsScan = false; desired.removeAll(); central.stopScan()
                for p in peripherals.values where p.state != .disconnected { central.cancelPeripheralConnection(p) }
                slots.removeAll(); haptics.removeAll(); writing.removeAll(); notified.removeAll()
                edit { $0.scanning = false; $0.hands = [SurrealHandCache(), SurrealHandCache()] }
            }
        }
    }
    func discover() {
        queue.async { [self] in
            guard enabled else { return }
            #if targetEnvironment(simulator)
            if demoTimer != nil { return }
            #endif
            wantsScan = true
            if central.state == .poweredOn { scanOnQueue() }
        }
    }
    private func scanOnQueue() {
        // System-paired peripherals may already be connected and no longer advertise.
        // Discover those as well as new advertisements; an app still calls connect().
        let connected = central.retrieveConnectedPeripherals(withServices: [CBUUID(string: SurrealWire.service)])
        trace("discover: \(connected.count) system-connected Surreal peripherals")
        for peripheral in connected {
            register(peripheral, name: peripheral.name ?? "", source: "system connection")
        }
        central.scanForPeripherals(withServices: [CBUUID(string: SurrealWire.service)])
        edit { $0.scanning = true }
        queue.asyncAfter(deadline: .now() + 15) { [self] in
            central.stopScan(); wantsScan = false; edit { $0.scanning = false }
            trace("scan finished: \(snapshot().devices.count) identified controllers")
        }
    }
    func reconnect() { queue.async { [self] in
        #if targetEnvironment(simulator)
        if demoTimer != nil { demoHands = [0, 1]; return }
        #endif
        reconnectOnQueue()
    } }
    private func reconnectOnQueue() {
        guard enabled, central.state == .poweredOn else { return }
        for hand in SurrealHand.allCases {
            guard slots[hand] == nil,
                  let raw = UserDefaults.standard.string(forKey: "klepton.surreal.\(hand.rawValue).device"),
                  let id = UUID(uuidString: raw) else { continue }
            for peripheral in central.retrievePeripherals(withIdentifiers: [id]) {
                // Stored side is assigned only after a verified name and successful subscription.
                peripherals[id] = peripheral; sides[id] = hand
                connectOnQueue(id)
            }
        }
    }
    func connect(_ id: UUID) { queue.async { [self] in connectOnQueue(id) } }
    private func connectOnQueue(_ id: UUID) {
        guard enabled, central.state == .poweredOn, let p = peripherals[id], let hand = sides[id] else { return }
        if let old = slots[hand], old != id { disconnectOnQueue(hand) }
        slots[hand] = id; desired.insert(id)
        edit { $0.hands[hand.rawValue].connectionRequested = true; $0.hands[hand.rawValue].deviceID = id }
        guard p.state != .connecting else { return }
        trace("connect hand=\(hand.rawValue) name=\(p.name ?? "unknown") state=\(p.state.rawValue)")
        edit { $0.hands[hand.rawValue].status = "Connecting…" }
        // A peripheral connected by the system still needs this app's connection.
        central.connect(p)
    }
    func disconnect(_ hand: SurrealHand) { queue.async { [self] in
        #if targetEnvironment(simulator)
        if demoTimer != nil {
            demoHands.remove(hand.rawValue)
            edit { $0.hands[hand.rawValue].disconnect() }
            return
        }
        #endif
        disconnectOnQueue(hand)
    } }
    private func disconnectOnQueue(_ hand: SurrealHand) {
        guard let id = slots.removeValue(forKey: hand) else { return }
        trace("disconnect requested hand=\(hand.rawValue)")
        desired.remove(id)
        edit { $0.hands[hand.rawValue].disconnect() }
        if let p = peripherals[id] { central.cancelPeripheralConnection(p) }
    }
    func recalibrate() { edit { for i in 0...1 { $0.hands[i].resetTracking() } } }
    func sample(hand: Int, reference: simd_float4x4?, at time: Double, alignmentTarget: simd_quatf? = nil) -> KleptonHandState {
        lock.lock(); defer { lock.unlock() }
        let settings = state.settings
        return state.hands[hand].sample(reference: reference, at: time, now: CACurrentMediaTime(), settings: settings, aimOffset: settings.aimOffsets[hand].rotation, alignmentTarget: alignmentTarget)
    }
    func vibrate(hand: Int, amplitude: Float, duration: Float) {
        guard (0...1).contains(hand), amplitude > 0 else { return }
        lock.lock()
        pendingPulse[hand] = (amplitude, duration)
        let schedule = !pulseScheduled
        pulseScheduled = true
        lock.unlock()
        if schedule { queue.async { [self] in drainHaptics() } }
    }
    private func drainHaptics() {
        lock.lock()
        let pending = pendingPulse; pendingPulse = [nil, nil]; pulseScheduled = false
        lock.unlock()
        guard enabled else { return }
        #if targetEnvironment(simulator)
        if demoTimer != nil {
            edit { for i in 0...1 {
                if let pulse = pending[i] { $0.lastHaptic[i] = String(format: "Simulated pulse %.2f / %.0f ms", pulse.0, pulse.1 * 1000) }
            } }
            return
        }
        #endif
        for hand in SurrealHand.allCases {
            guard let (amplitude, duration) = pending[hand.rawValue], let id = slots[hand],
                  let p = peripherals[id], p.state == .connected, let ch = haptics[id],
                  !writing.contains(id), CACurrentMediaTime() >= (nextPulse[id] ?? 0) else { continue }
            let response = ch.properties.contains(.write)
            guard response || (ch.properties.contains(.writeWithoutResponse) && p.canSendWriteWithoutResponse) else { continue }
            let packet = SurrealWire.vibration(sequence: sequence, amplitude: amplitude, duration: duration)
            sequence &+= 1
            nextPulse[id] = CACurrentMediaTime() + Double(min(0.25, max(0.03, duration)))
            if response { writing.insert(id) }
            p.writeValue(packet, for: ch, type: response ? .withResponse : .withoutResponse)
        }
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let message: String
        switch central.state {
        case .poweredOn: message = "Bluetooth ready"
        case .poweredOff: message = "Turn on Bluetooth on Vision Pro"
        case .unauthorized: message = "Allow Bluetooth for this game in Vision Pro Settings"
        case .unsupported: message = "Bluetooth LE unavailable"
        default: message = "Bluetooth is starting…"
        }
        edit { $0.bluetooth = message }
        trace(message)
        if central.state == .poweredOn, enabled {
            reconnectOnQueue(); if wantsScan { scanOnQueue() }
        } else {
            // CoreBluetooth may not deliver individual disconnect callbacks after power loss.
            slots.removeAll(); haptics.removeAll(); notified.removeAll(); writing.removeAll()
            edit { $0.scanning = false; $0.hands = [SurrealHandCache(), SurrealHandCache()] }
        }
    }
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? ""
        register(peripheral, name: name, source: "advertisement RSSI=\(RSSI)")
    }
    private func register(_ peripheral: CBPeripheral, name: String, source: String) {
        let hand = SurrealHand(name: name) ?? peripheral.name.flatMap { SurrealHand(name: $0) }
        if loggedDiscoveries.insert(peripheral.identifier).inserted {
            trace("found name=\(name) peripheralName=\(peripheral.name ?? "unknown") hand=\(hand.map { String($0.rawValue) } ?? "unidentified") source=\(source)")
        }
        guard let hand else { return }
        let id = peripheral.identifier
        peripherals[id] = peripheral; sides[id] = hand
        edit {
            if !$0.devices.contains(where: { $0.id == id }) { $0.devices.append(Device(id: id, name: name, hand: hand)) }
        }
    }
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard enabled, desired.contains(peripheral.identifier) else { central.cancelPeripheralConnection(peripheral); return }
        trace("connected: \(peripheral.name ?? "unknown"); discovering services")
        if let hand = sides[peripheral.identifier] {
            edit { $0.hands[hand.rawValue].status = "Connected to Bluetooth; reading controller services…" }
        }
        peripheral.delegate = self
        peripheral.discoverServices([CBUUID(string: SurrealWire.service), CBUUID(string: "180F")])
    }
    private func failed(_ p: CBPeripheral, message: String) {
        let id = p.identifier
        trace("connection ended: \(p.name ?? "unknown"): \(message)")
        haptics[id] = nil; notified[id] = nil; writing.remove(id); nextPulse[id] = nil
        guard let hand = sides[id], slots[hand] == id else { return }
        // Keep ownership during retries so Disconnect or a replacement controller can
        // cancel this intent. An old retry must never displace a newly selected device.
        if !desired.contains(id) { slots[hand] = nil }
        edit {
            $0.hands[hand.rawValue].disconnect()
            $0.hands[hand.rawValue].status = message
            $0.hands[hand.rawValue].connectionRequested = desired.contains(id)
        }
        guard enabled, desired.contains(id) else { return }
        queue.asyncAfter(deadline: .now() + 2) { [self] in
            guard enabled, desired.contains(id), slots[hand] == id else { return }
            connectOnQueue(id)
        }
    }
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        failed(peripheral, message: error?.localizedDescription ?? "Connection failed; retrying…")
    }
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        failed(peripheral, message: desired.contains(peripheral.identifier) ? "Disconnected; reconnecting when available" : "Disconnected")
    }
    private func reject(_ p: CBPeripheral, _ reason: String) {
        desired.remove(p.identifier)
        failed(p, message: reason)
        central.cancelPeripheralConnection(p)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard enabled, desired.contains(peripheral.identifier) else { return }
        trace("services \(peripheral.name ?? "unknown"): \(peripheral.services?.map { $0.uuid.uuidString }.joined(separator: ",") ?? "none") error=\(error?.localizedDescription ?? "none")")
        guard error == nil, let services = peripheral.services,
              services.contains(where: { $0.uuid == CBUUID(string: SurrealWire.service) }) else {
            reject(peripheral, "Surreal service unavailable"); return
        }
        for service in services { peripheral.discoverCharacteristics(nil, for: service) }
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard enabled, desired.contains(peripheral.identifier) else { return }
        guard error == nil else {
            if service.uuid == CBUUID(string: SurrealWire.service) { reject(peripheral, "Cannot read controller services") }
            return
        }
        let chars = service.characteristics ?? []
        trace("characteristics \(service.uuid): \(chars.map { "\($0.uuid):\($0.properties.rawValue)" }.joined(separator: ","))")
        if service.uuid == CBUUID(string: SurrealWire.service) {
            for uuid in [SurrealWire.pose, SurrealWire.input] {
                guard let ch = chars.first(where: { $0.uuid == CBUUID(string: uuid) }), ch.properties.contains(.notify) else {
                    reject(peripheral, "Required input notifications unavailable"); return
                }
                peripheral.setNotifyValue(true, for: ch)
            }
            // Replies to control messages (see requestControllerMode). Optional: input works without it.
            if let replies = chars.first(where: { $0.uuid == CBUUID(string: SurrealWire.controlResponse) }),
               replies.properties.contains(.notify) {
                peripheral.setNotifyValue(true, for: replies)
            }
            haptics[peripheral.identifier] = chars.first { $0.uuid == CBUUID(string: SurrealWire.haptic) }
        } else if let battery = chars.first(where: { $0.uuid == CBUUID(string: "2A19") }) {
            if battery.properties.contains(.read) { peripheral.readValue(for: battery) }
            if battery.properties.contains(.notify) { peripheral.setNotifyValue(true, for: battery) }
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        let uuid = characteristic.uuid.uuidString
        guard [SurrealWire.pose, SurrealWire.input].contains(uuid) else { return }
        trace("subscription \(peripheral.name ?? "unknown") \(uuid) notifying=\(characteristic.isNotifying) error=\(error?.localizedDescription ?? "none")")
        guard error == nil, characteristic.isNotifying else { reject(peripheral, "Input subscription failed"); return }
        let id = peripheral.identifier
        notified[id, default: []].insert(uuid)
        if notified[id]?.count == 2, let hand = sides[id], slots[hand] == id, enabled {
            edit { $0.hands[hand.rawValue].connected = true; $0.hands[hand.rawValue].status = "Connected; start game to calibrate" }
            UserDefaults.standard.set(id.uuidString, forKey: "klepton.surreal.\(hand.rawValue).device")
            requestControllerMode(peripheral)
        }
    }
    /// Subscribing is not enough: until a control message arrives, the controller keeps
    /// driving the visionOS pointer over HID and sends nothing on pose/input. On hardware
    /// (2026-10-23) the first pose packet followed the first control message by 2 ms. Ask
    /// the mode, then set controller mode; the replies arrive on 1006 and are traced.
    private func requestControllerMode(_ peripheral: CBPeripheral) {
        let id = peripheral.identifier
        let steps: [(Double, UInt8, [UInt8], String)] = [
            (0.0, SurrealWire.modeRequest, [], "mode request"),
            (0.5, SurrealWire.modeSet, [SurrealWire.controllerMode], "set controller mode"),
        ]
        for (delay, type, data, label) in steps {
            queue.asyncAfter(deadline: .now() + delay) { [self] in
                guard enabled, let p = peripherals[id], p.state == .connected, let ch = haptics[id] else {
                    trace("control: \(label) skipped, \(peripheral.name ?? "unknown") not ready")
                    return
                }
                let response = ch.properties.contains(.write)
                let packet = SurrealWire.tlv(type: type, sequence: sequence, data: data)
                sequence &+= 1
                trace("control: \(label) -> \(p.name ?? "unknown")")
                if response { writing.insert(id) }
                p.writeValue(packet, for: ch, type: response ? .withResponse : .withoutResponse)
            }
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let now = CACurrentMediaTime()
        guard enabled, error == nil, let data = characteristic.value,
              let hand = sides[peripheral.identifier], slots[hand] == peripheral.identifier else { return }
        let packetKey = "\(peripheral.identifier):\(characteristic.uuid):\(data.count)"
        if loggedPackets.insert(packetKey).inserted {
            trace("packet hand=\(hand.rawValue) characteristic=\(characteristic.uuid) bytes=\(data.count)")
        }
        switch characteristic.uuid.uuidString {
        case SurrealWire.pose:
            guard let pose = SurrealPose(data) else { return }
            let i = hand.rawValue
            var gap = 0.0, clockReset = false
            edit {
                if $0.hands[i].pose != nil { gap = now - $0.hands[i].receivedAt }
                clockReset = $0.hands[i].receivePose(pose, at: now)
            }
            if gap >= 0.25 { trace(String(format: "pose gap hand=%d %.0f ms", i, gap * 1000)) }
            if clockReset { trace("pose clock reset hand=\(i)") }
            if (pose.confidence > 0) != poseConfident[i] {
                poseConfident[i] = pose.confidence > 0
                trace("pose confidence hand=\(i) \(poseConfident[i] ? "back" : "zero")")
            }
        case SurrealWire.input:
            let settings = snapshot().settings
            guard let input = SurrealWire.buttons(data, hand: hand, settings: settings) else { return }
            edit { $0.hands[hand.rawValue].input = input; $0.hands[hand.rawValue].hasInput = true }
            if (input.handTrigger > 0.55) != gripDown[hand.rawValue] {
                gripDown[hand.rawValue].toggle()
                trace(String(format: "grip hand=%d %@ (%.2f)", hand.rawValue,
                             gripDown[hand.rawValue] ? "down" : "up", input.handTrigger))
            }
            if resetGesture.update(hand: hand.rawValue, grip: input.handTrigger, at: now) {
                // Same reset as the launcher's button; the next still grip hold re-registers.
                trace("reset gesture: \(SurrealResetGesture.squeezes) grip squeezes on both controllers — tracking reset")
                recalibrate()
                for h in 0...1 { vibrate(hand: h, amplitude: 0.8, duration: 0.25) }
            }
        case "2A19":
            if let value = data.first, value <= 100 { edit { $0.hands[hand.rawValue].battery = Int(value) } }
        case SurrealWire.controlResponse:
            guard let reply = SurrealWire.parseTLV(data) else {
                trace("control: unparsed reply hand=\(hand.rawValue) bytes=\(data.count)"); return
            }
            let mode = reply.type == SurrealWire.modeSetResponse
                ? (reply.data.first == SurrealWire.controllerMode ? " (controller mode)" : " (cursor mode)") : ""
            trace("control: reply hand=\(hand.rawValue) type=0x\(String(format: "%02x", reply.type)) seq=\(reply.sequence)\(mode)")
        default: break
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        writing.remove(peripheral.identifier)
        if let error { trace("control: write to \(peripheral.name ?? "unknown") \(characteristic.uuid) failed: \(error.localizedDescription)") }
    }
    #if targetEnvironment(simulator)
    /// Synthetic wire notifications only. No controller or Bluetooth validation is claimed.
    private func startDemo() {
        guard demoTimer == nil else { return }
        demoHands = [0, 1]
        edit { $0.simulated = true; $0.bluetooth = "SIMULATED Surreal Touch — no Bluetooth hardware" }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10))
        timer.setEventHandler { [weak self] in self?.demoTick() }
        demoTimer = timer; timer.resume()
    }
    private func demoTick() {
        let now = CACurrentMediaTime()
        for hand in SurrealHand.allCases where demoHands.contains(hand.rawValue) {
            let t = Float(now.truncatingRemainder(dividingBy: 100)) + Float(hand.rawValue)
            let angle = 0.3 * sin(t)
            let ticks = UInt64(now * 1_000_000_000)
            var bytes = (0..<8).map { UInt8(truncatingIfNeeded: ticks >> (8 * $0)) }
            // Wire coordinates are left-handed; native device -> world reflection is exercised.
            let floats: [Float] = [hand == .left ? -0.25 : 0.25, 1.2 + 0.08 * sin(t), 0.45,
                                   0, 0, sin(angle / 2), cos(angle / 2), 1,
                                   0, 0.08 * cos(t), 0, 0, 0, 0.3 * cos(t), 0, 0, 0]
            for value in floats { for i in 0..<4 { bytes.append(UInt8(truncatingIfNeeded: value.bitPattern >> (8 * i))) } }
            let buttons = Data([UInt8](repeating: 0, count: 8) + [UInt8(1 << (Int(now) % 4)),
                              UInt8((sin(t) + 1) * 63), 100, UInt8(128 + 100 * sin(t)), UInt8(128 + 100 * cos(t))])
            edit { snapshot in
                let i = hand.rawValue
                snapshot.hands[i].connected = true
                snapshot.hands[i].deviceID = UUID(uuidString: hand == .left ? "00000000-0000-0000-0000-000000000001" : "00000000-0000-0000-0000-000000000002")
                snapshot.hands[i].connectionRequested = true
                snapshot.hands[i].battery = 80 - i * 10
                snapshot.hands[i].receivePose(Data(bytes), at: now)
                snapshot.hands[i].input = SurrealWire.buttons(buttons, hand: hand, settings: snapshot.settings)!
                snapshot.hands[i].hasInput = true
                let target = now + 0.011
                let reference = snapshot.hands[i].pose!.predicted(at: target) * snapshot.settings.bodyFromGrip
                _ = snapshot.hands[i].sample(reference: reference, at: target, now: now, settings: snapshot.settings, aimOffset: snapshot.settings.aimOffsets[i].rotation, alignmentTarget: simd_quatf(reference))
            }
        }
    }
    #endif

}
