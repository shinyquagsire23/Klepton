// KleptonFrameGen: a picture between two game frames, made on the GPU
// (moonshot s3-framegen).
//
// A game that renders at half the display rate shows every frame twice. The
// compositor's timewarp already corrects the head's ROTATION for the repeat,
// but anything that moves in the picture – a saber, a block, the world as the
// head moves sideways – stands still for one display frame and jumps on the
// next: judder. This estimates how the picture moved between the two newest
// game frames and moves the newest one on by part of that, so the repeat is a
// new picture instead of the same one.
//
// It needs nothing from the game: no motion vectors, no depth – that is the
// difference from Meta's AppSpaceWarp, which most games here never feed. The
// motion is found by block matching on a luminance pyramid (8×8 blocks, a
// wide search at 1/8 size, refined down to full size), smoothed with a 3×3
// median, then the newest frame is sampled backwards along the vectors.
// Where something appears from behind something else there is no picture to
// take; those pixels take the newest frame unmoved, which is what a repeat
// shows anyway.
//
// Platform-neutral Metal (macOS and visionOS): the Mac lab measures it
// against real frames (tools/framegen), the compositor runs the same code.
import Metal
import simd

final class KLFrameGen {
    let device: MTLDevice
    private let luma, down, search, median, warp, turn, mvfield, mvwritten: MTLComputePipelineState
    private var cache: [String: MTLTexture] = [:]
    /// Pyramid levels below full size. 3 = the coarsest search runs at 1/8.
    static let levels = 3
    /// The finest level searched is the one about this wide: a Vision Pro eye picture can be
    /// 6880 px across, and a fixed half size made one generated frame take 100 ms there (8 fps on
    /// the headset, 2026-10-28). The coarsest is two levels above it.
    static var searchWidth = 320
    /// The finest level searched: 1 = half size. Full size costs four times as much for a
    /// field that ends up 8×8-block coarse anyway (measured: no better on Beat Saber's menu).
    static var finest = 1
    static var block = 8
    /// How much a block must gain over "no motion" before it moves (per pixel, luma 0…1), and
    /// whether the field is median-smoothed. Tunables for the lab (tools/framegen --tune).
    static var zeroBias: Float = 0.01
    static var smooth = true
    static var smoothCoarse = false
    /// Motion below this many pixels (after the turn is taken out) is left alone: the search's
    /// own error, not something moving. Without it a picture that only turned came out worse
    /// than the timewarped repeat (Beat Saber's menu, 2026-10-28).
    static var minMotion: Float = 1.5
    /// The same for a field from the game's own vectors (motionField). At 1.5 most of a slowly turning
    /// head's motion – a pixel or two a frame – was dropped (Assassin's Creed Nexus on the Mac, before
    /// the turn was taken out). With the turn out, what still stands is left with 0.3–0.6 px of the
    /// turn's own inexactness, and at 0.25 that sent most of the picture down the expensive path for an
    /// in-between moved by a sixth of a pixel: 0.75 leaves 96 % of Nexus's Animus still instead of 69 %,
    /// 1.05 instead of 1.57 ms an eye at 2880x2400 on an M4 (2026-10-29); on the headset's 5184x3286
    /// eyes the composite had taken 16 of a 10 ms display frame.
    static var gameMinMotion: Float = 0.75

    init(device: MTLDevice) throws {
        self.device = device
        let lib = try device.makeLibrary(source: KLFrameGen.source, options: nil)
        func pipe(_ name: String) throws -> MTLComputePipelineState {
            try device.makeComputePipelineState(function: lib.makeFunction(name: name)!)
        }
        luma = try pipe("fg_luma"); down = try pipe("fg_down"); search = try pipe("fg_search")
        median = try pipe("fg_median"); warp = try pipe("fg_warp"); turn = try pipe("fg_turn")
        mvfield = try pipe("fg_mvfield")
        mvwritten = try pipe("fg_mvwritten")
    }

    private func texture(_ key: String, _ w: Int, _ h: Int, _ format: MTLPixelFormat) -> MTLTexture {
        if let t = cache[key], t.width == w, t.height == h, t.pixelFormat == format { return t }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(1, w), height: max(1, h),
                                                         mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .private
        let t = device.makeTexture(descriptor: d)!
        cache[key] = t
        return t
    }

    private func dispatch(_ enc: MTLComputeCommandEncoder, _ p: MTLComputePipelineState, _ w: Int, _ h: Int) {
        enc.setComputePipelineState(p)
        let tg = MTLSize(width: 8, height: 8, depth: 1)
        enc.dispatchThreadgroups(MTLSize(width: (w + 7) / 8, height: (h + 7) / 8, depth: 1), threadsPerThreadgroup: tg)
    }

