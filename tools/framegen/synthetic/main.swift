// A synthetic check of KLFrameGen (visionos/Sources/KleptonFrameGen.swift) on a foveated picture,
// without a headset or a capture: a red box with a white arrow – Beat Saber's block – moving across
// the coarse edge of a rasterization rate map like the headset's, over a textured dark background.
// The generated picture is compared with the true one (PSNR of the whole picture and of the box's
// neighbourhood) in the squeezed physical pixels and in the map's screen pixels (KL_FRAMEGEN_SCREEN),
// and a strip of older / newer / truth / both results is written as strip-fove.png or strip-flat.png.
//
//   tools/framegen/build.sh && build/framegen_synthetic [flat] [ahead] [noold] [<px a frame, 40>]
//
// flat: no foveation (a map of rate 1); ahead: the picture half a frame after the newer one instead
// of between the two; noold: between without the older picture's colours.
// What it found (2026-10-28): the 3×3 median zeroed the edges of anything a few blocks wide (the
// arrow came out bent, as on the headset), smoothing the coarse levels lost fast motion, and the warp
// read each vector at the output pixel instead of where it points. Between, 40 px: 29.6 → 37.3 dB
// flat, 31.0 → 38.7 dB foveated.
import Metal
import simd
import Foundation
import ImageIO
import UniformTypeIdentifiers
import CoreGraphics

let dev = MTLCreateSystemDefaultDevice()!
guard dev.supportsRasterizationRateMap(layerCount: 1) else { print("no rate maps"); exit(1) }
let W = 2048, H = 2048
let d = MTLRasterizationRateMapDescriptor()
d.screenSize = MTLSize(width: W, height: H, depth: 0)
// Sharp middle, coarse edge, like the headset's foveation.
let q: [Float] = CommandLine.arguments.contains("flat") ? [1, 1, 1] : [0.25, 0.3, 0.5, 1, 1, 1, 0.5, 0.3, 0.25]
let layer = MTLRasterizationRateLayerDescriptor(horizontal: q, vertical: q)
d.setLayer(layer, at: 0)
let rm = dev.makeRasterizationRateMap(descriptor: d)!
let ps = rm.physicalSize(layer: 0)
print("screen \(W)x\(H) physical \(ps.width)x\(ps.height)")
let sa = rm.parameterDataSizeAndAlign
let rmb = dev.makeBuffer(length: max(16, sa.size), options: .storageModeShared)!
rm.copyParameterData(buffer: rmb, offset: 0)

