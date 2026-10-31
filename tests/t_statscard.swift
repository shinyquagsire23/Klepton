// The metrics panel (KL_STATS_CARD) over a guest's quad layers, run on the Mac's GPU — `make statscard`.
//
// The composite pass of KleptonCompositor.encodeComposite, cut down to one view: the eye picture as a quad
// at KL_REPROJECT_DEPTH, the guest's quad layers, then the panel, each through the real overlay shader
// (kl_reproject_overlay_msl), placed by the real kl_overlay_build, with the depth states of
// KleptonOverlayDepth and the panel's own drawing (KleptonMetrics.swift).
//
// Alien: Rogue Incursion (KL_OVERLAY_SUBMIT_ORDER on): in the menu its panels are metres away; in a level
// it also submits quads within reach — a panel held in the hand with an invisible margin, interaction
// labels (KleptonCompositor, overlaysInSubmitOrder). The level frame here is a model of that: a hand panel
// at 0.45 m in the panel's line of sight and a label at 0.8 m. Each writes its distance over its whole
// rectangle, and the metrics panel at 1.2 m, drawn after them with `.greater`, was not drawn at all. The
// same holds without submit order (the eye pass's `.greater` with writes stays in force for the quads).
//
// Writes the frames as PNGs (menu/level, before/after) to $T_STATSCARD_SHOTS or build/.
import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers
import simd

// kl_reproject.c asks for the target's name; this test runs as no title at all.
@_cdecl("kl_driver_target_name") func kl_driver_target_name() -> UnsafePointer<CChar>? { nil }

@main
enum TStatsCard {
    static func main() {

        var failed = false
        func ok(_ cond: Bool, _ what: String) {
            print("  " + what.padding(toLength: 72, withPad: " ", startingAt: 0) + (cond ? " ok" : " FAIL"))
            if !cond { failed = true }
        }

        guard let dev = MTLCreateSystemDefaultDevice(), let queue = dev.makeCommandQueue() else {
            print("  no Metal device — SKIP"); exit(0)
        }
        let lib: MTLLibrary
        do { lib = try dev.makeLibrary(source: String(cString: kl_reproject_overlay_msl()), options: nil) }
        catch { print("  \(error)"); ok(false, "the overlay shader compiles"); exit(1) }

        // The compositor's overlay pipeline (KleptonCompositor, "The overlay pass"), one view.
        let pd = MTLRenderPipelineDescriptor()
        pd.vertexFunction = lib.makeFunction(name: "kl_ov_v")
        pd.fragmentFunction = lib.makeFunction(name: "kl_ov_f")
        pd.colorAttachments[0].pixelFormat = .rgba8Unorm
        pd.depthAttachmentPixelFormat = .depth32Float
        pd.colorAttachments[0].isBlendingEnabled = true
        pd.colorAttachments[0].rgbBlendOperation = .add
        pd.colorAttachments[0].alphaBlendOperation = .add
        pd.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
        pd.colorAttachments[0].sourceAlphaBlendFactor = .one
        pd.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        pd.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        let pipe = try! dev.makeRenderPipelineState(descriptor: pd)

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        let sampler = dev.makeSamplerState(descriptor: sd)!

        let nearerWins = dev.makeDepthStencilState(descriptor: KleptonOverlayDepth.nearerWins())!
        let inOrder = dev.makeDepthStencilState(descriptor: KleptonOverlayDepth.inOrder())!
        let cardShipped = dev.makeDepthStencilState(descriptor: KleptonOverlayDepth.statsCard())!

        // A view about as wide as the headset's, square.
        let W = 1400, H = 1400
        let P = kl_reproject_projection(0.8, 0.8, 0.8, 0.8, 0.03)

        func texture(_ w: Int, _ h: Int, _ px: (Int, Int) -> (UInt8, UInt8, UInt8, UInt8)) -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
            d.usage = .shaderRead
            let t = dev.makeTexture(descriptor: d)!
            var bytes = [UInt8](repeating: 0, count: w * h * 4)
            for y in 0..<h { for x in 0..<w {
                let (r, g, b, a) = px(x, y); let i = (y * w + x) * 4
                bytes[i] = r; bytes[i + 1] = g; bytes[i + 2] = b; bytes[i + 3] = a
            } }
            t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
            return t
        }

