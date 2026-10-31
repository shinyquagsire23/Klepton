import CoreGraphics
import CoreText
import Foundation
import Metal
import simd

// Half-Life: Alyx's own loading screen, the amber hologram of City 17.
//
// The game does not draw it: it tells a web page of its own what to show (runtime/linux/kl_lx_interstitial.c)
// and SteamVR's web helper turns that page into a 3D scene. The page, hlvr/webui/interstitials.js, is a
// tree of Transform, Tint, Opacity, RenderModel and Panel elements that only that helper understands, so it
// cannot be shown here as a page; its tree is rebuilt below, element for element, from the models and
// textures in hlvr/models/ui/map_hologram_steamvr. What SteamVR's renderer does with `fresnel` is not
// public: the formula in hologramSource was matched by eye against a picture of the real thing.
//
// Everything is read from the game's own files, at start, on a thread of its own. Where they are missing
// or unreadable `ready` stays false and the compositor shows its plain card (makeLoadingCard).

/// The tree's order, in which later draws land on the depth an earlier one wrote.
private enum HologramLook {
    case depthOnly                  // Opacity 0: only hides what is behind it
    case solid                      // no texture
    case textured(String?)          // an alpha mask: `texture-id`, or nil for the material's own (`wireframe`)
}

private struct HologramItem {
    var model: String
    var tint: UInt32 = 0xFFFFFF
    var opacity: Float = 1
    var fresnel: SIMD2<Float>?      // opacity, strength
    var cull = false
    var look: HologramLook
    var writesDepth = true
    var world = matrix_identity_float4x4
}

private struct HologramMap {
    var pos: SIMD3<Float>
    var cables = 99
    var hidden = false
    var vault = false
}

final class KleptonHologram {
    /// One eye (or the one view of a pass that draws an eye at a time).
    struct View {
        var clipFromWorld: simd_float4x4
        var eye: SIMD3<Float>
    }

    private struct Mesh {
        let buffer: MTLBuffer
        let vertices: Int
        let material: String?
    }

    private struct Camera {
        var clip: simd_float4x4
        var eye: SIMD4<Float>
    }

    /// Matches `struct HDraw` in hologramSource.
    private struct Draw {
        var world: simd_float4x4
        var color: SIMD4<Float>         // rgb linear, a the Opacity element
        var fresnel: SIMD4<Float>       // opacity, strength, on, the whole screen's fade
    }

    private struct Panel {
        let texture: MTLTexture
        let size: SIMD2<Float>          // pixels
    }

    private let device: MTLDevice
    private let root: URL
    private let queue: MTLCommandQueue?
    private let lock = NSLock()
    private var meshes: [String: Mesh] = [:]
    private var textures: [String: MTLTexture] = [:]
    private var arcs: [MTLTexture] = []
    private var panels: [String: Panel] = [:]
    private var library: MTLLibrary?
    private var plate: MTLTexture?
    private var white: MTLTexture?
    private var sampler: MTLSamplerState?
    private var typeface: CGFont?

    private var ready_ = false
    /// Everything is read and the screen can be shown.
    var ready: Bool {
        lock.lock(); defer { lock.unlock() }
        return ready_
    }

    private var pipelineKey: (MTLPixelFormat, Int)?
    private var depthPipeline: MTLRenderPipelineState?
    private var modelPipeline: MTLRenderPipelineState?
    private var panelPipeline: MTLRenderPipelineState?
    private var panelDepthPipeline: MTLRenderPipelineState?
    private var depthOnlyState: MTLDepthStencilState?
    private var depthWriteState: MTLDepthStencilState?
    private var depthTestState: MTLDepthStencilState?
    private var alwaysState: MTLDepthStencilState?
    private var alwaysWriteState: MTLDepthStencilState?

    // What the page holds in its state.
    private var shown = false
    private var loading = false
    private var endedAt = 0.0
    private var startedAt = 0.0
    private var messageAt = 0.0
    private var map = ""
    private var message = ""
    private var loadingText = ""
    private var chapter = ""
    private var items: [HologramItem] = []
    private var worldFromScene = matrix_identity_float4x4

    init(device: MTLDevice, root: URL) {
        self.device = device
        self.root = root
        queue = device.makeCommandQueue()
    }

    /// Reads everything; safe on any thread. False if the game's files are not there.
    @discardableResult
    func load() -> Bool {
        let dir = root.appendingPathComponent("models/ui/map_hologram_steamvr")
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return false }
        var meshes: [String: Mesh] = [:]
        var textures: [String: MTLTexture] = [:]
        for name in names {
            let url = dir.appendingPathComponent(name)
            if name.hasSuffix(".obj") {
                if let m = readMesh(url) { meshes[String(name.dropLast(4))] = m }
            } else if name.hasSuffix(".dds"), let t = readDDS(url) {
                textures[String(name.dropLast(4))] = t
            }
        }
        guard meshes["voxel_cubes"] != nil, let library = makeLibrary() else {
            NSLog("[hologram] not shown: the map's models could not be read from \(dir.path)")
            return false
        }
        // panorama/fonts: the copies under webui are scrambled, for SteamVR's own web server to unscramble.
        let typeface = CGDataProvider(url: root.appendingPathComponent("panorama/fonts/raju-regular.otf") as CFURL)
            .flatMap { CGFont($0) }
        var arcs: [MTLTexture] = []
        for i in 1...6 {
            if let t = readArc(root.appendingPathComponent("webui/images/arc_0\(i).svg")) { arcs.append(t) }
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        sd.maxAnisotropy = 8
        let sampler = device.makeSamplerState(descriptor: sd)
        let white = makeBitmap(width: 1, height: 1) { $0.setFillColor(gray: 1, alpha: 1); $0.fill(CGRect(x: 0, y: 0, width: 1, height: 1)) }
        let plate = makePlate()

        lock.lock()
        self.meshes = meshes
        self.textures = textures
        self.arcs = arcs
        self.library = library
        self.sampler = sampler
        self.white = white
        self.plate = plate
        self.typeface = typeface
        ready_ = true
        lock.unlock()
        NSLog("[hologram] \(meshes.count) models, \(textures.count) textures read")
        return true
    }

