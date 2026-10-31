// The metrics panel (KL_STATS_CARD) for the macOS viewer: KleptonMetrics.swift's drawing, built as a dylib
// that kl_view_mtl.m loads only when the panel is asked for — so `build/m_boot` itself stays plain C and
// Objective-C, and a run without the panel never loads Swift at all.
//
//   make viewer-panel        build/libklvm_panel.dylib
//
// One call: the sample as it is now (kl_metrics), drawn into the caller's RGBA8 pixels at the size given.
// The minute of history behind the graphs is kept here, as KleptonCompositor.startStats keeps it.
import CoreGraphics
import Foundation

private var samples: [KLMetricsSample] = []
private var lastSeq: UInt32 = 0

private func cString<T>(_ v: inout T) -> String {
    withUnsafeBytes(of: &v) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
}

/// Draws the panel into `out` (`w` x `h` RGBA8, rows top first, `w * 4` bytes a row). `units` is the
/// panel's layout width (KLMetricsPanel.panelLayout.w) so that smaller levels are the same panel scaled.
/// Returns 1 when something was drawn.
@_cdecl("klvm_panel_draw")
public func klvm_panel_draw(_ out: UnsafeMutablePointer<UInt8>, _ w: Int32, _ h: Int32,
                            _ m: UnsafePointer<kl_metrics>) -> Int32 {
    var k = m.pointee
    if k.seq != 0 && k.seq != lastSeq {
        lastSeq = k.seq
        var s = KLMetricsSample()
        s.fps = k.fps; s.displayHz = k.display_hz
        s.compiles = k.compiles; s.compilesCached = k.compiles_cached
        s.gpuBusy = k.gpu_busy; s.gpuGameMs = k.gpu_game_ms; s.gpuCompMs = k.gpu_comp_ms
        s.cpuCores = k.cpu_cores; s.cpuTop = k.cpu_top
        s.cpuTopName = cString(&k.cpu_top_name)
        s.memUsedMB = Int(k.mem_used_mb); s.memLeftMB = Int(k.mem_left_mb)
        s.thermal = ProcessInfo.processInfo.thermalState.rawValue
        s.eyeW = Int(k.eye_w); s.eyeH = Int(k.eye_h)
        s.upscale = Int(k.upscale); s.outW = Int(k.out_w); s.outH = Int(k.out_h)
        s.path = cString(&k.path); s.upscaleNote = cString(&k.upscale_note)
        samples.append(s)
        if samples.count > KLMetricsPanel.history { samples.removeFirst(samples.count - KLMetricsPanel.history) }
    }
    guard let ctx = KLMetricsPanel.makeContext(w: Int(w), h: Int(h), units: KLMetricsPanel.panelLayout.w),
          let data = ctx.data else { return 0 }
    KLMetricsPanel.drawPanel(ctx, samples: samples)
    let row = Int(w) * 4
    for y in 0..<Int(h) {
        memcpy(out + y * row, data + y * ctx.bytesPerRow, row)
    }
    return 1
}

/// The panel's texture size and level count, and its size in metres, as the compositor uses them.
@_cdecl("klvm_panel_geometry")
public func klvm_panel_geometry(_ w: UnsafeMutablePointer<Int32>, _ h: UnsafeMutablePointer<Int32>,
                                _ levels: UnsafeMutablePointer<Int32>, _ size: UnsafeMutablePointer<Float>) {
    w.pointee = Int32(KLMetricsPanel.panelPixels.w); h.pointee = Int32(KLMetricsPanel.panelPixels.h)
    levels.pointee = Int32(KLMetricsPanel.panelLevels)
    size[0] = KLMetricsPanel.panelSize.x; size[1] = KLMetricsPanel.panelSize.y
}