        // The eye pictures: a menu backdrop, and a dim corridor with ceiling lights for the level.
        let menuEye = texture(512, 512) { x, y in
            let v = UInt8(20 + y * 40 / 512); return (v, v, UInt8(min(255, Int(v) + 40)), 255)
        }
        let levelEye = texture(512, 512) { x, y in
            let light = (x / 64) % 2 == 0 && y < 60
            let floor = y > 360
            let base = light ? 200 : (floor ? 38 : 22 + (x % 128 < 4 ? 30 : 0))
            return (UInt8(base / 2), UInt8(base), UInt8(base * 3 / 4), 255)
        }
        // The guest's quads: a menu panel, a hand-held panel whose margin is transparent, an interaction label.
        let menuPanel = texture(256, 160) { x, y in
            (y / 20) % 3 == 1 && x > 20 && x < 236 ? (230, 230, 230, 255) : (40, 44, 52, 220)
        }
        let handPanel = texture(300, 200) { x, y in
            let inner = x > 60 && x < 240 && y > 50 && y < 150
            return inner ? (30, 160, 70, 255) : (0, 0, 0, 0)
        }
        let label = texture(250, 80) { x, y in
            x > 10 && x < 240 && y > 10 && y < 70 ? (240, 240, 240, 255) : (0, 0, 0, 0)
        }

