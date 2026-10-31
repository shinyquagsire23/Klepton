import Foundation
#if canImport(Security)
import Security
#endif
#if os(macOS)
import SystemConfiguration
#endif

/// Sign-in through Steam's Authentication service, the same flow as the Steam client and
/// the Steam mobile app: either a QR code the user approves in the Steam app, or account name
/// and password with Steam Guard (app confirmation, app code or email code).
/// The result is a long-lived refresh token; the launcher never stores the password.
public struct SteamCredentials: Codable, Sendable, Equatable {
    public var accountName: String
    public var refreshToken: String
    public var guardData: String?

    public init(accountName: String, refreshToken: String, guardData: String? = nil) {
        self.accountName = accountName; self.refreshToken = refreshToken; self.guardData = guardData
    }

    public var steamID: UInt64? { SteamID.fromJWT(refreshToken) }
    public var expires: Date? { SteamID.expiry(ofJWT: refreshToken) }
}

public enum AuthPrompt: Sendable, Equatable {
    /// Show this URL as a QR code; the Steam app on a phone scans it.
    case qr(url: String)
    /// Approve the sign-in in the Steam app (a push notification), or type a code instead.
    case approveInApp(canEnterCode: Bool)
    /// Type the Steam Guard code from the Steam app.
    case deviceCode
    /// Type the code Steam emailed to the address hinted at.
    case emailCode(hint: String)
}

public final class SteamAuthSession {
    private let cm: CMClient
    private var clientID: UInt64
    private let requestID: Data
    private let interval: Double
    private let steamID: UInt64
    private let deviceName: String
    private(set) var confirmations: [AllowedConfirmation]
    public private(set) var challengeURL: String?

    init(cm: CMClient, clientID: UInt64, requestID: Data, interval: Float, steamID: UInt64, deviceName: String,
         confirmations: [AllowedConfirmation], challengeURL: String?) {
        self.cm = cm
        self.clientID = clientID
        self.requestID = requestID
        self.interval = Double(max(interval, 1))
        self.steamID = steamID
        self.deviceName = deviceName
        self.confirmations = confirmations
        self.challengeURL = challengeURL
    }

    /// What the UI should ask for, most convenient first (Steam's own order of preference).
    public var prompt: AuthPrompt {
        if let challengeURL { return .qr(url: challengeURL) }
        let types = confirmations.map(\.type)
        if types.contains(.deviceConfirmation) { return .approveInApp(canEnterCode: types.contains(.deviceCode)) }
        if types.contains(.deviceCode) { return .deviceCode }
        if let e = confirmations.first(where: { $0.type == .emailCode }) { return .emailCode(hint: e.message) }
        return .approveInApp(canEnterCode: false)
    }

    public var needsCode: Bool {
        let t = confirmations.map(\.type)
        return !(t.contains(.none) || t.isEmpty)
    }

    /// Submits a Steam Guard code (from the Steam app or email).
    public func submit(code: String) async throws {
        let type: AuthGuardType = confirmations.contains { $0.type == .deviceCode } ? .deviceCode : .emailCode
        _ = try await cm.call("Authentication.UpdateAuthSessionWithSteamGuardCode#1",
                              UpdateAuthSessionWithSteamGuardCodeRequest(clientID: clientID, steamID: steamID, code: code, codeType: type),
                              authed: false)
    }

    /// Polls until the user approves (QR scan or push), a code is accepted, or the session expires.
    /// `onNewChallenge` fires when Steam rotates the QR code.
    /// `onRemoteInteraction` fires once, when the phone has opened the link (the Steam app is showing "Approve").
    public func waitForApproval(onNewChallenge: ((String) -> Void)? = nil,
                                onRemoteInteraction: (() -> Void)? = nil) async throws -> SteamCredentials {
        var sawInteraction = false
        while true {
            try Task.checkCancellation()
            let data = try await cm.call("Authentication.PollAuthSessionStatus#1",
                                         PollAuthSessionStatusRequest(clientID: clientID, requestID: requestID), authed: false)
            let r = try PollAuthSessionStatusResponse(proto: data)
            if let n = r.newClientID, n != 0 { clientID = n }
            if let url = r.newChallengeURL, !url.isEmpty {
                challengeURL = url
                onNewChallenge?(url)
            }
            if r.hadRemoteInteraction, !sawInteraction {
                sawInteraction = true
                onRemoteInteraction?()
            }
            if let token = r.refreshToken, !token.isEmpty {
                return SteamCredentials(accountName: r.accountName ?? "", refreshToken: token, guardData: r.newGuardData)
            }
            try await Task.sleep(nanoseconds: UInt64(interval * 1e9))
        }
    }
}

