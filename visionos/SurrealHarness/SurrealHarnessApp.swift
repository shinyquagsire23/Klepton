// A controller diagnostic app, with the SAME input/backend/view files as packaged games.
// --surreal-demo enables synthetic wire packets on Simulator only.
import SwiftUI
import simd

func klT(_ english: String, _ german: String) -> String { english }
@MainActor final class KLRun: ObservableObject {
    static let shared = KLRun()
    @Published var started = false
    @Published var adjustingControllers = false
}

@main
struct SurrealHarnessApp: App {
    init() {
        if ProcessInfo.processInfo.arguments.contains("--surreal-demo") {
            KleptonControllerMode.surreal.save()
        }
    }
    @State private var testReport = ""
    var body: some Scene {
        WindowGroup(id: SurrealAimGuide.launcherID) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Text("Surreal Touch diagnostics").font(.largeTitle)
                    Text("Simulator tests use synthetic motion and button packets. Bluetooth, physical calibration and game behavior require Vision Pro hardware.")
                        .foregroundStyle(.secondary)
                    if !testReport.isEmpty { Text(testReport).foregroundStyle(.green) }
                    KleptonControllerSettingsView()
                }.padding(28)
                .background(SurrealAimPreviewOpener())
                .task {
                    #if targetEnvironment(simulator)
                    if ProcessInfo.processInfo.arguments.contains("--self-test") {
                        testReport = await Self.runTests()
                    }
                    #endif
                }
            }.frame(width: 1000, height: 900)
        }
        .defaultSize(width: 1000, height: 900)
        ImmersiveSpace(id: SurrealAimGuide.spaceID) { KleptonSurrealAimSpace() }
            .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
    #if targetEnvironment(simulator)
    @MainActor static func runTests() async -> String {
        let backend = KleptonSurrealBackend.shared
        var checks: [String: Bool] = [:]
        func settle() async { try? await Task.sleep(for: .milliseconds(250)) }
        backend.setSettings(.init())
        backend.setEnabled(true); await settle()
        let first = backend.snapshot()
        checks["synthetic source active"] = first.simulated && first.hands.allSatisfy(\.connected)
        checks["both hands calibrated"] = first.hands.allSatisfy { $0.registration != nil }
        let oldTicks = first.hands[0].pose?.ticks
        await settle()
        checks["pose stream advancing"] = backend.snapshot().hands[0].pose?.ticks != oldTicks
        let time = ProcessInfo.processInfo.systemUptime
        let left = backend.sample(hand: 0, reference: nil, at: time)
        let right = backend.sample(hand: 1, reference: nil, at: time)
        checks["left and right world poses"] = (left.position?.x ?? 0) < 0 && (right.position?.x ?? 0) > 0
        checks["analogs mapped"] = left.handTrigger > 0.7 && right.handTrigger > 0.7
        backend.disconnect(.left); await settle()
        let lost = backend.sample(hand: 0, reference: nil, at: ProcessInfo.processInfo.systemUptime)
        checks["disconnect releases and parks"] = lost.buttons == 0 && lost.indexTrigger == 0 && lost.handTrigger == 0 && lost.position == [0, -10, 0]
        checks["other hand remains connected"] = backend.snapshot().hands[1].connected
        backend.vibrate(hand: 1, amplitude: 0.5, duration: 0.1); await settle()
        checks["haptic routed to simulated hand"] = !backend.snapshot().lastHaptic[1].isEmpty
        backend.reconnect(); await settle()
        checks["reconnect restores both hands"] = backend.snapshot().hands.allSatisfy(\.connected)
        let earlyGuide = SurrealAimGuide()
        await earlyGuide.run()
        checks["line adjustment can start before RealityView creates content"] = earlyGuide.ready && earlyGuide.root.children.count == 2
        earlyGuide.stop()
        let handSettings = SurrealInputSettings(positionFromHands: true)
        backend.setSettings(handSettings)
        checks["position setting reaches backend"] = backend.snapshot().settings.positionFromHands
        await settle(); await settle()
        var handReference = matrix_identity_float4x4
        handReference.columns.3 = [-2, 1, -1, 1]
        let handLeft = backend.sample(hand: 0, reference: handReference, at: ProcessInfo.processInfo.systemUptime)
        handReference.columns.3 = [2, 1, -1, 1]
        let handRight = backend.sample(hand: 1, reference: handReference, at: ProcessInfo.processInfo.systemUptime)
        checks["both positions use matching hand references"] = handLeft.position == [-2, 1, -1] && handRight.position == [2, 1, -1]
        checks["physical input retained with hand positions"] = handLeft.handTrigger > 0.7 && handRight.handTrigger > 0.7
        backend.setSettings(handSettings)
        checks["unchanged settings preserve calibration"] = backend.snapshot().hands.allSatisfy { $0.registration != nil }
        let registered = backend.snapshot().hands.map(\.registration)
        var adjusted = handSettings; adjusted.aimOffsets[0] = SurrealAimOffset(degrees: [15, -20, 5])
        backend.setSettings(adjusted)
        checks["manual adjustment retains both world registrations"] = backend.snapshot().hands.map(\.registration) == registered
        let comparisonTime = ProcessInfo.processInfo.systemUptime
        let beforeWristChange = backend.sample(hand: 0, reference: handReference, at: comparisonTime)
        var rotatedHand = simd_float4x4(simd_quatf(angle: 1.2, axis: [1, 0, 0]))
        rotatedHand.columns.3 = handReference.columns.3
        let afterWristChange = backend.sample(hand: 0, reference: rotatedHand, at: comparisonTime)
        checks["hand rotation cannot steer the backend orientation"] = beforeWristChange.position == [2, 1, -1]
            && SurrealControllerOrientation.angle(beforeWristChange.orientation!, afterWristChange.orientation!) < 0.01
        backend.setSettings(handSettings)
        let reset = backend.sample(hand: 0, reference: handReference, at: comparisonTime)
        checks["plus minus adjustment changes live output"] = SurrealControllerOrientation.angle(beforeWristChange.orientation!, reset.orientation!) > 0.1
        backend.setSettings(.init())
        checks["native position can be restored"] = !backend.snapshot().settings.positionFromHands
        backend.setEnabled(false); await settle()
        checks["standard mode stops source"] = backend.snapshot().hands.allSatisfy { !$0.connected && $0.input.buttons == 0 }
        KleptonControllerMode.standard.save()
        checks["standard persisted"] = KleptonControllerMode.selected() == .standard
        KleptonControllerMode.surreal.save()
        checks["surreal persisted"] = KleptonControllerMode.selected() == .surreal
        backend.setEnabled(true)
        let report = checks.values.allSatisfy { $0 }
            ? "PASS: \(checks.count) simulator integration checks"
            : "FAIL: " + checks.filter { !$0.value }.keys.sorted().joined(separator: ", ")
        let file = URL.documentsDirectory.appendingPathComponent("surreal-self-test.json")
        if let data = try? JSONSerialization.data(withJSONObject: ["report": report, "checks": checks], options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: file, options: .atomic)
        }
        print(report)
        return report
    }
    #endif

}

@MainActor private enum SurrealPreviewState { static var opened = false }
private struct SurrealAimPreviewOpener: View {
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissWindow) private var dismissWindow
    var body: some View {
        Color.clear.frame(width: 1, height: 1).task {
            #if targetEnvironment(simulator)
            guard ProcessInfo.processInfo.arguments.contains("--aim-preview"), !SurrealPreviewState.opened else { return }
            SurrealPreviewState.opened = true
            try? await Task.sleep(for: .milliseconds(600))
            KLRun.shared.adjustingControllers = true
            let result = await openSpace(id: SurrealAimGuide.spaceID)
            print("Aim preview: \(result)")
            if case .opened = result { dismissWindow(id: SurrealAimGuide.launcherID) }
            else { KLRun.shared.adjustingControllers = false }
            #endif
        }
    }
}