    /// One picture's luminance pyramid (levels `finest`…`levels`) for one eye's rectangle. The
    /// compositor makes one per new game frame and keeps the last two: a generated frame then costs
    /// only the search and the warp.
    final class Pyramid {
        let levels: [MTLTexture]
        let rect: SIMD4<Int32>
        /// The level of `levels[0]` (1 = half size) and of the last one.
        let finest: Int, top: Int
        init(levels: [MTLTexture], rect: SIMD4<Int32>, finest: Int, top: Int) {
            self.levels = levels; self.rect = rect; self.finest = finest; self.top = top
        }
    }

    /// The finest level for a picture `width` wide: `finest` (the lab's choice) for a picture up
    /// to `searchWidth` × 2^finest, more for a larger one.
    static func finestLevel(width: Int) -> Int {
        var f = finest
        while (width >> f) > searchWidth * 3 / 2 && f < 6 { f += 1 }
        return f
    }

    /// The pyramid of `source` (a 2D texture, or one slice viewed as 2D) inside `rect`, in storage
    /// named `key` – reused, so two keys ping-pong for "older" and "newer".
    ///
    /// With `rateMap` the source was drawn through that rasterization rate map and `rect` is in the
    /// map's SCREEN pixels: the pyramid is the picture as it looks, evenly spread, each cell read
    /// from where the map put it (KL_FRAMEGEN_SCREEN). A third of the cost of searching the squeezed
    /// pixels with the map, but a synthetic test found fast things at the edge of the view worse
    /// there, where the squeezed picture makes their motion small – so it is a lever, not the rule.
    func pyramid(of source: MTLTexture, rect: SIMD4<Int32>? = nil, key: String,
                 rateMap: MTLBuffer? = nil, rateLayer: Int = 0,
                 commandBuffer cb: MTLCommandBuffer) -> Pyramid {
        let r = rect ?? SIMD4<Int32>(0, 0, Int32(source.width), Int32(source.height))
        let w = Int(r.z), h = Int(r.w), f = Self.finestLevel(width: w), top = max(f + 2, Self.levels)
        let levels = (f...top).map { texture("\(key).\($0)", max(1, w >> $0), max(1, h >> $0), .r16Float) }
        if let enc = cb.makeComputeCommandEncoder() {
            var lp = SIMD4<Int32>(r.x, r.y, Int32(1 << f), 0)
            var lm = SIMD4<Int32>(rateMap == nil ? 0 : 1, Int32(rateLayer), 0, 0)
            enc.setTexture(source, index: 0); enc.setTexture(levels[0], index: 1)
            enc.setBytes(&lp, length: 16, index: 0)
            if let rateMap { enc.setBuffer(rateMap, offset: 0, index: 1) } else { enc.setBytes(&lm, length: 16, index: 1) }
            enc.setBytes(&lm, length: 16, index: 2)
            dispatch(enc, luma, levels[0].width, levels[0].height)
            for i in 1..<levels.count {
                enc.setTexture(levels[i - 1], index: 0); enc.setTexture(levels[i], index: 1)
                dispatch(enc, down, levels[i].width, levels[i].height)
            }
            enc.endEncoding()
        }
        return Pyramid(levels: levels, rect: r, finest: f, top: top)
    }

