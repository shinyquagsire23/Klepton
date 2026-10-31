//
//  KleptonRealityPresent.swift — a proof of concept: the game's finished
//  picture shown through RealityKit instead of a Compositor Services layer
//  (Settings › Advanced › "Picture through RealityKit", KL_RK_PRESENT).
//
//  Why: visionOS foveates what it draws for an app. A Metal app's layer gets a
//  FIXED rate map (sharp in a wide middle, measured with KL_CP_GAZE_TRACE),
//  while RealityKit content gets the eye-tracked one: sharp where the person
//  looks, wherever that is (a test app with mip-coloured charts, 2026-10-18).
//  The app never learns where that is, so the game has to draw its whole
//  picture at full resolution, without foveated rendering of its own, and a
//  pass is added: this costs frame rate, it does not save any. It is offered
//  for the picture quality off-centre, and as the measurement that it is.
//
//  How: the game starts as always on Compositor Services, which is where the
//  display's geometry (eye positions, frusta, rate) is measured. Then the
//  picture moves once to a RealityKit space (KLRealityPresent.switchAtStart),
//  and the running compositor takes the new output (KleptonCompositor.attach).
//  The composite runs unchanged into a two-slice texture of our own
//  (KLDrawable's stand-in) for two virtual eyes with the measured frusta. That
//  texture is a LowLevelTexture on a quad 10 m away, a LowLevelMesh whose four
//  corners are written in the same command buffer as the picture, at the pose
//  the picture was drawn for — so RealityKit takes picture and place together
//  and draws the quad for the head as it is when shown: a late picture stays
//  where it belongs. A ShaderGraph material picks slice and UV per eye with the
//  camera-index switch. At 10 m the gap between the virtual eyes and the real
//  ones is under a fifth of a degree.
//
import SwiftUI
import RealityKit
import Metal
import simd

enum KLRealityPresent {
    static let spaceID = "KleptonImmersiveRK"
    static var enabled: Bool { klEnvOn("KL_RK_PRESENT", default: false) }

    private static func env(_ k: String) -> String? { getenv(k).map { String(cString: $0) } }
    private static func envFloat(_ k: String, _ d: Float) -> Float { env(k).flatMap(Float.init) ?? d }

    /// Metres from the head to the quad. Far, so that the few millimetres
    /// between our virtual eyes and the real ones do not show.
    static var distance: Float { max(2, envFloat("KL_RK_DISTANCE", 10)) }
    /// How far after the display link's next frame the picture is predicted to
    /// be on the display: RealityKit picks the texture up in its next frame and
    /// shows it after that.
    static var latency: TimeInterval { TimeInterval(envFloat("KL_RK_LATENCY_MS", 22)) / 1000 }
    /// How much larger than the frusta the quad is (1.15 = 15 % each way).
    static var edgeMargin: Float { max(1, envFloat("KL_RK_MARGIN", 1.15)) }
    /// Width of each eye's picture; the height follows the frustum.
    static var eyeWidth: Int { max(1024, Int(envFloat("KL_RK_EYE_W", 6144))) & ~1 }
    /// The rate the guest is told, until the display link says otherwise.
    static var hz: Float { envFloat("KL_RK_HZ", 90) }

    /// The display's geometry. Compositor Services states it on every drawable;
    /// RealityKit does not, so it is taken from the drawables of the first
    /// seconds (KleptonCompositor.primeDisplay), before the picture moves.
    struct Geometry {
        var midpoint: SIMD3<Float>          // eye midpoint in the device anchor's frame
        var offsets: [SIMD3<Float>]         // each eye from the midpoint
        var tangents: [SIMD4<Float>]        // l r t b, as the display's
    }
    nonisolated(unsafe) static var measured: Geometry?
    /// What a Vision Pro reports when nothing was measured (105 x 95 degrees
    /// an eye, IPD 63 mm) — only so a failed measurement still shows a picture.
    static let typical = Geometry(midpoint: SIMD3(0, -0.025, 0),
                                  offsets: [SIMD3(-0.0315, 0, 0), SIMD3(0.0315, 0, 0)],
                                  tangents: [SIMD4(1.7320, 1.0, 1.0, 1.1917), SIMD4(1.0, 1.7320, 1.0, 1.1917)])
    static var geometry: Geometry { measured ?? typical }

