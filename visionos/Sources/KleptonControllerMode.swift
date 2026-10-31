import Foundation

/// Shared wire values with AVPPlayCore.ControllerMode. Local choices survive app updates.
enum KleptonControllerMode: String, CaseIterable {
    case standard, surreal
    static let preferenceKey = "klepton.controllerMode"
    /// Whether this build offers Surreal Touch at all: AVP Play's "Surreal Touch support" off
    /// (KLEPTON_SURREAL_SUPPORT=0) builds it with KLSurrealAvailable NO, and the game then knows only
    /// standard controllers – no controller type to choose, no Surreal panel.
    static let available = surrealAvailable()
    static func surrealAvailable(_ value: String? = Bundle.main.object(forInfoDictionaryKey: "KLSurrealAvailable") as? String) -> Bool {
        value != "NO"
    }
    /// Standard wherever Surreal Touch is not available; a Surreal choice an earlier build stored is
    /// put back then, so the views that read the stored value see the same.
    static func selected(defaults: UserDefaults = .standard,
                         bundleDefault: String? = Bundle.main.object(forInfoDictionaryKey: "KLControllerModeDefault") as? String,
                         available: Bool = Self.available) -> Self {
        guard available else {
            if defaults.string(forKey: preferenceKey) == surreal.rawValue { standard.save(defaults: defaults) }
            return .standard
        }
        if let local = defaults.string(forKey: preferenceKey), let mode = Self(rawValue: local) { return mode }
        return bundleDefault.flatMap(Self.init(rawValue:)) ?? .standard
    }
    func save(defaults: UserDefaults = .standard) { defaults.set(rawValue, forKey: Self.preferenceKey) }
}
