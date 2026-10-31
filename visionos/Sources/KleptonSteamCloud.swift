// Steam Cloud saves: the game's saves here and on the player's other machines
// are one set, as with the Steam client.
//
// The game app signs in to Steam itself. The player types account name and
// password into this app once and confirms in the Steam app on a phone (Steam
// Guard); the password goes to Steam, encrypted with Steam's own key for the
// account, and is not kept. What is kept is the session Steam hands back — a
// long-lived token — in this device's keychain, where only this app reads it.
// It can be withdrawn here ("Sign out") and from Steam ("Deauthorize all
// devices"). A QR code, the other way Steam offers, is no use on a headset:
// Steam accepts it only when scanned with the Steam app, and nothing can scan
// what is shown inside a headset.
//
// The sync runs at each start, before the game does, in both directions: what
// the last session saved here goes up, what was saved elsewhere since comes
// down. It is Glovebox's (SteamCloudKit/README.md): a save here goes up only
// over the exact cloud version it was last in step with; where both sides
// changed, or there is no history yet, the cloud's copy wins and the copy here
// is set aside in lx/.steam-cloud-backups; nothing is ever deleted on either
// side. A start does not wait for it for more than a little while, and a sync
// that fails is a start without one.
import SwiftUI
import Security

/// The Steam session, in the keychain and nowhere else. After Glovebox's
/// (launcher/App/AlyxLauncher/Keychain.swift).
private enum KLSteamKeychain {
    private static let service = (Bundle.main.bundleIdentifier ?? "klepton") + ".steam"

    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    static func save(_ creds: SteamCredentials) {
        guard let data = try? JSONEncoder().encode(creds) else { return }
        SecItemDelete(query("current") as CFDictionary)
        var q = query("current")
        q[kSecValueData as String] = data
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
        if let g = creds.guardData { setGuardData(g, for: creds.accountName) }
    }

    static func load() -> SteamCredentials? {
        var q = query("current")
        q[kSecReturnData as String] = true
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return try? JSONDecoder().decode(SteamCredentials.self, from: data)
    }

    static func clear() { SecItemDelete(query("current") as CFDictionary) }