    /// A reverse-Z, infinite-far projection for the given tangents — the kind
    /// Compositor Services hands out, so the composite needs no other branch.
    static func projection(_ t: SIMD4<Float>, near: Float = 0.05) -> simd_float4x4 {
        let l = t.x, r = t.y, top = t.z, b = t.w
        return simd_float4x4(SIMD4<Float>(2 / (l + r), 0, 0, 0),
                             SIMD4<Float>(0, 2 / (top + b), 0, 0),
                             SIMD4<Float>((r - l) / (l + r), (top - b) / (top + b), 0, -1),
                             SIMD4<Float>(0, 0, near, 0))
    }
}

/// The texture RealityKit shows and the quad it is on.
final class KLRealityTarget: @unchecked Sendable {
    /// What the composite renders into: the system drawable's format if
    /// RealityKit takes it, else the next one it does. Settled when the target
    /// is made, which is before the compositor builds its pipelines.
    nonisolated(unsafe) static var colorFormat: MTLPixelFormat = .bgra8Unorm_srgb

    let width: Int
    let height: Int
    let texture: LowLevelTexture
    let depth: MTLTexture
    let views: [KLDrawable.View]
    let projections: [simd_float4x4]
    let geometry = KLRealityPresent.geometry
    let mipLevels: Int
    /// The quad as a mesh whose four corners are written on the GPU, in the
    /// same command buffer as the picture, so RealityKit takes both at the same
    /// moment: the picture and the place it was drawn for cannot fall apart.
    @MainActor let mesh: LowLevelMesh
    /// The corners in the device frame and their UVs (v up).
    let corners: [SIMD3<Float>]
    let cornerUV: [SIMD2<Float>]
    /// Staging for the corners, a few frames deep so the CPU never writes one
    /// the GPU is still copying from.
    private let staging: [MTLBuffer]
    private var stagingIndex = 0
    /// Union frustum tangents with the margin (for the per-eye UV mapping).
    let unionTangents: SIMD4<Float>