    /// The picture `phase` of a game frame after `newer`, from the motion `older` → `newer`, written
    /// into `out` at `newer`'s rectangle. 0.5 = the in-between frame at double rate; a negative phase
    /// goes back from `newer` (an interpolation when the two lie on both sides of the wanted moment).
    ///
    /// `rotation` maps a pixel of `newer` (homogeneous, pixels of its rectangle) to where the same
    /// direction lay in `older` if only the head had turned. That share of the motion is the
    /// compositor's timewarp's job – it moves `newer` to the display pose anyway – so only the rest
    /// (things moving, the parallax of a head moving sideways) is carried on. nil = all of it.
    ///
    /// `rateMap` (with `rateLayer`): the picture was drawn through a rasterization rate map, so its
    /// pixels are not evenly spread over the view – Beat Saber's eye is the top-left 2624×2560 of a
    /// 6888×5525 texture (headset, 2026-10-28). `rotation` is then in the map's SCREEN pixels, and the
    /// search converts with the map (physical → screen → turn → physical). Without it a turning head
    /// came out in patches. The buffer is `MTLRasterizationRateMap.copyParameterData`.
    ///
    /// `olderColor`: for a picture between the two (negative phase), `older`'s colours, in the same
    /// layout as `newerColor`. What the newer picture has already covered – the background behind the
    /// edge a thing moves away from – is only in the older one; without it a moving block came out
    /// stretched on that side.
    ///
    /// `screenRateMap` instead: the pyramids were made in the map's screen pixels (`pyramid`'s
    /// `rateMap`), so the search is the plain one and only the warp goes through the map – each
    /// pixel of `warpRect` (the physical part) to the screen, along the motion, and back.
    func generate(older: Pyramid, newer: Pyramid, newerColor: MTLTexture, phase: Float,
                  rotation: simd_float3x3? = nil, rateMap: MTLBuffer? = nil, rateLayer: Int = 0,
                  screenRateMap: MTLBuffer? = nil, warpRect: SIMD4<Int32>? = nil, olderColor: MTLTexture? = nil,
                  into out: MTLTexture, commandBuffer cb: MTLCommandBuffer) {
        guard let enc = cb.makeComputeCommandEncoder() else { return }
        guard older.finest == newer.finest, older.top == newer.top else { enc.endEncoding(); return }
        let f = newer.finest, top = newer.top
        var prior: MTLTexture? = nil
        for l in stride(from: top, through: f, by: -1) {
            let i = l - f
            let bw = (newer.levels[i].width + Self.block - 1) / Self.block
            let bh = (newer.levels[i].height + Self.block - 1) / Self.block
            let field = texture("v\(l)", bw, bh, .rg16Float)
            // ±3 at the top is ±3 · 2^top pixels of the picture: plenty once the turn is out, and a
            // wider one made the few threads there the slowest part (±6: 2 ms of 5 at 6880 px).
            var p = SIMD4<Int32>(Int32(Self.block), l == top ? 3 : 2, prior == nil ? 0 : 1, 0)
            var bias = Self.zeroBias
            // The head's turn, in full-size pixels (screen pixels with a rate map): OLDER is read
            // where the turn says the direction lay, so the search sees only what moved besides it.
            var turnFull = rotation ?? matrix_identity_float3x3
            var mp = SIMD4<Int32>(rateMap == nil ? 0 : 1, Int32(rateLayer), Int32(1 << l), rotation == nil ? 0 : 1)
            enc.setTexture(older.levels[i], index: 0); enc.setTexture(newer.levels[i], index: 1)
            enc.setTexture(prior ?? field, index: 2); enc.setTexture(field, index: 3)
            enc.setBytes(&p, length: 16, index: 0); enc.setBytes(&bias, length: 4, index: 1)
            enc.setBytes(&turnFull, length: MemoryLayout<simd_float3x3>.stride, index: 2)
            enc.setBytes(&mp, length: 16, index: 4)
            if let rateMap { enc.setBuffer(rateMap, offset: 0, index: 3) } else { enc.setBytes(&mp, length: 16, index: 3) }
            // One threadgroup per block, one thread per candidate offset: the candidates run side by
            // side instead of one after another in a single thread (4 ms → well under 1 at 2624 px).
            let side = 2 * Int(p.y) + 1
            enc.setComputePipelineState(search)
            enc.dispatchThreadgroups(MTLSize(width: bw, height: bh, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: side * side, height: 1, depth: 1))
            // Only the finest level is cleaned: on the coarser ones a thing near by is one or two blocks
            // (a box passing close, 80 px a game frame), and cleaning there took its motion away before
            // the finer levels could refine it.
            if Self.smooth, l == f || Self.smoothCoarse {
                let smoothed = texture("s\(l)", bw, bh, .rg16Float)
                enc.setTexture(field, index: 0); enc.setTexture(smoothed, index: 1)
                dispatch(enc, median, bw, bh)
                prior = smoothed
            } else {
                prior = field
            }
        }
        // The field is per block of the finest level searched, in that level's pixels.
        // The search already took the turn out (above): the field is only what moved besides it.
        warpPass(enc, field: prior!, cell: SIMD2(repeating: Float(Self.block << f)), scale: Float(1 << f), minMove: Self.minMotion, phase: phase,
                 rect: warpRect ?? newer.rect, rotation: rotation, screenRateMap: screenRateMap, rateLayer: rateLayer,
                 newerColor: newerColor, olderColor: olderColor, into: out)
        enc.endEncoding()
    }

    /// The picture `phase` of a game frame after the newest from a field the game gave (motionField)
    /// instead of one searched: no pyramid, no search, only the warp. `screenRateMap` as in `generate`
    /// – the game's vectors are in the picture's screen pixels.
    /// `shownColumns`: the columns of `warpRect` anyone will see (from, to); the others are not written.
    func generate(field: MTLTexture, cell: SIMD2<Float>, newerColor: MTLTexture, phase: Float, warpRect: SIMD4<Int32>,
                  shownColumns: SIMD2<Float>? = nil,
                  screenRateMap: MTLBuffer? = nil, rateLayer: Int = 0, olderColor: MTLTexture? = nil,
                  into out: MTLTexture, commandBuffer cb: MTLCommandBuffer) {
        guard let enc = cb.makeComputeCommandEncoder() else { return }
        warpPass(enc, field: field, cell: cell, scale: 1, minMove: Self.gameMinMotion, phase: phase, rect: warpRect, rotation: nil,
                 screenRateMap: screenRateMap, rateLayer: rateLayer, newerColor: newerColor, olderColor: olderColor, into: out,
                 shownColumns: shownColumns)
        enc.endEncoding()
    }

