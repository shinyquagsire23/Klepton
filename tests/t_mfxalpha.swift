// MetalFX in mixed reality (KleptonMetalFXAlpha.swift): Apple's spatial scaler
// on the Mac's Metal device, with the scaler set up as mfxMakeTarget does it,
// then the alpha pass. For every format the compositor upscales, a transparent
// half (0,0,0,0) must come out transparent with no colour — what the eye pass
// discards so the room shows (kl_reproject.c) — and an opaque half opaque with
// its colour, with one edge between them where the linear path has it. Two
// slices of one array (the eye provider's), each the mirror of the other, and
// a content smaller than its texture (a foveated GL eye) whose junk beyond the
// content must not bleed into the alpha. The scaler's own alpha is printed
// too: the reason this pass exists.
//
// The scaler's COLOUR does read past a content smaller than its texture: the
// last row and column of the copy carry a trace of what lies beyond it (here
// white junk, 0.12 of it), with or without this pass and with or without mixed
// reality. That edge is checked for alpha only and its colour printed.
import Foundation
import Metal
import MetalFX

@main
struct MetalFXAlphaTests {
    static var failed = 0

    static func check(_ ok: Bool, _ what: String) {
        if !ok { failed += 1; print("  FAIL \(what)") }
    }

    /// One pixel as stored, from linear-free code values (what the guest wrote).
    static func bytes(_ px: SIMD4<Float>, _ f: MTLPixelFormat) -> [UInt8] {
        switch f {
        case .rgba16Float:
            return [px.x, px.y, px.z, px.w].flatMap { v -> [UInt8] in
                let h = Float16(v).bitPattern
                return [UInt8(h & 0xff), UInt8(h >> 8)]
            }
        case .bgra8Unorm, .bgra8Unorm_srgb:
            return [px.z, px.y, px.x, px.w].map { UInt8(($0 * 255).rounded()) }
        default:
            return [px.x, px.y, px.z, px.w].map { UInt8(($0 * 255).rounded()) }
        }
    }

    static func pixel(_ b: UnsafeRawPointer, _ f: MTLPixelFormat) -> SIMD4<Float> {
        switch f {
        case .rgba16Float:
            let h = b.assumingMemoryBound(to: UInt16.self)
            return SIMD4((0..<4).map { Float(Float16(bitPattern: h[$0])) })
        case .bgra8Unorm, .bgra8Unorm_srgb:
            let u = b.assumingMemoryBound(to: UInt8.self)
            return SIMD4(Float(u[2]), Float(u[1]), Float(u[0]), Float(u[3])) / 255
        default:
            let u = b.assumingMemoryBound(to: UInt8.self)
            return SIMD4(Float(u[0]), Float(u[1]), Float(u[2]), Float(u[3])) / 255
        }
    }

    static let opaque = SIMD4<Float>(0.8, 0.5, 0.2, 1)
    static let clear = SIMD4<Float>(0, 0, 0, 0)

    struct Case {
        let name: String
        let tex: (w: Int, h: Int)
        let content: (w: Int, h: Int)
        let out: (w: Int, h: Int)
    }