    // MARK: The page's state

    /// One frame's worth of the guest's messages. `phase` is 0 once loading has ended; the page then fades
    /// the map out over a second and shows the chapter's name for six. Returns whether anything is shown.
    func update(phase: Int, map: String, message: String, loading: String, chapter: String, now: Double,
                originFromHead: simd_float4x4) -> Bool {
        if phase != 0, !self.loading {
            // Placed once, where the player is and looks, the floor under them: the game's facingAngle
            // says the same in a play space that is not this one.
            self.loading = true
            shown = true
            startedAt = now
            worldFromScene = Self.sceneOnFloor(originFromHead)
            NSLog("[hologram] loading \(map.isEmpty ? "(no level named)" : map), the floor \(originFromHead.columns.3.y) m under the eyes")
        } else if phase == 0, self.loading {
            self.loading = false
            endedAt = now
        }
        if phase != 0 {
            let text = Self.plainText(message)
            if text != self.message { messageAt = now }
            if map != self.map || text != self.message || items.isEmpty {
                self.map = map
                self.message = text
                items = Self.tree(map: map)
            }
            loadingText = loading
            self.chapter = chapter.replacingOccurrences(of: "TMP: ", with: "")
        }
        if !self.loading, shown, now - endedAt > 6.5 { shown = false }
        return shown
    }

    private static func sceneOnFloor(_ originFromHead: simd_float4x4) -> simd_float4x4 {
        var fwd = SIMD3<Float>(-originFromHead.columns.2.x, 0, -originFromHead.columns.2.z)
        fwd = simd_length(fwd) > 0.01 ? simd_normalize(fwd) : SIMD3<Float>(0, 0, -1)
        let z = -fwd, y = SIMD3<Float>(0, 1, 0), x = simd_cross(y, z)
        return simd_float4x4(SIMD4<Float>(x, 0), SIMD4<Float>(y, 0), SIMD4<Float>(z, 0),
                             SIMD4<Float>(originFromHead.columns.3.x, 0, originFromHead.columns.3.z, 1))
    }

    /// The prompt goes into the page as HTML.
    private static func plainText(_ html: String) -> String {
        html.replacingOccurrences(of: "<br\\s*/?>", with: "\n", options: .regularExpression)
            .replacingOccurrences(of: "<[^>]*>", with: "", options: .regularExpression)
    }

    // MARK: The tree

    private static let tertiaryMaps = ["tertiary_00", "tertiary_01", "tertiary_02", "tertiary_03",
                                       "skybox_00", "skybox_01", "skybox_02", "skybox_03"]
    private static let vaultCables = ["vault_cable_01", "vault_cable_02", "vault_cable_07", "vault_cable_09",
                                      "vault_cable_03", "vault_cable_04", "vault_cable_06", "vault_cable_05",
                                      "vault_cable_10", "vault_cable_08"]
    private static let gameMaps = ["a1_intro_world", "a1_intro_world_2", "a2_quarantine_entrance", "a2_pistol",
                                   "a2_hideout", "a2_headcrabs_tunnel", "a2_drainage", "a2_train_yard",
                                   "a3_station_street", "a3_hotel_lobby_basement", "a3_hotel_underground_pit",
                                   "a3_hotel_interior_rooftop", "a3_hotel_street", "a3_c17_processing_plant",
                                   "a3_distillery", "a4_c17_zoo", "a4_c17_tanker_yard", "a4_c17_water_tower",
                                   "a4_c17_parking_garage"]
    private static let mapInfo: [String: HologramMap] = [
        "a1_intro_world": HologramMap(pos: [0.315, 0.06, 0.97], hidden: true),
        "a1_intro_world_2": HologramMap(pos: [0.768, 0.05, 0.96], hidden: true),
        "a2_quarantine_entrance": HologramMap(pos: [0.284, 0.01, 0.61]),
        "a2_pistol": HologramMap(pos: [0.327, 0.015, 0.52]),
        "a2_hideout": HologramMap(pos: [0.25, 0.015, 0.44]),
        "a2_headcrabs_tunnel": HologramMap(pos: [0.142, 0, 0.373]),
        "a2_drainage": HologramMap(pos: [-0.035, 0.015, 0.348]),
        "a2_train_yard": HologramMap(pos: [-0.055, 0, 0.25]),
        "a3_station_street": HologramMap(pos: [-0.225, 0, 0.17]),
        "a3_hotel_lobby_basement": HologramMap(pos: [-0.208, 0.013, 0.25], cables: 0, vault: true),
        "a3_hotel_underground_pit": HologramMap(pos: [-0.195, -0.01, 0.22], cables: 0, vault: true),
        "a3_hotel_interior_rooftop": HologramMap(pos: [-0.19, 0.032, 0.27], cables: 0, vault: true),
        "a3_hotel_street": HologramMap(pos: [-0.218, 0.03, 0.195], cables: 0, vault: true),
        "a3_c17_processing_plant": HologramMap(pos: [-0.316, 0.04, 0.15], cables: 1, vault: true),
        "a3_distillery": HologramMap(pos: [-0.337, 0.032, 0.034], cables: 2, vault: true),
        "a4_c17_zoo": HologramMap(pos: [-0.18, 0.01, -0.07], cables: 5, vault: true),
        "a4_c17_tanker_yard": HologramMap(pos: [0.01, 0, -0.18], cables: 9, vault: true),
        "a4_c17_water_tower": HologramMap(pos: [0.008, 0.025, -0.31], vault: true),
        "a4_c17_parking_garage": HologramMap(pos: [-0.038, 0.035, -0.33]),
        "a5_vault": HologramMap(pos: [-0.088, 0.05, -0.33]),
        "a5_ending": HologramMap(pos: [-0.088, 0.05, -0.33]),
    ]

