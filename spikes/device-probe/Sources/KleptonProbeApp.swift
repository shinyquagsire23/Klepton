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
        out += "\n[P9] TSD slot 5 after graphics frameworks load (S0.1 residual)\n"
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

        print(out)          // also to console, for `simctl launch --console` / devicectl
        report = out
        done = true
    }

    func hex(_ v: UInt64) -> String { String(format: "%016llx", v) }
}