    static func run(_ device: MTLDevice, _ queue: MTLCommandQueue, _ alpha: KleptonMetalFXAlpha,
                    format f: MTLPixelFormat, _ c: Case) {
        let label = "\(f.rawValue) \(c.name)"
        let failedBefore = failed
        // The scaler as mfxMakeTarget makes it.
        let d = MTLFXSpatialScalerDescriptor()
        d.colorTextureFormat = f; d.outputTextureFormat = f
        d.inputWidth = c.tex.w; d.inputHeight = c.tex.h
        d.outputWidth = c.out.w; d.outputHeight = c.out.h
        d.colorProcessingMode = f == .rgba16Float ? .hdr : .perceptual
        guard let sc = d.makeSpatialScaler(device: device) else {
            check(false, "\(label): no scaler"); return
        }
        let sd = MTLTextureDescriptor()
        sd.textureType = .type2DArray; sd.pixelFormat = f
        sd.width = c.tex.w; sd.height = c.tex.h; sd.arrayLength = 2
        sd.usage = sc.colorTextureUsage.union(.shaderRead)
        sd.storageMode = .shared
        let src = device.makeTexture(descriptor: sd)!
        // Slice 0: opaque left of the split, transparent right of it; slice 1
        // the mirror. Beyond the content: opaque junk.
        let split = c.content.w / 2
        let bpp = f == .rgba16Float ? 8 : 4
        for s in 0..<2 {
            var data = [UInt8](); data.reserveCapacity(c.tex.w * c.tex.h * bpp)
            for y in 0..<c.tex.h {
                for x in 0..<c.tex.w {
                    let inside = x < c.content.w && y < c.content.h
                    let left = x < split
                    let px = !inside ? SIMD4<Float>(1, 1, 1, 1) : (left == (s == 0) ? opaque : clear)
                    data += bytes(px, f)
                }
            }
            src.replace(region: MTLRegionMake2D(0, 0, c.tex.w, c.tex.h), mipmapLevel: 0, slice: s,
                        withBytes: data, bytesPerRow: c.tex.w * bpp, bytesPerImage: data.count)
        }
        // The copy as mfxMakeTarget makes it.
        let td = MTLTextureDescriptor()
        td.textureType = .type2DArray; td.pixelFormat = f
        td.width = c.out.w; td.height = c.out.h; td.arrayLength = 2
        td.usage = sc.outputTextureUsage.union([.shaderRead, .renderTarget])
        td.storageMode = .private
        let dst = device.makeTexture(descriptor: td)!
        let ins = (0..<2).map { src.makeTextureView(pixelFormat: f, textureType: .type2D, levels: 0..<1, slices: $0..<$0 + 1)! }
        let outs = (0..<2).map { dst.makeTextureView(pixelFormat: f, textureType: .type2D, levels: 0..<1, slices: $0..<$0 + 1)! }

        func upscale(restore: Bool) -> [[SIMD4<Float>]] {
            let cmd = queue.makeCommandBuffer()!
            for s in 0..<2 {
                sc.colorTexture = ins[s]
                sc.inputContentWidth = c.content.w; sc.inputContentHeight = c.content.h
                sc.outputTexture = outs[s]
                sc.encode(commandBuffer: cmd)
                if restore {
                    check(alpha.encode(on: cmd, from: ins[s], content: c.content, into: outs[s]),
                          "\(label): alpha pass not encoded")
                }
            }
            let row = c.out.w * bpp, image = row * c.out.h
            let buf = device.makeBuffer(length: image * 2, options: .storageModeShared)!
            let blit = cmd.makeBlitCommandEncoder()!
            for s in 0..<2 {
                blit.copy(from: dst, sourceSlice: s, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                          sourceSize: MTLSize(width: c.out.w, height: c.out.h, depth: 1),
                          to: buf, destinationOffset: s * image, destinationBytesPerRow: row,
                          destinationBytesPerImage: image)
            }
            blit.endEncoding()
            cmd.commit(); cmd.waitUntilCompleted()
            if let e = cmd.error { check(false, "\(label): GPU error \(e)") }
            return (0..<2).map { s in
                (0..<c.out.w * c.out.h).map { pixel(buf.contents() + s * image + $0 * bpp, f) }
            }
        }

        // The scaler alone: what the room was covered with.
        let raw = upscale(restore: false)
        let xEdge = Double(split) * Double(c.out.w) / Double(c.content.w)
        let farClear = Int(xEdge) + 6, farOpaque = Int(xEdge) - 6
        let my = c.out.h / 2
        let rawA = raw[0][my * c.out.w + min(c.out.w - 1, farClear + 4)].w

        let px = upscale(restore: true)
        let tol: Float = f == .rgba16Float ? 0.002 : 1.0 / 255
        var worstClear: Float = 0, worstOpaque: Float = 1, worstColour: Float = 0
        var edgeOff = 0.0, monotonic = true, halo: Float = 0, rim: Float = 0
        let partial = c.content.w < c.tex.w || c.content.h < c.tex.h
        for s in 0..<2 {
            for y in 0..<c.out.h {
                var row = [Float](); row.reserveCapacity(c.out.w)
                for x in 0..<c.out.w {
                    let p = px[s][y * c.out.w + x]
                    if partial, x == c.out.w - 1 || y == c.out.h - 1 {
                        rim = max(rim, p.x, p.y, p.z)
                        if s == 0 ? x >= farClear : x <= farOpaque { worstClear = max(worstClear, p.w) }
                        row.append(s == 0 ? p.w : 1 - p.w)
                        continue
                    }
                    // More colour than coverage: light added over the room.
                    halo = max(halo, max(p.x, p.y, p.z) - p.w)
                    // In the slice's own sense: x as if slice 0 (mirror slice 1).
                    let clearSide = s == 0 ? x >= farClear : x <= farOpaque
                    let opaqueSide = s == 0 ? x <= farOpaque : x >= farClear
                    if clearSide {
                        worstClear = max(worstClear, p.w, p.x, p.y, p.z)
                    } else if opaqueSide {
                        worstOpaque = min(worstOpaque, p.w)
                        worstColour = max(worstColour, abs(p.x - opaque.x), abs(p.y - opaque.y), abs(p.z - opaque.z))
                    }
                    row.append(s == 0 ? p.w : 1 - p.w)   // falling from opaque to clear
                }
                for x in 1..<row.count where row[x] > row[x - 1] + tol { monotonic = false }
                // Where the alpha crosses one half, against where the linear path has it.
                if let x = row.firstIndex(where: { $0 < 0.5 }) {
                    edgeOff = max(edgeOff, abs(Double(x) - xEdge))
                }
            }
        }
        check(worstClear <= tol, "\(label): transparent half not transparent (max \(worstClear))")
        check(worstOpaque >= 1 - tol, "\(label): opaque half not opaque (min alpha \(worstOpaque))")
        check(worstColour <= 0.05, "\(label): opaque colour off by \(worstColour)")
        check(monotonic, "\(label): alpha not one clean edge (rises again across a row)")
        // Bilinear over one source texel is one scale factor of copy pixels wide.
        let scale = Double(c.out.w) / Double(c.content.w)
        check(edgeOff <= scale + 1, "\(label): edge \(edgeOff) px from where the linear path has it")
        check(halo <= 0.1, "\(label): colour past its alpha at the edge (\(halo))")
        print("  \(failed == failedBefore ? "ok  " : "FAIL") \(label): scaler alone alpha \(rawA) on the transparent side; "
              + "with the pass: transparent max \(worstClear), opaque min \(worstOpaque), "
              + "colour ±\(String(format: "%.3f", worstColour)), edge ±\(String(format: "%.1f", edgeOff)) px, "
              + "halo \(String(format: "%.3f", halo))" + (partial ? ", rim colour \(String(format: "%.2f", rim))" : ""))
    }

    static func main() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            print("mfxalpha: FAIL (no Metal device)"); exit(1)
        }
        guard MTLFXSpatialScalerDescriptor.supportsDevice(device) else {
            print("mfxalpha: SKIP (no MetalFX spatial scaler on \(device.name))"); exit(0)
        }
        print("=== MetalFX alpha in mixed reality (\(device.name)) ===")
        let alpha = KleptonMetalFXAlpha(device: device)
        alpha.onError = { print("  alpha: \($0)") }
        let cases = [
            Case(name: "whole 128x96 -> 192x144", tex: (128, 96), content: (128, 96), out: (192, 144)),
            Case(name: "content 100x80 of 128x96 -> 160x128", tex: (128, 96), content: (100, 80), out: (160, 128)),
        ]
        // mfxFormats in KleptonCompositor.swift, all of them.
        for f: MTLPixelFormat in [.rgba8Unorm, .rgba8Unorm_srgb, .bgra8Unorm, .bgra8Unorm_srgb, .rgba16Float] {
            for c in cases { run(device, queue, alpha, format: f, c) }
        }
        if failed > 0 { print("mfxalpha: FAIL (\(failed))"); exit(1) }
        print("mfxalpha: PASS")
    }
}
