//
//  KleptonCompositor.swift — the Compositor Services half of the eye seam.
//
//  What this file owns:
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
//       runtime/gfx/kl_reproject.h: the guest's picture is placed as a quad at a
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
//  same way (`ALVRClient/Renderer.swift`).
//
//  **The flip is not cosmetic.** GL renders bottom-up; Metal and the display
//  are top-down. The first host dump of these textures came out upside down
//. It is done in the shared shader, because the composite is the only
//  pass that reads the eye textures.
//
import SwiftUI
import CompositorServices
import Metal
import simd
import ARKit

// What the drawable costs, and therefore how much of the display we can afford
// to actually use. Three settings here decide that, and they are entangled:
//
//   * **8-bit sRGB colour, not RGBA16F.** The eye textures the guest renders
//     into stay RGBA16F — that is where Unity's HDR range lives and nothing
//     about it changes. What changes is the *drawable* this pass resolves into,
//     which is the last step before the display and holds a tone-mapped picture
//     with no headroom left to preserve. Halving its bytes per pixel halves the
//     bandwidth of every composite, and `bgra8Unorm_srgb` encodes on write, so
//     the linear values the shader produces are converted by the hardware
//     rather than thrown away. This is ALVR's choice on the same headset.
//     KL_CP_FORMAT=16 is the A/B back to rgba16Float.
//   * **Foveation.** The drawable is then rasterized through a variable rate
//     map: dense where the eye looks, sparse at the periphery. The composite
//     pass MUST set that map on its render pass — a pass that ignores it fills
//     the physical texture as if it were uniform, and the system's unwarp then
//     shows a distorted picture. That is why this was off until now, and it is
//     the reason for the amplified single-pass composite below.
//   * **maxRenderQuality = 1.0.** With foveation on, this is what buys the
//     resolution: ALVR measures the drawable going from 1888x1792 physical /
//     4338x3478 screen at the default to 2496x2432 / 6262x5020 at 1.0. It
//     costs memory (it is billed to the app) and per-frame GPU time, which is
//     why the ceiling and the *running* quality are two separate settings —
//     see `applyRenderQuality`.
//
// KL_CP_FOVEATION=0 turns the last two off together, because the quality knob
// is only accepted alongside foveation.
struct KleptonStageConfiguration: CompositorLayerConfiguration {
    /// The render quality to ask for, 0..1. Also read by the render loop, which
    /// has to set the *running* quality separately.
    static var requestedQuality: Float {
        let e = ProcessInfo.processInfo.environment["KL_CP_QUALITY"]
        let v = e.flatMap { Float($0) } ?? 1.0
        return min(max(v, 0.0), 1.0)
    }
    static var wantFoveation: Bool { klEnvOn("KL_CP_FOVEATION", default: true) }

    /// Whether `maxRenderQuality` was actually configured on the layer.
    ///
    /// Not the same question as `wantFoveation`, and the difference is a crash.
    /// The ceiling is only set when foveation is genuinely available; the
    /// *running* quality is set later by `applyRenderQuality`, and asking for a
    /// quality above a ceiling that was never configured is a configuration
    /// error, which Compositor Services reports as `__BUG_IN_CLIENT__` inside
    /// `cp_layer_renderer_set_render_quality` — an abort with no mention of
    /// either setting. The visionOS simulator is where the two diverge: it
    /// supports no foveation at all and reports its default render quality as
    /// **NaN**, so every immersive run there died on the second setting.
    nonisolated(unsafe) static var ceilingConfigured = false

    /// Foveation is only honourable with vertex amplification, so it is only
    /// asked for when the GPU has it.
    ///
    /// A foveated drawable's rate map has one layer per eye and Metal picks the
    /// layer by render-target array index, which only an amplified single pass
    /// can address (see `encodeComposite`). Without amplification the composite
    /// would have to ignore the map, and a pass that ignores the map does not
    /// render *slower*, it renders a **distorted picture** — the system unwarps
    /// something that was never warped. So the honest configuration on such a
    /// device is no foveation at all, decided here rather than discovered three
    /// layers down. Every Apple silicon GPU reports 2; this is a guard, not an
    /// expected path.
    static var canAmplify: Bool {
        MTLCreateSystemDefaultDevice()?.supportsVertexAmplificationCount(2) ?? false
    }

    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // (supportedColorFormats(options:) is visionOS 26+ and deprecated in
        // the same release, so it is not asked here — the drawable reports the
        // format it actually got instead, in the one-time log below.)
        let want: MTLPixelFormat =
            ProcessInfo.processInfo.environment["KL_CP_FORMAT"] == "16"
            ? .rgba16Float : .bgra8Unorm_srgb
        configuration.colorFormat = want
        configuration.depthFormat = .depth32Float

        // KL_CP_AMPLIFY=0 takes foveation down with it: without an amplified
        // single pass the rate map cannot be honoured (see encodeComposite),
        // so one knob restores the entire pre-session composite rather than
        // half of it.
        let amplify = Self.canAmplify && klEnvOn("KL_CP_AMPLIFY", default: true)
        let foveate = capabilities.supportsFoveation && Self.wantFoveation && amplify
        configuration.isFoveationEnabled = foveate
        if !amplify && capabilities.supportsFoveation && Self.wantFoveation {
            NSLog("[cp] foveation refused: this GPU has no vertex amplification, "
                  + "so the composite could not honour the rate map")
        }
        let quality = Self.requestedQuality
        if foveate {
            // Gated on foveation deliberately, as ALVR gates it: an
            // unsupported render quality is a *configuration error*, which
            // fails the whole layer rather than degrading, and there is no
            // point paying for resolution the rate map is not there to spend.
            configuration.maxRenderQuality = .init(quality)
        }
        Self.ceilingConfigured = foveate
        NSLog("[cp] colour format \(want.rawValue), foveation \(foveate) "
              + "(supported \(capabilities.supportsFoveation)), "
              + "max render quality \(foveate ? String(quality) : "default") "
              + "(device default \(capabilities.defaultRenderQuality))")

        // The layout query has to be told about foveation too — the supported
        // layouts are not the same set with a rate map in play.
        let layouts = capabilities.supportedLayouts(options: foveate ? [.foveationEnabled] : [])
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
    // 1 if the guest drew this picture with its origin at the TOP left — i.e.
    // with Vulkan rather than GL. Recorded per texture rather than decided once
    // (kl_reproject.h): getting it wrong is a correct-looking picture that is
    // upside down, and nothing in Metal, Compositor Services or the guest can
    // report it.
    var flipY: Bool = false
}

