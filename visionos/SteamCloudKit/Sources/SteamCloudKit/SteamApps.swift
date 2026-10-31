import Foundation

public struct AppDepot: Sendable, Hashable {
    public let id: UInt32
    public let name: String
    public let maxSize: UInt64
    public let publicManifest: UInt64?
    public let oslist: String
    public let osarch: String
    public let lowViolence: Bool
    /// Some depots are shared from another app (e.g. Steamworks redistributables).
    public let fromApp: UInt32?
}

public struct AppInfo: Sendable {
    public let appID: UInt32
    public let name: String
    public let buildID: UInt64
    public let depots: [AppDepot]
    public let changeNumber: UInt32
    public let missingToken: Bool
    /// Steam Cloud Auto-Cloud rules (`ufs/savefiles`).
    public let cloudRules: [CloudSaveRule]
}

/// Product info (PICS), licenses, depot keys and content servers.
public struct SteamApps {
    public let cm: CMClient
    public init(cm: CMClient) { self.cm = cm }

    public func accessTokens(apps: [UInt32] = [], packages: [UInt32] = []) async throws -> (apps: [UInt32: UInt64], packages: [UInt32: UInt64]) {
        let p = try await cm.request(.clientPICSAccessTokenRequest, body: CMsgClientPICSAccessTokenRequest(packageIDs: packages, appIDs: apps))
        let r = try CMsgClientPICSAccessTokenResponse(proto: p.body)
        return (r.appTokens, r.packageTokens)
    }

    func productInfo(apps: [(UInt32, UInt64)] = [], packages: [(UInt32, UInt64)] = []) async throws -> CMsgClientPICSProductInfoResponse {
        let req = CMsgClientPICSProductInfoRequest(packages: packages.map { .init(id: $0.0, accessToken: $0.1) },
                                                   apps: apps.map { .init(id: $0.0, accessToken: $0.1) })
        let p = try await cm.request(.clientPICSProductInfoRequest, body: req, timeout: 60)
        return try CMsgClientPICSProductInfoResponse(proto: p.body)
    }

    /// The raw app info KeyValues (e.g. for the `ufs` Steam Cloud rules).
    public func appInfoKV(_ appID: UInt32) async throws -> KV {
        let token = (try? await accessTokens(apps: [appID]).apps[appID]) ?? 0
        let r = try await productInfo(apps: [(appID, token)])
        guard let item = r.apps.first(where: { $0.id == appID }) else { throw SteamError.badData("Steam doesn't know app \(appID)") }
        let kv = try KV.parseText(item.buffer)
        return kv["appinfo"] ?? kv
    }

    public func appInfo(_ appID: UInt32) async throws -> AppInfo {
        let token = (try? await accessTokens(apps: [appID]).apps[appID]) ?? 0
        let r = try await productInfo(apps: [(appID, token)])
        guard let item = r.apps.first(where: { $0.id == appID }) else {
            throw SteamError.badData("Steam doesn't know app \(appID)")
        }
        let kv = try KV.parseText(item.buffer)
        let root = kv["appinfo"] ?? kv
        var depots: [AppDepot] = []
        for (key, v) in root["depots"]?.children ?? [] {
            guard let id = UInt32(key) else { continue }
            let manifests = v["manifests"]
            let pub = manifests?["public"]
            let gid = pub?["gid"]?.uint64 ?? pub?.uint64
            depots.append(AppDepot(
                id: id, name: v["name"]?.string ?? "", maxSize: v["maxsize"]?.uint64 ?? 0, publicManifest: gid,
                oslist: v["config"]?["oslist"]?.string ?? "", osarch: v["config"]?["osarch"]?.string ?? "",
                lowViolence: v["config"]?["lowviolence"]?.string == "1",
                fromApp: v["depotfromapp"]?.int.map { UInt32($0) }))
        }
        let buildID = root["depots"]?["branches"]?["public"]?["buildid"]?.uint64 ?? 0
        return AppInfo(appID: appID, name: root["common"]?["name"]?.string ?? "App \(appID)", buildID: buildID,
                       depots: depots, changeNumber: item.changeNumber, missingToken: item.missingToken,
                       cloudRules: CloudSaveRule.parse(ufs: root["ufs"]))
    }
}
