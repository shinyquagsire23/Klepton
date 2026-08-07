//
//  KleptonCompositor.swift — P5b, the Compositor Services half of P5.
//
//  PLANNING §12.1(3) and §12.9. What this file owns:
//
//    1. The eye textures. The guest asks OVRPlugin for eye storage
//       (ovrp_SetupEyeTexture2), kl_ovrp forwards that to kl_glfb, and
//       kl_glfb asks the registered provider for an MTLTexture. That provider
//       is `provideEyeTexture` below. The textures are OURS and outlive the
//       frame — see "why not zero-copy" below.
//    2. The frame clock. On device the deadline belongs to Compositor
//       Services, so this loop calls kl_app_frame() once per drawable rather
//       than letting the guest own a pump loop. That is why CADisplayLink was
//       never added as an interim pacer: it would be a third mechanism to
//       then remove.
//    3. The composite pass — sampling those eye textures into the drawable,
//       flipped.
//
//  **Why not zero-copy.** Handing the guest a cp_drawable's own texture would
//  avoid a copy, but drawables are per-frame while eye textures are created
//  once per (eye, stage) and reused. Timewarp needs the latter: reprojection
//  resamples an already-rendered eye image against a newer pose, so the image
//  has to still exist. Zero-copy and timewarp are mutually exclusive and
//  timewarp wins — the resampling pass reprojection needs *is* this composite
//  pass, so the copy is not actually extra work.
//
//  **The flip is not cosmetic.** GL renders bottom-up; Metal and the display
//  are top-down. The first host dump of these textures came out upside down
//  (§12.9). It is done here, in the shader, because this is the only pass that
//  reads the eye textures.
//
import SwiftUI
import CompositorServices
import Metal
import simd
import ARKit

// The layer's format has to be the guest's: Unity allocates RGBA16F eye
// textures (§12.9 measured 2198x2304 RGBA16F), and a composite that resolves
// into 8-bit would throw away the HDR range the tone map depends on.
struct KleptonStageConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .rgba16Float
        configuration.depthFormat = .depth32Float
        // Foveation is left off deliberately for now. It changes the drawable's
        // rasterization-rate map, and a composite pass that ignores that map
        // samples the wrong texels — a wrong picture rather than a slow one.
        // Worth turning on once the picture is right.
        configuration.isFoveationEnabled = false
        let layouts = capabilities.supportedLayouts(options: [])
        configuration.layout = layouts.contains(.layered) ? .layered : .dedicated
    }
}

// Compositor Services hands out `Duration`s from its own clock epoch, and
// ARKit wants a TimeInterval. There is no bridge in the SDK, so this is it.
extension Duration {
    var timeInterval: TimeInterval {
        let c = components
        return TimeInterval(c.seconds) + TimeInterval(c.attoseconds) * 1e-18
    }
}

// One (eye, stage) allocation. `slice` is which slice of `texture` the eye
// owns: both eyes share one 2-slice array, which is what the guest's own
// texture-array path expects and what `make mtltex` [Q3] proved works.
private struct EyeAllocation {
    var texture: MTLTexture
    var slice: Int
}

final class KleptonCompositor {
    private let layerRenderer: LayerRenderer
    private var device: MTLDevice!
    private var queue: MTLCommandQueue!
    private var pipeline: MTLRenderPipelineState!
    private var sampler: MTLSamplerState!

    // Keyed by (eye, stage). Indexed by stage from the start rather than kept
    // as a "last texture" global, because ovrp_GetEyeTextureStageCount is ours
    // to answer and Unity cycles stages: a pose or an image keyed to anything
    // but the stage reprojects one frame's picture with another frame's pose,
    // which looks like latency-dependent judder rather than like a bug.
    private var eyes: [Int: EyeAllocation] = [:]
    private let eyesLock = NSLock()
    private static func key(_ eye: Int, _ stage: Int) -> Int { eye &* 64 &+ stage }

    // ARKit is the pose-in seam kl_ovrp.h describes: "on visionOS it is ARKit's
    // WorldTrackingProvider answering the same call" the SDL viewer answers on
    // the host. Nothing below the seam changes — kl_ovrp still reports node 9
    // for the head and nodes 3/4 for the hands.
    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private let handTracking = HandTrackingProvider()
    private let controllers = KleptonControllers()
    private var arRunning = false

    init(_ layerRenderer: LayerRenderer) {
        self.layerRenderer = layerRenderer
    }