    /// Written by the display link before each frame (seconds, CACurrentMediaTime's clock).
    var targetTimestamp: TimeInterval = 0
    /// Set when the space goes away; the render loop ends on it.
    private let lock = NSLock()
    private var _stopped = false
    var stopped: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _stopped }
        set { lock.lock(); _stopped = newValue; lock.unlock() }
    }

    @MainActor init(device: MTLDevice) throws {
        let g = geometry
        // One size for both eyes: the wider of the two frusta, aspect from the taller.
        let wTan = g.tangents.map { $0.x + $0.y }.max() ?? 2.732
        let hTan = g.tangents.map { $0.z + $0.w }.max() ?? 2.19
        width = KLRealityPresent.eyeWidth
        height = (Int((Float(width) * hTan / wTan).rounded()) + 1) & ~1
        mipLevels = klEnvOn("KL_RK_MIPS", default: true)
            ? Int(floor(log2(Double(max(width, height))))) + 1 : 1
        var made: LowLevelTexture?
        var lastError: Error?
        for format in [MTLPixelFormat.bgra8Unorm_srgb, .rgba8Unorm_srgb, .rgba16Float] {
            let desc = LowLevelTexture.Descriptor(textureType: .type2DArray, pixelFormat: format,
                                                  width: width, height: height, depth: 1,
                                                  mipmapLevelCount: mipLevels, arrayLength: 2,
                                                  textureUsage: [.renderTarget, .shaderRead])
            do {
                made = try LowLevelTexture(descriptor: desc)
                Self.colorFormat = format
                break
            } catch {
                NSLog("[rk] RealityKit refused a \(format.rawValue) texture: \(error)")
                lastError = error
            }
        }
        guard let made else { throw lastError ?? NSError(domain: "KLRealityTarget", code: 2) }
        texture = made
        let dd = MTLTextureDescriptor()
        dd.textureType = .type2DArray
        dd.pixelFormat = .depth32Float
        dd.width = width; dd.height = height; dd.arrayLength = 2
        dd.usage = [.renderTarget]
        // Memoryless: the composite clears and never keeps it (no system
        // reprojection reads it here), and at this size it would be 240 MB.
        dd.storageMode = .memoryless
        guard let d = device.makeTexture(descriptor: dd) else {
            throw NSError(domain: "KLRealityTarget", code: 1)
        }
        depth = d
        let vp = MTLViewport(originX: 0, originY: 0, width: Double(width), height: Double(height),
                             znear: 0, zfar: 1)
        views = (0..<2).map { i in
            var m = matrix_identity_float4x4
            let p = g.midpoint + g.offsets[i]
            m.columns.3 = SIMD4<Float>(p.x, p.y, p.z, 1)
            return KLDrawable.View(transform: m,
                                   textureMap: .init(textureIndex: 0, sliceIndex: i, viewport: vp))
        }
        projections = g.tangents.map { KLRealityPresent.projection($0) }

        // The quad: the union of both frusta and a margin, D metres out, in the
        // device frame. Between two pictures the head turns while the quad stays
        // where the last one was drawn, so its edge must not come into view at once.
        let D = KLRealityPresent.distance
        let margin = KLRealityPresent.edgeMargin
        let Lu = (g.tangents.map(\.x).max() ?? 1.75) * margin
        let Ru = (g.tangents.map(\.y).max() ?? 1.75) * margin
        let Tu = (g.tangents.map(\.z).max() ?? 1.0) * margin
        let Bu = (g.tangents.map(\.w).max() ?? 1.2) * margin
        unionTangents = SIMD4(Lu, Ru, Tu, Bu)
        let m = g.midpoint
        var cs: [SIMD3<Float>] = [], uv: [SIMD2<Float>] = []
        for (u, v) in [(Float(0), Float(0)), (1, 0), (0, 1), (1, 1)] {
            let tx = -Lu + u * (Lu + Ru), ty = -Bu + v * (Tu + Bu)
            cs.append(SIMD3(m.x + tx * D, m.y + ty * D, m.z - D))
            uv.append(SIMD2(u, v))
        }
        corners = cs; cornerUV = uv
        var md = LowLevelMesh.Descriptor()
        md.vertexAttributes = [.init(semantic: .position, format: .float3, layoutIndex: 0, offset: 0),
                               .init(semantic: .uv0, format: .float2, layoutIndex: 0, offset: 12)]
        md.vertexLayouts = [.init(bufferIndex: 0, bufferStride: 20)]
        md.vertexCapacity = 4
        md.indexCapacity = 6
        md.indexType = .uint16
        let lm = try LowLevelMesh(descriptor: md)
        lm.withUnsafeMutableIndices { raw in
            let p = raw.bindMemory(to: UInt16.self)
            for (i, x) in [UInt16(0), 1, 2, 2, 1, 3].enumerated() { p[i] = x }
        }
        // Nothing to see until the first picture places it: all four corners at 0.
        lm.withUnsafeMutableBytes(bufferIndex: 0) { raw in raw.initializeMemory(as: UInt8.self, repeating: 0) }
        // Bounds that never cull it: the quad moves with the head every frame.
        lm.parts.replaceAll([LowLevelMesh.Part(indexOffset: 0, indexCount: 6, topology: .triangle, materialIndex: 0,
                                               bounds: BoundingBox(min: [-500, -500, -500], max: [500, 500, 500]))])
        mesh = lm
        staging = try (0..<4).map { _ in
            guard let b = device.makeBuffer(length: 4 * 20, options: .storageModeShared) else {
                throw NSError(domain: "KLRealityTarget", code: 3)
            }
            return b
        }
        NSLog("[rk] target \(width)x\(height) x2, \(mipLevels) mip level(s), quad at "
              + "\(KLRealityPresent.distance) m in the world, latency \(Int(KLRealityPresent.latency * 1000)) ms")
    }

    /// The texture to render this frame into, and the quad's corners for the
    /// pose it is drawn for, both in `cmd`: RealityKit takes the two together
    /// once it has completed. The quad lives in the WORLD, not on the head, so
    /// RealityKit draws it for the head as it is when shown and a late picture
    /// stays where it belongs (anchored to the head, it was dragged along with
    /// every turn). LowLevelTexture and LowLevelMesh are main-actor only, so
    /// this hops there for the moment it takes to hand them out.
    func beginFrame(_ cmd: MTLCommandBuffer, pose: simd_float4x4?) -> MTLTexture {
        // The corners where the head is for this picture, written now (CPU) and
        // copied into the mesh by this command buffer.
        var cornerBuffer: MTLBuffer?
        if let pose {
            let b = staging[stagingIndex]
            stagingIndex = (stagingIndex + 1) % staging.count
            let p = b.contents().bindMemory(to: Float.self, capacity: 20)
            for i in 0..<4 {
                let w = pose * SIMD4<Float>(corners[i], 1)
                p[i * 5 + 0] = w.x; p[i * 5 + 1] = w.y; p[i * 5 + 2] = w.z
                p[i * 5 + 3] = cornerUV[i].x; p[i * 5 + 4] = cornerUV[i].y
            }
            cornerBuffer = b
        }
        let hand = { () -> MTLTexture in
            MainActor.assumeIsolated {
                if let cornerBuffer, let blit = cmd.makeBlitCommandEncoder() {
                    let vb = self.mesh.replace(bufferIndex: 0, using: cmd)
                    blit.copy(from: cornerBuffer, sourceOffset: 0, to: vb, destinationOffset: 0, size: 4 * 20)
                    blit.endEncoding()
                }
                return self.texture.replace(using: cmd)
            }
        }
        if Thread.isMainThread { return hand() }
        return DispatchQueue.main.sync { hand() }
    }

    /// After the composite: the mip chain, so the parts the eye is not on are
    /// filtered instead of shimmering.
    func finishFrame(_ tex: MTLTexture, _ cmd: MTLCommandBuffer) {
        if mipLevels > 1, let blit = cmd.makeBlitCommandEncoder() {
            blit.generateMipmaps(for: tex)
            blit.endEncoding()
        }
    }

    // MARK: The quad

    /// The material: per eye, a slice of the array and a UV mapping onto it.
    private func materialSource(flipV: Bool) -> String {
        """
        #usda 1.0
        (
            defaultPrim = "Root"
            metersPerUnit = 1
            upAxis = "Y"
        )

        def Xform "Root"
        {
            def Material "Eyes"
            {
                asset inputs:eyes
                float2 inputs:scaleL = (1, 1)
                float2 inputs:offsetL = (0, 0)
                float2 inputs:scaleR = (1, 1)
                float2 inputs:offsetR = (0, 0)
                token outputs:mtlx:surface.connect = </Root/Eyes/Unlit.outputs:out>
                token outputs:realitykit:vertex

                def Shader "Unlit"
                {
                    uniform token info:id = "ND_realitykit_unlit_surfaceshader"
                    bool inputs:applyPostProcessToneMap = 0
                    bool inputs:hasPremultipliedAlpha = 0
                    color3f inputs:color.connect = </Root/Eyes/RGB.outputs:out>
                    float inputs:opacity = 1
                    token outputs:out
                }

                def Shader "RGB"
                {
                    uniform token info:id = "ND_convert_color4_color3"
                    color4f inputs:in.connect = </Root/Eyes/Tex.outputs:out>
                    color3f outputs:out
                }

                def Shader "Tex"
                {
                    uniform token info:id = "ND_RealityKitTexture2DArray_color4"
                    asset inputs:file.connect = </Root/Eyes.inputs:eyes>
                    int inputs:index.connect = </Root/Eyes/Slice.outputs:out>
                    float2 inputs:texcoord.connect = </Root/Eyes/UV.outputs:out>
                    string inputs:u_wrap_mode = "clamp_to_zero"
                    string inputs:v_wrap_mode = "clamp_to_zero"
                    bool inputs:no_flip_v = \(flipV ? 1 : 0)
                    color4f outputs:out
                }

                def Shader "Slice"
                {
                    uniform token info:id = "ND_realitykit_geometry_switch_cameraindex_integer"
                    int inputs:mono = 0
                    int inputs:left = 0
                    int inputs:right = 1
                    int outputs:out
                }

                def Shader "Scale"
                {
                    uniform token info:id = "ND_realitykit_geometry_switch_cameraindex_vector2"
                    float2 inputs:mono.connect = </Root/Eyes.inputs:scaleL>
                    float2 inputs:left.connect = </Root/Eyes.inputs:scaleL>
                    float2 inputs:right.connect = </Root/Eyes.inputs:scaleR>
                    float2 outputs:out
                }

                def Shader "Offset"
                {
                    uniform token info:id = "ND_realitykit_geometry_switch_cameraindex_vector2"
                    float2 inputs:mono.connect = </Root/Eyes.inputs:offsetL>
                    float2 inputs:left.connect = </Root/Eyes.inputs:offsetL>
                    float2 inputs:right.connect = </Root/Eyes.inputs:offsetR>
                    float2 outputs:out
                }

                def Shader "UV0"
                {
                    uniform token info:id = "ND_texcoord_vector2"
                    float2 outputs:out
                }

                def Shader "Scaled"
                {
                    uniform token info:id = "ND_multiply_vector2"
                    float2 inputs:in1.connect = </Root/Eyes/UV0.outputs:out>
                    float2 inputs:in2.connect = </Root/Eyes/Scale.outputs:out>
                    float2 outputs:out
                }

                def Shader "UV"
                {
                    uniform token info:id = "ND_add_vector2"
                    float2 inputs:in1.connect = </Root/Eyes/Scaled.outputs:out>
                    float2 inputs:in2.connect = </Root/Eyes/Offset.outputs:out>
                    float2 outputs:out
                }
            }
        }
        """
    }

    /// The head-anchored quad with the material, ready to add to the scene.
    @MainActor func makeEntity() async throws -> Entity {
        let g = geometry
        let D = KLRealityPresent.distance
        let Lu = unionTangents.x, Ru = unionTangents.y, Tu = unionTangents.z, Bu = unionTangents.w
        let resource = try await MeshResource(from: mesh)

        // The material, from source written next to the app's temporary files.
        let flipV = false
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("klrk", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let file = dir.appendingPathComponent("eyes.usda")
        try materialSource(flipV: flipV).write(to: file, atomically: true, encoding: .utf8)
        var material = try await ShaderGraphMaterial(named: "/Root/Eyes", from: "eyes.usda",
                                                     in: Bundle(url: dir))
        let eyes = try await TextureResource(from: texture)
        try material.setParameter(name: "eyes", value: .textureResource(eyes))
        // Per eye: from the quad's UV (union frustum) to that eye's picture,
        // allowing for where the eye sits (its offset seen from D metres).
        for (i, name) in ["L", "R"].enumerated() {
            let t = g.tangents[i], o = g.offsets[i]
            // The array is sampled with its rows top-down (seen on the headset,
            // 2026-10-18: the first build stood on its head), so v counts from
            // the top: 1 - (tanY + b) / (t + b). KL_RK_FLIPV=1 is the other way.
            let vUp = (Tu + Bu) / (t.z + t.w), vUp0 = (t.w - Bu - o.y / D) / (t.z + t.w)
            let flip = klEnvOn("KL_RK_FLIPV", default: false)
            let scale = SIMD2<Float>((Lu + Ru) / (t.x + t.y), flip ? vUp : -vUp)
            let offset = SIMD2<Float>((t.x - Lu - o.x / D) / (t.x + t.y), flip ? vUp0 : 1 - vUp0)
            try material.setParameter(name: "scale" + name, value: .simd2Float(scale))
            try material.setParameter(name: "offset" + name, value: .simd2Float(offset))
            NSLog("[rk] eye \(name): uv scale (\(scale.x), \(scale.y)) offset (\(offset.x), \(offset.y))")
        }
        // In the world at the origin: where it is, the corners say.
        return ModelEntity(mesh: resource, materials: [material])
    }
}

