import Foundation

/// The two languages the game's own window speaks: English, and German where
/// the device's first preferred language is German.
///
/// Both versions of a sentence sit where the sentence is used rather than in a
/// strings table: there are a few dozen of them, most carry a value, and a
/// table would be one more file that has to stay in step with the build
/// script. `KL_LANG=en|de` on a launch line overrides the device's choice, so
/// either language can be photographed on any simulator.
enum KLText {
    static let german: Bool = {
        if let forced = ProcessInfo.processInfo.environment["KL_LANG"] { return forced.lowercased().hasPrefix("de") }
        return Locale.preferredLanguages.first?.lowercased().hasPrefix("de") ?? false
    }()
}

func klT(_ en: String, _ de: String) -> String { KLText.german ? de : en }
