// framegen_mv: the generator fed by a game's own motion vectors (XR_FB_space_warp) against the one
// that searches, on three consecutive frames of a Mac run (moonshot s3-framegen).
//
//   tools/framegen/build.sh && build/framegen_mv <vk dir> <sw dir> <out dir> [--layer N] [--eye N]
//
// The frames are what a run with KL_VK_OUT, KL_VK_OUT_RUN=2 (or more) and KL_XR_SPACEWARP_OUT writes:
// xr_f<n>_layer<l>_eye<e>.png and sw_f<n>_layer<l>_eye<e>_mv_<w>x<h>.rgba16f. For every pair n, n+1,
// n is made back from n+1 – a whole game frame, the most a repeat ever has to cover – along the motion
// n → n+1, three ways, and each is compared with the real n:
//   repeat   n+1 as it is
//   search   n+1 moved back by the motion the search finds between n and n+1
//   game     n+1 moved back by the game's own vectors of n+1 (the motion n → n+1)
// Back and not ahead: a Mac run stops at every capture, so the head moves unevenly from frame to frame
// and a frame ahead cannot be told from the two before it – but the motion of one pair is that pair's.
// Nothing is taken out for the head: on the Mac there is no timewarp after it, so all of the motion is
// the generator's to carry. PSNR over the picture and over the pixels that changed between the two.
import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers
import simd

let args = CommandLine.arguments
guard args.count >= 4 else {
    FileHandle.standardError.write("usage: framegen_mv <vk dir> <sw dir> <out dir> [--layer N] [--eye N]\n".data(using: .utf8)!)
    exit(2)
}
let vkDir = URL(fileURLWithPath: args[1]), swDir = URL(fileURLWithPath: args[2]), outDir = URL(fileURLWithPath: args[3])
let layer = args.firstIndex(of: "--layer").flatMap { Int(args[$0 + 1]) } ?? 0
let eye = args.firstIndex(of: "--eye").flatMap { Int(args[$0 + 1]) } ?? 0
let scale = SIMD2<Float>(0.5, -0.5)
try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let gen = try KLFrameGen(device: device)

func load(_ url: URL) -> (MTLTexture, [UInt8])? {
    guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
    let w = img.width, h = img.height
    var px = [UInt8](repeating: 0, count: w * h * 4)
    let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
    d.usage = [.shaderRead, .shaderWrite]
    let t = device.makeTexture(descriptor: d)!
    t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: px, bytesPerRow: w * 4)
    return (t, px)
}

func loadMotion(_ url: URL, _ w: Int, _ h: Int) -> MTLTexture? {
    guard let data = try? Data(contentsOf: url), data.count == w * h * 8 else { return nil }
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: w, height: h, mipmapped: false)
    d.usage = [.shaderRead]
    let t = device.makeTexture(descriptor: d)!
    data.withUnsafeBytes { t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w * 8) }
    return t
}

func readBack(_ t: MTLTexture) -> [UInt8] {
    var px = [UInt8](repeating: 0, count: t.width * t.height * 4)
    t.getBytes(&px, bytesPerRow: t.width * 4, from: MTLRegionMake2D(0, 0, t.width, t.height), mipmapLevel: 0)
    return px
}

func makeOut(_ like: MTLTexture) -> MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: like.width, height: like.height,
                                                     mipmapped: false)
    d.usage = [.shaderRead, .shaderWrite]
    return device.makeTexture(descriptor: d)!
}

func psnr(_ a: [UInt8], _ b: [UInt8], mask: [Bool]? = nil) -> Double {
    var se = 0.0, n = 0
    for i in 0..<(a.count / 4) {
        if let mask, !mask[i] { continue }
        let la = 0.299 * Double(a[i * 4]) + 0.587 * Double(a[i * 4 + 1]) + 0.114 * Double(a[i * 4 + 2])
        let lb = 0.299 * Double(b[i * 4]) + 0.587 * Double(b[i * 4 + 1]) + 0.114 * Double(b[i * 4 + 2])
        se += (la - lb) * (la - lb); n += 1
    }
    guard n > 0, se > 0 else { return 99 }
    return 10 * log10(255 * 255 / (se / Double(n)))
}

func frameName(_ n: Int) -> String { String(format: "xr_f%05d_layer%d_eye%d.png", n, layer, eye) }

