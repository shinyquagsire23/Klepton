import Foundation

// What the MetalFX switch does to "Resolution" (KleptonMetalFX.setFromSettings,
// tapToggle). At 100 % there is nothing to upscale (KleptonMetalFX.canUpscale),
// so turning it on there sets the slider to 75 %, the start AVP Play presets for
// a MetalFX install (MetalFXResolution there), and remembers that the switch
// lowered it. Turning it off gives the 100 % back only then: the switch lowered
// it itself and the slider has not been moved by hand since (any move clears the
// mark). A resolution the player chose stays as it is. The mark is stored with
// the game's settings, so it holds across starts. Plain Foundation, so the rule
// runs on the Mac: tests/t_mfxpreset.swift, `make mfxpreset`.
struct KleptonMetalFXPreset {
    /// The resolution a game is set to when MetalFX is turned on at 100 %.
    static let renderScale = 0.75
    /// Set while the resolution is the one the switch lowered it to.
    static let loweredKey = "klepton.metalfxLoweredResolution"

    let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    /// Whether the switch lowered the resolution and nobody has moved it since.
    var lowered: Bool { defaults.bool(forKey: Self.loweredKey) }

    /// The resolution after the switch was turned, or nil to leave it as it is.
    /// `current` is the slider's value (KleptonRendering.renderScale, a fraction
    /// in steps of 0.05; 1.0 is 100 %). Keeps the mark up to date.
    func switched(on: Bool, from current: Double) -> Double? {
        if on {
            guard current >= 1.0 - 0.025 else { return nil }
            defaults.set(true, forKey: Self.loweredKey)
            return Self.renderScale
        }
        // The value is checked too: a stored resolution a target dropped since
        // (KleptonRendering's picture reset) is no longer the switch's.
        let restore = lowered && abs(current - Self.renderScale) < 0.025
        defaults.removeObject(forKey: Self.loweredKey)
        return restore ? 1.0 : nil
    }

    /// The player moved the slider: the resolution is theirs from now on.
    func movedByHand() {
        defaults.removeObject(forKey: Self.loweredKey)
    }

    /// What every Resolution slider writes through (KleptonRendering
    /// .renderScaleByHand): stores the value and counts as a move of the slider.
    func byHand(_ store: @escaping (Double) -> Void) -> (Double) -> Void {
        { value in store(value); self.movedByHand() }
    }
}

// What the Picture box of a settings page shows of the two (PicturePrimary): in
// every game, Alyx included, Resolution always and the MetalFX switch wherever
// the device has the scaler, both adjustable. Nothing else is asked: not the
// game, not Alyx's Graphics preset (AlyxPanel, a box of its own), so no preset
// hides or locks them. Alyx's page had both only under Advanced before.
// t_mfxpreset (g).
struct KleptonPictureControls: Equatable {
    var resolution: Bool
    var metalFX: Bool
    var adjustable: Bool

    static func primary(deviceHasScaler: Bool) -> Self {
        Self(resolution: true, metalFX: deviceHasScaler, adjustable: true)
    }
}

// MetalFX while the guest is in its own mixed reality (kl_ovrp_passthrough_live).
// The room shows where the eye picture's alpha says so (kl_reproject.c,
// kl_eye_sample_alpha and the discard after it), and Apple's spatial scaler
// writes alpha 1 into every pixel of its output, whatever came in (measured on
// a Mac, every format the compositor hands it): the upscaled copy alone would
// cover the room with the black of the game's empty pixels. So while
// passthrough is live the compositor either upscales and puts the source's
// alpha back into the copy (KleptonMetalFXAlpha, the default), or — with
// KL_MFX_MR_PAUSE=1, `pauseInMR` — keeps the linear path and goes back to
// upscaling once passthrough ends. Only the compositor's per-frame choice: the
// stored switch (klepton.metalfxUpscale), the resolution and the mark the
// switch keeps (KleptonMetalFXPreset) are not touched, so neither way ever
// lowers or restores anything. t_mfxpreset (h), (i).
struct KleptonMetalFXPause {
    enum Change: Equatable { case paused, resumed, alphaOn, alphaOff }

    /// KL_MFX_MR_PAUSE: the linear path during mixed reality instead of the
    /// alpha pass.
    let pauseInMR: Bool
    /// The switch is on and the guest shows the room.
    private(set) var inMR = false

    init(pauseInMR: Bool = false) { self.pauseInMR = pauseInMR }

    var paused: Bool { inMR && pauseInMR }

    /// One frame: whether to upscale, whether to put the alpha back after the
    /// scaler, and whether mixed reality began or ended with it (said once in
    /// the log). `chosen` is the stored switch (KleptonMetalFX.isOn). A switch
    /// turned off during mixed reality ends it without a "resumed".
    mutating func frame(chosen: Bool, mixedReality: Bool)
        -> (upscale: Bool, restoreAlpha: Bool, change: Change?) {
        let now = chosen && mixedReality
        let was = inMR
        inMR = now
        let r = (upscale: chosen && !(now && pauseInMR), restoreAlpha: now && !pauseInMR)
        if now == was { return (r.upscale, r.restoreAlpha, nil) }
        if now { return (r.upscale, r.restoreAlpha, pauseInMR ? .paused : .alphaOn) }
        return (r.upscale, r.restoreAlpha, chosen ? (pauseInMR ? .resumed : .alphaOff) : nil)
    }
}
