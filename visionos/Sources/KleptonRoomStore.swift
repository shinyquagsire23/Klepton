import Foundation
import Security
import simd

// The rooms a player has measured, and the play area in each — shared by every
// game AVP Play has built on this headset, so a room is measured once and not in
// each game again.
//
// Shared through the Keychain: every game is signed by the same team, and a
// team's provisioning profiles allow its apps the access groups "<team>.*"
// without anything to set up in Apple's portal (an App Group would need one
// per app, and a free team cannot make them). The group is
// "<team>.avpplay.shared" (Info.plist KLSharedKeychainGroup, expanded at build
// time; Klepton.entitlements names it). Where it cannot be used, the rooms are
// kept by this game alone, and the log says so.
//
// A room is stored in a frame of its own (floor at y = 0), which no app's
// immersive space shares. Each game finds that frame again by itself: the first
// time by matching what it sees against the room's obstacles (kl_room_register),
// from then on through a world anchor of its own that visionOS brings back
// whenever the headset is in that room again (KLRoomStore.anchors).

struct KLRoomWall: Codable, Equatable {
    var center: [Float]     // room frame, metres
    var normal: [Float]     // horizontal, into the room
    var width: Float
    var height: Float
}

struct KLRoomRecord: Codable, Identifiable, Equatable {
    var id: UUID
    var created: Date
    var updated: Date
    var cell: Float
    var x0: Float
    var z0: Float
    var w: Int32
    var h: Int32
    /// One bit a cell (kl_room_pack): inside the play area.
    var play: Data
    /// One bit a cell: something stands here — what finding the room again is matched against.
    var obstacles: Data
    /// The walls the plane detection named, for games that pin their mixed reality to the room (later).
    var walls: [KLRoomWall]

    var grid: kl_room_grid { kl_room_grid(cell: cell, x0: x0, z0: z0, w: w, h: h) }
    var ncells: Int { Int(w) * Int(h) }

    func unpack(_ bits: Data, as bit: UInt8, into out: inout [UInt8]) {
        guard bits.count >= (ncells + 7) / 8, out.count == ncells else { return }
        bits.withUnsafeBytes { b in
            out.withUnsafeMutableBufferPointer { o in
                kl_room_unpack(Int32(ncells), b.bindMemory(to: UInt8.self).baseAddress, bit, o.baseAddress)
            }
        }
    }
    /// The play area, one byte a cell.
    func playMask() -> [UInt8] {
        var m = [UInt8](repeating: 0, count: ncells)
        unpack(play, as: 1, into: &m)
        return m
    }
    /// Cells with KL_ROOM_OBSTACLE set where something stands.
    func obstacleCells() -> [UInt8] {
        var m = [UInt8](repeating: 0, count: ncells)
        unpack(obstacles, as: UInt8(KL_ROOM_OBSTACLE), into: &m)
        return m
    }
    static func pack(_ mask: [UInt8], bit: UInt8) -> Data {
        var bits = [UInt8](repeating: 0, count: (mask.count + 7) / 8)
        mask.withUnsafeBufferPointer { m in
            bits.withUnsafeMutableBufferPointer { b in
                kl_room_pack(Int32(mask.count), m.baseAddress, bit, b.baseAddress)
            }
        }
        return Data(bits)
    }
    var areaM2: Float {
        let m = playMask()
        return Float(m.reduce(0) { $0 + Int($1) }) * cell * cell
    }
    /// The play area's extent along the grid's two axes, in metres — what a
    /// Guardian reports as its dimensions (kl_ovrp_set_play_area).
    var playExtent: (width: Float, depth: Float) {
        let m = playMask()
        var x0 = Int(w), x1 = -1, z0 = Int(h), z1 = -1
        for z in 0..<Int(h) {
            for x in 0..<Int(w) where m[z * Int(w) + x] != 0 {
                x0 = min(x0, x); x1 = max(x1, x); z0 = min(z0, z); z1 = max(z1, z)
            }
        }
        guard x1 >= x0, z1 >= z0 else { return (0, 0) }
        return (Float(x1 - x0 + 1) * cell, Float(z1 - z0 + 1) * cell)
    }
}

enum KLRoomStore {
    static let maxRooms = 10

