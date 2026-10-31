import SwiftUI
import RealityKit
import ARKit
import GameController
import QuartzCore
import simd

// "Measure room" (issue #42, done the owner's way rather than as the issue
// first said): the start window opens a mixed immersive space of its own, as a
// Quest 3 does before a game.
//
//  1. Scan. Scene reconstruction meshes floor, walls and whatever stands in the
//     room; the mesh is drawn as a fine grid over the real surfaces. Rays from
//     where the player stands say which way has not been looked at yet, and the
//     card asks to turn that way.
//  2. Suggest. The largest free floor round the player, clear of anything
//     standing by 25 cm (kl_room_suggest), shown as a line on the floor that
//     fades outwards. "Fits", "Scan again" or "Draw it myself".
//  3. Draw. The room's grid stays, the suggestion goes. A ray from a
//     controller (or a hand's pinch on the floor) draws while the trigger is
//     held; letting go closes the line the shortest way. A line drawn outside
//     the area adds to it; one that reaches more than a few centimetres into it
//     cuts the shape out of it.
//  4. Save. The area and the room's obstacles and walls go to the store every
//     game shares (KLRoomStore); this game places a world anchor at the room's
//     origin, through which it finds the room again next time.
//
// The same space checks the room before a game starts (purpose .check): the
// anchor coming back, or — in a game that has not seen the room yet — the
// obstacles in view matched against each measured room. A room that is not
// recognised has to be measured before the game starts.

/// The room's lines, also into Documents/klepton-room.log: the scan and the
/// check happen before the game boots, and the game's own log is opened (and
/// emptied) by its boot — so without this, nothing of them reached a log that
/// can be fetched from the headset.
func roomLog(_ format: String, _ args: CVarArg...) {
    let line = args.isEmpty ? format : String(format: format, arguments: args)
    NSLog("%@", line)
    KLRoomLogFile.append(line)
}
enum KLRoomLogFile {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var handle: FileHandle?
    nonisolated(unsafe) private static var opened = false
    static func append(_ line: String) {
        lock.lock(); defer { lock.unlock() }
        if !opened {
            opened = true
            let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("klepton-room.log")
            // one run's worth, the one before kept beside it
            let prev = url.deletingLastPathComponent().appendingPathComponent("klepton-room.prev.log")
            try? FileManager.default.removeItem(at: prev)
            try? FileManager.default.moveItem(at: url, to: prev)
            FileManager.default.createFile(atPath: url.path, contents: nil)
            handle = try? FileHandle(forWritingTo: url)
        }
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        handle?.write(Data((f.string(from: Date()) + " " + line + "\n").utf8))
    }
}

@MainActor final class KLRoomFlow: ObservableObject {
    static let shared = KLRoomFlow()
    static let spaceID = "KleptonRoom"

    enum Purpose { case setup, check }
    enum Outcome: Equatable { case saved, recognised, cancelled, stationary }
    enum Review: Equatable { case suggested, existing, drawn }
    enum Phase: Equatable {
        case idle, starting, recognising, unknownRoom, scanning, review(Review), drawing, saving
        case done(Outcome), failed(String)
    }

    @Published private(set) var purpose: Purpose = .setup
    @Published private(set) var phase: Phase = .idle
    @Published private(set) var coverage = 0
    @Published private(set) var hint = ""
    @Published private(set) var notice = ""
    @Published private(set) var areaM2: Float = 0
    @Published private(set) var recognisedRoom = false
    @Published private(set) var canUndo = false
    /// What a line does. Automatic (the owner's rule, 2026-10-17): what it
    /// encloses lies mostly inside the area already — it is cut out; mostly
    /// outside — it is added. Add and Cut out decide it for every line.
    enum DrawMode: Int { case auto, add, cut }
    @Published var drawMode: DrawMode = .auto
    /// What the line being drawn will do, as far as can be told while drawing (its colour).
    private var cutting: Bool { drawMode == .cut }
    @Published private(set) var drawNote = ""
    /// Where the head is, on the room's floor (for "the piece the player stands in").
    private var lastHeadRoom = SIMD2<Float>(0, 0)
    /// Whether the space is open (from the start window's side).
    var active: Bool {
        switch phase { case .idle, .done, .failed: return false; default: return true }
    }
    /// The check before a start while it is only finding the room: nothing is
    /// shown in the room — no card, no grid over the walls — and the start
    /// window says what is going on. The card and the grid come once the player
    /// has something to do (room not recognised, nothing measured yet). The
    /// owner's wish (2026-10-17): the card and the grid flashing up at every
    /// start were in the way.
    var quiet: Bool {
        purpose == .check && (phase == .starting || phase == .recognising)
    }

    // --- ARKit -----------------------------------------------------------------
    private var session: ARKitSession?
    private var world: WorldTrackingProvider?
    private var scene: SceneReconstructionProvider?
    private var planes: PlaneDetectionProvider?
    private var hands: HandTrackingProvider?
    private var accessories: AccessoryTrackingProvider?
    private var accessoryAnchors: [Int: AccessoryAnchor] = [:]
    private var trackedControllers: [GCController] = []
    private var tasks: [Task<Void, Never>] = []

    // --- What was seen -----------------------------------------------------------
    /// Each mesh anchor's triangles in this space's own frame (9 floats each).
    private var tris: [UUID: (xyz: [Float], kinds: [UInt8])] = [:]
    private var meshAnchors: [UUID: MeshAnchor] = [:]
    private var meshBuilt: [UUID: TimeInterval] = [:]
    private var meshStale = Set<UUID>()
    private var wallPlanes: [UUID: PlaneAnchor] = [:]
    private var dirty = false
    private var lastMap: TimeInterval = 0
    private var lastRegister: TimeInterval = 0
    private var registering = false
    private var startedAt: TimeInterval = 0

    // --- The room ------------------------------------------------------------------
    private var rooms: [KLRoomRecord] = []
    private var room: KLRoomRecord?            // the measured room we are in, when known
    private var grid = kl_room_grid_make(0, 0, 6, 0.05)
    private var cells: [UInt8] = []
    private var play: [UInt8] = []
    private var history: [[UInt8]] = []
    /// room frame -> this space's frame; until the room is known, this space's
    /// frame with the floor at y = 0.
    private var rawFromRoom = matrix_identity_float4x4
    private var floorRaw: Float = 0
    private var floorFixed = false
    /// Whether the floor the room's frame puts at y = 0 has been compared with the floor the mesh shows.
    private var floorChecked = false
    private var gridCentred = false

    // --- Drawing ---------------------------------------------------------------------
    private var stroke: [SIMD2<Float>] = []
    private var strokeHand = -1
    private var triggerDown: [Bool] = [false, false]
    private var buttonsDown: [String: Bool] = [:]
    private var handStroke = false
    /// The system's drag on the floor is going on (the trigger, seen as a tap
    /// where the player looks). With a controller ray this counts as the
    /// trigger held; the place comes from the ray.
    private var systemDrag = false

    // --- The scene -------------------------------------------------------------------
    let root = Entity()
    private let meshRoot = Entity()
    private let areaRoot = Entity()
    private let strokeRoot = Entity()
    private var rays: [Entity] = []
    private var reticles: [Entity] = []
    let floorCatcher = Entity()
    private var card: Entity?
    private var cardPose: simd_float4x4?
    private lazy var meshMaterial: UnlitMaterial = {
        var m = UnlitMaterial(color: UIColor(white: 1.0, alpha: 1.0))
        m.triangleFillMode = .lines
        m.blending = .transparent(opacity: .init(floatLiteral: 0.32))
        return m
    }()

    // MARK: - Opening and closing

    /// From the start window, before it opens the space.
    func begin(_ purpose: Purpose) {
        self.purpose = purpose
        phase = .starting
        notice = ""; hint = ""; coverage = 0; areaM2 = 0
        recognisedRoom = false; canUndo = false
        room = nil; anchorGuess = nil; tris = [:]; meshAnchors = [:]; meshBuilt = [:]; meshStale = []; wallPlanes = [:]
        meshRoot.children.removeAll(); areaRoot.children.removeAll(); strokeRoot.children.removeAll()
        cells = []; play = []; history = []; stroke = []; strokeHand = -1
        rawFromRoom = matrix_identity_float4x4; floorRaw = 0; floorFixed = false; floorChecked = false; gridCentred = false
        cardPose = nil
        meshRoot.isEnabled = !quiet
        card?.isEnabled = !quiet
        rooms = KLRoomStore.load()
        roomLog("[room] opening the room space (%@), %d measured room(s)",
              purpose == .check ? "check before the start" : "measure", rooms.count)
    }

    func cancel() { finish(.cancelled) }
    /// "Stationary / seated" from the card: this game plays with the circle, and
    /// a start goes on without a room.
    func useStationary() {
        KLRoomGuard.setStyle(.stationary)
        roomLog("[room] stationary chosen on the card")
        finish(.stationary)
    }
    /// The card's "Close" after a failure.
    func close() { stop(); phase = .done(.cancelled) }

    private func finish(_ outcome: Outcome) {
        guard active else { return }
        roomLog("[room] done: \(outcome)")
        stop()
        phase = .done(outcome)
    }

    private func fail(_ message: String) {
        roomLog("[room] %@", message)
        stop()
        phase = .failed(message)
    }

    func stop() {
        for t in tasks { t.cancel() }
        tasks = []
        session?.stop()
        session = nil
    }

    // MARK: - The session