// MARK: - Moving the picture at start

extension KLRealityPresent {
    static let switchWindowID = "klrk-switch"

    /// The space actions, taken from the start window when it opened the
    /// first space; they stay usable after that window has gone.
    @MainActor private static var open: OpenImmersiveSpaceAction?
    @MainActor private static var dismiss: DismissImmersiveSpaceAction?
    @MainActor private static var openWindow: OpenWindowAction?
    @MainActor private static var dismissWindow: DismissWindowAction?
    /// True while the picture moves; the lifecycle does not exit meanwhile.
    nonisolated(unsafe) static var switching = false

    @MainActor static func capture(open: OpenImmersiveSpaceAction, dismiss: DismissImmersiveSpaceAction,
                                   openWindow: OpenWindowAction, dismissWindow: DismissWindowAction) {
        Self.open = open; Self.dismiss = dismiss
        Self.openWindow = openWindow; Self.dismissWindow = dismissWindow
    }

    /// Called by the compositor once the display is measured: the picture
    /// leaves the Compositor Services space for the RealityKit one. A small
    /// window is shown meanwhile, so the app is never without a scene.
    static func switchAtStart() {
        DispatchQueue.main.async {
            Task { @MainActor in
                // The start window hands over its actions as soon as the
                // first space is open; wait for them a moment if need be.
                for _ in 0..<50 where open == nil { try? await Task.sleep(for: .milliseconds(100)) }
                guard let open, let dismiss else {
                    NSLog("[rk] cannot move the picture: no space actions — staying on Compositor Services")
                    KleptonCompositor.handoverFailed()
                    return
                }
                switching = true
                defer { switching = false }
                openWindow?(id: switchWindowID)
                await dismiss()
                let r = await open(id: spaceID)
                NSLog("[rk] open \(spaceID) -> \(r)")
                if case .opened = r {} else { KleptonCompositor.handoverFailed() }
                dismissWindow?(id: switchWindowID)
            }
        }
    }
}

