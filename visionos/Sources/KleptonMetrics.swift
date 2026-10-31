import CoreGraphics
import CoreText
import Foundation

// How the game is running, drawn as a panel for the headset (KL_STATS_CARD).
//
// The model is the performance overlay a Quest developer knows: the numbers of
// the last second in a column — frame rate, shader compiles, GPU and CPU
// load, memory, heat, picture size and its MetalFX upscale — and beside it the last minute of the
// three that say why a game does not feel smooth, as graphs. A single number
// answers "how fast is it now"; the graphs answer "was that a hitch or is it
// always like this", which is the question a test run is usually about.
//
// In English whatever the device speaks: these are a developer's terms, and a
// screenshot of the panel should read the same wherever it was taken.
//
// Only the drawing is here, on CoreGraphics and CoreText alone, so that the
// same code can be run on a Mac with made-up samples and looked at as a picture
// (the headset is a slow place to find out that two labels overlap). Where the
// samples come from is runtime/diag/kl_metrics.c; how the panel gets into the
// view is KleptonCompositor.swift.

/// One second of how the game ran. A value that is not known is negative
/// (0 for the picture size) and is left out of the panel.
struct KLMetricsSample {
    var fps: Float = 0             // new pictures of the game shown a second
    var displayHz: Float = 0       // pictures presented a second: the rate the app is shown at
    var compiles: Float = 0        // shaders and pipeline states a second that Metal had to compile
    var compilesCached: Float = 0  // ...and the ones it only fetched from what it had compiled before
    var gpuBusy: Float = -1        // share of the time the GPU worked for this app, 0..1
    var gpuGameMs: Float = -1      // GPU time of one game frame
    var gpuCompMs: Float = -1      // GPU time of one composite pass
    var cpuCores: Float = -1       // CPU the process used, in cores
    var cpuTop: Float = -1         // the busiest thread's share of one core, 0..1
    var cpuTopName = ""
    /// Whether a tap of Create + Options switches MetalFX here
    /// (KleptonMetalFX.canUpscale) — the panel then says so on its last line.
    var metalfxTap = false
    var memUsedMB = -1
    var memLeftMB = -1
    var thermal = -1               // ProcessInfo.ThermalState's raw value, 0 nominal .. 3 critical
    var eyeW = 0, eyeH = 0         // the picture the game draws (render size)
    var upscale = -1               // MetalFX: 0 off, 1 on but linear, 2 upscaled; -1 not known
    var outW = 0, outH = 0         // ...the size after it
    var path = ""                  // how the picture comes: "Vulkan · per eye", "OpenXR GL · 2 layers", ...
    var upscaleNote = ""           // ...and why it is linear
    /// Frame generation (KleptonFrameGen.Mode): 0 off, 1 ahead, 2 between; and whether the tap cycles it.
    var frameGen = 0
    var frameGenTap = false
    var generated: Float = 0       // pictures a second it made, each of them shown
    var shown: Float = 0           // different pictures shown a second: the game's and the generated ones
    /// What the eye gets: the game's own pictures, or with generated ones those too.
    var shownFps: Float { shown > 0 ? shown : fps }
}

enum KLMetricsPanel {
    /// Samples the graphs hold: a minute.
    static let history = 60
    /// The panel is laid out on 1120 x 560 units and drawn at that size and at
    /// a half and a quarter of it, each by CoreText itself, as the levels of
    /// one texture. How many pixels the panel gets in the view is not this
    /// file's to say — it is drawn into the game's picture, and that is made
    /// with 30 pixels a degree at one game's output quality (0.4: the panel is
    /// then some 480 pixels wide) and 40 at another's (1.0: 650) — so the
    /// sampler takes the level that fits, and the type is as sharp as the
    /// picture it is in and never coarser than that for want of texels.
    /// Its size in the view is in metres at 1.2 m.
    static let panelLayout = (w: 1120, h: 560)
    static let panelPixels = (w: 1120, h: 560)
    static let panelLevels = 3
    static let panelSize = SIMD2<Float>(0.338, 0.169)
    /// The same for the one-line readout (KL_STATS_CARD=line).
    static let linePixels = (w: 960, h: 96)
    static let lineSize = SIMD2<Float>(0.40, 0.04)