    /// "<team>.avpplay.shared": as the build wrote it into Info.plist, or else
    /// with the team read off an item of this game's own, whose group is
    /// "<team>.<bundle id>".
    static let sharedGroup: String? = {
        if let g = Bundle.main.object(forInfoDictionaryKey: "KLSharedKeychainGroup") as? String,
           !g.contains("$("), g.hasSuffix(".avpplay.shared"), g.count > ".avpplay.shared".count + 1 {
            return g
        }
        let probe: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: "avpplay.rooms.probe",
                                    kSecAttrAccount as String: "team"]
        var add = probe
        add[kSecValueData as String] = Data([1])
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
        var q = probe
        q[kSecReturnAttributes as String] = true
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let attrs = out as? [String: Any], let group = attrs[kSecAttrAccessGroup as String] as? String,
              let dot = group.firstIndex(of: "."), group.distance(from: group.startIndex, to: dot) == 10
        else { return nil }
        return String(group[..<dot]) + ".avpplay.shared"
    }()

    private static func query(group: String?) -> [String: Any] {
        var q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "avpplay.rooms",
            kSecAttrAccount as String: "v1",
        ]
        if let group { q[kSecAttrAccessGroup as String] = group }
        return q
    }

    nonisolated(unsafe) private static var said = false
    private static func say(_ s: String) {
        if !said { said = true; roomLog("[room] %@", s) }
    }

    /// This game's own copy, where the shared group cannot be reached.
    private static var localURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("klepton-rooms.json")
    }

    private static func decode(_ data: Data) -> [KLRoomRecord]? {
        let raw = (try? (data as NSData).decompressed(using: .lzfse)) as Data? ?? data
        return try? JSONDecoder().decode([KLRoomRecord].self, from: raw)
    }

    static func load() -> [KLRoomRecord] {
        if let group = sharedGroup {
            var q = query(group: group)
            q[kSecReturnData as String] = true
            q[kSecMatchLimit as String] = kSecMatchLimitOne
            var out: CFTypeRef?
            let st = SecItemCopyMatching(q as CFDictionary, &out)
            if st == errSecSuccess, let d = out as? Data, let rooms = decode(d) {
                say("rooms shared through \(group): \(rooms.count)")
                return rooms
            }
            if st == errSecItemNotFound { say("no rooms in \(group) yet"); return [] }
            say("the shared rooms cannot be read (\(st)) — this game keeps its own")
        } else {
            say("no shared group in this build — this game keeps its own rooms")
        }
        guard let d = try? Data(contentsOf: localURL) else { return [] }
        return decode(d) ?? []
    }

    @discardableResult
    static func save(_ rooms: [KLRoomRecord]) -> Bool {
        let sorted = Array(rooms.sorted { $0.updated > $1.updated }.prefix(maxRooms))
        guard let json = try? JSONEncoder().encode(sorted) else { return false }
        let data = ((try? (json as NSData).compressed(using: .lzfse)) as Data?) ?? json
        if let group = sharedGroup {
            let q = query(group: group)
            var st = SecItemUpdate(q as CFDictionary, [kSecValueData as String: data] as CFDictionary)
            if st == errSecItemNotFound {
                var add = q
                add[kSecValueData as String] = data
                add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
                st = SecItemAdd(add as CFDictionary, nil)
            }
            if st == errSecSuccess {
                roomLog("[room] %d room(s) saved to %@ (%d bytes)", sorted.count, group, data.count)
                return true
            }
            roomLog("[room] saving to %@ failed (%d) — kept by this game only", group, st)
        }
        do { try data.write(to: localURL, options: .atomic); return true }
        catch { roomLog("[room] saving the rooms failed: \(error)"); return false }
    }

    static func upsert(_ room: KLRoomRecord) {
        var rooms = load().filter { $0.id != room.id }
        rooms.append(room)
        save(rooms)
    }

    static func removeAll() {
        if let group = sharedGroup { SecItemDelete(query(group: group) as CFDictionary) }
        try? FileManager.default.removeItem(at: localURL)
        UserDefaults.standard.removeObject(forKey: anchorsKey)
        roomLog("[room] all rooms removed")
    }

    // MARK: - This game's anchors

    /// room id -> the world anchor this game placed at that room's origin.
    private static let anchorsKey = "klepton.room.anchors"
    static var anchors: [UUID: UUID] {
        get {
            let raw = UserDefaults.standard.dictionary(forKey: anchorsKey) as? [String: String] ?? [:]
            var out: [UUID: UUID] = [:]
            for (k, v) in raw { if let a = UUID(uuidString: k), let b = UUID(uuidString: v) { out[a] = b } }
            return out
        }
        set {
            UserDefaults.standard.set(Dictionary(uniqueKeysWithValues: newValue.map { ($0.key.uuidString, $0.value.uuidString) }),
                                      forKey: anchorsKey)
        }
    }
}