    func run() async {
        guard phase == .starting else { return }
        setupScene()
        guard WorldTrackingProvider.isSupported, SceneReconstructionProvider.isSupported else {
            fail(klT("This device cannot measure a room (no scene reconstruction).",
                     "Dieses Gerät kann keinen Raum vermessen (keine Szenenrekonstruktion)."))
            return
        }
        let session = ARKitSession()
        self.session = session
        let world = WorldTrackingProvider()
        let scene = SceneReconstructionProvider(modes: [.classification])
        let planes = PlaneDetectionProvider(alignments: [.vertical])
        self.world = world; self.scene = scene; self.planes = planes
        var providers: [any DataProvider] = [world, scene]
        if PlaneDetectionProvider.isSupported { providers.append(planes) }
        if HandTrackingProvider.isSupported {
            let h = HandTrackingProvider()
            hands = h
            providers.append(h)
        }
        if let acc = await makeAccessoryProvider() { providers.append(acc) }
        var wanted: [ARKitSession.AuthorizationType] = [.worldSensing]
        if hands != nil { wanted.append(.handTracking) }
        let auth = await session.requestAuthorization(for: wanted)
        guard auth[.worldSensing] == .allowed else {
            fail(klT("Measuring the room needs the permission to sense your surroundings. Allow it under Settings › Privacy & Security › Surroundings for this game, then try again.",
                     "Zum Vermessen braucht das Spiel die Erlaubnis, die Umgebung zu erfassen. Unter Einstellungen › Datenschutz & Sicherheit › Umgebung für dieses Spiel erlauben, dann erneut versuchen."))
            return
        }
        if auth[.handTracking] != .allowed { hands = nil; providers.removeAll { $0 is HandTrackingProvider } }
        do {
            try await session.run(providers)
        } catch {
            fail(klT("The room scan could not start: \(error.localizedDescription)",
                     "Die Raumerfassung konnte nicht starten: \(error.localizedDescription)"))
            return
        }
        startedAt = CACurrentMediaTime()
        if purpose == .check && rooms.isEmpty {
            notice = klT("There is no play area yet. Measure the room once, then the game starts.",
                         "Es gibt noch keinen Spielbereich. Einmal den Raum vermessen, dann startet das Spiel.")
            phase = .scanning
        } else {
            phase = purpose == .check ? .recognising : .scanning
        }
        roomLog("[room] scanning (hands: %@, controllers: %d)", hands != nil ? "yes" : "no", accessoryAnchors.count)
        tasks.append(Task { [weak self] in
            for await u in scene.anchorUpdates { self?.meshUpdate(u.anchor, removed: u.event == .removed) }
        })
        tasks.append(Task { [weak self] in
            for await u in world.anchorUpdates { self?.worldAnchor(u.anchor, removed: u.event == .removed) }
        })
        if PlaneDetectionProvider.isSupported {
            tasks.append(Task { [weak self] in
                for await u in planes.anchorUpdates {
                    guard let self else { return }
                    if u.event == .removed || u.anchor.surfaceClassification != .wall { self.wallPlanes[u.anchor.id] = nil }
                    else { self.wallPlanes[u.anchor.id] = u.anchor }
                }
            })
        }
        if let acc = accessories { consumeAccessories(acc) }
        tasks.append(Task { [weak self] in
            while !Task.isCancelled {
                self?.tick()
                try? await Task.sleep(for: .milliseconds(16))
            }
        })
    }

    private func makeAccessoryProvider() async -> AccessoryTrackingProvider? {
        guard AccessoryTrackingProvider.isSupported else { return nil }
        let spatial = KleptonControllers.spatialControllers()
        trackedControllers = spatial
        var list: [Accessory] = []
        for c in spatial {
            if let a = try? await Accessory(device: c) { list.append(a) }
        }
        let p = AccessoryTrackingProvider(accessories: list)
        accessories = p
        return p
    }

    private func consumeAccessories(_ p: AccessoryTrackingProvider) {
        tasks.append(Task { [weak self] in
            for await u in p.anchorUpdates {
                guard let self else { return }
                let hand = u.anchor.accessory.inherentChirality == .left ? 0 : 1
                self.accessoryAnchors[hand] = u.event == .removed ? nil : u.anchor
            }
        })
    }

    /// A controller paired while the space is open: the session again with a new
    /// provider (as the compositor does), world tracking and the scan carry on.
    private func controllersChanged() async {
        guard let session, let world, let scene else { return }
        accessoryAnchors = [:]
        guard let acc = await makeAccessoryProvider() else { return }
        // Making the provider waits on the accessories, and the room can be
        // recognised and the space closed meanwhile (stop()). Running the
        // stopped session's stopped providers again is not an error ARKit
        // returns but an exception it raises, which Swift cannot catch: the
        // app aborted when the Sense controllers connected just after the room
        // was recognised (Arizona Sunshine, 2026-10-31, three crash reports
        // in KLRoomFlow.controllersChanged -> ARKitSession.run).
        guard active, self.session === session else { return }
        var providers: [any DataProvider] = [world, scene, acc]
        if let planes, PlaneDetectionProvider.isSupported { providers.append(planes) }
        if let hands { providers.append(hands) }
        do { try await session.run(providers); consumeAccessories(acc) }
        catch { roomLog("[room] controllers: the session did not take the new provider: \(error)") }
    }

    // MARK: - Anchors

    private func meshUpdate(_ a: MeshAnchor, removed: Bool) {
        if removed {
            tris[a.id] = nil; meshAnchors[a.id] = nil; meshStale.remove(a.id)
            meshRoot.findEntity(named: a.id.uuidString)?.removeFromParent()
            dirty = true
            return
        }
        meshAnchors[a.id] = a
        meshStale.insert(a.id)
        let g = a.geometry
        let v = g.vertices, f = g.faces
        guard f.primitive == .triangle, v.format == .float3 || v.componentsPerVector == 3 else { return }
        let nf = f.count
        var xyz = [Float](repeating: 0, count: 9 * nf)
        var kinds = [UInt8](repeating: 0, count: nf)
        let m = klRoomFloats(a.originFromAnchorTransform)
        var cls: UnsafeRawPointer? = nil, cbytes: Int32 = 1, cstride: Int32 = 1
        if let c = g.classifications {
            cls = UnsafeRawPointer(c.buffer.contents()).advanced(by: c.offset)
            cbytes = (c.format == .uchar || c.format == .char) ? 1 : 4
            cstride = Int32(c.stride)
        }
        let n = xyz.withUnsafeMutableBufferPointer { xb in
            kinds.withUnsafeMutableBufferPointer { kb in
                m.withUnsafeBufferPointer { mb in
                    kl_room_mesh_tris(UnsafeRawPointer(v.buffer.contents()).advanced(by: v.offset),
                                      Int32(v.stride), Int32(v.count),
                                      UnsafeRawPointer(f.buffer.contents()), Int32(f.bytesPerIndex), Int32(nf),
                                      cls, cbytes, cstride, mb.baseAddress, xb.baseAddress, kb.baseAddress)
                }
            }
        }
        tris[a.id] = (Array(xyz.prefix(9 * Int(n))), Array(kinds.prefix(Int(n))))
        dirty = true
    }

    private func worldAnchor(_ a: WorldAnchor, removed: Bool) {
        guard !removed, a.isTracked else { return }
        let map = KLRoomStore.anchors
        guard let roomID = map.first(where: { $0.value == a.id })?.key else { return }
        let m = a.originFromAnchorTransform
        // The room we are in moved in this space's frame — holding the Digital
        // Crown recentres the space, and the anchor is handed over again where it
        // now is. Followed, or the area stayed where the room had been (the
        // floor 2 m under the real one, 2026-10-17).
        if let r = room, r.id == roomID {
            let moved = simd_distance(m.columns.3, rawFromRoom.columns.3)
            guard moved > 0.01 else { return }
            roomLog("[room] the room's anchor moved by %.2f m in this space (recentred?) — following it", moved)
            rawFromRoom = m
            floorRaw = m.columns.3.y
            floorChecked = false
            dirty = true; lastMap = 0
            if !play.isEmpty, play.contains(1) { showArea(play) }
            return
        }
        guard room == nil, let r = rooms.first(where: { $0.id == roomID }) else { return }
        // Not taken as it is: after a restart one came back 3 m under the floor
        // and then jumped 2.2 m, and the area hung on it, far from where it
        // belongs (2026-10-17). It is where the search for the room starts; what
        // is in view decides (verifyAnchor).
        roomLog("[room] this game's anchor for room %@ is back at (%.2f %.2f %.2f) — checking it against what is in view",
                r.id.uuidString, m.columns.3.x, m.columns.3.y, m.columns.3.z)
        anchorGuess = (r, m)
    }

    /// The room an anchor of ours says we are in, and where — until the mesh confirms it.
    private var anchorGuess: (KLRoomRecord, simd_float4x4)?

