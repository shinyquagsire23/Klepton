// "Max resolution per eye" (KleptonEyeMax.swift): the options the start window
// offers, the choice stored per game (a fresh value over the same store stands
// in for a new start of the app), the warning above the title's cap, and the
// old Test switch KL_OVRP_EYE_MAX=4608 taken over once. Run on the Mac.
// `make maxeyeswift`, in `make check`.
import Foundation

@main
struct EyeMaxCheck {
    static var failed = 0
    static var suites: [String] = []

    /// A store of its own per case, so nothing reaches the Mac's own defaults.
    static func freshStore() -> (UserDefaults, String) {
        let name = "klepton.t_maxeye.\(UUID().uuidString)"
        suites.append(name)
        return (UserDefaults(suiteName: name)!, name)
    }

    static func expect<T: Equatable>(_ what: String, _ got: T, _ want: T) {
        print("  \(got == want ? "ok  " : "FAIL") \(what) -> \(got)\(got == want ? "" : " (want \(want))")")
        if got != want { failed += 1 }
    }

    static func main() {
        print("=== Max resolution per eye: options, storage, Test switch taken over ===")

        expect("options", KleptonEyeMax.options, [0, 2048, 2560, 3072, 3584, 4000, 4096, 4608, 5120, -1])

        // Never chosen: the title's cap, nothing stored.
        do {
            let (store, _) = freshStore()
            expect("never chosen", KleptonEyeMax(defaults: store).choice, KleptonEyeMax.byTitle)
        }
        // A choice survives a new start; Default removes it again.
        for v in [2560, 4608, KleptonEyeMax.native] {
            let (store, name) = freshStore()
            KleptonEyeMax(defaults: store).choice = v
            store.synchronize()
            let after = KleptonEyeMax(defaults: UserDefaults(suiteName: name)!)
            expect("chose \(v), new start", after.choice, v)
            after.choice = KleptonEyeMax.byTitle
            expect("chose \(v), then Default: stored", store.object(forKey: KleptonEyeMax.key) == nil, true)
        }
        // Something not offered (another build's value) reads as Default.
        do {
            let (store, _) = freshStore()
            store.set(3333, forKey: KleptonEyeMax.key)
            expect("stored 3333", KleptonEyeMax(defaults: store).choice, KleptonEyeMax.byTitle)
        }

        // The warning: above the title's cap, and Native always.
        expect("4608 over 4000", KleptonEyeMax.aboveTitle(4608, titleCap: 4000), true)
        expect("4000 over 4000", KleptonEyeMax.aboveTitle(4000, titleCap: 4000), false)
        expect("Default over 3072", KleptonEyeMax.aboveTitle(0, titleCap: 3072), false)
        expect("2048 over 3072", KleptonEyeMax.aboveTitle(2048, titleCap: 3072), false)
        expect("Native over 4640", KleptonEyeMax.aboveTitle(KleptonEyeMax.native, titleCap: 4640), true)

        // The old Test switch.
        let none: Set<String> = [], named: Set<String> = ["KL_XR_PREDICT", "KL_OVRP_EYE_MAX"]
        func migrated(stored: Bool?, install: Set<String>, choice: Int? = nil) -> (Int, Bool, Bool) {
            let (store, _) = freshStore()
            if let stored { store.set(stored, forKey: KleptonEyeMax.oldSwitchKey) }
            if let choice { store.set(choice, forKey: KleptonEyeMax.key) }
            let m = KleptonEyeMax(defaults: store)
            let did = m.migrateTestSwitch(onFromInstall: install)
            return (m.choice, did, store.object(forKey: KleptonEyeMax.oldSwitchKey) == nil)
        }
        func expectMigrated(_ what: String, _ got: (Int, Bool, Bool), _ choice: Int, _ did: Bool) {
            expect(what + ": choice", got.0, choice)
            expect(what + ": set 4608", got.1, did)
            expect(what + ": old state cleared", got.2, true)
        }
        expectMigrated("switch on", migrated(stored: true, install: none), 4608, true)
        expectMigrated("switch turned off", migrated(stored: false, install: named), 0, false)
        expectMigrated("never touched", migrated(stored: nil, install: none), 0, false)
        expectMigrated("on from the installation", migrated(stored: nil, install: named), 4608, true)
        expectMigrated("switch on, a choice stored", migrated(stored: true, install: none, choice: 2560), 2560, false)
        // Once: a choice made afterwards stays, even with the installation still naming it.
        do {
            let (store, name) = freshStore()
            store.set(true, forKey: KleptonEyeMax.oldSwitchKey)
            KleptonEyeMax(defaults: store).migrateTestSwitch(onFromInstall: named)
            KleptonEyeMax(defaults: store).choice = KleptonEyeMax.byTitle
            store.synchronize()
            let again = KleptonEyeMax(defaults: UserDefaults(suiteName: name)!)
            expect("once: second start", again.migrateTestSwitch(onFromInstall: named), false)
            expect("once: Default chosen after it stays", again.choice, KleptonEyeMax.byTitle)
        }

        for name in suites { UserDefaults.standard.removePersistentDomain(forName: name) }
        if failed > 0 { print("maxeye: FAIL (\(failed))"); exit(1) }
        print("maxeye: PASS")
    }
}