/// GL internal format -> MTLPixelFormat, for the eye textures the guest asks
/// for storage for.
///
/// **This is a requirement, not a preference.** ANGLE compares the GL internal
/// format we claim against the MTLTexture's own pixel format and refuses the
/// pair — `TextureImageSiblingMtl::ValidateClientBuffer`, "Incompatible format"
/// — so a provider hardwired to RGBA16F cannot back a guest that asked for
/// anything else. Unity asks for RGBA16F through OVRPlugin; Steam Link's OpenXR
/// swapchains ask for SRGB8_ALPHA8, which is what put this here. `nil` means
/// decline, and the guest then gets ordinary GL storage of the right format —
/// black in the headset, which is visible, rather than colour that is subtly
/// wrong, which is not.
private func klEyePixelFormat(_ glInternal: UInt32) -> MTLPixelFormat? {
    switch glInternal {
    case 0x881A: return .rgba16Float        // GL_RGBA16F
    case 0x8C43: return .rgba8Unorm_srgb    // GL_SRGB8_ALPHA8
    case 0x8058: return .rgba8Unorm         // GL_RGBA8
    default:     return nil
    }
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
    /// Always-pass, no write — for coplanar draws after the first. See where
    /// `depthState` is built for why the two cannot be one.
    private var depthPassState: MTLDepthStencilState?
    private var sampler: MTLSamplerState!
    /// Views this GPU can draw in one amplified pass. 2 everywhere real; 1
    /// forces the old per-view passes, and forces foveation off with them
    /// (KleptonStageConfiguration.canAmplify).
    private var amplification = 1

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
    /// The overlay pass — a guest's non-eye layers. nil when it failed to
    /// build, which is named at construction rather than per frame.
    private var overlayPipeline: MTLRenderPipelineState?
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

    /// The same storage for a layer that is NOT an eye — an OpenXR quad, which
    /// for a guest in cinematic mode is the whole picture. Its own table because
    /// the shapes differ: an eye is one slice of a shared 2-slice array, a layer
    /// is a plain 2D texture of whatever size the guest's swapchain is.
    private var layers: [Int: MTLTexture] = [:]
    private let layersLock = NSLock()

    // The GUEST's foveation map (KL_VRR=1), and the eye size it was built for.
    //
    // Not to be confused with `drawable.rasterizationRateMaps` — that is the
    // system foveating THIS file's composite pass, and the two are deliberately
    // detached (see unwarpGrid). This one makes the guest rasterize its own
    // expensive pass at a reduced peripheral rate, which leaves the eye texture
    // warped, which `unwarpGrid` then undoes.
    //
    // Touched only from the guest's render thread, inside provideEyeTexture.
    private var guestRateMap: MTLRasterizationRateMap?
    private var guestRateW = 0, guestRateH = 0
    private var guestRateRefused = false

    // The display's own foveation curve, sampled off the drawable's rate maps
    // in primeDisplay and re-issued at the guest's eye size. Empty when the
    // drawable is not foveated, and then the synthetic curve in kl_glfb is used
    // instead.
    private var displayQualityX: [Float] = []
    private var displayQualityY: [Float] = []

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

    // KL_SYNC_GUEST=1 restores the inline shape: kl_app_frame() called on
    // this thread, inside the submission window. That is the clock the lifecycle's
    // device numbers were measured against, so it stays reachable — and it is
    // the A/B for anything that looks like a pacing problem.
    // ...and it is not offered for a guest that brought its own frame loop —
    // Steam Link's OpenXR thread, or an Unreal guest's game thread — because
    // there is no inline frame to make: `kl_app_frame()` returns -1 for both by
    // design, and taking this arm would present black forever while looking
    // like a pacing choice.
    private let syncGuest = klEnvOn("KL_SYNC_GUEST", default: false)
                            && kl_app_target_owns_frame_loop() == 0

    // How many drawables went out before the guest had a picture to put in
    // them. Counted rather than logged per frame, because on device it is
    // legitimately hundreds and each one is uninteresting.
    private var blackFrames = 0
    private var loggedDepthRange = false

    // --- Frame cadence ------------------------------------------------------
    //
    // Whether our frames actually reach the display once per display period, or
    // whether the system is holding each one across two. This is the difference
    // between a picture that is merely late — which reprojection fixes every
    // period, and which is what this whole file is built to do — and a picture
    // that is REPEATED, which reprojection never gets to touch on the repeated
    // period. A held frame during head rotation is two images in the same place
    // the eye is not, i.e. temporal doubling, and it is invisible from every
    // other counter here: `presented`, `cmdbuf done/committed` and the timewarp
    // delta all look perfect while it happens.
    //
    // Measured against the display period this run actually observed
    // (primeDisplay), not against an assumed 90 or 120 Hz.
    private var displayPeriod: TimeInterval = 0
    private var lastPresentation: TimeInterval = 0
    private var cadenceN = 0
    private var cadenceSum = 0.0
    private var cadenceMax = 0.0
    private var cadenceRepeats = 0      // gaps of 1.5 periods or more
    private var missedDeadline = 0
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

        // The system's own pinch, rather than our arithmetic on two fingertip
        // joints. `onSpatialEvent` is how a fully-immersive Metal app is told
        // that the user selected something — the same recogniser the rest of
        // visionOS uses, with the platform's own hysteresis, its own idea of
        // what counts as a pinch, and gaze already folded in. Two rounds of
        // hand-rolled distance thresholding were unstable at exactly the
        // moment they mattered, and there is no version of that arithmetic
        // that beats the recogniser the OS ships.
        let controllers = self.controllers
        layerRenderer.onSpatialEvent = { events in
            controllers.handleSpatialEvents(events)
        }
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
            // The guest asks for views at an instant; only the tracker can
            // answer that, so it is handed to kl_ovrp here rather than having
            // kl_openxr guess from the pose we last published.
            Self.headAtTracker = worldTracking
            kl_ovrp_set_head_at(Self.headAt)
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
            // In head space, not device space — see the midpoint argument at
            // originFromHead. The guest derives its eye positions from this
            // pose and the offsets pushEyeOffsets sent, and those two have to
            // be stated about one origin or every near object sits wrong.
            let (p, q) = Self.decompose(
                Self.headFrom(anchor.originFromAnchorTransform, midpoint: eyeMidpointInDevice))
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

        // The anchors themselves, not their transforms: the grip pose is built
        // from the hand skeleton (the metacarpal a fist closes around) and so
        // is the pinch trigger, and neither is recoverable from the anchor
        // origin alone.
        //
        // Predicted to the presentation time, like the head above and like the
        // Sense controllers inside `update`. `latestAnchors` is the last sample
        // the tracker delivered, which arrives well below display rate — so a
        // 90 Hz render loop reading it draws the same hand pose for two or
        // three frames running, and hands do not hold still that long. That
        // reads as the hands running at a third of the frame rate, which is
        // exactly what it is.
        var left: HandAnchor?, right: HandAnchor?
        if HandTrackingProvider.isSupported {
            let hands = handTracking.handAnchors(at: presentationTime)
            if let a = hands.leftHand,  a.isTracked { left  = a }
            if let a = hands.rightHand, a.isTracked { right = a }
        }
        controllers.update(leftHand: left, rightHand: right, at: presentationTime)
        // ...and the same seam in the other direction: whatever the guest
        // queued for the controllers to play. Here rather than inside `update`
        // because it has nothing to do with the anchors that call takes — it is
        // output, and the only thing it shares with the input half is being
        // once a frame.
        controllers.pumpHaptics()
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
        // not catch getting this wrong.
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
            // Not necessarily a run without a picture: a VULKAN guest never
            // brings ANGLE up at all, and its eye textures arrive from MoltenVK
            // through kl_glfb's eye table instead of through the provider below
            // (see eyeSource). Everything ANGLE-specific — the provider and the
            // shared event — is what is skipped here, and neither exists on
            // that path.
            NSLog("[cp] ANGLE has no MTLDevice — no GL provider and no GL frame "
                  + "fence this run (KL_GLFB unset, or the guest is Vulkan); "
                  + "drawables will be cleared and presented, and any eye "
                  + "texture that reaches kl_glfb's table is still composited")
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
        kl_glfb_set_mtl_provider({ (eye, stage, w, h, fmt, out, ctx) -> Int32 in
            guard let ctx, let out else { return 0 }
            let me = Unmanaged<KleptonCompositor>.fromOpaque(ctx).takeUnretainedValue()
            return me.provideEyeTexture(eye: Int(eye), stage: Int(stage),
                                        w: Int(w), h: Int(h), format: fmt, out: out)
        }, ctx)
        // ...and the same for the layers that are not eyes, without which a
        // guest whose whole frame is a quad has nothing for the overlay pass to
        // sample (kl_glfb.h).
        kl_glfb_set_mtl_layer_provider({ (layer, stage, w, h, fmt, out, ctx) -> Int32 in
            guard let ctx, let out else { return 0 }
            let me = Unmanaged<KleptonCompositor>.fromOpaque(ctx).takeUnretainedValue()
            return me.provideLayerTexture(layer: Int(layer), stage: Int(stage),
                                          w: Int(w), h: Int(h), format: fmt, out: out)
        }, ctx)
        NSLog("[cp] MTL eye and layer providers installed on \(device.name) "
              + "(vertex amplification \(amplification))")
    }

    private func buildPipeline() {
        // The shader is kl_reproject.c's, shared verbatim with the macOS
        // viewer's compositor (runtime/gfx/kl_view_mtl.m) — one copy of the flip,
        // the quad and the array-slice sampler, because having them diverge
        // means debugging the picture twice, and the viewer is where this math
        // can actually be run before a device is available. Compiled from
        // source rather than shipped as a .metal so it stays beside the
        // matrices that feed it (kl_reproject_build).
        let src = String(cString: kl_reproject_msl())
        // The pipeline has to be linked against the format the LAYER got, not
        // against a constant: with the drawable now 8-bit sRGB by default, a
        // pipeline still claiming rgba16Float does not merely look wrong, it
        // fails to encode at all (Metal refuses the attachment mismatch) and
        // every frame goes out cleared.
        let color = layerRenderer.configuration.colorFormat
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction   = lib.makeFunction(name: "kl_reproject_v")
            desc.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
            desc.colorAttachments[0].pixelFormat = color
            desc.depthAttachmentPixelFormat = .depth32Float
            // Both eyes in one pass — see encodeComposite. Declared whatever
            // the layout turns out to be, because a pipeline that permits
            // amplification costs nothing when it is not amplified. Clamped to
            // what the GPU has: asking for 2 where only 1 exists does not
            // degrade, it fails the pipeline, and a nil pipeline is a black
            // display for the whole run.
            amplification = device.supportsVertexAmplificationCount(2) ? 2 : 1
            desc.maxVertexAmplificationCount = amplification
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

            // ...and the state for everything drawn AFTER the first thing at a
            // given distance. `.greater` against a depth already written is
            // false for an EQUAL depth, and every projection layer is a quad at
            // KL_REPROJECT_DEPTH — so with the state above left in force, layer
            // 0 writes its depth and every layer after it is discarded by the
            // depth test. That is the picture losing everything the guest
            // stacked on it, with no error surface: each draw is issued, each
            // returns, and the display shows the backdrop. Measured on device
            // as a lower-detail, shimmering picture, shimmering because WHICH
            // layer is first changes while the guest runs.
            let dsdAlways = MTLDepthStencilDescriptor()
            dsdAlways.depthCompareFunction = .always
            dsdAlways.isDepthWriteEnabled = false
            depthPassState = device.makeDepthStencilState(descriptor: dsdAlways)

            // The overlay pass — the guest's non-eye layers, which are the whole
            // of an Unreal title's UI. See encodeOverlays.
            //
            // The one pipeline here that BLENDS, and the eye pass must not: a
            // guest leaves the eye texture's alpha at 0 because that layer is
            // composited opaque, so a blending eye pass hands the
            // display a transparent frame over a correct picture. A UI quad's
            // alpha is authored and is the point of it.
            let odesc = MTLRenderPipelineDescriptor()
            let olib = try device.makeLibrary(source: String(cString: kl_reproject_overlay_msl()),
                                              options: nil)
            odesc.vertexFunction   = olib.makeFunction(name: "kl_ov_v")
            odesc.fragmentFunction = olib.makeFunction(name: "kl_ov_f")
            odesc.colorAttachments[0].pixelFormat = color
            odesc.depthAttachmentPixelFormat = .depth32Float
            odesc.maxVertexAmplificationCount = amplification
            odesc.colorAttachments[0].isBlendingEnabled = true
            odesc.colorAttachments[0].rgbBlendOperation = .add
            odesc.colorAttachments[0].alphaBlendOperation = .add
            odesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            odesc.colorAttachments[0].sourceAlphaBlendFactor = .one
            odesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            odesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            overlayPipeline = try? device.makeRenderPipelineState(descriptor: odesc)
            if overlayPipeline == nil {
                NSLog("[cp] the overlay pipeline failed to build — a guest's non-eye "
                      + "layers will not be composited (on Unreal that is its whole UI)")
            }

            // KL_CP_PROBE: swap ONE thing about the pass and see if the black
            // moves. Each rung isolates a different link in the chain, so a
            // single device run localises a dark picture instead of a session
            // of one-hypothesis-per-build. See kl_reproject.c for the shaders.
            if probe > 0 {
                let d = MTLRenderPipelineDescriptor()
                d.colorAttachments[0].pixelFormat = color
                d.depthAttachmentPixelFormat = .depth32Float
                d.maxVertexAmplificationCount = amplification
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

    /// Sample one rate map's per-axis quality curve into `n` uniform screen-space
    /// zones.
    ///
    /// A rate map is `screen -> physical`, and its local *rate* is that
    /// function's slope. So walking `physicalCoordinates(screenCoordinates:)`
    /// across uniform screen steps and differencing recovers the curve directly
    /// — no need for the descriptor that built it, which a built map does not
    /// report (the same reason `kl_glfb_eye_rate_zones` has to carry the zone
    /// count for maps we *do* build).
    ///
    /// Metal renormalizes quality so the peak is one sample per pixel, so the
    /// scale of what comes back does not matter, only the shape.
    private static func sampleCurve(_ map: MTLRasterizationRateMap, layer: Int,
                                    zonesX: Int, zonesY: Int) -> ([Float], [Float]) {
        let scr = map.screenSize
        func axis(_ n: Int, _ extent: Int, _ physicalAt: (Float) -> Float) -> [Float] {
            var q = [Float](repeating: 0, count: n)
            var prev = physicalAt(0)
            var peak: Float = 0
            for i in 0..<n {
                let next = physicalAt(Float(extent) * Float(i + 1) / Float(n))
                q[i] = max(0, next - prev)
                peak = max(peak, q[i])
                prev = next
            }
            // A degenerate map would make the descriptor invalid; flat full rate
            // is the honest reading of "no variation along this axis".
            guard peak > 0 else { return [Float](repeating: 1, count: n) }
            // Metal renormalizes so the peak is one sample per pixel, so only
            // the shape survives; the floor keeps a zero-width zone out of the
            // descriptor.
            for i in 0..<n { q[i] = max(q[i] / peak, 0.01) }
            return q
        }
        let qx = axis(zonesX, scr.width) {
            map.physicalCoordinates(screenCoordinates: MTLSamplePosition(x: $0, y: 0),
                                    layer: layer).x
        }
        let qy = axis(zonesY, scr.height) {
            map.physicalCoordinates(screenCoordinates: MTLSamplePosition(x: 0, y: $0),
                                    layer: layer).y
        }
        return (qx, qy)
    }

    /// Take the display's own foveation curve off a drawable, so the guest
    /// rasterizes the way the compositor already does instead of the way we
    /// guessed.
    ///
    /// Called from `primeDisplay`, which is before the guest boots and therefore
    /// before any eye texture exists — the ordering `updateGuestRateMap` needs.
    /// Sampled once, from the first drawable, exactly as `pushEyeTextureSize`
    /// is: `renderQuality` ramps rather than switching, so an early drawable can
    /// still carry the old quality — but quality scales the *resolution*, and
    /// only the normalized shape of the curve is taken from here.
    ///
    /// **The two eyes' maps are combined, per zone, by maximum.** visionOS gives
    /// each view its own map and they are not the same map mirrored-and-centred:
    /// the eyes converge, so each one's dense region sits inboard, and the pair
    /// are mirror images of each other. The guest cannot be given two, because
    /// both eyes render through ONE multisampled scene renderbuffer and resolve
    /// into ONE two-slice array texture — the map is registered against those
    /// objects, and neither of them knows which eye is in flight. Taking the
    /// envelope makes a symmetric curve that is dense wherever *either* eye
    /// wants density: strictly more samples than either map alone, so no eye is
    /// ever under-rendered, at the cost of some fragments the other eye did not
    /// need. Per-eye guest maps would mean per-eye scene storage, which is a
    /// different change to make deliberately.
    private func captureDisplayFoveation(_ drawable: LayerRenderer.Drawable) {
        guard displayQualityX.isEmpty else { return }
        let maps = drawable.rasterizationRateMaps
        guard let first = maps.first else {
            NSLog("[cp] drawable is not foveated — KL_VRR would use the synthetic "
                  + "curve (KL_VRR_ZONES / KL_VRR_EDGE)")
            return
        }
        // The zone count, ALVR's way (DummyMetalRenderer): a built map does not
        // report its zones, but physicalSize / physicalGranularity is the number
        // of cells it actually resolves to. Capped at the unwarp grid's own
        // limit, since a grid cell per zone is what makes the unwarp exact.
        let gran = first.physicalGranularity
        let phys = first.physicalSize(layer: 0)
        let cap = Int(KL_REPROJECT_GRID_MAX)
        let nx = max(1, min(cap, gran.width  > 0 ? phys.width  / gran.width  : 16))
        let ny = max(1, min(cap, gran.height > 0 ? phys.height / gran.height : 16))

        var qx = [Float](repeating: 0, count: nx)
        var qy = [Float](repeating: 0, count: ny)
        var sampled = 0
        for map in maps {
            for layer in 0..<map.layerCount {
                let (mx, my) = Self.sampleCurve(map, layer: layer, zonesX: nx, zonesY: ny)
                for i in 0..<nx { qx[i] = max(qx[i], mx[i]) }
                for i in 0..<ny { qy[i] = max(qy[i], my[i]) }
                sampled += 1
            }
        }
        guard sampled > 0 else { return }
        displayQualityX = qx
        displayQualityY = qy
        // BOTH curves, and y is not decoration: the horizontal one is an
        // envelope of two mirrored maps and is symmetric by construction, so it
        // can never show an up/down asymmetry even when one exists. The
        // vertical one is the only place a "the top periphery looks different
        // from the bottom" observation can be settled — a lopsided qy is the
        // display's own distribution being copied faithfully, a symmetric one
        // means look at our unwarp instead. Screen y=0 here is the render
        // target's row 0, which is the picture's BOTTOM (the guest stores it
        // GL-side-up), so qy is printed bottom-to-top.
        func curve(_ q: [Float]) -> String {
            q.map { String(format: "%.2f", $0) }.joined(separator: " ")
        }
        NSLog("%@", "[cp] display foveation curve: \(nx)x\(ny) zones from \(sampled) "
              + "view map(s), screen \(first.screenSize.width)x\(first.screenSize.height) "
              + "-> physical \(phys.width)x\(phys.height), granularity "
              + "\(gran.width)x\(gran.height)\n"
              + "[cp]   x rate (left to right): \(curve(qx))\n"
              + "[cp]   y rate (bottom to top): \(curve(qy))")
    }

    /// Build (or rebuild) the guest's foveation map for a `w` x `h` eye and push
    /// it to kl_glfb.
    ///
    /// **The curve is the display's**, resampled to the guest's screen size —
    /// which is why this is not simply handing the drawable's map straight
    /// through. The guest's eye size is not the drawable's: it comes from
    /// `kl_ovrp_set_eye_texture_size`, which caps and scales (`KL_OVRP_EYE_MAX`,
    /// `KL_OVRP_EYE_SCALE`) because Unity allocates stages x eyes of RGBA16F at
    /// whatever it is told. A rate map is bound to the `screenSize` it was built
    /// for, so a map for the drawable's resolution applied to a smaller eye
    /// texture would foveate against coordinates the guest never renders. Same
    /// shape, our size. `KL_VRR_ZONES` / `KL_VRR_EDGE` are the fallback for an
    /// unfoveated drawable, and are what the host stand-in
    /// (`klmtl_update_rate_map` in tests/t_mtl_provider.m) always uses.
    ///
    /// **Called before the eye texture is handed back**, deliberately. kl_glfb
    /// binds the texture as soon as this provider returns, and `FramebufferMtl`
    /// caches its render pass descriptor — a map that arrives after the first
    /// pass into a texture is silently ignored for that pass, and re-attaching
    /// the texture is not enough to force the rebuild.
    ///
    /// Keyed on the size because Unity re-creates its eye textures mid-startup
    /// at a different one; a map built for the old screen size would foveate
    /// against coordinates that no longer exist.
    private func updateGuestRateMap(w: Int, h: Int) {
        var zones: Int32 = 0
        var edge: Float = 0
        guard kl_glfb_foveation_wanted(&zones, &edge) != 0 else { return }
        if guestRateMap != nil, guestRateW == w, guestRateH == h { return }
        guard !guestRateRefused else { return }
        guard device.supportsRasterizationRateMap(layerCount: 1) else {
            // Said once: this is a property of the GPU, so it will not become
            // true later in the run.
            guestRateRefused = true
            NSLog("[cp] this GPU has no variable rasterization rate — KL_VRR ignored")
            return
        }
        var qx = displayQualityX, qy = displayQualityY
        var source = "the display's own"
        if qx.isEmpty || qy.isEmpty {
            let n = Int(zones)
            qx = [Float](repeating: 0, count: n)
            qy = [Float](repeating: 0, count: n)
            qx.withUnsafeMutableBufferPointer { kl_glfb_foveation_quality($0.baseAddress, zones, edge) }
            qy.withUnsafeMutableBufferPointer { kl_glfb_foveation_quality($0.baseAddress, zones, edge) }
            source = "synthetic, edge rate \(String(format: "%.2f", edge))"
        }
        // `__sampleCount:` is the pointer-taking initializer as Swift imports
        // it; the array-taking spelling does not exist here.
        let layer = qx.withUnsafeBufferPointer { hp in
            qy.withUnsafeBufferPointer { vp in
                MTLRasterizationRateLayerDescriptor(
                    __sampleCount: MTLSize(width: qx.count, height: qy.count, depth: 0),
                    horizontal: hp.baseAddress!, vertical: vp.baseAddress!)
            }
        }
        let desc = MTLRasterizationRateMapDescriptor()
        desc.screenSize = MTLSize(width: w, height: h, depth: 0)
        desc.label = "klepton guest foveation"
        desc.setLayer(layer, at: 0)
        guard let map = device.makeRasterizationRateMap(descriptor: desc) else {
            guestRateRefused = true
            NSLog("[cp] makeRasterizationRateMap \(w)x\(h) failed — KL_VRR ignored")
            return
        }
        let phys = map.physicalSize(layer: 0)
        // "%@", not the string itself: NSLog's first argument is a FORMAT
        // string, and this one contains a literal per-cent sign — "% o" was
        // being eaten as a conversion, so the device's first foveation line
        // read "33.50f the fragments".
        NSLog("%@", "[cp] guest foveation: screen \(w)x\(h) -> physical "
              + "\(phys.width)x\(phys.height) "
              + "(\(String(format: "%.1f", 100.0 * Double(phys.width * phys.height) / Double(w * h)))% "
              + "of the fragments, \(qx.count)x\(qy.count) zones, \(source))")
        // Held by this property, which is what keeps it alive: kl_glfb takes an
        // unmanaged pointer and never retains, exactly as it does not retain the
        // eye textures.
        guestRateMap = map
        guestRateW = w; guestRateH = h
        kl_glfb_set_eye_rate_map(Int32(w), Int32(h), Int32(qx.count), Int32(qy.count),
                                 Unmanaged.passUnretained(map).toOpaque())
    }

    /// kl_glfb calls this when the guest asks for storage for one (eye, stage).
    ///
    /// Two things here are load-bearing and both were learned by running it
    ///: the texture must be allocated on ANGLE's device, and the `w`/`h`
    /// reported back must be the texture's *real* size. The EGL extension takes
    /// the size from the MTLTexture and ignores what we claim, so a wrong-sized
    /// texture binds **successfully** and the guest renders into storage it does
    /// not have. kl_glfb refuses a mismatch rather than accept that.
    private func provideEyeTexture(eye: Int, stage: Int, w: Int, h: Int,
                                   format: UInt32,
                                   out: UnsafeMutablePointer<kl_mtl_eye_texture>) -> Int32 {
        let k = Self.key(eye, stage)
        let partnerKey = Self.key(eye == 0 ? 1 : 0, stage)

        guard let pixelFormat = klEyePixelFormat(format) else {
            NSLog("[cp] eye \(eye) stage \(stage) asks for GL internalformat "
                  + String(format: "0x%x", format)
                  + ", which has no MTLPixelFormat here — declining, so this eye "
                  + "renders into GL storage nothing composites")
            return 0
        }

        // Before anything is handed back, including on the cached path: the
        // size is only knowable here, and the map has to be in force before the
        // texture is bound. A no-op once the map matches this size.
        updateGuestRateMap(w: w, h: h)

        // **Nothing slow happens under eyesLock.** The render loop takes it once
        // per view per frame, so anything held across it stops frame submission
        // dead — and Compositor Services kills a client that stops submitting.
        // This function used to hold it across `makeTexture`, which at a loading
        // transition means three ~164 MB private allocations back to back while
        // the guest is also allocating hard. That is seconds of no frames, and
        // it presented as the app being backgrounded or killed *during loading
        // transitions* specifically — the one moment the eye swapchain is
        // re-created. The lock now covers dictionary access only.
        //
        // Unity really does re-create its eye textures at a different size
        // mid-startup — 1832x1920 during nativeRecreateGfxState, then 2198x2304
        // once the VRDevice has settled, and again on entering a map. A provider
        // that caches one texture per stage (the obvious implementation) hands
        // back the small one, so the size is part of the identity, not just the
        // key.
        eyesLock.lock()
        let cached = eyes[k]
        var partner = eyes[partnerKey]
        eyesLock.unlock()

        // The format is part of the identity for the same reason the size is:
        // handing back a texture whose pixel format is not the one the guest
        // asked for gets the pair refused by ANGLE, and the eye then quietly
        // falls back to GL storage nothing samples.
        func fits(_ a: EyeAllocation) -> Bool {
            a.texture.width == w && a.texture.height == h
                && a.texture.pixelFormat == pixelFormat
        }

        if let a = cached, fits(a) {
            out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(a.texture).toOpaque(),
                                             slice: Int32(a.slice),
                                             w: Int32(a.texture.width),
                                             h: Int32(a.texture.height))
            return 1
        }

        // Both eyes share one array texture, so the second eye must find the
        // first eye's allocation rather than make its own.
        var tex: MTLTexture? = nil
        if let p = partner, fits(p) { tex = p.texture }

        if tex == nil {
            let desc = MTLTextureDescriptor()
            desc.textureType = .type2DArray
            desc.pixelFormat = pixelFormat
            desc.width = w; desc.height = h
            desc.arrayLength = 2                 // one slice per eye
            desc.mipmapLevelCount = 1
            // renderTarget because the guest draws into it through the EGLImage;
            // shaderRead because this file's composite pass samples it back.
            desc.usage = [.renderTarget, .shaderRead]
            desc.storageMode = .private

            guard let t = device.makeTexture(descriptor: desc) else {
                NSLog("[cp] could not allocate eye \(eye) stage \(stage) \(w)x\(h)")
                return 0
            }
            t.label = "klepton eye stage \(stage)"
            tex = t
            // The real cost, not the arithmetic one. Metal reports what it
            // actually reserved, so this is the only honest input to "is the
            // eye swapchain why we are at 3.5 GB".
            let naive = w * h * 8 * 2
            NSLog("[cp] stage \(stage): GL "
                  + String(format: "0x%x", format) + " \(w)x\(h) array, 2 slices — "
                  + "\(t.allocatedSize / (1024 * 1024)) MiB allocated "
                  + "(uncompressed would be \(naive / (1024 * 1024)) MiB), "
                  + "compression=\(t.compressionType == .lossy ? "lossy" : "lossless")")
        }

        eyesLock.lock()
        // Re-check the partner: allocating outside the lock means the other eye
        // may have published one meanwhile. Adopting it is what keeps both eyes
        // on ONE array texture — the composite drops a view outright if they
        // disagree — and the one we just made is simply released. In practice
        // ovrp_SetupEyeTexture2 arrives serially on the guest's render thread,
        // so this costs nothing and exists so that assumption is not load-bearing.
        partner = eyes[partnerKey]
        if let p = partner, fits(p) { tex = p.texture }
        let final = tex!
        eyes[k] = EyeAllocation(texture: final, slice: eye)
        eyesLock.unlock()

        out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(final).toOpaque(),
                                         slice: Int32(eye),
                                         w: Int32(final.width), h: Int32(final.height))
        return 1
    }

    /// Storage for a guest's non-eye LAYER. See `layers` above and kl_glfb.h.
    ///
    /// Simpler than the eye allocator in every respect that made that one
    /// complicated: nothing is shared between two callers, there is no slice and
    /// no rate map. What it keeps is the identity rule — size and format are
    /// part of it, so a guest that rebuilds its swapchain is not handed the
    /// previous allocation and left rendering into storage it does not have.
    private func provideLayerTexture(layer: Int, stage: Int, w: Int, h: Int,
                                     format: UInt32,
                                     out: UnsafeMutablePointer<kl_mtl_eye_texture>) -> Int32 {
        guard let pixelFormat = klEyePixelFormat(format) else {
            NSLog("[cp] layer \(layer) stage \(stage) asks for GL internalformat "
                  + String(format: "0x%x", format)
                  + ", which has no MTLPixelFormat here — declining, so the layer "
                  + "renders into GL storage nothing composites")
            return 0
        }
        let k = layer &* 64 &+ stage
        layersLock.lock()
        let cached = layers[k]
        layersLock.unlock()
        var tex = cached
        if tex == nil || tex!.width != w || tex!.height != h ||
           tex!.pixelFormat != pixelFormat {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: pixelFormat, width: w, height: h, mipmapped: false)
            desc.usage = [.renderTarget, .shaderRead]
            desc.storageMode = .private
            guard let t = device.makeTexture(descriptor: desc) else {
                NSLog("[cp] could not allocate layer \(layer) stage \(stage) \(w)x\(h)")
                return 0
            }
            t.label = "klepton layer \(layer) stage \(stage)"
            tex = t
            layersLock.lock()
            layers[k] = t
            layersLock.unlock()
            NSLog("[cp] layer \(layer) stage \(stage): GL "
                  + String(format: "0x%x", format) + " \(w)x\(h) — "
                  + "\(t.allocatedSize / (1024 * 1024)) MiB allocated")
        }
        let final = tex!
        out.pointee = kl_mtl_eye_texture(texture: Unmanaged.passUnretained(final).toOpaque(),
                                         slice: 0,
                                         w: Int32(final.width), h: Int32(final.height))
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
            guard let drawable = frame.queryDrawables().first else { continue }
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
        applyRenderQuality()
        startARKit()

        // Before the guest, not after: the frustum and the display rate can
        // only be read from a drawable, and the guest reads both exactly once
        // during the lifecycle below. See primeDisplay().
        primeDisplay()

        // Bring the guest up, on its own thread. _begin is
        // once per process and does the /proc report, nativeRecreateGfxState,
        // nativeResume and the first nativeRender, and it happens over there
        // rather than here: this loop must be free to present black while the
        // guest is still starting, which can take tens of seconds on device.
        //
        // Under KL_SYNC_GUEST the old inline shape is kept instead, because
        // The device measurement was taken against it and the device has
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
                // ...and the audio, which does NOT come back on its own.
                //
                // This transition is the immersive space being temporarily
                // dismissed and restored — a Digital Crown press to passthrough,
                // the app's window being closed out from under it — and visionOS
                // silently stops calling CoreAudio's render callback across it:
                // no error, no interruption notification, and an output unit
                // that still reports itself started. It is the same OS bug ALVR
                // works around, and the same workaround (rebuild, do not
                // restart). kl_audio's own heartbeat finds it too, about a
                // second later; this is the half that knows the exact moment,
                // and a second of dropped music is the difference.
                kl_audio_resume()
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
                NSLog("[cp] \(cadenceSummary())")
                NSLog("[cp] \(stageSummary())")
            }
        }
        NSLog("[cp] render loop ended after \(presented) presented frames, "
              + "\(iterations) iterations, state=\(String(describing: layerRenderer.state))")
        // Ends the guest's loop and joins it, so its report — which is the
        // lifecycle report, and belongs to the guest's end of the run rather than
        // to this one — has been written before this returns.
        if syncGuest { kl_app_lifecycle_report() } else { kl_app_guest_stop() }
    }

    /// What each swapchain stage actually holds, per report interval.
    ///
    /// The doubling that survived every pose fix alternates
    /// with the stage count, and the only thing that differs between one stage
    /// and N is *which texture this pass samples*. So the remaining hypothesis
    /// is not two positions but two **pictures**: one stage holding a stale or
    /// wrong-generation image while another holds the live one. That is immune
    /// to every pose measurement, which is why all four of them came back clean.
    ///
    /// Three independent numbers per stage, chosen so that each failure reads
    /// differently instead of all three looking like "doubling":
    ///
    ///   draws   how many times the GUEST bound this stage as its draw target,
    ///           straight off the GL thunks and through no bookkeeping of ours.
    ///           Frozen here = the guest stopped drawing this stage.
    ///   serial  the guest frame whose pose is filed against it. Frozen while
    ///           `draws` climbs = the association, not the picture.
    ///   tex     the MTLTexture the guest is bound to, from kl_glfb's own
    ///           record — compared against the one this pass samples. These are
    ///           set by the same call and can still diverge, because the
    ///           provider caches per (eye, stage) on size while Unity re-creates
    ///           the swapchain mid-run at two different sizes. A divergence here
    ///           is the whole hypothesis, and nothing else in the run would show
    ///           it: every count stays healthy while the compositor samples a
    ///           texture the guest has stopped writing to.
    /// The storage to sample for one (eye, stage), from whichever half of the
    /// seam produced it.
    ///
    /// Two halves, and they run in opposite directions. On the **GL** path this
    /// compositor ALLOCATES the eye texture and hands it to ANGLE through the
    /// provider, so `eyes` is the authority and the guest renders into
    /// something we already hold. On the **VULKAN** path there is no provider at
    /// all: MoltenVK has already backed the guest's eye VkImage with an
    /// MTLTexture of its own, and kl_vulkan publishes that one into kl_glfb's
    /// eye table (`kl_glfb_note_eye_mtl_texture`). Nothing here can allocate it,
    /// so nothing here can be asked for it — the only way to find it is to look
    /// it up.
    ///
    /// The table is checked FIRST, and that is not arbitrary: it is filled by
    /// both paths (the provider registers its allocation there too), so it is
    /// the one place that always describes the storage the guest is actually
    /// rendering into. `eyes` remains the fallback because it survives the gap
    /// between ovrp_DestroyEyeTexture and the next SetupEyeTexture2, where the
    /// table is deliberately empty and showing the last good frame beats showing
    /// black.
    private func eyeSource(_ eye: Int, _ stage: Int) -> EyeAllocation? {
        var slice: Int32 = 0
        if let t = kl_glfb_eye_mtl_texture(Int32(eye), Int32(stage), &slice) {
            let tex = Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue() as MTLTexture
            return EyeAllocation(texture: arrayView(tex), slice: Int(slice),
                                 flipY: kl_glfb_eye_mtl_origin_top_left(Int32(eye),
                                                                        Int32(stage)) != 0)
        }
        eyesLock.lock(); let a = eyes[Self.key(eye, stage)]; eyesLock.unlock()
        return a
    }

    /// Every shader in kl_reproject.c samples a `texture2d_array<float>`, and
    /// binding a non-array texture to that slot does not fail — it samples
    /// nothing useful, which composites as a FLAT COLOUR over storage that is
    /// full of picture. Until Open Brush nothing here could produce one: the
    /// provider below allocates `.type2DArray` explicitly, and BONELAB's Vulkan
    /// eye image carries two array layers because the eye IS the layer there. An
    /// OpenXR guest on Vulkan has one swapchain per eye, so MoltenVK hands back a
    /// plain `.type2D` and the assumption breaks.
    ///
    /// A view rather than a second pipeline: only the type is wrong, and one
    /// shader shared by both compositors is worth more than two that can drift.
    /// Cached, because a view per frame is an allocation per frame. If the view
    /// cannot be made it says so once and returns the original — which
    /// composites exactly as badly as before, but names the reason rather than
    /// leaving a flat colour to be read as a guest that drew nothing.
    private var arrayViews: [ObjectIdentifier: MTLTexture] = [:]
    private var saidNoArrayView = false

    private func arrayView(_ tex: MTLTexture) -> MTLTexture {
        if tex.textureType == .type2DArray { return tex }
        let key = ObjectIdentifier(tex)
        if let v = arrayViews[key] { return v }
        guard let v = tex.makeTextureView(pixelFormat: tex.pixelFormat,
                                          textureType: .type2DArray,
                                          levels: 0..<tex.mipmapLevelCount,
                                          slices: 0..<1) else {
            if !saidNoArrayView {
                saidNoArrayView = true
                NSLog("[cp] the eye texture is textureType \(tex.textureType.rawValue) "
                      + "and an array VIEW of it could not be made — the shaders "
                      + "sample texture2d_array, so this composites as a flat colour")
            }
            return tex
        }
        arrayViews[key] = v
        return v
    }

    private func stageSummary() -> String {
        var out = "stages:"
        var bad = false
        for s in 0..<Int(kl_ovrp_stage_count()) {
            var r = kl_ovrp_render_pose()
            let have = withUnsafeMutablePointer(to: &r) {
                kl_ovrp_stage_render_pose(Int32(s), $0) != 0
            }
            out += " s\(s) draws=\(kl_glfb_stage_draw_count(Int32(s)))"
            out += " serial=\(have ? String(r.serial) : "-")"
            for eye in 0..<2 {
                var slice: Int32 = 0
                let guestTex = kl_glfb_eye_mtl_texture(Int32(eye), Int32(s), &slice)
                // tryLock, never lock: this runs on the render loop and a
                // diagnostic that can block frame submission is a worse bug
                // than the one it is chasing. A contended read just prints "?".
                var mine: EyeAllocation? = nil
                var read = false
                if eyesLock.try() { mine = eyes[Self.key(eye, s)]; eyesLock.unlock(); read = true }
                let mineTex = mine.map { Unmanaged.passUnretained($0.texture).toOpaque() }
                // A nil guest texture is not a disagreement: between
                // ovrp_DestroyEyeTexture and the next SetupEyeTexture2 the slot
                // is deliberately empty, and this cache still holds the old
                // texture on purpose — compositing the last good frame through
                // a loading transition is better than a black one. Only two
                // *different* live textures mean the two sides have diverged.
                if read && guestTex != nil && guestTex != mineTex { bad = true }
                // Only the low bits: two pointers into the same heap differ
                // there, and a full 64-bit address per eye per stage makes the
                // line unreadable at the moment it matters most.
                let tag = guestTex.map { String(UInt(bitPattern: $0) & 0xffffff, radix: 16) } ?? "nil"
                out += " e\(eye)=\(tag)"
                if !read { out += "?" }
                else if guestTex != nil && guestTex != mineTex { out += "!=SAMPLED" }
            }
            out += " |"
        }
        // The association's own health, live. A device run normally ends by the
        // immersive space being invalidated rather than by the guest finishing,
        // so the end-of-run report is the one thing that often is not written —
        // and these four numbers are exactly what it would have carried.
        var dropped: UInt64 = 0, multi: UInt64 = 0, cross: UInt64 = 0, disagree: UInt64 = 0
        kl_ovrp_association_stats(&dropped, &multi, &cross, &disagree)
        if dropped != 0 || multi != 0 || cross != 0 || disagree != 0 {
            out += " assoc: \(dropped) drew nothing, \(multi) multi-stage,"
            out += " \(cross) off-thread, \(disagree) index disagreed"
        }
        return out + (bad ? "  <-- the guest and the composite are on DIFFERENT textures" : "")
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
        // startSubmission must NOT be called before queryDrawable: the two
        // bail paths below would then abandon a submission they had already
        // started, which Compositor Services aborts on.

        // Poses first, then the frame that uses them.
        if let timing {
            updatePoses(at: LayerRenderer.Clock.Instant.epoch.duration(to: timing.presentationTime)
                            .timeInterval)
        }

        // Offer the guest this frame's pose and carry straight on. The guest
        // wakes on its own thread, ticks the Choreographer,
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

        // ALL of them, not just the first. visionOS 26 hands out more than one
        // drawable while the render quality is moving between two resolutions —
        // the system crossfades them so the change is not visible — and a loop
        // that renders drawables[0] and presents nothing else shows a half-empty
        // frame for the whole transition. That is exactly the transition
        // maxRenderQuality = 1.0 provokes, so the two changes belong together;
        // queryDrawable() is deprecated in 26 in favour of this for the same
        // reason, and ALVR loops over the array likewise.
        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else { bailNoDrawable += 1; return 0 }
        guard let cmd = queue?.makeCommandBuffer(), pipeline != nil, sampler != nil else {
            bailNoPipeline += 1
            return 0
        }
        // Everything above may abandon the frame; from here it will be
        // presented, so the submission can be opened.
        frame.startSubmission()
        let main = drawables[0]

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
        // Which guest frame this composite would show. Two sources, one
        // quantity: ANGLE signals our MTLSharedEvent from inside its own
        // command stream, and a VULKAN guest has no such event — kl_vulkan
        // makes the guest's queue idle at ovrp_EndFrame4 and publishes a serial
        // instead. `guestFrameEvent` being nil on that path is what keeps the
        // wait below from being encoded against a value nothing ever signals,
        // which would be a command buffer that is committed and never executes.
        // The guest's projection layers, snapshotted BEFORE the fence below is
        // read, and that order is the whole of it: the guest publishes its
        // frame's completion before the records that describe it
        // (klxr_EndFrame), so a list read first can only be OLDER than the
        // fence read second — never newer, which would be drawing layers whose
        // blits this pass never waited for. Seen as one frame of the display
        // showing a picture from several frames ago, a few seconds apart.
        projLayerCount = projLayers.withUnsafeMutableBufferPointer {
            kl_ovrp_eye_layer_live() != 0
                ? Int(kl_ovrp_proj_layers_snapshot($0.baseAddress, Int32($0.count)))
                : 0
        }
        let glFence = kl_glfb_gpu_fence_value()
        let frameValue = glFence != 0 ? glFence : kl_vulkan_frame_serial()
        // KL_CP_NOFENCE=1 skips the wait. A composite command buffer that waits
        // on an event value the guest's queue never signals is COMMITTED and
        // never EXECUTES: the drawable is presented, every counter here looks
        // healthy, and the display shows nothing. That failure is invisible
        // from this side, so it needs its own A/B — and cmdCompleted below is
        // the measurement that says whether it is happening.
        if let ev = guestFrameEvent, glFence != 0, !noFence {
            cmd.encodeWaitForEvent(ev, value: glFence)
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
            .duration(to: main.frameTiming.presentationTime).timeInterval
        let displayAnchor = worldTracking.queryDeviceAnchor(atTimestamp: presentation)
        let originFromDevice = displayAnchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        // OpenXR's VIEW space is DEFINED as the midpoint between the eyes.
        // visionOS's device anchor is a point on the hardware and is not that
        // midpoint, so treating the two as equal declares an equality that is
        // false at both seams at once: the guest is told its head is somewhere
        // it is not, and the timewarp rotates about a centre the picture was
        // not drawn about. The second one is the expensive half — a rotation
        // applied about the wrong centre moves the eyes along a lever arm
        // nothing models, so a pure head turn smears near content while the
        // measured delta stays small and healthy-looking.
        //
        // The drawable states the midpoint every frame and it needs no tuning:
        // it is the average of the two eyes' own transforms. `deviceAnchor`
        // below still gets the raw anchor, which is what that API means.
        let originFromHead = Self.headFrom(originFromDevice, midpoint: eyeMidpointInDevice)

        // Measure the pose the composite is about to DRAW with, which is not
        // always the frame record. With the layer path live the eye picture is
        // placed by the guest's own submitted layer pose; the frame record is
        // the head pose we latched when the guest opened the frame. Those are
        // different poses, and reporting the second while drawing the first
        // describes a correction nobody is applying — the delta reads healthy
        // while the picture swims.
        var measured = haveRendered ? rendered : nil
        if projLayerCount > 0 {
            var r = kl_ovrp_render_pose()
            r.serial = rendered.serial
            r.submit_ns = rendered.submit_ns
            let pl = projLayers[0]
            r.px = pl.pose.0; r.py = pl.pose.1; r.pz = pl.pose.2
            r.qx = pl.pose.3; r.qy = pl.pose.4; r.qz = pl.pose.5; r.qw = pl.pose.6
            measured = r
        }
        noteReprojection(rendered: measured,
                         originFromHead: originFromHead, stage: stage)

        // The drawable's depth range, once, with the depth our quad actually
        // reaches. This is not curiosity. The quad's distance is what the
        // system's own reprojection is told about our content, so it decides
        // how much parallax gets applied to a picture that has none — and the
        // one time it was chosen from a measurement, the measurement had two
        // variables in it and the wrong one got the blame. Both numbers, side
        // by side, so the next person does not have to take anyone's word.
        if !loggedDepthRange {
            loggedDepthRange = true
            let drawable = main
            let r = drawable.depthRange
            // The quad's ACTUAL clip-space depth, against the projection this
            // drawable really handed us — not the distance in metres, which
            // says nothing without the projection. In reverse-Z anything in
            // [0,1] is inside the clip range; a value near 0 means "far away",
            // which is what we want the system's own reprojection to believe
            // and is NOT the same thing as being clipped. `far = inf` above is
            // what makes that safe. Printed because this exact question was
            // argued about and settled wrongly once (kl_reproject_build).
            // No recorded pose: the depth does not depend on one, and asking
            // with nil is the case kl_reproject_build already handles.
            var probe = kl_reproject_build(nil, 0, matrix_identity_float4x4,
                                           drawable.views[0].transform,
                                           drawable.computeProjection(viewIndex: 0), 0, 0, 0)
            let ndcZ = withUnsafePointer(to: &probe) { kl_reproject_ndc_depth($0) }
            NSLog(String(format: "[cp] drawable depthRange = %@ (x=far, y=near in "
                                 + "reverse-Z); quad at %.1f m lands at NDC z %.6f "
                                 + "(%@)", "\(r)", probe.depth, ndcZ,
                         (ndcZ >= 0 && ndcZ <= 1) ? "inside the clip range"
                                                  : "*** OUTSIDE THE CLIP RANGE ***"))
            // The drawable's actual shape, once. A clear fills the whole
            // attachment whatever the viewport says, so a blue clear that is
            // not visible means the texture being cleared is not the texture
            // being displayed — and these are the numbers that say which.
            NSLog("[cp] \(drawables.count) drawable(s); [0]: "
                  + "\(drawable.colorTextures.count) color texture(s), "
                  + "\(drawable.views.count) view(s), layout="
                  + (layerRenderer.configuration.layout == .layered ? "layered" : "dedicated")
                  + ", render quality \(layerRenderer.renderQuality) of max "
                  + "\(layerRenderer.configuration.maxRenderQuality)")
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
            // The rate map is the whole point of foveation, and the two numbers
            // it carries are the ones to size the GUEST's eye textures against:
            // `screenSize` is the resolution the picture is rasterized at in
            // the fovea, `physicalSize` is what is actually stored. Rendering
            // the guest at much less than the screen size throws away the
            // resolution maxRenderQuality just paid for; much more is wasted.
            // (ALVR measures 4338x3478 screen / 1888x1792 physical at the
            // default quality, 6262x5020 / 2496x2432 at 1.0.)
            if let rm = drawable.rasterizationRateMaps.first {
                var line = "[cp] rate map: screen \(rm.screenSize.width)x\(rm.screenSize.height), "
                         + "\(rm.layerCount) layer(s)"
                for l in 0..<rm.layerCount {
                    let p = rm.physicalSize(layer: l)
                    line += ", physical[\(l)] \(p.width)x\(p.height)"
                }
                NSLog("%@", line)
            } else {
                NSLog("[cp] no rasterization rate map — foveation is off, the "
                      + "drawable is rasterized uniformly")
            }
        }

        // The guest's stereo separation, refreshed from the display itself.
        pushEyeOffsets(main)

        var encoded = 0
        for drawable in drawables {
            // Per drawable, not once: each one is presented separately and each
            // is reprojected by the system against its own anchor.
            drawable.deviceAnchor = displayAnchor
            encoded += encodeComposite(drawable, cmd: cmd, stage: stage,
                                       completeStage: completeStage,
                                       rendered: rendered, haveRendered: haveRendered,
                                       originFromHead: originFromHead)
            drawable.encodePresent(commandBuffer: cmd)
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
        cmd.commit()
        cmdCommitted += 1
        noteCadence(presentation: presentation, drawable: main)
        frame.endSubmission()      // only here — see startSubmission above
        return encoded > 0 ? 1 : 0
    }

    /// One drawable, every eye it carries. Returns how many eyes got a picture.
    ///
    /// **Both eyes go through ONE render pass now, and that is what foveation
    /// costs.** A foveated drawable comes with a rasterization rate map, and
    /// Metal applies that map's *layer N* to render-target array index N. A
    /// pass that selects one slice with `colorAttachments[0].slice` has a
    /// render-target array length of 1, so it can only ever reach the map's
    /// layer 0 — the right eye would be rasterized through the left eye's
    /// foveation pattern, and the system's unwarp would then be undoing a
    /// distortion that was never applied. So the layered path sets
    /// `renderTargetArrayLength = views.count` and draws both eyes in one draw
    /// call with vertex amplification, picking its per-eye uniforms by
    /// `[[amplification_id]]`. Apple's template and ALVR are both this shape.
    ///
    /// The `.dedicated` layout keeps the old per-view pass, with that view's
    /// own single-layer rate map, because there is nothing to amplify.
    private func encodeComposite(_ drawable: LayerRenderer.Drawable,
                                 cmd: MTLCommandBuffer, stage: Int, completeStage: Int,
                                 rendered: kl_ovrp_render_pose, haveRendered: Bool,
                                 originFromHead: simd_float4x4) -> Int {
        // KL_CP_AMPLIFY=0 — back to ONE RENDER PASS PER EYE, selecting the
        // slice on the attachment, with no rate map and no amplification.
        //
        // That is exactly the composite that existed before this session, and
        // it is the A/B that has been missing: every fix stacked on the
        // doubling so far has assumed the bug predates the amplified pass, and
        // nothing has tested that assumption. If both amplifications were
        // landing on array index 0 and viewport 0 — the failure mode of a view
        // mapping that is not being applied — then both eyes' quads are drawn
        // over each other into one slice, which is two overlapping images and
        // would be indifferent to the cant, the pose source and the swapchain
        // alike. Take this rung first; it splits "regression from this session"
        // from "pre-existing", and nothing else does.
        let layered = layerRenderer.configuration.layout == .layered
                      && drawable.views.count > 1
                      && amplification >= drawable.views.count
                      && klEnvOn("KL_CP_AMPLIFY", default: true)
        if layered {
            return encodeViews(Array(drawable.views.indices), drawable: drawable,
                               layered: true,
                               rateMap: drawable.rasterizationRateMaps.first,
                               cmd: cmd, stage: stage, completeStage: completeStage,
                               rendered: rendered, haveRendered: haveRendered,
                               originFromHead: originFromHead)
        }
        var n = 0
        let maps = drawable.rasterizationRateMaps
        // One map per view, or none. A LAYERED drawable has a single map whose
        // layers are the eyes, and a pass that selects one slice can only ever
        // reach layer 0 — so handing it that map rasterizes the right eye
        // through the left eye's foveation pattern. Better no map at all.
        let perView = maps.count == drawable.views.count
        for i in drawable.views.indices {
            n += encodeViews([i], drawable: drawable, layered: false,
                             rateMap: perView ? maps[i] : nil,
                             cmd: cmd, stage: stage, completeStage: completeStage,
                             rendered: rendered, haveRendered: haveRendered,
                             originFromHead: originFromHead)
        }
        return n
    }

    /// A group of views sharing one attachment, and therefore one render pass.
    private func encodeViews(_ viewIndices: [Int],
                             drawable: LayerRenderer.Drawable,
                             layered: Bool,
                             rateMap: MTLRasterizationRateMap?,
                             cmd: MTLCommandBuffer, stage: Int, completeStage: Int,
                             rendered: kl_ovrp_render_pose, haveRendered: Bool,
                             originFromHead: simd_float4x4) -> Int {
        guard let first = viewIndices.first else { return 0 }
        let firstMap = drawable.views[first].textureMap

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture     = drawable.colorTextures[firstMap.textureIndex]
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
        if firstMap.textureIndex < drawable.depthTextures.count {
            pass.depthAttachment.texture     = drawable.depthTextures[firstMap.textureIndex]
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
        if layered {
            // The array length, not a slice: this is what lets the rate map's
            // per-eye layers be reached at all (see encodeComposite).
            pass.renderTargetArrayLength = viewIndices.count
        } else {
            pass.colorAttachments[0].slice = firstMap.sliceIndex
            if pass.depthAttachment.texture != nil {
                pass.depthAttachment.slice = firstMap.sliceIndex
            }
        }
        // Foveation. Metal rasterizes into the (smaller) physical texture and
        // the system unwarps on the way to the display; a pass that leaves this
        // nil while the drawable is foveated fills the physical texture
        // uniformly and the unwarp then distorts a correct picture.
        pass.rasterizationRateMap = rateMap

        guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return 0 }
        enc.label = "klepton composite view\(viewIndices)"
        if probe == 1 { enc.endEncoding(); return viewIndices.count }
        guard let pipeline, let sampler else { enc.endEncoding(); return 0 }

        // One uniform per view, in the order the viewports and the amplification
        // mappings are given — the vertex shader indexes this array by
        // [[amplification_id]].
        var uniforms: [kl_reproject_uniforms] = []
        // Which texture each view samples, in the PASS's own order — index n
        // here is the [[amplification_id]] the vertex shader reads, so it is
        // parallel to `uniforms` and must be appended to on every path through
        // the loop, including the ones that skip a view.
        var sources: [MTLTexture?] = []
        var visible = 0
        for vi in viewIndices {
            let view = drawable.views[vi]
            let a = eyeSource(vi, stage)
            // No eye texture yet — the guest is still in _begin, or it has not
            // reached ovrp_SetupEyeTexture2 for this stage. The pass still runs,
            // because the clear above is the point: a drawable that is presented
            // without being written shows whatever was in that texture last, and
            // with the guest taking its own time to start there are a great many
            // such frames.
            // ...and nil again when the guest's last frame carried no eye
            // picture at all. The eye textures outlive the frame that filled
            // them, so without this the eye keeps being drawn after the guest
            // has stopped submitting a projection layer — and on a guest that
            // alternates (JKXR: a projection pair in the world, a quad in the
            // menu, out of the SAME swapchains) that is one screen composited
            // twice, full-field underneath its own panel. kl_ovrp.h has the
            // measurement.
            let alloc = (completeStage < 0 || kl_ovrp_eye_layer_live() == 0) ? nil : a
            var u = withUnsafePointer(to: rendered) { r in
                kl_reproject_build(haveRendered ? r : nil, Int32(vi),
                                   originFromHead,
                                   Self.headFromView(view.transform,
                                                     midpoint: eyeMidpointInDevice),
                                   drawable.computeProjection(viewIndex: vi),
                                   UInt32(alloc?.slice ?? 0),
                                   (alloc?.flipY ?? false) ? 1 : 0,
                                   // Shared grid for now; the block per eye is
                                   // assigned below, once the grid is known.
                                   0)
            }
            // KL_CP_EYE=<0|1> — composite ONLY that eye and leave the other
            // cleared. The measurement this whole hunt has been missing: it
            // separates BINOCULAR doubling (the two eyes disagree and cannot be
            // fused — the failure every fix so far has assumed) from TEMPORAL
            // doubling (one eye seeing two copies over time, i.e. a frame held
            // across display periods, which no amount of per-eye correctness
            // touches). If the doubling survives with one eye blanked, it is
            // temporal and everything tried so far was aimed at the wrong half.
            if let only = Self.onlyEye, only != vi {
                u.visible = 0
                uniforms.append(u)
                sources.append(nil)
                continue
            }
            u.visible = alloc == nil ? 0 : 1
            uniforms.append(u)
            sources.append(alloc?.texture)
        }

        // The two texture slots the shader has, and which view reaches which.
        //
        // This used to bind ONE texture and drop any view whose eye was not
        // identical to it, on the reasoning that both eyes share a single
        // 2-slice array. They do for every guest that goes through the eye
        // provider, which allocates exactly that, and for a Vulkan guest whose
        // eye IS the layer. They do NOT for an OpenXR guest on Vulkan: one
        // swapchain per eye means two unrelated MTLTextures, and the right eye
        // was dropped from the composite for the whole run — the log said so
        // and nothing joined it to a person seeing one eye go dark. Open Brush
        // is the first guest of that shape.
        //
        // So the shader takes two textures and each view says which is its own
        // (kl_reproject.c, `src`). View 0 samples slot 0 and every other view
        // samples slot 1, which is the two eyes; when they really do share one
        // texture it is bound twice and this reduces to what it was, bit for
        // bit, for every guest that has run here.
        let bind0 = sources.first.flatMap { $0 } ?? sources.compactMap { $0 }.first
        let bind1 = (sources.count > 1 ? sources[1] : nil) ?? bind0
        // No eye texture is not the same as no picture, and this used to return
        // here as if it were. A guest can present its whole frame as an OpenXR
        // QUAD layer and submit no projection layer at all — JKXR does, every
        // frame — so the eye pass is skipped and the overlay pass at the end of
        // this function is the composite. Everything between is the eye's.
        let haveEyes = bind0 != nil && bind1 != nil
        if !haveEyes {
            for n in uniforms.indices { uniforms[n].visible = 0 }
        }
        for n in uniforms.indices where haveEyes && uniforms[n].visible != 0 {
            // What the fragment stage will actually sample for this view. A
            // third distinct texture cannot be named — there are two slots —
            // so it is dropped rather than shown another eye's picture, which
            // is the one thing the old gate got right.
            if sources[n] === (n == 0 ? bind0 : bind1) {
                visible += 1
            } else {
                uniforms[n].visible = 0
                if !loggedSplitEyes {
                    loggedSplitEyes = true
                    NSLog("[cp] view \(viewIndices[n]) samples a third distinct "
                          + "eye texture at stage \(stage) — dropped from the "
                          + "composite (the shader binds two)")
                }
            }
        }
        // The viewport matters here in a way it did not for a full-screen
        // triangle: the quad is projected, so it lands where the projection
        // says, and the view's own bounds within a shared texture are part of
        // that. With amplification there is one per view, selected by the
        // mapping below rather than by the shader.
        enc.setViewports(viewIndices.map { drawable.views[$0].textureMap.viewport })
        if layered && viewIndices.count > 1 {
            var mappings = viewIndices.enumerated().map { (n, vi) in
                MTLVertexAmplificationViewMapping(
                    viewportArrayIndexOffset: UInt32(n),
                    renderTargetArrayIndexOffset: UInt32(drawable.views[vi].textureMap.sliceIndex))
            }
            enc.setVertexAmplificationCount(viewIndices.count, viewMappings: &mappings)
        }

        // The PER-LAYER composite, when the guest's last frame described its
        // projection layers one by one. It replaces everything the eye pass
        // does below — the layers ARE the eye picture — so `visible` comes from
        // it and the eye branch is skipped entirely. Zero is every guest that
        // files no list, which is every guest but Steam Link and every run
        // under KL_XR_LAYERS=0, and those take the eye pass unchanged.
        let nproj = projLayerCount
        if nproj > 0 {
            visible = encodeProjLayers(enc, viewIndices: viewIndices,
                                       drawable: drawable,
                                       originFromHead: originFromHead,
                                       count: nproj) > 0 ? viewIndices.count : 0
        }
        // The EYE pass, and only when there is an eye. `haveEyes` is false for
        // a guest whose whole picture is a composition layer (see above), and
        // the overlay pass below is then the entire composite rather than a
        // decoration on top of one.
        if nproj == 0, visible > 0, let bind0, let bind1 {
            if !loggedEyeTextures {
                loggedEyeTextures = true
                NSLog("[cp] eye textures: slot0 \(bind0.width)x\(bind0.height) "
                      + "type \(bind0.textureType.rawValue), slot1 "
                      + (bind1 === bind0 ? "= slot0 (the eyes share one texture)"
                                         : "\(bind1.width)x\(bind1.height) "
                                           + "type \(bind1.textureType.rawValue) "
                                           + "(one swapchain per eye)"))
            }

            // What each eye is ACTUALLY being placed with, said once per pass shape.
            //
            // This exists because a stereo geometry error has no error surface at
            // all: every call succeeds, the counters stay healthy, and the only
            // instrument is a person wearing the headset saying one eye looks wrong
            // — which cannot say WHICH of three things is wrong. The three are
            // separable from these numbers alone:
            //
            //   * `slice` — eye 1 sampling slice 0 is the right eye showing the
            //     LEFT eye's picture. Placed with eye 1's quad, that is a ~20 degree
            //     horizontal shift, because the display's per-eye tangents are
            //     mirror images (l=1.73/r=1.00 against l=1.00/r=1.73).
            //   * `tan` — the same shift with the pictures the right way round, if
            //     the quad is built with the other eye's frustum.
            //   * `rec=no` — no frame record, so BOTH quads fall back to the
            //     symmetric 90 degree default while the guest rendered asymmetric
            //     pictures. That one is wrong in both eyes, unequally.
            //
            // Keyed on the values rather than a bare `once`: the interesting case is
            // the one where they CHANGE (a stage re-created, a swapchain rebuilt),
            // and a first-frame-only line is exactly what would miss it.
            let shape = viewIndices.map { vi -> String in
                let u = uniforms[viewIndices.firstIndex(of: vi)!]
                return String(format: "v%d slice=%u tan(l%.3f r%.3f t%.3f b%.3f) vis=%u flip=%u",
                              vi, u.slice, u.tangents.x, u.tangents.y, u.tangents.z,
                              u.tangents.w, u.visible, u.flip_y)
            }.joined(separator: " | ")
            if shape != lastEyeShape {
                lastEyeShape = shape
                NSLog("[cp] eye placement: \(shape) | rec=\(haveRendered ? "yes" : "no") "
                      + "layered=\(layered) stage=\(stage)")
            }

            // ---------------------------------------------------------------
            // The crop, from the same frame record `rendered` came from — and it is
            // read PER EYE.
            //
            // The comment that stood here said one grid serves the amplified pass,
            // so two eyes with different viewports could not both be honoured, took
            // eye 0's, and justified it: Unity derives both from a single
            // `renderViewportScale`, so they are equal in every measured run. That
            // is true of a title that scales its resolution and FALSE of one using
            // Oculus symmetric projection, where both eyes are rendered with one
            // union frustum into a widened texture and each eye's own cone is a
            // different sub-rect of it — measured on BONELAB, eye 0 at x=0 and eye 1
            // at x=609, both 2271 wide of 2880. Eye 1 read
            // through eye 0's crop is its picture shifted by that offset, with every
            // call on the path returning success.
            //
            // So the grid carries a BLOCK PER EYE when the two rects differ, and one
            // block when they do not — which is bit-for-bit the old path for every
            // guest that has ever run here.
            var vp0 = SIMD4<Float>.zero
            var vp1 = SIMD4<Float>.zero
            var vpOf = SIMD2<Float>.zero
            if haveRendered {
                let l = rendered.viewport.0, r = rendered.viewport.1
                vp0 = SIMD4<Float>(Float(l.0), Float(l.1), Float(l.2), Float(l.3))
                vp1 = SIMD4<Float>(Float(r.0), Float(r.1), Float(r.2), Float(r.3))
                vpOf = SIMD2<Float>(Float(rendered.viewport_of.0),
                                    Float(rendered.viewport_of.1))
                // Said once, and said even when it is ZERO — which is the whole
                // point. `[ovrp] render viewport` is printed by the guest's own
                // EndFrame4 on the guest's thread, and until this line existed there
                // was nothing anywhere saying whether the number reached the
                // COMPOSITE. Those are different failures with one symptom: a rect
                // the submit never carried, a rect filed against another stage, and
                // a rect read correctly and cropped wrongly all look identical from
                // inside the headset. Zeroes here beside a non-100% line there is
                // the first; agreement is the third.
                if !loggedReadViewport {
                    loggedReadViewport = true
                    NSLog("[cp] first render viewport read from the frame record: "
                          + "\(l.0),\(l.1) \(l.2)x\(l.3) (stage \(stage), serial "
                          + "\(rendered.serial))"
                          + (l.2 <= 0 ? " — ZERO, so no crop is applied; compare "
                                      + "against the [ovrp] render viewport line" : ""))
                }
            }
            // Built from slot 0. The grid is a function of the eye texture's SIZE
            // and its rate map, and the two eyes agree on both — a guest that
            // rendered them at different sizes would already be failing the
            // viewport arithmetic below.
            let (gridBuf, gridVerts, gridPerEye) =
                unwarpGrid(bind0, viewport0: vp0, viewport1: vp1, viewportOf: vpOf)
            // ...and which block each view reads. Assigned here rather than passed
            // to kl_reproject_build above because the decision needs the eye
            // texture, which is not known until the loop that builds the uniforms
            // has run — so the uniforms are built for a shared grid (grid_per_eye
            // 0, the default and the old behaviour) and only a grid that really
            // carries two blocks moves eye 1 onto its own.
            if gridPerEye {
                for (n, vi) in viewIndices.enumerated() where vi == 1 {
                    uniforms[n].grid_eye = 1
                }
            }
            enc.setRenderPipelineState(probePipeline ?? pipeline)
            if let depthState { enc.setDepthStencilState(depthState) }
            enc.setFragmentTexture(bind0, index: 0)
            enc.setFragmentTexture(bind1, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                // Vertex only: the fragment stage has no amplification_id to index
                // this array with, so the one thing it needs — the eye's array
                // slice — arrives as a flat varying instead (kl_reproject.c).
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            // The unwarp grid, bound to the VERTEX stage. Identity until the guest's
            // eye textures are foveated; after that it carries the squeeze this pass
            // has to undo. See unwarpGrid and kl_reproject.h.
            enc.setVertexBuffer(gridBuf, offset: 0, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: gridVerts)
        }
        let panels = encodeOverlays(enc, viewIndices: viewIndices, drawable: drawable,
                                    originFromHead: originFromHead,
                                    writeDepth: visible == 0)
        enc.endEncoding()
        // "How many eyes got a picture", which is what the caller counts black
        // frames with — and a panel IS a picture. Without this a guest that
        // composites only quad layers reports every frame black while showing
        // its menu correctly, which is an instrument contradicting the display.
        if visible > 0 { return visible }
        return panels > 0 ? viewIndices.count : 0
    }

    /// The guest's PROJECTION layers, each as its own quad with its own frustum,
    /// in submission order — the macOS viewer's klvm_draw_proj_layers, on the
    /// device. Returns how many were drawn, which is what the caller's
    /// black-frame count needs.
    ///
    /// This REPLACES the eye pass rather than adding to it: the layers are the
    /// eye picture, stated whole instead of flattened into one texture. A guest
    /// that files no list — every guest but Steam Link, and every run under
    /// `KL_XR_LAYERS=0` — takes the eye pass unchanged, so nothing here is on
    /// any other target's path.
    ///
    /// Ordering is the draw order and nothing else. Each layer is a quad at
    /// `KL_REPROJECT_DEPTH`, so they are coplanar and the depth test cannot
    /// separate them; back to front is what OpenXR specifies and what
    /// submission order means. Depth is written by the FIRST layer drawn — the
    /// backdrop — because visionOS reprojects using the depth buffer and a
    /// frame that writes none is treated as empty (see encodeOverlays), while
    /// letting every layer write would stamp a nearer quad's depth over the
    /// same plane for no gain.
    @discardableResult
    private func encodeProjLayers(_ enc: MTLRenderCommandEncoder,
                                  viewIndices: [Int],
                                  drawable: LayerRenderer.Drawable,
                                  originFromHead: simd_float4x4,
                                  count: Int) -> Int {
        guard let pipeline, let sampler else { return 0 }
        let grid = identityGrid()
        var drawn = 0
        for i in 0..<min(count, projLayers.count) {
            let pl = projLayers[i]
            // One uniform per amplified view, in the pass's own order — the
            // vertex shader indexes this by [[amplification_id]], so a view this
            // layer does not cover says so with visible = 0 rather than by not
            // being drawn.
            var uniforms: [kl_reproject_uniforms] = []
            var sources: [MTLTexture?] = []
            for vi in viewIndices {
                let slot = vi == 1 ? pl.slot.1 : pl.slot.0
                var tex: MTLTexture? = nil
                var slice: Int32 = 0
                if slot >= 0,
                   let t = kl_glfb_eye_mtl_texture(Int32(vi), slot, &slice) {
                    // Through the array view for the reason the eye pass is:
                    // every shader in kl_reproject.c samples a
                    // texture2d_array, and a plain 2D bound to one samples
                    // nothing with no error at all.
                    tex = arrayView(Unmanaged<MTLTexture>.fromOpaque(t)
                                        .takeUnretainedValue() as MTLTexture)
                }
                var u = withUnsafePointer(to: pl) {
                    kl_proj_layer_build($0, Int32(vi), originFromHead,
                                        Self.headFromView(drawable.views[vi].transform,
                                                          midpoint: eyeMidpointInDevice),
                                        drawable.computeProjection(viewIndex: vi),
                                        UInt32(slice))
                }
                if tex == nil { u.visible = 0 }
                if let only = Self.onlyEye, only != vi { u.visible = 0 }
                uniforms.append(u)
                sources.append(tex)
            }
            // Two texture slots, view 0 in the first and every other view in the
            // second — the eye pass's rule, and for the same reason: a layer's
            // two eyes are either two slices of one texture or two textures.
            let bind0 = sources.first.flatMap { $0 } ?? sources.compactMap { $0 }.first
            let bind1 = (sources.count > 1 ? sources[1] : nil) ?? bind0
            guard let bind0, let bind1,
                  uniforms.contains(where: { $0.visible != 0 }) else {
                // Named once per layer index: "the guest submitted a layer we
                // cannot reach" and "the guest submitted fewer layers" are the
                // same picture, which is the failure this pass exists to end.
                if !loggedProjMiss.contains(i) {
                    loggedProjMiss.insert(i)
                    NSLog("[cp] projection layer \(i) (slots \(pl.slot.0)/\(pl.slot.1)) "
                          + "has no MTLTexture — not composited")
                }
                continue
            }
            // A view whose texture is neither bound slot is dropped rather than
            // shown another eye's picture — the eye pass's rule again.
            for n in uniforms.indices where uniforms[n].visible != 0 {
                if sources[n] !== (n == 0 ? bind0 : bind1) { uniforms[n].visible = 0 }
            }
            // The FIRST layer drawn writes depth and every one after it only
            // passes. visionOS reprojects from the depth buffer, so the frame
            // must carry some — but the layers are coplanar at
            // KL_REPROJECT_DEPTH, and a `.greater` test against a depth already
            // written discards every one of them.
            if let d = drawn == 0 ? depthState : depthPassState {
                enc.setDepthStencilState(d)
            }
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentTexture(bind0, index: 0)
            enc.setFragmentTexture(bind1, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            enc.setVertexBuffer(grid, offset: 0, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0,
                               vertexCount: Int(kl_reproject_grid_vertices(1, 1)))
            drawn += 1
        }
        // What each layer is ACTUALLY being placed with, said when it CHANGES —
        // this guest's layer arrangement moves while it runs (a layer appears
        // and disappears about once a second, and one layer's frustum halves
        // and doubles), so a first-frame-only line describes a shape the run
        // spends most of its time not being in.
        if drawn > 0 {
            let shape = "\(count) layer(s), \(drawn) composited"
            if shape != lastProjShape {
                lastProjShape = shape
                NSLog("[cp] per-layer composite: \(shape)")
            }
        }
        return drawn
    }

    /// The identity unwarp grid, built once. The per-layer pass binds this
    /// rather than `unwarpGrid`'s: a layer's picture is a whole guest swapchain
    /// image copied as it is, so there is no rate map to undo and no render
    /// viewport smaller than the texture.
    private func identityGrid() -> MTLBuffer? {
        if identityGridBuffer == nil {
            var tmp = [SIMD2<Float>](repeating: .zero,
                                     count: Int(kl_reproject_grid_entries(1, 1)))
            tmp.withUnsafeMutableBufferPointer { kl_reproject_grid_identity($0.baseAddress) }
            identityGridBuffer = tmp.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: [])
            }
        }
        return identityGridBuffer
    }
    /// This frame's projection layers, taken whole and taken EARLY — see where
    /// it is filled for why the read order against the frame fence matters.
    /// Read only on the render loop, which is also the only thread that writes
    /// it, so it needs no lock of its own.
    private var projLayers = [kl_ovrp_proj_layer](repeating: kl_ovrp_proj_layer(),
                                                  count: 8)
    private var projLayerCount = 0
    private var identityGridBuffer: MTLBuffer?
    private var loggedProjMiss = Set<Int>()
    private var lastProjShape = ""

    /// The layers that are NOT the eye, drawn on top of it in the same encoder.
    ///
    /// `ovrp_EndFrame4` is handed a list and every reader of it here used to walk
    /// past anything but the eye layer. That is right for the frame record and it
    /// silently dropped everything else. Unity's only other layer is a 1x1 nothing
    /// renders into; **Unreal draws its UI this way**, so on RE4 it dropped the
    /// studio splash, the loading screens and the menus while the world
    /// composited perfectly.
    ///
    /// No depth ordering is needed: the eye quad sits at `KL_REPROJECT_DEPTH`
    /// (500 m) and an overlay is a few metres away, so it is in front by geometry.
    /// `KL_GUEST_OVERLAYS=0` is the A/B. Deliberately not `KL_OVERLAYS`, which is
    /// already the SYSTEM overlays (the Home indicator, KleptonApp.swift) — two
    /// knobs one letter apart, fighting over one name, is a bug with no error
    /// surface at all.
    ///
    /// Returns how many layers were actually drawn — for a guest whose whole
    /// frame is a quad that IS the picture, and the caller's black-frame count
    /// depends on knowing it.
    ///
    /// `writeDepth` is set when the overlays ARE the frame — the eye pass drew
    /// nothing, so nothing else in this pass will write depth.
    ///
    /// That distinction is the whole of it. visionOS reprojects the submitted
    /// frame using its depth buffer, and the encoder's default state writes no
    /// depth at all, so a frame whose only content is a quad layer arrives as
    /// perfect colour over a depth buffer still holding the reverse-Z clear of
    /// 0 — "nothing is here at any finite distance" — and the compositor
    /// honours that literally. It is the same measurement the eye pipeline's
    /// depth state records: a quad with writes off is invisible, the same quad
    /// with writes on appears. It never showed on the guest this pass was
    /// written for, because an Unreal title draws its eye first and its UI
    /// second, so the depth was already there to ride on.
    ///
    /// Left OFF when the eye pass did draw, which is not timidity: an overlay
    /// blends, so a transparent region of a UI quad would otherwise stamp the
    /// quad's near depth over the world behind it and reproject that patch of
    /// the world at the panel's distance.
    @discardableResult
    private func encodeOverlays(_ enc: MTLRenderCommandEncoder,
                                viewIndices: [Int],
                                drawable: LayerRenderer.Drawable,
                                originFromHead: simd_float4x4,
                                writeDepth: Bool) -> Int {
        guard overlaysEnabled, let pipe = overlayPipeline else { return 0 }
        // .greater against that same clear, and the shader's own projected
        // position carries the quad's real distance, so what lands in the
        // buffer is where the panel actually is.
        if writeDepth, let depthState { enc.setDepthStencilState(depthState) }
        let n = Int(kl_ovrp_overlay_count())
        if n == 0 { return 0 }
        var drawn = 0
        for i in 0..<n {
            var ov = kl_ovrp_overlay()
            guard kl_ovrp_overlay_get(Int32(i), &ov) != 0 else { continue }
            // One uniform per amplified view, exactly as the eye pass does — the
            // array is indexed by [[amplification_id]] and a view with no
            // placement says so with `visible = 0` rather than by not drawing,
            // because with amplification there is one draw for both.
            var uniforms: [kl_overlay_uniforms] = []
            var texture: MTLTexture? = nil
            for vi in viewIndices {
                var w: Int32 = 0, h: Int32 = 0
                // Two producers, one consumer, asked in the order they can
                // answer: a Vulkan guest's layer image belongs to kl_vulkan, and
                // everything else reaches a compositor through kl_glfb's layer
                // table — an OpenXR quad on GL is bound there and a Vulkan one
                // is recorded there too. Vulkan first, so RE4's path is
                // untouched.
                let t = kl_vulkan_layer_mtl_texture(ov.layer_id, ov.stage, Int32(vi), &w, &h)
                    ?? kl_glfb_layer_mtl_texture(ov.layer_id, ov.stage, &w, &h)
                if texture == nil, let t { texture = Unmanaged<MTLTexture>
                    .fromOpaque(t).takeUnretainedValue() }
                var u = kl_overlay_build(&ov, Int32(vi), originFromHead,
                                         Self.headFromView(drawable.views[vi].transform,
                                                           midpoint: eyeMidpointInDevice),
                                         drawable.computeProjection(viewIndex: vi))
                if t == nil { u.visible = 0 }
                uniforms.append(u)
            }
            guard let texture, uniforms.contains(where: { $0.visible != 0 }) else {
                // Named once per layer: "the guest submitted a layer we cannot
                // reach" and "the guest submitted none" are the same picture.
                if !loggedOverlayMiss.contains(ov.layer_id) {
                    loggedOverlayMiss.insert(ov.layer_id)
                    NSLog("[cp] overlay layer \(ov.layer_id) (shape \(ov.shape), stage "
                          + "\(ov.stage), \(ov.size.0)x\(ov.size.1) m) has no placement "
                          + "or no MTLTexture — not composited")
                }
                continue
            }
            enc.setRenderPipelineState(pipe)
            enc.setFragmentTexture(texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
                enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            drawn += 1
            if !loggedOverlayDraw.contains(ov.layer_id) {
                loggedOverlayDraw.insert(ov.layer_id)
                NSLog("[cp] compositing overlay layer \(ov.layer_id): "
                      + "\(ov.size.0)x\(ov.size.1) m at (\(ov.pose.4), \(ov.pose.5), "
                      + "\(ov.pose.6))"
                      + (ov.head_locked != 0 ? ", HEAD-LOCKED" : ""))
            }
        }
        return drawn
    }
    private let overlaysEnabled = klEnvOn("KL_GUEST_OVERLAYS", default: true)
    private var loggedOverlayMiss = Set<Int32>()
    private var loggedOverlayDraw = Set<Int32>()
    private var loggedSplitEyes = false
    /// Said once: what the two texture slots resolved to, and whether the eyes
    /// share one texture or arrive as one swapchain each. Two guests of
    /// different shapes composite through this one pass and nothing else says
    /// which shape a run is.
    private var loggedEyeTextures = false
    /// The last per-eye placement described by `[cp] eye placement`, so the line
    /// is printed when it CHANGES rather than only on the first frame.
    private var lastEyeShape = ""
    /// KL_CP_EYE=<0|1>, the binocular/temporal split. See encodeViews.
    private static let onlyEye: Int? = {
        guard let e = ProcessInfo.processInfo.environment["KL_CP_EYE"],
              let v = Int(e), v == 0 || v == 1 else { return nil }
        NSLog("[cp] KL_CP_EYE=\(v) — compositing that eye ONLY; the other "
              + "stays cleared. Doubling that survives this is TEMPORAL.")
        return v
    }()

    /// Did this frame get its own display period, or is it being held?
    ///
    /// Two numbers, and they answer different questions. The **gap** between
    /// consecutive presentation times says what the system did with our frames:
    /// 1.00 periods means every frame we submit is shown once, 2.00 means each
    /// is being shown twice and half our reprojections never reach the display.
    /// The **deadline** says whether that is our fault: encoding that finishes
    /// after `renderingDeadline` is a frame the system had to fall back on.
    ///
    /// A picture can be several frames stale and still look perfect here, which
    /// is the point — staleness is reprojection's job and it does it every
    /// period. A repeat is the case reprojection cannot reach.
    private func noteCadence(presentation: TimeInterval,
                             drawable: LayerRenderer.Drawable) {
        // The deadline for THIS frame, against the same clock it is expressed
        // in. Checked after commit, so it measures the encode we just did.
        let now = LayerRenderer.Clock.Instant.epoch
            .duration(to: LayerRenderer.Clock().now).timeInterval
        let deadline = LayerRenderer.Clock.Instant.epoch
            .duration(to: drawable.frameTiming.renderingDeadline).timeInterval
        if now > deadline { missedDeadline += 1 }

        defer { lastPresentation = presentation }
        guard lastPresentation > 0 else { return }
        // The period, from whatever source has one. primeDisplay's measurement
        // is the good one, but it can fail to get eight drawables — and a
        // cadence report that silently does not exist because a DIFFERENT
        // measurement failed is exactly the kind of instrument that wastes a
        // device run. Falling back to the rate the guest is being told keeps
        // the ratio meaningful; failing that the raw milliseconds still are.
        if displayPeriod <= 0 {
            let hz = Double(kl_ovrp_display_frequency())
            displayPeriod = hz > 1 ? 1.0 / hz : 0
        }
        let seconds = presentation - lastPresentation
        let gap = displayPeriod > 0 ? seconds / displayPeriod : 0
        // A gap of zero or a negative one is the same drawable seen twice or a
        // clock that moved backwards; neither is a cadence measurement.
        guard seconds > 0 else { return }
        cadenceN += 1
        cadenceSum += seconds
        cadenceMax = max(cadenceMax, seconds)
        if gap >= 1.5 { cadenceRepeats += 1 }
    }

    /// The cadence numbers, for the liveness line — which is the one report
    /// here that is known to print, on a wall clock, whatever else has failed.
    /// Reported there rather than on a sample count of its own so that "the
    /// cadence line is missing" can only ever mean "no frames were presented".
    private func cadenceSummary() -> String {
        guard cadenceN > 0 else { return "cadence n/a (no presents yet)" }
        let meanS = cadenceSum / Double(cadenceN)
        let periods = displayPeriod > 0 ? meanS / displayPeriod : 0
        let out = String(format: "cadence %.2f ms mean / %.2f max", meanS * 1000,
                         cadenceMax * 1000)
            + (displayPeriod > 0 ? String(format: " = %.2f periods", periods) : "")
            + ", \(cadenceRepeats)/\(cadenceN) held >=1.5, "
            + "\(missedDeadline) past deadline"
        cadenceN = 0; cadenceSum = 0; cadenceMax = 0
        cadenceRepeats = 0; missedDeadline = 0
        return out
    }

    /// Harvest what only a drawable can tell us, before the guest exists to
    /// ask: the per-eye frustum and the display's real frame rate.
    ///
    /// An ordering problem. Both numbers can only be read from a `cp_drawable`,
    /// which arrives once the frame loop is running, but the guest reads both
    /// ONCE and early — Unity latches the frustum when its VRDevice initializes
    /// and the display frequency into its VR timing config. Answering correctly
    /// one frame later answers a question nobody asks again. So a throwaway
    /// frame is rendered here purely to harvest them, before
    /// `kl_app_lifecycle_begin()`.
    ///
    /// Tangents come from the projection matrix rather than `cp_view_tangents`,
    /// which visionOS 2.0 deprecated, and the matrix is what the composite pass
    /// projects with.
    /// Tell the guest where this display's eyes actually are.
    ///
    /// `view.transform` is `device_from_view`, so its translation is the
    /// head->eye offset in the head's own frame, which is what
    /// `kl_ovrp_set_eye_offset` wants. Without it nodes 0 and 1 both answer the
    /// head pose: an IPD of zero, both eyes rendering from the same point, and
    /// a world with no disparity in it.
    ///
    /// Pushed from `primeDisplay` so it is right before the guest's first
    /// frame, and again each frame because it costs two stores and the user can
    /// change the fit of the headset mid-run.
    ///
    /// Tell the guest how big its eye render targets should be.
    ///
    /// Everything else about the synthetic headset describes a Quest 2 on
    /// purpose — `Build.MODEL`, `ovrp_GetSystemHeadsetType`, the version string
    /// — because every Oculus branch in the guest is written against one. The
    /// eye SIZE is different in kind: nothing branches on it, it is just a
    /// resolution, and the Quest's 2290x2400 is wrong in both axes for the
    /// display underneath. So it is measured rather than claimed.
    ///
    /// Which number: with foveation on, the drawable's colour textures are the
    /// PHYSICAL size — already reduced — while the rate map's `screenSize`
    /// is the logical resolution the fovea is rasterized at. The guest should be
    /// sized against the latter: matching the physical size would throw away
    /// exactly the resolution `maxRenderQuality` was set to buy. Without a rate
    /// map there is no distinction and the texture size is the answer.
    ///
    /// The clamping lives in `kl_ovrp_set_eye_texture_size`, not here, so a host
    /// run through `t_mtl_provider` gets the same policy.

    // MARK: - The unwarp grid

    /// Screen -> physical, as `kl_reproject_grid_build` wants it. No captures,
    /// so it converts to a C function pointer.
    private static let screenToPhysical: kl_reproject_s2p = { ctx, sx, sy, px, py in
        guard let ctx,
              let map = Unmanaged<AnyObject>.fromOpaque(ctx)
                  .takeUnretainedValue() as? MTLRasterizationRateMap else { return }
        let p = map.physicalCoordinates(
            screenCoordinates: MTLSamplePosition(x: sx, y: sy), layer: 0)
        px?.pointee = p.x
        py?.pointee = p.y
    }

    private var gridBuffer: MTLBuffer?
    private var gridMap: UnsafeMutableRawPointer?
    private var gridTexW = -1, gridTexH = -1
    private var gridVertexCount = 0
    private var gridViewport: SIMD4<Float> = .zero
    /// Eye 1's rect, and whether the built grid carries a block for it. Part of
    /// the cache key: a guest that starts using symmetric projection partway
    /// through (BONELAB does — the plugin turns it on once the eye textures are
    /// sized) changes only this one, and a grid keyed on eye 0's alone would
    /// never be rebuilt.
    private var gridViewportR: SIMD4<Float> = .zero
    private var gridPerEye = false
    private var loggedSplitViewports = false
    private var loggedReadViewport = false
    private var loggedViewportOfSkew = false

    /// The reprojection quad's mesh, with texture coordinates that already undo
    /// the guest's foveation.
    ///
    /// Rebuilt only when the rate map or the eye texture's size changes — a rate
    /// map is static, so the unwarp is resolved once here and the rasterizer
    /// interpolates it for free, instead of every fragment paying a
    /// `map_screen_to_physical_coordinates` decode. One cell per ZONE, so the
    /// grid's vertices land on the map's own piecewise-linear breakpoints and
    /// the result is exact rather than a sampling of the curve.
    ///
    /// Note this is the GUEST's map (kl_glfb_eye_rate_map), which has nothing to
    /// do with the drawable's own foveation: Compositor Services rasterizes THIS
    /// pass through its own map, set on the render pass descriptor. Two maps,
    /// two purposes, deliberately detached.
    ///
    /// `viewport` is the guest's render viewport for this frame in eye-texture
    /// pixels, `.zero` for the whole texture — kl_ovrp_render_pose.viewport,
    /// taken from the same frame record the uniforms come from. It is part of
    /// the key: Beat Saber shrinks it on entering a map (the render-viewport
    /// note in kl_ovrp.h), and a grid built for the previous viewport crops the
    /// wrong rectangle with nothing anywhere reporting it.
    /// Returns the grid, the vertex count, and whether it carries a BLOCK PER
    /// EYE — which it does exactly when the two eyes submitted different rects.
    private func unwarpGrid(_ source: MTLTexture?,
                            viewport0: SIMD4<Float>,
                            viewport1: SIMD4<Float>,
                            viewportOf: SIMD2<Float>) -> (MTLBuffer?, Int, Bool) {
        let map = kl_glfb_eye_rate_map()
        let tw = source?.width ?? 0, th = source?.height ?? 0
        // A viewport that IS the whole texture is not a crop, and keeping it as
        // .zero means an unscaled title never leaves the pre-existing path.
        //
        // **A rect measured against another texture is not a crop, it is a
        // coincidence.** `viewportOf` is the eye texture size the guest's submit
        // was relative to (kl_ovrp.h); when it is not the texture in hand, the
        // rect says nothing about which texels of THIS one were written, and
        // cropping by it magnifies the picture by the ratio of the two — 1.2x on
        // device, which reads as a distorted, cross-eyed view rather than as a
        // cropping bug, because the binocular disparity is magnified with it.
        // Measured on a headset, 2026-08-12: a 3072x2464 rect against a
        // 3686x2956 texture, persistently, after Unity's fourth eye layer.
        //
        // The cause is fixed in klovrp_SetupLayer. This is the guard, and it is
        // deliberately NOT a rescale: with the two disagreeing we do not know
        // which region the guest wrote, and inventing one is how the first
        // version of this went wrong. Not cropping is the pre-viewport
        // behaviour — visibly wrong if it ever fires, which is the point.
        //
        // Applied to BOTH eyes, and it is one decision for the pair: a texture
        // the rects do not describe does not describe either of them.
        func sanitise(_ r: SIMD4<Float>) -> SIMD4<Float> {
            var vp = r
            if vp.z > 0, viewportOf.x > 0, viewportOf.y > 0,
               Int(viewportOf.x) != tw || Int(viewportOf.y) != th {
                if !loggedViewportOfSkew {
                    loggedViewportOfSkew = true
                    NSLog("[cp] the render viewport is \(Int(vp.z))x\(Int(vp.w)) of a "
                          + "\(Int(viewportOf.x))x\(Int(viewportOf.y)) eye texture, but "
                          + "the texture in hand is \(tw)x\(th) — NOT cropping, because "
                          + "that rect describes another texture (kl_ovrp.h viewport_of)")
                }
                vp = .zero
            }
            if vp.z <= 0 || vp.w <= 0 ||
                (vp.x == 0 && vp.y == 0 && Int(vp.z) == tw && Int(vp.w) == th) { vp = .zero }
            return vp
        }
        let vp = sanitise(viewport0)
        let vpR = sanitise(viewport1)
        // Two blocks only when the eyes really disagree. Equal rects — every
        // guest measured here before BONELAB's symmetric projection — take the
        // single-block path unchanged, cache key and all.
        let perEye = vp != vpR
        if perEye, !loggedSplitViewports {
            loggedSplitViewports = true
            NSLog("[cp] the eyes submitted DIFFERENT render viewports "
                  + "(\(Int(vp.x)),\(Int(vp.y)) \(Int(vp.z))x\(Int(vp.w)) vs "
                  + "\(Int(vpR.x)),\(Int(vpR.y)) \(Int(vpR.z))x\(Int(vpR.w))) "
                  + "— the unwarp grid carries a block per eye")
        }
        let cropped = vp.z > 0 && vp.w > 0
        let croppedR = vpR.z > 0 && vpR.w > 0
        let eyes: UInt32 = perEye ? 2 : 1
        if gridBuffer == nil || map != gridMap || tw != gridTexW || th != gridTexH
            || vp != gridViewport || vpR != gridViewportR {
            gridMap = map; gridTexW = tw; gridTexH = th
            gridViewport = vp; gridViewportR = vpR
            var nx: UInt32 = 1, ny: UInt32 = 1
            if let map, tw > 0, th > 0,
               let rm = Unmanaged<AnyObject>.fromOpaque(map)
                   .takeUnretainedValue() as? MTLRasterizationRateMap {
                var zx: Int32 = 0, zy: Int32 = 0
                kl_glfb_eye_rate_zones(&zx, &zy)
                if zx <= 0 || zy <= 0 {
                    // A map we did not build does not report its zones, but the
                    // GRANULARITY cell count is recoverable — the trick ALVR's
                    // DummyMetalRenderer uses to reconstruct a descriptor.
                    // Uniform in physical rather than screen space, so it is a
                    // dense sampling of the curve rather than zone-aligned and
                    // exact; at ~32px granules the residual is under a texel.
                    let gran = rm.physicalGranularity
                    let phys = rm.physicalSize(layer: 0)
                    zx = gran.width  > 0 ? Int32(phys.width  / gran.width)  : 16
                    zy = gran.height > 0 ? Int32(phys.height / gran.height) : 16
                }
                let cap = Int32(KL_REPROJECT_GRID_MAX)
                nx = UInt32(max(1, min(cap, zx)))
                ny = UInt32(max(1, min(cap, zy)))
                let n = Int(kl_reproject_grid_entries_n(nx, ny, eyes))
                var table = [SIMD2<Float>](repeating: .zero, count: n)
                let scr = rm.screenSize
                var rects = [[vp.x, vp.y, vp.z, vp.w], [vpR.x, vpR.y, vpR.z, vpR.w]]
                let croppedEye = [cropped, croppedR]
                for e in 0..<Int(eyes) {
                    table.withUnsafeMutableBufferPointer { buf in
                        rects[e].withUnsafeBufferPointer { r in
                            kl_reproject_grid_build_eye(buf.baseAddress, UInt32(e), nx, ny,
                                                        croppedEye[e] ? r.baseAddress : nil,
                                                        Float(scr.width), Float(scr.height),
                                                        Float(tw), Float(th),
                                                        Self.screenToPhysical, map)
                        }
                    }
                }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
                NSLog("[cp] unwarp grid \(nx)x\(ny)\(perEye ? " x2 eyes" : "") for a "
                      + "\(tw)x\(th) eye texture "
                      + "(map screen \(scr.width)x\(scr.height), viewport "
                      + (cropped ? "\(Int(vp.x)),\(Int(vp.y)) \(Int(vp.z))x\(Int(vp.w))"
                                 : "whole texture")
                      + (perEye ? " / \(Int(vpR.x)),\(Int(vpR.y)) "
                                  + "\(Int(vpR.z))x\(Int(vpR.w))" : "") + ")")
            } else if cropped || croppedR {
                // No rate map, and still a crop to do. One cell suffices: with
                // no map the screen->texture mapping is affine, so the four
                // corners carry it exactly. THIS is the path a KL_VRR=0 run
                // takes, and the one the corner-of-the-eye bug lived on — and
                // the one BONELAB's symmetric projection lands on today.
                let n = Int(kl_reproject_grid_entries_n(1, 1, eyes))
                var table = [SIMD2<Float>](repeating: .zero, count: n)
                var rects = [[vp.x, vp.y, vp.z, vp.w], [vpR.x, vpR.y, vpR.z, vpR.w]]
                let croppedEye = [cropped, croppedR]
                for e in 0..<Int(eyes) {
                    table.withUnsafeMutableBufferPointer { buf in
                        rects[e].withUnsafeBufferPointer { r in
                            kl_reproject_grid_build_eye(buf.baseAddress, UInt32(e), 1, 1,
                                                        croppedEye[e] ? r.baseAddress : nil,
                                                        Float(tw), Float(th),
                                                        Float(tw), Float(th), nil, nil)
                        }
                    }
                }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
                NSLog("[cp] render viewport \(Int(vp.x)),\(Int(vp.y)) "
                      + "\(Int(vp.z))x\(Int(vp.w))"
                      + (perEye ? " / \(Int(vpR.x)),\(Int(vpR.y)) "
                                  + "\(Int(vpR.z))x\(Int(vpR.w))" : "")
                      + " of a \(tw)x\(th) eye texture — compositing that sub-rect")
            } else {
                let n = Int(kl_reproject_grid_entries(1, 1))
                var table = [SIMD2<Float>](repeating: .zero, count: n)
                table.withUnsafeMutableBufferPointer { kl_reproject_grid_identity($0.baseAddress) }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
            }
            gridVertexCount = Int(kl_reproject_grid_vertices(nx, ny))
            gridPerEye = perEye
        }
        return (gridBuffer, gridVertexCount, gridPerEye)
    }

    private func pushEyeTextureSize(_ drawable: LayerRenderer.Drawable) {
        var w = 0, h = 0
        var source = "none"
        if let rm = drawable.rasterizationRateMaps.first {
            w = Int(rm.screenSize.width); h = Int(rm.screenSize.height)
            source = "rate map screenSize"
        } else if let t = drawable.colorTextures.first {
            w = t.width; h = t.height
            source = "colour texture"
        }
        guard w > 0, h > 0 else {
            NSLog("[cp] no drawable size to push — the guest keeps its default eye size")
            return
        }
        // The per-view viewport, logged rather than used. In a layered drawable
        // each view is a whole slice and the viewport is the full texture, so it
        // says the same thing; in a dedicated one it would not, and this is the
        // line that would show that before it became a wrong-sized picture.
        if let vp = drawable.views.first?.textureMap.viewport {
            NSLog(String(format: "[cp] eye size from %@: %dx%d "
                                 + "(view[0] viewport %.0fx%.0f, %d colour texture(s))",
                         source, w, h, vp.width, vp.height, drawable.colorTextures.count))
        }
        kl_ovrp_set_eye_texture_size(Int32(w), Int32(h))
    }

    /// The tracker the pose-at-time callback queries.
    ///
    /// Static because kl_ovrp takes a plain C function pointer, which cannot
    /// capture. Written once during setup and only read afterwards.
    nonisolated(unsafe) static var headAtTracker: WorldTrackingProvider?

    /// A head pose for an instant, for `kl_ovrp_set_head_at`.
    ///
    /// Two things here are defensive rather than obvious, and both come from
    /// this API's history:
    ///
    ///   * **The anchor is re-queried, never cached.** Device anchors have
    ///     carried hidden state, so an anchor object held across a frame is
    ///     not reliably the pose it was fetched for. Every call fetches.
    ///   * **The timestamp is walked back.** The tracker returns nil for an
    ///     instant it will not predict to, and how far ahead that is is not
    ///     documented and not fixed. Stepping back 5 ms at a time finds the
    ///     edge instead of guessing where it is; the interval is already
    ///     capped at 50 ms by the caller, so this walks at most that far.
    ///
    /// Returning 0 leaves the guest with the current pose, which is what it
    /// had before this existed — strictly better than inventing one.
    nonisolated(unsafe) static let headAt: @convention(c)
        (Double, UnsafeMutablePointer<Float>?) -> Int32 = { time, out in
        guard let out, let tracker = KleptonCompositor.headAtTracker else { return 0 }
        var t = time
        var anchor = tracker.queryDeviceAnchor(atTimestamp: t)
        var steps = 0
        while anchor == nil && steps < 10 {
            t -= 0.005
            anchor = tracker.queryDeviceAnchor(atTimestamp: t)
            steps += 1
        }
        guard let a = anchor else { return 0 }
        let (p, q) = KleptonCompositor.decompose(a.originFromAnchorTransform)
        // Head space, matching every other pose this file publishes.
        let m = KleptonCompositor.headFrom(a.originFromAnchorTransform,
                                           midpoint: KleptonCompositor.headAtMidpoint)
        let (hp, hq) = KleptonCompositor.decompose(m)
        _ = p; _ = q
        out[0] = hq.imag.x; out[1] = hq.imag.y; out[2] = hq.imag.z; out[3] = hq.real
        out[4] = hp.x;      out[5] = hp.y;      out[6] = hp.z
        return 1
    }

    /// The midpoint the callback uses — the instance's, copied out so a C
    /// function pointer can reach it.
    nonisolated(unsafe) static var headAtMidpoint = SIMD3<Float>(0, 0, 0)

    /// The eye midpoint in the device anchor's frame — the offset between what
    /// visionOS calls the device and what OpenXR calls VIEW.
    ///
    /// Zero until `pushEyeOffsets` has seen a drawable, and zero forever if
    /// `KL_HEAD_MIDPOINT=0`; both helpers below are the identity at zero, so
    /// the knob and the not-yet-measured case are one code path.
    private var eyeMidpointInDevice = SIMD3<Float>(0, 0, 0)

    /// `origin_from_head` from `origin_from_device`.
    private static func headFrom(_ originFromDevice: simd_float4x4,
                                 midpoint m: SIMD3<Float>) -> simd_float4x4 {
        if m == SIMD3<Float>(0, 0, 0) { return originFromDevice }
        var t = matrix_identity_float4x4
        t.columns.3 = SIMD4<Float>(m.x, m.y, m.z, 1)
        return originFromDevice * t
    }

    /// `head_from_view` from `device_from_view` — the drawable's own per-eye
    /// transform, restated about the midpoint so the whole composite chain is
    /// in one space.
    private static func headFromView(_ deviceFromView: simd_float4x4,
                                     midpoint m: SIMD3<Float>) -> simd_float4x4 {
        if m == SIMD3<Float>(0, 0, 0) { return deviceFromView }
        var t = matrix_identity_float4x4
        t.columns.3 = SIMD4<Float>(-m.x, -m.y, -m.z, 1)
        return t * deviceFromView
    }

    private func pushEyeOffsets(_ drawable: LayerRenderer.Drawable) {
        // Latched, not re-read per frame. A streaming guest sends its eye
        // transform to the host ONCE and the host will not revise it, so the
        // number the guest was given has to stay the number it was given; the
        // display side is free to move underneath it, and bridging the two is
        // the composite's job rather than something to hide by re-pushing.
        if ProcessInfo.processInfo.environment["KL_HEAD_MIDPOINT"] != "0",
           drawable.views.count >= 2 {
            let a = drawable.views[0].transform.columns.3
            let b = drawable.views[1].transform.columns.3
            eyeMidpointInDevice = SIMD3<Float>((a.x + b.x) * 0.5,
                                               (a.y + b.y) * 0.5,
                                               (a.z + b.z) * 0.5)
            Self.headAtMidpoint = eyeMidpointInDevice
            NSLog(String(format: "[cp] eye midpoint (%.4f, %.4f, %.4f) m from the "
                                 + "device anchor — head space is this, not the anchor "
                                 + "(KL_HEAD_MIDPOINT=0 restores the anchor)",
                         eyeMidpointInDevice.x, eyeMidpointInDevice.y,
                         eyeMidpointInDevice.z))
        }
        for (i, view) in drawable.views.enumerated() where i < 2 {
            // Relative to the eye MIDPOINT, because that is what the guest
            // calls its head. The drawable states these about the device
            // anchor.
            let t = view.transform.columns.3 - SIMD4<Float>(eyeMidpointInDevice.x,
                                                            eyeMidpointInDevice.y,
                                                            eyeMidpointInDevice.z, 0)
            kl_ovrp_set_eye_offset(Int32(i), t.x, t.y, t.z)
            // ...and the eye's ROTATION, which this loop used to discard.
            //
            // These displays are canted, so device_from_view is not a pure
            // translation — and the tangents pushed by primeDisplay come from
            // computeProjection, i.e. they are expressed in the *canted* frame.
            // Sending the shape without the direction told the guest to render
            // the right cone aimed straight ahead, and the composite then
            // viewed that picture through the real canted eye. The residual is
            // a per-eye rotation, equal and opposite between the two, which is
            // seen as a doubled image rather than as a blurred one.
            let q = simd_quatf(view.transform)
            kl_ovrp_set_eye_rotation(Int32(i), q.imag.x, q.imag.y, q.imag.z, q.real)
            if !loggedEyeOffsets && i == drawable.views.count - 1 {
                loggedEyeOffsets = true
                // Both eyes in the SAME space — the midpoint-relative one that
                // is actually pushed. Printing view[0]'s raw column beside
                // view[1]'s corrected one made the two eyes look wildly
                // asymmetric (2.6 cm apart vertically, 3.4 cm in depth) when
                // the pushed values are symmetric to a tenth of a millimetre,
                // and that read as a geometry fault for as long as it took to
                // subtract the midpoint by hand.
                let l = drawable.views[0].transform.columns.3
                    - SIMD4<Float>(eyeMidpointInDevice.x, eyeMidpointInDevice.y,
                                   eyeMidpointInDevice.z, 0)
                NSLog(String(format: "[cp] eye offsets from the midpoint: "
                             + "L (%.4f, %.4f, %.4f) R (%.4f, %.4f, %.4f) "
                             + "— IPD %.1f mm",
                             l.x, l.y, l.z, t.x, t.y, t.z,
                             abs(t.x - l.x) * 1000))
                // The half of view.transform the guest is NOT told about.
                //
                // We push the translation above, and the frustum tangents
                // separately — but nodes 0/1 report the HEAD's orientation, so
                // if these transforms are also rotated (canted displays), the
                // guest renders without that rotation while the composite
                // places the picture with it. Equal and opposite in the two
                // eyes, which is the error shape that reads as doubling rather
                // than as blur. Measured, not assumed: if these are ~0 the
                // whole concern is void, and if they are degrees then
                // KL_REPROJECT_NOCANT=1 is the A/B.
                for (i, v) in drawable.views.enumerated() where i < 2 {
                    let q = simd_quatf(v.transform)
                    let deg = q.angle * 180 / .pi
                    // The axis of a near-identity rotation is numerically
                    // meaningless (and may be NaN), so it is only worth
                    // printing once there is an angle to have an axis about.
                    if deg.isFinite && deg > 0.01 {
                        NSLog(String(format: "[cp] eye %d view rotation: %.3f deg "
                                     + "about (%.3f, %.3f, %.3f) — the guest is "
                                     + "NOT told this; KL_REPROJECT_NOCANT=1 drops it",
                                     i, deg, q.axis.x, q.axis.y, q.axis.z))
                    } else {
                        NSLog("[cp] eye \(i) view rotation: none (pure translation)")
                    }
                }
            }
        }
    }
    private var loggedEyeOffsets = false

    /// Ask for the resolution the configuration already paid for.
    ///
    /// `maxRenderQuality` is a **ceiling on memory**: it decides how large the
    /// drawable textures are allocated. The quality actually rendered at is a
    /// separate, runtime setting on the layer, and it starts at the device's
    /// default — so setting only the ceiling allocates the big textures and
    /// then draws into a fraction of them, which costs the memory and buys
    /// nothing. Both have to be set, and this is the second one.
    ///
    /// The system ramps between qualities over a short period rather than
    /// switching, which is why a frame can carry more than one drawable — see
    /// queryDrawables in renderFrame.
    private func applyRenderQuality() {
        let want = KleptonStageConfiguration.requestedQuality
        // On whether the ceiling was CONFIGURED, not on whether it was wanted.
        // The knob can be on and the ceiling still absent — the simulator
        // supports no foveation — and setting the running quality then aborts
        // the process inside Compositor Services. See `ceilingConfigured`.
        guard KleptonStageConfiguration.ceilingConfigured else {
            NSLog("[cp] render quality left at \(layerRenderer.renderQuality) "
                  + "(no maxRenderQuality ceiling was configured)")
            return
        }
        let before = layerRenderer.renderQuality
        layerRenderer.renderQuality = .init(want)
        NSLog("[cp] render quality \(before) -> \(layerRenderer.renderQuality) "
              + "(asked \(want), ceiling \(layerRenderer.configuration.maxRenderQuality))")
    }

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
            let all = frame.queryDrawables()
            guard let drawable = all.first else { continue }
            frame.startSubmission()
            stamps.append(LayerRenderer.Clock.Instant.epoch
                .duration(to: drawable.frameTiming.presentationTime).timeInterval)
            if tangents.isEmpty {
                tangents = (0..<drawable.views.count).map {
                    kl_reproject_tangents(drawable.computeProjection(viewIndex: $0))
                }
                pushEyeOffsets(drawable)
                // Before the guest boots, so it allocates the right swapchain
                // once instead of rebuilding when the size changes under it —
                // and each of those rebuilds used to leak a whole generation
                //.
                pushEyeTextureSize(drawable)
                // Also before the guest boots, and for a stricter reason: the
                // eye-texture provider builds the guest's own rate map from
                // this curve, and it must be in force before the first render
                // pass into an eye texture.
                captureDisplayFoveation(drawable)
            }
            // Every drawable the frame carries, not just the one measured from:
            // a frame that leaves one unpresented is a frame half-written.
            for d in all { clearAndPresent(d) }
            // Only after a drawable was actually presented: ending a
            // submission on the no-drawable path is an abort.
            frame.endSubmission()
        }

        for (i, t) in tangents.enumerated() where i < 2 {
            NSLog(String(format: "[cp] eye %d display tangents l=%.4f r=%.4f t=%.4f b=%.4f "
                                 + "(%.1f x %.1f degrees)", i, t.x, t.y, t.z, t.w,
                         (atan(t.x) + atan(t.y)) * 180 / .pi,
                         (atan(t.z) + atan(t.w)) * 180 / .pi))
            kl_ovrp_set_eye_frustum(Int32(i), t.x, t.y, t.z, t.w)
        }

        // The frame interval, as the median of the gaps between successive
        // presentation times. Median rather than mean because the first frames
        // of a freshly-started layer are not representative and one of them is
        // enough to move an average by several Hz.
        let gaps = zip(stamps.dropFirst(), stamps).map(-).filter { $0 > 0 }.sorted()
        if let mid = gaps.isEmpty ? nil : gaps[gaps.count / 2], mid > 0 {
            displayPeriod = mid          // what the cadence check measures against
            let hz = Float(1.0 / mid)
            NSLog(String(format: "[cp] display %.2f Hz measured over %d frames",
                         hz, gaps.count))
            kl_ovrp_set_display_frequency(hz)
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
    /// Worst pose age seen since the last line, in milliseconds.
    private var worstAgeMs: Double = 0

    private func noteReprojection(rendered: kl_ovrp_render_pose?,
                                  originFromHead: simd_float4x4, stage: Int) {
        reprojectFrames += 1
        var delta: Float = 0
        if var r = rendered {
            delta = withUnsafePointer(to: &r) {
                kl_reproject_delta_degrees($0, originFromHead)
            }
        }
        worstDelta = max(worstDelta, delta)
        // How OLD the pose being corrected against is. An angle on its own
        // cannot separate a fast head from a stale pose — it is the product of
        // the two — and those want opposite fixes. One displayed frame at
        // 120 Hz is 8.3 ms; tens of milliseconds means the record is not this
        // frame's and the correction is aimed at the wrong instant.
        var ageMs: Double = 0
        if let sub = rendered?.submit_ns, sub != 0 {
            var ts = timespec()
            clock_gettime(CLOCK_MONOTONIC, &ts)
            let now = UInt64(ts.tv_sec) * 1_000_000_000 + UInt64(ts.tv_nsec)
            if now > sub { ageMs = Double(now - sub) / 1_000_000 }
        }
        worstAgeMs = max(worstAgeMs, ageMs)
        guard reprojectFrames % 90 == 0 else { return }
        NSLog(String(format: "[cp] timewarp: stage %d serial %llu, delta %.2f deg "
                             + "(worst %.2f), pose age %.1f ms (worst %.1f), "
                             + "%d stale in a row",
                     stage, rendered?.serial ?? 0, delta, worstDelta,
                     ageMs, worstAgeMs, staleInARow))
        worstDelta = 0
        worstAgeMs = 0
    }
}