struct KLRealitySwitchView: View {
    var body: some View {
        Text(klT("Picture → RealityKit …", "Bild → RealityKit …"))
            .font(.title2)
            .padding(24)
    }
}

/// The setting (Settings › Advanced), stored per game.
enum KLRealityPresentSetting {
    static let key = "klepton.realityKitPresent"
    static var on: Bool { UserDefaults.standard.bool(forKey: key) }
    /// Not for a game that shows the room itself: the quad would cover it.
    static var offered: Bool { !KleptonRendering.targetIsMixedReality }

    /// Into the environment, before kl_app_configure and before the picture
    /// settings, so the game's own foveated rendering is off whatever those say:
    /// the system foveates RealityKit's picture by the eyes, and a fixed rate
    /// map in the game would blur exactly where they may land. Never over the
    /// launch line's values.
    static func applyEnvironment() {
        guard on && offered || klEnvOn("KL_RK_PRESENT", default: false) else { return }
        setenv("KL_RK_PRESENT", "1", 0)
        setenv("KL_VRR", "0", 0)            // GL games
        setenv("KL_FOVEATION", "off", 0)    // Vulkan games (kl_fove)
        setenv("KL_VK_FOVEATE", "0", 0)     // Vulkan, the Metal-side pass
        NSLog("[app] picture through RealityKit (proof of concept): on — the game's own foveated rendering is off")
    }
}