    private func verifyAnchor(_ now: TimeInterval) {
        guard room == nil, let (r, m) = anchorGuess, !registering, now - lastRegister > 1.0, !cells.isEmpty else { return }
        var pts = [Float](repeating: 0, count: 2 * 1500)
        let n = cells.withUnsafeBufferPointer { c in
            pts.withUnsafeMutableBufferPointer { p in kl_room_obstacle_points(grid, c.baseAddress, p.baseAddress, 1500) }
        }
        guard n >= 80 else { return }
        let floorPts = floorPoints()
        lastRegister = now
        registering = true
        // The anchor's room frame, as kl_room_fit in this space's floor frame (x, z only).
        let rfr = m.inverse
        var guess = kl_room_fit()
        guess.yaw = atan2(-rfr.columns.0.z, rfr.columns.0.x)
        guess.tx = rfr.columns.3.x; guess.tz = rfr.columns.3.z
        let points = Array(pts.prefix(2 * Int(n)))
        let floorY = floorRaw
        Task.detached(priority: .userInitiated) {
            let oc = r.obstacleCells()
            var fit = kl_room_fit()
            let rc = oc.withUnsafeBufferPointer { o in
                points.withUnsafeBufferPointer { p in
                    floorPts.withUnsafeBufferPointer { f in
                        kl_room_register_near_scene(r.grid, o.baseAddress, p.baseAddress, n,
                                                    f.baseAddress, Int32(floorPts.count / 2), &guess, 0.45, 0.8, &fit)
                    }
                }
            }
            await MainActor.run {
                self.registering = false
                guard self.room == nil, self.active else { return }
                guard rc == 0, fit.score >= 0.5 else {
                    roomLog("[room] the anchor does not fit what is in view (score %.2f, %d points) — searching the whole room instead",
                            fit.score, n)
                    self.anchorGuess = nil
                    return
                }
                let roomFromRaw = klRoomFromRaw(yaw: fit.yaw, tx: fit.tx, tz: fit.tz, floorY: floorY)
                let off = simd_distance(SIMD2<Float>(fit.tx, fit.tz), SIMD2<Float>(guess.tx, guess.tz))
                roomLog("[room] the anchor fits (score %.2f) after a correction of %.2f m and %.1f°",
                        fit.score, off, remainderf(fit.yaw - guess.yaw, 2 * .pi) * 180 / .pi)
                // A fresh anchor where the room really is: the game opened next finds that one.
                self.adopt(r, rawFromRoom: roomFromRaw.inverse, viaAnchor: false)
            }
        }
    }

    /// We are in a measured room, and this is where it lies.
    private func adopt(_ r: KLRoomRecord, rawFromRoom m: simd_float4x4, viaAnchor: Bool) {
        room = r
        rawFromRoom = m
        floorRaw = m.columns.3.y
        floorFixed = true
        grid = r.grid
        gridCentred = true
        recognisedRoom = true
        dirty = true
        lastMap = 0
        if purpose == .check {
            Task {
                if !viaAnchor { await placeAnchor(for: r.id) }
                finish(.recognised)
            }
            return
        }
        if !viaAnchor { Task { await placeAnchor(for: r.id) } }
        notice = klT("This room was measured before. Its play area is shown once the scan is complete.",
                     "Dieser Raum wurde schon vermessen. Sein Spielbereich erscheint, sobald die Erfassung fertig ist.")
    }

    /// This game's world anchor at the room's origin — how it finds the room
    /// again next time without matching.
    private func placeAnchor(for roomID: UUID) async {
        guard let world else { return }
        var map = KLRoomStore.anchors
        if let old = map[roomID] { try? await world.removeAnchor(forID: old) }
        let a = WorldAnchor(originFromAnchorTransform: rawFromRoom)
        do {
            try await world.addAnchor(a)
            map[roomID] = a.id
            KLRoomStore.anchors = map
            roomLog("[room] anchor %@ placed for room %@", a.id.uuidString, roomID.uuidString)
        } catch {
            roomLog("[room] the anchor for room %@ could not be placed: \(error)", roomID.uuidString)
        }
    }

    // MARK: - Each frame

    private func tick() {
        let now = CACurrentMediaTime()
        guard let world, let head = world.queryDeviceAnchor(atTimestamp: now), head.isTracked else { return }
        let hm = head.originFromAnchorTransform
        let headPos = SIMD3<Float>(hm.columns.3.x, hm.columns.3.y, hm.columns.3.z)
        placeCard(head: hm)
        card?.isEnabled = !quiet
        meshRoot.isEnabled = !quiet
        let hr = roomPoint(headPos)
        lastHeadRoom = SIMD2<Float>(hr.x, hr.z)
        if KleptonControllers.spatialControllers() != trackedControllers {
            trackedControllers = KleptonControllers.spatialControllers()
            Task { await controllersChanged() }
        }
        // Not built while quiet: the anchors stay stale and are built once the grid is shown.
        if !quiet { refreshMeshes(now) }
        if dirty && now - lastMap > 0.5 { rebuildMap(headPos: headPos); lastMap = now }

        switch phase {
        case .recognising:
            tryRegister(now)
            // Not found: the player decides — measure it, or play stationary.
            // The search goes on behind the question, and a room found then still starts the game.
            if now - startedAt > 8 {
                roomLog("[room] the room was not recognised within 8 s — asking")
                phase = .unknownRoom
            }
        case .unknownRoom:
            tryRegister(now)
        case .scanning:
            tryRegister(now)
            let (cov, worst) = updateCoverage(headPos: headPos, head: hm)
            if cov >= 85 && worst >= 0.5 && now - startedAt > 3 { propose() }
        case .drawing:
            draw(now: now)
        default:
            break
        }
        shortcuts()
        if phase != .drawing { for r in rays { r.isEnabled = false }; for r in reticles { r.isEnabled = false } }
    }

    // MARK: - The map

    private func rebuildMap(headPos: SIMD3<Float>) {
        dirty = false
        var all: [Float] = [], kinds: [UInt8] = []
        for (_, t) in tris { all.append(contentsOf: t.xyz); kinds.append(contentsOf: t.kinds) }
        let n = Int32(kinds.count)
        guard n > 0 else { return }
        // The floor the mesh shows (NaN where too little of it was seen), if it
        // agrees with the head.
        var est = all.withUnsafeBufferPointer { a in
            kinds.withUnsafeBufferPointer { k in kl_room_floor_estimate(a.baseAddress, k.baseAddress, n, .nan) }
        }
        if !est.isNaN, !(headPos.y - est > 0.5 && headPos.y - est < 2.2) { est = .nan }
        if !floorFixed {
            // The space's origin is on the floor where the player stood when it
            // opened; the mesh says better.
            floorRaw = est.isNaN ? 0 : est
            rawFromRoom = matrix_identity_float4x4
            rawFromRoom.columns.3.y = floorRaw
        } else if !est.isNaN, abs(est - floorRaw) > 0.12, !floorChecked {
            // A room found again with its floor somewhere else than the mesh's:
            // the mesh is what the player stands on.
            roomLog("[room] the room's floor was at %.2f m, the floor seen is at %.2f m — taking the one seen", floorRaw, est)
            floorRaw = est
            rawFromRoom.columns.3.y = est
            floorChecked = true
            if play.contains(1) { showArea(play) }
        } else if !est.isNaN {
            floorChecked = true
        }
        floorCatcher.position.y = floorRaw
        if !gridCentred {
            grid = kl_room_grid_make(headPos.x, headPos.z, 6, 0.05)
            gridCentred = true
        }
        let roomFromRaw = klRoomFloats(rawFromRoom.inverse)
        all.withUnsafeMutableBufferPointer { a in
            roomFromRaw.withUnsafeBufferPointer { m in kl_room_transform_tris(a.baseAddress, n, m.baseAddress) }
        }
        var c = [UInt8](repeating: 0, count: Int(kl_room_cells(grid)))
        c.withUnsafeMutableBufferPointer { cb in
            all.withUnsafeBufferPointer { a in
                kinds.withUnsafeBufferPointer { k in
                    kl_room_raster_tris(grid, cb.baseAddress, a.baseAddress, k.baseAddress, n, 0)
                }
            }
        }
        cells = c
    }

    private func roomPoint(_ raw: SIMD3<Float>) -> SIMD3<Float> {
        let p = rawFromRoom.inverse * SIMD4<Float>(raw, 1)
        return SIMD3<Float>(p.x, p.y, p.z)
    }
    private func rawPoint(_ x: Float, _ y: Float, _ z: Float) -> SIMD3<Float> {
        let p = rawFromRoom * SIMD4<Float>(x, y, z, 1)
        return SIMD3<Float>(p.x, p.y, p.z)
    }

    private func updateCoverage(headPos: SIMD3<Float>, head: simd_float4x4) -> (Int, Float) {
        guard !cells.isEmpty else { return (0, 0) }
        let u = roomPoint(headPos)
        var sec = [Float](repeating: 0, count: 8)
        let cov = cells.withUnsafeBufferPointer { c in
            sec.withUnsafeMutableBufferPointer { s in kl_room_coverage(grid, c.baseAddress, u.x, u.z, 8, s.baseAddress) }
        }
        coverage = Int(cov)
        // The emptiest sector, said relative to where the head faces.
        var worst = 0
        for k in 1..<8 where sec[k] < sec[worst] { worst = k }
        let fwdRaw = -SIMD3<Float>(head.columns.2.x, head.columns.2.y, head.columns.2.z)
        let f4 = rawFromRoom.inverse * SIMD4<Float>(fwdRaw, 0)
        let headAngle = atan2(f4.z, f4.x)
        let secAngle = (Float(worst) + 0.5) * .pi / 4
        var rel = remainderf(secAngle - headAngle, 2 * .pi)   // + = towards +z of the head's view = to the right
        if sec[worst] >= 0.5 { hint = ""; return (coverage, sec[worst]) }
        // looking down at the floor from above, +x right and +z towards the viewer:
        // a positive angle from the forward direction turns to the right.
        rel = rel * 180 / .pi
        if abs(rel) < 45 {
            hint = klT("Look ahead and a little down: the floor in front of you is not complete yet.",
                       "Schau nach vorn und etwas nach unten: der Boden vor dir fehlt noch.")
        } else if abs(rel) > 135 {
            hint = klT("Turn round: behind you is still missing.",
                       "Dreh dich um: hinter dir fehlt noch etwas.")
        } else if rel > 0 {
            hint = klT("Turn to your right: something is still missing there.",
                       "Dreh dich nach rechts: dort fehlt noch etwas.")
        } else {
            hint = klT("Turn to your left: something is still missing there.",
                       "Dreh dich nach links: dort fehlt noch etwas.")
        }
        return (coverage, sec[worst])
    }