    /// The field from the game's own motion vectors (XR_FB_space_warp) rather than a search: one vector
    /// per motion-vector texel, in pixels of the picture, the head's turn taken out as the search does.
    /// `motion` (a 2D texture or one slice of one) holds in `mvRect` how what is at each texel moved since
    /// the previous frame, as the game states it; `toPixels` turns its x and y into pixels of the picture
    /// (new place minus old), `cell` is how many picture pixels one texel covers across and down – not
    /// always the same: Assassin's Creed Nexus has 960x771 of them for a 4800x3081 eye. `rotation` maps a
    /// picture pixel of the newer frame to where the same direction lay in the older one (rotationMap);
    /// nil when the game's vectors leave the head's turn out already.
    func motionField(from motion: MTLTexture, mvRect: SIMD4<Int32>, toPixels: SIMD2<Float>, cell: SIMD2<Float>,
                     rotation: simd_float3x3?, key: String, commandBuffer cb: MTLCommandBuffer) -> MTLTexture? {
        guard mvRect.z > 0, mvRect.w > 0, let enc = cb.makeComputeCommandEncoder() else { return nil }
        let field = texture("mv.\(key)", Int(mvRect.z), Int(mvRect.w), .rg16Float)
        var r = mvRect
        var k = SIMD4<Float>(toPixels.x, toPixels.y, cell.x, rotation == nil ? 0 : cell.y)
        var rot = rotation ?? matrix_identity_float3x3
        var minMove = Self.gameMinMotion
        enc.setTexture(motion, index: 0); enc.setTexture(field, index: 1)
        enc.setBytes(&r, length: 16, index: 0); enc.setBytes(&k, length: 16, index: 1)
        enc.setBytes(&rot, length: MemoryLayout<simd_float3x3>.stride, index: 2)
        enc.setBytes(&minMove, length: 4, index: 3)
        dispatch(enc, mvfield, Int(mvRect.z), Int(mvRect.w))
        enc.endEncoding()
        return field
    }

    /// Whether the game drew into `motion` at all: every 8th texel looked at, those not (0,0,0,0) counted
    /// into `count` (a shared 4-byte buffer the caller has zeroed). An OVRPlugin game that asks for the
    /// space-warp images but never turns Application SpaceWarp on leaves them untouched – PowerWash
    /// Simulator VR on the Mac, head turning – where one that draws clears them to alpha 1 (Assassin's
    /// Creed Nexus, LEGO Bricktales). Not for OpenXR's: HITMAN 3 writes its vectors with alpha 0.
    func countWritten(_ motion: MTLTexture, into count: MTLBuffer, commandBuffer cb: MTLCommandBuffer) {
        guard let enc = cb.makeComputeCommandEncoder() else { return }
        enc.setTexture(motion, index: 0); enc.setBuffer(count, offset: 0, index: 0)
        dispatch(enc, mvwritten, (motion.width + 7) / 8, (motion.height + 7) / 8)
        enc.endEncoding()
    }

    private func warpPass(_ enc: MTLComputeCommandEncoder, field: MTLTexture, cell: SIMD2<Float>, scale: Float, minMove: Float,
                          phase: Float, rect: SIMD4<Int32>, rotation: simd_float3x3?, screenRateMap: MTLBuffer?, rateLayer: Int,
                          newerColor: MTLTexture, olderColor: MTLTexture?, into out: MTLTexture,
                          shownColumns: SIMD2<Float>? = nil) {
        var wp = SIMD4<Float>(phase, cell.x, scale, 0)
        var cells = SIMD4<Float>(cell.x, cell.y, shownColumns?.x ?? 0, shownColumns?.y ?? 0)
        var minMove = minMove
        var rr = rect
        var rot = rotation ?? matrix_identity_float3x3
        // `olderColor` (between two pictures only): `older`'s own colours, laid out as `newerColor`.
        let useOld = olderColor != nil && phase < 0
        var wm = SIMD4<Int32>(screenRateMap == nil ? 0 : 1, Int32(rateLayer), useOld ? 1 : 0, 0)
        enc.setTexture(newerColor, index: 0); enc.setTexture(field, index: 1); enc.setTexture(out, index: 2)
        enc.setTexture(useOld ? olderColor : newerColor, index: 3)
        enc.setBytes(&wp, length: 16, index: 0); enc.setBytes(&rr, length: 16, index: 1)
        enc.setBytes(&rot, length: MemoryLayout<simd_float3x3>.stride, index: 2)
        enc.setBytes(&minMove, length: 4, index: 3)
        if let screenRateMap { enc.setBuffer(screenRateMap, offset: 0, index: 4) } else { enc.setBytes(&wm, length: 16, index: 4) }
        enc.setBytes(&wm, length: 16, index: 5)
        enc.setBytes(&cells, length: 16, index: 6)
        dispatch(enc, warp, Int(rr.z), Int(rr.w))
    }