struct RealityPresentView: View {
    @AppStorage(KLRealityPresentSetting.key) private var on = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle(klT("Picture through RealityKit (proof of concept)",
                       "Bild über RealityKit (Proof of Concept)"), isOn: $on)
            Text(klT("A proof of concept, not an improvement: it makes the game run WORSE. The picture is handed to visionOS through RealityKit instead of the usual way, and visionOS then makes it sharp where your eyes are rather than only in the middle. For that the game has to draw its whole picture at full resolution, without its own foveated rendering, and one more pass is added — expect fewer frames a second and a warmer headset. At the start the picture moves once from the usual space to the RealityKit one (a moment of your room in between). Takes effect the next time the game starts.",
                     "Ein Proof of Concept, keine Verbesserung: Das Spiel läuft damit SCHLECHTER. Das Bild geht über RealityKit an visionOS statt auf dem üblichen Weg, und visionOS macht es dann dort scharf, wo deine Augen sind, statt nur in der Mitte. Dafür muss das Spiel sein ganzes Bild in voller Auflösung zeichnen, ohne eigenes Foveated Rendering, und es kommt ein Durchgang dazu – rechne mit weniger Bildern pro Sekunde und einem wärmeren Headset. Beim Start wechselt das Bild einmal aus dem üblichen Raum in den RealityKit-Raum (kurz ist dazwischen dein Raum zu sehen). Wirkt beim nächsten Start des Spiels."))
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}

