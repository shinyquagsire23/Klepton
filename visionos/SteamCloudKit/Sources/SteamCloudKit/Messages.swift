import Foundation

// Hand-written Steam messages. Field numbers come from SteamDatabase/Protobufs
// (steammessages_base, _clientserver_login, _clientserver_appinfo, _clientserver_2,
// _auth.steamclient, _contentsystem.steamclient, _cloud.steamclient, content_manifest).

public enum EMsg: UInt32 {
    case multi = 1
    case serviceMethodResponse = 147
    case serviceMethodCallFromClient = 151
    case clientHeartBeat = 703
    case clientLogOff = 706
    case clientLogOnResponse = 751
    case clientLoggedOff = 757
    case clientLicenseList = 780
    case clientGetDepotDecryptionKey = 5438
    case clientGetDepotDecryptionKeyResponse = 5439
    case clientLogon = 5514
    case clientPICSProductInfoRequest = 8903
    case clientPICSProductInfoResponse = 8904
    case clientPICSAccessTokenRequest = 8905
    case clientPICSAccessTokenResponse = 8906
    case serviceMethodCallFromClientNonAuthed = 9804
    case clientHello = 9805
}

/// The subset of Steam's EResult the launcher reports on.
public struct EResult: RawRepresentable, Equatable, CustomStringConvertible, Sendable {
    public let rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    public static let ok = EResult(rawValue: 1)
    public static let fail = EResult(rawValue: 2)
    public static let invalidPassword = EResult(rawValue: 5)
    public static let accessDenied = EResult(rawValue: 15)
    public static let fileNotFound = EResult(rawValue: 9)
    public static let expired = EResult(rawValue: 27)
    public static let tryAnotherCM = EResult(rawValue: 48)
    public static let serviceUnavailable = EResult(rawValue: 20)
    public static let rateLimitExceeded = EResult(rawValue: 84)
    public static let accountLoginDeniedNeedTwoFactor = EResult(rawValue: 85)
    public static let invalidLoginAuthCode = EResult(rawValue: 65)
    public static let twoFactorCodeMismatch = EResult(rawValue: 88)

    public var description: String {
        switch self {
        case .ok: return "OK"
        case .fail: return "Fail"
        case .invalidPassword: return "InvalidPassword"
        case .accessDenied: return "AccessDenied"
        case .fileNotFound: return "FileNotFound"
        case .expired: return "Expired"
        case .tryAnotherCM: return "TryAnotherCM"
        case .serviceUnavailable: return "ServiceUnavailable"
        case .rateLimitExceeded: return "RateLimitExceeded"
        case .accountLoginDeniedNeedTwoFactor: return "NeedTwoFactor"
        case .invalidLoginAuthCode: return "InvalidLoginAuthCode"
        case .twoFactorCodeMismatch: return "TwoFactorCodeMismatch"
        default: return "EResult(\(rawValue))"
        }
    }
}

// MARK: - Framing

public struct CMsgProtoBufHeader: ProtoEncodable, ProtoDecodable {
    public var steamID: UInt64?
    public var clientSessionID: Int32?
    public var jobIDSource: UInt64?
    public var jobIDTarget: UInt64?
    public var targetJobName: String?
    public var eresult: Int32?
    public var errorMessage: String?
    public var realm: UInt32?

    public init() {}

    public func encode(into w: inout ProtoWriter) {
        w.fixed64(1, steamID)
        w.int(2, clientSessionID)
        w.fixed64(10, jobIDSource)
        w.fixed64(11, jobIDTarget)
        w.string(12, targetJobName)
        w.uint(32, realm)
    }

    public init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: steamID = f.uint64
            case 2: clientSessionID = f.int32
            case 10: jobIDSource = f.uint64
            case 11: jobIDTarget = f.uint64
            case 12: targetJobName = f.string
            case 13: eresult = f.int32
            case 14: errorMessage = f.string
            default: break
            }
        }
    }
}

struct CMsgMulti: ProtoDecodable {
    var sizeUnzipped: UInt32 = 0
    var body = Data()
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            if f.number == 1 { sizeUnzipped = f.uint32 }
            if f.number == 2 { body = f.bytes }
        }
    }
}

struct CMsgClientHello: ProtoEncodable {
    var protocolVersion: UInt32
    func encode(into w: inout ProtoWriter) { w.uint(1, protocolVersion) }
}

struct CMsgClientHeartBeat: ProtoEncodable {
    func encode(into w: inout ProtoWriter) { w.bool(1, false) }
}

// MARK: - Logon

