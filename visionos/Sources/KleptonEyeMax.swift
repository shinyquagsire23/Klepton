import Foundation

// "Max resolution per eye" under Advanced in the start window: the cap on the
// longer axis of the eye picture the game is told (kl_ovrp_set_eye_max_choice).
// Default is the title's own cap (kl_ovrp_title_eye_cap: 3072, or what was
// tuned for it, 4000 for Gun Club VR); Native is no cap, the whole size the
// display asks for (about 4633x3717 on a Vision Pro). "Resolution" and MetalFX
// stay fractions of whatever this makes, since the render scale is applied
// after the cap. KL_OVRP_EYE_MAX on the launch line wins over it. Stored with
// the game's settings, so each game has its own; read at the start of a game.
// Plain Foundation, so the rules run on the Mac: tests/t_maxeye.swift, `make
// maxeyeswift`.
struct KleptonEyeMax {
    static let key = "klepton.eyeMax"
    /// The title's own cap: nothing stored.
    static let byTitle = 0
    /// No cap (KL_OVRP_EYE_NATIVE in kl_ovrp.h).
    static let native = -1
    /// What the picker offers, in its order.
    static let options = [byTitle, 2048, 2560, 3072, 3584, 4000, 4096, 4608, 5120, native]

    /// The Test switch this replaces: KL_OVRP_EYE_MAX=4608 under Advanced ›
    /// Test (KleptonTestSwitches' key for it), and what it set.
    static let oldSwitchKey = "klepton.test.KL_OVRP_EYE_MAX"
    static let oldSwitchEnv = "KL_OVRP_EYE_MAX"
    static let oldSwitchValue = 4608
    /// Set once the old switch was looked at, so an installation that still
    /// names it (KLTestOn) sets 4608 the first time only, not over a later choice.
    static let migratedKey = "klepton.eyeMax.migrated"

    let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    /// The stored choice; anything not offered (a later build's value) is the
    /// title's cap.
    var choice: Int {
        get {
            let v = defaults.integer(forKey: Self.key)
            return Self.options.contains(v) ? v : Self.byTitle
        }
        nonmutating set {
            if newValue == Self.byTitle || !Self.options.contains(newValue) {
                defaults.removeObject(forKey: Self.key)
            } else {
                defaults.set(newValue, forKey: Self.key)
            }
        }
    }

    /// Whether a choice goes past what the title was tested at: worth a word
    /// under the picker. Native is past any cap.
    static func aboveTitle(_ choice: Int, titleCap: Int) -> Bool {
        guard titleCap > 0 else { return false }
        return choice == native || choice > titleCap
    }

    /// The old Test switch, taken over once: on for this game (chosen in the
    /// window, or named by the installation and not turned off) becomes a
    /// choice of 4608, unless a choice is stored already; its own state is
    /// dropped either way. Returns whether it set 4608.
    @discardableResult
    func migrateTestSwitch(onFromInstall: Set<String>) -> Bool {
        guard !defaults.bool(forKey: Self.migratedKey) else { return false }
        defaults.set(true, forKey: Self.migratedKey)
        let stored = defaults.object(forKey: Self.oldSwitchKey) as? Bool
        defaults.removeObject(forKey: Self.oldSwitchKey)
        guard stored ?? onFromInstall.contains(Self.oldSwitchEnv),
              defaults.object(forKey: Self.key) == nil else { return false }
        choice = Self.oldSwitchValue
        return true
    }
}
