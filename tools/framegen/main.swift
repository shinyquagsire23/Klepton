// framegen: measures KLFrameGen (visionos/Sources/KleptonFrameGen.swift) on a
// lab run's consecutive frames (moonshot s3-framegen).
//
//   tools/framegen/build.sh && build/framegen <frames dir> <out dir> [--split] [--samples N]
//
// The frames are a 100 Hz capture on a fixed 10 ms clock, with the render pose
// beside each (frame_NNN.pose). The capture side – KL_LAB_DT_MS, head poses in the
// script, KL_GLFB_OUT_FROM/UNTIL/POSE and tools/lab/beatsaber145-framegen.txt –
// lives on the lab branch (moonshot/s3-framegen on moonshot/s1-headless), not
// here. Every second frame is taken as what a 50 Hz game delivers; the
// ones between are what the display needs and what it gets:
//   repeat   the newest game frame again (today, before the compositor's timewarp)
//   extrap   the newest game frame moved on by half its motion (no added latency)
//   interp   between the two game frames around it (a game frame of latency – for comparison)
// Each is compared with the real frame: PSNR over the picture, and over the
// pixels that changed since the newest game frame (where judder is seen).
// --split treats the picture as two eyes side by side. Writes results.csv,
// summary.txt and, for N frames, a strip: real | repeat | extrap | interp.
import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers
import simd

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: framegen <frames dir> <out dir> [--split] [--samples N]\n".data(using: .utf8)!)
    exit(2)
}
let inDir = URL(fileURLWithPath: args[1]), outDir = URL(fileURLWithPath: args[2])
let split = args.contains("--split")
let samples = args.firstIndex(of: "--samples").flatMap { Int(args[$0 + 1]) } ?? 4
// --tune finest,block,zeroBias,smooth[,minMotion] – e.g. 0,8,0,0 (full size, 8×8, no zero preference, no median)
if let i = args.firstIndex(of: "--tune") {
    let t = args[i + 1].split(separator: ",").map(String.init)
    KLFrameGen.finest = Int(t[0]) ?? 1; KLFrameGen.block = Int(t[1]) ?? 8
    KLFrameGen.zeroBias = Float(t[2]) ?? 0.01; KLFrameGen.smooth = t[3] != "0"
    if t.count > 4 { KLFrameGen.minMotion = Float(t[4]) ?? 1.5 }
}
try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

let names = try FileManager.default.contentsOfDirectory(atPath: inDir.path)
    .filter { $0.hasPrefix("frame_") && $0.hasSuffix(".png") }.sorted()
guard names.count >= 6 else { print("need at least 6 frames, found \(names.count)"); exit(1) }

let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let gen = try KLFrameGen(device: device)