    // MARK: - Finding a measured room by its obstacles

    private func tryRegister(_ now: TimeInterval) {
        if anchorGuess != nil { verifyAnchor(now); return }
        guard room == nil, !rooms.isEmpty, !registering, now - lastRegister > 2.0, !cells.isEmpty else { return }
        var pts = [Float](repeating: 0, count: 2 * 1500)
        let n = cells.withUnsafeBufferPointer { c in
            pts.withUnsafeMutableBufferPointer { p in kl_room_obstacle_points(grid, c.baseAddress, p.baseAddress, 1500) }
        }
        guard n >= 150 else { return }
        let floorPts = floorPoints()
        lastRegister = now
        registering = true
        let candidates = rooms
        let points = Array(pts.prefix(2 * Int(n)))
        let floorY = floorRaw
        let previous = lastWinners
        Task.detached(priority: .userInitiated) {
            var best: (KLRoomRecord, kl_room_fit)?
            var winners: [UUID: (kl_room_fit, Int)] = [:]
            for r in candidates {
                let oc = r.obstacleCells()
                var fit = kl_room_fit()
                let rc = oc.withUnsafeBufferPointer { o in
                    points.withUnsafeBufferPointer { p in
                        floorPts.withUnsafeBufferPointer { f in
                            kl_room_register_scene(r.grid, o.baseAddress, p.baseAddress, n,
                                                   f.baseAddress, Int32(floorPts.count / 2), &fit)
                        }
                    }
                }
                guard rc == 0 else { continue }
                // The same placement winning again and again is evidence too: in a
                // room full of things the next best can stay close behind the right
                // one (Beat Saber, 2026-10-17: 1.00 against 0.94..0.98, ten times).
                var streak = 1
                if let (p, k) = previous[r.id], klRoomSamePlacement(p, fit) { streak = k + 1 }
                winners[r.id] = (fit, streak)
                roomLog("[room] match against room %@: score %.2f, next best %.2f, %d points, %d floor, yaw %.1f° t (%.2f %.2f), %d in a row",
                        r.id.uuidString, fit.score, fit.second, fit.npts, Int32(floorPts.count / 2),
                        fit.yaw * 180 / .pi, fit.tx, fit.tz, Int32(streak))
                let clear = fit.score >= 0.6 && fit.score - fit.second >= 0.1
                let steady = fit.score >= 0.8 && fit.score - fit.second >= 0.03 && streak >= 3
                if clear || steady, fit.score > (best?.1.score ?? 0) { best = (r, fit) }
            }
            let found = best
            let seen = winners
            await MainActor.run {
                self.registering = false
                self.lastWinners = seen
                guard self.room == nil, self.active, let (r, fit) = found else { return }
                let roomFromRaw = klRoomFromRaw(yaw: fit.yaw, tx: fit.tx, tz: fit.tz, floorY: floorY)
                roomLog("[room] recognised room %@ by its obstacles and floor (yaw %.1f°)", r.id.uuidString, fit.yaw * 180 / .pi)
                self.adopt(r, rawFromRoom: roomFromRaw.inverse, viaAnchor: false)
            }
        }
    }

    /// Each room's last winning placement and how many searches in a row it won.
    private var lastWinners: [UUID: (kl_room_fit, Int)] = [:]

    /// Today's free floor (floor seen, nothing standing), as kl_room_register_scene takes it.
    private func floorPoints() -> [Float] {
        var f = [Float](repeating: 0, count: 2 * 1500)
        let nf = cells.withUnsafeBufferPointer { c in
            f.withUnsafeMutableBufferPointer { p in kl_room_floor_points(grid, c.baseAddress, p.baseAddress, 1500) }
        }
        return Array(f.prefix(2 * Int(nf)))
    }

    // MARK: - Suggesting

    /// After the scan, or on "Continue": the room's own area if it has one, else a suggestion.
    func propose() {
        guard !cells.isEmpty else { return }
        if let r = room, r.grid.w == grid.w, r.grid.h == grid.h {
            var m = r.playMask()
            if m.contains(1) {
                let u = lastHeadRoom
                m.withUnsafeMutableBufferPointer { p in
                    kl_room_fill_holes(grid, p.baseAddress, nil, 0)
                    kl_room_keep_main(grid, p.baseAddress, u.x, u.y)
                }
                play = m
                showArea(play)
                phase = .review(.existing)
                return
            }
        }
        suggest()
    }

    private func suggest() {
        guard let world, let head = world.queryDeviceAnchor(atTimestamp: CACurrentMediaTime()) else { return }
        let u = roomPoint(SIMD3<Float>(head.originFromAnchorTransform.columns.3.x, 0, head.originFromAnchorTransform.columns.3.z))
        var m = [UInt8](repeating: 0, count: cells.count)
        let count = cells.withUnsafeBufferPointer { c in
            m.withUnsafeMutableBufferPointer { mb in kl_room_suggest(grid, c.baseAddress, u.x, u.z, 0.25, 4.0, mb.baseAddress) }
        }
        play = m
        roomLog("[room] suggested %.1f m² round (%.2f, %.2f)", Float(count) * grid.cell * grid.cell, u.x, u.z)
        showArea(play)
        phase = .review(.suggested)
    }

    /// "Measure room" on the question after a room was not recognised.
    func measureNow() {
        notice = klT("Measure the room once; then the game starts.", "Einmal den Raum vermessen, dann startet das Spiel.")
        startedAt = CACurrentMediaTime()
        phase = .scanning
    }

    func scanAgain() {
        play = []; history = []
        showArea(nil)
        startedAt = CACurrentMediaTime()
        notice = ""
        phase = .scanning
    }

    /// What the card showed before "Draw it myself": its Cancel goes back there.
    private var beforeDrawing: (Review, [UInt8])?

    func startDrawing() {
        if case .review(let kind) = phase { beforeDrawing = (kind, play) } else { beforeDrawing = nil }
        history = []
        play = [UInt8](repeating: 0, count: cells.isEmpty ? Int(kl_room_cells(grid)) : cells.count)
        showArea(nil)
        stroke = []; strokeHand = -1
        canUndo = false
        drawMode = .auto
        drawNote = ""
        phase = .drawing
    }

    /// Cancel while drawing: back to the suggestion (or the area) shown before.
    func cancelDrawing() {
        stroke = []; strokeHand = -1; showStroke()
        guard let (kind, mask) = beforeDrawing else { scanAgain(); return }
        play = mask
        showArea(play)
        phase = .review(kind)
        roomLog("[room] drawing cancelled — back to the %@ area", kind == .existing ? "saved" : kind == .drawn ? "drawn" : "suggested")
    }

    func finishDrawing() {
        guard play.contains(1) else { return }
        phase = .review(.drawn)
        showArea(play)
    }

    func undo() {
        guard let last = history.popLast() else { return }
        play = last
        canUndo = !history.isEmpty
        showArea(play)
    }

    func clearDrawing() {
        guard play.contains(1) else { return }
        history.append(play)
        play = [UInt8](repeating: 0, count: play.count)
        canUndo = true
        showArea(nil)
    }

    // MARK: - Drawing

    private func floorHit(origin o: SIMD3<Float>, dir d: SIMD3<Float>) -> SIMD3<Float>? {
        guard d.y < -0.05 else { return nil }
        let t = (floorRaw - o.y) / d.y
        guard t > 0, t < 8 else { return nil }
        return o + d * t
    }