    /// The map's own frame: 1.1 m up, 1.5 m ahead, tilted 30° towards the player.
    private static let mapFrame = trs([0, 1.1, -1.5], rx: 30, scale: 0.8)

    /// The amber plate the city stands on, eight metres wide.
    private static let plateWorld = mapFrame * trs([0, 0, 0], rx: -90) * trs([0, -0.25, 0.01]) * trs([0, 0, 0], scale: [8, 1.8, 1])

    private static func trs(_ t: SIMD3<Float>, rx: Float = 0, rz: Float = 0, scale: SIMD3<Float> = [1, 1, 1]) -> simd_float4x4 {
        var m = simd_float4x4(diagonal: SIMD4<Float>(scale, 1))
        if rx != 0 { m = simd_float4x4(simd_quatf(angle: rx * .pi / 180, axis: [1, 0, 0])) * m }
        if rz != 0 { m = simd_float4x4(simd_quatf(angle: rz * .pi / 180, axis: [0, 0, 1])) * m }
        m.columns.3 = SIMD4<Float>(t, 1)
        return m
    }

    private static func trs(_ t: SIMD3<Float>, rx: Float = 0, scale: Float) -> simd_float4x4 {
        trs(t, rx: rx, scale: [scale, scale, scale])
    }

    /// The page's render(), for one map, in its order.
    private static func tree(map: String) -> [HologramItem] {
        var out: [HologramItem] = []
        let feet = "play_space_center_feet"
        out.append(HologramItem(model: feet, cull: true, look: .depthOnly))
        out.append(HologramItem(model: feet, tint: 0xFFBE55, opacity: 0, fresnel: [0.8, 0.1], cull: true, look: .solid))
        out.append(HologramItem(model: feet, tint: 0xFFBE55, opacity: 0.25, look: .textured(nil)))
        guard let info = mapInfo[map], !info.hidden else { return out }

        let index: Int
        if let i = gameMaps.firstIndex(of: map) { index = i } else { index = gameMaps.count - 1 }
        let current = gameMaps[index]
        let before = Array(gameMaps[..<index])
        let preCrash = index < gameMaps.count - 1
        let cables = info.cables < vaultCables.count ? Array(vaultCables[info.cables...]) : []
        let frame = mapFrame * trs([0, 0, 0], scale: 0.3)
        func add(_ model: String, _ tint: UInt32 = 0xFFFFFF, _ opacity: Float = 1, fresnel: SIMD2<Float>? = nil,
                 cull: Bool = true, look: HologramLook, depth: Bool = true) {
            out.append(HologramItem(model: model, tint: tint, opacity: opacity, fresnel: fresnel, cull: cull,
                                    look: look, writesDepth: depth, world: frame))
        }

        add("map_streets", 0x664C22, 0.1, cull: false, look: .textured(nil), depth: false)
        add("voxel_cubes", look: .depthOnly)
        add("combine_wall", cull: false, look: .depthOnly)
        if info.vault { add("a5_vault", cull: false, look: .depthOnly) }
        add("cables_citadel", cull: false, look: .depthOnly)
        if preCrash { add("a4_c17_parking_garage_precrash", cull: false, look: .depthOnly) }
        for m in tertiaryMaps + cables + before + [current] { add(m, look: .depthOnly) }

        add("voxel_cubes", 0xFFE2A8, -5, fresnel: [5, 1], look: .solid)
        for m in tertiaryMaps { add(m, 0x553C18, 0.75, fresnel: [-0.75, 0.75], look: .textured(m + "_ao")) }
        add("cables_citadel", 0x553C18, 0.75, fresnel: [0.75, 0.1], look: .textured("cables_ao"))
        if info.vault { add("a5_vault", 0x553C18, 1, fresnel: [-1, 0.5], look: .textured("a5_vault_ao")) }
        for m in cables { add(m, 0x805F2A, 0.75, fresnel: [0.75, 0.1], look: .textured("cables_ao")) }
        add("combine_wall", 0x553C18, 0.75, fresnel: [-0.75, 0.75], cull: false, look: .textured("combine_wall_ao"))
        if preCrash {
            add("a4_c17_parking_garage_precrash", 0x553C18, 0.75, fresnel: [-0.75, 0.75],
                look: .textured("a4_c17_parking_garage_precrash_ao"))
        }
        for m in before { add(m, 0x805F2A, 1, fresnel: [-1, 0.5], look: .textured(m + "_ao")) }
        add(current, 0xFFBE55, 1, fresnel: [-1, 0.5], look: .textured(current + "_ao"))

        for m in cables { add(m, 0x66471D, 0.25, cull: false, look: .textured(nil)) }
        add("cables_citadel", 0x66471D, 0.15, cull: false, look: .textured(nil))
        if info.vault { add("a5_vault", 0x805924, 0.75, cull: false, look: .textured(nil)) }
        if preCrash { add("a4_c17_parking_garage_precrash", 0x66471D, 0.75, cull: false, look: .textured(nil)) }
        for m in before { add(m, 0xCC953D, 0.75, cull: false, look: .textured(nil)) }
        add("combine_wall", 0x66471D, 0.25, cull: false, look: .textured(nil))
        for m in tertiaryMaps { add(m, 0x66471D, 0.75, cull: false, look: .textured(nil)) }
        add(current, 0xFFD155, 1, cull: false, look: .textured(nil))
        add("voxel_cubes", 0xFFBE55, 0.1, cull: false, look: .textured(nil))
        return out
    }