func load(_ name: String) -> (MTLTexture, [UInt8]) {
    let src = CGImageSourceCreateWithURL(inDir.appendingPathComponent(name) as CFURL, nil)!
    let img = CGImageSourceCreateImageAtIndex(src, 0, nil)!
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

/// PSNR (dB) of the luminance; `mask` limits it to pixels where the mask is set.
func psnr(_ a: [UInt8], _ b: [UInt8], mask: [Bool]? = nil) -> Double {
    var sum = 0.0, n = 0
    for i in 0..<(a.count / 4) {
        if let mask, !mask[i] { continue }
        func y(_ p: [UInt8]) -> Double { 0.2126 * Double(p[i * 4]) + 0.7152 * Double(p[i * 4 + 1]) + 0.0722 * Double(p[i * 4 + 2]) }
        let d = y(a) - y(b)
        sum += d * d; n += 1
    }
    guard n > 0 else { return .nan }
    let mse = sum / Double(n)
    return mse <= 1e-9 ? 99 : 10 * log10(255 * 255 / mse)
}

func writeStrip(_ parts: [[UInt8]], w: Int, h: Int, to url: URL) {
    let W = w * parts.count
    var px = [UInt8](repeating: 0, count: W * h * 4)
    for (k, p) in parts.enumerated() {
        for y in 0..<h { px.replaceSubrange((y * W + k * w) * 4 ..< (y * W + k * w + w) * 4, with: p[(y * w) * 4 ..< (y * w + w) * 4]) }
    }
    let ctx = CGContext(data: &px, width: W, height: h, bitsPerComponent: 8, bytesPerRow: W * 4,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
}

/// The render pose beside a frame (KL_GLFB_OUT_POSE): orientation and the left eye's tangents.
// FG_POSE_LAG=n: take the pose written n frames later (a capture whose pose lags its picture);
// FG_TAN_SCALE=x,y: scale the tangents (a picture cropped or stretched against its frustum).
let poseLag = Int(ProcessInfo.processInfo.environment["FG_POSE_LAG"] ?? "0") ?? 0
let tanScale: SIMD2<Float> = {
    let f = (ProcessInfo.processInfo.environment["FG_TAN_SCALE"] ?? "1,1").split(separator: ",").compactMap { Float($0) }
    return f.count == 2 ? SIMD2(f[0], f[1]) : SIMD2(1, 1)
}()
func pose(_ i0: Int) -> (simd_quatf, SIMD4<Float>)? {
    let i = min(max(0, i0 + poseLag), names.count - 1)
    let url = inDir.appendingPathComponent(names[i].replacingOccurrences(of: ".png", with: ".pose"))
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
    var q: simd_quatf?, t: SIMD4<Float>?
    for line in text.split(separator: "\n") {
        let f = line.split(separator: " ").compactMap { Float($0) }
        if line.hasPrefix("pose "), f.count == 7 { q = simd_quatf(ix: f[3], iy: f[4], iz: f[5], r: f[6]) }
        if line.hasPrefix("tangents0 "), f.count == 4 { t = SIMD4(f[0], f[1], f[2], f[3]) }
    }
    guard let q, let t else { return nil }
    return (q, t * SIMD4(tanScale.x, tanScale.x, tanScale.y, tanScale.y))
}
let fair = pose(0) != nil
if fair { print("render poses found: comparing as on the headset, with the head's turn taken out and timewarped") }

var csv = "frame,psnr_repeat,psnr_extrap,psnr_interp,moving_px,moving_repeat,moving_extrap,moving_interp,gpu_ms,tw_repeat,tw_extrap,tw_moving_px,tw_moving_repeat,tw_moving_extrap\n"
var rows: [[Double]] = []
var cache: [String: (MTLTexture, [UInt8])] = [:]
func get(_ i: Int) -> (MTLTexture, [UInt8]) {
    if let c = cache[names[i]] { return c }
    let v = load(names[i]); cache[names[i]] = v
    if cache.count > 8 { cache.removeValue(forKey: cache.keys.sorted().first!) }
    return v
}
let sampleEvery = max(1, (names.count / 2) / max(1, samples))
var written = 0
// Game frames are the even indices; k (odd) is the display frame between game frames k-1 and k+1.
for k in stride(from: 3, to: names.count - 1, by: 2) {
    let (older, _) = get(k - 3), (newer, newerPx) = get(k - 1), (next, _) = get(k + 1), (_, truth) = get(k)
    let w = newer.width, h = newer.height
    let rects: [SIMD4<Int32>] = split
        ? [SIMD4(0, 0, Int32(w / 2), Int32(h)), SIMD4(Int32(w / 2), 0, Int32(w - w / 2), Int32(h))]
        : [SIMD4(0, 0, Int32(w), Int32(h))]
    let ex = makeOut(newer), it = makeOut(newer)
    let cb = queue.makeCommandBuffer()!
    for r in rects {
        gen.extrapolate(older: older, newer: newer, phase: 0.5, into: ex, commandBuffer: cb, rect: r)
    }
    cb.commit(); cb.waitUntilCompleted()
    let gpuMs = (cb.gpuEndTime - cb.gpuStartTime) * 1000
    let cb2 = queue.makeCommandBuffer()!
    for r in rects { gen.extrapolate(older: newer, newer: next, phase: -0.5, into: it, commandBuffer: cb2, rect: r) }
    cb2.commit(); cb2.waitUntilCompleted()
    let exPx = readBack(ex), itPx = readBack(it)
    // As on the headset: the repeat is timewarped to the display pose; the generated frame carries on
    // only what the turn does not explain, and is timewarped the same way.
    var tw: [Double] = [.nan, .nan, .nan, .nan, .nan]
    if fair, let po = pose(k - 3), let pn = pose(k - 1), let pk = pose(k) {
        let size = SIMD2<Float>(Float(w), Float(h)), full = SIMD4<Int32>(0, 0, Int32(w), Int32(h))
        let toNewer = KLFrameGen.rotationMap(from: pk.0, to: pn.0, tangents: pn.1, size: size)
        let newerToOlder = KLFrameGen.rotationMap(from: pn.0, to: po.0, tangents: pn.1, size: size)
        let rep = makeOut(newer), gen2 = makeOut(newer), gen2tw = makeOut(newer)
        let cb3 = queue.makeCommandBuffer()!
        gen.turn(newer, rotation: toNewer, rect: full, into: rep, commandBuffer: cb3)
        let a = gen.pyramid(of: older, key: "older", commandBuffer: cb3)
        let b = gen.pyramid(of: newer, key: "newer", commandBuffer: cb3)
        gen.generate(older: a, newer: b, newerColor: newer, phase: 0.5, rotation: newerToOlder, into: gen2, commandBuffer: cb3)
        gen.turn(gen2, rotation: toNewer, rect: full, into: gen2tw, commandBuffer: cb3)
        cb3.commit(); cb3.waitUntilCompleted()
        let repPx = readBack(rep), genPx = readBack(gen2tw)
        var m2 = [Bool](repeating: false, count: w * h); var mv2 = 0
        for i in 0..<(w * h) {
            var d = 0
            for c in 0..<3 { d = max(d, abs(Int(truth[i * 4 + c]) - Int(repPx[i * 4 + c]))) }
            if d > 12 { m2[i] = true; mv2 += 1 }
        }
        tw = [psnr(repPx, truth), psnr(genPx, truth), Double(mv2) / Double(w * h) * 100,
              psnr(repPx, truth, mask: m2), psnr(genPx, truth, mask: m2)]
        if (k / 2) % sampleEvery == 1 && written < samples {
            writeStrip([truth, repPx, genPx], w: w, h: h, to: outDir.appendingPathComponent(String(format: "tw_%03d.png", k)))
        }
    }
    // "Moving": the real frame differs visibly from the newest game frame there.
    var mask = [Bool](repeating: false, count: w * h)
    var moving = 0
    for i in 0..<(w * h) {
        var d = 0
        for c in 0..<3 { d = max(d, abs(Int(truth[i * 4 + c]) - Int(newerPx[i * 4 + c]))) }
        if d > 12 { mask[i] = true; moving += 1 }
    }
    let row = [Double(k), psnr(newerPx, truth), psnr(exPx, truth), psnr(itPx, truth), Double(moving) / Double(w * h) * 100,
               psnr(newerPx, truth, mask: mask), psnr(exPx, truth, mask: mask), psnr(itPx, truth, mask: mask), gpuMs] + tw
    rows.append(row)
    csv += row.map { String(format: "%.3f", $0) }.joined(separator: ",") + "\n"
    if (k / 2) % sampleEvery == 1 && written < samples {
        writeStrip([truth, newerPx, exPx, itPx], w: w, h: h, to: outDir.appendingPathComponent(String(format: "strip_%03d.png", k)))
        written += 1
    }
}
try csv.write(to: outDir.appendingPathComponent("results.csv"), atomically: true, encoding: .utf8)
func mean(_ c: Int) -> Double { let v = rows.map { $0[c] }.filter { $0.isFinite }; return v.reduce(0, +) / Double(max(1, v.count)) }
func wins(_ a: Int, _ b: Int) -> Int { rows.filter { $0[a].isFinite && $0[b].isFinite && $0[a] > $0[b] }.count }
let summary = """
framegen on \(names.count) frames (\(rows.count) in-between frames)\(split ? ", two eyes" : "")
PSNR whole picture   repeat \(String(format: "%.2f", mean(1))) dB   extrap \(String(format: "%.2f", mean(2))) dB   interp \(String(format: "%.2f", mean(3))) dB
PSNR moving pixels   repeat \(String(format: "%.2f", mean(5))) dB   extrap \(String(format: "%.2f", mean(6))) dB   interp \(String(format: "%.2f", mean(7))) dB   (\(String(format: "%.1f", mean(4)))% of the picture moves)
extrap better than repeat in \(wins(2, 1)) of \(rows.count) frames (moving pixels: \(wins(6, 5)))
GPU per frame (extrap): \(String(format: "%.2f", mean(8))) ms
\(fair ? """
as on the headset (timewarp to the display pose, the turn taken out of the generated motion):
PSNR whole picture   repeat+timewarp \(String(format: "%.2f", mean(9))) dB   generated+timewarp \(String(format: "%.2f", mean(10))) dB
PSNR moving pixels   repeat+timewarp \(String(format: "%.2f", mean(12))) dB   generated+timewarp \(String(format: "%.2f", mean(13))) dB   (\(String(format: "%.1f", mean(11)))% still wrong after the timewarp)
generated better in \(wins(10, 9)) of \(rows.count) frames (moving pixels: \(wins(13, 12)))
""" : "no render poses beside the frames (KL_GLFB_OUT_POSE=1): no comparison with the timewarp")

"""
try summary.write(to: outDir.appendingPathComponent("summary.txt"), atomically: true, encoding: .utf8)
print(summary, terminator: "")