    private func draw(now: TimeInterval) {
        // Controllers first; a hand's pinch on the floor is the gesture's (handDrag).
        var pressed = [false, false]
        for c in KleptonControllers.spatialControllers() {
            let hand = KleptonControllers.hand(of: c)
            let v = c.input.buttons["Trigger"]?.pressedInput.value ?? 0
            pressed[hand] = triggerDown[hand] ? v > 0.35 : v > 0.55
        }
        if systemDrag, !pressed[0], !pressed[1] {
            // the hand whose ray is drawing, else the right one, else the one there is
            let h = strokeHand == 0 || strokeHand == 1 ? strokeHand : (accessoryAnchors[1] != nil ? 1 : 0)
            pressed[h] = true
        }
        for hand in 0...1 {
            rays[hand].isEnabled = false
            reticles[hand].isEnabled = false
            guard let stored = accessoryAnchors[hand], let acc = accessories else { triggerDown[hand] = pressed[hand]; continue }
            let a = acc.predictAnchor(for: stored, at: now) ?? stored
            let m = a.accessory.locations.contains(.aim)
                ? a.coordinateSpace(for: .aim, correction: .none).ancestorFromSpaceTransformFloat().matrix
                : a.originFromAnchorTransform
            let o = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
            let d = simd_normalize(-SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z))
            let hit = floorHit(origin: o, dir: d)
            let len = hit.map { simd_length($0 - o) } ?? 3.0
            var t = m
            t.columns.0 = SIMD4<Float>(simd_normalize(SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z)), 0)
            t.columns.2 = SIMD4<Float>(-d, 0)
            t.columns.1 = SIMD4<Float>(simd_normalize(simd_cross(-d, SIMD3<Float>(t.columns.0.x, t.columns.0.y, t.columns.0.z))), 0)
            rays[hand].transform = Transform(matrix: t)
            rays[hand].scale = SIMD3<Float>(1, 1, len)
            rays[hand].isEnabled = true
            if let hit {
                reticles[hand].position = hit + SIMD3<Float>(0, 0.004, 0)
                reticles[hand].isEnabled = true
            }
            let was = triggerDown[hand]
            triggerDown[hand] = pressed[hand]
            if pressed[hand] && !was && strokeHand < 0 { strokeHand = hand; stroke = [] }
            if hand == strokeHand {
                if pressed[hand], let hit { addStrokePoint(roomPoint(hit)) }
                if !pressed[hand] { closeStroke() }
            }
        }
    }

    private func addStrokePoint(_ p: SIMD3<Float>) {
        let q = SIMD2<Float>(p.x, p.z)
        if let last = stroke.last, simd_distance(last, q) < 0.03 { return }
        stroke.append(q)
        showStroke()
    }

    /// The pinch on the floor, for a player without controllers.
    /// Not while a controller is in use: its trigger is also the system's tap at
    /// where the player LOOKS, and that tap reached this gesture — the line grew
    /// where the eyes were, not where the ray pointed.
    func handDrag(at scenePoint: SIMD3<Float>) {
        guard phase == .drawing else { return }
        if !accessoryAnchors.isEmpty {
            if !systemDrag { roomLog("[room] the trigger came as the system's tap — drawing along the ray") }
            systemDrag = true
            return
        }
        guard strokeHand < 0 || strokeHand == 2 else { return }
        if strokeHand < 0 { strokeHand = 2; stroke = [] }
        handStroke = true
        addStrokePoint(roomPoint(SIMD3<Float>(scenePoint.x, floorRaw, scenePoint.z)))
    }
    func handDragEnded() {
        systemDrag = false
        guard strokeHand == 2 else { return }
        closeStroke()
    }

    /// Letting go. What the line encloses — by itself, closed the shortest way,
    /// or together with the area (a line across a gap or a bay) — is added, or
    /// with "Cut out" taken away. The area never keeps a hole or a second piece:
    /// what it encloses is filled, a piece drawn up to a metre away is joined
    /// to it, and after a cut only the piece the player stands in stays.
    private func closeStroke() {
        defer { stroke = []; strokeHand = -1; showStroke() }
        guard stroke.count >= 2, !play.isEmpty else { return }
        var length: Float = 0
        for k in 1..<stroke.count { length += simd_distance(stroke[k - 1], stroke[k]) }
        guard length >= 0.15 else { roomLog("[room] a stroke of %.2f m — too short, dropped", length); return }
        var flat: [Float] = stroke.flatMap { [$0.x, $0.y] }
        var area: Float = 0
        for k in 0..<stroke.count {
            let a = stroke[k], b = stroke[(k + 1) % stroke.count]
            area += a.x * b.y - b.x * a.y
        }
        area = abs(area) * 0.5
        // The line itself, 8 cm wide, and what it encloses when closed.
        var shape = [UInt8](repeating: 0, count: play.count)
        shape.withUnsafeMutableBufferPointer { sh in
            flat.withUnsafeMutableBufferPointer { f in
                if stroke.count >= 3 && area > 0.02 {
                    kl_room_fill_polygon(grid, sh.baseAddress, f.baseAddress, Int32(stroke.count), 1)
                }
                kl_room_fill_polyline(grid, sh.baseAddress, f.baseAddress, Int32(stroke.count), 0.04, 1)
            }
        }
        let before = play
        var after = play
        let u = lastHeadRoom
        var gap: Float = -1
        // Automatic: the share of what the line takes in that is in the area already.
        var inside = 0, total = 0
        for c in 0..<shape.count where shape[c] != 0 { total += 1; if play[c] != 0 { inside += 1 } }
        let share = total > 0 ? Float(inside) / Float(total) : 0
        let cutting = drawMode == .cut || (drawMode == .auto && share > 0.5)
        after.withUnsafeMutableBufferPointer { p in
            shape.withUnsafeBufferPointer { sh in
                if cutting {
                    kl_room_combine(grid, p.baseAddress, sh.baseAddress, Int32(KL_ROOM_SUBTRACT))
                    kl_room_keep_main(grid, p.baseAddress, u.x, u.y)
                } else {
                    gap = kl_room_add_piece(grid, p.baseAddress, sh.baseAddress, 1.0)
                    kl_room_close(grid, p.baseAddress, 0.06)
                }
                kl_room_fill_holes(grid, p.baseAddress, nil, 0)
                kl_room_keep_main(grid, p.baseAddress, u.x, u.y)
            }
        }
        if !cutting, gap > 1.0 {
            drawNote = klT("That piece is more than a metre from the area. Draw it next to the area so they join.",
                           "Das Stück ist mehr als einen Meter vom Bereich entfernt. Zeichne es an den Bereich heran, damit es dazukommt.")
            roomLog("[room] stroke %.1f m from the area — not added", gap)
            return
        }
        guard after != before else {
            drawNote = cutting
                ? klT("Nothing was cut: draw the line across the edge of the area, round the piece to take away.",
                      "Nichts herausgeschnitten: zieh die Linie über den Rand des Bereichs, um das Stück herum, das weg soll.")
                : ""
            roomLog("[room] stroke of %.2f m changed nothing (%@)", length, cutting ? "cut" : "add")
            return
        }
        drawNote = ""
        history.append(before)
        canUndo = true
        play = after
        roomLog("[room] stroke of %.2f m, encloses %.1f m², %.0f%% of it inside, %@ (gap %.2f m) — area now %.1f m²",
                length, area, share * 100, cutting ? "cut out" : "added", gap,
                Float(play.reduce(0) { $0 + Int($1) }) * grid.cell * grid.cell)
        showArea(play)
    }

    // MARK: - Saving

    func save() {
        guard play.contains(1), !cells.isEmpty else { return }
        phase = .saving
        let now = Date()
        var obst = cells.map { $0 & UInt8(KL_ROOM_OBSTACLE) }
        if let r = room, r.grid.w == grid.w, r.grid.h == grid.h {
            // What this scan did not see stays as it was measured before.
            let old = r.obstacleCells()
            for c in 0..<obst.count where cells[c] == 0 { obst[c] = old[c] }
        }
        let roomFromRaw = rawFromRoom.inverse
        let walls: [KLRoomWall] = wallPlanes.values.compactMap { p in
            let m = roomFromRaw * p.originFromAnchorTransform * p.geometry.extent.anchorFromExtentTransform
            let c1 = SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z)
            let c2 = SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
            var n = abs(c1.y) < abs(c2.y) ? c1 : c2
            n.y = 0
            guard simd_length(n) > 0.5 else { return nil }
            n = simd_normalize(n)
            return KLRoomWall(center: [m.columns.3.x, m.columns.3.y, m.columns.3.z], normal: [n.x, n.y, n.z],
                              width: p.geometry.extent.width, height: p.geometry.extent.height)
        }
        let record = KLRoomRecord(id: room?.id ?? UUID(), created: room?.created ?? now, updated: now,
                                  cell: grid.cell, x0: grid.x0, z0: grid.z0, w: grid.w, h: grid.h,
                                  play: KLRoomRecord.pack(play, bit: 1),
                                  obstacles: KLRoomRecord.pack(obst, bit: UInt8(KL_ROOM_OBSTACLE)),
                                  walls: walls)
        KLRoomStore.upsert(record)
        roomLog("[room] saved room %@: %.1f m² play area, %d walls", record.id.uuidString, record.areaM2, walls.count)
        room = record
        Task {
            await placeAnchor(for: record.id)
            finish(.saved)
        }
    }

    // MARK: - Controller shortcuts

    /// The face buttons as the card's buttons, for hands that are holding
    /// controllers: right ✕ the main one, right ○ the second, left △ "Draw it
    /// myself", left □ "Cancel".
    private func shortcuts() {
        var now: [String: Bool] = [:]
        for c in KleptonControllers.spatialControllers() {
            let side = KleptonControllers.hand(of: c) == 0 ? "L" : "R"
            for b in ["Button A", "Button B"] {
                now[side + b] = c.input.buttons[b]?.pressedInput.isPressed ?? false
            }
        }
        func edge(_ k: String) -> Bool { (now[k] ?? false) && !(buttonsDown[k] ?? false) }
        defer { buttonsDown = now }
        if edge("LButton A") { if phase == .drawing { cancelDrawing() } else { cancel() }; return }
        switch phase {
        case .scanning:
            if edge("RButton A"), coverage >= 50 { propose() }
            if edge("RButton B") { scanAgain() }
        case .review(let kind):
            if edge("RButton A"), play.contains(1) { save() }
            if edge("RButton B") { scanAgain() }
            if edge("LButton B") { startDrawing() }
            _ = kind
        case .drawing:
            if edge("RButton A") { finishDrawing() }
            if edge("RButton B") { undo() }
            if edge("LButton B") { drawMode = DrawMode(rawValue: (drawMode.rawValue + 1) % 3) ?? .auto }
        default: break
        }
    }

    // MARK: - The scene

    private func setupScene() {
        guard rays.isEmpty else { return }
        root.addChild(meshRoot); root.addChild(areaRoot); root.addChild(strokeRoot)
        for hand in 0...1 {
            let ray = Entity()
            var mat = UnlitMaterial(color: hand == 0 ? UIColor(red: 0.35, green: 0.8, blue: 1, alpha: 1)
                                                    : UIColor(red: 0.5, green: 0.9, blue: 1, alpha: 1))
            mat.blending = .transparent(opacity: .init(floatLiteral: 0.85))
            // A unit-long beam along -z, stretched to the floor each frame.
            let beam = ModelEntity(mesh: .generateBox(width: 0.004, height: 0.004, depth: 1), materials: [mat])
            beam.position = SIMD3<Float>(0, 0, -0.5)
            ray.addChild(beam)
            ray.isEnabled = false
            root.addChild(ray); rays.append(ray)
            let ret = ModelEntity(mesh: .generateCylinder(height: 0.003, radius: 0.04), materials: [mat])
            ret.isEnabled = false
            root.addChild(ret); reticles.append(ret)
        }
        floorCatcher.components.set(CollisionComponent(shapes: [.generateBox(width: 30, height: 0.02, depth: 30)]))
        floorCatcher.components.set(InputTargetComponent())
        root.addChild(floorCatcher)
    }

    func installCard(_ e: Entity) {
        // A space opened again brings a new card; the last one's must go.
        if let old = card, old !== e { old.removeFromParent() }
        card = e
        e.isEnabled = !quiet
        root.addChild(e)
    }

    /// The card stays in view without being glued to the face: it follows when
    /// the head has turned more than 35 degrees away from it, or walked off.
    private func placeCard(head: simd_float4x4) {
        guard let card else { return }
        let hp = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
        var fwd = -SIMD3<Float>(head.columns.2.x, 0, head.columns.2.z)
        if simd_length(fwd) < 1e-3 { fwd = SIMD3<Float>(0, 0, -1) }
        fwd = simd_normalize(fwd)
        let lower: Float = phase == .drawing ? 0.35 : 0.15
        let target = hp + fwd * 1.1 - SIMD3<Float>(0, lower, 0)
        var pose = cardPose
        if pose == nil { pose = Self.facing(target, from: hp) }
        let at = SIMD3<Float>(pose!.columns.3.x, pose!.columns.3.y, pose!.columns.3.z)
        var toCard = at - hp; toCard.y = 0
        let off = simd_length(toCard) < 1e-3 ? 0 : acos(min(1, max(-1, simd_dot(simd_normalize(toCard), fwd))))
        if off > 35 * .pi / 180 || abs(simd_length(toCard) - 1.1) > 0.5 || abs(at.y - target.y) > 0.3 {
            let next = at + (target - at) * 0.12
            pose = Self.facing(next, from: hp)
        }
        cardPose = pose
        card.transform = Transform(matrix: pose!)
    }

    private static func facing(_ p: SIMD3<Float>, from head: SIMD3<Float>) -> simd_float4x4 {
        var back = head - p; back.y = 0
        back = simd_length(back) < 1e-3 ? SIMD3<Float>(0, 0, 1) : simd_normalize(back)
        let right = simd_normalize(simd_cross(SIMD3<Float>(0, 1, 0), back))
        return simd_float4x4(columns: (SIMD4<Float>(right, 0), SIMD4<Float>(0, 1, 0, 0),
                                       SIMD4<Float>(back, 0), SIMD4<Float>(p, 1)))
    }

    /// The scanned mesh as a fine grid over the real surfaces, rebuilt at most
    /// once a second an anchor.
    private func refreshMeshes(_ now: TimeInterval) {
        var budget = 3
        for id in meshStale where budget > 0 {
            if let t = meshBuilt[id], now - t < 1.0 { continue }
            meshStale.remove(id)
            guard let a = meshAnchors[id] else { continue }
            meshBuilt[id] = now
            budget -= 1
            buildMesh(a)
        }
    }

    private func buildMesh(_ a: MeshAnchor) {
        let g = a.geometry
        let v = g.vertices, f = g.faces
        guard f.primitive == .triangle else { return }
        var pos: [SIMD3<Float>] = []
        pos.reserveCapacity(v.count)
        let vb = UnsafeRawPointer(v.buffer.contents()).advanced(by: v.offset)
        for i in 0..<v.count {
            let p = vb.advanced(by: i * v.stride).assumingMemoryBound(to: Float.self)
            pos.append(SIMD3<Float>(p[0], p[1], p[2]))
        }
        var idx: [UInt32] = []
        idx.reserveCapacity(f.count * 3)
        let ib = UnsafeRawPointer(f.buffer.contents())
        if f.bytesPerIndex == 2 {
            let p = ib.assumingMemoryBound(to: UInt16.self)
            for k in 0..<(f.count * 3) { idx.append(UInt32(p[k])) }
        } else {
            let p = ib.assumingMemoryBound(to: UInt32.self)
            for k in 0..<(f.count * 3) { idx.append(p[k]) }
        }
        var d = MeshDescriptor(name: "room-\(a.id.uuidString)")
        d.positions = MeshBuffers.Positions(pos)
        d.primitives = .triangles(idx)
        guard let mesh = try? MeshResource.generate(from: [d]) else { return }
        let name = a.id.uuidString
        if let e = meshRoot.findEntity(named: name) as? ModelEntity {
            e.model?.mesh = mesh
            e.transform = Transform(matrix: a.originFromAnchorTransform)
        } else {
            let e = onFloor(ModelEntity(mesh: mesh, materials: [meshMaterial]), .grid)
            e.name = name
            e.transform = Transform(matrix: a.originFromAnchorTransform)
            meshRoot.addChild(e)
        }
    }

    /// Everything drawn on the floor is in one sort group, drawn in this order
    /// after the room's grid, none of it writing or testing depth: the floor's
    /// mesh is a few centimetres noisy, and a line lying in it, or two pieces of
    /// line lying on one another, flickered where the depth test picked one or
    /// the other from frame to frame.
    private let floorGroup = ModelSortGroup(depthPass: nil)
    private enum FloorLayer: Int32 { case grid = 0, fill = 1, fade = 2, line = 6, stroke = 7 }
    private func onFloor(_ e: ModelEntity, _ layer: FloorLayer, offset: Int32 = 0) -> ModelEntity {
        e.components.set(ModelSortGroupComponent(group: floorGroup, order: layer.rawValue + offset))
        return e
    }
    private static func floorMaterial(_ color: UIColor, opacity: Float) -> UnlitMaterial {
        var m = UnlitMaterial(color: color)
        m.blending = .transparent(opacity: .init(floatLiteral: opacity))
        m.faceCulling = .none
        m.writesDepth = false
        m.readsDepth = false
        return m
    }
    private static let lineColor = UIColor(red: 0.35, green: 0.85, blue: 1.0, alpha: 1)

    /// The play area on the floor: a thin line at its edge with a short glow
    /// outwards, and a faint fill. nil hides it.
    private func showArea(_ mask: [UInt8]?) {
        areaRoot.children.removeAll()
        guard let mask, mask.contains(1) else { areaM2 = 0; return }
        let count = mask.reduce(0) { $0 + Int($1) }
        areaM2 = Float(count) * grid.cell * grid.cell
        var pts = [Float](repeating: 0, count: 2 * 40000)
        var lens = [Int32](repeating: 0, count: 256)
        let loops = mask.withUnsafeBufferPointer { m in
            pts.withUnsafeMutableBufferPointer { p in
                lens.withUnsafeMutableBufferPointer { l in kl_room_contours(grid, m.baseAddress, p.baseAddress, 40000, l.baseAddress, 256) }
            }
        }
        if loops > 0 {
            var start = 0
            var rings: [[SIMD2<Float>]] = []
            for len in lens.prefix(Int(loops)).map(Int.init) {
                let loop = (0..<len).map { SIMD2<Float>(pts[2 * (start + $0)], pts[2 * (start + $0) + 1]) }
                start += len
                rings.append(Self.chaikin(Self.chaikin(loop)))
            }
            // The line, 2.5 cm across on the edge; the glow outside it in four
            // bands of falling opacity (a texture's opacity was not applied).
            let bands: [(Float, Float, Float)] = [(0.0125, 0.04, 0.38), (0.04, 0.07, 0.22), (0.07, 0.10, 0.11), (0.10, 0.13, 0.04)]
            for (k, b) in bands.enumerated() {
                if let e = band(rings, from: b.0, to: b.1, opacity: b.2) { areaRoot.addChild(onFloor(e, .fade, offset: Int32(k))) }
            }
            if let e = band(rings, from: -0.0125, to: 0.0125, opacity: 0.95) { areaRoot.addChild(onFloor(e, .line)) }
        }
        if let fill = makeFill(mask) { areaRoot.addChild(onFloor(fill, .fill)) }
    }

    /// A ribbon along each ring between `from` and `to` metres outside it.
    private func band(_ rings: [[SIMD2<Float>]], from: Float, to: Float, opacity: Float) -> ModelEntity? {
        var positions: [SIMD3<Float>] = [], idx: [UInt32] = []
        let y: Float = 0.01
        for loop in rings {
            let n = loop.count
            guard n >= 3 else { continue }
            let base = UInt32(positions.count)
            for k in 0..<n {
                let prev = loop[(k + n - 1) % n], next = loop[(k + 1) % n]
                var t = next - prev
                t = simd_length(t) < 1e-5 ? SIMD2<Float>(1, 0) : simd_normalize(t)
                let out = SIMD2<Float>(t.y, -t.x)       // the area is on the left
                let a = loop[k] + out * from, b = loop[k] + out * to
                positions.append(rawPoint(a.x, y, a.y))
                positions.append(rawPoint(b.x, y, b.y))
            }
            for k in 0..<UInt32(n) {
                let a = base + 2 * k, b = base + 2 * ((k + 1) % UInt32(n))
                idx += [a, a + 1, b + 1, a, b + 1, b]
            }
        }
        guard !idx.isEmpty else { return nil }
        var d = MeshDescriptor(name: "room-band")
        d.positions = MeshBuffers.Positions(positions)
        d.primitives = .triangles(idx)
        guard let mesh = try? MeshResource.generate(from: [d]) else { return nil }
        return ModelEntity(mesh: mesh, materials: [Self.floorMaterial(Self.lineColor, opacity: opacity)])
    }

    private func makeFill(_ mask: [UInt8]) -> ModelEntity? {
        var positions: [SIMD3<Float>] = [], idx: [UInt32] = []
        let w = Int(grid.w), h = Int(grid.h), c = grid.cell
        let y: Float = 0.008
        for j in 0..<h {
            var i = 0
            while i < w {
                guard mask[j * w + i] != 0 else { i += 1; continue }
                let i0 = i
                while i < w && mask[j * w + i] != 0 { i += 1 }
                let x0 = grid.x0 + Float(i0) * c, x1 = grid.x0 + Float(i) * c
                let z0 = grid.z0 + Float(j) * c, z1 = z0 + c
                let base = UInt32(positions.count)
                positions += [rawPoint(x0, y, z0), rawPoint(x1, y, z0), rawPoint(x1, y, z1), rawPoint(x0, y, z1)]
                idx += [base, base + 1, base + 2, base, base + 2, base + 3]
            }
        }
        guard !idx.isEmpty else { return nil }
        var d = MeshDescriptor(name: "room-fill")
        d.positions = MeshBuffers.Positions(positions)
        d.primitives = .triangles(idx)
        guard let mesh = try? MeshResource.generate(from: [d]) else { return nil }
        return ModelEntity(mesh: mesh, materials: [Self.floorMaterial(UIColor(red: 0.35, green: 0.8, blue: 1.0, alpha: 1), opacity: 0.08)])
    }

    /// The line being drawn, as a thin ribbon on the floor.
    private func showStroke() {
        strokeRoot.children.removeAll()
        guard stroke.count >= 2 else { return }
        var positions: [SIMD3<Float>] = [], idx: [UInt32] = []
        let y: Float = 0.012
        for k in 0..<stroke.count {
            let a = stroke[max(0, k - 1)], b = stroke[min(stroke.count - 1, k + 1)]
            var t = b - a
            t = simd_length(t) < 1e-5 ? SIMD2<Float>(1, 0) : simd_normalize(t)
            let n = SIMD2<Float>(-t.y, t.x) * 0.01
            let p = stroke[k]
            positions.append(rawPoint(p.x - n.x, y, p.y - n.y))
            positions.append(rawPoint(p.x + n.x, y, p.y + n.y))
            if k > 0 {
                let base = UInt32(2 * (k - 1))
                idx += [base, base + 1, base + 3, base, base + 3, base + 2]
            }
        }
        var d = MeshDescriptor(name: "room-stroke")
        d.positions = MeshBuffers.Positions(positions)
        d.primitives = .triangles(idx)
        guard let mesh = try? MeshResource.generate(from: [d]) else { return }
        let colour = cutting ? UIColor(red: 1.0, green: 0.35, blue: 0.3, alpha: 1) : UIColor(red: 1.0, green: 0.85, blue: 0.3, alpha: 1)
        let e = ModelEntity(mesh: mesh, materials: [Self.floorMaterial(colour, opacity: 1)])
        strokeRoot.addChild(onFloor(e, .stroke))
    }

    private static func chaikin(_ p: [SIMD2<Float>]) -> [SIMD2<Float>] {
        guard p.count >= 3 else { return p }
        var out: [SIMD2<Float>] = []
        out.reserveCapacity(p.count * 2)
        for k in 0..<p.count {
            let a = p[k], b = p[(k + 1) % p.count]
            out.append(a * 0.75 + b * 0.25)
            out.append(a * 0.25 + b * 0.75)
        }
        return out
    }

}