    // MARK: - Poses

    private func startARKit() { Task { await runARKit() } }

    /// Build the provider set and run the session.
    ///
    /// Idempotent and **re-runnable**: `AccessoryTrackingProvider` takes its
    /// accessory list at construction, so a controller connecting or
    /// disconnecting means a new provider, which means running the session
    /// again with a rebuilt list. That is not a restart — the session is not
    /// stopped first, world and hand tracking carry straight on, and this is
    /// how reconnects are meant to be handled.
    private func runARKit() async {
        do {
            // Hand tracking is requested separately because it is the half
            // that can be refused: world tracking is available to any full
            // immersive space, hand tracking needs the user's consent. A
            // refusal must leave the head working rather than take the
            // whole seam down with it.
            var providers: [any DataProvider] = [worldTracking]
            if HandTrackingProvider.isSupported { providers.append(handTracking) }
            let accessories = await controllers.makeAccessoryProvider()
            if let accessories { providers.append(accessories) }
            try await arSession.run(providers)
            arRunning = true
            NSLog("[cp] ARKit running (hands: \(HandTrackingProvider.isSupported), "
                  + "sense: \(controllers.senseConnected))")
            if let accessories {
                // A fresh anchor stream per provider. The previous one ends
                // when its provider is replaced, so these do not accumulate.
                Task { await controllers.consumeAccessoryAnchors(accessories) }
            }
        } catch {
            NSLog("[cp] ARKit failed to start: \(error) — falling back to the "
                  + "synthetic standing head kl_ovrp provides")
        }
    }