    /// Steam Guard's "remember this device", so the next sign-in skips the code.
    static func guardData(for account: String) -> String? {
        var q = query("guard." + account.lowercased())
        q[kSecReturnData as String] = true
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private static func setGuardData(_ g: String, for account: String) {
        let key = "guard." + account.lowercased()
        SecItemDelete(query(key) as CFDictionary)
        var q = query(key)
        q[kSecValueData as String] = Data(g.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(q as CFDictionary, nil)
    }
}

@MainActor
final class KLSteamCloud: ObservableObject {
    static let shared = KLSteamCloud()
    static let enabledKey = "steam.cloud.enabled"
    private static let lastKey = "steam.cloud.last"

    enum Step: Equatable {
        case idle, working
        case approveInApp(canEnterCode: Bool)
        case enterCode(email: String?)
    }

    /// The signed-in account's name; empty when there is no session.
    @Published private(set) var accountName = ""
    @Published private(set) var step = Step.idle
    @Published private(set) var problem: String?
    /// What the last sync did, for the settings window.
    @Published private(set) var last = UserDefaults.standard.string(forKey: KLSteamCloud.lastKey) ?? ""
    /// A start is waiting for the sync.
    @Published private(set) var syncing = false

    private var session: SteamAuthSession?
    private var task: Task<Void, Never>?

    /// The game this app is, as Steam Cloud knows it; nil for a game that is not from Steam.
    nonisolated static var game: CloudGame? { klIsAlyx() ? .halfLifeAlyx : nil }
    nonisolated static var enabled: Bool { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }

    private init() { accountName = KLSteamKeychain.load()?.accountName ?? "" }

    // MARK: Sign-in

    func signIn(account: String, password: String) {
        let account = account.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !account.isEmpty, !password.isEmpty else { return }
        problem = nil
        step = .working
        task?.cancel()
        task = Task {
            let cm = CMClient()
            do {
                try await cm.connect()
                let s = try await SteamAuth.beginCredentials(cm: cm, accountName: account, password: password,
                                                             guardData: KLSteamKeychain.guardData(for: account))
                session = s
                switch s.prompt {
                case .approveInApp(let canEnterCode): step = .approveInApp(canEnterCode: canEnterCode)
                case .deviceCode: step = .enterCode(email: nil)
                case .emailCode(let hint): step = .enterCode(email: hint)
                case .qr: break
                }
                var creds = try await s.waitForApproval()
                if creds.accountName.isEmpty { creds.accountName = account }
                KLSteamKeychain.save(creds)
                accountName = creds.accountName
                NSLog("[steam] signed in; the session is in the keychain")
            } catch {
                if !Task.isCancelled { problem = Self.friendly(error) }
            }
            session = nil
            step = .idle
            await cm.logOff()
        }
    }

    func submit(code: String) {
        guard let s = session else { return }
        let code = code.trimmingCharacters(in: .whitespacesAndNewlines)
        Task {
            do { try await s.submit(code: code) } catch { problem = Self.friendly(error) }
        }
    }

    func useCodeInstead() { step = .enterCode(email: nil) }

    func cancelSignIn() {
        task?.cancel()
        session = nil
        step = .idle
    }

    func signOut() {
        task?.cancel()
        KLSteamKeychain.clear()
        accountName = ""
        problem = nil
        NSLog("[steam] signed out; the session is removed from the keychain")
    }

    /// An error as a sentence for a player. Never the raw text of a server's answer.
    private static func friendly(_ error: Error) -> String {
        if case SteamError.result(let r, _) = error {
            switch r {
            case .invalidPassword:
                return klT("Steam did not accept the account name or password.",
                           "Steam hat Kontoname oder Passwort nicht angenommen.")
            case .rateLimitExceeded:
                return klT("Steam is refusing sign-ins for the moment (too many attempts). Try again later.",
                           "Steam nimmt gerade keine Anmeldung an (zu viele Versuche). Versuch es später noch einmal.")
            case .expired:
                return klT("The sign-in took too long. Try again.", "Die Anmeldung hat zu lange gedauert. Versuch es noch einmal.")
            default:
                return klT("Steam refused the sign-in (\(r)).", "Steam hat die Anmeldung abgelehnt (\(r)).")
            }
        }
        if error is URLError || error is TransportError {
            return klT("Steam could not be reached. Is the Vision Pro online?",
                       "Steam war nicht zu erreichen. Ist die Vision Pro online?")
        }
        return klT("The sign-in did not work.", "Die Anmeldung hat nicht funktioniert.")
    }

    // MARK: Sync

    private func noteSync(_ text: String) {
        let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .short, timeStyle: .short)
        last = "\(stamp): \(text)"
        UserDefaults.standard.set(last, forKey: Self.lastKey)
    }

    private func sessionRefused() {
        KLSteamKeychain.clear()
        accountName = ""
        problem = klT("Steam no longer accepts the saved session. Sign in again.",
                      "Steam nimmt die gemerkte Sitzung nicht mehr an. Melde dich neu an.")
    }

    /// One two-way sync of this game's saves. Throws what went wrong; the caller says it.
    private nonisolated static func sync(game: CloudGame, creds: SteamCredentials,
                                         settled: @escaping @Sendable () -> Void) async throws -> CloudSync.Report {
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            throw SteamError.badData("no Documents")
        }
        let root = docs.appendingPathComponent("lx", isDirectory: true)
        let cm = CMClient()
        try await cm.connect()
        do {
            _ = try await cm.logon(accountName: creds.accountName, refreshToken: creds.refreshToken,
                                   machineName: SteamAuth.deviceName())
            let info = try await SteamApps(cm: cm).appInfo(game.appID)
            guard !info.cloudRules.isEmpty else { await cm.logOff(); return CloudSync.Report() }
            var remote = SteamCloud(cm: cm, appID: game.appID, buildID: info.buildID)
            remote.batchLedger = root.appendingPathComponent(".steam-cloud-batches.json")
            let leftOver = await remote.closeLeftOverBatches()
            if leftOver > 0 { NSLog("[steam] cloud sync: closed \(leftOver) upload batch(es) an earlier start left open") }
            // Sending goes on behind the game, and a game that is left ends it
            // where it is. How far it came is written down file by file, so
            // that a sync cut off does not look like one that never ran.
            remote.uploadStep = { done, of, what in
                NSLog("[steam] cloud sync: \(done)/\(of) \(what)")
                if done == 0 { return }               // waiting for Steam, nothing sent yet
                let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .short, timeStyle: .short)
                UserDefaults.standard.set(stamp + ": " + klT("sending saves to Steam — \(done) of \(of) so far; the rest follows at the next start",
                                                             "schickt Spielstände zu Steam – bisher \(done) von \(of); der Rest folgt beim nächsten Start"),
                                          forKey: lastKey)
            }
            var sync = CloudSync(remote: remote,
                                 rules: info.cloudRules,
                                 rootFolder: { token in game.roots[token].map { $0.isEmpty ? root : root.appendingPathComponent($0) } },
                                 stateURL: root.appendingPathComponent(".steam-cloud.json"),
                                 backupRoot: root.appendingPathComponent(".steam-cloud-backups"))
            sync.neverUpload = game.neverUpload
            sync.localSettled = settled
            let report = try await sync.run()
            await cm.logOff()
            return report
        } catch {
            await cm.logOff()
            throw error
        }
    }

    private func syncNow(game: CloudGame, creds: SteamCredentials, settled: @escaping @Sendable () -> Void) async {
        syncing = true
        defer { syncing = false }
        do {
            let r = try await Self.sync(game: game, creds: creds, settled: {
                settled()
                Task { @MainActor in KLSteamCloud.shared.syncing = false }
            })
            let parts = [r.downloaded.isEmpty ? nil : klT("\(r.downloaded.count) from Steam", "\(r.downloaded.count) von Steam geholt"),
                         r.uploaded.isEmpty ? nil : klT("\(r.uploaded.count) to Steam", "\(r.uploaded.count) zu Steam geschickt"),
                         r.backedUp.isEmpty ? nil : klT("\(r.backedUp.count) set aside", "\(r.backedUp.count) beiseitegelegt")]
                .compactMap { $0 }
            noteSync(parts.isEmpty ? klT("saves were already in step", "Spielstände waren schon gleich")
                                   : parts.joined(separator: ", "))
            NSLog("[steam] cloud sync: \(r.downloaded.count) down, \(r.uploaded.count) up, \(r.backedUp.count) set aside, "
                  + "\(r.relocated.count) moved to a free slot, \(r.unchanged) unchanged")
        } catch SteamError.result(let r, _) where r == .invalidPassword || r == .accessDenied || r == .expired {
            sessionRefused()
            noteSync(klT("not synced — sign in again", "nicht abgeglichen – bitte neu anmelden"))
            NSLog("[steam] cloud sync: Steam refused the saved session (\(r)); it is removed")
        } catch is CancellationError {
            noteSync(klT("not synced — it took too long", "nicht abgeglichen – es hat zu lange gedauert"))
            NSLog("[steam] cloud sync: given up after the wait a start allows")
        } catch {
            // What went wrong, in the line the settings show: the first sync on a
            // headset failed and the reason was only in a system log nobody can
            // read off the device. Never a URL (a cloud file's carries a signed
            // query) and never anything of the session.
            let why: String
            if case SteamError.result(let r, _) = error, r.rawValue == 108 {
                // Too many pending: an upload an earlier start did not finish is
                // still open at Steam, which lets it lapse after some minutes.
                why = klT("Steam is still closing an earlier upload; the saves go up at the next start",
                          "Steam schließt noch einen früheren Upload ab; die Spielstände gehen beim nächsten Start hoch")
            }
            else if let e = error as? URLError { why = "URLError \(e.code.rawValue)" }
            else if let e = error as? SteamError { why = String(String(describing: e).prefix(140)) }
            else if let e = error as? TransportError { why = String(String(describing: e).prefix(140)) }
            else { why = String(describing: type(of: error)) }
            noteSync(klT("not synced — \(why)", "nicht abgeglichen – \(why)"))
            NSLog("[steam] cloud sync failed: \(why)")
        }
    }

    /// The sync a start waits for: called on the boot thread before the game's
    /// files are touched. It returns when nothing more will be written here —
    /// the downloads are in place — or after `wait` seconds, whichever is
    /// first: a start that hangs on a network is worse than a start without
    /// the newest save.
    ///
    /// Sending what was saved here is not waited for. The first sync of a
    /// headset with a few weeks of saves had forty files and sixty megabytes
    /// to send; cut off after the wait, it left Steam with an upload batch
    /// nobody finished, and the next start was refused with "busy" (Vision
    /// Pro, 2026-10-11). The bytes are read before the first one is sent, so
    /// the uploads go on behind the game.
    nonisolated static func syncBeforeStart(wait: Double = 40) {
        guard let game, enabled, let creds = KLSteamKeychain.load() else { return }
        let settled = DispatchSemaphore(value: 0)
        final class Once: @unchecked Sendable {
            private let lock = NSLock(); private var fired = false
            func fire(_ s: DispatchSemaphore) { lock.lock(); let first = !fired; fired = true; lock.unlock(); if first { s.signal() } }
            var done: Bool { lock.lock(); defer { lock.unlock() }; return fired }
        }
        let once = Once()
        let work = Task { @MainActor in
            await shared.syncNow(game: game, creds: creds, settled: { once.fire(settled) })
            once.fire(settled)                     // ended without reaching that point: failed, or nothing to do
        }
        if settled.wait(timeout: .now() + wait) == .timedOut {
            // Still listing or downloading. What it is in the middle of writing
            // it finishes or drops (each file is written beside its place and
            // renamed); the game must not start while that is going on.
            work.cancel()
            _ = settled.wait(timeout: .now() + 10)
        }
    }
}