// MARK: - The space and its card

struct KleptonRoomSpace: View {
    @ObservedObject private var flow = KLRoomFlow.shared
    @Environment(\.dismissImmersiveSpace) private var dismissSpace

    var body: some View {
        RealityView { content, attachments in
            content.add(flow.root)
            if let card = attachments.entity(for: "card") { flow.installCard(card) }
        } attachments: {
            Attachment(id: "card") { KLRoomCard(flow: flow) }
        }
        .gesture(DragGesture(minimumDistance: 0)
            .targetedToEntity(flow.floorCatcher)
            .onChanged { v in flow.handDrag(at: v.convert(v.location3D, from: .local, to: .scene)) }
            .onEnded { _ in flow.handDragEnded() })
        .task { await flow.run() }
        .onChange(of: flow.phase) { _, phase in
            if case .done = phase { Task { await dismissSpace() } }
        }
        .onDisappear {
            flow.stop()
            if flow.active { flow.cancel() }
        }
    }
}

struct KLRoomCard: View {
    @ObservedObject var flow: KLRoomFlow

    private func m2(_ v: Float) -> String { String(format: "%.1f m²", v) }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            switch flow.phase {
            case .starting:
                title(klT("Measure room", "Raum vermessen"))
                ProgressView()
            case .recognising:
                title(klT("Finding the room …", "Raum wird erkannt …"))
                Text(klT("Look round the room for a moment.", "Schau dich kurz im Raum um."))
                ProgressView()
                buttons { stationaryButton; cancelButton }
            case .unknownRoom:
                title(klT("Room not recognised", "Raum nicht erkannt"))
                Text(klT("This room has not been measured, or it is not recognised from here. Measure it, or play stationary — standing on the spot or seated.",
                         "Dieser Raum ist nicht vermessen oder wird von hier aus nicht erkannt. Vermiss ihn, oder spiel stationär – auf der Stelle stehend oder sitzend."))
                buttons {
                    Button(klT("Measure room", "Raum vermessen")) { flow.measureNow() }
                        .buttonStyle(.borderedProminent)
                    stationaryButton
                    cancelButton
                }
            case .scanning:
                title(klT("Measure room", "Raum vermessen"))
                if !flow.notice.isEmpty { Text(flow.notice).foregroundStyle(.yellow) }
                Text(klT("Look round slowly, at the floor too: walls, furniture and the free floor are being captured. The grid shows what has been seen.",
                         "Schau dich langsam um, auch auf den Boden: Wände, Möbel und freier Boden werden erfasst. Das Gitter zeigt, was schon erfasst ist."))
                ProgressView(value: Double(flow.coverage), total: 100)
                if !flow.hint.isEmpty {
                    Label(flow.hint, systemImage: "arrow.triangle.2.circlepath").font(.headline)
                }
                buttons {
                    if flow.coverage >= 50 {
                        Button(klT("Continue", "Weiter")) { flow.propose() }
                            .buttonStyle(.borderedProminent)
                    }
                    stationaryButton
                    cancelButton
                }
                keys(klT("✕ Continue · ○ Start again · □ Cancel", "✕ Weiter · ○ Neu beginnen · □ Abbrechen"))
            case .review(let kind):
                title(kind == .existing ? klT("Play area of this room", "Spielbereich dieses Raums")
                      : kind == .drawn ? klT("Your play area", "Dein Spielbereich")
                      : klT("Suggested play area", "Vorgeschlagener Spielbereich"))
                if flow.areaM2 < 0.05 {
                    Text(klT("No free floor was found round you. Draw the play area yourself, or make some room and scan again.",
                             "Um dich herum wurde kein freier Boden gefunden. Zeichne den Spielbereich selbst, oder schaffe etwas Platz und scanne erneut."))
                        .foregroundStyle(.yellow)
                } else {
                    Text(klT("\(m2(flow.areaM2)) — the line on the floor. Does it fit?",
                             "\(m2(flow.areaM2)) – die Linie auf dem Boden. Passt sie?"))
                    if flow.areaM2 < 1.0 {
                        Text(klT("That is very little room to play in.", "Das ist sehr wenig Platz zum Spielen."))
                            .foregroundStyle(.yellow)
                    }
                }
                buttons {
                    Button(klT("Fits — save", "Passt – speichern")) { flow.save() }
                        .buttonStyle(.borderedProminent)
                        .disabled(flow.areaM2 < 0.05)
                    Button(klT("Scan again", "Neu scannen")) { flow.scanAgain() }
                    Button(klT("Draw it myself", "Selbst zeichnen")) { flow.startDrawing() }
                    cancelButton
                }
                keys(klT("✕ Save · ○ Scan again · △ Draw · □ Cancel", "✕ Speichern · ○ Neu scannen · △ Zeichnen · □ Abbrechen"))
            case .drawing:
                title(klT("Draw the play area", "Spielbereich zeichnen"))
                Text(klT("Point a controller at the floor and hold the trigger to draw (or pinch and drag with a hand). Letting go closes the line. If what it encloses lies mostly outside the area, it is added — a line across a gap closes it; if mostly inside, it is cut out. Holes and separate pieces never stay.",
                         "Zeig mit einem Controller auf den Boden und halte den Abzug, um zu zeichnen (oder mit der Hand zusammenkneifen und ziehen). Loslassen schließt die Linie. Liegt das Umschlossene größtenteils außerhalb des Bereichs, kommt es dazu – eine Linie über eine Lücke schließt sie; liegt es größtenteils darin, wird es herausgeschnitten. Löcher und einzelne Inseln bleiben nie."))
                Picker(klT("Drawing", "Zeichnen"), selection: $flow.drawMode) {
                    Text(klT("Automatic", "Automatisch")).tag(KLRoomFlow.DrawMode.auto)
                    Text(klT("Add", "Hinzufügen")).tag(KLRoomFlow.DrawMode.add)
                    Text(klT("Cut out", "Herausschneiden")).tag(KLRoomFlow.DrawMode.cut)
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 480)
                if !flow.drawNote.isEmpty { Text(flow.drawNote).foregroundStyle(.yellow) }
                if flow.areaM2 > 0 { Text(m2(flow.areaM2)).font(.headline) }
                buttons {
                    Button(klT("Done", "Fertig")) { flow.finishDrawing() }
                        .buttonStyle(.borderedProminent)
                        .disabled(flow.areaM2 < 0.05)
                    Button(klT("Undo", "Rückgängig")) { flow.undo() }.disabled(!flow.canUndo)
                    Button(klT("Clear", "Alles löschen")) { flow.clearDrawing() }.disabled(flow.areaM2 == 0)
                    Button(klT("Cancel", "Abbrechen"), role: .cancel) { flow.cancelDrawing() }
                }
                keys(klT("✕ Done · ○ Undo · △ Automatic / add / cut · □ Back", "✕ Fertig · ○ Rückgängig · △ Automatisch / Hinzufügen / Schneiden · □ Zurück"))
            case .saving:
                title(klT("Saving …", "Wird gespeichert …"))
                ProgressView()
            case .failed(let message):
                title(klT("Measure room", "Raum vermessen"))
                Text(message).foregroundStyle(.yellow)
                buttons { Button(klT("Close", "Schließen")) { flow.close() } }
            case .idle, .done:
                EmptyView()
            }
        }
        .padding(28)
        .frame(width: 700, alignment: .leading)
        .glassBackgroundEffect()
    }

    private func title(_ s: String) -> some View { Text(s).font(.title.bold()) }
    private func keys(_ s: String) -> some View {
        Text(klT("Controllers: ", "Controller: ") + s).font(.caption).foregroundStyle(.secondary)
    }
    private var cancelButton: some View { Button(klT("Cancel", "Abbrechen"), role: .cancel) { flow.cancel() } }
    private var stationaryButton: some View {
        Button(klT("Stationary / seated", "Stationär / sitzend")) { flow.useStationary() }
    }
    @ViewBuilder private func buttons<C: View>(@ViewBuilder _ c: () -> C) -> some View {
        HStack(spacing: 14) { c() }
    }
}

