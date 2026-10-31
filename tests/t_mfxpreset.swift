// The MetalFX switch and "Resolution" (KleptonMetalFXPreset.swift): turned on at
// 100 % it sets 75 % and marks that it did; turned off it gives 100 % back only
// while that mark stands (no move of the slider since). A resolution the player
// chose is left alone either way. The mark lives in UserDefaults, so a fresh
// value over the same store stands in for a new start of the app. Also what a
// Picture box shows of the two (KleptonPictureControls). Run on the Mac.
// `make mfxpreset`, in `make check`.
import Foundation

@main
struct MetalFXPresetCheck {
    static var failed = 0
    static var suites: [String] = []

    /// A store of its own per case, so nothing reaches the Mac's own defaults.
    static func freshStore() -> (UserDefaults, String) {
        let name = "klepton.t_mfxpreset.\(UUID().uuidString)"
        suites.append(name)
        return (UserDefaults(suiteName: name)!, name)
    }

    static func show(_ v: Double?) -> String { v.map { String(format: "%.0f%%", $0 * 100) } ?? "unchanged" }

    static func expect(_ what: String, _ got: Double?, _ want: Double?) {
        print("  \(got == want ? "ok  " : "FAIL") \(what) -> \(show(got))\(got == want ? "" : " (want \(show(want)))")")
        if got != want { failed += 1 }
    }

    static func expectMark(_ what: String, _ preset: KleptonMetalFXPreset, _ want: Bool) {
        let got = preset.lowered
        print("  \(got == want ? "ok  " : "FAIL") \(what): mark \(got ? "set" : "clear")")
        if got != want { failed += 1 }
    }