    /// Decompose a visionOS transform into the position+quaternion kl_ovrp wants.
    ///
    /// No basis change: visionOS and the Oculus tracking space share the
    /// convention the guest expects — right-handed, +Y up, -Z forward — so a
    /// flip here would be inventing a bug rather than fixing one.
    private static func decompose(_ m: simd_float4x4) -> (SIMD3<Float>, simd_quatf) {
        (SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z), simd_quatf(m))
    }

    /// Sample every pose for this frame and push it across the seam.
    ///
    /// Called once per frame, *before* kl_app_frame(), so the guest renders
    /// with this frame's pose rather than the previous one's — the same
    /// ordering reason the Choreographer is ticked before nativeRender.
    private func updatePoses(at presentationTime: TimeInterval) {
        guard arRunning else { return }

        // Predicted to the presentation time, not "now": the pose the guest
        // renders with should be the pose the display will show it at, which is
        // the whole point of having a deadline to render against.
        if let anchor = worldTracking.queryDeviceAnchor(atTimestamp: presentationTime) {
            let (p, q) = Self.decompose(anchor.originFromAnchorTransform)
            kl_ovrp_set_head_pose(p.x, p.y, p.z, q.imag.x, q.imag.y, q.imag.z, q.real)
        }

        // Hands and controllers both go through KleptonControllers, which is a
        // merge rather than a switch: a Sense controller wins per hand where it
        // is present, the wrist anchor fills in where it is not. Only a Sense
        // controller can press anything.
        // Controllers pair and unpair mid-run, and the provider list is fixed
        // at ARKitSession.run — so a changed set means rebuilding the accessory
        // provider and running the session again. Not a restart: world and hand
        // tracking carry on untouched.
        if controllers.spatialControllersChanged() {
            NSLog("[cp] spatial controller set changed — rebuilding providers")
            // The old provider's anchors are about to stop arriving; drop what
            // it left behind so a disconnected controller cannot hold its last
            // pose while the hand fallback waits behind it.
            controllers.forgetAccessoryPoses()
            Task { await runARKit() }
        }

        var left: simd_float4x4?, right: simd_float4x4?
        if HandTrackingProvider.isSupported {
            let hands = handTracking.latestAnchors
            if let a = hands.leftHand,  a.isTracked { left  = a.originFromAnchorTransform }
            if let a = hands.rightHand, a.isTracked { right = a.originFromAnchorTransform }
        }
        controllers.update(leftHand: left, rightHand: right)
    }

    // MARK: - Setup

    /// Must run before the guest asks for eye storage, because the provider is
    /// what answers that question.
    func installProvider() {
        // ANGLE's own MTLDevice, not MTLCreateSystemDefaultDevice(). The
        // EGL_ANGLE_metal_texture_client_buffer extension requires the texture
        // to belong to the display's device; a texture from another device
        // creates an EGLImage that fails at bind time, and on a single-GPU Mac
        // the two happen to be the same object, so testing on the host would
        // not catch getting this wrong (§12.9).
        guard let d = kl_glfb_mtl_device() else {
            NSLog("[cp] ANGLE has no MTLDevice yet — provider not installed")
            return
        }
        device = Unmanaged<MTLDevice>.fromOpaque(d).takeUnretainedValue() as MTLDevice
        queue = device.makeCommandQueue()
        buildPipeline()

        let ctx = Unmanaged.passUnretained(self).toOpaque()
        kl_glfb_set_mtl_provider({ (eye, stage, w, h, out, ctx) -> Int32 in
            guard let ctx, let out else { return 0 }
            let me = Unmanaged<KleptonCompositor>.fromOpaque(ctx).takeUnretainedValue()
            return me.provideEyeTexture(eye: Int(eye), stage: Int(stage),
                                        w: Int(w), h: Int(h), out: out)
        }, ctx)
        NSLog("[cp] MTL provider installed on \(device.name)")
    }

    private func buildPipeline() {
        // Compiled from source rather than a .metal file so the shader lives
        // beside the pass it serves and the project generator stays a source
        // list. It is twenty lines and compiles once.
        let src = """
        #include <metal_stdlib>
        using namespace metal;

        struct VOut { float4 pos [[position]]; float2 uv; };

        // A full-screen triangle, not a quad: three vertices, no buffer, no
        // seam down the middle where two triangles meet.
        vertex VOut kl_blit_v(uint vid [[vertex_id]]) {
            float2 p = float2((vid << 1) & 2, vid & 2);
            VOut o;
            o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
            // v is flipped here: the guest rendered this through GL, which puts
            // the origin at the BOTTOM left, and everything downstream of us is
            // top-left. Doing it in the sampler's coordinates costs nothing.
            o.uv  = float2(p.x, 1.0 - p.y);
            return o;
        }

        fragment float4 kl_blit_f(VOut in [[stage_in]],
                                  texture2d_array<float> tex [[texture(0)]],
                                  sampler samp [[sampler(0)]],
                                  constant uint &slice [[buffer(0)]]) {
            return tex.sample(samp, in.uv, slice);
        }
        """
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction   = lib.makeFunction(name: "kl_blit_v")
            desc.fragmentFunction = lib.makeFunction(name: "kl_blit_f")
            desc.colorAttachments[0].pixelFormat = .rgba16Float
            desc.depthAttachmentPixelFormat = .depth32Float
            pipeline = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("[cp] composite pipeline failed: \(error)")
            return
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sd)
    }

    // MARK: - The provider

    /// kl_glfb calls this when the guest asks for storage for one (eye, stage).
    ///
    /// Two things here are load-bearing and both were learned by running it
    /// (§12.9): the texture must be allocated on ANGLE's device, and the `w`/`h`
    /// reported back must be the texture's *real* size. The EGL extension takes
    /// the size from the MTLTexture and ignores what we claim, so a wrong-sized
    /// texture binds **successfully** and the guest renders into storage it does
    /// not have. kl_glfb refuses a mismatch rather than accept that.
    private func provideEyeTexture(eye: Int, stage: Int, w: Int, h: Int,
                                   out: UnsafeMutablePointer<kl_mtl_eye_texture>) -> Int32 {
        eyesLock.lock()
        defer { eyesLock.unlock() }

        let k = Self.key(eye, stage)
        // Unity really does re-create its eye textures at a different size
        // mid-startup — 1832x1920 during nativeRecreateGfxState, then 2198x2304
        // once the VRDevice has settled. A provider that caches one texture per
        // stage (the obvious implementation) hands back the small one, so the
        // size is part of the identity, not just the key.
        if let a = eyes[k], a.texture.width == w, a.texture.height == h {
            out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(a.texture).toOpaque(),
                                             slice: Int32(a.slice),
                                             w: Int32(a.texture.width),
                                             h: Int32(a.texture.height))
            return 1
        }

        let desc = MTLTextureDescriptor()
        desc.textureType = .type2DArray
        desc.pixelFormat = .rgba16Float
        desc.width = w; desc.height = h
        desc.arrayLength = 2                 // one slice per eye
        desc.mipmapLevelCount = 1
        // renderTarget because the guest draws into it through the EGLImage;
        // shaderRead because this file's composite pass samples it back.
        desc.usage = [.renderTarget, .shaderRead]
        desc.storageMode = .private

        // Both eyes share one array texture, so the second eye must find the
        // first eye's allocation rather than make its own.
        let partner = eyes[Self.key(eye == 0 ? 1 : 0, stage)]
        let tex: MTLTexture
        if let p = partner, p.texture.width == w, p.texture.height == h {
            tex = p.texture
        } else {
            guard let t = device.makeTexture(descriptor: desc) else {
                NSLog("[cp] could not allocate eye \(eye) stage \(stage) \(w)x\(h)")
                return 0
            }
            t.label = "klepton eye stage \(stage)"
            tex = t
            NSLog("[cp] stage \(stage): RGBA16F \(w)x\(h) array, 2 slices")
        }
        eyes[k] = EyeAllocation(texture: tex, slice: eye)
        out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(tex).toOpaque(),
                                         slice: Int32(eye),
                                         w: Int32(tex.width), h: Int32(tex.height))
        return 1
    }

    // MARK: - The frame loop

    func startRenderLoop() {
        let t = Thread { self.renderLoop() }
        t.name = "Klepton Compositor"
        // The guest blocks — Baselib futexes, IL2CPP's GC suspending the world —
        // and this thread now calls into it, so it must not be the main thread.
        t.start()
    }

    private func renderLoop() {
        layerRenderer.waitUntilRunning()
        installProvider()
        startARKit()

        // Bring the guest up to its first frame before asking it for one per
        // drawable. _begin is once per process and does the /proc report,
        // nativeRecreateGfxState, nativeResume and the first nativeRender.
        if kl_app_lifecycle_begin() != 0 {
            NSLog("[cp] lifecycle_begin failed: \(String(cString: kl_app_status()))")
            return
        }

        var presented = 0
        loop: while true {
            switch layerRenderer.state {
            case .invalidated:
                break loop
            case .paused:
                layerRenderer.waitUntilRunning()
                continue loop
            default:
                autoreleasepool { presented += renderFrame() }
            }
        }
        NSLog("[cp] render loop ended after \(presented) presented frames")
        kl_app_lifecycle_report()
    }

    private func renderFrame() -> Int {
        guard let frame = layerRenderer.queryNextFrame() else { return 0 }
        frame.startUpdate()
        frame.endUpdate()

        // Wait for the input deadline before sampling the pose, so the pose the
        // guest renders with is as late as the compositor allows. This is the
        // pacing that replaces a free-running pump.
        let timing = frame.predictTiming()
        if let timing {
            LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        }
        frame.startSubmission()
        defer { frame.endSubmission() }

        // Poses first, then the frame that uses them.
        if let timing {
            updatePoses(at: LayerRenderer.Clock.Instant.epoch.duration(to: timing.presentationTime)
                            .timeInterval)
        }

        // One guest frame per drawable: ticks the Choreographer, calls
        // nativeRender, drains the posted-task queue. The guest renders into
        // the eye textures the provider handed it.
        _ = kl_app_frame()

        guard let drawable = frame.queryDrawable() else { return 0 }

        guard let pipeline, let sampler, let cmd = queue.makeCommandBuffer() else { return 0 }

        var encoded = 0
        for (i, view) in drawable.views.enumerated() {
            let eye = i                              // view 0 = left, 1 = right
            eyesLock.lock(); let alloc = eyes[Self.key(eye, 0)]; eyesLock.unlock()
            guard let alloc else { continue }

            let pass = MTLRenderPassDescriptor()
            let map = view.textureMap
            pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
            pass.colorAttachments[0].slice       = map.sliceIndex
            pass.colorAttachments[0].loadAction  = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)
            if let depth = drawable.depthTextures.first {
                pass.depthAttachment.texture     = depth
                pass.depthAttachment.slice       = map.sliceIndex
                pass.depthAttachment.loadAction  = .clear
                pass.depthAttachment.storeAction = .dontCare
            }
            guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { continue }
            enc.label = "klepton composite eye \(eye)"
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentTexture(alloc.texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            var slice = UInt32(alloc.slice)
            enc.setFragmentBytes(&slice, length: MemoryLayout<UInt32>.size, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
            encoded += 1
        }

        drawable.encodePresent(commandBuffer: cmd)
        cmd.commit()
        return encoded > 0 ? 1 : 0
    }
}