        // The metrics panel as startStats makes it: the panel in three levels, each drawn for itself.
        var samples: [KLMetricsSample] = []
        for i in 0..<30 {
            var s = KLMetricsSample()
            s.displayHz = 90; s.fps = i < 20 ? 72 : 45
            s.gpuBusy = 0.82; s.gpuGameMs = 11.2; s.gpuCompMs = 0.9
            s.cpuCores = 2.6; s.cpuTop = 0.7; s.cpuTopName = "GameThread"
            s.memUsedMB = 3900; s.memLeftMB = 1400
            s.eyeW = 2574; s.eyeH = 2094
            s.path = "Vulkan · array"
            samples.append(s)
        }
        let px = KLMetricsPanel.panelPixels, levels = KLMetricsPanel.panelLevels
        let cd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: px.w, height: px.h, mipmapped: false)
        cd.mipmapLevelCount = levels
        cd.usage = .shaderRead
        let card = dev.makeTexture(descriptor: cd)!
        for level in 0..<levels {
            let ctx = KLMetricsPanel.makeContext(w: px.w >> level, h: px.h >> level, units: KLMetricsPanel.panelLayout.w)!
            KLMetricsPanel.drawPanel(ctx, samples: samples)
            card.replace(region: MTLRegionMake2D(0, 0, px.w >> level, px.h >> level), mipmapLevel: level,
                         withBytes: ctx.data!, bytesPerRow: (px.w >> level) * 4)
        }

        func quad(_ w: Int, _ h: Int, at p: SIMD3<Float>, size: SIMD2<Float>, headLocked: Bool = false) -> kl_ovrp_overlay {
            var ov = kl_ovrp_overlay()
            ov.shape = 0
            ov.head_locked = headLocked ? 1 : 0
            ov.tex_w = Int32(w); ov.tex_h = Int32(h)
            ov.viewport.0 = (0, 0, Int32(w), Int32(h))
            ov.viewport.1 = (0, 0, Int32(w), Int32(h))
            ov.pose = (0, 0, 0, 1, p.x, p.y, p.z)
            ov.size = (size.x, size.y)
            ov.origin_top_left = 1
            return ov
        }
        // encodeStatsCard's panel, where and as large as the compositor puts it.
        let cardQuad = quad(px.w, px.h, at: SIMD3(-0.05, 0.101, -1.2), size: KLMetricsPanel.panelSize, headLocked: true)

        struct Layer { var tex: MTLTexture; var ov: kl_ovrp_overlay }
        let eyeQuad = { (t: MTLTexture) in Layer(tex: t, ov: quad(t.width, t.height, at: SIMD3(0, 0, -500), size: SIMD2(1000, 1000))) }
        let menu = [Layer(tex: menuPanel, ov: quad(256, 160, at: SIMD3(0.1, -0.05, -2.5), size: SIMD2(1.6, 1.0)))]
        let level = [Layer(tex: handPanel, ov: quad(300, 200, at: SIMD3(-0.02, 0.02, -0.45), size: SIMD2(0.30, 0.20))),
                     Layer(tex: label, ov: quad(250, 80, at: SIMD3(0.12, 0.12, -0.8), size: SIMD2(0.25, 0.08)))]

        // One frame: the eye pass, the guest's quads, then the panel (nil: not shown) — encodeComposite's order.
        func frame(eye: MTLTexture, guest: [Layer], guestState: MTLDepthStencilState,
                   cardState: MTLDepthStencilState?) -> [UInt8] {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
            td.usage = .renderTarget; td.storageMode = .shared
            let color = dev.makeTexture(descriptor: td)!
            let dd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: W, height: H, mipmapped: false)
            dd.usage = .renderTarget; dd.storageMode = .private
            let depth = dev.makeTexture(descriptor: dd)!
            let rp = MTLRenderPassDescriptor()
            rp.colorAttachments[0].texture = color
            rp.colorAttachments[0].loadAction = .clear
            rp.colorAttachments[0].storeAction = .store
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
            rp.depthAttachment.texture = depth
            rp.depthAttachment.loadAction = .clear
            rp.depthAttachment.storeAction = .store
            rp.depthAttachment.clearDepth = 0
            let cmd = queue.makeCommandBuffer()!
            let enc = cmd.makeRenderCommandEncoder(descriptor: rp)!
            enc.setRenderPipelineState(pipe)
            enc.setFragmentSamplerState(sampler, index: 0)
            func draw(_ l: Layer, _ state: MTLDepthStencilState) {
                var ov = l.ov
                var u = kl_overlay_build(&ov, 0, matrix_identity_float4x4, matrix_identity_float4x4, P)
                enc.setDepthStencilState(state)
                enc.setFragmentTexture(l.tex, index: 0)
                enc.setVertexBytes(&u, length: MemoryLayout<kl_overlay_uniforms>.stride, index: 0)
                enc.setFragmentBytes(&u, length: MemoryLayout<kl_overlay_uniforms>.stride, index: 0)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            draw(eyeQuad(eye), nearerWins)
            for l in guest { draw(l, guestState) }
            if let cardState { draw(Layer(tex: card, ov: cardQuad), cardState) }
            enc.endEncoding()
            cmd.commit(); cmd.waitUntilCompleted()
            var out = [UInt8](repeating: 0, count: W * H * 4)
            color.getBytes(&out, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0)
            return out
        }

        // The panel's rectangle on screen, from the uniforms the shader gets.
        var cov = cardQuad
        let cu = kl_overlay_build(&cov, 0, matrix_identity_float4x4, matrix_identity_float4x4, P)
        var xs: [Float] = [], ys: [Float] = []
        for c in [SIMD2<Float>(-1, -1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(1, 1)] {
            let p = cu.projection * (cu.model_view * SIMD4(c.x * cu.half_size.x, c.y * cu.half_size.y, 0, 1))
            xs.append((p.x / p.w * 0.5 + 0.5) * Float(W)); ys.append((0.5 - p.y / p.w * 0.5) * Float(H))
        }
        let rx = (Int(xs.min()!) + 3)..<(Int(xs.max()!) - 3), ry = (Int(ys.min()!) + 3)..<(Int(ys.max()!) - 3)

        // How much of the panel is to be seen: of the pixels the panel changes over the bare eye picture, the
        // share it also changes in this frame.
        func changed(_ a: [UInt8], _ b: [UInt8], _ x: Int, _ y: Int) -> Bool {
            let i = (y * W + x) * 4
            return abs(Int(a[i]) - Int(b[i])) + abs(Int(a[i + 1]) - Int(b[i + 1])) + abs(Int(a[i + 2]) - Int(b[i + 2])) > 24
        }
        func shown(_ eye: MTLTexture, _ guest: [Layer], _ guestState: MTLDepthStencilState,
                   _ cardState: MTLDepthStencilState) -> (Double, [UInt8]) {
            let ref = frame(eye: eye, guest: [], guestState: guestState, cardState: cardState)
            let bare = frame(eye: eye, guest: [], guestState: guestState, cardState: nil)
            let with = frame(eye: eye, guest: guest, guestState: guestState, cardState: cardState)
            let without = frame(eye: eye, guest: guest, guestState: guestState, cardState: nil)
            var want = 0, got = 0
            for y in ry { for x in rx where changed(ref, bare, x, y) {
                want += 1
                if changed(with, without, x, y) { got += 1 }
            } }
            return (want == 0 ? 0 : Double(got) / Double(want), with)
        }

        func png(_ px: [UInt8], _ path: String) {
            let cs = CGColorSpace(name: CGColorSpace.sRGB)!
            let data = CFDataCreate(nil, px, px.count)!
            let img = CGImage(width: W, height: H, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: W * 4, space: cs,
                              bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                              provider: CGDataProvider(data: data)!, decode: nil, shouldInterpolate: false,
                              intent: .defaultIntent)!
            let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
            CGImageDestinationAddImage(dst, img, nil)
            CGImageDestinationFinalize(dst)
        }
        let shots = ProcessInfo.processInfo.environment["T_STATSCARD_SHOTS"] ?? "build"
        try? FileManager.default.createDirectory(atPath: shots, withIntermediateDirectories: true)

        print("=== the metrics panel over the guest's quad layers ===")
        print("  panel on screen: x \(rx), y \(ry)")
        // Before: the panel drawn with the eye pass's state, as encodeStatsCard did.
        let (menuBefore, menuBeforePx) = shown(menuEye, menu, inOrder, nearerWins)
        let (levelBefore, levelBeforePx) = shown(levelEye, level, inOrder, nearerWins)
        let (levelPlainBefore, _) = shown(levelEye, level, nearerWins, nearerWins)
        print(String(format: "  before: menu %.0f %%, level %.0f %% (submit order), level %.0f %% (depth-tested quads)",
                     menuBefore * 100, levelBefore * 100, levelPlainBefore * 100))
        ok(menuBefore > 0.95, "before: the panel shows in Alien's menu")
        ok(levelBefore < 0.05, "before: reproduced — the panel is gone in a level (near quads, submit order)")
        // After: KleptonOverlayDepth.statsCard, what the compositor now builds the panel's state from.
        let (menuAfter, menuAfterPx) = shown(menuEye, menu, inOrder, cardShipped)
        let (levelAfter, levelAfterPx) = shown(levelEye, level, inOrder, cardShipped)
        let (levelPlainAfter, _) = shown(levelEye, level, nearerWins, cardShipped)
        print(String(format: "  after:  menu %.0f %%, level %.0f %% (submit order), level %.0f %% (depth-tested quads)",
                     menuAfter * 100, levelAfter * 100, levelPlainAfter * 100))
        ok(menuAfter > 0.95, "after: the panel shows in the menu")
        ok(levelAfter > 0.95, "after: the panel shows in a level with quads nearer than it (submit order)")
        ok(levelPlainAfter > 0.95, "after: ...and with the quads depth-tested (every other title)")
        // The panel still writes its depth: the system reprojects the frame from it.
        ok(KleptonOverlayDepth.statsCard().isDepthWriteEnabled, "the panel still writes its depth")
        // The guest's quads are untouched: what lies beside the panel is the same before and after.
        var same = true
        for y in stride(from: 0, to: H, by: 7) { for x in stride(from: 0, to: W, by: 7) where !((rx.lowerBound - 8)..<(rx.upperBound + 8)).contains(x) || !((ry.lowerBound - 8)..<(ry.upperBound + 8)).contains(y) {
            if changed(levelBeforePx, levelAfterPx, x, y) { same = false }
        } }
        ok(same, "outside the panel the level frame is unchanged")

        png(menuBeforePx, shots + "/menu-before.png")
        png(levelBeforePx, shots + "/level-before.png")
        png(menuAfterPx, shots + "/menu-after.png")
        png(levelAfterPx, shots + "/level-after.png")
        print("  frames: \(shots)/{menu,level}-{before,after}.png")
        print(failed ? "\n=== t_statscard FAILED ===" : "\n=== t_statscard: the metrics panel is over every quad ===")
        exit(failed ? 1 : 0)
    }
}