struct CMsgClientLogon: ProtoEncodable {
    var protocolVersion: UInt32 = CMClient.protocolVersion
    var cellID: UInt32 = 0
    var clientLanguage = "english"
    var clientOSType: UInt32 = CMClient.osType
    var shouldRememberPassword: Bool?
    var machineID: Data?
    var accountName: String?
    var machineName: String?
    var accessToken: String?  // a refresh token from the Authentication service

    func encode(into w: inout ProtoWriter) {
        w.uint(1, protocolVersion)
        w.uint(3, cellID)
        w.string(6, clientLanguage)
        w.uint(7, clientOSType)
        w.bool(8, shouldRememberPassword)
        w.bytes(30, machineID)
        w.string(50, accountName)
        w.string(96, machineName)
        w.bool(102, true)  // supports_rate_limit_response
        w.string(108, accessToken)
    }
}

struct CMsgClientLogonResponse: ProtoDecodable {
    var eresult = EResult.fail
    var heartbeatSeconds: Int32 = 9
    var cellID: UInt32 = 0
    var clientSuppliedSteamID: UInt64 = 0
    var countryCode = ""
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: eresult = EResult(rawValue: f.int32)
            case 3: heartbeatSeconds = f.int32
            case 7: cellID = f.uint32
            case 20: clientSuppliedSteamID = f.uint64
            case 16: countryCode = f.string
            default: break
            }
        }
    }
}

struct CMsgClientLoggedOff: ProtoDecodable {
    var eresult = EResult.fail
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in if f.number == 1 { eresult = EResult(rawValue: f.int32) } }
    }
}

public struct License: Sendable {
    public var packageID: UInt32 = 0
    public var accessToken: UInt64 = 0
    public var timeCreated: UInt32 = 0
    public var licenseType: UInt32 = 0
    public var flags: UInt32 = 0
}

struct CMsgClientLicenseList: ProtoDecodable {
    var eresult = EResult.fail
    var licenses: [License] = []
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: eresult = EResult(rawValue: f.int32)
            case 2:
                var l = License()
                try ProtoReader.fields(f.bytes) { g in
                    switch g.number {
                    case 1: l.packageID = g.uint32
                    case 2: l.timeCreated = g.uint32
                    case 7: l.flags = g.uint32
                    case 9: l.licenseType = g.uint32
                    case 17: l.accessToken = g.uint64
                    default: break
                    }
                }
                licenses.append(l)
            default: break
            }
        }
    }
}

// MARK: - PICS (product info)

struct CMsgClientPICSAccessTokenRequest: ProtoEncodable {
    var packageIDs: [UInt32] = []
    var appIDs: [UInt32] = []
    func encode(into w: inout ProtoWriter) {
        for p in packageIDs { w.uint(1, p) }
        for a in appIDs { w.uint(2, a) }
    }
}

struct CMsgClientPICSAccessTokenResponse: ProtoDecodable {
    var appTokens: [UInt32: UInt64] = [:]
    var packageTokens: [UInt32: UInt64] = [:]
    var appDenied: [UInt32] = []
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1, 3:
                var id: UInt32 = 0, token: UInt64 = 0
                try ProtoReader.fields(f.bytes) { g in
                    if g.number == 1 { id = g.uint32 }
                    if g.number == 2 { token = g.uint64 }
                }
                if f.number == 1 { packageTokens[id] = token } else { appTokens[id] = token }
            case 4: appDenied += try ProtoReader.repeatedUInt32(f)
            default: break
            }
        }
    }
}

struct CMsgClientPICSProductInfoRequest: ProtoEncodable {
    struct Item { var id: UInt32; var accessToken: UInt64 }
    var packages: [Item] = []
    var apps: [Item] = []
    func encode(into w: inout ProtoWriter) {
        for p in packages {
            var s = ProtoWriter(); s.uint(1, p.id); s.uint(2, p.accessToken)
            w.bytes(1, s.data)
        }
        for a in apps {
            var s = ProtoWriter(); s.uint(1, a.id); s.uint(2, a.accessToken)
            w.bytes(2, s.data)
        }
        w.bool(7, true)  // single_response
    }
}