    /// `source` as seen after the head turned: each output pixel takes the source at `rotation`
    /// applied to it (pixels of `rect`). The compositor's timewarp, for the lab to compare with.
    func turn(_ source: MTLTexture, rotation: simd_float3x3, rect: SIMD4<Int32>, into out: MTLTexture,
              commandBuffer cb: MTLCommandBuffer) {
        guard let enc = cb.makeComputeCommandEncoder() else { return }
        var rr = rect, rot = rotation
        enc.setTexture(source, index: 0); enc.setTexture(out, index: 1)
        enc.setBytes(&rr, length: 16, index: 0); enc.setBytes(&rot, length: MemoryLayout<simd_float3x3>.stride, index: 1)
        dispatch(enc, turn, Int(rect.z), Int(rect.w))
        enc.endEncoding()
    }

    /// The pixel map of a pure head turn for one eye: a pixel of the picture rendered with
    /// orientation `from` → where the same direction lies in the one rendered with `to`. Both with
    /// the same tangents (left, right, up, down; positive) on a `size` picture whose row 0 is the top.
    static func rotationMap(from: simd_quatf, to: simd_quatf, tangents t: SIMD4<Float>, size: SIMD2<Float>,
                            topDown: Bool = true) -> simd_float3x3 {
        let (l, r, u, d) = (t.x, t.y, t.z, t.w)
        // pixel = K · (x/-z, y/-z, 1) for a direction (x, y, z) in eye space (-z forward, +y up);
        // a picture GL drew has row 0 at the bottom.
        let K = simd_float3x3(rows: [SIMD3(size.x / (l + r), 0, size.x * l / (l + r)),
                                     topDown ? SIMD3(0, -size.y / (u + d), size.y * u / (u + d))
                                             : SIMD3(0, size.y / (u + d), size.y * d / (u + d)),
                                     SIMD3(0, 0, 1)])
        let S = simd_float3x3(diagonal: SIMD3(1, 1, -1))
        let rel = simd_float3x3(to.inverse * from)
        return K * S * rel * S * K.inverse
    }

    /// Both steps at once, for a pair seen once (the lab measurement).
    func extrapolate(older: MTLTexture, newer: MTLTexture, phase: Float, into out: MTLTexture,
                     commandBuffer cb: MTLCommandBuffer, rect: SIMD4<Int32>? = nil) {
        let a = pyramid(of: older, rect: rect, key: "older", commandBuffer: cb)
        let b = pyramid(of: newer, rect: rect, key: "newer", commandBuffer: cb)
        generate(older: a, newer: b, newerColor: newer, phase: phase, into: out, commandBuffer: cb)
    }

    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    kernel void fg_turn(texture2d<float, access::sample> src [[texture(0)]],
                        texture2d<float, access::write> out [[texture(1)]],
                        constant int4 &r [[buffer(0)]], constant float3x3 &rot [[buffer(1)]],
                        uint2 g [[thread_position_in_grid]]) {
        if (int(g.x) >= r.z || int(g.y) >= r.w) return;
        constexpr sampler s(coord::pixel, address::clamp_to_edge, filter::linear);
        float3 h = rot * float3(float2(g) + 0.5, 1.0);
        float2 pos = clamp(h.xy / h.z, float2(0.5), float2(r.zw) - 0.5) + float2(r.xy);
        out.write(src.sample(s, pos), uint2(int2(g) + r.xy));
    }