    /// A bitmap to draw one of the two into, with its origin at the top left.
    /// `units` is the width the drawing is laid out on, where that is not the
    /// width in pixels.
    static func makeContext(w: Int, h: Int, units: Int? = nil) -> CGContext? {
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(h)); ctx.scaleBy(x: 1, y: -1)
        if let units, units != w { ctx.scaleBy(x: CGFloat(w) / CGFloat(units), y: CGFloat(w) / CGFloat(units)) }
        // CoreText draws upwards; in a context turned top-down the glyphs are turned back.
        ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
        return ctx
    }

    // ---- colours ------------------------------------------------------------

    private static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
        CGColor(srgbRed: r, green: g, blue: b, alpha: a)
    }
    private static let back = rgb(0.035, 0.04, 0.05, 0.90)
    private static let well = rgb(1, 1, 1, 0.05)
    private static let grid = rgb(1, 1, 1, 0.12)
    private static let bright = rgb(0.95, 0.95, 0.95)
    private static let dim = rgb(0.62, 0.65, 0.70)
    private static let green = rgb(0.36, 0.86, 0.47)
    private static let blue = rgb(0.38, 0.72, 1.0)
    private static let amber = rgb(1.0, 0.76, 0.30)
    private static let red = rgb(1.0, 0.45, 0.36)
    private static let slate = rgb(0.50, 0.56, 0.66)

    // ---- text ---------------------------------------------------------------

    private static var fonts: [String: CTFont] = [:]
    /// The system's face, with digits of one width so that a number which
    /// changes does not move what stands beside it.
    private static func font(_ size: CGFloat, bold: Bool = false) -> CTFont {
        let key = "\(size)\(bold)"
        if let f = fonts[key] { return f }
        let base = CTFontCreateUIFontForLanguage(bold ? .emphasizedSystem : .system, size, nil)
            ?? CTFontCreateWithName("Helvetica" as CFString, size, nil)
        let digits: [[CFString: Any]] = [[kCTFontFeatureTypeIdentifierKey: kNumberSpacingType,
                                         kCTFontFeatureSelectorIdentifierKey: kMonospacedNumbersSelector]]
        let desc = CTFontDescriptorCreateCopyWithAttributes(
            CTFontCopyFontDescriptor(base), [kCTFontFeatureSettingsAttribute: digits] as CFDictionary)
        let f = CTFontCreateWithFontDescriptor(desc, size, nil)
        fonts[key] = f
        return f
    }

    private static func width(_ s: String, font: CTFont) -> CGFloat {
        let attrs = [kCTFontAttributeName: font] as CFDictionary
        guard let str = CFAttributedStringCreate(nil, s as CFString, attrs) else { return 0 }
        return CGFloat(CTLineGetTypographicBounds(CTLineCreateWithAttributedString(str), nil, nil, nil))
    }

    private enum Align { case left, right }
    /// Draws `s` with its baseline at `y`; returns its width.
    @discardableResult
    private static func text(_ ctx: CGContext, _ s: String, x: CGFloat, y: CGFloat, font: CTFont,
                             color: CGColor, align: Align = .left) -> CGFloat {
        let attrs = [kCTFontAttributeName: font, kCTForegroundColorAttributeName: color] as CFDictionary
        guard let str = CFAttributedStringCreate(nil, s as CFString, attrs) else { return 0 }
        let line = CTLineCreateWithAttributedString(str)
        let width = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        ctx.textPosition = CGPoint(x: align == .right ? x - width : x, y: y)
        CTLineDraw(line, ctx)
        return width
    }

    // ---- what the numbers are called ----------------------------------------

    static func thermalName(_ state: Int) -> String? {
        switch state {
        case 0: return "cool"
        case 1: return "warm"
        case 2: return "hot"
        case 3: return "critical"
        default: return nil
        }
    }

    private static func gigabytes(_ mb: Int) -> String { String(format: "%.1f", Double(mb) / 1024) }
    private static func decimal(_ v: Float) -> String { String(format: "%.1f", v) }

    /// The one-line readout: the four numbers one glances at.
    static func line(_ s: KLMetricsSample) -> String {
        var parts = [String(format: "%.0f fps", s.shownFps), String(format: "Display %.0f Hz", s.displayHz)]
        if s.gpuBusy >= 0 { parts.append(String(format: "GPU %.0f %%", s.gpuBusy * 100)) }
        if let t = thermalName(s.thermal) { parts.append(t) }
        return parts.joined(separator: "   ·   ")
    }

    static func drawLine(_ ctx: CGContext, sample: KLMetricsSample) {
        let w = CGFloat(linePixels.w), h = CGFloat(linePixels.h)
        ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(rgb(0.035, 0.04, 0.05))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        text(ctx, line(sample), x: 28, y: h / 2 + 16, font: font(44, bold: true), color: bright)
    }

    // ---- the panel ----------------------------------------------------------

    /// The whole panel from the samples so far, oldest first.
    static func drawPanel(_ ctx: CGContext, samples: [KLMetricsSample]) {
        let w = CGFloat(panelLayout.w), h = CGFloat(panelLayout.h)
        ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(back)
        ctx.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerWidth: 28,
                           cornerHeight: 28, transform: nil))
        ctx.fillPath()
        let left: CGFloat = 28, right: CGFloat = 404
        // How to get rid of it, and back: the two menu buttons of the Sense controllers (kl_metrics_chord).
        text(ctx, "Hold Create + Options 1 s to hide", x: left, y: h - 22, font: font(21), color: dim)
        // A tap of the two switches MetalFX where it can upscale (KleptonMetalFX.tapToggle), on the line above,
        // or cycles frame generation where that was chosen for the game or MetalFX has nothing to switch.
        let fgTap = samples.last?.frameGenTap ?? false
        let tapLine = (samples.last?.metalfxTap ?? false) || fgTap
        if tapLine {
            text(ctx, fgTap ? "Tap both: frame gen off / ahead / between" : "Tap both: MetalFX on / off",
                 x: left, y: h - 50, font: font(21), color: dim)
        }
        // Shown for the first time, before its first second is over.
        guard let now = samples.last else {
            text(ctx, "Measuring …", x: left, y: 112, font: font(56, bold: true), color: bright)
            return
        }

        // The column: the frame rate large – what is shown, the generated pictures included – then one row a
        // number.
        let big = text(ctx, String(format: "%.0f", now.shownFps), x: left, y: 112, font: font(100, bold: true),
                       color: bright)
        text(ctx, "FPS", x: left + big + 12, y: 112, font: font(30, bold: true), color: dim)
        text(ctx, String(format: "%.0f Hz", now.displayHz), x: right, y: 112, font: font(30), color: dim,
             align: .right)

        var rows: [(String, String, CGColor)] = []
        rows.append(("Shaders", String(format: "%.0f new · %.0f cached", now.compiles, now.compilesCached),
                     now.compiles >= 1 ? amber : bright))
        if now.gpuBusy >= 0 { rows.append(("GPU", String(format: "%.0f %%", now.gpuBusy * 100), bright)) }
        if now.gpuGameMs >= 0 {
            rows.append(("GPU, game", decimal(now.gpuGameMs) + " ms", bright))
        }
        if now.gpuCompMs >= 0 {
            rows.append(("GPU, display", decimal(now.gpuCompMs) + " ms", bright))
        }
        if now.cpuCores >= 0 {
            rows.append(("CPU", decimal(now.cpuCores) + " cores", bright))
        }
        if now.cpuTop >= 0 {
            // The thread's own name where it has one: which thread is the busy one is half the answer.
            let name = now.cpuTopName.isEmpty ? "Busiest thread"
                                              : String(now.cpuTopName.prefix(16))
            rows.append((name, String(format: "%.0f %%", now.cpuTop * 100), now.cpuTop >= 0.9 ? amber : bright))
        }
        if now.memUsedMB >= 0 {
            var v = gigabytes(now.memUsedMB) + " GB"
            if now.memLeftMB >= 0 { v += " · " + gigabytes(now.memLeftMB) + " left" }
            rows.append(("Memory", v, now.memLeftMB >= 0 && now.memLeftMB < 300 ? red : bright))
        }
        if let t = thermalName(now.thermal) {
            rows.append(("Heat", t, now.thermal >= 3 ? red : now.thermal == 2 ? amber : bright))
        }
        if now.eyeW > 0 { rows.append(("Render", "\(now.eyeW) × \(now.eyeH)", bright)) }
        // The upscale: whether it is on, how the picture comes (the engine and the compositor's path for
        // it), and what it is upscaled to. A switch that is on and does nothing says why, in amber.
        if now.upscale >= 0 {
            switch now.upscale {
            case 2: rows.append(("MetalFX", now.upscaleNote.isEmpty ? "on" : "on · " + now.upscaleNote, green))
            case 1: rows.append(("MetalFX", now.upscaleNote.isEmpty ? "on · linear"
                                                                   : "linear · " + now.upscaleNote, amber))
            default: rows.append(("MetalFX", "off", bright))
            }
        }
        // Frame generation: which of the three (a tap cycles them), and the game's own rate beside it – the
        // large number above has the generated pictures in it. "same rate" while every display frame has a
        // new game picture anyway (the display at the game's rate): nothing to make then.
        if now.frameGenTap || now.frameGen != 0 {
            let name = ["off", "ahead", "between"][min(max(now.frameGen, 0), 2)]
            if now.frameGen == 0 {
                rows.append(("Frame gen", "off", bright))
            } else if now.generated >= 1 {
                rows.append(("Frame gen", String(format: "%@ · game %.0f", name, now.fps), green))
            } else {
                rows.append(("Frame gen", name + " · same rate", amber))
            }
        }
        if !now.path.isEmpty { rows.append(("Path", now.path, bright)) }
        if now.upscale == 2, now.outW > 0 { rows.append(("Output", "\(now.outW) × \(now.outH)", green)) }
        // Nine rows fit at the full size; more close up, down to a size still read at a glance, so the
        // column always ends above the hints at the bottom — one line, two where a tap switches MetalFX.
        let footer: CGFloat = tapLine ? 90 : 62
        let step = min(41, (h - footer - 164) / CGFloat(max(rows.count - 1, 1)))
        let size = min(27, (step * 0.68).rounded())
        var y: CGFloat = 164
        for (label, value, color) in rows {
            let lw = text(ctx, label, x: left, y: y, font: font(size), color: dim)
            // A long value (the path) is set smaller rather than over its label.
            var vs = size
            while vs > 16, width(value, font: font(vs, bold: true)) > right - left - lw - 14 { vs -= 1 }
            text(ctx, value, x: right, y: y, font: font(vs, bold: true), color: color, align: .right)
            y += step
        }

        // The graphs: a minute each, the newest second at the right.
        let gx: CGFloat = 432, gw = w - gx - 24, gh: CGFloat = 160
        let top = fpsScale(samples)
        graph(ctx, CGRect(x: gx, y: 24, width: gw, height: gh),
              title: "Frames per second", top: top, unit: "",
              series: fpsSeries(samples))
        graph(ctx, CGRect(x: gx, y: 24 + gh + 16, width: gw, height: gh),
              title: "Load", top: 100, unit: " %",
              series: [(samples.map { $0.cpuTop < 0 ? -1 : $0.cpuTop * 100 }, amber, 3, "Thread"),
                       (samples.map { $0.gpuBusy < 0 ? -1 : $0.gpuBusy * 100 }, blue, 3, "GPU")])
        // A bar for every second in which shaders were asked for, in two parts: the ones that had to be
        // compiled, from the bottom, and on top of them the ones that were only fetched. Where a hitch in
        // the first graph has a red bar under it, that is what the hitch was.
        let most = samples.map { $0.compiles + $0.compilesCached }.max() ?? 0
        graph(ctx, CGRect(x: gx, y: 24 + 2 * (gh + 16), width: gw, height: gh),
              title: "Shaders per second",
              top: most <= 10 ? 10 : most <= 30 ? 30 : most <= 100 ? 100 : most <= 300 ? 300 : 1000, unit: "",
              series: [(samples.map { $0.compiles }, red, 0, "new"),
                       (samples.map { $0.compilesCached }, slate, 0, "cached")])
    }

    /// The frame-rate graph's lines: the display's rate, what the game drew, and – once a picture of the
    /// minute was generated – what was shown, the generated pictures included, over the game's line.
    private static func fpsSeries(_ samples: [KLMetricsSample])
        -> [(values: [Float], color: CGColor, width: CGFloat, name: String)] {
        var series: [(values: [Float], color: CGColor, width: CGFloat, name: String)] =
            [(samples.map { $0.displayHz }, dim, 2, "Hz")]
        if samples.contains(where: { $0.generated >= 1 }) {
            series.append((samples.map { $0.fps }, amber, 3, "Game"))
            series.append((samples.map { $0.shownFps }, green, 4, "Shown"))
        } else {
            series.append((samples.map { $0.fps }, green, 4, "Game"))
        }
        return series
    }

    /// The top of the frame-rate graph: the display rate the minute reached,
    /// rounded up to one a display has.
    private static func fpsScale(_ samples: [KLMetricsSample]) -> Float {
        let most = samples.map { max($0.fps, $0.displayHz) }.max() ?? 0
        for step: Float in [60, 72, 80, 90, 100, 120] where most <= step * 1.04 { return step }
        return 144
    }

    /// One graph: a title, what each line is and stands at, and the minute.
    /// A series with width 0 is drawn as bars, and several of those as one bar
    /// in parts, the first at the bottom. Values below 0 are gaps.
    private static func graph(_ ctx: CGContext, _ r: CGRect, title: String, top: Float, unit: String,
                              series: [(values: [Float], color: CGColor, width: CGFloat, name: String)]) {
        ctx.setFillColor(well)
        ctx.addPath(CGPath(roundedRect: r, cornerWidth: 12, cornerHeight: 12, transform: nil))
        ctx.fillPath()
        text(ctx, title, x: r.minX + 14, y: r.minY + 30, font: font(22), color: dim)
        // What each line stands at, from the right edge inwards.
        var lx = r.maxX - 14
        for s in series where !s.name.isEmpty {
            guard let v = s.values.last, v >= 0 else { continue }
            lx -= text(ctx, s.name + String(format: " %.0f", v) + unit, x: lx, y: r.minY + 30,
                       font: font(22, bold: true), color: s.color, align: .right) + 22
        }

        let plot = CGRect(x: r.minX + 14, y: r.minY + 44, width: r.width - 74, height: r.height - 58)
        ctx.setStrokeColor(grid)
        ctx.setLineWidth(1)
        for (frac, label) in [(CGFloat(1), top), (0.5, top / 2), (0, 0)] {
            let gy = plot.maxY - frac * plot.height
            ctx.move(to: CGPoint(x: plot.minX, y: gy)); ctx.addLine(to: CGPoint(x: plot.maxX, y: gy))
            ctx.strokePath()
            text(ctx, String(format: "%.0f", label), x: r.maxX - 14, y: gy + 6, font: font(17), color: dim,
                 align: .right)
        }

        let slot = plot.width / CGFloat(history - 1)
        func point(_ i: Int, _ n: Int, _ v: Float) -> CGPoint {
            // The newest sample at the right edge whatever the count, so the graph grows in from there.
            CGPoint(x: plot.maxX - CGFloat(n - 1 - i) * slot,
                    y: plot.maxY - CGFloat(min(max(v, 0), top) / top) * plot.height)
        }
        // Where each second's bar has got to, for the part that goes on top.
        var stacked = [CGFloat](repeating: plot.maxY, count: history)
        for s in series {
            let values = Array(s.values.suffix(history))
            if s.width == 0 {
                ctx.setFillColor(s.color)
                for (i, v) in values.enumerated() where v > 0 {
                    // Never so low that one shader in a second goes unseen.
                    let x = point(i, values.count, 0).x, from = stacked[i]
                    let to = max(from - max(CGFloat(min(v, top) / top) * plot.height, 6), plot.minY)
                    ctx.fill(CGRect(x: x - slot * 0.35, y: to, width: slot * 0.7, height: from - to))
                    stacked[i] = to
                }
                continue
            }
            ctx.setStrokeColor(s.color)
            ctx.setLineWidth(s.width)
            ctx.setLineJoin(.round)
            var open = false
            for (i, v) in values.enumerated() {
                guard v >= 0 else { open = false; continue }
                let p = point(i, values.count, v)
                if open { ctx.addLine(to: p) } else { ctx.move(to: p); open = true }
            }
            ctx.strokePath()
        }
    }
}
