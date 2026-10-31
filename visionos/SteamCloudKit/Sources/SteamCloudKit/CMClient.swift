import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum SteamError: Error, CustomStringConvertible, Equatable {
    case notConnected
    case disconnected
    case timeout(String)
    case result(EResult, String)
    case noServers
    case badData(String)
    case unsupported(String)

    public var description: String {
        switch self {
        case .notConnected: return "Not connected to Steam."
        case .disconnected: return "Steam closed the connection."
        case .timeout(let what): return "Steam didn't answer in time (\(what))."
        case .result(let r, let what): return "\(what) failed: \(r)"
        case .noServers: return "No Steam servers are reachable."
        case .badData(let s): return "Unexpected data from Steam: \(s)"
        case .unsupported(let s): return s
        }
    }
}

/// One message from Steam, already unwrapped from any CMsgMulti.
public struct Packet {
    public let emsg: UInt32
    public let header: CMsgProtoBufHeader
    public let body: Data
}

/// A connection to a Steam connection manager: framing, jobs, unified service calls,
/// logon and heartbeats. This mirrors SteamKit2's CMClient/SteamClient, trimmed to what a
/// game launcher needs.
public actor CMClient {
    static let protocolVersion: UInt32 = 65581
    /// k_EOSTypeMacOS: Steam has no visionOS OS type, and the headset runs Apple's OS family.
    static let osType: UInt32 = UInt32(bitPattern: -102)
    private static let protoMask: UInt32 = 0x8000_0000
    static let anonymousSteamID: UInt64 = 0x01A0_0000_0000_0000

    private let makeTransport: () -> CMTransport
    private var transport: CMTransport?
    private var receiveTask: Task<Void, Never>?
    private var heartbeatTask: Task<Void, Never>?

    private var nextJobID: UInt64 = 1
    private var jobs: [UInt64: CheckedContinuation<Packet, Error>] = [:]
    private var waiters: [UInt32: [CheckedContinuation<Packet, Error>]] = [:]
    private var mailbox: [UInt32: Packet] = [:]

    public private(set) var steamID: UInt64 = 0
    public private(set) var sessionID: Int32 = 0
    public private(set) var cellID: UInt32 = 0
    public private(set) var isLoggedOn = false
    public private(set) var endpoint: String?

    public var log: (@Sendable (String) -> Void)?

    public init(makeTransport: @escaping () -> CMTransport = Transports.make) {
        self.makeTransport = makeTransport
    }

    public func setLogger(_ f: @escaping @Sendable (String) -> Void) { log = f }

    // MARK: Connection

    /// Picks the least-loaded WebSocket CMs from Steam's directory and connects to the first
    /// one that answers, skipping `avoiding` (CMs that just turned us away) while others are left.
    public func connect(cellID: UInt32 = 0, avoiding: Set<String> = []) async throws {
        disconnect()
        let url = URL(string: "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=\(cellID)&cmtype=websockets")!
        let (data, _) = try await URLSession.shared.data(from: url)
        struct List: Decodable {
            struct R: Decodable { let serverlist: [S] }
            struct S: Decodable { let endpoint: String; let type: String; let wtd_load: Double? }
            let response: R
        }
        let servers = try JSONDecoder().decode(List.self, from: data).response.serverlist
            .filter { $0.type == "websockets" }
            // Port 443 first: it gets through hotel, office and proxy networks that block 27xxx.
            .sorted { ($0.endpoint.hasSuffix(":443") ? 0 : 1, $0.wtd_load ?? 100) < ($1.endpoint.hasSuffix(":443") ? 0 : 1, $1.wtd_load ?? 100) }
        guard !servers.isEmpty else { throw SteamError.noServers }
        let fresh = servers.filter { !avoiding.contains($0.endpoint) }

        var lastError: Error = SteamError.noServers
        for s in (fresh.isEmpty ? servers : fresh).prefix(4) {
            do {
                try await open(endpoint: s.endpoint)
                return
            } catch {
                lastError = error
                log?("CM \(s.endpoint) failed: \(error)")
                disconnect()
            }
        }
        throw lastError
    }

    private func open(endpoint: String) async throws {
        let t = makeTransport()
        try await t.connect(url: URL(string: "wss://\(endpoint)/cmsocket/")!)
        transport = t
        self.endpoint = endpoint
        receiveTask = Task { [weak self] in await self?.receiveLoop(t) }
        try await send(.clientHello, body: CMsgClientHello(protocolVersion: Self.protocolVersion))
        log?("connected to \(endpoint)")
    }

    public func disconnect() {
        heartbeatTask?.cancel(); heartbeatTask = nil
        receiveTask?.cancel(); receiveTask = nil
        transport?.close(); transport = nil
        isLoggedOn = false
        failAll(SteamError.disconnected)
    }

    private func failAll(_ error: Error) {
        for (_, c) in jobs { c.resume(throwing: error) }
        jobs.removeAll()
        for (_, list) in waiters { for c in list { c.resume(throwing: error) } }
        waiters.removeAll()
    }

    // MARK: Framing

    private func frame(_ emsg: UInt32, header: CMsgProtoBufHeader, body: Data) -> Data {
        let h = header.encoded()
        var d = Data(capacity: 8 + h.count + body.count)
        withUnsafeBytes(of: (emsg | Self.protoMask).littleEndian) { d.append(contentsOf: $0) }
        withUnsafeBytes(of: UInt32(h.count).littleEndian) { d.append(contentsOf: $0) }
        d.append(h)
        d.append(body)
        return d
    }

    private func baseHeader() -> CMsgProtoBufHeader {
        var h = CMsgProtoBufHeader()
        h.steamID = steamID
        h.clientSessionID = sessionID
        return h
    }

    func send(_ emsg: EMsg, header: CMsgProtoBufHeader? = nil, body: ProtoEncodable) async throws {
        guard let transport else { throw SteamError.notConnected }
        try await transport.send(frame(emsg.rawValue, header: header ?? baseHeader(), body: body.encoded()))
    }

    private func receiveLoop(_ t: CMTransport) async {
        while !Task.isCancelled {
            do {
                let data = try await t.receive()
                try dispatch(data)
            } catch {
                if !Task.isCancelled {
                    log?("receive ended: \(error)")
                    isLoggedOn = false
                    failAll(SteamError.disconnected)
                }
                return
            }
        }
    }

    private static func u32(_ d: Data, _ at: Int) -> UInt32 {
        let i = d.startIndex + at
        return UInt32(d[i]) | UInt32(d[i + 1]) << 8 | UInt32(d[i + 2]) << 16 | UInt32(d[i + 3]) << 24
    }

    private func dispatch(_ data: Data) throws {
        guard data.count >= 8 else { return }
        let raw = Self.u32(data, 0)
        guard raw & Self.protoMask != 0 else { return }  // the launcher only uses protobuf messages
        let emsg = raw & ~Self.protoMask
        let hlen = Int(Self.u32(data, 4))
        guard data.count >= 8 + hlen else { throw SteamError.badData("short header") }
        let s = data.startIndex
        let header = try CMsgProtoBufHeader(proto: data.subdata(in: (s + 8)..<(s + 8 + hlen)))
        let body = data.subdata(in: (s + 8 + hlen)..<data.endIndex)

        if emsg == EMsg.multi.rawValue {
            let multi = try CMsgMulti(proto: body)
            var payload = multi.body
            if multi.sizeUnzipped > 0 {
                payload = try Codec.gunzip(multi.body, expected: Int(multi.sizeUnzipped))
            }
            var off = 0
            while off + 4 <= payload.count {
                let n = Int(Self.u32(payload, off))
                let st = payload.startIndex + off + 4
                guard off + 4 + n <= payload.count else { break }
                try dispatch(payload.subdata(in: st..<(st + n)))
                off += 4 + n
            }
            return
        }

        let packet = Packet(emsg: emsg, header: header, body: body)
        if let sid = header.clientSessionID, sid != 0 { sessionID = sid }
        if let target = header.jobIDTarget, target != UInt64.max, let c = jobs.removeValue(forKey: target) {
            c.resume(returning: packet)
            return
        }
        if emsg == EMsg.clientLoggedOff.rawValue {
            isLoggedOn = false
            log?("logged off by Steam: \((try? CMsgClientLoggedOff(proto: body))?.eresult.description ?? "?")")
        }
        if var list = waiters[emsg], !list.isEmpty {
            let c = list.removeFirst()
            waiters[emsg] = list
            c.resume(returning: packet)
        } else {
            mailbox[emsg] = packet
        }
    }

    // MARK: Requests

    /// Sends a message that Steam answers with a job reply, and waits for the reply.
    func request(_ emsg: EMsg, header: CMsgProtoBufHeader? = nil, body: ProtoEncodable, timeout: Double = 30) async throws -> Packet {
        guard let transport else { throw SteamError.notConnected }
        let job = nextJobID; nextJobID += 1
        var h = header ?? baseHeader()
        h.jobIDSource = job
        let bytes = frame(emsg.rawValue, header: h, body: body.encoded())
        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<Packet, Error>) in
            jobs[job] = c
            Task {
                do { try await transport.send(bytes) } catch { self.failJob(job, error) }
            }
            Task {
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1e9))
                self.failJob(job, SteamError.timeout("\(emsg)"))
            }
        }
    }

    private func failJob(_ job: UInt64, _ error: Error) {
        jobs.removeValue(forKey: job)?.resume(throwing: error)
    }

    /// Waits for the next unsolicited message of a type (or returns one that already arrived).
    func waitFor(_ emsg: EMsg, timeout: Double = 30) async throws -> Packet {
        if let p = mailbox.removeValue(forKey: emsg.rawValue) { return p }
        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<Packet, Error>) in
            waiters[emsg.rawValue, default: []].append(c)
            Task {
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1e9))
                self.expireWaiter(emsg.rawValue)
            }
        }
    }

    private func expireWaiter(_ emsg: UInt32) {
        guard var list = waiters[emsg], !list.isEmpty else { return }
        let c = list.removeFirst()
        waiters[emsg] = list
        c.resume(throwing: SteamError.timeout("waiting for \(emsg)"))
    }

    /// Calls a unified service method such as `Authentication.BeginAuthSessionViaQR#1`.
    public func call(_ method: String, _ body: ProtoEncodable, authed: Bool = true, timeout: Double = 30) async throws -> Data {
        var h = baseHeader()
        h.targetJobName = method
        if !authed { h.steamID = 0; h.clientSessionID = 0 }
        let emsg: EMsg = authed ? .serviceMethodCallFromClient : .serviceMethodCallFromClientNonAuthed
        let reply = try await request(emsg, header: h, body: body, timeout: timeout)
        let result = EResult(rawValue: reply.header.eresult ?? 2)
        guard result == .ok else {
            throw SteamError.result(result, reply.header.errorMessage.map { "\(method) (\($0))" } ?? method)
        }
        return reply.body
    }

    // MARK: Logon

    public struct LogonInfo: Sendable {
        public let steamID: UInt64
        public let cellID: UInt32
        public let country: String
    }

    /// Logs on with a refresh token from the Authentication service, the way the modern
    /// Steam client does. No password is ever sent here.
    public func logon(accountName: String, refreshToken: String, machineName: String) async throws -> LogonInfo {
        guard let sid = SteamID.fromJWT(refreshToken) else { throw SteamError.badData("refresh token has no SteamID") }
        var h = CMsgProtoBufHeader()
        h.steamID = sid
        h.clientSessionID = 0
        var body = CMsgClientLogon()
        body.accountName = accountName
        body.accessToken = refreshToken
        body.shouldRememberPassword = true
        body.machineName = machineName
        body.machineID = MachineID.current()
        return try await doLogon(header: h, body: body)
    }

    /// Anonymous logon: enough for public product info and free depots, used by tests.
    public func logonAnonymous() async throws -> LogonInfo {
        var h = CMsgProtoBufHeader()
        h.steamID = Self.anonymousSteamID
        h.clientSessionID = 0
        return try await doLogon(header: h, body: CMsgClientLogon())
    }

    private func doLogon(header: CMsgProtoBufHeader, body: CMsgClientLogon) async throws -> LogonInfo {
        mailbox.removeValue(forKey: EMsg.clientLogOnResponse.rawValue)
        try await send(.clientLogon, header: header, body: body)
        let p = try await waitFor(.clientLogOnResponse)
        let r = try CMsgClientLogonResponse(proto: p.body)
        guard r.eresult == .ok else { throw SteamError.result(r.eresult, "Steam logon") }
        steamID = p.header.steamID ?? header.steamID ?? 0
        cellID = r.cellID
        isLoggedOn = true
        startHeartbeat(seconds: max(5, Int(r.heartbeatSeconds)))
        log?("logged on as \(steamID), cell \(cellID)")
        return LogonInfo(steamID: steamID, cellID: cellID, country: r.countryCode)
    }

    private func startHeartbeat(seconds: Int) {
        heartbeatTask?.cancel()
        heartbeatTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(seconds) * 1_000_000_000)
                try? await self?.send(.clientHeartBeat, body: CMsgClientHeartBeat())
            }
        }
    }

    public func logOff() async {
        try? await send(.clientLogOff, body: CMsgClientHeartBeat())
        disconnect()
    }

    /// The license list Steam pushes right after logon.
    public func licenses(timeout: Double = 20) async throws -> [License] {
        let p = try await waitFor(.clientLicenseList, timeout: timeout)
        mailbox[EMsg.clientLicenseList.rawValue] = p  // keep it for later callers
        let list = try CMsgClientLicenseList(proto: p.body)
        guard list.eresult == .ok else { throw SteamError.result(list.eresult, "License list") }
        return list.licenses
    }
}

