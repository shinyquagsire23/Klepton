// Half-Life: Alyx's loading screen (KleptonHologram.swift), run on the Mac's GPU — `make hologram`.
//
// One view from where a sitting player's eyes are, through a reverse-Z projection as the composite pass
// has, into the pass's formats. Needs the game's files: $T_ALYX_HLVR, a folder named hlvr. Without them
// there is nothing to draw and the test says so and passes.
//
// Writes the pictures as PNGs to $T_HOLOGRAM_SHOTS or build/.
import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers
import simd

@main
enum THologram {
    static let width = 1920, height = 1080

    static func main() {
        guard let hlvr = ProcessInfo.processInfo.environment["T_ALYX_HLVR"],
              FileManager.default.fileExists(atPath: hlvr + "/models/ui/map_hologram_steamvr") else {
            print("t_hologram: T_ALYX_HLVR not set, skipped")
            return
        }
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no GPU") }
        let holo = KleptonHologram(device: device, root: URL(fileURLWithPath: hlvr))
        let started = Date()
        guard holo.load() else { fatalError("the game's files were not read") }
        print("t_hologram: read in \(String(format: "%.2f", Date().timeIntervalSince(started))) s")
        var failed = false
        func check(_ ok: Bool, _ what: String) {
            print("t_hologram: \(ok ? "ok" : "FAILED"): \(what)")
            if !ok { failed = true }
        }

        let prompt = "Drücke zum Starten auf Trigger"
        let shot = render(device, queue, holo, name: "prompt", map: "a4_c17_zoo", message: prompt, chapter: "")
        check(lit(shot, x: 0.5, y: 0.2, 0.15) > 0.002, "the prompt shows above the map")
        check(lit(shot, x: 0.5, y: 0.65, 0.04) > 0.02, "the map shows")
        _ = render(device, queue, holo, name: "loading", map: "a2_pistol", message: "", chapter: "")
        let intro = render(device, queue, holo, name: "intro", map: "a1_intro_world", message: "", chapter: "", pitch: 20)
        check(lit(intro, x: 0.5, y: 0.5, 0.45) == 0, "a level the page keeps off the map waits in black")
        let flash = render(device, queue, holo, name: "flash", map: "startup", message: prompt, chapter: "", at: 0.2)
        check(lit(flash, x: 0.5, y: 0.5, 0.45) == 0, "a prompt that is gone at once does not flash up")
        let introPrompt = render(device, queue, holo, name: "introprompt", map: "a1_intro_world", message: prompt, chapter: "", at: 1)
        check(lit(introPrompt, x: 0.5, y: 0.2, 0.15) > 0.002, "a prompt that stays shows there too")
        let chapterShot = render(device, queue, holo, name: "chapter", map: "a2_pistol", message: "", chapter: "VERSCHRÄNKUNG",
                                 ended: true, at: 3, pitch: 20)
        check(lit(chapterShot, x: 0.5, y: 0.5, 0.2) > 0.002, "the chapter's name shows once the level has loaded")
        check(lit(chapterShot, x: 0.5, y: 0.9, 0.08) < 0.02, "the map has faded by then")
        exit(failed ? 1 : 0)
    }

    static func render(_ device: MTLDevice, _ queue: MTLCommandQueue, _ holo: KleptonHologram, name: String,
                       map: String, message: String, chapter: String, ended: Bool = false, at: Double = 0.5, pitch: Float? = nil) -> [UInt8] {
        let head = headPose(pitch: pitch)
        _ = holo.update(phase: 0, map: "", message: "", loading: "", chapter: "", now: 0, originFromHead: head)
        _ = holo.update(phase: 1, map: map, message: message, loading: "LADEVORGANG LÄUFT", chapter: chapter,
                        now: 100, originFromHead: head)
        if ended { _ = holo.update(phase: 0, map: "", message: "", loading: "", chapter: "", now: 100, originFromHead: head) }

        func target(_ format: MTLPixelFormat) -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = format == .depth32Float ? .private : .shared
            return device.makeTexture(descriptor: d)!
        }
        let color = target(.bgra8Unorm_srgb), depth = target(.depth32Float)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = color
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        pass.depthAttachment.storeAction = .dontCare
        let cmd = queue.makeCommandBuffer()!
        let enc = cmd.makeRenderCommandEncoder(descriptor: pass)!
        let view = KleptonHologram.View(clipFromWorld: projection() * head.inverse, eye: SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z))
        let started = Date()
        guard holo.encode(enc, color: .bgra8Unorm_srgb, amplification: 1, views: [view], now: 100 + at) else { fatalError("nothing drawn") }
        print("t_hologram: \(name) encoded in \(String(format: "%.1f", Date().timeIntervalSince(started) * 1000)) ms")
        enc.endEncoding()
        cmd.commit()
        cmd.waitUntilCompleted()
        print("t_hologram: \(name) on the GPU \(String(format: "%.1f", (cmd.gpuEndTime - cmd.gpuStartTime) * 1000)) ms")

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        color.getBytes(&pixels, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        write(pixels, name: name)
        return pixels
    }

    /// Eyes at sitting height, looking a little down: where the picture this was matched to was taken.
    static func headPose(pitch override: Float?) -> simd_float4x4 {
        let env = ProcessInfo.processInfo.environment
        let eyeHeight = env["T_HOLOGRAM_EYE"].flatMap(Float.init) ?? 1.15
        let pitch = override ?? env["T_HOLOGRAM_PITCH"].flatMap(Float.init) ?? -6
        var m = simd_float4x4(simd_quatf(angle: pitch * .pi / 180, axis: [1, 0, 0]))
        m.columns.3 = SIMD4<Float>(0, eyeHeight, 0, 1)
        return m
    }

    /// Reverse-Z with an infinite far plane, the kind Compositor Services hands out.
    static func projection() -> simd_float4x4 {
        let fov = ProcessInfo.processInfo.environment["T_HOLOGRAM_FOV"].flatMap(Float.init) ?? 75
        let near: Float = 0.1, f = 1 / tan(fov * .pi / 360), aspect = Float(width) / Float(height)
        return simd_float4x4(SIMD4<Float>(f, 0, 0, 0),
                             SIMD4<Float>(0, f * aspect, 0, 0),
                             SIMD4<Float>(0, 0, 0, -1),
                             SIMD4<Float>(0, 0, near, 0))
    }

    /// The share of pixels in a box around (x, y), sides `size` of the picture, that hold anything.
    static func lit(_ p: [UInt8], x: Float, y: Float, _ size: Float) -> Float {
        let x0 = Int((x - size) * Float(width)), x1 = Int((x + size) * Float(width))
        let y0 = Int((y - size) * Float(height)), y1 = Int((y + size) * Float(height))
        var n = 0, on = 0
        for yy in y0..<y1 {
            for xx in x0..<x1 {
                let i = (yy * width + xx) * 4
                n += 1
                if Int(p[i]) + Int(p[i + 1]) + Int(p[i + 2]) > 120 { on += 1 }
            }
        }
        return Float(on) / Float(max(n, 1))
    }

    static func write(_ bgra: [UInt8], name: String) {
        var rgba = bgra
        for i in stride(from: 0, to: rgba.count, by: 4) { rgba.swapAt(i, i + 2); rgba[i + 3] = 255 }
        let dir = ProcessInfo.processInfo.environment["T_HOLOGRAM_SHOTS"] ?? "build"
        let url = URL(fileURLWithPath: "\(dir)/t_hologram_\(name).png")
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(dest, image, nil)
        CGImageDestinationFinalize(dest)
        print("t_hologram: wrote \(url.path)")
    }
}