/// The immersive space of this mode: the quad, and the compositor that feeds it.
struct KleptonRealitySpace: View {
    @State private var target: KLRealityTarget?

    var body: some View {
        RealityView { content in
            guard let device = MTLCreateSystemDefaultDevice() else { return }
            do {
                let t = try KLRealityTarget(device: device)
                content.add(try await t.makeEntity())
                target = t
                NSLog("[rk] space open — starting the compositor on RealityKit")
                KleptonCompositor.attach(rk: t)
            } catch {
                NSLog("[rk] could not set up RealityKit presentation: \(error)")
            }
        }
        .onDisappear {
            NSLog("[rk] space closed")
            target?.stopped = true
        }
    }
}

/// Paces the RealityKit loop off the display link, on the compositor's own
/// thread — or, if the link does not fire there within half a second, off a
/// plain timer at the requested rate, so a missing link is a coarser pace and
/// not a frozen game.
final class KLRealityTicker: NSObject {
    private let body: (TimeInterval) -> Bool
    private var link: CADisplayLink?
    private var running = true
    private var lastTick: CFTimeInterval = 0

    /// `body` gets the time the next frame is due (CACurrentMediaTime's clock)
    /// and returns false to stop.
    init(_ body: @escaping (TimeInterval) -> Bool) { self.body = body }

    func run(preferredHz: Float) {
        let l = CADisplayLink(target: self, selector: #selector(tick(_:)))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: 45, maximum: 120, preferred: preferredHz)
        l.add(to: .current, forMode: .default)
        link = l
        let started = CACurrentMediaTime()
        var fallback = false
        let period = 1.0 / Double(max(30, preferredHz))
        while running {
            if !fallback {
                _ = RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.1))
                if lastTick == 0 && CACurrentMediaTime() - started > 0.5 {
                    fallback = true
                    link?.invalidate(); link = nil
                    NSLog("[rk] the display link does not fire on this thread — pacing off a timer at %.0f Hz",
                          1 / period)
                }
            } else {
                let next = CACurrentMediaTime() + period
                if !body(next) { running = false; break }
                let wait = next - CACurrentMediaTime()
                if wait > 0 { Thread.sleep(forTimeInterval: wait) }
            }
        }
    }

    @objc private func tick(_ l: CADisplayLink) {
        lastTick = CACurrentMediaTime()
        if !body(l.targetTimestamp) {
            l.invalidate()
            link = nil
            running = false
        }
    }
}