// MARK: - The game's settings

/// The two choices of the play area right in the start window, under Start (the
/// owner's wish, 2026-10-17: switch it there, simply): room scale, stationary or
/// off, and at the edge the wall or the game fading out. The same settings as
/// Settings › Play area, which keeps the rest (no edge at all, the rooms).
struct RoomQuickSettings: View {
    @AppStorage(KLRoomGuard.modeKey) private var mode = KLRoomGuard.Mode.wall.rawValue
    @AppStorage(KLRoomGuard.styleKey) private var style = KLRoomGuard.Style.roomscale.rawValue
    @AppStorage(KLRoomGuard.enabledKey) private var on = KLRoomGuard.installDefault

    /// One picker for "on, and which kind": off, room scale, stationary.
    private var kind: Binding<String> {
        Binding(get: { on ? style : "off" },
                set: { v in
                    if v == "off" { on = false } else { on = true; style = v }
                })
    }

    var body: some View {
        // One row of a fixed height on glass: nothing appears or goes away when a
        // choice changes or Start is pressed (the owner: things that jump look broken),
        // and readable on any game's picture.
        HStack(spacing: 14) {
            Text(klT("Play area", "Spielbereich")).font(.callout.weight(.medium))
            Picker(klT("Play area", "Spielbereich"), selection: kind) {
                Text(klT("Room scale", "Raumgröße")).tag(KLRoomGuard.Style.roomscale.rawValue)
                Text(klT("Stationary", "Stationär")).tag(KLRoomGuard.Style.stationary.rawValue)
                Text(klT("Off", "Aus")).tag("off")
            }
            .pickerStyle(.segmented)
            .frame(width: 330)
            Divider().frame(height: 26)
            Group {
                Text(klT("At the edge", "Am Rand")).font(.callout.weight(.medium))
                Picker(klT("At the edge", "Am Rand"), selection: $mode) {
                    Text(klT("Wall", "Wand")).tag(KLRoomGuard.Mode.wall.rawValue)
                    Text(klT("Fade out", "Ausblenden")).tag(KLRoomGuard.Mode.room.rawValue)
                }
                .pickerStyle(.segmented)
                .frame(width: 230)
            }
            .disabled(!on)
            .opacity(on ? 1 : 0.45)
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 12)
        .glassBackgroundEffect(in: .capsule)
    }
}

