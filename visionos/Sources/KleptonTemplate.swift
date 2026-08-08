// Apple's MetalImmersiveTemplate, copied as directly as it can be, with the
// spinning cube replaced by a blue clear. Source: ../MetalImmersiveTemplate.
//
// Why verbatim rather than "the same idea": every reconstruction I wrote of this
// loop looked correct, passed every check I could think of, and composited
// nothing. The template does several things to a drawable that none of my
// versions did, and I had no way to know which of them mattered — so the copy is
// the point, and the bisection back towards our own compositor comes after it
// renders. Differences from what KleptonCompositor does today, all of them
// candidates for the black screen:
//
//   * MTLResidencySet. The drawable's colour and depth textures are added to a
//     per-frame residency set and the command buffer is told to use it. Metal 4
//     on visionOS 26 does not implicitly make them resident, and a render pass
//     that writes to a non-resident texture is exactly "everything looks right
//     and nothing appears".
//   * device = layerRenderer.device — the layer's own device, not
//     MTLCreateSystemDefaultDevice().
//   * ONE render pass for both eyes, with renderTargetArrayLength = views.count
//     and vertex amplification. Ours does two passes with slice = N.
//   * MSAA into memoryless targets, resolving into the drawable's textures,
//     whenever the device supports it — so the drawable texture is a RESOLVE
//     target and never the render target.
//   * rasterizationRateMap set from the drawable.
//   * frame pacing on an MTLSharedEvent against maxBuffersInFlight.
//   * the configuration sets NO colorFormat and NO depthFormat. We set both.
//   * a CompositorContent struct that is its own CompositorLayerConfiguration,
//     rather than a bare CompositorLayer in the ImmersiveSpace closure.
//
// KL_TEMPLATE=1 runs it INSTEAD of booting the guest. KL_TPL_MIXED=1 puts it in
// .mixed, where passthrough behind the layer makes "blue" and "nothing" two
// visibly different answers instead of two black fields.
import SwiftUI
import CompositorServices
import Metal
import ARKit
import simd

enum Template {
    static let id = "KleptonTemplate"
    static var wanted: Bool { klEnvOn("KL_TEMPLATE", default: false) }
    static var mixed: Bool { klEnvOn("KL_TPL_MIXED", default: false) }
}

nonisolated let tplMaxBuffersInFlight = 3

extension MTLDevice {
    nonisolated var tplSupportsMSAA: Bool { supports32BitMSAA && supportsTextureSampleCount(4) }
    nonisolated var tplRasterSampleCount: Int { tplSupportsMSAA ? 4 : 1 }
}

extension LayerRenderer.Clock.Instant {
    nonisolated var tplTimeInterval: TimeInterval {
        let c = LayerRenderer.Clock.Instant.epoch.duration(to: self).components
        return TimeInterval(c.seconds)
            + (TimeInterval(c.attoseconds / 1_000_000_000) / TimeInterval(NSEC_PER_SEC))
    }
}

final class TplTaskExecutor: TaskExecutor {
    private let queue = DispatchQueue(label: "TplRenderThreadQueue", qos: .userInteractive)
    func enqueue(_ job: UnownedJob) {
        queue.async { job.runSynchronously(on: self.asUnownedSerialExecutor()) }
    }
    nonisolated func asUnownedSerialExecutor() -> UnownedTaskExecutor {
        UnownedTaskExecutor(ordinary: self)
    }
    static let shared = TplTaskExecutor()
}

/// The template's scene shape: a CompositorContent that is also its own
/// configuration. Note it sets neither colorFormat nor depthFormat.
struct TemplateImmersiveContent: CompositorContent {
    var body: some CompositorContent {
        CompositorLayer(configuration: self) { @MainActor layerRenderer in
            TemplateRenderer.startRenderLoop(layerRenderer, arSession: ARKitSession())
        }
    }
}

extension TemplateImmersiveContent: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        let foveationEnabled = capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveationEnabled
        let options: LayerRenderer.Capabilities.SupportedLayoutsOptions =
            foveationEnabled ? [.foveationEnabled] : []
        let supportedLayouts = capabilities.supportedLayouts(options: options)
        configuration.layout = supportedLayouts.contains(.layered) ? .layered : .dedicated
        NSLog("[tpl] configured: foveation=\(foveationEnabled) "
              + "layout=\(configuration.layout.rawValue) "
              + "color=\(configuration.colorFormat.rawValue) "
              + "depth=\(configuration.depthFormat.rawValue)")
    }
}