    // MARK: Drawing

    /// Draws the screen into `enc`, whose pass holds nothing nearer than the eye picture (500 m) or the
    /// black behind frames without one (600 m). Leaves the pass's culling as it found it, the default.
    func encode(_ enc: MTLRenderCommandEncoder, color: MTLPixelFormat, amplification: Int, views: [View],
                now: Double) -> Bool {
        guard shown, ready, buildPipelines(color: color, amplification: amplification),
              let depthPipeline, let modelPipeline, let panelPipeline, let sampler, let white
        else { return false }
        let t = Float(now - (loading ? startedAt : endedAt))
        let info = Self.mapInfo[map]
        let onMap = info.map { !$0.hidden } ?? false
        // A level with no place on the map is the start of the game or the prologue. The page has only
        // the word "loading" for those, over the room around the player that it asks SteamVR for
        // (SetStageOverride), and there is no such room here: alone in the dark it was only strange, so
        // those wait in black. The prompt still shows, unless it is gone at once, as it is at the start.
        let prompt = loading && !message.isEmpty && (onMap || now - messageAt > 0.5)
        let fade = !onMap && !prompt ? 0 : loading ? Float(1) : max(0, 1 - t)
        var cameras = views.map { Camera(clip: $0.clipFromWorld, eye: SIMD4<Float>($0.eye, 1)) }
        let camerasLength = MemoryLayout<Camera>.stride * cameras.count
        enc.setFrontFacing(.counterClockwise)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.setVertexBytes(&cameras, length: camerasLength, index: 1)
        defer {
            enc.setFrontFacing(.clockwise)
            enc.setCullMode(.none)
        }

        if fade > 0 {
            if let plate {
                let flicker = 1 - 0.1 * Self.noise(floor(now * 12))
                drawPanel(enc, panelPipeline, plate, Self.plateWorld, opacity: 0.2 * flicker * fade, gamma: 2.2)
            }
            enc.setVertexBytes(&cameras, length: camerasLength, index: 1)
            for item in items {
                guard let mesh = meshes[item.model] else { continue }
                var draw = Draw(world: worldFromScene * item.world,
                                color: SIMD4<Float>(Self.linear(item.tint), item.opacity),
                                fresnel: SIMD4<Float>(item.fresnel?.x ?? 0, item.fresnel?.y ?? 0,
                                                      item.fresnel == nil ? 0 : 1, fade))
                enc.setCullMode(item.cull ? .back : .none)
                if case .depthOnly = item.look {
                    enc.setRenderPipelineState(depthPipeline)
                    enc.setDepthStencilState(depthOnlyState)
                } else {
                    enc.setRenderPipelineState(modelPipeline)
                    enc.setDepthStencilState(item.writesDepth ? depthWriteState : depthTestState)
                    var texture: MTLTexture?
                    if case .textured(let id) = item.look { texture = textures[id ?? mesh.material ?? ""] }
                    enc.setFragmentTexture(texture ?? white, index: 0)
                    enc.setFragmentBytes(&draw, length: MemoryLayout<Draw>.stride, index: 0)
                }
                enc.setVertexBuffer(mesh.buffer, offset: 0, index: 0)
                enc.setVertexBytes(&draw, length: MemoryLayout<Draw>.stride, index: 2)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: mesh.vertices)
            }
            enc.setCullMode(.none)
            // Depth for the plate too, now that nothing can land behind it: the system drops a tile of the
            // drawable that holds no depth, and with it the plate's and the streets' faint light.
            if onMap, let panelDepthPipeline {
                var draw = Draw(world: worldFromScene * Self.plateWorld, color: .zero, fresnel: .zero)
                enc.setRenderPipelineState(panelDepthPipeline)
                enc.setDepthStencilState(depthOnlyState)
                enc.setVertexBytes(&draw, length: MemoryLayout<Draw>.stride, index: 2)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
        }

        if prompt {
            if let panel = promptPanel(message) {
                let scale = 1 / panel.size.x
                drawPanel(enc, panelPipeline, panel.texture,
                          Self.trs([0, 1.5, -1.5], scale: [1, panel.size.y * scale, 1]), opacity: 1, writesDepth: true)
            }
        } else if loading, message.isEmpty, onMap, let pos = info?.pos {
            drawCallout(enc, cameras: &cameras, length: camerasLength, pos: pos)
            let anchor = Self.mapFrame * Self.trs(pos, rx: -90) * Self.trs([0, 0, 0.1], rx: 30)
            drawLoading(enc, panelPipeline, em: 120, padding: SIMD2<Float>(24, 54), height: 0.12, anchor: anchor, now: now)
        }
        if !loading, !chapter.isEmpty, let panel = chapterPanel(chapter.uppercased()) {
            let scale = 0.2 / panel.size.y
            drawPanel(enc, panelPipeline, panel.texture,
                      Self.trs([0, 1.7, -1.5], scale: [panel.size.x * scale, 0.2, 1]),
                      opacity: Self.chapterOpacity(t), writesDepth: true)
        }
        return true
    }

    /// ChapterKeyframes in styles.css, six seconds.
    private static func chapterOpacity(_ t: Float) -> Float {
        let p = t / 6
        if p < 0.05 { return 0 }
        if p < 0.35 { return (p - 0.05) / 0.30 }
        if p < 0.70 { return 1 }
        return max(0, (1 - p) / 0.30)
    }

