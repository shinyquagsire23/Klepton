import SwiftUI
import RealityKit
import ARKit
import QuartzCore
import simd

/// Optional line-only adjustment before gameplay; owns ARKit only while open.
@MainActor final class SurrealAimGuide: ObservableObject {
    static let spaceID = "SurrealAimAdjustment"
    static let launcherID = "KleptonStart"
    @Published var ready = false
    @Published var canCenter = false
    @Published var failure = ""
    let root = Entity()
    private var rays: [Entity] = []
    private var panel: Entity?
    private let session = ARKitSession()
    private let hands = HandTrackingProvider()
    private let world = WorldTrackingProvider()
    private let backend = KleptonSurrealBackend.shared
    private var origin: simd_float4x4?
    private var forward: simd_quatf?
    private var devices: [simd_quatf?] = [nil, nil]
    private var deviceIDs: [UUID?] = [nil, nil]
    private var ticks: [UInt64?] = [nil, nil]
    private var alignments: [SurrealControllerOrientation?] = [nil, nil]
    private var lastFrame: Double = 0
    private var stopped = false
    private var running = false

    func setupRays() {
        guard rays.isEmpty else { return }
        for hand in 0...1 {
            let ray = Entity()
            let material = UnlitMaterial(color: hand == 0 ? .cyan : .orange)
            let stem = ModelEntity(mesh: .generateCylinder(height: 0.5, radius: 0.003), materials: [material])
            stem.orientation = simd_quatf(angle: -.pi / 2, axis: [1, 0, 0]); stem.position.z = -0.25
            ray.addChild(stem)
            let tick = ModelEntity(mesh: .generateCylinder(height: 0.06, radius: 0.003), materials: [material])
            tick.position = [0, 0.03, -0.5]; ray.addChild(tick)
            ray.isEnabled = false; root.addChild(ray); rays.append(ray)
        }
    }
    func installPanel(_ entity: Entity) { panel = entity; placePanel() }
    private func placePanel() {
        guard let origin else { return }
        var offset = matrix_identity_float4x4; offset.columns.3 = [0, 0.10, -1.05, 1]
        panel?.transform = Transform(matrix: origin * offset)
    }
    func centerForward() {
        guard canCenter, CACurrentMediaTime() - lastFrame < 0.25, let forward else { return }
        for i in 0...1 {
            if let device = devices[i] { alignments[i] = .init(device: device, target: forward) }
        }
    }
    func run() async {
        guard !KLRun.shared.started, !stopped else { return }
        setupRays(); running = true
        defer { session.stop(); running = false; if stopped { KLRun.shared.adjustingControllers = false } }
        #if targetEnvironment(simulator)
        if backend.snapshot().simulated {
            var base = matrix_identity_float4x4; base.columns.3.y = 1.6
            origin = base; ready = true; placePanel()
            tick(simulated: true)
            if ProcessInfo.processInfo.arguments.contains("--aim-preview") {
                while !Task.isCancelled && !stopped {
                    tick(simulated: true); try? await Task.sleep(for: .milliseconds(20))
                }
            }
            return
        }
        #endif
        guard HandTrackingProvider.isSupported, WorldTrackingProvider.isSupported else {
            failure = klT("Hand and world tracking are unavailable.", "Hand- und Welttracking nicht verfügbar."); return
        }
        let permissions = await session.requestAuthorization(for: [.handTracking, .worldSensing])
        guard !stopped, permissions.values.allSatisfy({ $0 == .allowed }) else {
            failure = klT("Allow hand and world tracking in Settings.", "Hand- und Welttracking in Einstellungen erlauben."); return
        }
        do {
            try await session.run([world, hands])
            while !Task.isCancelled && !stopped {
                tick(); try await Task.sleep(for: .milliseconds(20))
            }
        } catch is CancellationError { }
        catch { failure = error.localizedDescription }
    }
    func stop() {
        stopped = true; session.stop()
        if !running { KLRun.shared.adjustingControllers = false }
    }
    private func tick(simulated: Bool = false) {
        let now = CACurrentMediaTime()
        canCenter = false; devices = [nil, nil]
        for ray in rays { ray.isEnabled = false }
        let head: simd_float4x4
        #if targetEnvironment(simulator)
        if simulated { head = origin ?? matrix_identity_float4x4 }
        else {
            guard let anchor = world.queryDeviceAnchor(atTimestamp: now), anchor.isTracked else { return }
            head = anchor.originFromAnchorTransform
        }
        #else
        guard let anchor = world.queryDeviceAnchor(atTimestamp: now), anchor.isTracked else { return }
        head = anchor.originFromAnchorTransform
        #endif
        guard let heading = SurrealControllerOrientation.forward(head: head) else { return }
        forward = heading
        if origin == nil {
            var base = simd_float4x4(heading); base.columns.3 = head.columns.3
            origin = base; ready = true; placePanel()
        }
        let state = backend.snapshot()
        let anchors = simulated ? nil : hands.handAnchors(at: now)
        for i in 0...1 {
            let h = state.hands[i]
            guard h.connected, let pose = h.pose, now - h.receivedAt < 0.25 else {
                alignments[i] = nil; continue
            }
            if deviceIDs[i] != h.deviceID || ticks[i].map({ pose.ticks < $0 }) == true { alignments[i] = nil }
            deviceIDs[i] = h.deviceID; ticks[i] = pose.ticks
            let position: SIMD3<Float>
            if simulated { position = [i == 0 ? -0.22 : 0.22, 1.38, -0.60] }
            else {
                guard let hand = i == 0 ? anchors?.leftHand : anchors?.rightHand, hand.isTracked else { continue }
                position = Self.gripPosition(hand)
            }
            let device = simd_quatf(pose.predicted(at: now)); devices[i] = device
            let offset = state.settings.aimOffsets[i].rotation
            let q = alignments[i]?.rotation(device: device, offset: offset) ?? (simd_quatf(origin!) * device * offset).normalized
            var transform = simd_float4x4(q); transform.columns.3 = SIMD4(position, 1)
            rays[i].transform = Transform(matrix: transform); rays[i].isEnabled = true
        }
        lastFrame = now; canCenter = devices.allSatisfy { $0 != nil }
    }
    private static func gripPosition(_ anchor: HandAnchor) -> SIMD3<Float> {
        var local = SIMD4<Float>(0, 0, 0, 1)
        if let skeleton = anchor.handSkeleton {
            let a = skeleton.joint(.middleFingerMetacarpal), b = skeleton.joint(.middleFingerKnuckle)
            if a.isTracked && b.isTracked { local = (a.anchorFromJointTransform.columns.3 + b.anchorFromJointTransform.columns.3) / 2 }
        }
        let p = anchor.originFromAnchorTransform * local
        return [p.x, p.y, p.z]
    }
}