/// The game-side settings of the play area.
enum KLRoomGuard {
    /// The whole feature: the "Measure room" button, the start only with a play
    /// area, and the edge in the game. A setting of each game (Settings › Play
    /// area); until it is touched there, what the installation says
    /// (KLRoomGuardDefault: on, unless AVP Play installs with its setting for new
    /// games off). KL_ROOM_GUARD on the launch line decides either way.
    static let enabledKey = "klepton.room.enabled"
    static var installDefault: Bool {
        (Bundle.main.object(forInfoDictionaryKey: "KLRoomGuardDefault") as? String) != "0"
    }
    static var enabled: Bool {
        if let v = ProcessInfo.processInfo.environment["KL_ROOM_GUARD"] {
            return !["", "0", "no", "off", "false"].contains(v.lowercased())
        }
        return (UserDefaults.standard.object(forKey: enabledKey) as? Bool) ?? installDefault
    }

    /// Room scale: the measured play area of the room. Stationary (standing or
    /// seated, as a Quest's "Stationary"): a circle round where the player is when
    /// the game starts, nothing to measure. A setting of each game.
    enum Style: String, CaseIterable, Identifiable {
        case roomscale, stationary
        var id: String { rawValue }
    }
    static let styleKey = "klepton.room.style"
    static var style: Style {
        if let v = ProcessInfo.processInfo.environment["KL_ROOM_STYLE"], let s = Style(rawValue: v) { return s }
        return Style(rawValue: UserDefaults.standard.string(forKey: styleKey) ?? "") ?? .roomscale
    }
    static func setStyle(_ s: Style) { UserDefaults.standard.set(s.rawValue, forKey: styleKey) }
    /// The stationary circle's radius: 1.5 m across, room to turn and reach.
    static let stationaryRadius: Float = 0.75

    enum Mode: String, CaseIterable, Identifiable {
        case wall       // a see-through wall at the edge, as on a Quest
        case room       // the game turns see-through and the room shows (mixed space)
        case off
        var id: String { rawValue }
    }
    static let modeKey = "klepton.room.mode"
    static var mode: Mode {
        if let v = ProcessInfo.processInfo.environment["KL_ROOM_MODE"], let m = Mode(rawValue: v) { return m }
        return Mode(rawValue: UserDefaults.standard.string(forKey: modeKey) ?? "") ?? .wall
    }
}

/// A room's frame from a turn about the vertical and a shift on the floor:
/// room = R(yaw) * raw + (tx, -floorY, tz) — kl_room_register's answer as a matrix.
func klRoomFromRaw(yaw: Float, tx: Float, tz: Float, floorY: Float) -> simd_float4x4 {
    var m = simd_float4x4(simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)))
    m.columns.3 = SIMD4<Float>(tx, -floorY, tz, 1)
    return m
}

/// Two answers of kl_room_register that put the room in the same place: within
/// 3 degrees and 10 cm.
func klRoomSamePlacement(_ a: kl_room_fit, _ b: kl_room_fit) -> Bool {
    abs(remainderf(a.yaw - b.yaw, 2 * .pi)) < 0.05 && simd_distance(SIMD2<Float>(a.tx, a.tz), SIMD2<Float>(b.tx, b.tz)) < 0.10
}

/// A 4x4 as the 16 floats kl_room's calls take (column-major).
func klRoomFloats(_ m: simd_float4x4) -> [Float] {
    [m.columns.0, m.columns.1, m.columns.2, m.columns.3].flatMap { [$0.x, $0.y, $0.z, $0.w] }
}