actor TemplateRenderer {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue
    #if !targetEnvironment(simulator)
    let residencySets: [MTLResidencySet]
    #endif

    let endFrameEvent: MTLSharedEvent
    var committedFrameIndex: UInt64 = 0
    var uniformBufferIndex = 0

    /// A real draw, not just a clear. The template draws a cube; mine only
    /// cleared, and a pass with no draw in it is a genuine deviation — it can be
    /// elided, and under MSAA there is arguably nothing to resolve. So the blue
    /// comes from a fragment shader over a fullscreen triangle, which is the
    /// smallest thing that is unambiguously *drawn*.
    let pipelineState: MTLRenderPipelineState?
    let depthState: MTLDepthStencilState?
    let useMSAA: Bool

    /// The quad is drawn at a REAL depth, and the depth buffer is written.
    ///
    /// visionOS reprojects the submitted frame using its depth buffer, so depth
    /// is not optional decoration on the colour — it is what tells the compositor
    /// where the pixels are. The drawable's range is reverse-Z with an infinite
    /// far plane (measured: depthRange = (inf, 0.1)), so NDC z = near / distance
    /// and z = 0 means *infinitely far*. A fullscreen triangle at z = 0 with
    /// depth writes disabled therefore submits a perfectly good blue image whose
    /// depth says "there is nothing here at any finite distance" — which is
    /// exactly the shape of the bug: correct colour in the framebuffer, and a
    /// display that shows none of it.
    ///
    /// 0.1 / 2.0 = 0.05 puts the quad two metres out.
    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; };
    vertex VOut tpl_v(uint vid [[vertex_id]]) {
        float2 p = float2((vid << 1) & 2, vid & 2);
        VOut o;
        o.pos = float4(p * 2.0 - 1.0, 0.05, 1.0);   // reverse-Z: ~2 m
        return o;
    }
    fragment float4 tpl_f() { return float4(0.0, 0.0, 1.0, 1.0); }
    """

    var perDrawableTarget = [LayerRenderer.Drawable.Target: DrawableTarget]()

    let worldTracking: WorldTrackingProvider
    let layerRenderer: LayerRenderer

    private var frames = 0
    private var anchored = 0
    private var nextReport = Date()
    private var loggedShape = false

    init(_ layerRenderer: LayerRenderer) {
        self.layerRenderer = layerRenderer
        self.device = layerRenderer.device        // the LAYER's device
        // Local binding, exactly as the template has it: the closure below must
        // not capture a partially-initialised self.
        let device = self.device
        self.commandQueue = device.makeCommandQueue()!

        #if !targetEnvironment(simulator)
        let residencySetDesc = MTLResidencySetDescriptor()
        residencySetDesc.initialCapacity = 3      // color + depth + spare
        self.residencySets = (0...tplMaxBuffersInFlight).map { _ in
            try! device.makeResidencySet(descriptor: residencySetDesc)
        }
        #endif

        self.endFrameEvent = device.makeSharedEvent()!
        self.endFrameEvent.signaledValue = UInt64(tplMaxBuffersInFlight)
        committedFrameIndex = UInt64(tplMaxBuffersInFlight)

        // Decided once: the pipeline's rasterSampleCount has to match the render
        // target's, so the MSAA choice cannot be re-read per frame.
        let msaa = device.tplSupportsMSAA && !klEnvOn("KL_TPL_NOMSAA", default: false)
        self.useMSAA = msaa
        var pipe: MTLRenderPipelineState?
        do {
            let lib = try device.makeLibrary(source: Self.shaderSource, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "tpl_v")
            d.fragmentFunction = lib.makeFunction(name: "tpl_f")
            d.colorAttachments[0].pixelFormat = layerRenderer.configuration.colorFormat
            d.depthAttachmentPixelFormat = layerRenderer.configuration.depthFormat
            d.rasterSampleCount = msaa ? device.tplRasterSampleCount : 1
            d.maxVertexAmplificationCount = layerRenderer.properties.viewCount
            pipe = try device.makeRenderPipelineState(descriptor: d)
            NSLog("[tpl] blue pipeline built (msaa=\(msaa) samples=\(d.rasterSampleCount) "
                  + "amplification=\(d.maxVertexAmplificationCount))")
        } catch {
            NSLog("[tpl] blue pipeline FAILED: \(error)")
            pipe = nil
        }
        self.pipelineState = pipe

        // The template's own depth state, and it is load-bearing rather than
        // cosmetic: .greater against a reverse-Z clear of 0 (so anything nearer
        // than infinity passes) and, critically, depth WRITES ENABLED so the
        // compositor is handed the geometry it reprojects with.
        let dsd = MTLDepthStencilDescriptor()
        dsd.depthCompareFunction = .greater
        dsd.isDepthWriteEnabled = true
        self.depthState = device.makeDepthStencilState(descriptor: dsd)

        worldTracking = WorldTrackingProvider()
    }

    private func startARSession(_ arSession: ARKitSession) async {
        do {
            try await arSession.run([worldTracking])
            NSLog("[tpl] ARKit world tracking running")
        } catch {
            NSLog("[tpl] ARKit failed: \(error)")
        }
    }

    @MainActor
    static func startRenderLoop(_ layerRenderer: LayerRenderer, arSession: ARKitSession) {
        let renderer = TemplateRenderer(layerRenderer)
        Task(executorPreference: TplTaskExecutor.shared) {
            await renderer.startARSession(arSession)
            await renderer.renderLoop()
        }
    }

    private func updateDynamicBufferState(frameIndex: UInt64) {
        uniformBufferIndex = (uniformBufferIndex + 1) % tplMaxBuffersInFlight
        #if !targetEnvironment(simulator)
        residencySets[uniformBufferIndex].removeAllAllocations()
        residencySets[uniformBufferIndex].commit()
        #endif
        perDrawableTarget = perDrawableTarget.filter { $0.value.lastUsedFrameIndex + 90 > frameIndex }
    }

    func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }

        guard self.endFrameEvent.wait(untilSignaledValue: committedFrameIndex - UInt64(tplMaxBuffersInFlight),
                                      timeoutMS: 10000) else { return }

        frame.startUpdate()
        self.updateDynamicBufferState(frameIndex: frame.frameIndex)
        frame.endUpdate()

        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        guard let commandBuffer = commandQueue.makeCommandBuffer() else { return }

        #if !targetEnvironment(simulator)
        commandBuffer.useResidencySet(self.residencySets[uniformBufferIndex])
        #endif

        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else { return }

        frame.startSubmission()
        for drawable in drawables {
            render(drawable: drawable, commandBuffer: commandBuffer, frameIndex: frame.frameIndex)
        }
        committedFrameIndex += 1
        commandBuffer.encodeSignalEvent(self.endFrameEvent, value: committedFrameIndex)
        commandBuffer.commit()
        frame.endSubmission()

        frames += 1
        if Date() >= nextReport {
            nextReport = Date().addingTimeInterval(2)
            NSLog("[tpl] \(frames) frames, \(anchored) anchored, "
                  + "state=\(String(describing: layerRenderer.state))")
        }
    }

    func render(drawable: LayerRenderer.Drawable, commandBuffer: MTLCommandBuffer, frameIndex: UInt64) {
        let time = drawable.frameTiming.presentationTime.tplTimeInterval
        let deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: time)
        drawable.deviceAnchor = deviceAnchor
        if deviceAnchor != nil { anchored += 1 }

        if perDrawableTarget[drawable.target] == nil {
            perDrawableTarget[drawable.target] = .init(drawable: drawable)
        }
        let drawableTarget = perDrawableTarget[drawable.target]!
        drawableTarget.updateBufferState(uniformBufferIndex: uniformBufferIndex, frameIndex: frameIndex)

        let renderPassDescriptor = MTLRenderPassDescriptor()

        // KL_TPL_NOMSAA=1 renders straight into the drawable's texture instead
        // of resolving into it. The template always takes the MSAA path, but the
        // template also DRAWS; this pass only clears, and a multisample resolve
        // of a pass containing no draw is exactly the kind of thing a driver may
        // treat as having nothing to resolve. The clear is the entire content
        // here, so that would look identical to "nothing composites" — it needs
        // its own A/B rather than an assumption.
        if useMSAA {
            let renderTargets = drawableTarget.memorylessTargets[uniformBufferIndex]
            renderPassDescriptor.colorAttachments[0].resolveTexture = drawable.colorTextures[0]
            renderPassDescriptor.colorAttachments[0].texture = renderTargets.color
            renderPassDescriptor.depthAttachment.resolveTexture = drawable.depthTextures[0]
            renderPassDescriptor.depthAttachment.texture = renderTargets.depth
            renderPassDescriptor.colorAttachments[0].storeAction = .multisampleResolve
            renderPassDescriptor.depthAttachment.storeAction = .multisampleResolve
        } else {
            renderPassDescriptor.colorAttachments[0].texture = drawable.colorTextures[0]
            renderPassDescriptor.depthAttachment.texture = drawable.depthTextures[0]
            renderPassDescriptor.colorAttachments[0].storeAction = .store
            renderPassDescriptor.depthAttachment.storeAction = .store
        }

        renderPassDescriptor.colorAttachments[0].loadAction = .clear
        // The one intended difference from the template: blue, not transparent
        // black. This is the whole content of the floor test.
        renderPassDescriptor.colorAttachments[0].clearColor =
            MTLClearColor(red: 0.0, green: 0.0, blue: 1.0, alpha: 1.0)
        renderPassDescriptor.depthAttachment.loadAction = .clear
        renderPassDescriptor.depthAttachment.clearDepth = 0.0
        renderPassDescriptor.rasterizationRateMap = drawable.rasterizationRateMaps.first
        if layerRenderer.configuration.layout == .layered {
            renderPassDescriptor.renderTargetArrayLength = drawable.views.count
        }

        #if !targetEnvironment(simulator)
        let residencySet = self.residencySets[uniformBufferIndex]
        residencySet.addAllocations([drawable.colorTextures[0], drawable.depthTextures[0]])
        residencySet.commit()
        #endif

        if !loggedShape {
            loggedShape = true
            NSLog("[tpl] drawables=\(perDrawableTarget.count) views=\(drawable.views.count) "
                  + "msaa=\(device.tplSupportsMSAA) "
                  + "color=\(drawable.colorTextures[0].width)x\(drawable.colorTextures[0].height)"
                  + " array=\(drawable.colorTextures[0].arrayLength) "
                  + "rrm=\(drawable.rasterizationRateMaps.isEmpty ? "none" : "yes")")
        }

        guard let renderEncoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPassDescriptor) else {
            NSLog("[tpl] failed to make render encoder")
            return
        }
        renderEncoder.label = "Template Blue Clear"
        // No cube: the clear IS the frame. Viewports and amplification are still
        // set exactly as the template sets them, because they are part of how the
        // pass is described to Compositor Services and not part of the drawing.
        let viewports = drawable.views.map { $0.textureMap.viewport }
        renderEncoder.setViewports(viewports)
        if drawable.views.count > 1 {
            var viewMappings = (0..<drawable.views.count).map {
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0),
                                                  renderTargetArrayIndexOffset: UInt32($0))
            }
            renderEncoder.setVertexAmplificationCount(viewports.count, viewMappings: &viewMappings)
        }
        // The blue is DRAWN, at a real depth, with depth writes on — see
        // shaderSource. Colour alone is not a frame on this platform.
        if let pipelineState {
            renderEncoder.setRenderPipelineState(pipelineState)
            if let depthState { renderEncoder.setDepthStencilState(depthState) }
            renderEncoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        renderEncoder.endEncoding()

        drawable.encodePresent(commandBuffer: commandBuffer)
    }

    func renderLoop() {
        NSLog("[tpl] render loop starting")
        while true {
            if layerRenderer.state == .invalidated {
                NSLog("[tpl] layer invalidated after \(frames) frames (\(anchored) anchored)")
                return
            } else if layerRenderer.state == .paused {
                layerRenderer.waitUntilRunning()
                continue
            } else {
                autoreleasepool { self.renderFrame() }
            }
        }
    }
}

extension TemplateRenderer {
    class DrawableTarget {
        var lastUsedFrameIndex: UInt64
        let memorylessTargets: [(color: MTLTexture, depth: MTLTexture)]

        nonisolated init(drawable: LayerRenderer.Drawable) {
            lastUsedFrameIndex = 0
            let device = drawable.colorTextures[0].device
            nonisolated func renderTarget(resolveTexture: MTLTexture) -> MTLTexture {
                let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: resolveTexture.pixelFormat,
                    width: resolveTexture.width,
                    height: resolveTexture.height,
                    mipmapped: false)
                descriptor.usage = .renderTarget
                descriptor.textureType = .type2DMultisampleArray
                descriptor.sampleCount = device.tplRasterSampleCount
                descriptor.storageMode = .memoryless
                descriptor.arrayLength = resolveTexture.arrayLength
                return device.makeTexture(descriptor: descriptor)!
            }
            if device.tplSupportsMSAA {
                memorylessTargets = .init(
                    repeating: (renderTarget(resolveTexture: drawable.colorTextures[0]),
                                renderTarget(resolveTexture: drawable.depthTextures[0])),
                    count: tplMaxBuffersInFlight)
            } else {
                memorylessTargets = []
            }
        }
    }
}

extension TemplateRenderer.DrawableTarget {
    nonisolated func updateBufferState(uniformBufferIndex: Int, frameIndex: UInt64) {
        lastUsedFrameIndex = frameIndex
    }
}