    /// The Flicker animation of the background panel, which is a list of arbitrary numbers too.
    private static func noise(_ x: Double) -> Float {
        let s = sin(x * 12.9898) * 43758.5453
        return Float(s - floor(s))
    }

    private func drawCallout(_ enc: MTLRenderCommandEncoder, cameras: inout [Camera], length: Int, pos: SIMD3<Float>) {
        guard let mesh = meshes["map_callout_line"], let pipeline = modelPipeline else { return }
        let base = Self.mapFrame * Self.trs(pos)
        let stems: [simd_float4x4] = [
            base * Self.trs([0, 0, 0], scale: [0.003, 0.1, 0.003]) * Self.trs([0.288675, 0, 0]),
            base * Self.trs([0, 0.1, 0], rz: -90, scale: [0.003, 0.01, 0.003]) * Self.trs([0.288675, 0, 0]),
        ]
        enc.setRenderPipelineState(pipeline)
        enc.setDepthStencilState(depthWriteState)
        enc.setFragmentTexture(white, index: 0)
        for stem in stems {
            var draw = Draw(world: worldFromScene * stem, color: [1, 1, 1, 1], fresnel: [0, 0, 0, 1])
            enc.setVertexBuffer(mesh.buffer, offset: 0, index: 0)
            enc.setVertexBytes(&draw, length: MemoryLayout<Draw>.stride, index: 2)
            enc.setFragmentBytes(&draw, length: MemoryLayout<Draw>.stride, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: mesh.vertices)
        }
    }

    /// The word for "loading" with its spinner and its dots. The panel is laid out in the page's pixels and
    /// scaled to the height the tree gives it.
    private func drawLoading(_ enc: MTLRenderCommandEncoder, _ pipe: MTLRenderPipelineState, em: Float,
                             padding: SIMD2<Float>, height: Float, anchor: simd_float4x4, now: Double) {
        guard let panel = loadingPanel(loadingText, dots: Self.dots(now - startedAt), em: em, padding: padding)
        else { return }
        let scale = height / panel.size.y
        let w = panel.size.x * scale, h = panel.size.y * scale
        let origin = anchor * Self.trs([w / 2, 0, 0])
        drawPanel(enc, pipe, panel.texture, origin * Self.trs([0, 0, 0], scale: [w, h, 1]), opacity: 1, writesDepth: true)
        // The arcs turn one after the other, two seconds a round, each over 40 % of it (the keyframes in
        // the page, with their ease-out).
        let starts: [Double] = [0, 0.1, 0.2, 0.2, 0.3, 0.35]
        let cycle = (now - startedAt).truncatingRemainder(dividingBy: 2) / 2
        let centre = origin * Self.trs([(padding.x + em / 2) * scale - w / 2, 0, 0])
        for (i, arc) in arcs.enumerated() {
            let p = max(0, min(1, (cycle - starts[i]) / 0.4))
            let eased = 1 - (1 - p) * (1 - p)
            let turn = Self.trs([0, 0, 0], rz: Float(-360 * eased), scale: [em * scale, em * scale, 1])
            drawPanel(enc, pipe, arc, centre * turn, opacity: 1)
        }
    }

    private static func dots(_ t: Double) -> Int {
        let p = t.truncatingRemainder(dividingBy: 2)
        return p < 0.5 ? 0 : p < 1 ? 1 : p < 1.5 ? 2 : 3
    }