    // Luminance of a rectangle of the source at 1/s size (s = 1 << finest): the mean of each s×s cell.
    // lm.x != 0: the rectangle is in a rate map's screen pixels, each point read where the map put it.
    kernel void fg_luma(texture2d<float, access::read> src [[texture(0)]],
                        texture2d<half, access::write> dst [[texture(1)]],
                        constant int4 &p [[buffer(0)]],
                        constant rasterization_rate_map_data *rmd [[buffer(1)]],
                        constant int4 &lm [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {
        if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
        int s = p.z; float y = 0;
        uint2 m = uint2(src.get_width() - 1, src.get_height() - 1);
        // Up to 4×4 reads per cell; a larger cell is read at 4×4 points spread over it.
        int n = min(s, 4), step = s / n;
        for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
            int2 at = int2(g) * s + int2(i, j) * step + step / 2 + p.xy;
            if (lm.x != 0) {
                rasterization_rate_map_decoder map(*rmd);
                at = int2(map.map_screen_to_physical_coordinates(float2(at) + 0.5, uint(lm.y)));
            }
            float3 c = src.read(min(uint2(max(at, int2(0))), m)).rgb;
            y += dot(c, float3(0.2126, 0.7152, 0.0722));
        }
        dst.write(half4(half(y / float(n * n))), g);
    }

    kernel void fg_down(texture2d<half, access::read> src [[texture(0)]],
                        texture2d<half, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
        if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
        uint2 s = g * 2, m = uint2(src.get_width() - 1, src.get_height() - 1);
        half v = src.read(min(s, m)).r + src.read(min(s + uint2(1, 0), m)).r
               + src.read(min(s + uint2(0, 1), m)).r + src.read(min(s + uint2(1, 1), m)).r;
        dst.write(half4(v * 0.25h), g);
    }

    // One block of NEW: the offset d (pixels of this level) for which OLDER at p - d (after the head's
    // turn) looks most like NEW at p, i.e. the motion old -> new. p.x block size (8), p.y search radius
    // (≤ 3), p.z 1 = start from the coarser field. One threadgroup per block, one thread per offset; the
    // block's NEW pixels are read once into threadgroup memory. A small cost per pixel away from the
    // start keeps flat areas (sky, walls) from picking noise; "no motion" wins near-ties.
    kernel void fg_search(texture2d<half, access::sample> oldL [[texture(0)]],
                          texture2d<half, access::read> newL [[texture(1)]],
                          texture2d<half, access::read> prior [[texture(2)]],
                          texture2d<half, access::write> field [[texture(3)]],
                          constant int4 &p [[buffer(0)]], constant float &zeroBias [[buffer(1)]],
                          constant float3x3 &turn [[buffer(2)]],
                          constant rasterization_rate_map_data *rmd [[buffer(3)]],
                          constant int4 &mp [[buffer(4)]],
                          uint2 blk [[threadgroup_position_in_grid]], uint2 tid [[thread_position_in_threadgroup]],
                          uint2 ntg [[threads_per_threadgroup]]) {
        constexpr sampler ls(coord::pixel, address::clamp_to_edge, filter::linear);
        uint t = tid.x, n = ntg.x;
        threadgroup half nb[64];
        threadgroup float costs[64];
        threadgroup float2 starts[1];
        int B = min(p.x, 8), R = p.y, side = 2 * R + 1;
        int2 base = int2(blk) * B;
        int2 size = int2(newL.get_width(), newL.get_height());
        for (uint i = t; i < uint(B * B); i += n) {
            int2 q = clamp(base + int2(int(i) % B, int(i) / B), int2(0), size - 1);
            nb[i] = newL.read(uint2(q)).r;
        }
        if (t == 0) {
            float2 st = 0;
            if (p.z != 0) {
                uint2 pg = min(blk / 2, uint2(prior.get_width() - 1, prior.get_height() - 1));
                st = float2(prior.read(pg).rg) * 2.0;
            }
            starts[0] = round(st);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        int2 s0 = int2(starts[0]);
        int2 off = int2(int(t) % side - R, int(t) / side - R);
        int2 d = s0 + off;
        float sad = 0;
        float sc = float(mp.z);
        for (int y = 0; y < B; y++) {
            for (int x = 0; x < B; x++) {
                int2 q = clamp(base + int2(x, y), int2(0), size - 1);
                float2 at = (float2(q) + 0.5) * sc;                 // full-size (physical) pixels
                if (mp.w != 0) {
                    if (mp.x != 0) {
                        rasterization_rate_map_decoder map(*rmd);
                        float2 scr = map.map_physical_to_screen_coordinates(at, uint(mp.y));
                        float3 h = turn * float3(scr, 1.0);
                        at = map.map_screen_to_physical_coordinates(h.xy / h.z, uint(mp.y));
                    } else {
                        float3 h = turn * float3(at, 1.0);
                        at = h.xy / h.z;
                    }
                }
                float2 o = at / sc - float2(d);
                sad += abs(float(nb[y * B + x]) - float(oldL.sample(ls, o).r));
            }
        }
        bool isZero = all(d == int2(0));
        float cost = sad + 0.02 * float(B * B) * length(float2(off)) - (isZero ? zeroBias * float(B * B) : 0.0);
        if (int(t) < side * side) costs[t] = cost;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (t == 0) {
            float best = costs[0]; int bi = 0;
            for (int i = 1; i < side * side; i++) if (costs[i] < best) { best = costs[i]; bi = i; }
            int2 bd = s0 + int2(bi % side - R, bi / side - R);
            if (field.get_width() > blk.x && field.get_height() > blk.y)
                field.write(half4(half2(bd), 0, 0), blk);
        }
    }

    // The field cleaned of lone blocks: a block keeps its vector when two of its eight neighbours agree
    // with it (within a pixel of this level), else it takes the 3×3 median. A plain median made the
    // edge blocks of anything a few blocks wide take the still background's zero – the middle moved,
    // the outline stayed, and an arrow on a block came out bent (Beat Saber, worst at the edge of a
    // foveated eye, where a block holds the most; headset and a synthetic test, 2026-10-28).
    kernel void fg_median(texture2d<half, access::read> src [[texture(0)]],
                          texture2d<half, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
        if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
        int2 m = int2(src.get_width() - 1, src.get_height() - 1);
        float xs[9], ys[9]; int n = 0, agree = 0;
        float2 c = float2(src.read(g).rg);
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            float2 v = float2(src.read(uint2(clamp(int2(g) + int2(dx, dy), int2(0), m))).rg);
            xs[n] = v.x; ys[n] = v.y; n++;
            if ((dx != 0 || dy != 0) && all(abs(v - c) <= 1.0)) agree++;
        }
        if (agree >= 2) { dst.write(half4(half2(c), 0, 0), g); return; }
        for (int i = 0; i < 9; i++) for (int j = i + 1; j < 9; j++) {
            if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
        dst.write(half4(half(xs[4]), half(ys[4]), 0, 0), g);
    }

    // The field from the game's motion vectors (motionField): per texel of mv's rect, its x and y in
    // picture pixels (k.xy), less what the head's turn explains (k.w != 0; rot maps a pixel of the newer
    // picture to the older one), at the texel's centre, a texel k.z picture pixels wide and k.w high.
    kernel void fg_mvwritten(texture2d<float, access::read> mv [[texture(0)]], device atomic_uint *n [[buffer(0)]],
                             uint2 g [[thread_position_in_grid]]) {
        uint2 p = g * 8;
        if (p.x >= mv.get_width() || p.y >= mv.get_height()) return;
        if (any(mv.read(p) != 0)) atomic_fetch_add_explicit(n, 1, memory_order_relaxed);
    }

    kernel void fg_mvfield(texture2d<float, access::read> mv [[texture(0)]],
                           texture2d<half, access::write> field [[texture(1)]],
                           constant int4 &r [[buffer(0)]], constant float4 &k [[buffer(1)]],
                           constant float3x3 &rot [[buffer(2)]], constant float &minMove [[buffer(3)]],
                           uint2 g [[thread_position_in_grid]]) {
        if (int(g.x) >= r.z || int(g.y) >= r.w) return;
        float2 raw = mv.read(uint2(int2(g) + r.xy)).xy;
        // Exactly zero: nothing was drawn into the vectors here – Assassin's Creed Nexus's sky and fog, a
        // third of its picture. That moves with the head alone, which the timewarp does; taking the turn
        // out of a zero pushed the sky back by half a turn in every in-between (headset, 2026-10-29).
        if (all(raw == 0)) { field.write(half4(0), g); return; }
        float2 v = raw * k.xy;
        if (k.w != 0) {
            float2 P = (float2(g) + 0.5) * k.zw;
            float3 h = rot * float3(P, 1.0);
            v -= P - h.xy / h.z;
        }
        if (!all(isfinite(v)) || length(v) < minMove) v = 0;
        field.write(half4(half2(v), 0, 0), g);
    }

    // The field at a position (full-size pixels; screen pixels with a map), read bilinearly between
    // block centres, in full-size pixels.
    static float2 fg_field_at(texture2d<half, access::read> field, float2 pos, float2 B, float scale) {
        float2 fp = pos / B - 0.5;
        int2 m = int2(field.get_width() - 1, field.get_height() - 1);
        int2 f0 = clamp(int2(floor(fp)), int2(0), m), f1 = clamp(f0 + 1, int2(0), m);
        float2 t = clamp(fp - floor(fp), 0.0, 1.0);
        float2 v00 = float2(field.read(uint2(f0)).rg), v10 = float2(field.read(uint2(f1.x, f0.y)).rg);
        float2 v01 = float2(field.read(uint2(f0.x, f1.y)).rg), v11 = float2(field.read(uint2(f1)).rg);
        return mix(mix(v00, v10, t.x), mix(v01, v11, t.x), t.y) * scale;
    }

    // A position of the field's pixels (screen pixels with a map, else pixels of the rectangle) in the
    // texture, inside this eye's rectangle: across the seam is the other eye.
    static float2 fg_tex_pos(float2 at, int4 r, int4 wm, constant rasterization_rate_map_data *rmd) {
        if (wm.x != 0) {
            rasterization_rate_map_decoder map(*rmd);
            at = map.map_screen_to_physical_coordinates(at, uint(wm.y));
        } else {
            at += float2(r.xy);
        }
        return clamp(at, float2(r.xy) + 0.5, float2(r.xy + r.zw) - 0.5);
    }

    // NEW moved on by phase × its motion: each output pixel takes NEW from where its content was.
    // The field is per block of NEW, at NEW's positions – not at the output's: the output pixel p shows
    // what lay at q = p - phase·v(q) in NEW. Each of the four blocks around p offers its vector; the
    // one that still holds where it points to (v(q) ≈ itself) is taken, and of several that hold, the
    // largest – the thing in front, which covers the still background it moves over. Read at p alone,
    // a moving block's outline stayed behind on one side and was stretched on the other.
    //
    // Between two pictures with OLD's colours (wm.z) the test is finer than a block: a vector is right
    // where NEW at p - phase·v and OLD at p - (1 + phase)·v show the same, and the pixel is the mix of
    // the two. Where no vector makes them agree, one of the two pictures has covered what belongs
    // here: NEW, when the slowest vector around (the background's) does not hold in NEW's field – then
    // OLD shows it, moved by that vector; else NEW does.
    kernel void fg_warp(texture2d<float, access::sample> newT [[texture(0)]],
                        texture2d<half, access::read> field [[texture(1)]],
                        texture2d<float, access::write> out [[texture(2)]],
                        texture2d<float, access::sample> oldT [[texture(3)]],
                        constant float4 &p [[buffer(0)]], constant int4 &r [[buffer(1)]],
                        constant float3x3 &rot [[buffer(2)]], constant float &minMove [[buffer(3)]],
                        constant rasterization_rate_map_data *rmd [[buffer(4)]],
                        constant int4 &wm [[buffer(5)]],
                        constant float4 &cells [[buffer(6)]],
                        uint2 g [[thread_position_in_grid]]) {
        if (int(g.x) >= r.z || int(g.y) >= r.w) return;
        // cells.zw: the columns of the rectangle that are shown (zw = 0: all); the rest is left unwritten.
        if (cells.w > 0 && (float(g.x) < cells.z || float(g.x) >= cells.w)) return;
        float2 B = cells.xy;
        // wm.x != 0: this pixel is a physical one of a rate map whose screen the field was found in.
        float2 here = float2(g) + 0.5;
        float2 scr = here;
        if (wm.x != 0) {
            rasterization_rate_map_decoder map(*rmd);
            scr = map.map_physical_to_screen_coordinates(here + float2(r.xy), uint(wm.y));
        }
        float2 fp = scr / B - 0.5;
        int2 m = int2(field.get_width() - 1, field.get_height() - 1);
        int2 f0 = clamp(int2(floor(fp)), int2(0), m), f1 = clamp(f0 + 1, int2(0), m);
        float2 c[4] = { float2(field.read(uint2(f0)).rg), float2(field.read(uint2(f1.x, f0.y)).rg),
                        float2(field.read(uint2(f0.x, f1.y)).rg), float2(field.read(uint2(f1)).rg) };
        uint2 at = uint2(int2(g) + r.xy);
        // Most of a picture does not move: copy those pixels as they are.
        if (all(c[0] == 0) && all(c[1] == 0) && all(c[2] == 0) && all(c[3] == 0)) {
            out.write(newT.read(at), at);
            return;
        }
        float phase = p.x, scale = p.z;
        constexpr sampler s(coord::pixel, address::clamp_to_edge, filter::linear);
        // A vector holds where it points to when the field there is within a pixel and a half of the
        // level searched (its own resolution) of it.
        float tol = 1.5 * scale;
        float2 slow = c[0];
        for (int k = 1; k < 4; k++) if (length(c[k]) < length(slow)) slow = c[k];
        slow *= scale;
        if (length(slow) < minMove) slow = 0;

        if (wm.z != 0) {
            float bestScore = 1e9; float4 bestN = 0, bestO = 0;
            // Only the blocks' own vectors: one blended between them (half a block's motion) found false
            // agreement on a plain surface – red at both ends – and left a strip of it behind.
            for (int k = 0; k < 4; k++) {
                float2 cand = c[k] * scale;
                if (length(cand) < minMove) cand = 0;
                float4 n = newT.sample(s, fg_tex_pos(scr - phase * cand, r, wm, rmd));
                float4 o = oldT.sample(s, fg_tex_pos(scr - (1.0 + phase) * cand, r, wm, rmd));
                float held = length(fg_field_at(field, scr - phase * cand, B, scale) - cand) / scale;
                float score = dot(abs(n.rgb - o.rgb), float3(1)) + 0.01 * min(held, 4.0);
                if (score < bestScore) { bestScore = score; bestN = n; bestO = o; }
            }
            float4 result;
            if (bestScore < 0.15) {
                result = mix(bestO, bestN, 1.0 + phase);
            } else if (length(fg_field_at(field, scr - phase * slow, B, scale) - slow) <= tol) {
                result = newT.sample(s, fg_tex_pos(scr - phase * slow, r, wm, rmd));
            } else {
                result = oldT.sample(s, fg_tex_pos(scr - (1.0 + phase) * slow, r, wm, rmd));
            }
            out.write(result, at);
            return;
        }

        float2 v = fg_field_at(field, scr, B, scale);
        float bestErr = 1e9, bestLen = -1;
        for (int k = 0; k < 5; k++) {
            float2 cand = k < 4 ? c[k] * scale : fg_field_at(field, scr, B, scale);
            float2 there = fg_field_at(field, scr - phase * cand, B, scale);
            float err = length(there - cand), len = length(cand);
            bool holds = err <= tol;
            bool better = holds ? (bestErr > tol || len > bestLen + 0.5) : (bestErr > tol && err < bestErr);
            if (better) { bestErr = err; bestLen = len; v = holds ? there : cand; }
        }
        if (length(v) < minMove) v = 0;
        if (wm.x == 0 && p.w != 0) {
            // Take out what the head's turn explains: that part the timewarp does.
            float3 h = rot * float3(here, 1.0);
            v -= here - h.xy / h.z;
        }
        out.write(newT.sample(s, fg_tex_pos(scr - phase * v, r, wm, rmd)), at);
    }
    """
}