struct CMsgClientPICSProductInfoResponse: ProtoDecodable {
    struct Item { var id: UInt32 = 0; var changeNumber: UInt32 = 0; var missingToken = false; var buffer = Data() }
    var apps: [Item] = []
    var packages: [Item] = []
    var unknownApps: [UInt32] = []
    var unknownPackages: [UInt32] = []
    var responsePending = false
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1, 3:
                var it = Item()
                try ProtoReader.fields(f.bytes) { g in
                    switch g.number {
                    case 1: it.id = g.uint32
                    case 2: it.changeNumber = g.uint32
                    case 3: it.missingToken = g.bool
                    case 5: it.buffer = g.bytes
                    default: break
                    }
                }
                if f.number == 1 { apps.append(it) } else { packages.append(it) }
            case 2: unknownApps += try ProtoReader.repeatedUInt32(f)
            case 4: unknownPackages += try ProtoReader.repeatedUInt32(f)
            case 6: responsePending = f.bool
            default: break
            }
        }
    }
}

struct CMsgClientGetDepotDecryptionKey: ProtoEncodable {
    var depotID: UInt32
    var appID: UInt32
    func encode(into w: inout ProtoWriter) { w.uint(1, depotID); w.uint(2, appID) }
}

struct CMsgClientGetDepotDecryptionKeyResponse: ProtoDecodable {
    var eresult = EResult.fail
    var depotID: UInt32 = 0
    var key = Data()
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: eresult = EResult(rawValue: f.int32)
            case 2: depotID = f.uint32
            case 3: key = f.bytes
            default: break
            }
        }
    }
}

// MARK: - Authentication service

public enum AuthGuardType: Int32, Sendable {
    case unknown = 0, none = 1, emailCode = 2, deviceCode = 3, deviceConfirmation = 4, emailConfirmation = 5, machineToken = 6
}

struct DeviceDetails: ProtoEncodable {
    var friendlyName: String
    var platformType: UInt32 = 1  // k_EAuthTokenPlatformType_SteamClient
    var osType: Int32 = Int32(bitPattern: CMClient.osType)
    func encode(into w: inout ProtoWriter) {
        w.string(1, friendlyName)
        w.uint(2, platformType)
        w.int(3, osType)
    }
}

struct BeginAuthSessionViaQRRequest: ProtoEncodable {
    var device: DeviceDetails
    func encode(into w: inout ProtoWriter) {
        w.string(1, device.friendlyName)
        w.uint(2, device.platformType)
        w.message(3, device)
        w.string(4, "Client")
    }
}

struct AllowedConfirmation: Sendable {
    var type: AuthGuardType = .unknown
    var message = ""
}

private func parseConfirmation(_ data: Data) throws -> AllowedConfirmation {
    var c = AllowedConfirmation()
    try ProtoReader.fields(data) { g in
        if g.number == 1 { c.type = AuthGuardType(rawValue: g.int32) ?? .unknown }
        if g.number == 2 { c.message = g.string }
    }
    return c
}

struct BeginAuthSessionViaQRResponse: ProtoDecodable {
    var clientID: UInt64 = 0
    var challengeURL = ""
    var requestID = Data()
    var interval: Float = 5
    var confirmations: [AllowedConfirmation] = []
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: clientID = f.uint64
            case 2: challengeURL = f.string
            case 3: requestID = f.bytes
            case 4: interval = f.float
            case 5: confirmations.append(try parseConfirmation(f.bytes))
            default: break
            }
        }
    }
}

struct GetPasswordRSAPublicKeyRequest: ProtoEncodable {
    var accountName: String
    func encode(into w: inout ProtoWriter) { w.string(1, accountName) }
}

struct GetPasswordRSAPublicKeyResponse: ProtoDecodable {
    var modulusHex = "", exponentHex = "", timestamp: UInt64 = 0
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: modulusHex = f.string
            case 2: exponentHex = f.string
            case 3: timestamp = f.uint64
            default: break
            }
        }
    }
}

struct BeginAuthSessionViaCredentialsRequest: ProtoEncodable {
    var accountName: String
    var encryptedPassword: String
    var encryptionTimestamp: UInt64
    var device: DeviceDetails
    var guardData: String?
    func encode(into w: inout ProtoWriter) {
        w.string(1, device.friendlyName)
        w.string(2, accountName)
        w.string(3, encryptedPassword)
        w.uint(4, encryptionTimestamp)
        w.bool(5, true)                  // remember_login
        w.uint(6, device.platformType)
        w.uint(7, UInt32(1))             // k_ESessionPersistence_Persistent
        w.string(8, "Client")
        w.message(9, device)
        w.string(10, guardData)
    }
}