let src = """
#include <metal_stdlib>
using namespace metal;
float3 scene(float2 s, float bx) {
    float3 c = float3(0.06, 0.07, 0.12) * (1.0 + 0.6 * sin(s.x * 0.05) * cos(s.y * 0.043));
    float2 b = s - float2(bx, 1300.0);
    if (all(b >= 0.0) && all(b < 220.0)) {
        // An arrow on a red box: white chevron.
        float2 u = b / 220.0;
        bool arrow = abs(u.x - 0.5) < 0.12 + 0.0 * u.y && u.y > 0.2 && u.y < 0.8;
        bool head = u.y > 0.15 && u.y < 0.45 && abs(u.x - 0.5) < (0.45 - u.y) * 1.2;
        c = (arrow || head) ? float3(1) : float3(0.9, 0.12, 0.1);
    }
    return c;
}
kernel void draw(texture2d<float, access::write> out [[texture(0)]],
                 constant rasterization_rate_map_data *rmd [[buffer(0)]], constant float &bx [[buffer(1)]],
                 uint2 g [[thread_position_in_grid]]) {
    if (g.x >= out.get_width() || g.y >= out.get_height()) return;
    rasterization_rate_map_decoder map(*rmd);
    float2 s = map.map_physical_to_screen_coordinates(float2(g) + 0.5, 0);
    out.write(float4(scene(s, bx), 1), g);
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let drawP = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "draw")!)
func tex() -> MTLTexture {
    // A screen-size texture whose top-left physical part holds the picture (as Beat Saber's eye).
    let t = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
    t.usage = [.shaderRead, .shaderWrite]; t.storageMode = .shared
    return dev.makeTexture(descriptor: t)!
}
let queue = dev.makeCommandQueue()!
func draw(_ t: MTLTexture, _ bx: Float) {
    let cb = queue.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    var x = bx
    e.setComputePipelineState(drawP); e.setTexture(t, index: 0); e.setBuffer(rmb, offset: 0, index: 0)
    e.setBytes(&x, length: 4, index: 1)
    e.dispatchThreads(MTLSize(width: ps.width, height: ps.height, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
}
func bytes(_ t: MTLTexture) -> [UInt8] {
    var b = [UInt8](repeating: 0, count: W * H * 4)
    t.getBytes(&b, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0)
    return b
}
// PSNR over the physical part, and over the box's neighbourhood alone.
func psnr(_ a: [UInt8], _ b: [UInt8], box: Bool) -> Double {
    var se = 0.0, n = 0
    let pw = ps.width, ph = ps.height
    for y in 0..<ph { for x in 0..<pw {
        if box {
            let s = rm.screenCoordinates(physicalCoordinates: MTLCoordinate2D(x: Float(x) + 0.5, y: Float(y) + 0.5), layer: 0)
            if !(s.x > 1500 && s.x < 2000 && s.y > 1250 && s.y < 1580) { continue }
        }
        for c in 0..<3 { let e = Double(a[(y * W + x) * 4 + c]) - Double(b[(y * W + x) * 4 + c]); se += e * e; n += 1 }
    } }
    return 10 * log10(255.0 * 255.0 / max(se / Double(n), 1e-9))
}

let x0: Float = 1560, step: Float = Float(CommandLine.arguments.compactMap { Float($0) }.first ?? 40)
let older = tex(), newer = tex(), truth = tex()
let ahead = CommandLine.arguments.contains("ahead")
draw(older, x0); draw(newer, x0 + step); draw(truth, ahead ? x0 + step * 1.5 : x0 + step / 2)
let fg = try! KLFrameGen(device: dev)
let tb = bytes(truth)
var outs: [[UInt8]] = []
print(String(format: "repeat of the newest: all %.2f dB, box %.2f dB", psnr(bytes(newer), tb, box: false), psnr(bytes(newer), tb, box: true)))
for screen in [false, true] {
    let out = tex()
    let phys = SIMD4<Int32>(0, 0, Int32(min(W, ps.width + 2)), Int32(min(H, ps.height + 2)))
    let r = screen ? SIMD4<Int32>(0, 0, Int32(W), Int32(H)) : phys
    var best = 1e9
    for run in 0..<6 {
        let cb = queue.makeCommandBuffer()!
        let a = fg.pyramid(of: older, rect: r, key: "o", rateMap: screen ? rmb : nil, commandBuffer: cb)
        let b = fg.pyramid(of: newer, rect: r, key: "n", rateMap: screen ? rmb : nil, commandBuffer: cb)
        fg.generate(older: a, newer: b, newerColor: newer, phase: ahead ? 0.5 : -0.5, rotation: matrix_identity_float3x3,
                    rateMap: screen ? nil : rmb, rateLayer: 0, screenRateMap: screen ? rmb : nil, warpRect: phys, olderColor: CommandLine.arguments.contains("noold") ? nil : older,
                    into: out, commandBuffer: cb)
        cb.commit(); cb.waitUntilCompleted()
        if run > 0 { best = min(best, (cb.gpuEndTime - cb.gpuStartTime) * 1000) }
    }
    let ob = bytes(out)
    outs.append(ob)
    print(String(format: "%@: all %.2f dB, box %.2f dB, %.2f ms (two pyramids + one picture)",
                 screen ? "screen space  " : "physical space", psnr(ob, tb, box: false), psnr(ob, tb, box: true), best))
}

// A strip around the box, in screen space (resampled from physical), for a look: older, newer, truth, outputs.
func strip(_ pics: [[UInt8]], _ path: String) {
    let cw = 560, ch = 300, x0s = 1480, y0s = 1260
    var img = [UInt8](repeating: 255, count: cw * ch * pics.count * 4)
    for (k, b) in pics.enumerated() {
        for y in 0..<ch { for x in 0..<cw {
            let ph = rm.physicalCoordinates(screenCoordinates: MTLCoordinate2D(x: Float(x0s + x) + 0.5, y: Float(y0s + y) + 0.5), layer: 0)
            let px = min(Int(ph.x), W - 1), py = min(Int(ph.y), H - 1)
            for c in 0..<4 { img[((k * ch + y) * cw + x) * 4 + c] = b[(py * W + px) * 4 + c] }
        } }
    }
    let ctx = CGContext(data: &img, width: cw, height: ch * pics.count, bitsPerComponent: 8, bytesPerRow: cw * 4,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dst, ctx.makeImage()!, nil); CGImageDestinationFinalize(dst)
}
strip([bytes(older), bytes(newer), tb] + outs, CommandLine.arguments.contains("flat") ? "strip-flat.png" : "strip-fove.png")