/// Settings › Play area: what the game shows near the edge, and the rooms.
struct RoomSettingsView: View {
    let measure: () -> Void
    @AppStorage(KLRoomGuard.modeKey) private var mode = KLRoomGuard.Mode.wall.rawValue
    @AppStorage(KLRoomGuard.styleKey) private var style = KLRoomGuard.Style.roomscale.rawValue
    @AppStorage(KLRoomGuard.enabledKey) private var on = KLRoomGuard.installDefault
    @State private var rooms = KLRoomStore.load()
    @State private var confirmClear = false

    var body: some View {
        GroupBox(klT("Play area", "Spielbereich")) {
            VStack(alignment: .leading, spacing: 10) {
                Toggle(klT("Use a play area in this game", "Spielbereich in diesem Spiel verwenden"), isOn: $on)
                Text(on
                     ? klT("The game starts once the room you are in is recognised or measured, or you play stationary; near the edge it shows what is set below.",
                           "Das Spiel startet, sobald der Raum, in dem du bist, erkannt oder vermessen ist, oder du stationär spielst; am Rand zeigt es, was unten eingestellt ist.")
                     : klT("Off: no room measuring, no check at the start, nothing near the edge.",
                           "Aus: kein Vermessen, keine Prüfung beim Start, nichts am Rand."))
                    .font(.callout).foregroundStyle(.secondary)
                if on {
                Picker(klT("Kind", "Art"), selection: $style) {
                    Text(klT("Room scale", "Raumgröße")).tag(KLRoomGuard.Style.roomscale.rawValue)
                    Text(klT("Stationary / seated", "Stationär / sitzend")).tag(KLRoomGuard.Style.stationary.rawValue)
                }
                .pickerStyle(.segmented)
                Text(style == KLRoomGuard.Style.stationary.rawValue
                     ? klT("For playing on the spot, standing or seated: a circle 1.5 m across round where you are when the game starts, nothing to measure. Recentring moves it to where you are.",
                           "Zum Spielen auf der Stelle, stehend oder sitzend: ein Kreis von 1,5 m Durchmesser um die Stelle, an der du beim Start bist, ohne Vermessen. Neu zentrieren setzt ihn dorthin, wo du gerade bist.")
                     : klT("For walking about: the play area measured for the room you are in. The game starts once the room is recognised or measured.",
                           "Zum Herumgehen: der für deinen Raum vermessene Spielbereich. Das Spiel startet, sobald der Raum erkannt oder vermessen ist."))
                    .font(.callout).foregroundStyle(.secondary)
                Picker(klT("Near the edge", "Am Rand"), selection: $mode) {
                    Text(klT("Virtual wall", "Virtuelle Wand")).tag(KLRoomGuard.Mode.wall.rawValue)
                    Text(klT("Fade out", "Ausblenden")).tag(KLRoomGuard.Mode.room.rawValue)
                    Text(klT("Off", "Aus")).tag(KLRoomGuard.Mode.off.rawValue)
                }
                .pickerStyle(.segmented)
                Text(mode == KLRoomGuard.Mode.room.rawValue
                     ? klT("Near the edge of the play area the game turns see-through, the nearer the more, and the room shows. With the frame rate set to up to 120 the Vision Pro shows the room by itself, and the wall is shown instead.",
                           "Am Rand des Spielbereichs wird das Spiel durchsichtig, je näher, desto mehr, und der Raum scheint durch. Bei Bildrate bis 120 zeigt die Vision Pro den Raum selbst; dann erscheint stattdessen die Wand.")
                     : mode == KLRoomGuard.Mode.off.rawValue
                     ? klT("Nothing is shown near the edge. The game still starts only with a play area for the room.",
                           "Am Rand wird nichts gezeigt. Das Spiel startet trotzdem nur mit einem Spielbereich für den Raum.")
                     : klT("A see-through wall shows at the edge of the play area when your head or a hand comes near it: barely there at first, clearer the nearer you get. The floor never sets it off.",
                           "Am Rand des Spielbereichs zeigt sich eine durchsichtige Wand, wenn Kopf oder Hand ihm nahe kommen: erst kaum zu sehen, je näher, desto deutlicher. Der Boden löst sie nie aus."))
                    .font(.callout).foregroundStyle(.secondary)
                HStack {
                    Button(klT("Measure room", "Raum vermessen"), action: measure)
                    Spacer()
                    if !rooms.isEmpty {
                        Button(klT("Forget all rooms", "Alle Räume vergessen"), role: .destructive) { confirmClear = true }
                    }
                }
                Text(rooms.isEmpty
                     ? klT("No room measured yet.", "Noch kein Raum vermessen.")
                     : klT("\(rooms.count) room(s) measured, shared by all games: ", "\(rooms.count) Raum/Räume vermessen, für alle Spiele: ")
                       + rooms.map { String(format: "%.1f m²", $0.areaM2) }.joined(separator: ", "))
                    .font(.caption).foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .confirmationDialog(klT("Forget every measured room in all games?", "Alle vermessenen Räume in allen Spielen vergessen?"),
                            isPresented: $confirmClear) {
            Button(klT("Forget all rooms", "Alle Räume vergessen"), role: .destructive) {
                KLRoomStore.removeAll()
                rooms = []
            }
        }
    }
}