public enum SteamID {
    /// Refresh and access tokens are JWTs whose `sub` claim is the SteamID64.
    public static func fromJWT(_ token: String) -> UInt64? {
        let parts = token.split(separator: ".")
        guard parts.count >= 2 else { return nil }
        var b64 = parts[1].replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        while b64.count % 4 != 0 { b64 += "=" }
        guard let data = Data(base64Encoded: b64),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let sub = obj["sub"] as? String else { return nil }
        return UInt64(sub)
    }

    /// The token's expiry, so the launcher can ask for a fresh sign-in before it lapses.
    public static func expiry(ofJWT token: String) -> Date? {
        let parts = token.split(separator: ".")
        guard parts.count >= 2 else { return nil }
        var b64 = parts[1].replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        while b64.count % 4 != 0 { b64 += "=" }
        guard let data = Data(base64Encoded: b64),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let exp = obj["exp"] as? Double else { return nil }
        return Date(timeIntervalSince1970: exp)
    }
}

/// Steam's machine ID: a binary KeyValues "MessageObject" with three hashes. Steam uses it
/// to recognise a device across logons. We derive stable values from a per-install UUID.
enum MachineID {
    static func current() -> Data {
        let key = "SteamLauncherKit.machineSeed"
        let seed: String
        if let s = UserDefaults.standard.string(forKey: key) {
            seed = s
        } else {
            seed = UUID().uuidString
            UserDefaults.standard.set(seed, forKey: key)
        }
        func hex(_ salt: String) -> String {
            Codec.sha1(Data((seed + salt).utf8)).map { String(format: "%02x", $0) }.joined()
        }
        var d = Data([0x00]) + Data("MessageObject".utf8) + Data([0])
        for (k, v) in [("BB3", hex("bb3")), ("FF2", hex("ff2")), ("3B3", hex("3b3"))] {
            d += Data([0x01]) + Data(k.utf8) + Data([0]) + Data(v.utf8) + Data([0])
        }
        d += Data([0x08, 0x08])
        return d
    }
}
