import SwiftUI
import Metal
import Foundation

@main
struct KleptonProbeApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
            .defaultSize(width: 900, height: 1000)
    }
}

struct ContentView: View {
    @State private var report = "running…"
    @State private var done = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Klepton device probe").font(.largeTitle.bold())
            ScrollView {
                Text(report)
                    .font(.system(.footnote, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            HStack {
                if done {
                    ShareLink(item: report) { Label("Share report", systemImage: "square.and.arrow.up") }
                } else {
                    ProgressView(); Text("running probes…")
                }
                Spacer()
            }
        }
        .padding(24)
        .task { await run() }
    }

    func run() async {
        var out = ""
        // ---- C battery (P1..P7) ----
        let bundlePath = Bundle.main.bundlePath
        if let c = klepton_run_probes(bundlePath) {
            out += String(cString: c)
            free(c)
        }

        // ---- P8: Metal runtime shader compilation (gates MoltenVK) ----
        out += "\n\n[P8] Metal runtime shader compilation (gates MoltenVK / SPIRV-Cross)\n"
        if let dev = MTLCreateSystemDefaultDevice() {
            out += "    device            : \(dev.name)\n"
            let src = """
            #include <metal_stdlib>
            using namespace metal;
            vertex float4 v_main(uint vid [[vertex_id]]) { return float4(vid, 0, 0, 1); }
            fragment half4 f_main() { return half4(1); }
            """
            let t0 = CFAbsoluteTimeGetCurrent()
            do {
                let lib = try await dev.makeLibrary(source: src, options: nil)
                let dt = (CFAbsoluteTimeGetCurrent() - t0) * 1000
                out += "    makeLibrary(source:): OK in \(String(format: "%.1f", dt)) ms\n"
                out += "    functions         : \(lib.functionNames.joined(separator: ", "))\n"
                out += "    VERDICT           : runtime MSL compilation PERMITTED [MoltenVK viable]\n"
            } catch {
                out += "    makeLibrary FAILED: \(error)\n"
                out += "    VERDICT           : runtime MSL compilation BLOCKED [MoltenVK BROKEN]\n"
            }
        } else {
            out += "    no Metal device\n"
        }

        // ---- P9: does loading graphics/AR frameworks claim Darwin TSD slot 5? ----
        out += "\n[P9] TSD slot 5 after graphics frameworks load\n"
        out += "    slot 5 after Metal: \(hex(klepton_tsd_slot(5)))"
        out += klepton_tsd_slot(5) == 0 ? "  [still FREE]\n" : "  [CLAIMED - problem]\n"
        for fw in ["/System/Library/Frameworks/ARKit.framework/ARKit",
                   "/System/Library/Frameworks/CompositorServices.framework/CompositorServices",
                   "/System/Library/Frameworks/RealityKit.framework/RealityKit"] {
            let h = dlopen(fw, RTLD_NOW)
            let name = (fw as NSString).lastPathComponent
            out += "    dlopen \(name): \(h != nil ? "OK" : "failed")  slot5=\(hex(klepton_tsd_slot(5)))\n"
        }
        out += "    VERDICT: slot 5 \(klepton_tsd_slot(5) == 0 ? "STILL FREE [fix holds]" : "CLAIMED [need another slot]")\n"

        // ---- P10: thread/stack sanity for Unity's thread count ----
        out += "\n[P10] thread creation sanity (Unity spawns many)\n"
        var made = 0
        var threads: [Thread] = []
        for _ in 0..<64 {
            let t = Thread { Thread.sleep(forTimeInterval: 0.3) }
            t.stackSize = 512 * 1024
            t.start(); threads.append(t); made += 1
        }
        out += "    started \(made) threads with 512KB stacks: OK\n"

        // ---- P13: vtool'd ANGLE under AMFI, and the Metal interop primitive ----
        // The last unverified tail of the ANGLE retarget, plus the mechanism the
        // whole P5 renderer rests on. Swift allocates the texture because that is
        // where it will come from for real (Compositor Services), and each side
        // verifies through its own API: C reports what GL wrote, Swift reports
        // what actually landed in the MTLTexture. Neither implies the other.
        out += "\n[P13] ANGLE (vtool-retargeted) + MTLTexture interop  (port rung P5)\n"
        out += angleProbe(bundlePath: bundlePath)

        print(out)          // also to console, for `simctl launch --console` / devicectl
        report = out
        done = true
    }

    func angleProbe(bundlePath: String) -> String {
        guard let devPtr = klepton_angle_init(bundlePath) else {
            return String(cString: klepton_angle_log())
        }
        let dev = Unmanaged<AnyObject>.fromOpaque(devPtr).takeUnretainedValue() as! MTLDevice
        klepton_angle_note("    ANGLE's MTLDevice = \(dev.name)")
        let sysSame = (MTLCreateSystemDefaultDevice() as AnyObject?) === (dev as AnyObject)
        klepton_angle_note("    (system default is \(sysSame ? "the same" : "a DIFFERENT") object)")

        // ---- case 1: two eyes as two slices of one RGBA8 array texture.
        // Small, and the only case where GL can read its own work back with a
        // fixed format/type pair — so it is where the two-sided check lives.
        let W = 64, H = 32
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .rgba8Unorm
        d.width = W; d.height = H; d.arrayLength = 2
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared          // so getBytes: can read it directly
        guard let arr = dev.makeTexture(descriptor: d) else {
            klepton_angle_note("    makeTexture(2DArray RGBA8) FAILED")
            return String(cString: klepton_angle_log())
        }
        let want: [(Float, Float, Float)] = [(1, 0, 0), (0, 1, 0)]
        let texPtr = Unmanaged.passUnretained(arr as AnyObject).toOpaque()
        for slice in 0..<2 {
            let rc = klepton_angle_draw(texPtr, Int32(slice), 0, Int32(W), Int32(H),
                                        want[slice].0, want[slice].1, want[slice].2)
            if rc != 0 { klepton_angle_note("    slice \(slice): C side failed at step \(rc)"); continue }
            var px = [UInt8](repeating: 0, count: W * H * 4)
            px.withUnsafeMutableBytes { raw in
                arr.getBytes(raw.baseAddress!, bytesPerRow: W * 4, bytesPerImage: W * H * 4,
                             from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, slice: slice)
            }
            let o = ((H / 2) * W + W / 2) * 4
            let wr = UInt8(want[slice].0 * 255), wg = UInt8(want[slice].1 * 255)
            let ok: Bool = (px[o] == wr && px[o + 1] == wg)
            let got = "\(px[o]),\(px[o + 1]),\(px[o + 2])"
            let verdict = ok ? "[landed in OUR texture]" : "[WRONG]"
            klepton_angle_note("    slice \(slice): Metal-side readback = \(got)  \(verdict)")
        }
        // Slice independence. If EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE were ignored,
        // every check above still passes — each read follows its own draw — and
        // only this one fails. Both eyes would then carry the same image, which
        // presents as a compositor bug rather than as an EGL one.
        var px0 = [UInt8](repeating: 0, count: W * H * 4)
        px0.withUnsafeMutableBytes { raw in
            arr.getBytes(raw.baseAddress!, bytesPerRow: W * 4, bytesPerImage: W * H * 4,
                         from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, slice: 0)
        }
        let o0 = ((H / 2) * W + W / 2) * 4
        let got0 = "\(px0[o0]),\(px0[o0 + 1]),\(px0[o0 + 2])"
        let indep: Bool = (px0[o0] == 255 && px0[o0 + 1] == 0)
        let v0 = indep ? "[eyes are independent]" : "[SLICE ATTR IGNORED]"
        klepton_angle_note("    slice 0 survived slice 1: \(got0)  \(v0)")

        // ---- case 2: the guest's real eye texture. RGBA16F at 2198x2304, two
        // slices — the size and format kl_ovrp.c's SetupEyeTexture2 allocates
        // (h wide, w tall; the transposition is explained there). The drawn value
        // is 2.0, above 1.0 on purpose: a float target carries it and a silently
        // -unorm one clamps, which distinguishes "really RGBA16F" from "accepted
        // the enum".
        let EW = 2198, EH = 2304
        let ed = MTLTextureDescriptor()
        ed.textureType = .type2DArray
        ed.pixelFormat = .rgba16Float
        ed.width = EW; ed.height = EH; ed.arrayLength = 2
        ed.usage = [.renderTarget, .shaderRead]
        ed.storageMode = .shared
        guard let eye = dev.makeTexture(descriptor: ed) else {
            klepton_angle_note("    makeTexture(RGBA16F \(EW)x\(EH) x2) FAILED — "
                + "the eye allocation is the problem, not the interop")
            return String(cString: klepton_angle_log())
        }
        let eyeMB: Int = EW * EH * 8 * 2 / (1024 * 1024)
        klepton_angle_note("    eye texture allocated: RGBA16F \(EW)x\(EH) x2 slices (\(eyeMB) MB)")
        let eyePtr = Unmanaged.passUnretained(eye as AnyObject).toOpaque()
        let rc = klepton_angle_draw(eyePtr, 1, 1, Int32(EW), Int32(EH), 2.0, 0.5, 0.25)
        if rc == 0 {
            var h4 = [Float16](repeating: 0, count: 4)
            h4.withUnsafeMutableBytes { raw in
                eye.getBytes(raw.baseAddress!, bytesPerRow: 8, bytesPerImage: 8,
                             from: MTLRegionMake2D(EW / 2, EH / 2, 1, 1),
                             mipmapLevel: 0, slice: 1)
            }
            let r = Float(h4[0]), g = Float(h4[1])
            let ok = r > 1.9 && r < 2.1 && g > 0.45 && g < 0.55
            klepton_angle_note(String(format:
                "    eye slice 1: Metal-side half = %.3f, %.3f  %@", r, g,
                ok ? "[HDR value survived — genuinely float]" : "[WRONG — clamped?]"))
            klepton_angle_note(ok
                ? "    VERDICT: P5 interop HOLDS on device — ANGLE renders into our MTLTextures"
                : "    VERDICT: interop reached the texture but the values are wrong")
        } else {
            klepton_angle_note("    eye slice 1: failed at step \(rc)")
            klepton_angle_note("    VERDICT: the RGBA8 case worked but the real eye format did not")
        }
        return String(cString: klepton_angle_log())
    }

    func hex(_ v: UInt64) -> String { String(format: "%016llx", v) }
}