    private func drawPanel(_ enc: MTLRenderCommandEncoder, _ pipe: MTLRenderPipelineState, _ texture: MTLTexture,
                           _ world: simd_float4x4, opacity: Float, gamma: Float = 1, writesDepth: Bool = false) {
        var draw = Draw(world: worldFromScene * world, color: [1, 1, 1, opacity], fresnel: [gamma, 0, 0, 0])
        enc.setRenderPipelineState(pipe)
        enc.setDepthStencilState(writesDepth ? alwaysWriteState : alwaysState)
        enc.setCullMode(.none)
        enc.setFragmentTexture(texture, index: 0)
        enc.setVertexBytes(&draw, length: MemoryLayout<Draw>.stride, index: 2)
        enc.setFragmentBytes(&draw, length: MemoryLayout<Draw>.stride, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    private static func linear(_ rgb: UInt32) -> SIMD3<Float> {
        func l(_ v: UInt32) -> Float {
            let c = Float(v & 0xFF) / 255
            return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
        }
        return SIMD3<Float>(l(rgb >> 16), l(rgb >> 8), l(rgb))
    }

    // MARK: Panels

    private func font(_ size: CGFloat) -> CTFont? {
        typeface.map { CTFontCreateWithGraphicsFont($0, size, nil, nil) }
    }

    private func attributed(_ text: String, _ font: CTFont, centred: Bool) -> NSAttributedString {
        var alignment: CTTextAlignment = centred ? .center : .left
        let setting = withUnsafeBytes(of: &alignment) { raw in
            CTParagraphStyleSetting(spec: .alignment, valueSize: MemoryLayout<CTTextAlignment>.size,
                                    value: raw.baseAddress!)
        }
        let paragraph = [setting].withUnsafeBufferPointer { CTParagraphStyleCreate($0.baseAddress!, 1) }
        return NSAttributedString(string: text, attributes: [
            kCTFontAttributeName as NSAttributedString.Key: font,
            kCTForegroundColorAttributeName as NSAttributedString.Key: CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1),
            kCTParagraphStyleAttributeName as NSAttributedString.Key: paragraph,
        ])
    }

    /// The prompt: 180 px type in a page 2048 px wide. Its panel is as wide as the text and that width is
    /// what the tree scales to a metre, up to the page's width, where the text wraps.
    private func promptPanel(_ text: String) -> Panel? {
        centredPanel(text, em: 180)
    }

    private func chapterPanel(_ text: String) -> Panel? {
        centredPanel(text, em: 180)
    }

    private func centredPanel(_ text: String, em: CGFloat) -> Panel? {
        let key = "c\(em) \(text)"
        if let p = panels[key] { return p }
        guard let font = font(em) else { return nil }
        let s = attributed(text, font, centred: true)
        let setter = CTFramesetterCreateWithAttributedString(s)
        let page = CGSize(width: 2048, height: 2048)
        var fit = CTFramesetterSuggestFrameSizeWithConstraints(setter, CFRange(), nil, page, nil)
        let wrapped = fit.height > ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font)) * 1.5
        if wrapped { fit.width = page.width }
        let w = Int(ceil(fit.width)), h = Int(ceil(fit.height))
        guard let tex = makeBitmap(width: w, height: h, draw: { ctx in
            let path = CGPath(rect: CGRect(x: 0, y: 0, width: w, height: h), transform: nil)
            CTFrameDraw(CTFramesetterCreateFrame(setter, CFRange(), path, nil), ctx)
        }) else { return nil }
        let p = Panel(texture: tex, size: SIMD2<Float>(Float(w), Float(h)))
        panels[key] = p
        return p
    }

    private func loadingPanel(_ text: String, dots: Int, em: Float, padding: SIMD2<Float>) -> Panel? {
        let key = "l\(em) \(dots) \(text)"
        if let p = panels[key] { return p }
        guard let font = font(CGFloat(em)) else { return nil }
        let s = attributed(text + String(repeating: ".", count: dots), font, centred: false)
        let line = CTLineCreateWithAttributedString(s)
        let width = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        let full = CTLineCreateWithAttributedString(attributed(text, font, centred: false))
        let textWidth = CGFloat(CTLineGetTypographicBounds(full, nil, nil, nil))
        let lineHeight = CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font)
        let e = CGFloat(em)
        let w = Int(ceil(CGFloat(padding.x) * 2 + e * 1.1 + textWidth + e * 0.8))
        let h = Int(ceil(CGFloat(padding.y) * 2 + lineHeight))
        guard width >= 0, let tex = makeBitmap(width: w, height: h, draw: { ctx in
            ctx.textPosition = CGPoint(x: CGFloat(padding.x) + e * 1.1, y: CGFloat(padding.y) + CTFontGetDescent(font))
            CTLineDraw(line, ctx)
        }) else { return nil }
        let p = Panel(texture: tex, size: SIMD2<Float>(Float(w), Float(h)))
        panels[key] = p
        return p
    }

    /// `.BackgroundHeader` over `.Background`: amber fading out to both sides. The page's scanlines are two
    /// pixels on a plate eight metres wide, and at this distance only shimmer: left out.
    private func makePlate() -> MTLTexture? {
        let w = 2048, header = 32, body = 512
        return makeBitmap(width: w, height: header + body) { ctx in
            let space = CGColorSpace(name: CGColorSpace.sRGB)!
            func band(_ rgb: UInt32, y: Int, height: Int) {
                func c(_ a: CGFloat) -> CGColor {
                    CGColor(colorSpace: space, components: [CGFloat(rgb >> 16 & 255) / 255, CGFloat(rgb >> 8 & 255) / 255,
                                                            CGFloat(rgb & 255) / 255, a])!
                }
                guard let g = CGGradient(colorsSpace: space, colors: [c(0), c(1), c(1), c(0)] as CFArray,
                                         locations: [0, 0.4, 0.6, 1]) else { return }
                ctx.saveGState()
                ctx.clip(to: CGRect(x: 0, y: y, width: w, height: height))
                ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: w, y: 0), options: [])
                ctx.restoreGState()
            }
            band(0x553C18, y: 0, height: body)
            band(0xFFBE55, y: body, height: header)
        }
    }

    /// An RGBA picture in the page's colours, premultiplied as CoreGraphics leaves it, bottom row first in
    /// CoreGraphics' terms and top row first in the texture's.
    private func makeBitmap(width: Int, height: Int, draw: (CGContext) -> Void) -> MTLTexture? {
        guard width > 0, height > 0,
              let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                  bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        draw(ctx)
        guard let data = ctx.data else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height,
                                                            mipmapped: true)
        desc.usage = .shaderRead
        guard let tex = device.makeTexture(descriptor: desc), let cmd = queue?.makeCommandBuffer(),
              let blit = cmd.makeBlitCommandEncoder() else { return nil }
        tex.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: data, bytesPerRow: width * 4)
        blit.generateMipmaps(for: tex)
        blit.endEncoding()
        cmd.commit()
        cmd.waitUntilCompleted()
        return tex
    }

    // MARK: The game's files

    /// A flat-shaded triangle list. The files carry no normals and each face is its own smoothing group.
    private func readMesh(_ url: URL) -> Mesh? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        var positions: [SIMD3<Float>] = []
        var uvs: [SIMD2<Float>] = []
        var out: [Float] = []
        out.reserveCapacity(data.count / 2)
        var material: String?
        data.split(separator: 10).forEach { lineBytes in
            guard let line = String(data: lineBytes, encoding: .ascii) else { return }
            let parts = line.split(whereSeparator: { $0 == " " || $0 == "\r" || $0 == "\t" })
            guard let tag = parts.first else { return }
            switch tag {
            case "v" where parts.count >= 4:
                positions.append(SIMD3<Float>(Float(parts[1]) ?? 0, Float(parts[2]) ?? 0, Float(parts[3]) ?? 0))
            case "vt" where parts.count >= 3:
                uvs.append(SIMD2<Float>(Float(parts[1]) ?? 0, Float(parts[2]) ?? 0))
            case "f" where parts.count >= 4:
                let corners: [(Int, Int)] = parts[1...].map { c in
                    let f = c.split(separator: "/", omittingEmptySubsequences: false)
                    return ((Int(f[0]) ?? 1) - 1, f.count > 1 ? (Int(f[1]) ?? 1) - 1 : -1)
                }
                for i in 1..<(corners.count - 1) {
                    let tri = [corners[0], corners[i], corners[i + 1]]
                    guard tri.allSatisfy({ $0.0 >= 0 && $0.0 < positions.count }) else { continue }
                    let a = positions[tri[0].0], b = positions[tri[1].0], c = positions[tri[2].0]
                    let cross = simd_cross(b - a, c - a)
                    let n = simd_length(cross) > 0 ? simd_normalize(cross) : SIMD3<Float>(0, 1, 0)
                    for v in tri {
                        let p = positions[v.0]
                        // The textures were baked with V running upwards.
                        let uv = v.1 >= 0 && v.1 < uvs.count ? uvs[v.1] : .zero
                        out.append(contentsOf: [p.x, p.y, p.z, n.x, n.y, n.z, uv.x, 1 - uv.y])
                    }
                }
            default: break
            }
        }
        let mtl = url.deletingPathExtension().appendingPathExtension("mtl")
        if let text = try? String(contentsOf: mtl, encoding: .ascii) {
            for line in text.split(whereSeparator: \.isNewline) where line.hasPrefix("map_Kd ") {
                material = line.dropFirst(7).split(separator: ".").first.map(String.init)
            }
        }
        guard !out.isEmpty, let buffer = device.makeBuffer(bytes: out, length: out.count * 4, options: .storageModeShared)
        else { return nil }
        return Mesh(buffer: buffer, vertices: out.count / 8, material: material)
    }

    /// BC7 with its mips, the one format in that folder.
    private func readDDS(_ url: URL) -> MTLTexture? {
        guard device.supportsBCTextureCompression, let data = try? Data(contentsOf: url), data.count > 148 else { return nil }
        func u32(_ at: Int) -> Int { data.withUnsafeBytes { Int($0.loadUnaligned(fromByteOffset: at, as: UInt32.self)) } }
        guard data[84...87].elementsEqual("DX10".utf8), u32(128) == 98 || u32(128) == 99 else { return nil }
        let height = u32(12), width = u32(16), mips = max(1, u32(28))
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bc7_rgbaUnorm, width: width, height: height,
                                                            mipmapped: mips > 1)
        desc.mipmapLevelCount = mips
        desc.usage = .shaderRead
        guard let tex = device.makeTexture(descriptor: desc) else { return nil }
        var offset = 148
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            for level in 0..<mips {
                let w = max(1, width >> level), h = max(1, height >> level)
                let rowBytes = ((w + 3) / 4) * 16, rows = (h + 3) / 4
                guard offset + rowBytes * rows <= raw.count else { return }
                tex.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: level,
                            withBytes: raw.baseAddress! + offset, bytesPerRow: rowBytes)
                offset += rowBytes * rows
            }
        }
        return tex
    }

    /// One piece of the spinner: a 512 px SVG holding a single path of M, L, l, C, c and z.
    private func readArc(_ url: URL) -> MTLTexture? {
        guard let svg = try? String(contentsOf: url, encoding: .utf8),
              let start = svg.range(of: " d=\""), let end = svg[start.upperBound...].firstIndex(of: "\"")
        else { return nil }
        let path = CGMutablePath()
        var numbers: [CGFloat] = []
        var command: Character = " "
        var at = CGPoint.zero
        func flush() {
            var n = numbers
            switch command {
            case "M", "m":
                while n.count >= 2 {
                    at = command == "M" ? CGPoint(x: n[0], y: n[1]) : CGPoint(x: at.x + n[0], y: at.y + n[1])
                    path.move(to: at); n.removeFirst(2)
                }
            case "L", "l":
                while n.count >= 2 {
                    at = command == "L" ? CGPoint(x: n[0], y: n[1]) : CGPoint(x: at.x + n[0], y: at.y + n[1])
                    path.addLine(to: at); n.removeFirst(2)
                }
            case "C", "c":
                while n.count >= 6 {
                    let o = command == "C" ? CGPoint.zero : at
                    let end = CGPoint(x: o.x + n[4], y: o.y + n[5])
                    path.addCurve(to: end, control1: CGPoint(x: o.x + n[0], y: o.y + n[1]),
                                  control2: CGPoint(x: o.x + n[2], y: o.y + n[3]))
                    at = end; n.removeFirst(6)
                }
            case "z", "Z": path.closeSubpath()
            default: break
            }
            numbers = []
        }
        var token = ""
        func endNumber() {
            if let v = Double(token) { numbers.append(CGFloat(v)) }
            token = ""
        }
        for ch in svg[start.upperBound..<end] {
            if ch.isLetter {
                endNumber(); flush(); command = ch
            } else if ch == "-" {
                endNumber(); token = "-"
            } else if ch == "," || ch.isWhitespace {
                endNumber()
            } else if ch == ".", token.contains(".") {
                endNumber(); token = "."
            } else {
                token.append(ch)
            }
        }
        endNumber(); flush()
        let size = 128
        return makeBitmap(width: size, height: size) { ctx in
            ctx.translateBy(x: 0, y: CGFloat(size))
            ctx.scaleBy(x: CGFloat(size) / 512, y: -CGFloat(size) / 512)
            ctx.addPath(path)
            ctx.setFillColor(gray: 1, alpha: 1)
            ctx.setStrokeColor(gray: 1, alpha: 1)
            ctx.setLineWidth(4)
            ctx.drawPath(using: .fillStroke)
        }
    }

    // MARK: Pipelines

    private func makeLibrary() -> MTLLibrary? {
        let options = MTLCompileOptions()
        options.preserveInvariance = true
        do { return try device.makeLibrary(source: Self.hologramSource, options: options) } catch {
            NSLog("[hologram] shaders: \(error)")
            return nil
        }
    }

    private func buildPipelines(color: MTLPixelFormat, amplification: Int) -> Bool {
        if let key = pipelineKey, key == (color, amplification) { return modelPipeline != nil }
        pipelineKey = (color, amplification)
        guard let library else { return false }
        func pipeline(_ vertex: String, _ fragment: String?, writesColor: Bool, premultiplied: Bool) -> MTLRenderPipelineState? {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: vertex)
            d.fragmentFunction = fragment.flatMap { library.makeFunction(name: $0) }
            d.depthAttachmentPixelFormat = .depth32Float
            d.maxVertexAmplificationCount = amplification
            let c = d.colorAttachments[0]!
            c.pixelFormat = color
            c.writeMask = writesColor ? .all : []
            if writesColor {
                c.isBlendingEnabled = true
                c.sourceRGBBlendFactor = premultiplied ? .one : .sourceAlpha
                c.sourceAlphaBlendFactor = .one
                c.destinationRGBBlendFactor = .oneMinusSourceAlpha
                c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            do { return try device.makeRenderPipelineState(descriptor: d) } catch {
                NSLog("[hologram] pipeline \(vertex): \(error)")
                return nil
            }
        }
        depthPipeline = pipeline("holo_model_v", nil, writesColor: false, premultiplied: false)
        modelPipeline = pipeline("holo_model_v", "holo_model_f", writesColor: true, premultiplied: false)
        panelPipeline = pipeline("holo_panel_v", "holo_panel_f", writesColor: true, premultiplied: true)
        panelDepthPipeline = pipeline("holo_panel_v", nil, writesColor: false, premultiplied: false)
        func state(_ compare: MTLCompareFunction, writes: Bool) -> MTLDepthStencilState? {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = writes
            return device.makeDepthStencilState(descriptor: d)
        }
        // Reverse-Z, as the rest of the pass. `OrEqual`: each model is drawn several times over the depth
        // of the first time.
        depthOnlyState = state(.greaterEqual, writes: true)
        depthWriteState = state(.greaterEqual, writes: true)
        depthTestState = state(.greaterEqual, writes: false)
        alwaysState = state(.always, writes: false)
        alwaysWriteState = state(.always, writes: true)
        return depthPipeline != nil && modelPipeline != nil && panelPipeline != nil
    }

    private static let hologramSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct HVertex { packed_float3 pos; packed_float3 nrm; packed_float2 uv; };
    struct HCamera { float4x4 clip; float4 eye; };
    struct HDraw { float4x4 world; float4 color; float4 fresnel; };
    struct HOut {
        float4 pos [[position, invariant]];
        float3 wpos;
        float3 nrm;
        float2 uv;
        float3 eye;
    };
    vertex HOut holo_model_v(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                             constant HVertex *v [[buffer(0)]],
                             constant HCamera *cams [[buffer(1)]],
                             constant HDraw &d [[buffer(2)]]) {
        HOut o;
        float4 w = d.world * float4(float3(v[vid].pos), 1.0);
        o.pos = cams[amp].clip * w;
        o.wpos = w.xyz;
        o.nrm = normalize((d.world * float4(float3(v[vid].nrm), 0.0)).xyz);
        o.uv = float2(v[vid].uv);
        o.eye = cams[amp].eye.xyz;
        return o;
    }
    fragment float4 holo_model_f(HOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                 sampler s [[sampler(0)]], constant HDraw &d [[buffer(0)]]) {
        float a = d.color.a;
        if (d.fresnel.z != 0.0) {
            float nv = abs(dot(normalize(in.nrm), normalize(in.eye - in.wpos)));
            a = mix(a, d.fresnel.x, min(d.fresnel.y * pow(1.0 - nv, 5.0), 1.0));
        }
        a = clamp(a, 0.0, 1.0) * tex.sample(s, in.uv).a * d.fresnel.w;
        // SteamVR blends in sRGB, this pass in linear light: over the black that is behind the map,
        // this alpha gives the same picture.
        return float4(d.color.rgb, pow(a, 2.2));
    }
    struct HPanel { float4 pos [[position]]; float2 uv; };
    vertex HPanel holo_panel_v(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                               constant HCamera *cams [[buffer(1)]],
                               constant HDraw &d [[buffer(2)]]) {
        float2 t = float2(vid & 1, (vid >> 1) & 1);
        HPanel o;
        o.pos = cams[amp].clip * d.world * float4(t.x - 0.5, 0.5 - t.y, 0.0, 1.0);
        o.uv = t;
        return o;
    }
    fragment float4 holo_panel_f(HPanel in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                 sampler s [[sampler(0)]], constant HDraw &d [[buffer(0)]]) {
        float4 c = tex.sample(s, in.uv);
        if (c.a <= 0.0) discard_fragment();
        float3 rgb = c.rgb / c.a;
        rgb = select(pow((rgb + 0.055) / 1.055, float3(2.4)), rgb / 12.92, rgb <= float3(0.04045));
        float a = pow(c.a * d.color.a, d.fresnel.x);
        return float4(rgb * a, a);
    }
    """
}
