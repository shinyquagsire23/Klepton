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
//    3. The composite pass — which is also the reprojection pass. See
//       runtime/kl_reproject.h: the guest's picture is placed as a quad at a
//       fixed far distance, sized by the frustum it was *rendered* with, in the
//       space the pose it was rendered with defines, and then looked at from
//       where the head is now through the projection this drawable wants. That
//       corrects two things at once — the stale pose (timewarp) and the fact
//       that the guest's Quest-shaped frustum is not the Vision Pro's.
//
//  **Why not zero-copy.** Handing the guest a cp_drawable's own texture would
//  avoid a copy, but drawables are per-frame while eye textures are created
//  once per (eye, stage) and reused. Timewarp needs the latter: reprojection
//  resamples an already-rendered eye image against a newer pose, so the image
//  has to still exist. Zero-copy and timewarp are mutually exclusive and
//  timewarp wins — the resampling pass reprojection needs *is* this composite
//  pass, so the copy is not actually extra work.
//
//  **Two reprojections happen, and they are not the same one.** This pass moves
//  the guest's picture from the pose it was rendered with to the pose we
//  predict for presentation. Compositor Services then does its own correction
//  from that predicted pose to what the hardware actually reports at scanout —
//  which is what `drawable.deviceAnchor` is for, and why the anchor set below
//  is the *display* one and not the render one. Setting the render pose there
//  instead would apply our delta twice. ALVR's visionOS client splits it the
//  same way (`ALVRClient/Renderer.swift`, PLANNING §12.8).
//
//  **The flip is not cosmetic.** GL renders bottom-up; Metal and the display
//  are top-down. The first host dump of these textures came out upside down
//  (§12.9). It is done in the shared shader, because the composite is the only
//  pass that reads the eye textures.
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
        // KL_CP_FORMAT=8 selects bgra8Unorm_srgb, which is what ALVR uses on
        // this same headset. rgba16Float is what the guest renders and what the
        // tone map wants, and the drawable does come back reporting it — but a
        // format the layer ACCEPTS is not necessarily one it DISPLAYS, and that
        // distinction is untested here. Logged against the capability list so
        // the answer is in the log rather than in an argument.
        // (supportedColorFormats(options:) is visionOS 26+, so it is not asked
        // here — the drawable reports the format it actually got instead.)
        let want: MTLPixelFormat =
            ProcessInfo.processInfo.environment["KL_CP_FORMAT"] == "8"
            ? .bgra8Unorm_srgb : .rgba16Float
        NSLog("[cp] asking for colour format \(want.rawValue)")
        configuration.colorFormat = want
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
    // Optional, not implicitly unwrapped, and that is the whole fix for one
    // crash: when ANGLE has no MTLDevice this used to stay nil while the loop
    // carried on, and primeDisplay trapped on it before the guest thread was
    // even started — so the immersive path could not run at all without
    // KL_GLFB, which is exactly the configuration you want when the question
    // is about the lifecycle rather than the picture.
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState!
    private var depthState: MTLDepthStencilState?
    private var sampler: MTLSamplerState!

    // KL_CP_PROBE — the bisection ladder for a black immersive space. The pass
    // has many links and a black frame indicts all of them equally; each rung
    // removes one and keeps the rest, so whichever rung first shows something
    // names the link below it.
    //
    //   1  clear only, solid blue, no draw   does ANY drawable reach the display?
    //   2  the quad, flat magenta            does the reprojected quad land in
    //                                        the viewport at all? (geometry,
    //                                        poses, projection, 500 m depth)
    //   3  the quad, sampled, alpha := 1     is the picture arriving alpha-0?
    //   4  full-viewport blit, alpha := 1    is the eye texture lit? bypasses
    //                                        the quad, the poses and the
    //                                        projection — the viewer's own
    //                                        proven path, on device
    //
    // Read them in order: the FIRST rung that shows something is the answer,
    // and every rung above it is then known-good.
    private let probe = Int(ProcessInfo.processInfo.environment["KL_CP_PROBE"] ?? "") ?? 0
    private var probePipeline: MTLRenderPipelineState?
    /// Red, green, blue — one second each, off the wall clock. Deliberately
    /// full-intensity primaries: whatever tone mapping or colour management sits
    /// between the drawable and the display, these stay obviously themselves.
    private static func cyclingClear() -> MTLClearColor {
        switch Int(Date().timeIntervalSince1970) % 3 {
        case 0:  return MTLClearColorMake(1, 0, 0, 1)
        case 1:  return MTLClearColorMake(0, 1, 0, 1)
        default: return MTLClearColorMake(0, 0, 1, 1)
        }
    }

    private static func probeName(_ p: Int) -> String {
        switch p {
        case 1: return "clear only, solid blue, no draw"
        case 2: return "quad with flat magenta — geometry only"
        case 3: return "quad, sampled, alpha forced to 1"
        case 4: return "full-viewport blit of the eye texture, alpha forced to 1"
        default: return "unknown"
        }
    }

    // GPU ordering, not CPU. The guest's frame is in ANGLE's Metal command
    // queue and this pass is in ours, and Metal orders nothing across queues:
    // without an explicit wait the composite samples a texture ANGLE may still
    // be rendering into. That reads as intermittent tearing and is exactly the
    // kind of bug that gets blamed on the guest. kl_glfb signals this event at
    // each eglSwapBuffers (EGL_ANGLE_metal_shared_event_sync); the value is
    // also how this loop tells a fresh guest frame from a reused one.
    private var guestFrameEvent: MTLSharedEvent!
    private var lastGuestFrame: UInt64 = 0
    private var staleInARow = 0

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

    // How much work reprojection is doing, sampled for the log.
    private var reprojectFrames = 0
    private var worstDelta: Float = 0

    // KL_OVRP_QUEST_FOV=1 keeps the synthetic symmetric 90° frustum and the
    // Quest 2's 72 Hz instead of the display's own, which is the A/B if the
    // real numbers turn out to send Unity somewhere unexpected. The priming
    // pass still runs and still logs what it measured.
    private let keepQuestDisplay = klEnvOn("KL_OVRP_QUEST_FOV", default: false)

    // KL_SYNC_GUEST=1 restores P5b's shape: kl_app_frame() called inline on
    // this thread, inside the submission window. That is the clock P5.4's
    // device numbers were measured against, so it stays reachable — and it is
    // the A/B for anything that looks like a pacing problem after §12.12.
    private let syncGuest = klEnvOn("KL_SYNC_GUEST", default: false)

    // How many drawables went out before the guest had a picture to put in
    // them. Counted rather than logged per frame, because on device it is
    // legitimately hundreds and each one is uninteresting.
    private var blackFrames = 0
    private var loggedDepthRange = false
    private let noFence = klEnvOn("KL_CP_NOFENCE", default: false)
    private var cmdCommitted = 0
    private let cmdLock = NSLock()
    private var cmdCompleted = 0
    private var cmdError: Error?
    private var loggedFirstPicture = false
    private var loggedGuestEnd = false

    // Why a frame produced nothing. Compositor Services expects this loop to
    // keep presenting, and every one of these three is a path that leaves
    // WITHOUT presenting — so if the system tears the app down for not holding
    // up its end, the reason is one of these counters and nothing else in the
    // log would say so. Reported by the liveness line in renderLoop().
    private var bailNoFrame = 0
    private var bailNoDrawable = 0
    private var bailNoPipeline = 0

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
            // This is the pose the guest will render with, and kl_ovrp latches
            // it into the stage record when the guest reaches ovrp_BeginFrame —
            // which is what the composite pass reads back to know where the
            // picture it is about to show was drawn from.
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
        let angle = kl_glfb_mtl_device()
        if let d = angle {
            device = Unmanaged<MTLDevice>.fromOpaque(d).takeUnretainedValue() as MTLDevice
        } else {
            // No ANGLE: the null GL driver, i.e. a run asking about the
            // lifecycle rather than the picture. There will be no eye textures
            // to composite — but this loop still has to hold up its end of the
            // bargain, which is to prime the display, present cleared drawables
            // and keep publishing poses to the guest. Its own device is enough
            // for all three, and *not* taking one here is what used to kill the
            // whole compositor in primeDisplay.
            guard let d = MTLCreateSystemDefaultDevice() else {
                NSLog("[cp] no MTLDevice at all — the compositor cannot run")
                return
            }
            device = d
        }
        queue = device.makeCommandQueue()
        buildPipeline()

        guard angle != nil else {
            NSLog("[cp] ANGLE has no MTLDevice — no eye textures this run "
                  + "(KL_GLFB unset?); drawables will be cleared and presented, "
                  + "and the guest still gets its frames")
            return
        }

        // Register the shared event before the guest can swap, so no frame is
        // ever composited without the wait.
        if let ev = device.makeSharedEvent() {
            guestFrameEvent = ev
            kl_glfb_set_gpu_fence(Unmanaged.passUnretained(ev).toOpaque())
        } else {
            NSLog("[cp] no MTLSharedEvent — the composite cannot be ordered "
                  + "against ANGLE's queue")
        }

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
        // The shader is kl_reproject.c's, shared verbatim with the macOS
        // viewer's compositor (runtime/kl_view_mtl.m) — one copy of the flip,
        // the quad and the array-slice sampler, because having them diverge
        // means debugging the picture twice, and the viewer is where this math
        // can actually be run before a device is available. Compiled from
        // source rather than shipped as a .metal so it stays beside the
        // matrices that feed it (kl_reproject_build).
        let src = String(cString: kl_reproject_msl())
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
            desc.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
            desc.colorAttachments[0].pixelFormat = .rgba16Float
            desc.depthAttachmentPixelFormat = .depth32Float
            pipeline = try device.makeRenderPipelineState(descriptor: desc)

            // Depth WRITES, which this pass never did. visionOS reprojects the
            // submitted frame using its depth buffer, so a pass that writes no
            // depth submits perfect colour with the geometry "nothing is here at
            // any finite distance" — and the compositor honours that literally.
            // Measured on device: a fullscreen quad at reverse-Z 0 with writes
            // off is invisible; the same quad at 0.05 (2 m) with writes on
            // appears. .greater matches the reverse-Z clear of 0.
            let dsd = MTLDepthStencilDescriptor()
            dsd.depthCompareFunction = .greater
            dsd.isDepthWriteEnabled = true
            depthState = device.makeDepthStencilState(descriptor: dsd)

            // KL_CP_PROBE: swap ONE thing about the pass and see if the black
            // moves. Each rung isolates a different link in the chain, so a
            // single device run localises a dark picture instead of a session
            // of one-hypothesis-per-build. See kl_reproject.c for the shaders.
            if probe > 0 {
                let d = MTLRenderPipelineDescriptor()
                d.colorAttachments[0].pixelFormat = .rgba16Float
                d.depthAttachmentPixelFormat = .depth32Float
                switch probe {
                case 2:  d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_solid_f")
                case 3:  d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_opaque_f")
                case 4:  d.vertexFunction   = lib.makeFunction(name: "kl_probe_full_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_probe_full_f")
                default: d.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
                         d.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
                }
                if probe >= 2 && probe <= 4 {
                    probePipeline = try? device.makeRenderPipelineState(descriptor: d)
                }
                NSLog("[cp] PROBE \(probe): \(Self.probeName(probe)) — "
                      + "this is a DIAGNOSTIC frame, not the real pass")
            }
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

    /// KL_CP_PROBE=9 — the floor. Apple's template minus the cube: wait for the
    /// layer, then nothing but query / update / drawable / submit / clear /
    /// present, forever. No ARKit, no priming pass, no eye-texture provider, no
    /// guest, no shared-event wait, no pipeline, no poses, no anchor.
    ///
    /// Every rung above this one has come back clean while the display stayed
    /// black, which means the thing being bisected is not a feature of the pass.
    /// This establishes whether THIS APP can put any colour on THIS display at
    /// all. Blue here and black at rung 1 localises the fault to what rung 1
    /// still has that this does not; black here says the fault is structural —
    /// the scene, the configuration, or the layer — and nothing in renderFrame()
    /// was ever going to fix it.
    private func minimalLoop() {
        NSLog("[cp] PROBE 9: MINIMAL LOOP — clear to blue, present, nothing else")
        var n = 0
        var next = Date()
        while true {
            if layerRenderer.state == .invalidated { break }
            if layerRenderer.state == .paused { layerRenderer.waitUntilRunning(); continue }
            guard let frame = layerRenderer.queryNextFrame() else { continue }
            frame.startUpdate()
            frame.endUpdate()
            if let t = frame.predictTiming() {
                LayerRenderer.Clock().wait(until: t.optimalInputTime)
            }
            guard let drawable = frame.queryDrawable() else { continue }
            frame.startSubmission()
            // deviceAnchor is deliberately LEFT ALONE. Stripping it would be
            // stripping something already measured as working (anchor=set), and
            // the system uses it to place and reproject the frame — so a black
            // result would no longer distinguish "structural fault" from "the
            // probe broke it". A floor test may remove only what is suspect.
            if let cmd = queue?.makeCommandBuffer() {
                for view in drawable.views {
                    let map = view.textureMap
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
                    pass.colorAttachments[0].slice       = map.sliceIndex
                    pass.colorAttachments[0].loadAction  = .clear
                    pass.colorAttachments[0].storeAction = .store
                    pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 1, 1)
                    cmd.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
                }
                drawable.encodePresent(commandBuffer: cmd)
                cmd.commit()
            }
            frame.endSubmission()
            n += 1
            if Date() >= next {
                next = Date().addingTimeInterval(2)
                NSLog("[cp] minimal: \(n) frames presented, state=\(String(describing: layerRenderer.state))")
            }
        }
        NSLog("[cp] minimal loop ended after \(n) frames")
    }

    private func renderLoop() {
        layerRenderer.waitUntilRunning()
        if probe == 9 {
            // The system default device, exactly as Apple's template uses —
            // not ANGLE's. Nothing here samples a guest texture, so there is no
            // reason to involve the device that owns them, and every reason not
            // to: it is one of the things being ruled out.
            queue = MTLCreateSystemDefaultDevice()?.makeCommandQueue()
            minimalLoop()
            return
        }
        installProvider()
        startARKit()

        // Before the guest, not after: the frustum and the display rate can
        // only be read from a drawable, and the guest reads both exactly once
        // during the lifecycle below. See primeDisplay().
        primeDisplay()

        // Bring the guest up, on its own thread — PLANNING §12.12. _begin is
        // once per process and does the /proc report, nativeRecreateGfxState,
        // nativeResume and the first nativeRender, and it happens over there
        // rather than here: this loop must be free to present black while the
        // guest is still starting, which can take tens of seconds on device.
        //
        // Under KL_SYNC_GUEST the old inline shape is kept instead, because
        // P5.4's device measurement was taken against it and the device has
        // never been run with either. It is the A/B for anything that looks
        // like a pacing regression.
        if syncGuest {
            NSLog("[cp] KL_SYNC_GUEST — driving the guest inline on this thread")
            if kl_app_lifecycle_begin() != 0 {
                NSLog("[cp] lifecycle_begin failed: \(String(cString: kl_app_status()))")
                return
            }
        } else if kl_app_guest_start() != 0 {
            NSLog("[cp] guest thread failed to start: \(String(cString: kl_app_status()))")
            return
        }

        // This loop's own liveness, logged on a wall clock rather than a frame
        // count. The device kills an immersive app that stops presenting, and
        // that kill arrives as a bare SIGKILL with no crash report at all — so
        // "was this loop still running, and was it producing anything" is a
        // question the log has to be able to answer after the fact. A frame
        // counter cannot answer it: a loop that has stopped iterating stops
        // incrementing, and the absence of a line is exactly what a healthy
        // quiet loop looks like too.
        var presented = 0
        var iterations = 0
        var nextReport = Date()
        loop: while true {
            switch layerRenderer.state {
            case .invalidated:
                break loop
            case .paused:
                NSLog("[cp] layer paused at iteration \(iterations) — waiting")
                layerRenderer.waitUntilRunning()
                NSLog("[cp] layer running again")
                continue loop
            default:
                autoreleasepool { presented += renderFrame() }
            }
            iterations += 1
            if Date() >= nextReport {
                nextReport = Date().addingTimeInterval(2)
                cmdLock.lock(); let done = cmdCompleted; cmdLock.unlock()
                NSLog("[cp] alive: \(iterations) iters, \(presented) with a picture, "
                      + "\(blackFrames) black, guest=\(kl_app_guest_state()), "
                      + "bail(frame/drawable/pipeline)="
                      + "\(bailNoFrame)/\(bailNoDrawable)/\(bailNoPipeline), "
                      // committed vs completed: if these diverge and stay
                      // diverged, the GPU is waiting on the guest's fence and
                      // nothing is being drawn at all.
                      + "cmdbuf \(done)/\(cmdCommitted) done"
                      + (noFence ? " (NOFENCE)" : ""))
            }
        }
        NSLog("[cp] render loop ended after \(presented) presented frames, "
              + "\(iterations) iterations, state=\(String(describing: layerRenderer.state))")
        // Ends the guest's loop and joins it, so its report — which is the
        // P5.4 report, and belongs to the guest's end of the run rather than
        // to this one — has been written before this returns.
        if syncGuest { kl_app_lifecycle_report() } else { kl_app_guest_stop() }
    }

    private func renderFrame() -> Int {
        guard let frame = layerRenderer.queryNextFrame() else { bailNoFrame += 1; return 0 }
        frame.startUpdate()
        frame.endUpdate()

        // Wait for the input deadline before sampling the pose, so the pose the
        // guest renders with is as late as the compositor allows. This is the
        // pacing that replaces a free-running pump.
        let timing = frame.predictTiming()
        if let timing {
            LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        }
        // NOT `defer { frame.endSubmission() }`. Compositor Services treats
        // ending a submission that never presented a drawable as a client bug
        // and aborts the process:
        //
        //   __BUG_IN_CLIENT__  <- cp_frame_end_submission
        //                      <- $defer #1 in renderFrame()
        //
        // — SIGABRT on the compositor thread, 2026-08-07 on device, after
        // thousands of healthy frames. A defer is exactly wrong here: the whole
        // point of the early returns below is that this frame will NOT be
        // presented, and those are the frames that must not be ended. ALVR's
        // Renderer.swift and Apple's own template both simply abandon the frame
        // — no endSubmission on that path — and this now does the same.
        // ...and it is started AFTER the drawable is in hand, below — which is
        // the order Apple's template and ALVR both use:
        //
        //   queryNextFrame -> startUpdate/endUpdate -> predictTiming
        //     -> wait(optimalInputTime) -> queryDrawable -> startSubmission
        //     -> encode -> encodePresent -> commit -> endSubmission
        //
        // This used to call startSubmission here, before queryDrawable. That
        // also makes the two bail paths below abandon a submission they had
        // already started, which is the same shape as trap 14 from the other
        // side.

        // Poses first, then the frame that uses them.
        if let timing {
            updatePoses(at: LayerRenderer.Clock.Instant.epoch.duration(to: timing.presentationTime)
                            .timeInterval)
        }

        // Offer the guest this frame's pose and carry straight on — PLANNING
        // §12.12. The guest wakes on its own thread, ticks the Choreographer,
        // calls nativeRender and renders into the eye textures the provider
        // handed it, and this loop does not wait for any of it. A guest that
        // keeps up finishes before the next publish and behaves exactly as the
        // synchronous version did; a guest that does not simply leaves the
        // stage record where it was, and the composite below reprojects the
        // picture already there against the newer pose. That case is the whole
        // reason the pass exists, and it was unreachable while the thing being
        // reprojected was the thing blocking this loop.
        if syncGuest {
            _ = kl_app_frame()
        } else {
            kl_app_guest_publish()
            // A guest that has *ended* looks exactly like a guest that is
            // merely slow — both leave the stage record where it was and both
            // make `stale` climb. Say which, once, or the log below cannot be
            // read: the same numbers mean "reprojection is earning its keep"
            // in one case and "there is nothing left to reproject" in the other.
            if !loggedGuestEnd && kl_app_guest_state() < 0 {
                loggedGuestEnd = true
                NSLog("[cp] the guest thread has ended (\(String(cString: kl_app_status())))"
                      + " — everything from here is its last picture, reprojected")
            }
        }

        guard let drawable = frame.queryDrawable() else { bailNoDrawable += 1; return 0 }
        frame.startSubmission()
        guard let pipeline, let sampler, let cmd = queue?.makeCommandBuffer() else {
            bailNoPipeline += 1
            return 0
        }

        // Which stage holds a *finished* picture. -1 means the guest has not
        // completed a frame yet (ovrp_EndFrame never seen) — there is nothing
        // to show, but the drawable still has to be presented or Compositor
        // Services stalls waiting for it.
        //
        // Sampling stage 0 anyway would be worse than showing black: with the
        // guest decoupled it may well be *mid-draw* into exactly that texture,
        // so the frames before the first EndFrame would be a torn picture
        // rather than an empty one, and reprojected against no pose at all.
        let completeStage = Int(kl_ovrp_last_complete_stage())
        let stage = max(0, completeStage)
        var rendered = kl_ovrp_render_pose()
        var haveRendered = withUnsafeMutablePointer(to: &rendered) {
            kl_ovrp_stage_render_pose(Int32(stage), $0) != 0
        }
        // `complete` is redundant today and will not be: with one stage, the
        // guest's *next* ovrp_BeginFrame overwrites the record this one is
        // reading, so the moment the guest stops being driven synchronously
        // from this thread a record can describe a frame still being drawn.
        // Reprojecting against a pose whose picture does not exist yet is a
        // silent wrong answer, so it is checked now rather than after the
        // change that makes it possible.
        if haveRendered && rendered.complete == 0 { haveRendered = false }

        // Order this pass after the guest's rendering. The value only advances
        // when the guest swaps, so an unchanged one also means "no new picture"
        // — which is not an error, it is the case reprojection is for.
        let frameValue = kl_glfb_gpu_fence_value()
        // KL_CP_NOFENCE=1 skips the wait. A composite command buffer that waits
        // on an event value the guest's queue never signals is COMMITTED and
        // never EXECUTES: the drawable is presented, every counter here looks
        // healthy, and the display shows nothing. That failure is invisible
        // from this side, so it needs its own A/B — and cmdCompleted below is
        // the measurement that says whether it is happening.
        if let ev = guestFrameEvent, frameValue != 0, !noFence {
            cmd.encodeWaitForEvent(ev, value: frameValue)
        }
        if frameValue != 0 && frameValue == lastGuestFrame {
            staleInARow += 1
        } else {
            staleInARow = 0
            lastGuestFrame = frameValue
        }

        // The pose the picture will be *shown* at, as freshly predicted as we
        // can make it: re-queried here rather than reused from updatePoses,
        // because the guest's render has happened in between and the prediction
        // is that much better for it. This is what the pass reprojects to, and
        // therefore also what the drawable is told it was rendered with.
        let presentation = LayerRenderer.Clock.Instant.epoch
            .duration(to: drawable.frameTiming.presentationTime).timeInterval
        let displayAnchor = worldTracking.queryDeviceAnchor(atTimestamp: presentation)
        drawable.deviceAnchor = displayAnchor
        let originFromDevice = displayAnchor?.originFromAnchorTransform ?? matrix_identity_float4x4

        noteReprojection(rendered: haveRendered ? rendered : nil,
                         originFromDevice: originFromDevice, stage: stage)

        // The drawable's depth range, once. This is not curiosity: the quad is
        // placed at KL_REPROJECT_DEPTH metres (500 by default, so the eye offset
        // is negligible), and if that is beyond the far plane the compositor
        // chose, every corner is clipped and the display is black with a pass
        // that ran perfectly. ALVR places its own panel at 1 m. Logged rather
        // than assumed because it is the compositor's number, not ours.
        if !loggedDepthRange {
            loggedDepthRange = true
            let r = drawable.depthRange
            NSLog("[cp] drawable depthRange = \(r) (x=far, y=near in reverse-Z); "
                  + "quad at \(ProcessInfo.processInfo.environment["KL_REPROJECT_DEPTH"] ?? "500") m")
            // The drawable's actual shape, once. A clear fills the whole
            // attachment whatever the viewport says, so a blue clear that is
            // not visible means the texture being cleared is not the texture
            // being displayed — and these are the numbers that say which.
            NSLog("[cp] drawable: \(drawable.colorTextures.count) color texture(s), "
                  + "\(drawable.views.count) view(s), anchor="
                  + (drawable.deviceAnchor == nil ? "nil" : "set"))
            // Whose device owns what. installProvider() takes ANGLE's MTLDevice
            // so the eye textures can be shared with the guest, and its own
            // comment notes that on the host ANGLE's device and the system
            // default "happen to be the same object, so testing on the host
            // would not catch getting this wrong". Compositor Services allocates
            // the drawable on ITS device. If those are two different objects,
            // every render pass here attaches textures the command queue does
            // not own — which is not a picture that comes out wrong, it is a
            // picture that never happens.
            if let dt = drawable.colorTextures.first?.device {
                let ours = queue?.device
                NSLog("[cp] devices: queue=\(ours?.name ?? "nil")/"
                      + "\(ours.map { String($0.registryID) } ?? "-") "
                      + "drawable=\(dt.name)/\(dt.registryID) "
                      + (ours === dt ? "SAME OBJECT" : "*** DIFFERENT OBJECTS ***"))
            }
            for (i, t) in drawable.colorTextures.enumerated() {
                NSLog("[cp]   color[\(i)] \(t.width)x\(t.height) "
                      + "array=\(t.arrayLength) type=\(t.textureType.rawValue) "
                      + "fmt=\(t.pixelFormat.rawValue)")
            }
            for (i, v) in drawable.views.enumerated() {
                let m = v.textureMap
                NSLog("[cp]   view[\(i)] -> texture \(m.textureIndex) slice "
                      + "\(m.sliceIndex) viewport \(m.viewport)")
            }
        }

        var encoded = 0
        for (i, view) in drawable.views.enumerated() {
            let eye = i                              // view 0 = left, 1 = right
            eyesLock.lock(); let a = eyes[Self.key(eye, stage)]; eyesLock.unlock()
            let alloc = completeStage < 0 ? nil : a

            let pass = MTLRenderPassDescriptor()
            let map = view.textureMap
            pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
            pass.colorAttachments[0].slice       = map.sliceIndex
            pass.colorAttachments[0].loadAction  = .clear
            pass.colorAttachments[0].storeAction = .store
            // Rung 1 clears to blue instead of black and draws nothing at all:
            // if the display stays black through THAT, nothing this file draws
            // was ever going to be seen and the fault is below the pass.
            // Rung 1's colour CYCLES — red, green, blue, one second each — and
            // that is the point. A static colour cannot distinguish "our clear
            // is reaching the display" from "the display is showing black for
            // its own reasons", because both look like a constant field. A
            // colour that changes on a known period can: if it cycles, our
            // content is being composited and the question moves to what we
            // draw; if it is a steady anything, it is not ours at all.
            pass.colorAttachments[0].clearColor  = probe == 1
                ? Self.cyclingClear() : MTLClearColorMake(0, 0, 0, 1)
            if let depth = drawable.depthTextures.first {
                pass.depthAttachment.texture     = depth
                pass.depthAttachment.slice       = map.sliceIndex
                pass.depthAttachment.loadAction  = .clear
                // .store, NOT .dontCare. visionOS reprojects the submitted frame
                // using its depth buffer, so throwing the depth away hands the
                // compositor a colour image with no geometry — and a reverse-Z
                // clear of 0 reads as "everything is infinitely far away".
                // .dontCare here meant the quad's colour was correct and its
                // depth said nothing was there.
                pass.depthAttachment.storeAction = .store
                pass.depthAttachment.clearDepth  = 0.0
            }
            guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { continue }
            enc.label = "klepton composite eye \(eye)"
            if probe == 1 { enc.endEncoding(); encoded += 1; continue }
            // No eye texture yet — the guest is still in _begin, or it has not
            // reached ovrp_SetupEyeTexture2 for this stage. The encoder is
            // still made and still ended, because the clear above is the point:
            // a drawable that is presented without being written shows whatever
            // was in that texture last, and with the guest now taking its own
            // time to start there are a great many such frames.
            guard let alloc else { enc.endEncoding(); continue }

            // The viewport matters here in a way it did not for a full-screen
            // triangle: the quad is projected, so it lands where the projection
            // says, and the view's own bounds within a shared texture are part
            // of that.
            enc.setViewport(map.viewport)
            enc.setRenderPipelineState(probePipeline ?? pipeline)
            if let depthState { enc.setDepthStencilState(depthState) }
            enc.setFragmentTexture(alloc.texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)

            var u = withUnsafePointer(to: rendered) { r in
                kl_reproject_build(haveRendered ? r : nil, Int32(eye),
                                   originFromDevice, view.transform,
                                   drawable.computeProjection(viewIndex: i),
                                   UInt32(alloc.slice))
            }
            enc.setVertexBytes(&u, length: MemoryLayout<kl_reproject_uniforms>.size, index: 0)
            enc.setFragmentBytes(&u, length: MemoryLayout<kl_reproject_uniforms>.size, index: 0)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            enc.endEncoding()
            encoded += 1
        }

        // The one thing worth saying out loud about the decoupling: how long
        // the guest took to put anything in a drawable. Under the old inline
        // shape this could not be measured, because the loop did not run until
        // the guest had already produced its first frame.
        if encoded == 0 {
            blackFrames += 1
        } else if !loggedFirstPicture {
            loggedFirstPicture = true
            NSLog("[cp] first guest picture composited, after \(blackFrames) black frames")
        }

        // Did this command buffer actually RUN? "Presented" only means committed,
        // and a buffer stalled on a shared-event wait never executes — so a
        // black display and a perfect frame count are the same picture from
        // here. Counted, with the first error printed, because "committed but
        // never completed" is otherwise indistinguishable from "completed and
        // drew black".
        cmd.addCompletedHandler { [weak self] b in
            guard let self else { return }
            self.cmdLock.lock()
            self.cmdCompleted += 1
            let first = self.cmdError == nil && b.error != nil
            if first { self.cmdError = b.error }
            self.cmdLock.unlock()
            if first { NSLog("[cp] composite command buffer error: \(b.error!)") }
        }
        drawable.encodePresent(commandBuffer: cmd)
        cmd.commit()
        cmdCommitted += 1
        frame.endSubmission()      // only here — see startSubmission above
        return encoded > 0 ? 1 : 0
    }

    /// Harvest what only a drawable can tell us, *before* the guest exists to
    /// ask — the per-eye frustum and the display's real frame rate.
    ///
    /// **This exists because of an ordering problem, and the solution is
    /// ALVR's.** Both numbers can only be read from a `cp_drawable`, and a
    /// drawable only arrives once the frame loop is running — but the guest
    /// reads both *once*, early, and stores them: Unity latches the frustum
    /// when its VRDevice initializes and the display frequency straight into
    /// its VR timing config. Answering correctly one frame later is answering a
    /// question nobody will ask again.
    ///
    /// `ALVRClient/DummyMetalRenderer.swift` solves it by rendering throwaway
    /// frames purely to harvest `views[].transform` and the tangents, setting
    /// `haveRenderInfo`, and only then starting the real renderer — ALVR has to
    /// do it in a whole separate ImmersiveSpace opened first
    /// (`Entry/EntryControls.swift` opens it, waits for the flag, closes it),
    /// because its renderer needs the info before its own layer exists. We own
    /// this loop and the guest has not been driven yet, so the same mechanism
    /// fits inline: prime here, then `kl_app_lifecycle_begin()`. One fewer
    /// space, same trick.
    ///
    /// Tangents come from the projection matrix rather than `cp_view_tangents`,
    /// which visionOS 2.0 deprecated — and the matrix is the more honest source
    /// anyway, since it is what the composite pass projects with.
    private func primeDisplay() {
        var stamps: [TimeInterval] = []
        var tangents: [SIMD4<Float>] = []
        // Eight frames: enough for a median frame interval that a slow first
        // frame cannot drag, and under 100 ms of startup.
        for _ in 0..<8 {
            guard layerRenderer.state == .running,
                  let frame = layerRenderer.queryNextFrame() else { break }
            frame.startUpdate()
            frame.endUpdate()
            let timing = frame.predictTiming()
            if let timing { LayerRenderer.Clock().wait(until: timing.optimalInputTime) }
            // Drawable first, THEN startSubmission — the documented order, and
            // the same correction as renderFrame(). These are the layer's first
            // eight frames, so getting the sequence wrong here is not a local
            // matter: it is the state the whole session starts from.
            guard let drawable = frame.queryDrawable() else { continue }
            frame.startSubmission()
            stamps.append(LayerRenderer.Clock.Instant.epoch
                .duration(to: drawable.frameTiming.presentationTime).timeInterval)
            if tangents.isEmpty {
                tangents = (0..<drawable.views.count).map {
                    kl_reproject_tangents(drawable.computeProjection(viewIndex: $0))
                }
            }
            clearAndPresent(drawable)
            // Only after a drawable was actually presented — trap 14. The old
            // shape ended the submission even on the no-drawable path, which is
            // the abort waiting to happen rather than the one already seen.
            frame.endSubmission()
        }

        for (i, t) in tangents.enumerated() where i < 2 {
            NSLog(String(format: "[cp] eye %d display tangents l=%.4f r=%.4f t=%.4f b=%.4f "
                                 + "(%.1f x %.1f degrees)", i, t.x, t.y, t.z, t.w,
                         (atan(t.x) + atan(t.y)) * 180 / .pi,
                         (atan(t.z) + atan(t.w)) * 180 / .pi))
            if !keepQuestDisplay { kl_ovrp_set_eye_frustum(Int32(i), t.x, t.y, t.z, t.w) }
        }

        // The frame interval, as the median of the gaps between successive
        // presentation times. Median rather than mean because the first frames
        // of a freshly-started layer are not representative and one of them is
        // enough to move an average by several Hz.
        let gaps = zip(stamps.dropFirst(), stamps).map(-).filter { $0 > 0 }.sorted()
        if let mid = gaps.isEmpty ? nil : gaps[gaps.count / 2], mid > 0 {
            let hz = Float(1.0 / mid)
            NSLog(String(format: "[cp] display %.2f Hz measured over %d frames%@",
                         hz, gaps.count,
                         keepQuestDisplay ? " (not pushed: KL_OVRP_QUEST_FOV)" : ""))
            if !keepQuestDisplay { kl_ovrp_set_display_frequency(hz) }
        } else {
            NSLog("[cp] could not measure the display rate — the guest keeps "
                  + "\(kl_ovrp_display_frequency()) Hz")
        }
        NSLog(String(format: "[cp] guest will be told %.2f Hz",
                     kl_ovrp_display_frequency()))
    }

    /// A drawable with nothing in it. The priming frames are shown to the user,
    /// so they have to be *something*; an unwritten drawable is whatever was in
    /// that texture last.
    private func clearAndPresent(_ drawable: LayerRenderer.Drawable) {
        guard let cmd = queue?.makeCommandBuffer() else { return }
        for view in drawable.views {
            let map = view.textureMap
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture     = drawable.colorTextures[map.textureIndex]
            pass.colorAttachments[0].slice       = map.sliceIndex
            pass.colorAttachments[0].loadAction  = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1)
            cmd.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
        drawable.encodePresent(commandBuffer: cmd)
        cmd.commit()
    }

    /// The measurement that says whether any of this is doing anything.
    ///
    /// `delta` is how far the head turned between the pose the picture was
    /// rendered with and the pose it is being displayed at — zero means the
    /// guest is keeping up and the pass is a blit, and a growing value with
    /// `stale` frames behind it means pictures are being reused, which is
    /// reprojection earning its place rather than a fault.
    private func noteReprojection(rendered: kl_ovrp_render_pose?,
                                  originFromDevice: simd_float4x4, stage: Int) {
        reprojectFrames += 1
        var delta: Float = 0
        if var r = rendered {
            delta = withUnsafePointer(to: &r) {
                kl_reproject_delta_degrees($0, originFromDevice)
            }
        }
        worstDelta = max(worstDelta, delta)
        guard reprojectFrames % 90 == 0 else { return }
        NSLog(String(format: "[cp] timewarp: stage %d serial %llu, delta %.2f deg "
                             + "(worst %.2f), %d stale in a row",
                     stage, rendered?.serial ?? 0, delta, worstDelta, staleInARow))
        worstDelta = 0
    }
}