    static func main() {
        print("=== MetalFX switch: resolution preset ===")

        // On: only at 100 %.
        for (from, want) in [(1.0, 0.75), (1.0 - 1e-9, 0.75), (0.95, nil), (0.75, nil), (0.5, nil), (0.25, nil)] as [(Double, Double?)] {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            expect(String(format: "on at %.0f%%", from * 100), p.switched(on: true, from: from), want)
            expectMark(String(format: "on at %.0f%%", from * 100), p, want != nil)
        }
        // Off without anything the switch did.
        for from in [1.0, 0.75, 0.5] {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            expect(String(format: "off at %.0f%%, never lowered", from * 100), p.switched(on: false, from: from), nil)
        }

        // (a) Lowered by the switch, then off: 100 % again, the mark is gone.
        do {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            let r = p.switched(on: true, from: 1.0)!
            expect("(a) lowered by the switch, then off", p.switched(on: false, from: r), 1.0)
            expectMark("(a) after off", p, false)
        }
        // (b) Lowered by the switch, the slider moved (even back to 75 %), then off: unchanged.
        for moved in [0.6, 0.75, 1.0] {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            _ = p.switched(on: true, from: 1.0)
            p.movedByHand()
            expectMark(String(format: "(b) slider moved to %.0f%%", moved * 100), p, false)
            expect(String(format: "(b) lowered, slider moved to %.0f%%, then off", moved * 100),
                   p.switched(on: false, from: moved), nil)
        }
        // (c) 75 % chosen by hand, then on and off: unchanged, never marked.
        do {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            p.movedByHand()
            expect("(c) manual 75%, then on", p.switched(on: true, from: 0.75), nil)
            expectMark("(c) manual 75%, on", p, false)
            expect("(c) manual 75%, then off", p.switched(on: false, from: 0.75), nil)
        }
        // (d) The mark survives a new start: a fresh value over the same store
        // (UserDefaults written through and read back by a new instance).
        do {
            let (store, name) = freshStore()
            let before = KleptonMetalFXPreset(defaults: store)
            _ = before.switched(on: true, from: 1.0)
            store.synchronize()
            let after = KleptonMetalFXPreset(defaults: UserDefaults(suiteName: name)!)
            expectMark("(d) after a new start", after, true)
            expect("(d) lowered, new start, then off", after.switched(on: false, from: 0.75), 1.0)
        }
        // (e) Marked, but the stored resolution is no longer 75 % (a target's
        // picture reset dropped it): the switch does not claim it.
        do {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            _ = p.switched(on: true, from: 1.0)
            expect("(e) lowered, resolution reset elsewhere to 90%, then off", p.switched(on: false, from: 0.9), nil)
        }
        // (f) The setter every Resolution slider writes through (byHand: the
        // Picture box's, Alyx's beside the switch, Advanced's): it stores the
        // value into the one setting and counts as a move, even back to 75 %.
        for moved in [0.75, 0.6] {
            let p = KleptonMetalFXPreset(defaults: freshStore().0)
            var scale = 1.0
            let slider = p.byHand { scale = $0 }
            if let r = p.switched(on: true, from: scale) { scale = r }
            slider(moved)
            expect(String(format: "(f) slider beside the switch moved to %.0f%%: stored", moved * 100), scale, moved)
            expectMark(String(format: "(f) slider beside the switch moved to %.0f%%", moved * 100), p, false)
            expect(String(format: "(f) lowered, slider beside the switch moved to %.0f%%, then off", moved * 100),
                   p.switched(on: false, from: scale), nil)
        }
        // (g) The Picture box of every page (KleptonPictureControls.primary, what
        // PicturePrimary shows): Resolution always, MetalFX wherever the device
        // has the scaler, neither locked. The rule has no other input — not the
        // game, not Alyx's Graphics preset — so no preset hides or locks them.
        for scaler in [true, false] {
            let c = KleptonPictureControls.primary(deviceHasScaler: scaler)
            let want = KleptonPictureControls(resolution: true, metalFX: scaler, adjustable: true)
            let ok = c == want
            print("  \(ok ? "ok  " : "FAIL") (g) Picture box, scaler \(scaler ? "present" : "absent"): "
                  + "Resolution \(c.resolution ? "shown" : "hidden"), MetalFX \(c.metalFX ? "shown" : "hidden"), "
                  + (c.adjustable ? "adjustable" : "locked"))
            if !ok { failed += 1 }
        }

        // (h) With KL_MFX_MR_PAUSE=1, mixed reality pauses MetalFX and its end
        // resumes it (KleptonMetalFXPause), frame by frame, without touching
        // what is stored: the switch lowered the resolution before the pause,
        // and the store is the same after pause and resume, so off still gives
        // 100 % back.
        do {
            let (store, _) = freshStore()
            let p = KleptonMetalFXPreset(defaults: store)
            let scale = p.switched(on: true, from: 1.0)!
            store.set(true, forKey: "klepton.metalfxUpscale")
            let before = NSDictionary(dictionary: store.dictionaryRepresentation().filter { $0.key.hasPrefix("klepton.") })
            var pause = KleptonMetalFXPause(pauseInMR: true)
            typealias Step = (what: String, chosen: Bool, mr: Bool, upscale: Bool, change: KleptonMetalFXPause.Change?)
            let steps: [Step] = [
                ("on, no MR", true, false, true, nil),
                ("MR starts", true, true, false, .paused),
                ("MR goes on", true, true, false, nil),
                ("MR ends", true, false, true, .resumed),
                ("MR again", true, true, false, .paused),
                ("switched off during MR", false, true, false, nil),
                ("off, MR ends", false, false, false, nil),
                ("off, MR starts", false, true, false, nil),
                ("switched on during MR", true, true, false, .paused),
                ("MR ends", true, false, true, .resumed),
            ]
            for s in steps {
                let r = pause.frame(chosen: s.chosen, mixedReality: s.mr)
                let ok = r.upscale == s.upscale && !r.restoreAlpha && r.change == s.change
                print("  \(ok ? "ok  " : "FAIL") (h) \(s.what): \(r.upscale ? "upscales" : "linear")"
                      + (r.change.map { ", \($0)" } ?? ""))
                if !ok { failed += 1 }
            }
            let after = NSDictionary(dictionary: store.dictionaryRepresentation().filter { $0.key.hasPrefix("klepton.") })
            let same = before.isEqual(after) && before.count == 2  // the switch and the mark
            print("  \(same ? "ok  " : "FAIL") (h) pause and resume leave the stored switch, resolution and mark alone")
            if !same { failed += 1 }
            expectMark("(h) after pause and resume", p, true)
            expect("(h) lowered, paused, resumed, then off", p.switched(on: false, from: scale), 1.0)
        }

        // (i) By default mixed reality keeps upscaling and puts the alpha back
        // (KleptonMetalFXAlpha) while it lasts, and nothing stored moves either.
        do {
            let (store, _) = freshStore()
            let p = KleptonMetalFXPreset(defaults: store)
            let scale = p.switched(on: true, from: 1.0)!
            store.set(true, forKey: "klepton.metalfxUpscale")
            let before = NSDictionary(dictionary: store.dictionaryRepresentation().filter { $0.key.hasPrefix("klepton.") })
            var mr = KleptonMetalFXPause()
            typealias Step = (what: String, chosen: Bool, mr: Bool, upscale: Bool, alpha: Bool,
                              change: KleptonMetalFXPause.Change?)
            let steps: [Step] = [
                ("on, no MR", true, false, true, false, nil),
                ("MR starts", true, true, true, true, .alphaOn),
                ("MR goes on", true, true, true, true, nil),
                ("MR ends", true, false, true, false, .alphaOff),
                ("MR again", true, true, true, true, .alphaOn),
                ("switched off during MR", false, true, false, false, nil),
                ("off, MR ends", false, false, false, false, nil),
                ("off, MR starts", false, true, false, false, nil),
                ("switched on during MR", true, true, true, true, .alphaOn),
                ("MR ends", true, false, true, false, .alphaOff),
            ]
            for s in steps {
                let r = mr.frame(chosen: s.chosen, mixedReality: s.mr)
                let ok = r.upscale == s.upscale && r.restoreAlpha == s.alpha && r.change == s.change
                    && !mr.paused
                print("  \(ok ? "ok  " : "FAIL") (i) \(s.what): \(r.upscale ? "upscales" : "linear")"
                      + (r.restoreAlpha ? " + alpha" : "") + (r.change.map { ", \($0)" } ?? ""))
                if !ok { failed += 1 }
            }
            let after = NSDictionary(dictionary: store.dictionaryRepresentation().filter { $0.key.hasPrefix("klepton.") })
            let same = before.isEqual(after) && before.count == 2
            print("  \(same ? "ok  " : "FAIL") (i) mixed reality with the alpha pass leaves the stored switch, resolution and mark alone")
            if !same { failed += 1 }
            expectMark("(i) after mixed reality", p, true)
            expect("(i) lowered, mixed reality, then off", p.switched(on: false, from: scale), 1.0)
        }

        for name in suites { UserDefaults.standard.removePersistentDomain(forName: name) }
        if failed > 0 { print("mfxpreset: FAIL (\(failed))"); exit(1) }
        print("mfxpreset: PASS")
    }
}