struct BeginAuthSessionViaCredentialsResponse: ProtoDecodable {
    var clientID: UInt64 = 0
    var requestID = Data()
    var interval: Float = 5
    var confirmations: [AllowedConfirmation] = []
    var steamID: UInt64 = 0
    var extendedError = ""
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: clientID = f.uint64
            case 2: requestID = f.bytes
            case 3: interval = f.float
            case 4: confirmations.append(try parseConfirmation(f.bytes))
            case 5: steamID = f.uint64
            case 8: extendedError = f.string
            default: break
            }
        }
    }
}

struct UpdateAuthSessionWithSteamGuardCodeRequest: ProtoEncodable {
    var clientID: UInt64
    var steamID: UInt64
    var code: String
    var codeType: AuthGuardType
    func encode(into w: inout ProtoWriter) {
        w.uint(1, clientID)
        w.fixed64(2, steamID)
        w.string(3, code)
        w.uint(4, UInt32(codeType.rawValue))
    }
}

struct PollAuthSessionStatusRequest: ProtoEncodable {
    var clientID: UInt64
    var requestID: Data
    func encode(into w: inout ProtoWriter) { w.uint(1, clientID); w.bytes(2, requestID) }
}

struct PollAuthSessionStatusResponse: ProtoDecodable {
    var newClientID: UInt64?
    var newChallengeURL: String?
    var refreshToken: String?
    var accessToken: String?
    var hadRemoteInteraction = false
    var accountName: String?
    var newGuardData: String?
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1: newClientID = f.uint64
            case 2: newChallengeURL = f.string
            case 3: refreshToken = f.string
            case 4: accessToken = f.string
            case 5: hadRemoteInteraction = f.bool
            case 6: accountName = f.string
            case 7: newGuardData = f.string
            default: break
            }
        }
    }
}

// MARK: - Content

struct GetManifestRequestCodeRequest: ProtoEncodable {
    var appID: UInt32, depotID: UInt32, manifestID: UInt64, branch: String
    func encode(into w: inout ProtoWriter) {
        w.uint(1, appID); w.uint(2, depotID); w.uint(3, manifestID); w.string(4, branch)
    }
}

struct GetManifestRequestCodeResponse: ProtoDecodable {
    var code: UInt64 = 0
    init(proto: Data) throws { try ProtoReader.fields(proto) { f in if f.number == 1 { code = f.uint64 } } }
}

struct GetServersForSteamPipeRequest: ProtoEncodable {
    var cellID: UInt32
    var maxServers: UInt32 = 20
    func encode(into w: inout ProtoWriter) { w.uint(1, cellID); w.uint(2, maxServers) }
}

public struct ContentServer: Sendable, Hashable {
    public var type = ""
    public var host = ""
    public var vhost = ""
    public var https = ""
    public var load: Int32 = 0
    public var weightedLoad: Float = 0
    public var allowedAppIDs: [UInt32] = []
    public var useAsProxy = false
}

struct GetServersForSteamPipeResponse: ProtoDecodable {
    var servers: [ContentServer] = []
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            guard f.number == 1 else { return }
            var s = ContentServer()
            try ProtoReader.fields(f.bytes) { g in
                switch g.number {
                case 1: s.type = g.string
                case 4: s.load = g.int32
                case 5: s.weightedLoad = g.float
                case 8: s.host = g.string
                case 9: s.vhost = g.string
                case 10: s.useAsProxy = g.bool
                case 12: s.https = g.string
                case 13: s.allowedAppIDs += try ProtoReader.repeatedUInt32(g)
                default: break
                }
            }
            servers.append(s)
        }
    }
}

// MARK: - Steam Cloud

struct CloudEnumerateUserFilesRequest: ProtoEncodable {
    var appID: UInt32
    var startIndex: UInt32 = 0
    func encode(into w: inout ProtoWriter) {
        w.uint(1, appID); w.bool(2, true); w.uint(3, UInt32(500)); w.uint(4, startIndex)
    }
}

public struct CloudFile: Sendable, Hashable {
    public init() {}
    public var filename = ""
    public var timestamp: UInt64 = 0
    public var size: UInt32 = 0
    public var url = ""
    public var sha1Hex = ""
    public var platforms: [String] = []
}

struct CloudEnumerateUserFilesResponse: ProtoDecodable {
    var files: [CloudFile] = []
    var total: UInt32 = 0
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 1:
                var c = CloudFile()
                try ProtoReader.fields(f.bytes) { g in
                    switch g.number {
                    case 3: c.filename = g.string
                    case 4: c.timestamp = g.uint64
                    case 5: c.size = g.uint32
                    case 6: c.url = g.string
                    case 9: c.platforms.append(g.string)
                    case 10: c.sha1Hex = g.string
                    default: break
                    }
                }
                files.append(c)
            case 2: total = f.uint32
            default: break
            }
        }
    }
}