private struct SurrealAimControls: View {
    let hand: Int
    @State private var offset: SurrealAimOffset
    init(hand: Int) { self.hand = hand; _offset = State(initialValue: .stored(hand: hand)) }
    private func save() {
        offset.save(hand: hand)
        KleptonSurrealBackend.shared.setSettings(.stored())
    }
    var body: some View {
        VStack(spacing: 12) {
            Text(hand == 0 ? klT("Left · blue", "Links · blau") : klT("Right · orange", "Rechts · orange")).font(.headline)
            ForEach(0..<3) { axis in
                HStack {
                    Text([klT("Up / down", "Hoch / runter"), klT("Left / right", "Links / rechts"), klT("Twist", "Drehung")][axis]).frame(width: 105, alignment: .leading)
                    Button("−") { offset.degrees[axis] = max(-90, offset.degrees[axis] - 1); save() }
                        .disabled(offset.degrees[axis] <= -90)
                        .accessibilityLabel(klT("Decrease angle", "Winkel verringern"))
                    Text("\(Int(offset.degrees[axis]))°").monospacedDigit().frame(width: 42)
                    Button("+") { offset.degrees[axis] = min(90, offset.degrees[axis] + 1); save() }
                        .disabled(offset.degrees[axis] >= 90)
                        .accessibilityLabel(klT("Increase angle", "Winkel erhöhen"))
                }
            }
            Button(klT("Reset to 0°", "Auf 0° zurücksetzen")) { offset = .init(); save() }
        }
    }
}

struct KleptonSurrealAimSpace: View {
    @StateObject private var guide = SurrealAimGuide()
    @Environment(\.dismissImmersiveSpace) private var dismissSpace
    @Environment(\.openWindow) private var openWindow
    var body: some View {
        RealityView { content, attachments in
            guide.setupRays(); content.add(guide.root)
            if let panel = attachments.entity(for: "controls") { guide.installPanel(panel); content.add(panel) }
        } attachments: {
            Attachment(id: "controls") {
                VStack(spacing: 18) {
                    Text(klT("Adjust controller direction", "Controller-Richtung anpassen")).font(.title)
                    Text(klT("Optional. Point both controllers forward with sticks up, look ahead and choose Center forward. Use + / − to adjust the lines. Changes are saved for this game.", "Optional. Beide Controller mit Sticks nach oben nach vorn richten, geradeaus schauen und Vorwärts zentrieren wählen. Linien mit + / − anpassen. Änderungen werden für dieses Spiel gespeichert."))
                    Text(klT("Hands: position only · Surreal: rotation and buttons", "Hände: nur Position · Surreal: Drehung und Tasten")).font(.callout)
                    if !guide.failure.isEmpty { Text(guide.failure).foregroundStyle(.orange) }
                    else if !guide.ready { ProgressView() }
                    else if !guide.canCenter { Text(klT("Keep both hands visible and controllers connected.", "Beide Hände sichtbar und Controller verbunden halten.")).foregroundStyle(.secondary) }
                    HStack(spacing: 30) { SurrealAimControls(hand: 0); SurrealAimControls(hand: 1) }
                    HStack {
                        Button(klT("Center forward", "Vorwärts zentrieren")) { guide.centerForward() }.disabled(!guide.canCenter)
                        Button(klT("Done", "Fertig")) {
                            Task { guide.stop(); openWindow(id: SurrealAimGuide.launcherID); await dismissSpace() }
                        }
                    }
                    #if targetEnvironment(simulator)
                    Text("SIMULATED · sensor rotation with illustrative positions").font(.caption).foregroundStyle(.orange)
                    #endif
                }.padding(24).frame(width: 760).glassBackgroundEffect()
            }
        }
        .task { await guide.run() }
        .onDisappear { guide.stop() }
    }
}