public enum SteamAuth {
    public static func deviceName() -> String {
        #if os(visionOS)
        return "Apple Vision Pro"
        #elseif os(macOS)
        // The name in System Settings › General › About. Not ProcessInfo.hostName: that looks up the
        // Mac's Bonjour name on the local network, and macOS then asks to let the app find devices there.
        return SCDynamicStoreCopyComputerName(nil, nil) as String? ?? "Mac"
        #else
        return ProcessInfo.processInfo.hostName
        #endif
    }

    /// Starts a QR sign-in. The CM connection must be open (logon isn't needed).
    public static func beginQR(cm: CMClient, deviceName: String = deviceName()) async throws -> SteamAuthSession {
        let data = try await cm.call("Authentication.BeginAuthSessionViaQR#1",
                                     BeginAuthSessionViaQRRequest(device: DeviceDetails(friendlyName: deviceName)), authed: false)
        let r = try BeginAuthSessionViaQRResponse(proto: data)
        return SteamAuthSession(cm: cm, clientID: r.clientID, requestID: r.requestID, interval: r.interval, steamID: 0,
                                deviceName: deviceName, confirmations: r.confirmations, challengeURL: r.challengeURL)
    }

    /// Starts an account-name-and-password sign-in. The password is RSA-encrypted with Steam's
    /// per-account key before it leaves the device and is never stored.
    public static func beginCredentials(cm: CMClient, accountName: String, password: String, guardData: String? = nil,
                                        deviceName: String = deviceName()) async throws -> SteamAuthSession {
        let keyData = try await cm.call("Authentication.GetPasswordRSAPublicKey#1",
                                        GetPasswordRSAPublicKeyRequest(accountName: accountName), authed: false)
        let key = try GetPasswordRSAPublicKeyResponse(proto: keyData)
        let encrypted = try RSA.encryptPKCS1(Data(password.utf8), modulusHex: key.modulusHex, exponentHex: key.exponentHex)
        let req = BeginAuthSessionViaCredentialsRequest(accountName: accountName, encryptedPassword: encrypted.base64EncodedString(),
                                                        encryptionTimestamp: key.timestamp, device: DeviceDetails(friendlyName: deviceName),
                                                        guardData: guardData)
        let data = try await cm.call("Authentication.BeginAuthSessionViaCredentials#1", req, authed: false)
        let r = try BeginAuthSessionViaCredentialsResponse(proto: data)
        return SteamAuthSession(cm: cm, clientID: r.clientID, requestID: r.requestID, interval: r.interval, steamID: r.steamID,
                                deviceName: deviceName, confirmations: r.confirmations, challengeURL: nil)
    }
}

enum RSA {
    static func encryptPKCS1(_ message: Data, modulusHex: String, exponentHex: String) throws -> Data {
        guard let mod = Data(hex: modulusHex), let exp = Data(hex: exponentHex) else { throw SteamError.badData("RSA key") }
        #if canImport(Security)
        let der = derRSAPublicKey(modulus: mod, exponent: exp)
        let attrs: [String: Any] = [kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
                                    kSecAttrKeyClass as String: kSecAttrKeyClassPublic,
                                    kSecAttrKeySizeInBits as String: mod.count * 8]
        var err: Unmanaged<CFError>?
        guard let key = SecKeyCreateWithData(der as CFData, attrs as CFDictionary, &err),
              let out = SecKeyCreateEncryptedData(key, .rsaEncryptionPKCS1, message as CFData, &err) else {
            throw SteamError.badData("RSA encrypt: \(err?.takeRetainedValue().localizedDescription ?? "?")")
        }
        return out as Data
        #else
        _ = (mod, exp)
        throw SteamError.unsupported("Password sign-in needs Apple's Security framework; use the QR code sign-in on Linux.")
        #endif
    }

    /// PKCS#1 RSAPublicKey: SEQUENCE { INTEGER modulus, INTEGER exponent }.
    static func derRSAPublicKey(modulus: Data, exponent: Data) -> Data {
        func len(_ n: Int) -> Data {
            if n < 0x80 { return Data([UInt8(n)]) }
            var bytes: [UInt8] = []
            var v = n
            while v > 0 { bytes.insert(UInt8(v & 0xFF), at: 0); v >>= 8 }
            return Data([0x80 | UInt8(bytes.count)] + bytes)
        }
        func integer(_ d: Data) -> Data {
            var v = Data(d.drop { $0 == 0 })
            if let f = v.first, f & 0x80 != 0 { v.insert(0, at: 0) }
            return Data([0x02]) + len(v.count) + v
        }
        let body = integer(modulus) + integer(exponent)
        return Data([0x30]) + len(body.count) + body
    }
}