struct CloudClientFileDownloadRequest: ProtoEncodable {
    var appID: UInt32
    var filename: String
    func encode(into w: inout ProtoWriter) { w.uint(1, appID); w.string(2, filename); w.uint(3, UInt32(1)) }
}

struct CloudClientFileDownloadResponse: ProtoDecodable {
    var fileSize: UInt32 = 0, rawFileSize: UInt32 = 0
    var sha = Data()
    var timestamp: UInt64 = 0
    var host = "", path = ""
    var https = true
    var headers: [(String, String)] = []
    var encrypted = false
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            switch f.number {
            case 2: fileSize = f.uint32
            case 3: rawFileSize = f.uint32
            case 4: sha = f.bytes
            case 5: timestamp = f.uint64
            case 7: host = f.string
            case 8: path = f.string
            case 9: https = f.bool
            case 10:
                var n = "", v = ""
                try ProtoReader.fields(f.bytes) { g in
                    if g.number == 1 { n = g.string }
                    if g.number == 2 { v = g.string }
                }
                headers.append((n, v))
            case 11: encrypted = f.bool
            default: break
            }
        }
    }
}

struct CloudBeginAppUploadBatchRequest: ProtoEncodable {
    var appID: UInt32, machineName: String, files: [String], buildID: UInt64
    func encode(into w: inout ProtoWriter) {
        w.uint(1, appID); w.string(2, machineName)
        for f in files { w.string(3, f) }
        if buildID != 0 { w.uint(6, buildID) }
    }
}

struct CloudBeginAppUploadBatchResponse: ProtoDecodable {
    var batchID: UInt64 = 0
    init(proto: Data) throws { try ProtoReader.fields(proto) { f in if f.number == 1 { batchID = f.uint64 } } }
}

struct CloudCompleteAppUploadBatchRequest: ProtoEncodable {
    var appID: UInt32, batchID: UInt64, result: EResult
    func encode(into w: inout ProtoWriter) { w.uint(1, appID); w.uint(2, batchID); w.uint(3, UInt32(bitPattern: result.rawValue)) }
}

struct CloudClientBeginFileUploadRequest: ProtoEncodable {
    var appID: UInt32, size: UInt32, sha: Data, timestamp: UInt64, filename: String, batchID: UInt64
    func encode(into w: inout ProtoWriter) {
        w.uint(1, appID); w.uint(2, size); w.uint(3, size); w.bytes(4, sha); w.uint(5, timestamp); w.string(6, filename)
        w.uint(7, UInt32.max)   // sync to every platform, as Auto-Cloud does
        w.bool(10, false)       // can_encrypt
        w.uint(13, batchID)
    }
}

struct CloudUploadBlock {
    var host = "", path = "", https = true, method = "PUT"
    var headers: [(String, String)] = []
    var offset: UInt64 = 0, length: UInt32 = 0
    var explicitBody: Data?
}

struct CloudClientBeginFileUploadResponse: ProtoDecodable {
    var blocks: [CloudUploadBlock] = []
    init(proto: Data) throws {
        try ProtoReader.fields(proto) { f in
            guard f.number == 2 else { return }
            var b = CloudUploadBlock()
            try ProtoReader.fields(f.bytes) { g in
                switch g.number {
                case 1: b.host = g.string
                case 2: b.path = g.string
                case 3: b.https = g.bool
                case 4: b.method = [1: "GET", 2: "HEAD", 3: "POST", 4: "PUT", 5: "DELETE"][g.int32] ?? "PUT"
                case 5:
                    var n = "", v = ""
                    try ProtoReader.fields(g.bytes) { h in
                        if h.number == 1 { n = h.string }
                        if h.number == 2 { v = h.string }
                    }
                    b.headers.append((n, v))
                case 6: b.offset = g.uint64
                case 7: b.length = g.uint32
                case 8: b.explicitBody = g.bytes
                default: break
                }
            }
            blocks.append(b)
        }
    }
}

struct CloudClientCommitFileUploadRequest: ProtoEncodable {
    var succeeded: Bool, appID: UInt32, sha: Data, filename: String
    func encode(into w: inout ProtoWriter) { w.bool(1, succeeded); w.uint(2, appID); w.bytes(3, sha); w.string(4, filename) }
}

struct CloudClientCommitFileUploadResponse: ProtoDecodable {
    var committed = false
    init(proto: Data) throws { try ProtoReader.fields(proto) { f in if f.number == 1 { committed = f.bool } } }
}