/// "Steam Cloud" in the game's settings.
struct KLSteamCloudPanel: View {
    @ObservedObject private var cloud = KLSteamCloud.shared
    @AppStorage(KLSteamCloud.enabledKey) private var enabled = true
    @State private var account = ""
    @State private var password = ""
    @State private var code = ""

    var body: some View {
        GroupBox("Steam Cloud") {
            VStack(alignment: .leading, spacing: 10) {
                Toggle(klT("Keep saves in step with Steam Cloud", "Spielstände mit Steam Cloud abgleichen"), isOn: $enabled)
                if !enabled {
                    Text(klT("Saves stay on this Vision Pro only.", "Spielstände bleiben nur auf dieser Vision Pro."))
                        .font(.callout).foregroundStyle(.secondary)
                } else if !cloud.accountName.isEmpty {
                    HStack {
                        Label(klT("Signed in as \(cloud.accountName)", "Angemeldet als \(cloud.accountName)"),
                              systemImage: "checkmark.circle.fill")
                        Spacer()
                        Button(klT("Sign out", "Abmelden")) { cloud.signOut() }
                    }
                    Text(klT("At every start the saves here and in your Steam Cloud are brought in step, in both directions. What you save in a session goes up at the next start.",
                             "Bei jedem Start werden die Spielstände hier und in deiner Steam Cloud abgeglichen, in beide Richtungen. Was du in einer Runde speicherst, geht beim nächsten Start hoch."))
                        .font(.callout).foregroundStyle(.secondary)
                    if !cloud.last.isEmpty {
                        Text(klT("Last sync \(cloud.last)", "Letzter Abgleich \(cloud.last)"))
                            .font(.callout).foregroundStyle(.secondary)
                    }
                } else {
                    signIn
                }
                if let p = cloud.problem {
                    Label(p, systemImage: "exclamationmark.triangle.fill")
                        .font(.callout).foregroundStyle(.yellow)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder private var signIn: some View {
        switch cloud.step {
        case .idle:
            TextField(klT("Steam account name", "Steam-Kontoname"), text: $account)
                .textContentType(.username)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .textFieldStyle(.roundedBorder)
            SecureField(klT("Password", "Passwort"), text: $password)
                .textContentType(.password)
                .textFieldStyle(.roundedBorder)
            Button(klT("Sign in to Steam", "Bei Steam anmelden")) {
                cloud.signIn(account: account, password: password)
                password = ""
            }
            .disabled(account.isEmpty || password.isEmpty)
            Text(klT("The password goes to Steam only, encrypted, for this one sign-in, and is not kept. What is kept is the session Steam hands back, in this Vision Pro's keychain.",
                     "Das Passwort geht nur an Steam, verschlüsselt, für diese eine Anmeldung, und wird nicht gespeichert. Gemerkt wird die Sitzung, die Steam zurückgibt, im Schlüsselbund dieser Vision Pro."))
                .font(.callout).foregroundStyle(.secondary)
        case .working:
            HStack { ProgressView(); Text(klT("Signing in …", "Anmeldung läuft …")) }
        case .approveInApp(let canEnterCode):
            Label(klT("Confirm the sign-in in the Steam app on your phone.",
                      "Bestätige die Anmeldung in der Steam-App auf deinem Handy."), systemImage: "iphone")
            HStack {
                if canEnterCode {
                    Button(klT("Enter a code instead", "Stattdessen Code eingeben")) { cloud.useCodeInstead() }
                }
                Button(klT("Cancel", "Abbrechen")) { cloud.cancelSignIn() }
            }
        case .enterCode(let email):
            Text(email.map { klT("Steam sent a code to \($0).", "Steam hat einen Code an \($0) geschickt.") }
                 ?? klT("Enter the Steam Guard code from the Steam app.", "Gib den Steam-Guard-Code aus der Steam-App ein."))
            HStack {
                TextField("Code", text: $code)
                    .textContentType(.oneTimeCode)
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled()
                    .textFieldStyle(.roundedBorder)
                    .frame(maxWidth: 200)
                Button(klT("Send", "Senden")) { cloud.submit(code: code); code = "" }
                    .disabled(code.isEmpty)
                Button(klT("Cancel", "Abbrechen")) { cloud.cancelSignIn() }
            }
        }
    }
}

/// One line for the start page: whether the saves are kept in step with Steam
/// Cloud, and where to change that. Nothing for a game that is not from Steam.
struct KLSteamCloudStatus: View {
    @ObservedObject private var cloud = KLSteamCloud.shared
    @AppStorage(KLSteamCloud.enabledKey) private var enabled = true

    var body: some View {
        if KLSteamCloud.game != nil {
            if enabled, !cloud.accountName.isEmpty {
                Label(klT("Steam Cloud is on (\(cloud.accountName)).", "Steam Cloud ist an (\(cloud.accountName))."),
                      systemImage: "checkmark.icloud")
                    .font(.callout).foregroundStyle(.secondary)
            } else {
                Label(enabled
                      ? klT("Steam Cloud is not active. Sign in to Steam under Settings to keep your saves in step.",
                            "Steam Cloud ist nicht aktiv. Melde dich unter „Einstellungen“ bei Steam an, um deine Spielstände abzugleichen.")
                      : klT("Steam Cloud is off. It can be switched on under Settings.",
                            "Steam Cloud ist aus. Es lässt sich unter „Einstellungen“ einschalten."),
                      systemImage: "icloud.slash")
                    .font(.callout).foregroundStyle(.secondary)
            }
        }
    }
}
