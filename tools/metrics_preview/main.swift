// The metrics panel (KL_STATS_CARD) as a picture, from a made-up minute — the
// drawing of visionos/Sources/KleptonMetrics.swift run on the Mac, so that a
// change to the layout can be looked at without a headset.
//
//   make metrics-preview            build/metrics-panel.png, build/metrics-line.png
//   build/metrics_preview 12 out    12 samples, out-panel.png / out-line.png
//
// Also, at full size, out-panel-notap.png (MetalFX cannot upscale: no tap
// hint), out-panel-tap.png (it can: MetalFX switched off by a tap, and the
// hint for the tap) and out-panel-framegen.png (Half-Life: Alyx at 30 to 40 of
// 50 Hz, frame generation "between" from the middle of the minute on).
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

func write(_ ctx: CGContext, _ path: String) {
    let img = ctx.makeImage()!
    let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dst, img, nil)
    CGImageDestinationFinalize(dst)
}

// A made-up minute: a game at 90 of 90, a level load in the middle, a few hitches, the system halving at the end.
var samples: [KLMetricsSample] = []
var rng = SystemRandomNumberGenerator()
func jitter(_ a: Float) -> Float { Float.random(in: -a...a, using: &rng) }
let count = Int(CommandLine.arguments.dropFirst().compactMap { Int($0) }.first ?? 60)
for i in 0..<count {
    var s = KLMetricsSample()
    s.displayHz = 90 + jitter(0.4)
    s.fps = 90 + jitter(0.6)
    s.gpuBusy = 0.58 + jitter(0.04)
    s.cpuTop = 0.62 + jitter(0.05)
    s.cpuCores = 2.4 + jitter(0.2)
    if (20..<26).contains(i) { s.fps = 31 + jitter(6); s.cpuTop = 0.98; s.gpuBusy = 0.25 + jitter(0.05); s.cpuCores = 3.6 }
    if i == 38 || i == 47 { s.fps = 84 }
    if i >= 52 { s.fps = 72 + jitter(3); s.gpuBusy = 0.93 + jitter(0.03) }
    if (20..<26).contains(i) { s.compiles = Float.random(in: 8...40, using: &rng); s.compilesCached = Float.random(in: 5...30, using: &rng) }
    if i == 38 || i == 47 { s.compiles = 2 }
    if i == 12 || i == 44 || i >= 58 { s.compilesCached = Float.random(in: 2...12, using: &rng) }
    s.gpuGameMs = s.gpuBusy * 1000 / max(s.fps, 1) - 0.9
    s.gpuCompMs = 0.9 + jitter(0.1)
    s.cpuTopName = "UnityMain"
    s.memUsedMB = 3180 + i * 3
    s.memLeftMB = 1840 - i * 3
    s.thermal = i < 40 ? 0 : 1
    s.eyeW = 2688; s.eyeH = 2160
    s.upscale = 2; s.outW = 3584; s.outH = 2880
    s.path = "OpenXR Vulkan · 3 layers"
    samples.append(s)
}
let out = CommandLine.arguments.last!
// At the size a game with output quality 0.4 shows it with: 480 pixels across.
let p = KLMetricsPanel.makeContext(w: 480, h: 240, units: KLMetricsPanel.panelLayout.w)!
KLMetricsPanel.drawPanel(p, samples: samples)
write(p, out + "-panel.png")
// At full size: a game where a tap does nothing, and one where it has just switched MetalFX off.
let full = KLMetricsPanel.makeContext(w: KLMetricsPanel.panelPixels.w, h: KLMetricsPanel.panelPixels.h,
                                      units: KLMetricsPanel.panelLayout.w)!
KLMetricsPanel.drawPanel(full, samples: samples)
write(full, out + "-panel-notap.png")
var test = samples
test[test.count - 1].metalfxTap = true
test[test.count - 1].upscale = 0
test[test.count - 1].outW = 0
test[test.count - 1].outH = 0
let t = KLMetricsPanel.makeContext(w: KLMetricsPanel.panelPixels.w, h: KLMetricsPanel.panelPixels.h,
                                   units: KLMetricsPanel.panelLayout.w)!
KLMetricsPanel.drawPanel(t, samples: test)
write(t, out + "-panel-tap.png")
var fgs = samples
for i in fgs.indices {
    fgs[i].displayHz = 50 + jitter(0.3)
    fgs[i].fps = 34 + jitter(4)
    fgs[i].frameGenTap = true
    fgs[i].upscale = -1
    fgs[i].path = "Source 2 · Vulkan · 1 layer"
    if i >= 30 { fgs[i].frameGen = 2; fgs[i].generated = 50 - fgs[i].fps - 1 + jitter(0.5) }
    fgs[i].shown = fgs[i].generated > 0 ? min(fgs[i].displayHz, fgs[i].fps + fgs[i].generated) : 0
}
let fgp = KLMetricsPanel.makeContext(w: KLMetricsPanel.panelPixels.w, h: KLMetricsPanel.panelPixels.h,
                                     units: KLMetricsPanel.panelLayout.w)!
KLMetricsPanel.drawPanel(fgp, samples: fgs)
write(fgp, out + "-panel-framegen.png")
let l = KLMetricsPanel.makeContext(w: KLMetricsPanel.linePixels.w, h: KLMetricsPanel.linePixels.h)!
KLMetricsPanel.drawLine(l, sample: samples.last!)
write(l, out + "-line.png")