let files = try FileManager.default.contentsOfDirectory(atPath: vkDir.path)
let frames = Set(files.compactMap { name -> Int? in
    guard name.hasSuffix("_layer\(layer)_eye\(eye).png"), name.hasPrefix("xr_f") else { return nil }
    return Int(name.dropFirst(4).prefix(5))
})
let swFiles = try FileManager.default.contentsOfDirectory(atPath: swDir.path)
var csv = "frame,repeat_all,search_all,game_all,repeat_moved,search_moved,game_moved,moved_pct\n"
var sums = [Double](repeating: 0, count: 6), count = 0
for n in frames.sorted() where frames.contains(n + 1) {
    let prefix = String(format: "sw_f%05d_layer%d_eye%d_mv_", n + 1, layer, eye)
    guard let mvName = swFiles.first(where: { $0.hasPrefix(prefix) }),
          let a = load(vkDir.appendingPathComponent(frameName(n))),
          let b = load(vkDir.appendingPathComponent(frameName(n + 1))) else { continue }
    let c = a   // what is to be made: the older frame
    let dims = mvName.dropFirst(prefix.count).dropLast(".rgba16f".count).split(separator: "x").compactMap { Int($0) }
    guard dims.count == 2, let mv = loadMotion(swDir.appendingPathComponent(mvName), dims[0], dims[1]) else { continue }
    let W = b.0.width, H = b.0.height
    let rect = SIMD4<Int32>(0, 0, Int32(W), Int32(H))
    let outS = makeOut(b.0), outG = makeOut(b.0)
    let cb = queue.makeCommandBuffer()!
    let pa = gen.pyramid(of: a.0, rect: rect, key: "a", commandBuffer: cb)
    let pb = gen.pyramid(of: b.0, rect: rect, key: "b", commandBuffer: cb)
    gen.generate(older: pa, newer: pb, newerColor: b.0, phase: -1, into: outS, commandBuffer: cb)
    let cell = SIMD2(Float(W) / Float(dims[0]), Float(H) / Float(dims[1]))
    if let field = gen.motionField(from: mv, mvRect: SIMD4(0, 0, Int32(dims[0]), Int32(dims[1])),
                                   toPixels: scale * SIMD2(Float(W), Float(H)), cell: cell, rotation: nil,
                                   key: "lab", commandBuffer: cb) {
        gen.generate(field: field, cell: cell, newerColor: b.0, phase: -1, warpRect: rect, into: outG, commandBuffer: cb)
    }
    cb.commit(); cb.waitUntilCompleted()
    let s = readBack(outS), g = readBack(outG)
    var moved = [Bool](repeating: false, count: W * H)
    var nm = 0
    for i in 0..<(W * H) {
        let d = abs(Int(b.1[i * 4]) - Int(c.1[i * 4])) + abs(Int(b.1[i * 4 + 1]) - Int(c.1[i * 4 + 1]))
            + abs(Int(b.1[i * 4 + 2]) - Int(c.1[i * 4 + 2]))
        if d > 24 { moved[i] = true; nm += 1 }
    }
    let r = [psnr(b.1, c.1), psnr(s, c.1), psnr(g, c.1),
             psnr(b.1, c.1, mask: moved), psnr(s, c.1, mask: moved), psnr(g, c.1, mask: moved)]
    for k in 0..<6 { sums[k] += r[k] }
    count += 1
    let pct = 100.0 * Double(nm) / Double(W * H)
    csv += "\(n)," + r.map { String(format: "%.2f", $0) }.joined(separator: ",") + String(format: ",%.1f\n", pct)
    print(String(format: "f%05d  all: repeat %.2f search %.2f game %.2f   moved (%.1f%%): repeat %.2f search %.2f game %.2f dB",
                 n, r[0], r[1], r[2], pct, r[3], r[4], r[5]))
    // A strip: real | repeat | search | game, a quarter of the size.
    let tw = W / 4, th = H / 4
    var strip = [UInt8](repeating: 0, count: tw * 4 * th * 4)
    for (k, src) in [c.1, b.1, s, g].enumerated() {
        for y in 0..<th { for x in 0..<tw {
            let si = ((y * 4) * W + x * 4) * 4, di = (y * tw * 4 + k * tw + x) * 4
            strip[di] = src[si]; strip[di + 1] = src[si + 1]; strip[di + 2] = src[si + 2]; strip[di + 3] = 255
        } }
    }
    let ctx = CGContext(data: &strip, width: tw * 4, height: th, bitsPerComponent: 8, bytesPerRow: tw * 16,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    if let img = ctx.makeImage(),
       let dst = CGImageDestinationCreateWithURL(outDir.appendingPathComponent(String(format: "strip_f%05d.png", n)) as CFURL,
                                                 UTType.png.identifier as CFString, 1, nil) {
        CGImageDestinationAddImage(dst, img, nil); CGImageDestinationFinalize(dst)
    }
}
try csv.write(to: outDir.appendingPathComponent("results.csv"), atomically: true, encoding: .utf8)
if count > 0 {
    let m = sums.map { $0 / Double(count) }
    let line = String(format: "%d pairs – all: repeat %.2f search %.2f game %.2f dB; moved pixels: repeat %.2f search %.2f game %.2f dB\n",
                      count, m[0], m[1], m[2], m[3], m[4], m[5])
    print(line, terminator: "")
    try line.write(to: outDir.appendingPathComponent("summary.txt"), atomically: true, encoding: .utf8)
} else {
    print("no pair with motion vectors found")
}
