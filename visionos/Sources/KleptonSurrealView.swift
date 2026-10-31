import SwiftUI

/// Pairing is local to the headset; changing the Mac default never pairs devices.
struct KleptonControllerSettingsView: View {
    @State private var mode = KleptonControllerMode.selected()
    @AppStorage("klepton.surreal.analogScale") private var analogScale = 127.0
    @AppStorage("klepton.surreal.invertY") private var invertY = false
    @AppStorage("klepton.surreal.gripPitch") private var gripPitch = 0.0
    @AppStorage("klepton.surreal.positionFromHands") private var positionFromHands = false
    @ObservedObject private var session = KLRun.shared
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var adjustmentError = ""
    private let backend = KleptonSurrealBackend.shared

    /// False where a page puts the controls into a box of its own (Alyx's "Controls").
    var boxed = true

    var body: some View {
        // AVP Play's "Surreal Touch support" off: only standard controllers, nothing to choose.
        if !KleptonControllerMode.available {
            EmptyView()
        } else if boxed {
            GroupBox(klT("Controllers", "Controller")) { controls }
        } else {
            controls
        }
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker(klT("Controller Type", "Controllertyp"), selection: $mode) {
                Text(klT("Standard Controllers", "Standard-Controller")).tag(KleptonControllerMode.standard)
                Text("Surreal Touch").tag(KleptonControllerMode.surreal)
            }
            .disabled(session.started || session.adjustingControllers)
            .onChange(of: mode) { _, value in
                value.save()
                backend.setEnabled(value == .surreal && !KleptonSurrealSDK.enabled)
            }
            Text(klT("Choose before starting. To change after playing, quit and reopen this game. Your choice is kept for this game.",
                     "Vor dem Start wählen. Nach dem Spielen das Spiel beenden und erneut öffnen. Die Wahl wird für dieses Spiel gespeichert."))
                .font(.callout).foregroundStyle(.secondary)
            if mode == .surreal, KleptonSurrealSDK.enabled {
                // KleptonSurrealSDK: nothing below this branch is used, but the button settings.
                Text(klT("Surreal Interactive's own software connects the controllers when the game starts and aligns them by itself. They only need to be paired in Settings → Bluetooth and turned on; there is nothing to pair or calibrate here.",
                         "Die Software von Surreal Interactive verbindet die Controller beim Spielstart und richtet sie selbst aus. Sie müssen nur in den Einstellungen unter Bluetooth gekoppelt und eingeschaltet sein; hier gibt es nichts zu koppeln oder zu kalibrieren."))
                    .font(.callout).foregroundStyle(.secondary)
                DisclosureGroup(klT("Advanced input settings", "Erweiterte Eingabeeinstellungen")) {
                    VStack(alignment: .leading) { buttonSettings }
                        .onChange(of: analogScale) { _, _ in applySettings() }
                        .onChange(of: invertY) { _, _ in applySettings() }
                }
            } else if mode == .surreal {
                Toggle(klT("Position from hand tracking", "Position aus Handtracking"), isOn: $positionFromHands)
                    .disabled(session.started || session.adjustingControllers)
                    .onChange(of: positionFromHands) { _, _ in applySettings() }
                Text(klT("Optional, off by default. Use each real hand for position; Surreal Touch supplies rotation and buttons. Choose before Start. If a hand leaves view, position freezes for 0.15 seconds, then the controller hides and inputs release until the hand returns.",
                         "Optional, standardmäßig aus. Die echte Hand liefert die Position; Surreal Touch liefert Drehung und Tasten. Vor Start wählen. Verlässt eine Hand das Sichtfeld, bleibt die Position 0,15 Sekunden stehen. Danach wird der Controller ausgeblendet und die Eingabe freigegeben, bis die Hand zurückkehrt."))
                    .font(.callout).foregroundStyle(.secondary)
                TimelineView(.periodic(from: .now, by: 0.2)) { _ in
                    let state = backend.snapshot()
                    VStack(alignment: .leading, spacing: 12) {
                        if positionFromHands {
                            HStack {
                                Button(klT("Adjust direction (+ / −)", "Richtung anpassen (+ / −)")) {
                                    Task {
                                        session.adjustingControllers = true
                                        let result = await openSpace(id: SurrealAimGuide.spaceID)
                                        if case .opened = result {
                                            adjustmentError = ""
                                            dismissWindow(id: SurrealAimGuide.launcherID)
                                        }
                                        else {
                                            session.adjustingControllers = false
                                            adjustmentError = klT("Direction adjustment did not open. Try again before Start.", "Richtungsanpassung nicht geöffnet. Vor Start erneut versuchen.")
                                        }
                                    }
                                }.disabled(session.started || session.adjustingControllers || !state.hands.allSatisfy(\.connected))

                            }
                            Text(klT("Optional: adjust each controller's direction with + / − and a live line. No pose-matching wizard. Leave all angles at 0° if the default feels right.",
                                     "Optional: Jede Controller-Richtung mit + / − und einer Live-Linie anpassen. Kein Posen-Assistent. Alle Winkel auf 0° lassen, wenn die Grundeinstellung passt."))
                                .font(.callout).foregroundStyle(.secondary)
                            if !adjustmentError.isEmpty { Text(adjustmentError).foregroundStyle(.orange) }
                        }
                        Text(state.bluetooth)
                            .foregroundStyle(state.simulated ? Color.orange : Color.primary)
                        HStack {
                            Button(state.scanning ? klT("Discovering…", "Suche läuft…") : klT("Discover controllers", "Controller suchen")) { backend.discover() }
                                .disabled(state.scanning)
                            Button(klT("Reconnect paired controllers", "Gekoppelte Controller verbinden")) { backend.reconnect() }
                        }
                        ForEach(SurrealHand.allCases, id: \.rawValue) { hand in
                            let h = state.hands[hand.rawValue]
                            VStack(alignment: .leading, spacing: 6) {
                                Text(hand == .left ? klT("Left controller", "Linker Controller") : klT("Right controller", "Rechter Controller")).font(.headline)
                                Text(h.status)
                                if let battery = h.battery { Text(klT("Battery: \(battery)%", "Akku: \(battery)%")) }
                                if h.connectionRequested || h.connected {
                                    HStack {
                                        Button(h.connected ? klT("Disconnect", "Trennen") : klT("Cancel connection", "Verbindung abbrechen")) { backend.disconnect(hand) }
                                        if h.connected {
                                            Button(klT("Vibration test", "Vibration testen")) { backend.vibrate(hand: hand.rawValue, amplitude: 0.5, duration: 0.1) }
                                        }
                                    }
                                }
                                if h.connected {
                                    Text(String(format: "Trigger %.2f  Grip %.2f  Stick %.2f, %.2f  Buttons 0x%08X",
                                                h.input.indexTrigger, h.input.handTrigger, h.input.stick.x, h.input.stick.y, h.input.buttons))
                                        .font(.system(.caption, design: .monospaced))
                                    if !state.lastHaptic[hand.rawValue].isEmpty { Text(state.lastHaptic[hand.rawValue]).font(.caption) }
                                    if let pose = h.pose {
                                        Text(String(format: "Device position %.2f, %.2f, %.2f · confidence %.2f",
                                                    pose.transform.columns.3.x, pose.transform.columns.3.y, pose.transform.columns.3.z, pose.confidence))
                                            .font(.system(.caption, design: .monospaced))
                                    }
                                }
                                ForEach(state.devices.filter { $0.hand == hand }) { device in
                                    Button(klT("Connect \(device.name)", "\(device.name) verbinden")) { backend.connect(device.id) }
                                        .disabled(h.connected || h.connectionRequested)
                                }
                                if !h.connected && !h.connectionRequested && !state.devices.contains(where: { $0.hand == hand }) {
                                    Text(klT("Not connected. Turn on this controller, then choose Discover controllers.", "Nicht verbunden. Diesen Controller einschalten und Controller suchen wählen."))
                                        .font(.callout).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
                Text(positionFromHands
                     ? klT("After Start, look straight ahead. Hold both controllers pointing straight forward and level, with sticks facing up,  Keep hands visible and hold each grip still for half a second, then release. This sets forward once; subsequent hand and head rotation cannot steer the controllers. Repeat after Recalibrate or reconnect.",
                           "Nach Start geradeaus schauen. Beide Controller waagerecht nach vorn richten, Sticks nach oben. Hände sichtbar halten und jede Grifftaste eine halbe Sekunde ruhig drücken, dann loslassen. Danach beeinflussen Hand- und Kopfdrehung die Controller nicht. Nach Neu kalibrieren oder erneutem Verbinden wiederholen.")
                     : klT("After Start, hold both controllers still with hands visible and squeeze each grip to calibrate. Repeat after reconnecting. Missing controllers stay inactive; hand gestures and other gamepads do not press game buttons in this mode.",
                           "Nach Start beide Controller ruhig und die Hände sichtbar halten, dann jede Grifftaste drücken zum Kalibrieren. Nach erneutem Verbinden wiederholen. Fehlende Controller bleiben inaktiv; Handgesten und andere Gamepads lösen in diesem Modus keine Spieltasten aus."))
                    .font(.callout).foregroundStyle(.secondary)
                DisclosureGroup(klT("Advanced input settings", "Erweiterte Eingabeeinstellungen")) {
                    VStack(alignment: .leading) {
                        buttonSettings
                        if !positionFromHands {
                        Text(klT("Grip pitch: \(Int(gripPitch))°", "Griffneigung: \(Int(gripPitch))°"))
                        Slider(value: $gripPitch, in: -90...90, step: 1)
                        Text(klT("This legacy pitch applies only to native positioning. Use Adjust direction for the optional line adjustments.", "Diese Neigung gilt nur bei nativer Positionierung. Für die optionale Linienanpassung Richtung anpassen verwenden."))
                            .font(.caption).foregroundStyle(.secondary)
                        }
                        Button(klT("Reset forward alignment", "Vorwärtsausrichtung zurücksetzen")) { backend.recalibrate() }
                    }
                    .onChange(of: analogScale) { _, _ in applySettings() }
                    .onChange(of: invertY) { _, _ in applySettings() }
                    .onChange(of: gripPitch) { _, _ in applySettings() }
                    .disabled(session.adjustingControllers)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .onAppear {
            applySettings()
            // With Surreal Interactive's SDK the game connects the controllers itself; the
            // launcher must not hold them first.
            if !session.started { backend.setEnabled(mode == .surreal && !KleptonSurrealSDK.enabled) }
        }
    }
    /// The two settings that apply to the buttons whichever source reads them.
    @ViewBuilder private var buttonSettings: some View {
        Picker(klT("Trigger / grip full scale", "Trigger-/Griff-Maximalwert"), selection: $analogScale) {
            Text("127").tag(127.0); Text("255").tag(255.0)
        }
        Toggle(klT("Invert stick Y", "Stick-Y umkehren"), isOn: $invertY)
    }
    private func applySettings() {
        backend.setSettings(.stored())
    }
}
