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
import MetalFX
import simd
import ARKit
import AVFoundation
import CoreVideo
import QuartzCore
import UIKit
import CoreText

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
        // getenv, not ProcessInfo's dictionary: a game's own settings name it
        // at the start of a boot (KleptonAlyx.swift), after the process began.
        let e = getenv("KL_CP_QUALITY").map { String(cString: $0) }
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

/// What the composite needs of a drawable, and nothing more.
///
/// A Compositor Services drawable when the picture goes to the system's own
/// layer — the ordinary path, forwarded member for member — or a stand-in with
/// our own textures when it goes to RealityKit instead (KL_RK_PRESENT,
/// KleptonRealityPresent.swift). The composite below reads only these members,
/// so it draws the same picture either way; only where it lands differs.
final class KLDrawable {
    struct TextureMap { let textureIndex: Int; let sliceIndex: Int; let viewport: MTLViewport }
    struct View { let transform: simd_float4x4; let textureMap: TextureMap }
    struct Timing { let presentationTime: LayerRenderer.Clock.Instant; let renderingDeadline: LayerRenderer.Clock.Instant }
    let views: [View]
    let colorTextures: [MTLTexture]
    let depthTextures: [MTLTexture]
    let rasterizationRateMaps: [MTLRasterizationRateMap]
    let depthRange: SIMD2<Float>
    let frameTiming: Timing
    private let projections: [simd_float4x4]
    /// The real drawable, when there is one.
    let system: LayerRenderer.Drawable?

    init(_ d: LayerRenderer.Drawable) {
        system = d
        views = d.views.map {
            View(transform: $0.transform,
                 textureMap: TextureMap(textureIndex: $0.textureMap.textureIndex,
                                        sliceIndex: $0.textureMap.sliceIndex,
                                        viewport: $0.textureMap.viewport))
        }
        colorTextures = d.colorTextures
        depthTextures = d.depthTextures
        rasterizationRateMaps = d.rasterizationRateMaps
        depthRange = d.depthRange
        frameTiming = Timing(presentationTime: d.frameTiming.presentationTime,
                             renderingDeadline: d.frameTiming.renderingDeadline)
        projections = d.views.indices.map { d.computeProjection(viewIndex: $0) }
    }

    /// The stand-in: our own textures, uniform (no rate map).
    init(views: [View], projections: [simd_float4x4], color: [MTLTexture], depth: [MTLTexture],
         presentation: TimeInterval, deadline: TimeInterval) {
        system = nil
        self.views = views
        self.projections = projections
        colorTextures = color
        depthTextures = depth
        rasterizationRateMaps = []
        depthRange = SIMD2<Float>(Float.infinity, 0.1)
        let at = { (t: TimeInterval) in LayerRenderer.Clock.Instant.epoch.advanced(by: .seconds(t)) }
        frameTiming = Timing(presentationTime: at(presentation), renderingDeadline: at(deadline))
    }

    /// The pose the picture is drawn for: handed to the system drawable, or
    /// kept for whoever places the stand-in's picture (the RealityKit quad).
    private(set) var anchorSet: DeviceAnchor?
    var deviceAnchor: DeviceAnchor? {
        get { system?.deviceAnchor ?? anchorSet }
        set { system?.deviceAnchor = newValue; anchorSet = newValue }
    }
    func computeProjection(viewIndex: Int) -> simd_float4x4 { projections[viewIndex] }
    func encodePresent(commandBuffer: MTLCommandBuffer) { system?.encodePresent(commandBuffer: commandBuffer) }
}

final class KleptonCompositor {
    private var layerRenderer: LayerRenderer?
    /// Set in the RealityKit mode (KL_RK_PRESENT), where there is no layer.
    private var rkTarget: KLRealityTarget?

    // The picture moving to the RealityKit space at start (KL_RK_PRESENT,
    // KLRealityPresent.switchAtStart): the running compositor takes the new
    // output, and the guest never notices.
    nonisolated(unsafe) static var live: KleptonCompositor?
    nonisolated(unsafe) static var expectingHandover = false
    private let handover = DispatchSemaphore(value: 0)
    private let handoverLock = NSLock()
    private var pendingLayer: LayerRenderer?
    private var pendingRK: KLRealityTarget?
    /// The colour format the pipelines were built for.
    private var pipelineFormat: MTLPixelFormat = .invalid

    /// A new Compositor Services layer: handed to the running compositor if a
    /// move is under way, else a compositor of its own (the usual start).
    static func attach(layer: LayerRenderer) {
        if let c = live, expectingHandover {
            c.handoverLock.lock(); c.pendingLayer = layer; c.handoverLock.unlock()
            c.handover.signal()
            return
        }
        let c = KleptonCompositor(layer)
        live = c
        c.startRenderLoop()
    }
    /// A RealityKit target for the running compositor (the picture moving at
    /// start, KLRealityPresent.switchAtStart). There is no other way in.
    static func attach(rk target: KLRealityTarget) {
        guard let c = live, expectingHandover else {
            NSLog("[rk] a RealityKit space opened with no compositor waiting for it — ignored")
            return
        }
        c.handoverLock.lock(); c.pendingRK = target; c.handoverLock.unlock()
        c.handover.signal()
    }
    /// A move that failed: let the waiting loop end as usual.
    static func handoverFailed() {
        expectingHandover = false
        live?.handover.signal()
    }
    private var device: MTLDevice!
    // Optional, not implicitly unwrapped, and that is the whole fix for one
    // crash: when ANGLE has no MTLDevice this used to stay nil while the loop
    // carried on, and primeDisplay trapped on it before the guest thread was
    // even started — so the immersive path could not run at all without
    // KL_GLFB, which is exactly the configuration you want when the question
    // is about the lifecycle rather than the picture.
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState!
    private var guestDepthPipeline: MTLRenderPipelineState?
    private var loggedGuestDepth = false
    private var depthState: MTLDepthStencilState?
    /// The guest's quad layers in the order it submitted them: no test, and
    /// depth still written. See `overlaysInSubmitOrder`.
    private var overlayOrderState: MTLDepthStencilState?
    /// The metrics panel's: over everything. See KleptonOverlayDepth.statsCard.
    private var statsCardState: MTLDepthStencilState?
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
    /// The projection-layer pass for a layer that asks to be blended by its
    /// alpha (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT): premultiplied
    /// colour, as OpenXR has it unless the layer says otherwise, and the
    /// unpremultiplied variant. See encodeProjLayers.
    private var projBlendPipeline: MTLRenderPipelineState?
    private var projBlendUnpremulPipeline: MTLRenderPipelineState?
    /// The equirect (360 panorama) overlay pipeline and its depth state.
    /// Drawn as a skybox behind the eye content: depth-tests .greaterEqual at
    /// the far plane and writes no depth, so real eye pixels stay in front and
    /// the panorama fills the rest (which, during shader warmup, is everything).
    private var equirectPipeline: MTLRenderPipelineState?
    private var equirectDepthState: MTLDepthStencilState?
    private var loadingSkyboxDepthState: MTLDepthStencilState?
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
    // ...and each view's own horizontal curve, left eye first, for a guest that
    // can be given a map layer per eye (see updateGuestRateMap). Symmetrised:
    // the right one is the left one mirrored, so both layers store the same
    // number of texels.
    private var displayEyeQualityX: [[Float]] = []

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

    // --- The boot rate SETTLE gate -------------------------------------------
    //
    // The panel does not start at the rate it runs at. A Vision Pro brings the
    // layer up at 120 Hz and drops to 90 once the immersive session is properly
    // engaged, some hundreds of milliseconds later — and priming takes a fixed
    // eight frames, i.e. ~66 ms, which lands squarely inside that window. So
    // the rate the guest is told is decided by a race.
    //
    // That race matters more than the two numbers suggest, because the guest
    // reads its refresh rate exactly ONCE, early, and paces everything off it
    // for the rest of the session. Measured on device: a boot that lost the
    // race told the guest 120 while the panel delivered 90 slots a second all
    // session. 120:90 is 4:3, so every fourth guest frame shares a panel slot
    // with its neighbour and displayed motion steps alternate one slot / two
    // slots — a whole-image zigzag proportional to head speed, identical in
    // feel to a pose bug and not one. Five consecutive sessions won the race
    // and validated the design on luck; the sixth lost it.
    //
    // So: measure until the cadence has been the SAME cadence for a while, and
    // only then latch. This holds every downstream consumer with no extra
    // plumbing, because primeDisplay runs before kl_app_guest_start — the guest
    // thread does not exist yet, so its one early Hz read cannot precede the
    // latch.
    //
    // The numbers, and why:
    //   KL_HZ_SETTLE_MS (500) — the run of agreeing intervals must span half a
    //     second. That is ~45 slots at 90 Hz and ~60 at 120, far longer than
    //     the 120->90 transition itself (under a second, end to end, in the
    //     device log), and long enough that a rate still in motion cannot fake
    //     it. Half a second of black at boot is free: the guest takes tens of
    //     seconds to come up behind it.
    //   KL_HZ_SETTLE_MIN_GAPS (120) — DURATION IS NOT RESOLUTION. Sixty
    //     intervals cannot resolve a tenth of a percent, and a tenth of a
    //     percent is 8 microseconds a frame. 120 intervals is ~1.0 s at 120 Hz
    //     and ~1.3 s at 90.
    //   KL_HZ_SETTLE_TIMEOUT_MS (3000) — a panel that never gives a steady half
    //     second is real (thermal ramping, a loaded device), and blocking the
    //     boot on it forever is worse than guessing. At three seconds we latch
    //     the median of everything seen and SAY SO, so a confounded session is
    //     identifiable from its own log.
    //   tolerance 6% — 120 and 90 are 33% apart, so this discriminates them
    //     with an order of magnitude to spare while absorbing sub-millisecond
    //     scheduling noise.
    // A gap that is a near-exact 2x or 3x of the run's period is a HELD frame,
    // not a rate change; it extends the run rather than breaking it.
    private let hzSettle          = klEnvOn("KL_HZ_SETTLE", default: true)
    private let hzSettleMs        = Double(klEnvFloat("KL_HZ_SETTLE_MS", 500))
    private let hzSettleTimeoutMs = Double(klEnvFloat("KL_HZ_SETTLE_TIMEOUT_MS", 3000))
    private let hzSettleTol       = Double(klEnvFloat("KL_HZ_SETTLE_TOL", 0.06))
    private let hzSettleMinGaps   = Int(klEnvFloat("KL_HZ_SETTLE_MIN_GAPS", 120))
    private let hzSnapTol         = Double(klEnvFloat("KL_HZ_SNAP_TOL", 0.005))

    // THE ESTIMATOR IS A MEDIAN, and that is the load-bearing choice rather
    // than a detail. The obvious way to turn a settled run into a period is the
    // mean — total span over total slots — and it is measurably wrong here: the
    // run necessarily begins at the layer's very first frames, which is where
    // primeDisplay is also computing the foveation curve and pushing the eye
    // textures, so the slowest frames of the whole session are inside the
    // measured window. Worse, if the running mean is also the tolerance
    // reference, those outliers drag the reference far enough to keep accepting
    // themselves. Measured on device, that mean came out 0.16% low against a
    // dead-steady panel — 13.9 microseconds a frame, which is enough to walk
    // the guest a whole panel slot every five seconds. A median of the same
    // samples is immune to it, and averaging for longer is not: the error was
    // bias, not noise. (The eight-frame code this replaced took a median for
    // precisely this reason, and said so.)
    //
    // ...and the last step, snapping: a measurement within half a percent of a
    // rate the panel actually runs IS that rate. The guest is told this number
    // once and paces against it forever, so our estimate's last two digits
    // would otherwise become a permanent frequency offset against the panel.
    // The candidates are what visionOS documents for this panel plus the 60 a
    // thermal demotion produces; the closest pair is 96 and 100, 4.2% apart, so
    // half a percent has eight times the margin it needs.
    private static let canonicalHz: [Double] = [120, 100, 96, 90, 60]

    private var cadenceN = 0
    private var cadenceSum = 0.0
    private var cadenceMax = 0.0
    private var cadenceRepeats = 0      // gaps of 1.5 periods or more
    private var missedDeadline = 0
    // Consecutive liveness reports whose mean gap disagrees with the latched
    // display period by more than 10%. See cadenceSummary().
    private var cadenceDisagreed = 0
    private let noFence = klEnvOn("KL_CP_NOFENCE", default: false)
    // The last device anchor the tracker actually answered with, and how often
    // it has had to stand in. See displayOriginFromDevice for why holding it is
    // not the same kind of guess as the identity it replaced.
    private var lastGoodOriginFromDevice: simd_float4x4?
    private var anchorWalkedBack = 0
    private var anchorHeld = 0
    private var anchorUnknown = 0
    private let holdAnchor = klEnvOn("KL_CP_ANCHOR_HOLD", default: true)
    // KL_CP_ANCHOR_TRACKED: a device anchor the tracker itself marks as not
    // tracked counts as no answer. As the sender of fix 37 describes it,
    // `queryDeviceAnchor` goes on returning an anchor while the headset has
    // lost its place and is finding it again, with `isTracked` false and a
    // transform that is not where the head is; taken as a pose, the guest and
    // the composite both follow it. Refused, the three readers below do what
    // they already do for a nil: the guest keeps its last pose, the composite
    // holds the last measured one (KL_CP_ANCHOR_HOLD) and the drawable is
    // given no anchor. Default on for the one title it was run with — Real VR
    // Fishing (x2582932495064035), on its sender's headset (2026-10-14; not
    // run here) — because what every other game shows through such a moment
    // has not been looked at.
    // Static: `headAt` is a C function pointer and can capture nothing.
    private static let trackedAnchorsOnly =
        klEnvOn("KL_CP_ANCHOR_TRACKED", default: klTargetName() == "x2582932495064035")
    private static func usable(_ anchor: DeviceAnchor?) -> DeviceAnchor? {
        guard let anchor, !trackedAnchorsOnly || anchor.isTracked else { return nil }
        return anchor
    }
    // Host recenter — the Quest Oculus-button hold, done where the Quest does
    // it: in the OS layer. `recenterXform` rebases the world origin so the
    // head's current yaw+XZ become identity; folded into originFromDevice(),
    // the ONE function every consumer reads, so published guest poses and the
    // compositor's own placement shift together and the streamed view (Steam
    // Link -> SteamVR) snaps to the new forward coherently. Set by holding the
    // right Sense Options button >= 1 s (KleptonControllers).
    nonisolated(unsafe) static var pendingRecenter = false
    private var recenterXform = matrix_identity_float4x4
    private var cmdCommitted = 0
    private let cmdLock = NSLock()
    private var cmdCompleted = 0
    // Where a composite buffer's time on the GPU goes, under cmdLock. "0 past
    // deadline" only says the ENCODE was on time; a buffer that then sits
    // behind the guest's rendering on a GPU that does not preempt reaches the
    // display late all the same, and the system answers by showing every frame
    // twice (cadence 2.00). `queued` is commit -> the GPU starting it, `ran` is
    // the pass itself: a long queue with a short run is the guest's load, a
    // long run is this pass's own cost. Reported with the cadence.
    private var gpuTimedN = 0
    private var gpuQueuedSum = 0.0, gpuQueuedMax = 0.0
    private var gpuRanSum = 0.0, gpuRanMax = 0.0
    private var cmdError: Error?
    // Startup diagnostics for the signal-9 wall: what the last renderFrame saw,
    // surfaced into the alive report, plus a short per-frame trace of the first
    // few seconds so the log can say whether the loop keeps iterating and
    // whether its command buffers complete — the two things a 2 s report cannot
    // catch on an app that dies in ~1 s.
    private var lastGlFenceSeen: UInt64 = 0
    private var lastProjLayersSeen = 0
    private var lastEncodedSeen = 0
    private var startupTrace = 0
    // Startup-deadlock guard: the guest's fence value goes non-zero after its
    // first swap, then every frame waits on it. If the guest then goes
    // heads-down (loading a multi-GB OBB, compiling shaders) the signalling
    // command buffer stalls behind that GPU work and the composite buffers pile
    // up uncompleted — the drawable pool starves, this loop stops presenting,
    // and visionOS SIGKILLs the app with no report. Bounding the outstanding
    // fenced buffers keeps a drawable free so the loop always presents (black
    // if need be) and survives until the guest catches up.
    private let maxFenceBacklog = Int(klEnvFloat("KL_CP_FENCE_BACKLOG", 2))
    private var fenceBacklogSkips = 0
    private var warnedFenceBacklog = false
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
    /// The real room for a guest that asks for one (kl_ovrp's emulated room,
    /// KL_OVRP_SCENE_ROOM — Synth Riders' mixed-reality mode): visionOS's plane
    /// detection, floor, ceiling and walls handed to kl_ovrp_scene_planes. In a
    /// session of its own, so that a refused world-sensing permission or a
    /// provider that will not start costs the room and never the head. Planes
    /// arrive in ARKit's world space, the space the head pose is published in.
    private static var roomSession: ARKitSession?
    private static func startRoomPlanes() {
        guard roomSession == nil, kl_ovrp_scene_wanted() != 0 else { return }
        guard PlaneDetectionProvider.isSupported else {
            NSLog("[cp] room: plane detection is not supported here — the guest gets a box")
            return
        }
        let session = ARKitSession()
        roomSession = session
        Task.detached {
            let auth = await session.requestAuthorization(for: [.worldSensing])
            guard auth[.worldSensing] == .allowed else {
                NSLog("[cp] room: world sensing not allowed (\(String(describing: auth[.worldSensing]))) — the guest gets a box")
                return
            }
            let planes = PlaneDetectionProvider(alignments: [.horizontal, .vertical])
            do {
                try await session.run([planes])
            } catch {
                NSLog("[cp] room: plane detection failed to start: \(error) — the guest gets a box")
                return
            }
            NSLog("[cp] room: plane detection running")
            var seen: [UUID: PlaneAnchor] = [:]
            for await update in planes.anchorUpdates {
                switch update.event {
                case .added, .updated: seen[update.anchor.id] = update.anchor
                case .removed: seen[update.anchor.id] = nil
                }
                publishRoomPlanes(Array(seen.values))
            }
        }
    }

    /// Where the plane lies in ARKit's extent frame (originFromAnchor ·
    /// anchorFromExtent) is not written down in the SDK, and the two readings
    /// differ by a quarter turn: a floor taken the wrong way stands upright and
    /// its outline has no area (Synth Riders: "ich bekomme einen 0x0x0 Raum",
    /// 2026-10-15). So it is read off the planes themselves: whichever local axis
    /// of the horizontal ones (floor, ceiling) points up is the normal — Z: the
    /// plane is in XY, already the scene convention kl_ovrp answers in; Y: it is
    /// in XZ and is turned (X stays, Y is -Z, Z is Y). Then each normal is made
    /// to point into the room: up for a floor, down for a ceiling, towards the
    /// floor's middle for a wall (a half turn about the plane's own Y otherwise).
    private static let sceneFromXZPlane = simd_float4x4(columns: (
        SIMD4<Float>(1, 0, 0, 0), SIMD4<Float>(0, 0, -1, 0),
        SIMD4<Float>(0, 1, 0, 0), SIMD4<Float>(0, 0, 0, 1)))
    private static let halfTurnY = simd_float4x4(diagonal: SIMD4<Float>(-1, 1, -1, 1))
    private static var roomPlanesSaid = 0
    private static func publishRoomPlanes(_ anchors: [PlaneAnchor]) {
        var picked: [(kind: Int32, m: simd_float4x4, w: Float, h: Float)] = []
        for a in anchors {
            let kind: Int32
            switch a.classification {
            case .floor: kind = 0
            case .ceiling: kind = 1
            case .wall: kind = 2
            default: continue
            }
            let e = a.geometry.extent
            picked.append((kind, a.originFromAnchorTransform * e.anchorFromExtentTransform, e.width, e.height))
        }
        // The convention, by vote of the horizontal planes; XY where none is seen yet.
        var zUp = 0, yUp = 0
        for p in picked where p.kind != 2 {
            let y = abs(p.m.columns.1.y), z = abs(p.m.columns.2.y)
            if z > y { zUp += 1 } else { yUp += 1 }
        }
        let planeInXZ = yUp > zUp
        var frames = picked.map { p -> (kind: Int32, m: simd_float4x4, w: Float, h: Float) in
            (p.kind, planeInXZ ? p.m * sceneFromXZPlane : p.m, p.w, p.h)
        }
        let floorAt = frames.first(where: { $0.kind == 0 }).map { SIMD3($0.m.columns.3.x, $0.m.columns.3.y, $0.m.columns.3.z) }
        let wallsMid: SIMD3<Float>? = {
            let ws = frames.filter { $0.kind == 2 }
            guard !ws.isEmpty else { return nil }
            return ws.reduce(SIMD3<Float>(0, 0, 0)) { $0 + SIMD3($1.m.columns.3.x, $1.m.columns.3.y, $1.m.columns.3.z) } / Float(ws.count)
        }()
        let middle = floorAt ?? wallsMid ?? SIMD3<Float>(0, 0, 0)
        for i in frames.indices {
            let n = SIMD3(frames[i].m.columns.2.x, frames[i].m.columns.2.y, frames[i].m.columns.2.z)
            let at = SIMD3(frames[i].m.columns.3.x, frames[i].m.columns.3.y, frames[i].m.columns.3.z)
            let flip: Bool
            switch frames[i].kind {
            case 0: flip = n.y < 0
            case 1: flip = n.y > 0
            default:
                var toMid = middle - at; toMid.y = 0
                flip = simd_dot(SIMD3(n.x, 0, n.z), toMid) < 0
            }
            if flip { frames[i].m = frames[i].m * halfTurnY }
        }
        if roomPlanesSaid < 6, !frames.isEmpty {
            roomPlanesSaid += 1
            let desc = frames.map { f -> String in
                let n = f.m.columns.2, p = f.m.columns.3
                return String(format: "%@ %.1fx%.1f at (%.2f %.2f %.2f) n (%.2f %.2f %.2f)",
                              ["floor", "ceiling", "wall"][Int(f.kind)], f.w, f.h, p.x, p.y, p.z, n.x, n.y, n.z)
            }.joined(separator: "; ")
            NSLog("[cp] room: planes in their %@ plane (%d:%d) — %@", planeInXZ ? "XZ" : "XY", yUp, zUp, desc)
        }
        var kinds: [Int32] = [], ms: [Float] = [], ext: [Float] = []
        for f in frames {
            for c in [f.m.columns.0, f.m.columns.1, f.m.columns.2, f.m.columns.3] {
                ms.append(contentsOf: [c.x, c.y, c.z, c.w])
            }
            kinds.append(f.kind)
            ext.append(contentsOf: [f.w, f.h])
        }
        kinds.withUnsafeBufferPointer { k in
            ms.withUnsafeBufferPointer { m in
                ext.withUnsafeBufferPointer { x in
                    kl_ovrp_scene_planes(Int32(k.count), k.baseAddress, m.baseAddress, x.baseAddress)
                }
            }
        }
    }

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
            Self.startRoomPlanes()
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

    /// The device anchor for the instant the composite is about to draw at.
    ///
    /// This and `originFromDevice` below are one answer in two halves, and what
    /// they exist for is that **`origin_from_device` is never the identity while
    /// the session has ever tracked.** `queryDeviceAnchor` answers nil for an instant it will not
    /// predict to, and how far ahead that is is neither documented nor fixed —
    /// a guest hitch that pushes the presentation time out, or a tracking
    /// interruption, is enough to reach it.
    ///
    /// Substituting the identity there does not weaken the correction, it
    /// asserts something false: that the head is at the world origin, unrotated.
    /// The composite places the guest's picture in the space its render pose
    /// defines and looks at it from this one, so the quad stops counteracting
    /// head motion and sits fixed in front of the face — and stays there for as
    /// long as the tracker keeps refusing, which is what makes it read as stuck
    /// rather than as a glitch.
    ///
    /// Holding the last pose actually measured is wrong by however far the head
    /// has moved since, which is bounded by the length of the outage. It is also
    /// the answer `updatePoses` already gives the guest, by skipping the push
    /// and leaving the previous pose in place: the two seams now fail the same
    /// way instead of opposite ways, which is what made the disagreement
    /// invisible.
    ///
    /// The walk-back is `headAt`'s, for `headAt`'s reason — stepping back finds
    /// the edge of what the tracker will predict to instead of guessing where it
    /// is. `KL_CP_ANCHOR_HOLD=0` restores the identity fallback as the A/B.
    private func displayAnchor(at presentation: TimeInterval) -> DeviceAnchor? {
        var t = presentation
        var steps = 0
        while steps < 10 {
            if let a = Self.usable(worldTracking.queryDeviceAnchor(atTimestamp: t)) {
                if steps > 0 { anchorWalkedBack += 1 }
                return a
            }
            t -= 0.005
            steps += 1
        }
        return nil
    }

    /// ...and the transform the composite's own math uses, which is where the
    /// holding happens. Kept separate from the anchor above because the anchor
    /// OBJECT goes to `cp_drawable.deviceAnchor`, and that is the system's
    /// input to its own reprojection: handing it one we invented would put our
    /// correction and the system's on different heads. Nil there is a refusal
    /// the system understands; the identity here is a lie it cannot see.
    private func originFromDevice(_ anchor: DeviceAnchor?) -> simd_float4x4 {
        if let anchor {
            lastGoodOriginFromDevice = anchor.originFromAnchorTransform
            return recenterXform * anchor.originFromAnchorTransform
        }
        guard holdAnchor, let held = lastGoodOriginFromDevice else {
            // Nothing has ever tracked — the frames before ARKit starts — or the
            // knob is off. The identity is the only answer left, and it is
            // counted so that it can never again be taken silently.
            anchorUnknown += 1
            return matrix_identity_float4x4
        }
        anchorHeld += 1
        return recenterXform * held
    }

    // The tracker's own state of the head anchor, once a second: how many
    // frames were tracked, orientation-only, untracked or missing, and where
    // ARKit put the head. A position that stands still while the player
    // walks is either the game or this — orientation-only keeps answering
    // with a transform whose translation no longer moves (Angry Birds VR,
    // 2026-10-18: the island followed the player).
    private var anchorStates = (tracked: 0, orientation: 0, untracked: 0, missing: 0)
    private func traceAnchorState(_ a: DeviceAnchor?) {
        if let a {
            switch a.trackingState {
            case .tracked: anchorStates.tracked += 1
            case .orientationTracked: anchorStates.orientation += 1
            default: anchorStates.untracked += 1
            }
        } else { anchorStates.missing += 1 }
        let n = anchorStates.tracked + anchorStates.orientation + anchorStates.untracked + anchorStates.missing
        guard n >= 100 else { return }
        let t = a?.originFromAnchorTransform.columns.3 ?? .zero
        NSLog("[cp] head anchor: tracked %d, orientation only %d, untracked %d, none %d; ARKit head at (%.2f, %.2f, %.2f)",
              anchorStates.tracked, anchorStates.orientation, anchorStates.untracked, anchorStates.missing,
              t.x, t.y, t.z)
        anchorStates = (0, 0, 0, 0)
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
        let rawHeadAnchor = worldTracking.queryDeviceAnchor(atTimestamp: presentationTime)
        traceAnchorState(rawHeadAnchor)
        let controllerHeadAnchor = Self.usable(rawHeadAnchor)
        if let anchor = controllerHeadAnchor {
            // This is the pose the guest will render with, and kl_ovrp latches
            // it into the stage record when the guest reaches ovrp_BeginFrame —
            // which is what the composite pass reads back to know where the
            // picture it is about to show was drawn from.
            // In head space, not device space — see the midpoint argument at
            // originFromHead. The guest derives its eye positions from this
            // pose and the offsets pushEyeOffsets sent, and those two have to
            // be stated about one origin or every near object sits wrong.
            let selectionHead = Self.headFrom(anchor.originFromAnchorTransform, midpoint: eyeMidpointInDevice)
            controllers.updateSelectionHead(selectionHead)
            let (p, q) = Self.decompose(selectionHead)
            // Date the pose with the instant it is about. It was predicted to
            // `presentationTime`, so that — not "now" — is the clock the head
            // velocity has to be differenced on. The two differ by this
            // thread's scheduling jitter, and dividing a uniform
            // presentation-time step by a jittery call-time one multiplies the
            // derived velocity by a per-frame noise factor. See
            // klovrp_derive_motion in runtime/xr/kl_ovrp.c.
            kl_ovrp_set_head_pose_time(presentationTime)
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
        controllers.update(leftHand: left, rightHand: right, at: presentationTime,
                           headTransform: controllerHeadAnchor?.originFromAnchorTransform)
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
        let color = layerRenderer?.configuration.colorFormat ?? KLRealityTarget.colorFormat
        pipelineFormat = color
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
            // Without this pipeline encodeComposite never looks for guest depth.
            if klTargetName() == "x3635172946605196" {
                desc.fragmentFunction = lib.makeFunction(name: "kl_reproject_depth_f")
                guestDepthPipeline = try? device.makeRenderPipelineState(descriptor: desc)
                desc.fragmentFunction = lib.makeFunction(name: "kl_reproject_f")
            }
            // The same pass, blending — for a projection layer stacked on others
            // that asks for it by its alpha. kl_reproject_f hands out the
            // guest's alpha (and drops what is fully transparent) when the
            // layer's uniforms carry eye_alpha 1.
            for premul in [true, false] {
                desc.colorAttachments[0].isBlendingEnabled = true
                desc.colorAttachments[0].rgbBlendOperation = .add
                desc.colorAttachments[0].alphaBlendOperation = .add
                desc.colorAttachments[0].sourceRGBBlendFactor = premul ? .one : .sourceAlpha
                desc.colorAttachments[0].sourceAlphaBlendFactor = .one
                desc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
                desc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
                let ps = try? device.makeRenderPipelineState(descriptor: desc)
                if premul { projBlendPipeline = ps } else { projBlendUnpremulPipeline = ps }
            }
            desc.colorAttachments[0].isBlendingEnabled = false

            // Depth WRITES, which this pass never did. visionOS reprojects the
            // submitted frame using its depth buffer, so a pass that writes no
            // depth submits perfect colour with the geometry "nothing is here at
            // any finite distance" — and the compositor honours that literally.
            // Measured on device: a fullscreen quad at reverse-Z 0 with writes
            // off is invisible; the same quad at 0.05 (2 m) with writes on
            // appears. .greater matches the reverse-Z clear of 0.
            depthState = device.makeDepthStencilState(descriptor: KleptonOverlayDepth.nearerWins())

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

            overlayOrderState = device.makeDepthStencilState(descriptor: KleptonOverlayDepth.inOrder())
            statsCardState = device.makeDepthStencilState(descriptor: KleptonOverlayDepth.statsCard())

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

            // The equirect panorama pipeline — a 360 skybox behind everything.
            // Opaque (a background is not blended), same formats/amplification as
            // the quad pass. See kl_msl_equirect and encodeOverlays.
            let qdesc = MTLRenderPipelineDescriptor()
            if let qlib = try? device.makeLibrary(source: String(cString: kl_reproject_equirect_msl()),
                                                  options: nil) {
                qdesc.vertexFunction   = qlib.makeFunction(name: "kl_eq_v")
                qdesc.fragmentFunction = qlib.makeFunction(name: "kl_eq_f")
                qdesc.colorAttachments[0].pixelFormat = color
                qdesc.depthAttachmentPixelFormat = .depth32Float
                qdesc.maxVertexAmplificationCount = amplification
                equirectPipeline = try? device.makeRenderPipelineState(descriptor: qdesc)
            }
            if equirectPipeline == nil {
                NSLog("[cp] the equirect pipeline failed to build — 360 background "
                      + "overlays (AC Nexus loading skybox) will not be composited")
            }
            // Far-plane, read-only depth so the panorama sits behind eye content.
            let qds = MTLDepthStencilDescriptor()
            qds.depthCompareFunction = .greaterEqual
            qds.isDepthWriteEnabled = false
            equirectDepthState = device.makeDepthStencilState(descriptor: qds)
            // The loading skybox WRITES a finite far depth. visionOS reprojects
            // using the submitted depth and discards a frame whose depth is all
            // 0 (reverse-Z infinity) as "nothing there" - which is why a
            // colour-correct skybox (and even its clear) showed black. Writing a
            // distant-but-finite depth makes it a real backdrop the compositor
            // keeps.
            let lsds = MTLDepthStencilDescriptor()
            lsds.depthCompareFunction = .greaterEqual
            lsds.isDepthWriteEnabled = true
            loadingSkyboxDepthState = device.makeDepthStencilState(descriptor: lsds)

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

    /// KL_CP_GAZE_TRACE: where each view's rate map is densest, four times a
    /// second for the first two minutes — the measurement of whether the maps a
    /// drawable carries follow the eyes. The centre of the full-rate plateau is
    /// printed per view as a fraction of the screen (x from the left, y from the
    /// map's row 0); a figure that moves as the person looks around is a map
    /// that is rebuilt from the gaze, one that stays put is a fixed curve.
    private var gazeTraceFrames = 0
    private var gazeTraceLines = 0
    private lazy var gazeTraceOn = klEnvOn("KL_CP_GAZE_TRACE", default: false)
    private func traceGazeMaps(_ drawables: [LayerRenderer.Drawable]) {
        guard gazeTraceOn, gazeTraceLines < 480 else { return }
        gazeTraceFrames += 1
        guard gazeTraceFrames % 25 == 0 else { return }
        func centre(_ q: [Float]) -> Float {
            var sum: Float = 0, weight: Float = 0
            for (i, v) in q.enumerated() where v >= 0.9 {
                sum += (Float(i) + 0.5) * v; weight += v
            }
            return weight > 0 ? sum / weight / Float(q.count) : -1
        }
        var line = "[cp] gaze-map f\(gazeTraceFrames):"
        for (d, drawable) in drawables.enumerated() {
            for (m, map) in drawable.rasterizationRateMaps.enumerated() {
                for layer in 0..<map.layerCount {
                    let (qx, qy) = Self.sampleCurve(map, layer: layer, zonesX: 48, zonesY: 48)
                    let p = map.physicalSize(layer: layer)
                    line += String(format: " d%d.m%d.l%d x=%.3f y=%.3f phys %dx%d |", d, m, layer,
                                   centre(qx), centre(qy), p.width, p.height)
                }
            }
        }
        gazeTraceLines += 1
        NSLog("%@", line)
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
        var perView: [[Float]] = []
        for map in maps {
            for layer in 0..<map.layerCount {
                let (mx, my) = Self.sampleCurve(map, layer: layer, zonesX: nx, zonesY: ny)
                for i in 0..<nx { qx[i] = max(qx[i], mx[i]) }
                for i in 0..<ny { qy[i] = max(qy[i], my[i]) }
                perView.append(mx)
                sampled += 1
            }
        }
        guard sampled > 0 else { return }
        // Two views: keep each eye's own horizontal curve as well. The pair is
        // made an exact mirror image — each zone the denser of "the left eye
        // here" and "the right eye at the mirrored place" — so that neither eye
        // gets fewer samples than its display asks for and both map layers come
        // out the same physical size, which the copy between the guest's
        // swapchain and the compositor's eye textures relies on.
        if perView.count == 2 {
            var left = perView[0]
            for i in 0..<nx { left[i] = max(left[i], perView[1][nx - 1 - i]) }
            displayEyeQualityX = [left, Array(left.reversed())]
        }
        // ...and the envelope's valleys are filled. The two eyes' dense regions
        // sit inboard of centre, mirrored, so their per-zone maximum has TWO
        // peaks with a dip between them — 1.00 either side and 0.83 in the two
        // middle zones, measured on device — and that dip is in the exact centre
        // of the guest's picture, where both eyes look. It read as a vertical
        // crease down the middle of the view, like the fold of a folding screen
        // (owner, Doom 3, 2026-10-08). Nothing wants fewer samples there: it is
        // an artefact of combining two curves, not either display's request. So
        // each zone takes the lower of "the most seen so far from the left" and
        // "from the right", which leaves a single plateau and only ever adds
        // samples.
        func filled(_ q: [Float]) -> [Float] {
            guard q.count > 2 else { return q }
            var fromLeft = q, fromRight = q
            for i in 1..<q.count { fromLeft[i] = max(fromLeft[i - 1], q[i]) }
            for i in stride(from: q.count - 2, through: 0, by: -1) {
                fromRight[i] = max(fromRight[i + 1], q[i])
            }
            return zip(fromLeft, fromRight).map { min($0, $1) }
        }
        qx = filled(qx)
        qy = filled(qy)
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
        // One layer per slice the guest rasterizes in a pass: a mirrored guest
        // that draws both eyes at once (OVR_multiview, a layered pass) needs two.
        // They are the same curve — the compositor unwarps both eyes through
        // layer 0, so a second layer shaped differently would be unwarped wrong.
        let layers = max(1, Int(kl_glfb_rate_map_layers()))
        guard device.supportsRasterizationRateMap(layerCount: layers) else {
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

        // ---- The rate floor: a rasterization rate is a mip level ----
        //
        // A rasterization rate map does not only thin the fragments. The
        // fragment shader runs in PHYSICAL space, so `dFdx`/`dFdy` — and every
        // texture LOD Metal derives from them — measure physical pixels, not
        // screen pixels, and nothing renormalizes them. A zone rasterized at
        // rate `r` therefore samples a mip level `log2(1/r)` HIGHER than the
        // same surface would unfoveated: rate 0.5 is +1 level, 0.25 is +2, and
        // this display's own peripheral rates (about 0.12 in x and 0.10 in y)
        // are +3.1 and +3.3 levels, a tenfold over-blur.
        //
        // Flooring the per-zone rate bounds that at a stated number of mip
        // levels while keeping most of the saving: the floor is the deepest
        // over-blur we are willing to hand the guest, not an off switch for
        // foveation. On the measured curve, 0.50 takes the worst bias from +3.3
        // to +1.0 and the fragment count from about 29% to about 49% of the
        // unfoveated eye (rebelancap, shinyquagsire23/Klepton pull request 21).
        //
        // OFF here unless KL_VRR_FLOOR is set (there it defaults to 0.50). The
        // eye sizes this fork ships — Beat Saber 1.45 at the display's own
        // 6890x5526, Doom 3 at 5200 — were chosen for the fragment count of the
        // RAW curve, and a floor of 0.50 would hand those games nearly twice the
        // fragments. A game that wants it gets it with a smaller eye, per target.
        let rateFloor: Float = min(1.0, max(0.0, klEnvFloat("KL_VRR_FLOOR", 0)))
        var eyeQualityX = displayEyeQualityX
        func bias(_ q: [Float]) -> Double {
            guard let m = q.min(), m > 0 else { return 0 }
            return log2(1.0 / Double(m))
        }
        func frac(_ a: [Float], _ b: [Float]) -> Double {
            guard !a.isEmpty, !b.isEmpty else { return 1 }
            let ma = Double(a.reduce(0, +)) / Double(a.count)
            let mb = Double(b.reduce(0, +)) / Double(b.count)
            return ma * mb
        }
        let rawBiasX = bias(qx), rawBiasY = bias(qy), rawFrac = frac(qx, qy)
        if rateFloor > 0.01 {
            for i in qx.indices { qx[i] = max(qx[i], rateFloor) }
            for i in qy.indices { qy[i] = max(qy[i], rateFloor) }
            // ...and the curves per eye, which a layered guest is drawn through
            // instead of the envelope (below).
            for e in eyeQualityX.indices {
                for i in eyeQualityX[e].indices { eyeQualityX[e][i] = max(eyeQualityX[e][i], rateFloor) }
            }
        }
        NSLog("%@", "[cp] foveation LOD: rate floor "
              + (rateFloor > 0.01 ? String(format: "%.2f", rateFloor) : "off")
              + " — worst mip bias x +\(String(format: "%.2f", rawBiasX)) -> "
              + "+\(String(format: "%.2f", bias(qx))), y +\(String(format: "%.2f", rawBiasY)) -> "
              + "+\(String(format: "%.2f", bias(qy)))"
              + "; fragments \(String(format: "%.1f", 100 * rawFrac))% -> "
              + "\(String(format: "%.1f", 100 * frac(qx, qy)))% "
              + "(KL_VRR_FLOOR=<0.01..1> sets the floor; envelope figures)")
        // `__sampleCount:` is the pointer-taking initializer as Swift imports
        // it; the array-taking spelling does not exist here.
        // A layer per eye, each with that eye's OWN horizontal curve, where the
        // guest draws both eyes in one layered pass (kl_glfb_rate_map_layers) and
        // the display gave two. The envelope above is dense wherever either eye
        // wants density, which for the pair of mirrored, inboard curves is most
        // of the width: at 6890x5526 it stores 4509x2720 an eye where the
        // display itself keeps 2624x2560. With its own curve an eye stores about
        // what its display keeps — over 40% fewer fragments for the guest — and
        // unwarpGrid reads each eye through its own layer.
        //
        // Only for a guest it was looked at with (kl_glfb_rate_map_per_eye: Beat
        // Saber from 1.45 on, whose owner called both eyes fine). The two curves
        // are mirror images, each eye's dense part inboard, so one spot of the
        // scene is sharp for one eye and coarse for the other — which is what
        // the display does to every picture, and in Doom 3 it was seen all the
        // same: "auf dem linken auge nur links vom bild … aber dafür übers halbe
        // bild und auf dem rechten auge nur rechts", at 4400, at 5720 and at
        // 6759 across alike, so the size of the eye texture is not what decides
        // it (a rule by size was tried for a build and did not help). Every
        // other guest gets the envelope for both eyes, as until 1.1.5.
        // KL_VRR_PER_EYE=0|1 decides regardless.
        let perEyeWanted = klEnvOn("KL_VRR_PER_EYE", default: kl_glfb_rate_map_per_eye() != 0)
        let perEye = layers == 2 && eyeQualityX.count == 2
            && eyeQualityX[0].count == qx.count && source == "the display's own"
            && perEyeWanted
        func makeLayer(_ i: Int) -> MTLRasterizationRateLayerDescriptor {
            let hx = perEye ? eyeQualityX[i] : qx
            return hx.withUnsafeBufferPointer { hp in
                qy.withUnsafeBufferPointer { vp in
                    MTLRasterizationRateLayerDescriptor(
                        __sampleCount: MTLSize(width: hx.count, height: qy.count, depth: 0),
                        horizontal: hp.baseAddress!, vertical: vp.baseAddress!)
                }
            }
        }
        if perEye { source += ", a curve per eye" }
        else if layers == 2, source == "the display's own" { source += ", one envelope for both eyes" }
        let desc = MTLRasterizationRateMapDescriptor()
        desc.screenSize = MTLSize(width: w, height: h, depth: 0)
        desc.label = "klepton guest foveation"
        for i in 0..<layers { desc.setLayer(makeLayer(i), at: i) }
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
              + "of the fragments, \(qx.count)x\(qy.count) zones, \(layers) layer(s), \(source))")
        // Held by this property, which is what keeps it alive: kl_glfb takes an
        // unmanaged pointer and never retains, exactly as it does not retain the
        // eye textures.
        guestRateMap = map
        guestRateW = w; guestRateH = h
        // What the guest will actually write of each eye: the larger layer, and
        // two texels over for the rounding at its edge.
        var pw = 0, ph = 0
        for l in 0..<map.layerCount {
            let p = map.physicalSize(layer: l)
            pw = max(pw, p.width); ph = max(ph, p.height)
        }
        kl_glfb_set_eye_rate_map(Int32(w), Int32(h), Int32(qx.count), Int32(qy.count),
                                 Unmanaged.passUnretained(map).toOpaque())
        kl_glfb_set_eye_rate_physical(Int32(min(w, pw + 2)), Int32(min(h, ph + 2)))
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
        let layerRenderer = self.layerRenderer!
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
        let layerRenderer = self.layerRenderer!
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

        // The RealityKit proof of concept: measured, the picture now moves to
        // its own space; this loop keeps going until that layer closes and
        // then takes the RealityKit output (afterOutputEnded).
        if KLRealityPresent.enabled {
            Self.expectingHandover = true
            NSLog("[rk] picture through RealityKit is on — moving it there now that the display is measured")
            KLRealityPresent.switchAtStart()
        }

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

        csFrames()
        afterOutputEnded()
    }

    /// Frames into the Compositor Services layer until it is invalidated.
    private func csFrames() {
        let layerRenderer = self.layerRenderer!
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
        var nextHapticsReport = Date().addingTimeInterval(10)
        NSLog("[cp] render loop entered (startup frame-trace on; build has fence content-gate + diag)")
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
                arrayViews.frameDone()
            }
            iterations += 1
            if Date() >= nextReport {
                nextReport = Date().addingTimeInterval(0.5)
                cmdLock.lock(); let done = cmdCompleted; cmdLock.unlock()
                NSLog("[cp] alive: \(iterations) iters, \(presented) with a picture, "
                      + "\(blackFrames) black, guest=\(kl_app_guest_state()), "
                      + "bail(frame/drawable/pipeline)="
                      + "\(bailNoFrame)/\(bailNoDrawable)/\(bailNoPipeline), "
                      // committed vs completed: if these diverge and stay
                      // diverged, the GPU is waiting on the guest's fence and
                      // nothing is being drawn at all.
                      + "cmdbuf \(done)/\(cmdCommitted) done"
                      + " fence=\(lastGlFenceSeen) layers=\(lastProjLayersSeen) enc=\(lastEncodedSeen)"
                      + (fenceBacklogSkips > 0 ? " fence-skips \(fenceBacklogSkips)" : "")
                      + (noFence ? " (NOFENCE)" : ""))
                NSLog("[cp] \(cadenceSummary())")
                NSLog("[cp] \(stageSummary())")
                // The haptics queue's own census, every 10 s and only when it
                // has something to say (see KleptonControllers.hapticsQueue).
                if Date() >= nextHapticsReport {
                    nextHapticsReport = Date().addingTimeInterval(10)
                    if let h = controllers.hapticsSummary() { NSLog("[cp] \(h)") }
                }
            }
        }
        NSLog("[cp] render loop ended after \(presented) presented frames, "
              + "\(iterations) iterations, state=\(String(describing: layerRenderer.state))")
    }

    /// The output has gone: either for good (the crown, the game quit) or
    /// because the picture is moving to the RealityKit space
    /// (KLRealityPresent.switchAtStart), in which case that one is waited for
    /// and the guest never notices.
    private func afterOutputEnded() {
        while Self.expectingHandover {
            NSLog("[rk] output closed for the move — waiting for the RealityKit space")
            if handover.wait(timeout: .now() + 10) == .timedOut {
                NSLog("[rk] no new space within 10 s — ending as usual")
                Self.expectingHandover = false
                break
            }
            Self.expectingHandover = false
            handoverLock.lock()
            let layer = pendingLayer, rk = pendingRK
            pendingLayer = nil; pendingRK = nil
            handoverLock.unlock()
            if let layer {
                layerRenderer = layer
                rkTarget = nil
                let controllers = self.controllers
                layer.onSpatialEvent = { events in controllers.handleSpatialEvents(events) }
                layer.waitUntilRunning()
                if layer.configuration.colorFormat != pipelineFormat { buildPipeline(); boundary = nil }
                applyRenderQuality()
                displayPeriod = 0.01
                kl_ovrp_set_display_frequency(100)
                kl_app_set_panel_hz(100)
                NSLog("[rk] now on Compositor Services")
                csFrames()
            } else if let rk {
                layerRenderer = nil
                rkTarget = rk
                if KLRealityTarget.colorFormat != pipelineFormat { buildPipeline(); boundary = nil }
                NSLog("[rk] now on RealityKit")
                rkFrames(rk)
            }
        }
        // The crown press invalidates the layer with NO scenePhase change at
        // all (measured on device: none fires), and the system then takes
        // ~30-60 s to background the process — which is when Lifecycle exits.
        // Go silent the moment the picture is gone, not when the OS gets to
        // it; the guest may keep rendering until the join below.
        KleptonAudio.setSilenced(true)
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
    /// Cached, because a view per frame is an allocation per frame — and
    /// pruned against the eye table, because a view keeps its parent alive and
    /// the parents are the guest's swapchain images (see KleptonArrayViews).
    /// If the view cannot be made it says so once and returns the original —
    /// which composites exactly as badly as before, but names the reason rather
    /// than leaving a flat colour to be read as a guest that drew nothing.
    private lazy var arrayViews: KleptonArrayViews = {
        let c = KleptonArrayViews(live: Self.eyeTableTextures)
        c.onDrop = { dropped, kept in
            NSLog("[cp] dropped \(dropped) array view(s) of eye textures the guest "
                  + "has released (swapchain rebuilt), \(kept) kept")
        }
        return c
    }()
    private var saidNoArrayView = false

    private func arrayView(_ tex: MTLTexture) -> MTLTexture {
        if tex.textureType == .type2DArray { return tex }
        if let v = arrayViews.view(of: tex, make: {
            $0.makeTextureView(pixelFormat: $0.pixelFormat, textureType: .type2DArray,
                               levels: 0..<$0.mipmapLevelCount, slices: 0..<1)
        }) { return v }
        if !saidNoArrayView {
            saidNoArrayView = true
            NSLog("[cp] the eye texture is textureType \(tex.textureType.rawValue) "
                  + "and an array VIEW of it could not be made — the shaders "
                  + "sample texture2d_array, so this composites as a flat colour")
        }
        return tex
    }

    /// Every texture the eye table names, for any eye and stage: the only
    /// source of what `arrayView` is handed, on both the eye and the layer path.
    private static func eyeTableTextures() -> Set<UnsafeRawPointer> {
        var named = Set<UnsafeRawPointer>()
        for eye in 0..<2 {
            for s in 0..<Int(KL_MTL_MAX_STAGES) {
                if let t = kl_glfb_eye_mtl_texture(Int32(eye), Int32(s), nil) {
                    named.insert(UnsafeRawPointer(t))
                }
            }
        }
        return named
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
                //
                // ...and a nil CACHE entry is not one either. `eyes` is filled
                // by the GL provider path only; a Vulkan guest's eye storage
                // lives in kl_glfb's table (checked first by eyeSource) and
                // never appears here. Comparing against the empty cache flagged
                // every Vulkan run as "DIFFERENT textures" — which is what sent
                // the wrath2 black-screen hunt to the eye seam when the actual
                // fault was the overlay quad's size parse (kl_ovrp.c).
                if read && guestTex != nil && mineTex != nil && guestTex != mineTex { bad = true }
                // Only the low bits: two pointers into the same heap differ
                // there, and a full 64-bit address per eye per stage makes the
                // line unreadable at the moment it matters most.
                let tag = guestTex.map { String(UInt(bitPattern: $0) & 0xffffff, radix: 16) } ?? "nil"
                out += " e\(eye)=\(tag)"
                if !read { out += "?" }
                else if guestTex != nil && mineTex != nil && guestTex != mineTex { out += "!=SAMPLED" }
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

    /// KL_CP_LATE_WAIT_MS (budget for this composite, in ms; 0 = off): a frame
    /// of the game that is still on the GPU when this pass takes the newest
    /// finished one is waited for, until the rendering deadline less that
    /// budget. LEGO Bricktales (2026-10-18): the game started 100 frames a
    /// second and 91-98 were shown — the rest finished a few milliseconds after
    /// this pass had taken the one before. Only frames that go through the
    /// KL_VK_FRAME_ASYNC worker can be waited for. Default 4.5 for the titles
    /// that have the worker on by default (LEGO, Arkham Shadow, Moss — the
    /// measurements are at klovrp_frame_async_title in kl_ovrp.c); LEGO's pass
    /// needs about 3 ms of GPU.
    private static let lateWaitBudgetMs: Double = Double(klEnvFloat(
        "KL_CP_LATE_WAIT_MS", Float(kl_ovrp_late_wait_default_ms())))
    private var lateWait = (n: 0, filed: 0, gaveUp: 0, none: 0, waitedMs: 0.0, slackMs: 0.0)
    private func waitForInflightFrame(_ drawable: LayerRenderer.Drawable) {
        let budget = Self.lateWaitBudgetMs
        guard budget > 0 else { return }
        let clock = LayerRenderer.Clock()
        let now = LayerRenderer.Clock.Instant.epoch.duration(to: clock.now).timeInterval
        let deadline = LayerRenderer.Clock.Instant.epoch
            .duration(to: drawable.frameTiming.renderingDeadline).timeInterval
        let slackMs = (deadline - now) * 1000 - budget
        lateWait.n += 1
        lateWait.slackMs += slackMs
        if slackMs > 0.2 {
            let r = kl_ovrp_wait_inflight(slackMs)
            if r == 0 { lateWait.none += 1 }
            else {
                if r > 0 { lateWait.filed += 1 } else { lateWait.gaveUp += 1 }
                let after = LayerRenderer.Clock.Instant.epoch.duration(to: clock.now).timeInterval
                lateWait.waitedMs += (after - now) * 1000
            }
        }
        if lateWait.n >= 1000 {
            let w = lateWait
            NSLog(String(format: "[cp] late wait over %d frames (budget %.1f ms): slack %.1f ms mean; "
                         + "a frame in flight %d times — filed in time %d, gave up %d, %.2f ms waited mean",
                         w.n, budget, w.slackMs / Double(w.n), w.filed + w.gaveUp, w.filed, w.gaveUp,
                         w.filed + w.gaveUp > 0 ? w.waitedMs / Double(w.filed + w.gaveUp) : 0))
            lateWait = (0, 0, 0, 0, 0, 0)
        }
    }

    private func renderFrame() -> Int {
        if let rk = rkTarget { return rkRenderFrame(rk) }
        guard let frame = layerRenderer!.queryNextFrame() else { bailNoFrame += 1; return 0 }
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
        publishToGuest()

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
        let n = composeFrame(drawables.map(KLDrawable.init), cmd: cmd, raw: drawables)
        frame.endSubmission()      // only here — see startSubmission above
        return n
    }

    /// Offer the guest this frame's pose (see renderFrame).
    private func publishToGuest() {
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
                if kl_app_guest_asked_quit() != 0 {
                    // The game quit by itself (KL_UNITY_QUIT_EXITS, kl_app.c):
                    // on a Quest its activity is finished and the player is
                    // back in the home space, and holding its last picture
                    // here would only be something to close by hand. Left
                    // from the main thread, not from inside this submission,
                    // and the way backgrounding leaves.
                    NSLog("[cp] the guest thread has ended: the game asked to quit — leaving the app")
                    DispatchQueue.main.async { Lifecycle.leave() }
                } else {
                    NSLog("[cp] the guest thread has ended (\(String(cString: kl_app_status())))"
                          + " — everything from here is its last picture, reprojected")
                }
            }
        }
    }

    /// Everything after the drawables are in hand: guest picture, poses,
    /// composite, present. Shared by the Compositor Services loop and the
    /// RealityKit one (rkRenderFrame), which differ only in where the drawable
    /// comes from.
    private func composeFrame(_ drawables: [KLDrawable], cmd: MTLCommandBuffer,
                              raw: [LayerRenderer.Drawable],
                              presetAnchor: DeviceAnchor? = nil, hasPresetAnchor: Bool = false,
                              finish: ((MTLCommandBuffer) -> Void)? = nil) -> Int {
        let main = drawables[0]
        // The late wait reads the Compositor Services deadline; the RealityKit
        // output has none and goes without it.
        if let first = raw.first { waitForInflightFrame(first) }
        if displayQualityX.isEmpty, let first = raw.first { captureDisplayFoveation(first) }

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
        lastGlFenceSeen = glFence; lastProjLayersSeen = projLayerCount
        let frameValue = glFence != 0 ? glFence : kl_vulkan_frame_serial()
        // KL_CP_NOFENCE=1 skips the wait. A composite command buffer that waits
        // on an event value the guest's queue never signals is COMMITTED and
        // never EXECUTES: the drawable is presented, every counter here looks
        // healthy, and the display shows nothing. That failure is invisible
        // from this side, so it needs its own A/B — and cmdCompleted below is
        // the measurement that says whether it is happening.
        // Only wait on the guest fence while the backlog of committed-but-not-
        // completed composite buffers is small. Once it grows, the guest's
        // signal is not arriving (it is heads-down), and continuing to wait
        // would starve the drawable pool and get the app watchdog-killed — so
        // present without the wait until the guest catches up. A no-op for a
        // healthy guest, whose buffers complete every frame (backlog stays 0-1).
        //
        // Two gates, both about NOT deadlocking the drawable pool:
        //   1. Content: a black frame (no projection layers yet) has nothing to
        //      order against, so it must not wait — the guest sets the fence
        //      non-zero on its first swap but then goes heads-down (loading a
        //      multi-GB OBB, compiling shaders) without signalling it. Waiting
        //      there is a command buffer that never completes; it holds the one
        //      drawable, the next frame blocks acquiring one, the loop stops
        //      presenting after a SINGLE committed buffer (cmdbuf 0/1), and
        //      visionOS SIGKILLs with no report. Present black, unwaited.
        //   2. Backlog: even with content, if committed buffers stop completing
        //      the guest is stalling — present without the wait so a drawable
        //      stays free. Belt-and-braces behind gate 1.
        // Both are no-ops for a healthy guest: it submits layers every frame
        // and its buffers complete, so the wait is always encoded as before.
        cmdLock.lock(); let doneNow = cmdCompleted; cmdLock.unlock()
        let outstanding = cmdCommitted - doneNow
        // ...or eyes that are copied into place and fenced per frame
        // (kl_glfb_frame_fence): there the fence value advances with every
        // submitted frame and is always one ANGLE has already been handed, so
        // waiting on it cannot be a wait on a guest that has gone quiet.
        let haveGuestContent = projLayerCount > 0 || kl_glfb_frame_fenced() != 0
        if let ev = guestFrameEvent, glFence != 0, !noFence,
           haveGuestContent, outstanding <= maxFenceBacklog {
            cmd.encodeWaitForEvent(ev, value: glFence)
        } else if glFence != 0 && !noFence && (!haveGuestContent || outstanding > maxFenceBacklog) {
            fenceBacklogSkips += 1
            if !warnedFenceBacklog {
                warnedFenceBacklog = true
                NSLog("[cp] guest fence set (\(glFence)) but "
                      + (haveGuestContent
                         ? "backlog \(outstanding) buffers not completed"
                         : "no projection layers yet")
                      + " — presenting WITHOUT the fence wait so the drawable pool "
                      + "does not starve (the guest is still loading). "
                      + "Further skips silenced.")
            }
        }
        do {    // counted only once the metrics panel has been shown (kl_metrics_begin)
            let again = frameValue != 0 && frameValue == lastGuestFrame
            kl_metrics_note_shown(frameValue != 0 && !again ? 1 : 0, again ? 1 : 0)
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
        fgPresentation = presentation
        // The RealityKit path decides the pose before the composite, because
        // the quad's corners are written for it in the same command buffer.
        let displayAnchor = hasPresetAnchor ? presetAnchor : displayAnchor(at: presentation)
        let originFromDevice = originFromDevice(displayAnchor)
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
        if Self.pendingRecenter {
            Self.pendingRecenter = false
            // Yaw + horizontal position only, exactly Quest's recenter: height
            // and pitch/roll stay physical. Build the head's yaw-XZ frame and
            // fold its inverse into the origin, so from the NEXT frame the
            // current gaze direction is the world's -Z at the world's origin.
            let c2 = originFromHead.columns.2
            let yaw = atan2f(c2.x, c2.z)          // head +Z projected to XZ
            let px = originFromHead.columns.3.x
            let pz = originFromHead.columns.3.z
            let cy = cosf(yaw), sy = sinf(yaw)
            let m = simd_float4x4(
                simd_float4( cy, 0, -sy, 0),
                simd_float4(  0, 1,   0, 0),
                simd_float4( sy, 0,  cy, 0),
                simd_float4( px, 0,  pz, 1))
            recenterXform = m.inverse * recenterXform
            controllers.trackingRebase = recenterXform
            // The new tracking-space poses first, then the count the guest
            // reads (kl_ovrp_notify_recenter, GetLocalTrackingSpaceRecenterCount).
            updatePoses(at: ProcessInfo.processInfo.systemUptime)
            kl_ovrp_notify_recenter()
            NSLog("[cp] RECENTER: yaw %.1f deg, pos (%.2f, %.2f) folded into the origin",
                  yaw * 180 / .pi, px, pz)
        }

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
        // EXPERIMENT (missioniss head-lock): log the DISPLAY head yaw (live) next
        // to the RENDER pose yaw (what the guest drew), once a second. Both
        // tracking together means the reprojection inputs are correct and the
        // frozen view is the guest's own render; a fixed display yaw means the
        // compositor is not getting live head rotation.
        Self.headLockTraceN += 1
        if Self.headLockTraceN % 90 == 0, projLayerCount > 0 {
            let c2 = originFromHead.columns.2
            let dispYaw = atan2f(c2.x, c2.z) * 180 / .pi
            let pl = projLayers[0]
            let qy = pl.pose.4, qz = pl.pose.5, qw = pl.pose.6, qx = pl.pose.3
            let rendYaw = atan2f(2*(qw*qy + qx*qz), 1 - 2*(qy*qy + qz*qz)) * 180 / .pi
            NSLog("[cp] headlock-probe: display yaw %.1f  render yaw %.1f", dispYaw, rendYaw)
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
                  + ((layerRenderer?.configuration.layout ?? .layered) == .layered ? "layered" : "dedicated")
                  + ", render quality \(layerRenderer.map { "\($0.renderQuality)" } ?? "-") of max "
                  + "\(layerRenderer.map { "\($0.configuration.maxRenderQuality)" } ?? "-")")
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

        traceGazeMaps(raw)

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
        // When this buffer was handed to Metal, on the clock gpuStartTime and
        // gpuEndTime are expressed in — see gpuQueuedSum.
        finish?(cmd)
        let committedAt = CACurrentMediaTime()
        cmd.addCompletedHandler { [weak self] b in
            guard let self else { return }
            self.cmdLock.lock()
            self.cmdCompleted += 1
            if b.gpuEndTime > 0 && b.gpuStartTime >= committedAt {
                let queued = b.gpuStartTime - committedAt
                let ran = b.gpuEndTime - b.gpuStartTime
                self.gpuTimedN += 1
                self.gpuQueuedSum += queued; self.gpuQueuedMax = max(self.gpuQueuedMax, queued)
                self.gpuRanSum += ran;       self.gpuRanMax = max(self.gpuRanMax, ran)
            }
            let n = self.cmdCompleted
            let first = self.cmdError == nil && b.error != nil
            if first { self.cmdError = b.error }
            self.cmdLock.unlock()
            kl_metrics_note_composite(b.gpuEndTime - b.gpuStartTime)
            if n <= 12 { NSLog("[cp] cmdbuf #\(n) COMPLETED" + (b.error != nil ? " err=\(b.error!)" : "")) }
            if first { NSLog("[cp] composite command buffer error: \(b.error!)") }
        }
        cmd.commit()
        cmdCommitted += 1
        lastEncodedSeen = encoded
        if startupTrace < 300 {
            startupTrace += 1
            cmdLock.lock(); let dn = cmdCompleted; cmdLock.unlock()
            NSLog("[cp] trace f\(startupTrace) fence=\(glFence) layers=\(projLayerCount) "
                  + "enc=\(encoded) cmd=\(cmdCommitted)/\(dn)")
        }
        noteCadence(presentation: presentation, drawable: main)
        return encoded > 0 ? 1 : 0
    }

    // MARK: MetalFX spatial upscale
    //
    // The guest renders small (the "Resolution" setting) and this puts its
    // pictures back up to the size they would have had, with Apple's spatial
    // scaler, before the composite samples them — instead of the composite's
    // own linear stretch. Single frame in, single frame out, no motion data.
    //
    // Every way a picture reaches the composite is upscaled:
    //   * the eye pass — a 2-slice array both eyes share (the GL eye provider,
    //     Vulkan through OVRPlugin), a texture per eye (Vulkan through one
    //     swapchain per eye, which `arrayView` wraps), Vulkan foveation (kl_fove
    //     expands its eyes back to screen layout before this sees them);
    //   * a GL eye drawn through the display's rate map (kl_glfb_eye_rate_map):
    //     the texture holds the squeezed picture in its top-left physical part.
    //     Only that part is upscaled (the scaler's input content), uniformly, so
    //     the squeeze keeps its shape and the unwarp grid is built normalised to
    //     that part (`MfxOrigin.content`) — the same texture coordinates land on
    //     the same scene point in the copy;
    //   * projection layers (every OpenXR guest, KL_XR_LAYERS=1: Alyx, Steam
    //     Link, Doom 3, Walkabout, Beat Saber 1.45): each layer and each of its
    //     eyes is upscaled from its own texture — a layer's rect is a fraction
    //     of its texture (kl_proj_layer_grid), so it holds for the copy as it
    //     is. One layer's eyes both or neither; the layers of a frame share a
    //     budget of output pixels (KL_METALFX_BUDGET, in full eyes, 4 by
    //     default) so a guest with many layers cannot put eight full-size
    //     scalers on the pass that has the least time, and a layer past it is
    //     said once and drawn linear.
    // What keeps the linear path, each said once (`mfxSay`) and shown on the
    // metrics panel: a format outside `mfxFormats`, multisampling, a view
    // MoltenVK or the guest made of another texture (the scaler's input must
    // be a plain 2D texture a slice), eyes of different sizes in the eye pass
    // (its grid is built from slot 0), and a guest started at 100 % — nothing
    // to restore (kl_ovrp_eye_texture_scale).

    /// One upscaled copy, its per-slice output views and the scaler for that
    /// geometry. Keyed by the shape AND by which source of the frame it serves
    /// (the eye pass's slot 0 or 1, or a layer's index and slot): two textures
    /// must never write into one copy, while the stages of one swapchain — only
    /// one of which is composited in a frame — can share it, which is the memory.
    private struct MfxTarget {
        let texture: MTLTexture
        let outViews: [MTLTexture]
        let scaler: any MTLFXSpatialScaler
        var lastUse: UInt64
    }
    /// An upscaled copy's raw texture, and the part of it the copy holds — the
    /// whole texture, or the physical part of a foveated one — in its pixels.
    private struct MfxOrigin {
        let source: MTLTexture
        let content: SIMD2<Float>
    }
    /// What the metrics panel says about the upscale (kl_metrics_note_upscale).
    private struct MfxStats: Equatable {
        var state: Int32 = 0          // 0 off, 1 on and linear, 2 upscaled
        var note = ""                 // why linear, in a few words
        var outW = 0, outH = 0        // the copy, in the picture's own (screen) pixels
    }
    private var mfxTargets: [String: MfxTarget] = [:]
    /// This frame's swap, rebuilt every frame: raw texture -> upscaled copy.
    private var mfxUpgraded: [ObjectIdentifier: MTLTexture] = [:]
    /// ...and back: upscaled copy -> the raw texture it was made from. The
    /// composite builds its grids against the raw texture's pixels normalised
    /// to `content` (see unwarpGrid's `uvSize`) and the stats name the size the
    /// game draws, not this one. This frame's only — keyed across frames, a
    /// freed texture's identifier is the next one's.
    private var mfxOrigin: [ObjectIdentifier: MfxOrigin] = [:]
    /// The last fresh upscale and the guest frame it was built for. The
    /// composite runs at display rate while a heavy guest only delivers 30-50
    /// pictures a second: without this every repeat re-encodes the identical
    /// upscale and the scaler's memory traffic contends with the guest's own
    /// GPU. Holds every raw texture the frame looked at (`sources`), so their
    /// identifiers cannot be reused while it is consulted.
    private var mfxCache: (frame: UInt64, sources: [MTLTexture],
                           upgraded: [ObjectIdentifier: MTLTexture],
                           origin: [ObjectIdentifier: MfxOrigin], keys: [String],
                           stats: MfxStats)?
    private var mfxTick: UInt64 = 0
    private var mfxLastShape = ""
    private var mfxLastOn = false
    private var mfxPause = KleptonMetalFXPause(pauseInMR: klEnvOn("KL_MFX_MR_PAUSE", default: false))
    /// This frame puts the source's alpha back into each copy (mixed reality).
    private var mfxRestoreAlpha = false
    private lazy var mfxAlpha: KleptonMetalFXAlpha = {
        let a = KleptonMetalFXAlpha(device: device)
        a.onError = { NSLog("[metalfx] \($0)") }
        return a
    }()
    private var mfxSaid = Set<String>()
    private var mfxStats = MfxStats()
    private lazy var mfxSupported = MTLFXSpatialScalerDescriptor.supportsDevice(device)
    /// What the scaler is given. Others keep the linear path.
    private static let mfxFormats: Set<MTLPixelFormat> = [
        .rgba16Float, .rgba8Unorm, .rgba8Unorm_srgb, .bgra8Unorm, .bgra8Unorm_srgb,
    ]
    /// A copy nobody asked for in this many frames is dropped (a few seconds):
    /// a swapchain re-created at another size, a toggle that stays off.
    private static let mfxKeepTicks: UInt64 = 600
    /// KL_METALFX_COLOR=hdr|linear|perceptual: one colour processing mode for
    /// every format MetalFX allows it for, for A/B tests — unset, RGBA16F is
    /// `.hdr` and the rest `.perceptual`. An sRGB format is always
    /// `.perceptual` (see mfxMakeTarget). Read once; a copy made before keeps
    /// its mode.
    private static let mfxColorOverride: MTLFXSpatialScalerColorProcessingMode? = {
        let v = ProcessInfo.processInfo.environment["KL_METALFX_COLOR"]?.lowercased() ?? ""
        let mode: MTLFXSpatialScalerColorProcessingMode?
        switch v {
        case "": return nil
        case "hdr": mode = .hdr
        case "linear": mode = .linear
        case "perceptual": mode = .perceptual
        default: mode = nil
        }
        NSLog(mode != nil ? "[metalfx] colour processing \(v) for every format (KL_METALFX_COLOR)"
                          : "[metalfx] KL_METALFX_COLOR=\(v) is not hdr, linear or perceptual — ignored")
        return mode
    }()
    /// The projection layers' share of output pixels a frame, in full eyes.
    private static let mfxLayerBudget = max(1, min(16, klEnvFloat("KL_METALFX_BUDGET", 4)))

    /// Says once why a picture keeps the linear path, and gives the panel the
    /// short form.
    private func mfxSay(_ reason: String, short: String) {
        if mfxStats.state != 2 { mfxStats.state = 1; mfxStats.note = short }
        // Bounded: a reason that names sizes can come in many forms in a run.
        guard mfxSaid.count < 64, mfxSaid.insert(reason).inserted else { return }
        NSLog("[metalfx] linear path: \(reason)")
    }

    /// The part of `tex` the guest wrote, in its pixels: the whole texture, or
    /// for one drawn through the display's rate map (the eye provider's, or a
    /// mirrored layer's — a texture of the map's screen size) the map's
    /// physical size, top left, two texels over for the rounding at its edge
    /// as kl_glfb copies it. The unwarp grid maps the screen into exactly these
    /// texels, so a copy of them alone loses nothing the composite reads.
    private func mfxContent(_ tex: MTLTexture) -> SIMD2<Float> {
        let whole = SIMD2<Float>(Float(tex.width), Float(tex.height))
        guard let mp = kl_glfb_eye_rate_map(),
              let rm = Unmanaged<AnyObject>.fromOpaque(mp).takeUnretainedValue()
                  as? MTLRasterizationRateMap,
              rm.screenSize.width == tex.width, rm.screenSize.height == tex.height else { return whole }
        var pw = 0, ph = 0
        for l in 0..<rm.layerCount {
            let p = rm.physicalSize(layer: l)
            pw = max(pw, p.width); ph = max(ph, p.height)
        }
        guard pw > 0, ph > 0 else { return whole }
        return SIMD2<Float>(Float(min(tex.width, pw + 2)), Float(min(tex.height, ph + 2)))
    }

    /// One source, planned: where the scaler reads each slice from, which part
    /// of it, and which copy it writes into. Nothing is encoded until every
    /// source of its group has a plan — both eyes of the eye pass, or of one
    /// layer, are upscaled or neither is.
    private struct MfxPlan {
        let source: MTLTexture
        let input: (Int) -> MTLTexture?
        let slices: [Int]
        let content: (w: Int, h: Int)
        let key: String
        let out: (w: Int, h: Int)
    }

    private func mfxPlan(_ src: MTLTexture, slices: Set<Int>, key slot: String,
                         rs: Float, fullW: Int, fullH: Int) -> MfxPlan? {
        guard Self.mfxFormats.contains(src.pixelFormat) else {
            mfxSay("texture format \(src.pixelFormat.rawValue)", short: "format"); return nil
        }
        guard src.usage.contains(.shaderRead), src.sampleCount == 1 else {
            mfxSay("texture usage \(src.usage.rawValue), samples \(src.sampleCount)", short: "usage")
            return nil
        }
        // The scaler reads one plain 2D texture a slice.
        let input: (Int) -> MTLTexture?
        if src.textureType == .type2DArray, src.parent == nil {
            input = { s in
                src.makeTextureView(pixelFormat: src.pixelFormat, textureType: .type2D,
                                    levels: 0..<1, slices: s..<s + 1)
            }
        } else if src.textureType == .type2DArray, src.arrayLength == 1,
                  let p = src.parent, p.textureType == .type2D, p.parent == nil,
                  p.width == src.width, p.height == src.height,
                  p.pixelFormat == src.pixelFormat, p.sampleCount == 1,
                  src.parentRelativeLevel == 0, src.parentRelativeSlice == 0,
                  p.usage.contains(.shaderRead) {
            // `arrayView` of a texture per eye: read the texture itself.
            input = { s in s == 0 ? p : nil }
        } else {
            mfxSay("texture type \(src.textureType.rawValue), parent "
                   + "\(src.parent.map { "type \($0.textureType.rawValue)" } ?? "nil"), "
                   + "\(src.arrayLength) slice(s)", short: "texture view")
            return nil
        }
        let c = mfxContent(src)
        let cw = Int(c.x), ch = Int(c.y)
        // What the guest would have drawn at 100 %: the part it wrote over the
        // scale it was started with — never more pixels than the eye that scale
        // was taken from. The slider only takes away, so restoring more than
        // 100 % invents pixels the game never drew — measured without a hold
        // 5760x3696 (+31 % over the display), composite GPU 8.6 -> 34.8 ms and
        // cadence 10 -> 21 ms. Held by AREA, uniformly: a texture the guest
        // shaped otherwise (Doom 3's square, BONELAB's widened eye) keeps its
        // shape and its aspect never distorts, where a box of the eye's own
        // shape cut the long side short. Even, so a halved viewport stays whole.
        func even(_ v: Double) -> Int { (Int(v + 0.5) + 1) & ~1 }
        func evenDown(_ v: Double) -> Int { max(Int(v) & ~1, 2) }
        var dstW = even(Double(cw) / Double(rs))
        var dstH = even(Double(ch) / Double(rs))
        let fullArea = Double(fullW) * Double(fullH)
        if Double(dstW) * Double(dstH) > fullArea {
            let k = (fullArea / (Double(dstW) * Double(dstH))).squareRoot()
            dstW = evenDown(Double(dstW) * k)
            dstH = evenDown(Double(dstH) * k)
        }
        guard dstW > cw, dstH > ch else {
            mfxSay("texture \(src.width)x\(src.height) (written \(cw)x\(ch)) is already at the "
                   + "full size \(fullW)x\(fullH)", short: "full size")
            return nil
        }
        let slices = slices.filter { $0 >= 0 && $0 < src.arrayLength }.sorted()
        guard !slices.isEmpty else { return nil }
        let key = "\(slot) \(src.width)x\(src.height)x\(src.arrayLength) \(cw)x\(ch)->\(dstW)x\(dstH) "
                  + "\(src.pixelFormat.rawValue)"
        return MfxPlan(source: src, input: input, slices: slices, content: (cw, ch), key: key,
                       out: (dstW, dstH))
    }

    /// The scaler first, then the copy with the usage the scaler asks for: its
    /// output is written by MetalFX's own passes, and a texture without what
    /// `outputTextureUsage` names is undefined on the GPU, not an error here.
    private func mfxMakeTarget(_ src: MTLTexture, width: Int, height: Int) -> MfxTarget? {
        let d = MTLFXSpatialScalerDescriptor()
        d.colorTextureFormat = src.pixelFormat
        d.outputTextureFormat = src.pixelFormat
        // The whole texture: the part read is the encode's input content.
        d.inputWidth = src.width; d.inputHeight = src.height
        d.outputWidth = width; d.outputHeight = height
        // An sRGB texture takes `.perceptual` and nothing else: MetalFX makes
        // no scaler for an sRGB format with `.linear` or `.hdr` — nil, and an
        // assertion under the API validation ("sRGB inputs or outputs not
        // supported with non-perceptual color processing"). That was every
        // GL guest with an sRGB eye (GL_SRGB8_ALPHA8, format 71): Beat Saber
        // 1.45 on the headset, 2026-10-15, "no scaler for 4306x3454 ->
        // 2386x2302 format 71" and the panel saying linear. Measured on a Mac
        // (M4): 71 and 81 give a scaler only with `.perceptual`; 70 and 115
        // with all three.
        let srgb = src.pixelFormat == .rgba8Unorm_srgb || src.pixelFormat == .bgra8Unorm_srgb
        switch src.pixelFormat {
        case .rgba16Float: d.colorProcessingMode = .hdr
        default: d.colorProcessingMode = .perceptual
        }
        if let forced = Self.mfxColorOverride {
            if !srgb || forced == .perceptual { d.colorProcessingMode = forced }
            else if mfxSaid.insert("color-srgb").inserted {
                NSLog("[metalfx] KL_METALFX_COLOR does not apply to the sRGB format "
                      + "\(src.pixelFormat.rawValue) — MetalFX takes only perceptual there")
            }
        }
        guard let sc = d.makeSpatialScaler(device: device) else {
            mfxSay("no scaler for \(src.width)x\(src.height) -> \(width)x\(height) "
                   + "format \(src.pixelFormat.rawValue)", short: "no scaler")
            return nil
        }
        guard src.usage.contains(sc.colorTextureUsage) else {
            mfxSay("texture usage \(src.usage.rawValue) lacks what the scaler reads "
                   + "with (\(sc.colorTextureUsage.rawValue))", short: "usage")
            return nil
        }
        let desc = MTLTextureDescriptor()
        desc.textureType = .type2DArray
        desc.pixelFormat = src.pixelFormat
        desc.width = width; desc.height = height
        desc.arrayLength = src.arrayLength
        desc.mipmapLevelCount = 1
        // .renderTarget for the alpha pass in mixed reality (KleptonMetalFXAlpha).
        desc.usage = sc.outputTextureUsage.union([.shaderRead, .renderTarget])
        desc.storageMode = .private
        guard let t = device.makeTexture(descriptor: desc) else { return nil }
        t.label = "klepton metalfx \(width)x\(height)"
        var views: [MTLTexture] = []
        for s in 0..<t.arrayLength {
            guard let v = t.makeTextureView(pixelFormat: t.pixelFormat, textureType: .type2D,
                                            levels: 0..<1, slices: s..<s + 1) else { return nil }
            views.append(v)
        }
        return MfxTarget(texture: t, outViews: views, scaler: sc, lastUse: mfxTick)
    }

    /// Upscales this frame's pictures on `cmd` — the eye pass's eyes, or every
    /// projection layer's — or leaves `mfxUpgraded` empty for what keeps the
    /// linear path, which then composites exactly as before.
    // MARK: Frame generation (moonshot s3-framegen)
    //
    // With the game at half the display rate (KL_GUEST_PACE_DIV=2, which the test switch turns on with
    // it), every second drawable has no new game picture and today shows the last one again, timewarped.
    // Here that repeat becomes a generated picture instead: each new game picture's luminance pyramid
    // is kept (two per eye), and on the first repeat after it the newest picture is moved on by half its
    // motion since the one before – with the head's turn taken out, because the timewarp after this does
    // that part. The result goes through the same swap MetalFX uses (mfxUpgraded), so the composite
    // samples it exactly where it would have sampled the game's own texture.
    private var fg: KLFrameGen?
    private var fgFailed = false
    /// One game picture as the generator keeps it: its pyramid for the search – or, where the game sent
    /// its own motion vectors (XR_FB_space_warp), the field made from them and the picture pixels one
    /// of its cells covers instead – with the pose and frustum it was drawn with. `serial` tells pictures
    /// apart (fgPrevOf).
    private struct FgSide { let pyramid: KLFrameGen.Pyramid?; let field: MTLTexture?; let cell: SIMD2<Float>
                            let q: simd_quatf; let tangents: SIMD4<Float>; let view: SIMD4<Float>
                            let topDown: Bool; let width: Int; let height: Int; let rect: SIMD4<Int32>; let serial: Int }
    private var fgSerial = 0
    /// When this drawable is shown (composeFrame), and when the two newest game pictures were first
    /// shown: the generated picture's place between them is (now - newest) / (newest - older), so a
    /// game that delivers unevenly – Half-Life: Alyx below its 50 – gets each in-between where it
    /// belongs, and a second or third repeat one further on (up to one whole game frame).
    private var fgPresentation: Double = 0
    private var fgTimes: (older: Double, newer: Double) = (0, 0)
    /// The game's time between pictures, smoothed (0 = none yet): "between" only for a game that is
    /// slower than the display for good, not at one late picture.
    private var fgSpanAvg: Double = 0
    /// The display's frame time, from the drawables' presentation times (0 = not known yet).
    private var fgPeriod: Double = 0
    private var fgLastPresentation: Double = 0
    private var fgHist: [String: (older: FgSide?, newer: FgSide)] = [:]
    private var fgParity: [String: Int] = [:]
    /// One output per picture source (a layer, or the eye pass), not per texture: the game rotates
    /// three swapchain stages and rebuilds them at a scene change, and an output per texture grew by a
    /// full eye copy each time – 580 MiB at Beat Saber's size, until visionOS ended the app on leaving a
    /// level (headset, 2026-10-28). Only the drawn part of a foveated eye is held.
    private var fgOut: [String: MTLTexture] = [:]
    private var fgDone: (frame: UInt64, stale: Int) = (0, -1)
    private var fgDoneSwap: [(ObjectIdentifier, MTLTexture, MfxOrigin)] = []
    private var fgSaid: Set<String> = []
    private var fgRateMap: (map: MTLRasterizationRateMap?, buffer: MTLBuffer?) = (nil, nil)
    /// KL_FRAMEGEN_SCREEN=1: a foveated eye's motion is found and applied in the map's even screen pixels
    /// (KLFrameGen.pyramid) rather than in its squeezed physical ones. A third of the cost, but fast
    /// things at the edge of the view are found worse (synthetic test, 2026-10-28: 32.9 against 36.6 dB
    /// at 80 px a frame), so it is a lever to compare, not the default.
    private let fgScreenSpace = klEnvOn("KL_FRAMEGEN_SCREEN", default: false)
    /// "Between" only: the colours of the newest game picture, kept for the next one, whose "older"
    /// it then is – what the newer picture has covered is only there (KLFrameGen.generate's olderColor).
    /// One per output, in the output's layout; `fgPrevOf` says which pyramid it belongs to.
    private var fgPrev: [String: MTLTexture] = [:]
    private var fgPrevOf: [String: Int] = [:]
    /// Whether an OVRPlugin game draws into the space-warp images it asked for (KLFrameGen.countWritten,
    /// read back when the GPU is done): eight pictures in a row with nothing in them and the eye pass is
    /// searched instead – PowerWash Simulator VR asks for them and leaves them untouched – until the game
    /// draws into them again.
    private final class FgMvWatch {
        private let lock = NSLock()
        private var emptyRun = 0
        func report(_ written: UInt32) {
            lock.lock(); defer { lock.unlock() }
            let was = emptyRun >= 8
            emptyRun = written == 0 ? emptyRun + 1 : 0
            if was != (emptyRun >= 8) {
                NSLog(was ? "[framegen] the game draws its motion vectors again – using them"
                          : "[framegen] the game asked for motion-vector images but draws nothing into them – searching instead")
            }
        }
        var empty: Bool { lock.lock(); defer { lock.unlock() }; return emptyRun >= 8 }
    }
    private let fgMvWatch = FgMvWatch()
    private var fgMvCount: [MTLBuffer] = []
    private var fgMvCountAt = 0
    private func fgSay(_ key: String, _ text: String) {
        if fgSaid.insert(key).inserted { NSLog("[framegen] \(text)") }
    }

    /// The same texel layout without sRGB: read and written raw, so the bytes pass through unchanged.
    private static func fgLinear(_ f: MTLPixelFormat) -> MTLPixelFormat {
        switch f {
        case .rgba8Unorm_srgb: return .rgba8Unorm
        case .bgra8Unorm_srgb: return .bgra8Unorm
        default: return f
        }
    }

    /// The head's turn as a pixel map of a `size` picture whose cone `tangents` covers only `view` of it
    /// (fractions; zero = all of it): worked out in that part and moved to where it lies.
    private static func fgTurn(from: simd_quatf, to: simd_quatf, tangents: SIMD4<Float>, size: SIMD2<Float>,
                               topDown: Bool, view: SIMD4<Float>) -> simd_float3x3 {
        guard view.z > view.x, view.w > view.y else {
            return KLFrameGen.rotationMap(from: from, to: to, tangents: tangents, size: size, topDown: topDown)
        }
        let o = SIMD2(view.x, view.y) * size, part = SIMD2(view.z - view.x, view.w - view.y) * size
        let r = KLFrameGen.rotationMap(from: from, to: to, tangents: tangents, size: part, topDown: topDown)
        let t = simd_float3x3(SIMD3(1, 0, 0), SIMD3(0, 1, 0), SIMD3(o.x, o.y, 1))
        let back = simd_float3x3(SIMD3(1, 0, 0), SIMD3(0, 1, 0), SIMD3(-o.x, -o.y, 1))
        return t * r * back
    }

    private func fgPrepass(stage: Int, on cmd: any MTLCommandBuffer, rendered: kl_ovrp_render_pose, haveRendered: Bool) {
        guard KleptonFrameGen.isOn else {
            if !fgHist.isEmpty || !fgOut.isEmpty { fgHist = [:]; fgOut = [:]; fgDoneSwap = []; fgPrev = [:]; fgPrevOf = [:] }
            return
        }
        guard kl_ovrp_eye_layer_live() != 0, !fgFailed else { return }
        if fg == nil {
            do { fg = try KLFrameGen(device: device); fgSay("on", "on – generated pictures in the display frames without a new game picture") }
            catch { fgFailed = true; NSLog("[framegen] cannot build its shaders: \(error)"); return }
        }
        guard let fg else { return }
        let frame = lastGuestFrame
        // A second drawable of the same display frame reuses what the first one made.
        if fgDone.frame == frame && fgDone.stale == staleInARow {
            for (k, v, o) in fgDoneSwap { mfxUpgraded[k] = v; mfxOrigin[ObjectIdentifier(v)] = o }
            return
        }
        fgDone = (frame, staleInARow); fgDoneSwap = []
        if fgLastPresentation > 0 {
            let d = fgPresentation - fgLastPresentation
            if d > 0.004, d < 0.040 { fgPeriod = fgPeriod == 0 ? d : fgPeriod * 0.9 + d * 0.1 }
        }
        fgLastPresentation = fgPresentation

        // This frame's pictures: (key, array texture, slice, pose, tangents, row 0 at the top).
        typealias Src = (key: String, tex: MTLTexture, slice: Int, q: simd_quatf, tangents: SIMD4<Float>, topDown: Bool, eye: Int,
                         sub: SIMD4<Float>, mv: MTLTexture?, mvSlice: Int, mvSub: SIMD4<Float>, view: SIMD4<Float>)
        var srcs: [Src] = []
        // The eye pass's motion-vector image, looked at on every new picture (fgMvWatch).
        var watch: MTLTexture? = nil
        if projLayerCount > 0 {
            for i in 0..<min(projLayerCount, projLayers.count) {
                let pl = projLayers[i]
                let q = simd_quatf(ix: pl.pose.3, iy: pl.pose.4, iz: pl.pose.5, r: pl.pose.6)
                for vi in 0..<2 {
                    let slot = vi == 1 ? pl.slot.1 : pl.slot.0
                    var slice: Int32 = 0
                    guard slot >= 0, let t = kl_glfb_eye_mtl_texture(Int32(vi), slot, &slice) else { continue }
                    let tex = arrayView(Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue() as MTLTexture)
                    let tg = vi == 1 ? pl.tangents.1 : pl.tangents.0
                    // The part of the texture that is the picture (Source 2's dynamic resolution), as
                    // fractions from row 0; all zero = all of it.
                    let rr = vi == 1 ? pl.rect.1 : pl.rect.0
                    // The game's own motion vectors for this view, where it sends them.
                    let mvp = vi == 1 ? pl.sw_mv.1 : pl.sw_mv.0
                    let mv = KleptonFrameGen.gameVectors ? mvp.map { Unmanaged<MTLTexture>.fromOpaque($0).takeUnretainedValue() as MTLTexture } : nil
                    let mr = vi == 1 ? pl.sw_mv_rect.1 : pl.sw_mv_rect.0
                    srcs.append(("l\(i).\(vi)", tex, Int(slice), q, SIMD4(tg.0, tg.1, tg.2, tg.3), pl.origin_top_left != 0, vi,
                                 SIMD4(rr.0, rr.1, rr.2, rr.3), mv, Int(vi == 1 ? pl.sw_mv_slice.1 : pl.sw_mv_slice.0),
                                 SIMD4(mr.0, mr.1, mr.2, mr.3), .zero))
                }
            }
        } else if haveRendered {
            let q = simd_quatf(ix: rendered.qx, iy: rendered.qy, iz: rendered.qz, r: rendered.qw)
            for eye in 0..<2 {
                guard let a = eyeSource(eye, stage) else { continue }
                let tg = eye == 1 ? rendered.tangents.1 : rendered.tangents.0
                // The part of the texture the eye's cone covers, as a fraction of the texture the rect was
                // measured against (viewport_of). On the canted display Assassin's Creed Nexus draws each eye
                // over all of a 4800-wide texture, vectors too, but its cone is 3786 of it, the right eye's from
                // 1014 (Mac with KL_OVRP_EYE_TAN=vision, 2026-10-29): the picture and its vectors stay whole,
                // only the head's turn is worked out in that part – over the whole texture it came out a fifth
                // too large and, for the right eye, beside where it belongs.
                let vp = eye == 1 ? rendered.viewport.1 : rendered.viewport.0
                let of = SIMD2(Float(rendered.viewport_of.0), Float(rendered.viewport_of.1))
                let view: SIMD4<Float> = vp.2 > 0 && vp.3 > 0 && of.x > 0 && of.y > 0
                    ? SIMD4(Float(vp.0) / of.x, Float(vp.1) / of.y, Float(vp.0 + vp.2) / of.x, Float(vp.1 + vp.3) / of.y)
                    : .zero
                // An OVRPlugin game with Application SpaceWarp (Assassin's Creed Nexus) draws its motion vectors
                // into the images of the stage it drew the eyes into (ovrp_GetLayerTextureSpaceWarp).
                var mv: MTLTexture? = nil
                var mvSlice = 0
                if KleptonFrameGen.gameVectors {
                    var w: Int32 = 0, h: Int32 = 0, layers: Int32 = 0
                    if let p = kl_vulkan_spacewarp_mtl(0, Int32(stage), &w, &h, &layers) {
                        mv = Unmanaged<MTLTexture>.fromOpaque(p).takeUnretainedValue() as MTLTexture
                        mvSlice = layers > 1 ? eye : 0
                        if eye == 0, let mv { watch = mv }
                        if fgMvWatch.empty { mv = nil }
                    }
                }
                // "e.<eye>", like a layer's "l<i>.<eye>": both eyes in one 2-layer image share one output
                // ("e/a"), since the composite looks the output up by the image. Keyed "e0"/"e1" each eye had
                // its own, the second replaced the first, and the left eye showed a layer nothing wrote –
                // flicker, old pictures, half-black ones (Assassin's Creed Nexus, headset, 2026-10-29).
                srcs.append(("e.\(eye)", a.texture, a.slice, q, SIMD4(tg.0, tg.1, tg.2, tg.3), a.flipY, eye, .zero, mv, mvSlice, .zero, view))
            }
        }
        // Where a layer brings the game's own vectors, the layers without them are left as they are:
        // HITMAN 3 VR: Reloaded's second layer is its interface – menus and prompts that barely move –
        // and searching it at 4036 px took most of a composite that ran 10 ms of a 10 ms display frame,
        // until visionOS halved the display to 50 Hz and nothing was made at all (headset, 2026-10-29).
        if srcs.contains(where: { $0.mv != nil }) {
            let without = srcs.filter { $0.mv == nil }.map(\.key)
            if !without.isEmpty {
                fgSay("only mv", "only the layers with the game's own vectors get pictures; left as they are: " + without.joined(separator: ", "))
                srcs.removeAll { $0.mv == nil }
                for k in without {
                    fgHist[k] = nil; fgPrev[k] = nil; fgPrevOf[k] = nil
                    fgOut[k] = nil; fgOut[String(k.split(separator: ".").first ?? "") + "/a"] = nil
                }
            }
        }
        guard !srcs.isEmpty else { return }

        // A picture drawn through the display's rate map is the map's screen size, but only its
        // physical part – top left – holds the picture (mfxContent). Work on that part, and turn the
        // head with the map.
        var rate: (buffer: MTLBuffer, physical: [SIMD2<Int32>])? = nil
        if let mp = kl_glfb_eye_rate_map(),
           let rm = Unmanaged<AnyObject>.fromOpaque(mp).takeUnretainedValue() as? MTLRasterizationRateMap,
           let first = srcs.first, rm.screenSize.width == first.tex.width, rm.screenSize.height == first.tex.height {
            if fgRateMap.map !== rm {
                let sa = rm.parameterDataSizeAndAlign
                if let b = device.makeBuffer(length: max(16, sa.size), options: .storageModeShared) {
                    rm.copyParameterData(buffer: b, offset: 0)
                    fgRateMap = (rm, b)
                    fgSay("ratemap", "foveated eye: \(rm.screenSize.width)x\(rm.screenSize.height) screen, physical "
                          + (0..<rm.layerCount).map { "\(rm.physicalSize(layer: $0).width)x\(rm.physicalSize(layer: $0).height)" }.joined(separator: ", "))
                }
            }
            if let b = fgRateMap.buffer {
                rate = (b, (0..<rm.layerCount).map {
                    let ps = rm.physicalSize(layer: $0)
                    return SIMD2(Int32(min(first.tex.width, ps.width + 2)), Int32(min(first.tex.height, ps.height + 2)))
                })
            }
        }
        func rect(_ eye: Int, _ tex: MTLTexture, _ sub: SIMD4<Float> = .zero) -> SIMD4<Int32> {
            if let r = rate, !r.physical.isEmpty { let p = r.physical[min(eye, r.physical.count - 1)]; return SIMD4(0, 0, p.x, p.y) }
            let W = Float(tex.width), H = Float(tex.height)
            if sub.z > sub.x, sub.w > sub.y, sub.x >= 0, sub.y >= 0, sub.z <= 1, sub.w <= 1 {
                let x0 = Int32((sub.x * W).rounded()), y0 = Int32((sub.y * H).rounded())
                let x1 = Int32((sub.z * W).rounded()), y1 = Int32((sub.w * H).rounded())
                if x1 - x0 >= 64, y1 - y0 >= 64 { return SIMD4(x0, y0, x1 - x0, y1 - y0) }
            }
            return SIMD4(0, 0, Int32(tex.width), Int32(tex.height))
        }

        func view2D(_ tex: MTLTexture, _ slice: Int) -> MTLTexture? {
            tex.makeTextureView(pixelFormat: Self.fgLinear(tex.pixelFormat), textureType: .type2D,
                                levels: 0..<1, slices: slice..<(slice + 1))
        }

        // Makes the picture `phase` of a game frame after the newest (negative: back towards the older
        // one) for every source that has two comparable pictures, and swaps it in.
        func generateAll(phase: Float) {
            var made = 0
            for s in srcs {
                guard let h = fgHist[s.key], let older = h.older, h.newer.width == s.tex.width, h.newer.height == s.tex.height,
                      h.newer.rect == rect(s.eye, s.tex, s.sub) else { continue }
                let id = ObjectIdentifier(s.tex)
                // Both eyes of a 2-slice array share one output; a texture per eye gets one each.
                // Decided by the texture, not the name: the composite finds the output by it (mfxUpgraded).
                let shared = srcs.contains { $0.tex === s.tex && $0.key != s.key }
                let outKey = shared ? String(s.key.split(separator: ".").first ?? "") + "/a" : s.key
                // The larger of the eyes' drawn parts, for both: they differ by a few pixels in a level
                // (2340 vs 2352), and an output sized per eye was rebuilt every frame – black half-made
                // pictures and a new unwarp grid each time (headset, 2026-10-28). The same rule as mfxContent.
                // Without a rate map the output is the whole texture: a picture in a corner (Source 2's rect)
                // is written where it lies.
                let all = (0..<2).map { rect($0, s.tex) }
                let ow = rate == nil ? s.tex.width : Int(all.map { $0.z }.max() ?? 0)
                let oh = rate == nil ? s.tex.height : Int(all.map { $0.w }.max() ?? 0)
                var out = fgOut[outKey]
                if out == nil || out!.width != ow || out!.height != oh || out!.pixelFormat != s.tex.pixelFormat
                    || out!.arrayLength != s.tex.arrayLength {
                    let d = MTLTextureDescriptor()
                    d.textureType = .type2DArray; d.pixelFormat = s.tex.pixelFormat
                    d.width = ow; d.height = oh; d.arrayLength = s.tex.arrayLength
                    d.usage = [.shaderRead, .shaderWrite, .pixelFormatView]; d.storageMode = .private
                    out = device.makeTexture(descriptor: d)
                    fgOut[outKey] = out
                    if let out { fgSay("out \(outKey) \(ow)x\(oh)", "output \(outKey): \(ow)x\(oh), \(out.arrayLength) slice(s), \(out.allocatedSize >> 20) MiB") }
                }
                guard let out, let src = view2D(s.tex, s.slice), let dst = view2D(out, s.slice) else { continue }
                if s.tex.arrayLength > 1, srcs.filter({ $0.tex === s.tex }).count < s.tex.arrayLength {
                    // A slice nobody here writes would show whatever the copy held: copy the source first.
                    if let blit = cmd.makeBlitCommandEncoder() {
                        for sl in 0..<s.tex.arrayLength {
                            blit.copy(from: s.tex, sourceSlice: sl, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                                      sourceSize: MTLSize(width: ow, height: oh, depth: 1),
                                      to: out, destinationSlice: sl, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
                        }
                        blit.endEncoding()
                    }
                }
                // The turn in the picture's own pixels: the rate map's screen (= the texture) for a foveated
                // eye, else the rect the frustum was drawn into.
                let rr = h.newer.rect
                let rsize = rate != nil ? SIMD2(Float(src.width), Float(src.height)) : SIMD2(Float(rr.z), Float(rr.w))
                let rot = Self.fgTurn(from: h.newer.q, to: older.q, tangents: h.newer.tangents, size: rsize,
                                      topDown: h.newer.topDown, view: h.newer.view)
                let screen = fgScreenSpace && rate != nil
                // The older picture's colours, when they were kept for exactly that picture.
                let oldColor = phase < 0 && fgPrevOf[s.key] == older.serial
                    ? fgPrev[s.key].flatMap { $0.makeTextureView(pixelFormat: Self.fgLinear($0.pixelFormat)) } : nil
                if let field = h.newer.field {
                    // The game's own vectors: in its screen pixels, so a foveated eye warps through the map.
                    // Only the columns the composite shows (the eye's cone, 79 % of Nexus's texture), and 16
                    // beyond for its filter.
                    let v = h.newer.view, tw = Float(rr.z)
                    let shown: SIMD2<Float>? = rate == nil && v.z > v.x ? SIMD2(max(0, v.x * tw - 16), min(tw, v.z * tw + 16)) : nil
                    fg.generate(field: field, cell: h.newer.cell, newerColor: src, phase: phase, warpRect: rr, shownColumns: shown,
                                screenRateMap: rate?.buffer, rateLayer: s.eye, olderColor: oldColor, into: dst, commandBuffer: cmd)
                } else if let po = older.pyramid, let pn = h.newer.pyramid {
                    fg.generate(older: po, newer: pn, newerColor: src, phase: phase,
                                rotation: rot, rateMap: screen ? nil : rate?.buffer, rateLayer: s.eye,
                                screenRateMap: screen ? rate?.buffer : nil, warpRect: rr, olderColor: oldColor,
                                into: dst, commandBuffer: cmd)
                } else { continue }
                // The output holds the source's top-left ow×oh at 1:1: the composite's grid is built against
                // the source, normalised to that part (the same route a MetalFX copy takes).
                let origin = MfxOrigin(source: s.tex, content: SIMD2<Float>(Float(ow), Float(oh)))
                mfxUpgraded[id] = out
                mfxOrigin[ObjectIdentifier(out)] = origin
                if !fgDoneSwap.contains(where: { $0.0 == id }) { fgDoneSwap.append((id, out, origin)) }
                made += 1
            }
            if made > 0 {
                KleptonFrameGen.noteMade()
                fgSay("first", "first generated picture (\(made) eye picture(s))")
            }
        }

        if staleInARow == 0 {
            // A new game picture: keep its pyramid, the one before it becomes "older".
            fgTimes = (fgTimes.newer, fgPresentation)
            if let w = watch, let v = w.textureType == .type2DArray
                ? w.makeTextureView(pixelFormat: w.pixelFormat, textureType: .type2D, levels: 0..<1, slices: 0..<1) : w {
                if fgMvCount.isEmpty {
                    fgMvCount = (0..<4).compactMap { _ in device.makeBuffer(length: 4, options: .storageModeShared) }
                }
                if !fgMvCount.isEmpty {
                    let b = fgMvCount[fgMvCountAt % fgMvCount.count]; fgMvCountAt += 1
                    b.contents().storeBytes(of: UInt32(0), as: UInt32.self)
                    fg.countWritten(v, into: b, commandBuffer: cmd)
                    let watcher = fgMvWatch
                    cmd.addCompletedHandler { _ in watcher.report(b.contents().load(as: UInt32.self)) }
                }
            }
            for s in srcs {
                guard let v = view2D(s.tex, s.slice) else { fgSay("view", "no 2D view of the eye texture (format \(s.tex.pixelFormat.rawValue)) – off"); continue }
                let parity = (fgParity[s.key] ?? 0) ^ 1
                fgParity[s.key] = parity
                let r = rect(s.eye, v, s.sub)
                let prev = fgHist[s.key]?.newer
                fgSerial += 1
                // The game's own vectors, where it sent them: the field straight from them, no pyramid.
                // They are of the picture as the game saw it – a foveated eye's screen, else its rect.
                var field: MTLTexture? = nil
                var cell = SIMD2<Float>(0, 0)
                if let mvt = s.mv,
                   let mv2 = mvt.textureType == .type2DArray
                       ? mvt.makeTextureView(pixelFormat: mvt.pixelFormat, textureType: .type2D, levels: 0..<1,
                                             slices: s.mvSlice..<(s.mvSlice + 1)) : mvt {
                    let pw = Float(rate != nil ? v.width : Int(r.z)), ph = Float(rate != nil ? v.height : Int(r.w))
                    let W = Float(mv2.width), H = Float(mv2.height), m = s.mvSub
                    let mr: SIMD4<Int32> = m.z > m.x && m.w > m.y
                        ? SIMD4(Int32((m.x * W).rounded()), Int32((m.y * H).rounded()),
                                Int32(((m.z - m.x) * W).rounded()), Int32(((m.w - m.y) * H).rounded()))
                        : SIMD4(0, 0, Int32(W), Int32(H))
                    // Across and down apart: 960x771 vectors for Nexus's 4800x3081 eye are 5 by 4 pixels, and
                    // one size for both took the lower part's vectors from ever higher up (headset, 2026-10-29).
                    cell = SIMD2(pw / Float(max(1, mr.z)), ph / Float(max(1, mr.w)))
                    let rot = KleptonFrameGen.gameVectorsHaveHead
                        ? prev.map { Self.fgTurn(from: s.q, to: $0.q, tangents: s.tangents, size: SIMD2(pw, ph),
                                                 topDown: s.topDown, view: s.view) } : nil
                    // The scale is for a picture with row 0 at the top; one GL drew has it at the bottom.
                    let scale = KleptonFrameGen.gameVectorScale * SIMD2(1, s.topDown ? 1 : -1)
                    field = fg.motionField(from: mv2, mvRect: mr, toPixels: scale * SIMD2(pw, ph),
                                           cell: cell, rotation: rot, key: s.key, commandBuffer: cmd)
                    if field != nil {
                        fgSay("mv \(s.key)", "\(s.key): the game's own motion vectors (\(mr.z)x\(mr.w) at \(mr.x),\(mr.y), \(cell.x)x\(cell.y) picture pixels each) instead of a search")
                    }
                }
                // A foveated eye's pyramid is of its screen, read through the map (fgScreenSpace).
                let screen = fgScreenSpace && rate != nil
                let pyr = field != nil ? nil
                    : fg.pyramid(of: v, rect: screen ? SIMD4(0, 0, Int32(v.width), Int32(v.height)) : r,
                                 key: "\(s.key).\(parity)", rateMap: screen ? rate?.buffer : nil, rateLayer: s.eye,
                                 commandBuffer: cmd)
                let side = FgSide(pyramid: pyr, field: field, cell: cell, q: s.q, tangents: s.tangents, view: s.view, topDown: s.topDown,
                                  width: v.width, height: v.height, rect: r, serial: fgSerial)
                // A picture of another size (a resolution step) cannot be compared: no older one then.
                fgHist[s.key] = (prev.flatMap { $0.width == side.width && $0.height == side.height && $0.rect == side.rect ? $0 : nil }, side)
            }
            // Interpolation ("between"): this drawable shows the middle between the two newest pictures,
            // and the newest itself follows on the next one – real in-betweens, as DLSS makes them, one
            // display frame later (10 ms at 100 Hz). Only where the game takes two display frames or
            // more a picture: at the display's rate (visionOS halved it to the game's 50) every drawable
            // has a new picture, the newest would never be shown and every one would be a middle – 50
            // made a second and none of them more than the game drew (headset, 2026-10-28).
            if KleptonFrameGen.interp {
                let span = fgTimes.newer - fgTimes.older
                if fgTimes.older > 0, span > 0 {
                    let s = min(span, 0.060)
                    fgSpanAvg = fgSpanAvg == 0 ? s : fgSpanAvg * 0.9 + s * 0.1
                }
                // At one late picture of a game as fast as the display, a middle held the newest back a
                // display frame, the next one came on time and the held one was never shown: an older
                // picture between the new ones. visionOS ran Assassin's Creed Nexus at 50 Hz for a whole
                // session, 11 % of its pictures late, and 2030 such middles were made (headset,
                // 2026-10-29); with the smoothed span 2 in a simulation of that, the 100 Hz cases unchanged.
                if fgPeriod > 0, fgTimes.older > 0, span > fgPeriod * 1.5, span < 0.060, fgSpanAvg > fgPeriod * 1.5 {
                    generateAll(phase: -0.5)
                    // ...and this picture's colours kept for the next one, after the pass above has read the last.
                    if let blit = cmd.makeBlitCommandEncoder() {
                        for s in srcs {
                            guard let newest = fgHist[s.key]?.newer else { continue }
                            let all = (0..<2).map { rect($0, s.tex) }
                            let ow = rate == nil ? s.tex.width : Int(all.map { $0.z }.max() ?? 0)
                            let oh = rate == nil ? s.tex.height : Int(all.map { $0.w }.max() ?? 0)
                            var prev = fgPrev[s.key]
                            if prev == nil || prev!.width != ow || prev!.height != oh || prev!.pixelFormat != s.tex.pixelFormat {
                                let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: s.tex.pixelFormat, width: ow, height: oh,
                                                                                 mipmapped: false)
                                d.usage = [.shaderRead, .pixelFormatView]; d.storageMode = .private
                                prev = device.makeTexture(descriptor: d)
                                fgPrev[s.key] = prev
                                if let prev { fgSay("prev \(s.key) \(ow)x\(oh)", "older colours \(s.key): \(ow)x\(oh), \(prev.allocatedSize >> 20) MiB") }
                            }
                            guard let prev else { continue }
                            blit.copy(from: s.tex, sourceSlice: s.slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                                      sourceSize: MTLSize(width: ow, height: oh, depth: 1),
                                      to: prev, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
                            fgPrevOf[s.key] = newest.serial
                        }
                        blit.endEncoding()
                    }
                } else {
                    fgSay("same rate", "the game keeps up with the display – nothing to put between its pictures")
                }
            }
            return
        }
        // With interpolation the repeat IS the newest picture, shown a display frame late.
        guard KleptonFrameGen.mode == .ahead else { return }
        // Where this drawable lies between the two newest game pictures, measured on the display's own
        // clock. Nothing to make when the game stalled (a gap over 60 ms is a hitch, not a rate) or
        // when the times are not known yet.
        let span = fgTimes.newer - fgTimes.older
        guard fgTimes.older > 0, span > 0.004, span < 0.060 else { return }
        let phase = Float(min(1.0, max(0.0, (fgPresentation - fgTimes.newer) / span)))
        guard phase > 0.05 else { return }
        generateAll(phase: phase)
    }

    private func mfxPrepass(stage: Int, on cmd: any MTLCommandBuffer) {
        mfxUpgraded = [:]
        mfxOrigin = [:]
        mfxTick &+= 1
        if mfxTick % 120 == 0, !mfxTargets.isEmpty {
            let before = mfxTargets.count
            mfxTargets = mfxTargets.filter { mfxTick &- $0.value.lastUse < Self.mfxKeepTicks }
            if mfxTargets.count != before {
                NSLog("[metalfx] released \(before - mfxTargets.count) unused upscale target(s)")
            }
        }
        let on = KleptonMetalFX.isOn
        if on != mfxLastOn {
            mfxLastOn = on
            NSLog(on ? "[metalfx] on — next frames upscale to the full eye size"
                       + (UserDefaults.standard.object(forKey: KleptonMetalFX.key) == nil
                          ? " (on from the installation, \(KleptonMetalFX.installSwitch))" : "")
                     : "[metalfx] off — linear path")
            if !on {
                // Off is off: the copies are hundreds of MiB at a large eye.
                mfxTargets = [:]
                mfxCache = nil
                mfxLastShape = ""
            }
        }
        // While the guest shows the room (KleptonMetalFXPause) the scaler's
        // output is opaque everywhere and would black it out: the source's
        // alpha goes back into each copy after it (KleptonMetalFXAlpha), or with
        // KL_MFX_MR_PAUSE=1 the linear path holds until passthrough ends. The
        // stored switch stays as it is either way.
        let mr = mfxPause.frame(chosen: on, mixedReality: kl_ovrp_passthrough_live() != 0)
        if let change = mr.change {
            switch change {
            case .paused:
                NSLog("[metalfx] paused while the game shows the room (passthrough, KL_MFX_MR_PAUSE): "
                      + "the scaler writes alpha 1, which would cover the room — linear path")
            case .resumed: NSLog("[metalfx] passthrough ended — upscaling again")
            case .alphaOn:
                NSLog("[metalfx] the game shows the room (passthrough): upscaling with the "
                      + "picture's alpha put back after the scaler (MR alpha)")
            case .alphaOff: NSLog("[metalfx] passthrough ended — upscaling without the alpha pass")
            }
            // A copy made for the other way must not be reused.
            mfxCache = nil
        }
        mfxRestoreAlpha = mr.restoreAlpha
        mfxStats = MfxStats(state: on ? 1 : 0)
        guard on else { return }
        guard mr.upscale else { mfxStats.note = "paused: MR"; return }
        guard mfxSupported else {
            mfxSay("MetalFX spatial scaling is not supported on this device", short: "unsupported")
            return
        }
        guard kl_ovrp_eye_layer_live() != 0 else { mfxStats.note = "no picture"; return }
        // The scale the guest's eye size was made with — not the stored
        // setting, which the slider in the game changes at once while the
        // guest keeps the size it was started with. An OpenXR guest is told the
        // same size (klxr_EnumerateViewConfigurationViews), so this holds for
        // its layers as for the eye.
        var rs: Float = 1
        var fullW: Int32 = 0, fullH: Int32 = 0
        kl_ovrp_eye_texture_scale(&rs, &fullW, &fullH)
        guard rs < 0.999, fullW > 0, fullH > 0 else {
            mfxSay("the game was started at 100 % resolution", short: "100 %")
            return
        }

        // The frame's sources in groups that go together: the eye pass is one
        // group (its grid is built from slot 0 for both eyes), each projection
        // layer another. Within a group, the distinct textures in eye order and
        // the slices read of each: one for a shared 2-slice array, two for a
        // texture per eye. A texture seen in an earlier group is not planned
        // twice — the copy is keyed by the texture, not by who reads it.
        typealias Source = (tex: MTLTexture, slices: Set<Int>)
        var groups: [[Source]] = []
        var seen: [ObjectIdentifier: (g: Int, i: Int)] = [:]
        func add(_ tex: MTLTexture, slice: Int, to g: inout [Source], group: Int) {
            if let at = seen[ObjectIdentifier(tex)] {
                if at.g == group { g[at.i].slices.insert(slice) }
                else { groups[at.g][at.i].slices.insert(slice) }
                return
            }
            seen[ObjectIdentifier(tex)] = (group, g.count)
            g.append((tex, [slice]))
        }
        let layered = projLayerCount > 0
        if layered {
            for i in 0..<min(projLayerCount, projLayers.count) {
                let pl = projLayers[i]
                var g: [Source] = []
                for vi in 0..<2 {
                    let slot = vi == 1 ? pl.slot.1 : pl.slot.0
                    var slice: Int32 = 0
                    guard slot >= 0, let t = kl_glfb_eye_mtl_texture(Int32(vi), slot, &slice) else { continue }
                    // The same array view encodeProjLayers binds, so the swap finds it.
                    let tex = arrayView(Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue() as MTLTexture)
                    add(tex, slice: Int(slice), to: &g, group: groups.count)
                }
                if !g.isEmpty { groups.append(g) }
            }
        } else {
            var g: [Source] = []
            for eye in 0..<2 {
                if let a = eyeSource(eye, stage) { add(a.texture, slice: a.slice, to: &g, group: 0) }
            }
            if !g.isEmpty { groups.append(g) }
        }
        guard !groups.isEmpty else { mfxStats.note = "no picture"; return }
        let all = groups.flatMap { $0.map { $0.tex } }

        // Same guest frame as the last prepass, looking at the same textures:
        // they still hold the pictures the copies were made from. The same
        // "same" the metrics panel uses for fresh vs repeated pictures (see the
        // drawable loop).
        let fence = kl_glfb_gpu_fence_value()
        let frame = fence != 0 ? fence : kl_vulkan_frame_serial()
        if frame != 0, let c = mfxCache, c.frame == frame, c.sources.count == all.count,
           zip(c.sources, all).allSatisfy({ $0 === $1 }) {
            mfxUpgraded = c.upgraded
            mfxOrigin = c.origin
            mfxStats = c.stats
            for k in c.keys { mfxTargets[k]?.lastUse = mfxTick }
            return
        }
        mfxCache = nil

        let fullArea = Double(fullW) * Double(fullH)
        var budget = layered ? fullArea * Double(Self.mfxLayerBudget) : .infinity
        var keys: [String] = []
        var shapes: [String] = []
        for (gi, sources) in groups.enumerated() {
            let name = layered ? "layer \(gi)" : "eye"
            if !layered, sources.count > 1 {
                let s0 = sources[0].tex
                guard sources.dropFirst().allSatisfy({ $0.tex.width == s0.width && $0.tex.height == s0.height }) else {
                    mfxSay("the eyes' textures differ in size", short: "eye sizes"); continue
                }
            }
            var plans: [MfxPlan] = []
            for (slot, s) in sources.enumerated() {
                guard let p = mfxPlan(s.tex, slices: s.slices,
                                      key: layered ? "l\(gi).\(slot)" : "e\(slot)", rs: rs,
                                      fullW: Int(fullW), fullH: Int(fullH)) else { break }
                plans.append(p)
            }
            guard plans.count == sources.count else { continue }
            // The layers' budget: a layer whose copies would take the frame past
            // it stays linear, and the ones after it may still fit.
            let cost = plans.reduce(0.0) { $0 + Double($1.out.w) * Double($1.out.h) * Double($1.slices.count) }
            if cost > budget {
                mfxSay("\(name): \(Int(cost / 1e6)) MP of copies is past the budget of "
                       + "\(Self.mfxLayerBudget) full eyes a frame (KL_METALFX_BUDGET)", short: "budget")
                continue
            }
            // Targets and inputs before anything is encoded, for the all-or-nothing.
            var inputs: [[MTLTexture]] = []
            var ok = true
            for p in plans {
                if mfxTargets[p.key] == nil {
                    guard let t = mfxMakeTarget(p.source, width: p.out.w, height: p.out.h) else { ok = false; break }
                    mfxTargets[p.key] = t
                }
                var row: [MTLTexture] = []
                for s in p.slices {
                    guard let v = p.input(s) else {
                        mfxSay("no input view of slice \(s)", short: "texture view"); ok = false; break
                    }
                    row.append(v)
                }
                if !ok { break }
                inputs.append(row)
            }
            guard ok else { continue }
            if mfxRestoreAlpha, let bad = plans.first(where: { mfxAlpha.pipeline(for: $0.source.pixelFormat) == nil }) {
                mfxSay("no alpha pass for format \(bad.source.pixelFormat.rawValue) in mixed reality",
                       short: "MR alpha")
                continue
            }
            budget -= cost
            for (p, row) in zip(plans, inputs) {
                guard var t = mfxTargets[p.key] else { continue }
                for (s, input) in zip(p.slices, row) where s < t.outViews.count {
                    t.scaler.colorTexture = input
                    t.scaler.inputContentWidth = p.content.w
                    t.scaler.inputContentHeight = p.content.h
                    t.scaler.outputTexture = t.outViews[s]
                    t.scaler.encode(commandBuffer: cmd)
                    if mfxRestoreAlpha {
                        mfxAlpha.encode(on: cmd, from: input, content: p.content, into: t.outViews[s])
                    }
                }
                t.lastUse = mfxTick
                mfxTargets[p.key] = t
                keys.append(p.key)
                mfxUpgraded[ObjectIdentifier(p.source)] = t.texture
                mfxOrigin[ObjectIdentifier(t.texture)] =
                    MfxOrigin(source: p.source, content: SIMD2<Float>(Float(p.content.w), Float(p.content.h)))
                if mfxStats.state != 2 {
                    // The first picture upscaled is the one the panel names: the
                    // eye, or the backdrop layer. In the picture's own pixels —
                    // for a foveated one the screen size the copy stands for.
                    mfxStats = MfxStats(state: 2, note: mfxRestoreAlpha ? "MR alpha" : "",
                                        outW: Int((Double(p.out.w) * Double(p.source.width) / Double(p.content.w)).rounded()),
                                        outH: Int((Double(p.out.h) * Double(p.source.height) / Double(p.content.h)).rounded()))
                }
                shapes.append("\(name) \(p.content.w)x\(p.content.h)"
                              + (p.content.w != p.source.width || p.content.h != p.source.height
                                 ? " of \(p.source.width)x\(p.source.height)" : "")
                              + "->\(p.out.w)x\(p.out.h)")
            }
        }
        let shape = shapes.joined(separator: ", ")
        if !shape.isEmpty, shape != mfxLastShape {
            mfxLastShape = shape
            NSLog("[metalfx] upscaling \(shape) (render scale \(rs), full eye \(fullW)x\(fullH))")
        }
        if frame != 0 { mfxCache = (frame, all, mfxUpgraded, mfxOrigin, keys, mfxStats) }
    }

    /// The panel's picture rows, said to kl_metrics when they change: the size
    /// the game draws (the eye, or the first projection layer — the backdrop),
    /// the way it comes, and the upscale.
    private var statsPicture = (w: 0, h: 0, path: "", mfx: MfxStats())
    private func noteStatsPicture(stage: Int) {
        guard kl_ovrp_eye_layer_live() != 0 else { return }
        let gfx = kl_vulkan_guest_active() != 0 ? "Vulkan" : "GL"
        var tex: MTLTexture? = nil
        var path = ""
        if projLayerCount > 0 {
            for i in 0..<min(projLayerCount, projLayers.count) where tex == nil {
                let pl = projLayers[i]
                var slice: Int32 = 0
                if pl.slot.0 >= 0, let t = kl_glfb_eye_mtl_texture(0, pl.slot.0, &slice) {
                    tex = Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue() as MTLTexture
                }
            }
            path = "OpenXR \(gfx) · \(projLayerCount) layer\(projLayerCount == 1 ? "" : "s")"
        } else if let a0 = eyeSource(0, stage) {
            tex = a0.texture
            let a1 = eyeSource(1, stage)
            path = "\(gfx) · " + (a1.map { $0.texture === a0.texture } ?? true ? "array" : "per eye")
        }
        guard let tex else { return }
        if mfxContent(tex) != SIMD2<Float>(Float(tex.width), Float(tex.height)) { path += " · foveated" }
        guard tex.width != statsPicture.w || tex.height != statsPicture.h
              || path != statsPicture.path || mfxStats != statsPicture.mfx else { return }
        statsPicture = (tex.width, tex.height, path, mfxStats)
        kl_metrics_note_eye(Int32(tex.width), Int32(tex.height))
        kl_metrics_note_upscale(mfxStats.state, Int32(mfxStats.outW), Int32(mfxStats.outH),
                                path, mfxStats.note)
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
    private func encodeComposite(_ drawable: KLDrawable,
                                 cmd: MTLCommandBuffer, stage: Int, completeStage: Int,
                                 rendered: kl_ovrp_render_pose, haveRendered: Bool,
                                 originFromHead: simd_float4x4) -> Int {
        // MetalFX prepass: small pictures up to the full eye size before sampling.
        mfxPrepass(stage: stage, on: cmd)
        // Frame generation: a display frame without a new game picture gets one made from the last two
        // (KleptonFrameGen.swift). Only where MetalFX swapped nothing – the two would fight over the swap.
        if mfxUpgraded.isEmpty { fgPrepass(stage: stage, on: cmd, rendered: rendered, haveRendered: haveRendered) }
        noteStatsPicture(stage: stage)
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
        // The RealityKit stand-in has one texture per eye: a pass each.
        let layered = (layerRenderer?.configuration.layout
                       ?? (drawable.colorTextures.count == 1 ? .layered : .dedicated)) == .layered
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
                             drawable: KLDrawable,
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
        // ...and to nothing at all while the guest is in its own mixed reality
        // (kl_ovrp_passthrough_live): the eye pass then discards every pixel
        // that holds nothing of the game's, and what a discarded pixel keeps
        // is this clear. The system shows the room by itself only where a
        // whole block of the drawable has no depth; inside a block that also
        // holds a piece of the game it composites the colour, and opaque
        // black there was a black fringe out to the block's edge all around
        // the game's world ("am Rand der 3D Welt gibt es überall schwarze
        // Ränder", 2026-10-12).
        pass.colorAttachments[0].clearColor  = probe == 1
            ? Self.cyclingClear()
            : MTLClearColorMake(0, 0, 0, kl_ovrp_passthrough_live() != 0 ? 0 : 1)
        if firstMap.textureIndex < drawable.depthTextures.count {
            pass.depthAttachment.texture     = drawable.depthTextures[firstMap.textureIndex]
            pass.depthAttachment.loadAction  = .clear
            // .store, NOT .dontCare. visionOS reprojects the submitted frame
            // using its depth buffer, so throwing the depth away hands the
            // compositor a colour image with no geometry — and a reverse-Z
            // clear of 0 reads as "everything is infinitely far away".
            // .dontCare here meant the quad's colour was correct and its
            // depth said nothing was there.
            pass.depthAttachment.storeAction = drawable.system == nil ? .dontCare : .store
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

        // Loading skybox. While an Equirect loading layer is live the guest's
        // eye is black - AC Nexus feeds that layer an Android MediaPlayer video
        // surface that does not exist on visionOS - so rather than reproject a
        // black eye, play the game's own loading mp4 and draw it as the 360
        // environment. When the guest finishes loading it destroys the layer,
        // this tears the player down, and the eye path below resumes.
        if kl_loadingvideo_active() == 0 {
            if loadingVideo != nil {
                loadingVideo?.stop(); loadingVideo = nil; loadingVideoTried = false
            }
        } else if let tex = loadingVideoTexture() {
            drawLoadingSkybox(enc, viewIndices: viewIndices, drawable: drawable,
                              layered: layered, originFromHead: originFromHead,
                              texture: tex, sampler: sampler)
            // The menu is the video backdrop PLUS the guest's UI quad overlays on
            // top (once it can allocate its layers). Draw them over the skybox so
            // the menu shows through instead of only the loading environment.
            _ = encodeOverlays(enc, viewIndices: viewIndices, drawable: drawable,
                               originFromHead: originFromHead, writeDepth: true,
                               backdrop: false)
            enc.endEncoding()
            return viewIndices.count
        }

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
            var a = eyeSource(vi, stage)
            if let au = a, let up = mfxUpgraded[ObjectIdentifier(au.texture)] {
                a = EyeAllocation(texture: up, slice: au.slice, flipY: au.flipY)
            }
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
            //
            // MetalFX: the bound texture may be the upscaled copy. The grid is
            // then built against the raw texture the guest's rects and rate map
            // describe, normalised to the part of it the copy holds — the same
            // texture coordinates are the same scene point in the copy.
            let gridOrigin = mfxOrigin[ObjectIdentifier(bind0)]
            let (gridBuf, gridVerts, gridPerEye) =
                unwarpGrid(gridOrigin?.source ?? bind0, viewport0: vp0, viewport1: vp1, viewportOf: vpOf,
                           topLeft: uniforms.first(where: { $0.visible != 0 })?.flip_y ?? 0 != 0,
                           uvSize: gridOrigin?.content)
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
            var depthTextures: [MTLTexture] = []
            var depthParams: [SIMD4<Float>] = []
            if haveRendered && probePipeline == nil && guestDepthPipeline != nil {
                for vi in viewIndices {
                    let d = vi == 0 ? rendered.depth.0 : rendered.depth.1
                    var w: Int32 = 0, h: Int32 = 0
                    if d.layer_plus_one > 0,
                       let raw = kl_glfb_layer_mtl_texture(d.layer_plus_one - 1, d.stage, &w, &h) {
                        let t = arrayView(Unmanaged<MTLTexture>.fromOpaque(raw).takeUnretainedValue())
                        if t.textureType == .type2DArray && d.slice >= 0 && Int(d.slice) < t.arrayLength {
                            depthTextures.append(t)
                            depthParams.append(SIMD4(d.near_z,d.far_z,d.min_depth,d.max_depth))
                            depthParams.append(SIMD4(Float(d.slice),0,0,0))
                        }
                    }
                }
            }
            let useGuestDepth = depthTextures.count == viewIndices.count && !depthTextures.isEmpty
            enc.setRenderPipelineState(useGuestDepth ? guestDepthPipeline! : (probePipeline ?? pipeline))
            if useGuestDepth {
                enc.setFragmentTexture(depthTextures[0], index: 2)
                enc.setFragmentTexture(depthTextures.count > 1 ? depthTextures[1] : depthTextures[0], index: 3)
                depthParams.withUnsafeBytes { enc.setFragmentBytes($0.baseAddress!,length:$0.count,index:2) }
                uniforms.withUnsafeBytes { enc.setFragmentBytes($0.baseAddress!,length:$0.count,index:0) }
                if !loggedGuestDepth { loggedGuestDepth=true; NSLog("[cp] guest per-pixel depth active for real-hand occlusion") }
            }
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
                                  drawable: KLDrawable,
                                  originFromHead: simd_float4x4,
                                  count: Int) -> Int {
        guard let pipeline, let sampler else { return 0 }
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
                    // MetalFX: this frame's upscaled copy of it, where there is
                    // one (mfxPrepass). The layer's rect is a fraction of the
                    // texture, so it holds for the copy unchanged.
                    if let raw = tex, let up = mfxUpgraded[ObjectIdentifier(raw)] { tex = up }
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
            // ...unless the layer's picture was rasterized through a rate map.
            // A mirrored guest's eyes can be (kl_glfb.c, klfb_mirror_vrr): the
            // guest then draws into the PHYSICAL part of its swapchain image, the
            // copy brings that across texel for texel, and this pass has to undo
            // the squeeze exactly as the eye pass does — or the picture is a
            // small warped rectangle in one corner of a black view (on device,
            // 2026-10-08). The map says which textures: only one of its own
            // screen size was drawn through it. A layer that is also cropped is
            // not handled here; none of the mirrored guests does both.
            // Asked of the raw texture: an upscaled copy is not the map's size,
            // and holds only the physical part (MfxOrigin.content).
            let origin0 = mfxOrigin[ObjectIdentifier(bind0)]
            let raw0 = origin0?.source ?? bind0
            var foveated = false
            if let mp = kl_glfb_eye_rate_map(),
               let rm = Unmanaged<AnyObject>.fromOpaque(mp).takeUnretainedValue()
                   as? MTLRasterizationRateMap {
                foveated = rm.screenSize.width == raw0.width
                    && rm.screenSize.height == raw0.height
            }
            // The unwarp grid is ONE block shared by both eyes unless the guest's
            // map has a curve per eye (unwarpGrid's third answer), where this
            // pass's uniforms were built for the layer grid's block per eye: left
            // as it was against a shared grid, eye 1 indexes past its end and
            // draws nothing. Against a grid that DOES carry a block per eye the
            // uniforms are already right, and forcing both eyes onto block 0
            // unwarps the right eye through the left eye's curve — a grossly
            // distorted right eye beside a correct left one (on device,
            // 2026-10-12, the first build with a curve per eye).
            var unwarp: (MTLBuffer?, Int, Bool)? = nil
            if foveated {
                unwarp = unwarpGrid(raw0, viewport0: .zero, viewport1: .zero, viewportOf: .zero,
                                    topLeft: false, uvSize: origin0?.content)
                if unwarp?.2 != true {
                    for n in uniforms.indices { uniforms[n].grid_eye = 0 }
                }
            }
            // A layer over others that asks to be blended by its alpha is: its
            // empty pixels let what is under it show (HITMAN 3 VR: Reloaded's
            // interface layer over its world). The first layer drawn is the
            // backdrop and stays opaque, as before.
            var layerPipeline: MTLRenderPipelineState = pipeline
            if drawn > 0, (pl.xr_flags & 0x2) != 0,
               let bp = (pl.xr_flags & 0x4) != 0 ? projBlendUnpremulPipeline : projBlendPipeline {
                layerPipeline = bp
                for n in uniforms.indices { uniforms[n].eye_alpha = 1 }
                if !loggedProjBlend.contains(i) {
                    loggedProjBlend.insert(i)
                    NSLog("[cp] projection layer \(i) is blended over the ones before it by its alpha "
                          + "(flags 0x\(String(pl.xr_flags, radix: 16)))")
                }
            }
            enc.setRenderPipelineState(layerPipeline)
            enc.setFragmentTexture(bind0, index: 0)
            enc.setFragmentTexture(bind1, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            // The layer's own grid, not the identity: the picture may be only
            // part of the texture (kl_ovrp_proj_layer.rect — Source 2's dynamic
            // resolution), and each eye's part is a block of its own.
            var layerGrid = [SIMD2<Float>](repeating: .zero,
                                           count: Int(KL_PROJ_LAYER_GRID_ENTRIES))
            withUnsafePointer(to: pl) { p in
                layerGrid.withUnsafeMutableBufferPointer { kl_proj_layer_grid(p, $0.baseAddress) }
            }
            if foveated, let (gridBuf, gridVerts, _) = unwarp {
                // Said once per size: what the grid actually holds, because "the
                // unwarp ran" and "the unwarp did something" are different claims.
                if let gridBuf, loggedLayerUnwarp != bind0.width {
                    loggedLayerUnwarp = bind0.width
                    let g = gridBuf.contents().bindMemory(to: SIMD2<Float>.self,
                                                          capacity: gridBuf.length / 8)
                    let nx = Int(g[0].x), ny = Int(g[0].y)
                    func at(_ x: Int, _ y: Int) -> SIMD2<Float> { g[1 + y * (nx + 1) + x] }
                    NSLog("%@", "[cp] layer unwarp: grid \(nx)x\(ny), \(gridVerts) vertices, "
                          + "uv(0,0)=\(at(0, 0)) uv(mid)=\(at(nx / 2, ny / 2)) "
                          + "uv(max)=\(at(nx, ny)); texture \(bind0.width)x\(bind0.height) "
                          + "slices \(bind0.arrayLength); grid_eye "
                          + "\(uniforms.map { $0.grid_eye }), visible \(uniforms.map { $0.visible })")
                }
                enc.setVertexBuffer(gridBuf, offset: 0, index: 1)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: gridVerts)
            } else {
                layerGrid.withUnsafeBytes { buf in
                    enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 1)
                }
                enc.drawPrimitives(type: .triangle, vertexStart: 0,
                                   vertexCount: Int(kl_reproject_grid_vertices(1, 1)))
            }
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
    nonisolated(unsafe) static var headLockTraceN = 0
    private var identityGridBuffer: MTLBuffer?
    private var loggedLayerUnwarp = -1
    private var loggedProjMiss = Set<Int>()
    private var loggedProjBlend = Set<Int>()
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
                                drawable: KLDrawable,
                                originFromHead: simd_float4x4,
                                writeDepth: Bool,
                                backdrop: Bool = true) -> Int {
        guard overlaysEnabled, let pipe = overlayPipeline else { return 0 }
        // .greater against that same clear, and the shader's own projected
        // position carries the quad's real distance, so what lands in the
        // buffer is where the panel actually is.
        if writeDepth, let depthState { enc.setDepthStencilState(depthState) }
        // ...unless the guest is in its own mixed reality (a passthrough layer in
        // its frames, kl_ovrp_passthrough_live): there the room is the backdrop.
        if writeDepth, kl_ovrp_passthrough_live() == 0,
           kl_ovrp_eyeless_black() != 0 || (backdrop && mixedBlack) {
            encodeBlackBackdrop(enc, pipe: pipe, viewIndices: viewIndices, drawable: drawable,
                                originFromHead: originFromHead)
        }
        let n = Int(kl_ovrp_overlay_count())
        var drawn = 0
        var drewEquirect = false
        for i in 0..<n {
            var ov = kl_ovrp_overlay()
            guard kl_ovrp_overlay_get(Int32(i), &ov) != 0 else { continue }
            // Equirect layers are the loading/menu backdrop, which the host
            // draws itself from the real mp4 (drawLoadingSkybox). The guest's
            // own equirect layers here are 2x2 placeholders or the wild video
            // handle - drawing them as a full skybox stretches 4 pixels over the
            // sphere and flickers over everything. Skip them; the backdrop is
            // already handled.
            if ov.shape == 5 {
                // The guest's own equirect layer — the menu ENVIRONMENT backdrop.
                // Skipped while these were 2x2 placeholders; now that the menu
                // renders for real, the 512x512+ ones carry the backdrop the user
                // sees as "missing background". Draw the first per pass whose texL
                // resolves, through the same skybox pipeline the loading video
                // uses (behind everything at far depth). KL_OVERLAY_EQ=0 restores
                // the old skip. The quad restore below puts the pipeline and depth
                // state back for the panels that follow.
                // ov.tex != 0: an equirect whose texL is NULL (layer 4, the video
                // placeholder) must NOT fall back to its empty swapchain image —
                // that drew a BLACK sphere over the far plane. And the depth
                // state is restored UNCONDITIONALLY: encodeEquirect swaps in the
                // greaterEqual-at-far compare, and leaking it into the quad
                // draws below made every panel fail the depth test — composited
                // in the log, invisible in the headset (the post-equirect
                // regression).
                if Self.drawEquirectOverlays, !drewEquirect, ov.tex_w >= 64, ov.tex != 0,
                   encodeEquirect(enc, ov: ov, viewIndices: viewIndices,
                                  drawable: drawable, originFromHead: originFromHead) {
                    drewEquirect = true
                }
                if drewEquirect, let depthState { enc.setDepthStencilState(depthState) }
                continue
            }
            // KL_OVERLAY_SKIP_HEADLOCKED=1: drop head-locked overlays. In AC Nexus
            // the interactive menu is entirely WORLD-locked (buttons/panels placed
            // in the scene); the only head-locked quads are the fullscreen loading
            // /fade cover (e.g. layer 3 1024x1024 and layer 5 1.35m, both glued
            // 0.5 m in front of the eyes) which the game leaves up in its init loop
            // and which draws over the menu behind it. Skipping them reveals the
            // menu; off by default so no other guest's head-locked HUD regresses.
            if Self.skipHeadLockedOverlays && ov.head_locked != 0 {
                if !loggedOverlayMiss.contains(ov.layer_id) {
                    loggedOverlayMiss.insert(ov.layer_id)
                    NSLog("[cp] overlay layer \(ov.layer_id) (shape \(ov.shape), "
                          + "\(ov.size.0)x\(ov.size.1) m) SKIPPED — head-locked "
                          + "(KL_OVERLAY_SKIP_HEADLOCKED)")
                }
                continue
            }
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
                // The guest-submitted texL first: AC Nexus draws its menu UI
                // into its own textures and submits those, not into the swapchain
                // image we handed out — so that handle, resolved to its MTLTexture,
                // is where the panel pixels actually are. Falls back to the layer
                // image (RE4's path) then kl_glfb when texL carries nothing.
                let t = kl_vulkan_mtl_for_handle(ov.tex)
                    ?? kl_vulkan_layer_mtl_texture(ov.layer_id, ov.stage, Int32(vi), &w, &h)
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
            // The equirect branch above may have swapped the depth state for
            // its behind-everything compare; restore the quad's before drawing.
            if writeDepth, let depthState { enc.setDepthStencilState(depthState) }
            // ...or, where asked for, the state that lets each quad land over
            // the ones before it whatever their distances.
            if Self.overlaysInSubmitOrder, let overlayOrderState {
                enc.setDepthStencilState(overlayOrderState)
            }
            enc.setRenderPipelineState(pipe)
            enc.setFragmentTexture(texture, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            uniforms.withUnsafeBytes { buf in
                enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
                enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
            }
            // Four for a quad; a cylinder's strip runs along its arc.
            let stripVertices = uniforms.first(where: { $0.visible != 0 })
                .map { u in var v = u; return Int(kl_overlay_strip_vertices(&v)) } ?? 4
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: stripVertices)
            drawn += 1
            if !loggedOverlayDraw.contains(ov.layer_id) {
                loggedOverlayDraw.insert(ov.layer_id)
                NSLog("[cp] compositing overlay layer \(ov.layer_id): "
                      + "\(ov.size.0)x\(ov.size.1) m at (\(ov.pose.4), \(ov.pose.5), "
                      + "\(ov.pose.6))"
                      + (ov.head_locked != 0 ? ", HEAD-LOCKED" : ""))
            }
        }
        // The play area's edge (KleptonBoundary.swift), over the game and its panels, under our own cards.
        encodeBoundary(enc, viewIndices: viewIndices, drawable: drawable, originFromHead: originFromHead)
        for hand: Int32 in [0, 1] {
            drawn += encodeHandGuide(enc, viewIndices: viewIndices, drawable: drawable, originFromHead: originFromHead, hand: hand)
        }
        drawn += encodeLoadingCard(enc, viewIndices: viewIndices, drawable: drawable,
                                   originFromHead: originFromHead)
        drawn += encodeStatsCard(enc, viewIndices: viewIndices, drawable: drawable,
                                 originFromHead: originFromHead)
        return drawn
    }

    // Space Atlas VR: both hands from their own measured joints.
    // Right index reports pinch; left hand reports palm-up posture. No guessed UI contact.
    // Both hands' input handling, including the left menu gesture, stays untouched.
    private var rightHandProbePipeline: MTLRenderPipelineState?
    private var rightHandProbeTried = false
    private var leftPalmIsUp = false
    /// Only Space Atlas draws these hands; every other game skips the per-frame snapshot.
    private lazy var handGuideTitle = String(cString: kl_app_target_name_or_default()) == "x8016534201745429"
    private func encodeHandGuide(_ enc: MTLRenderCommandEncoder, viewIndices: [Int],
                                     drawable: KLDrawable, originFromHead: simd_float4x4, hand: Int32) -> Int {
        guard handGuideTitle else { return 0 }
        var joints = [Float](repeating: 0, count: 81)
        var pinch: Float = 0, palmUp: Float = 0
        let tracked = joints.withUnsafeMutableBufferPointer {
            kl_ovrp_hand_guide(hand, $0.baseAddress, &pinch, &palmUp)
        }
        guard tracked != 0 else {
            if hand == 0 { leftPalmIsUp = false }
            return 0
        }
        if hand == 0 {
            // Enter at 60 degrees from up, release at ~69 degrees: avoid
            // flickering on small tracking changes while holding the menu pose.
            leftPalmIsUp = palmUp > (leftPalmIsUp ? 0.35 : 0.50)
        }
        if !rightHandProbeTried {
            rightHandProbeTried = true
            let source = """
            #include <metal_stdlib>
            using namespace metal;
            struct O { float4 pos [[position]]; float shade; };
            vertex O rh_v(uint id [[vertex_id]], constant float4 *v [[buffer(0)]],
                          constant float4x4 *mvp [[buffer(1)]], ushort eye [[amplification_id]]) {
                O o; o.pos=mvp[eye]*float4(v[id].xyz,1.0);
                o.shade=v[id].w; return o;
            }
            fragment float4 rh_f(O o [[stage_in]], constant float4 &color [[buffer(0)]]) {
                return float4(color.rgb*o.shade,1.0);
            }
            """
            do {
                let lib = try device.makeLibrary(source: source, options: nil)
                let p = MTLRenderPipelineDescriptor()
                p.vertexFunction = lib.makeFunction(name: "rh_v")
                p.fragmentFunction = lib.makeFunction(name: "rh_f")
                p.colorAttachments[0].pixelFormat = layerRenderer?.configuration.colorFormat ?? KLRealityTarget.colorFormat
                p.depthAttachmentPixelFormat = .depth32Float
                p.maxVertexAmplificationCount = amplification
                rightHandProbePipeline = try device.makeRenderPipelineState(descriptor: p)
                NSLog("[hands] both hands: tracked palms and five digits; right index pinch color, left palm-up color; inputs unchanged")
            } catch { NSLog("[hands] right-hand diagnostic unavailable: %@", String(describing: error)) }
        }
        guard let pipe = rightHandProbePipeline else { return 0 }
        let points = (0..<27).map { i in
            SIMD3<Float>(joints[i*3],joints[i*3+1],joints[i*3+2])
        }
        func unit(_ v: SIMD3<Float>, fallback: SIMD3<Float>) -> SIMD3<Float> {
            simd_length(v) > 0.00001 ? simd_normalize(v) : fallback
        }
        let wrist = points[0]
        let across = unit(points[6]-points[21], fallback: SIMD3<Float>(1,0,0))
        let along = unit(points[11]-wrist, fallback: SIMD3<Float>(0,0,-1))
        let palmNormal = unit(simd_cross(across,along), fallback: SIMD3<Float>(0,1,0))
        let palmWidth = min(0.11,max(0.05,simd_distance(points[6],points[21])))
        let light = unit(SIMD3<Float>(originFromHead.columns.1.x,
                                      originFromHead.columns.1.y,
                                      originFromHead.columns.1.z), fallback: palmNormal)
        func vertex(_ p: SIMD3<Float>, _ normal: SIMD3<Float>) -> SIMD4<Float> {
            SIMD4<Float>(p,0.55+0.45*max(0,simd_dot(normal,light)))
        }
        var white = [SIMD4<Float>]()
        var index = [SIMD4<Float>]()

        // Rounded, tapered fingers follow their own knuckles, not the camera.
        // Catmull-Rom interpolation rounds the joints without replacing poses.
        func finger(_ ids: [Int], radius: Float) -> [SIMD4<Float>] {
            let control = ids.map { points[$0] }
            var centers = [SIMD3<Float>]()
            var sizes = [Float]()
            for i in 0..<(control.count-1) {
                let a = control[max(0,i-1)], b = control[i]
                let c = control[i+1], d = control[min(control.count-1,i+2)]
                for k in 0..<3 {
                    let t = Float(k)/3, t2 = t*t, t3 = t2*t
                    // Catmull-Rom, one typed term at a time: as a single expression
                    // Swift 6.3 (Xcode 26.6) gives up type-checking it.
                    let c1: SIMD3<Float> = (c - a) * t
                    let c2: SIMD3<Float> = (2 * a - 5 * b + 4 * c - d) * t2
                    let c3: SIMD3<Float> = (3 * b - a - 3 * c + d) * t3
                    let sum: SIMD3<Float> = 2 * b + c1 + c2 + c3
                    centers.append(0.5 * sum)
                    let fraction = (Float(i)+t)/Float(control.count-1)
                    sizes.append(radius*(1-0.38*fraction))
                }
            }
            let tip = control.last!
            let tipDirection = unit(tip-control[control.count-2], fallback: along)
            let tipRadius = radius*0.62
            // The last ring's hemisphere ends exactly at the measured fingertip.
            centers.append(tip-tipDirection*tipRadius); sizes.append(tipRadius)
            centers.append(tip-tipDirection*tipRadius*0.30); sizes.append(tipRadius*0.70)
            centers.append(tip); sizes.append(0)
            let sides = 12
            var rings = [[SIMD4<Float>]]()
            for i in centers.indices {
                let tangent = unit(centers[min(centers.count-1,i+1)]-centers[max(0,i-1)], fallback: tipDirection)
                var x = simd_cross(tangent,palmNormal)
                if simd_length(x)<0.01 { x = simd_cross(tangent,across) }
                x = unit(x,fallback: across)
                let y = unit(simd_cross(tangent,x),fallback: palmNormal)
                rings.append((0..<sides).map { k in
                    let angle = Float(k)*2*Float.pi/Float(sides)
                    let normal = x*cos(angle)+y*sin(angle)
                    return vertex(centers[i]+(x*cos(angle)+y*sin(angle)*0.88)*sizes[i],normal)
                })
            }
            var mesh = [SIMD4<Float>]()
            for i in 0..<(rings.count-1) {
                for k in 0..<sides {
                    let n = (k+1)%sides
                    mesh += [rings[i][k],rings[i+1][k],rings[i][n],
                             rings[i][n],rings[i+1][k],rings[i+1][n]]
                }
            }
            return mesh
        }
        white += finger([1,2,3,4],radius: palmWidth*0.14)       // thumb, opposable by measured pose
        index = finger([6,7,8,9],radius: palmWidth*0.12)
        white += finger([11,12,13,14],radius: palmWidth*0.125)
        white += finger([16,17,18,19],radius: palmWidth*0.115)
        white += finger([21,22,23,24],radius: palmWidth*0.095)

        // A closed, bevelled palm connects wrist, thumb base and all knuckles.
        // Its width comes from this hand; no game mesh or fixed hand pose is used.
        let outline = [wrist+across*palmWidth*0.27,points[1],points[5],
                       points[6],points[11],points[16],points[21],points[20],
                       wrist-across*palmWidth*0.27]
        let center = outline.reduce(SIMD3<Float>(repeating: 0),+)/Float(outline.count)
        let thickness = palmWidth*0.15
        var top = [SIMD4<Float>](), bottom = [SIMD4<Float>]()
        for p in outline {
            let radial = unit(p-center,fallback: across)
            let bevelNormal = unit(palmNormal*0.7+radial*0.3,fallback: palmNormal)
            top.append(vertex(p+palmNormal*thickness*0.6,bevelNormal))
            bottom.append(vertex(p-palmNormal*thickness*0.6,-bevelNormal))
        }
        for k in outline.indices {
            let n = (k+1)%outline.count
            white += [vertex(center+palmNormal*thickness,palmNormal),top[k],top[n],
                      vertex(center-palmNormal*thickness,-palmNormal),bottom[n],bottom[k],
                      top[k],bottom[k],top[n],top[n],bottom[k],bottom[n]]
        }
        var mvp = viewIndices.map { vi in
            drawable.computeProjection(viewIndex: vi) *
                (originFromHead*Self.headFromView(drawable.views[vi].transform,midpoint: eyeMidpointInDevice)).inverse
        }
        // The full hand exceeds setVertexBytes' 4 KB limit. A Metal buffer is
        // retained by the command buffer until this draw has finished.
        func draw(_ mesh: [SIMD4<Float>], color: SIMD4<Float>) {
            let buffer = mesh.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!,length: $0.count,options: .storageModeShared)
            }
            guard let buffer else { return }
            var color = color
            enc.setVertexBuffer(buffer,offset: 0,index: 0)
            enc.setFragmentBytes(&color,length: MemoryLayout<SIMD4<Float>>.stride,index: 0)
            enc.drawPrimitives(type: .triangle,vertexStart: 0,vertexCount: mesh.count)
        }
        enc.setRenderPipelineState(pipe)
        if let depthState { enc.setDepthStencilState(depthState) }
        enc.setCullMode(.none)
        mvp.withUnsafeMutableBytes { enc.setVertexBytes($0.baseAddress!,length: $0.count,index: 1) }
        let leftGreen = hand == 0 && leftPalmIsUp
        draw(white,color: leftGreen ? SIMD4<Float>(0,1,0,1) : SIMD4<Float>(1,1,1,1))
        draw(index,color: leftGreen || (hand == 1 && pinch >= 0.8) ? SIMD4<Float>(0,1,0,1) : SIMD4<Float>(1,1,1,1))
        return 2
    }

    // The play area (KLRoomGuard): made at the first frame, once the device and
    // the pass's formats are known; nil for the rest of the run where there is
    // nothing to show.
    private var boundary: KLBoundary?
    private var boundaryTried = false
    private var boundaryRecenter: simd_float4x4?
    private func encodeBoundary(_ enc: MTLRenderCommandEncoder, viewIndices: [Int],
                                drawable: KLDrawable, originFromHead: simd_float4x4) {
        if !boundaryTried {
            boundaryTried = true
            boundary = KLBoundary.make(device: device, color: layerRenderer?.configuration.colorFormat ?? KLRealityTarget.colorFormat,
                                       amplification: amplification)
            boundary?.watch(worldTracking)
        }
        guard let boundary, let head = lastGoodOriginFromDevice else { return }
        // A stationary circle goes round the head at the first frame, and again
        // where the player recentres (the Options hold) — as on a Quest.
        if boundary.stationary, boundaryRecenter != recenterXform {
            boundaryRecenter = recenterXform
            boundary.centre(at: SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z))
        }
        let views = viewIndices.map { vi in
            (projection: drawable.computeProjection(viewIndex: vi),
             viewFromWorld: (originFromHead * Self.headFromView(drawable.views[vi].transform,
                                                                midpoint: eyeMidpointInDevice)).inverse)
        }
        var points = [SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)]
        points += KleptonControllers.rawHandPositions()
        boundary.encode(enc, views: views, worldFromRaw: recenterXform, points: points,
                        mixedSpace: Immersive.mixedAtBoot, gameShowsRoom: kl_ovrp_passthrough_live() != 0,
                        depthState: depthPassState)
    }

    // How the game is running, shown in the headset (KL_STATS_CARD): a panel in
    // the manner of a Quest's performance overlay — the last second in numbers,
    // the last minute as graphs (KleptonMetrics.swift) — or, with
    // KL_STATS_CARD=line, the one line this began as. Fixed to the head, above
    // the middle of the view, where it is out of the way of what one looks at
    // and still readable with a glance.
    //
    // The two menu buttons of the controllers held together for a second show
    // it and hide it, in every game (kl_metrics_chord); KL_STATS_CARD only says
    // whether it is there from the start. Until it is first shown nothing of it
    // exists — no sampling, no count of the GPU's command buffers, no texture —
    // so the first time it appears it says that it is measuring, and its graphs
    // begin there.
    //
    // A meter must not be what it measures. So nothing of it happens on this
    // thread but two counts a frame (kl_metrics_note_shown, _note_composite)
    // and one quad in a pass that is being drawn anyway: the sample is taken
    // once a second on the heartbeat's thread (kl_metrics.c), and the picture of it
    // is drawn on a queue of its own into the texture no frame is reading — two
    // textures, swapped when the new one is complete.
    private enum StatsMode { case panel, line }
    private static let statsMode: StatsMode =
        ProcessInfo.processInfo.environment["KL_STATS_CARD"]?.lowercased() == "line" ? .line : .panel
    private var statsFromStart: Bool? = klEnvOn("KL_STATS_CARD", default: false)     // nil once applied
    private let statsLock = NSLock()
    private var statsFront: MTLTexture?                 // under statsLock: the one a frame may sample
    private var statsTimer: DispatchSourceTimer?
    private var statsSampler: MTLSamplerState?          // the panel's own: it picks between the texture's levels
    private let statsQueue = DispatchQueue(label: "klepton.metrics", qos: .utility)

    private func startStats(_ mode: StatsMode) {
        kl_metrics_begin()
        let px = mode == .panel ? KLMetricsPanel.panelPixels : KLMetricsPanel.linePixels
        // The panel in three sizes, each drawn for itself, as the levels of one texture: it is shown with
        // as many pixels as the game's picture has there, which differs from game to game (KleptonMetrics).
        let levels = mode == .panel ? KLMetricsPanel.panelLevels : 1
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: px.w,
                                                            height: px.h, mipmapped: false)
        desc.mipmapLevelCount = levels
        desc.usage = .shaderRead
        let contexts = (0..<levels).compactMap {
            KLMetricsPanel.makeContext(w: px.w >> $0, h: px.h >> $0,
                                       units: mode == .panel ? KLMetricsPanel.panelLayout.w : nil)
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        guard let a = device.makeTexture(descriptor: desc), let b = device.makeTexture(descriptor: desc),
              let levelSampler = device.makeSamplerState(descriptor: sd), contexts.count == levels else {
            NSLog("[cp] the metrics panel has no texture to draw into — not shown")
            return
        }
        statsSampler = levelSampler
        let textures = [a, b]
        var samples: [KLMetricsSample] = []
        var seq: UInt32 = 0, turn = 0, drawn = false
        // The upscale as the compositor said it last, and whether a tap of Create + Options switches it
        // (KleptonMetalFX.tapToggle). A switch is drawn at once rather than with the next second's
        // sample — whoever tapped is looking for it.
        struct Upscale: Equatable { var tap = false, state = -1, outW = 0, outH = 0, path = "", note = "",
                                    frameGen = 0, frameGenTap = false }
        func upscaleNow() -> Upscale {
            var u = kl_metrics()
            kl_metrics_upscale_now(&u)
            // The tap cycles frame generation instead where it was chosen or MetalFX has nothing to switch
            // (KleptonMetalFX.tapToggle).
            let tap = KleptonTapAction.resolved
            let fgTap = tap == .frameGen
            return Upscale(tap: KleptonMetalFX.canUpscale && tap == .metalFX, state: Int(u.upscale),
                           outW: Int(u.out_w), outH: Int(u.out_h),
                           path: withUnsafeBytes(of: &u.path) {
                               String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                           },
                           note: withUnsafeBytes(of: &u.upscale_note) {
                               String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                           },
                           frameGen: KleptonFrameGen.mode.rawValue, frameGenTap: fgTap)
        }
        func put(_ u: Upscale, into s: inout KLMetricsSample) {
            s.metalfxTap = u.tap
            s.upscale = u.state; s.outW = u.outW; s.outH = u.outH
            s.path = u.path; s.upscaleNote = u.note
            s.frameGen = u.frameGen; s.frameGenTap = u.frameGenTap
        }
        var upscale = Upscale()
        let timer = DispatchSource.makeTimerSource(queue: statsQueue)
        timer.schedule(deadline: .now(), repeating: 0.25, leeway: .milliseconds(50))
        timer.setEventHandler { [weak self] in
            var m = kl_metrics()
            let now = kl_metrics_latest(&m)
            // Once at once, before there is a sample — whoever has just held the two buttons for a second
            // is to see that it worked — and from then on with each new one.
            let fresh = now != 0 && now != seq
            let u = upscaleNow()
            let switched = u != upscale
            upscale = u
            guard fresh || !drawn || (switched && !samples.isEmpty) else { return }
            if switched, !fresh, var last = samples.popLast() {
                put(u, into: &last)
                samples.append(last)
            }
            if fresh {
                seq = now
                var s = KLMetricsSample()
                s.fps = m.fps; s.displayHz = m.display_hz
                s.compiles = m.compiles; s.compilesCached = m.compiles_cached
                s.gpuBusy = m.gpu_busy; s.gpuGameMs = m.gpu_game_ms; s.gpuCompMs = m.gpu_comp_ms
                s.cpuCores = m.cpu_cores; s.cpuTop = m.cpu_top
                s.cpuTopName = withUnsafeBytes(of: &m.cpu_top_name) {
                    String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                }
                s.memUsedMB = Int(m.mem_used_mb); s.memLeftMB = Int(m.mem_left_mb)
                s.thermal = ProcessInfo.processInfo.thermalState.rawValue
                s.eyeW = Int(m.eye_w); s.eyeH = Int(m.eye_h)
                // Generated pictures are each shown once (none is made for a display frame that brings a
                // new game picture anyway), so what the eye gets is the game's plus those.
                s.generated = Float(KleptonFrameGen.ratePerSecond())
                s.shown = min(s.displayHz, s.fps + s.generated)
                // The upscale as it is now rather than as the sample took it, so a tap is not undone by a
                // sample a quarter of a second old.
                put(u, into: &s)
                samples.append(s)
                if samples.count > KLMetricsPanel.history { samples.removeFirst(samples.count - KLMetricsPanel.history) }
            }
            if mode == .line && samples.isEmpty { return }
            drawn = true
            turn += 1
            let back = textures[turn % 2]
            for (level, ctx) in contexts.enumerated() {
                if mode == .panel { KLMetricsPanel.drawPanel(ctx, samples: samples) }
                else if let s = samples.last { KLMetricsPanel.drawLine(ctx, sample: s) }
                guard let data = ctx.data else { return }
                back.replace(region: MTLRegionMake2D(0, 0, px.w >> level, px.h >> level), mipmapLevel: level,
                             withBytes: data, bytesPerRow: (px.w >> level) * 4)
            }
            guard let self else { return }
            self.statsLock.lock(); self.statsFront = back; self.statsLock.unlock()
        }
        timer.resume()
        statsTimer = timer
        NSLog("[cp] metrics %@ in the view (KL_STATS_CARD), refreshed once a second",
              mode == .panel ? "panel" : "line")
    }

    private func encodeStatsCard(_ enc: MTLRenderCommandEncoder,
                                 viewIndices: [Int],
                                 drawable: KLDrawable,
                                 originFromHead: simd_float4x4) -> Int {
        guard let pipe = overlayPipeline else { return 0 }
        let mode = Self.statsMode
        if let fromStart = statsFromStart {
            statsFromStart = nil
            if fromStart { kl_metrics_set_visible(1) }
        }
        // Create and Options held together for a second bring it up and put it away (kl_metrics_chord).
        guard kl_metrics_visible() != 0 else { return 0 }
        if statsTimer == nil { startStats(mode) }
        statsLock.lock(); let front = statsFront; statsLock.unlock()
        guard let tex = front else { return 0 }
        var ov = kl_ovrp_overlay()
        ov.shape = 0
        ov.head_locked = 1
        ov.tex_w = Int32(tex.width); ov.tex_h = Int32(tex.height)
        ov.viewport.0 = (0, 0, Int32(tex.width), Int32(tex.height))
        ov.viewport.1 = (0, 0, Int32(tex.width), Int32(tex.height))
        // In the head's own frame, 1.2 m ahead: the panel just left of the middle with its lower edge a little
        // above it (the owner's place for it, found in three tries on the headset); the line at the lower left.
        let size = mode == .panel ? KLMetricsPanel.panelSize : KLMetricsPanel.lineSize
        ov.pose = mode == .panel ? (0, 0, 0, 1, -0.05, 0.101, -1.2) : (0, 0, 0, 1, -0.16, -0.36, -1.2)
        ov.size = (size.x, size.y)
        ov.origin_top_left = 1
        var uniforms: [kl_overlay_uniforms] = []
        for vi in viewIndices {
            uniforms.append(kl_overlay_build(&ov, Int32(vi), originFromHead,
                                             Self.headFromView(drawable.views[vi].transform,
                                                               midpoint: eyeMidpointInDevice),
                                             drawable.computeProjection(viewIndex: vi)))
        }
        guard uniforms.contains(where: { $0.visible != 0 }) else { return 0 }
        // Over the guest's quads whatever their distance: a hand-held panel or a label nearer than 1.2 m
        // writes its depth over its whole rectangle and would hide the panel (KleptonOverlayDepth).
        if let s = statsCardState ?? depthState { enc.setDepthStencilState(s) }
        enc.setRenderPipelineState(pipe)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(statsSampler ?? sampler, index: 0)
        uniforms.withUnsafeBytes { buf in
            enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
        }
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        return 1
    }

    // Black behind the quads of a frame that carries no eye picture (kl_ovrp.c,
    // kl_ovrp_eyeless_black). In a mixed space the system shows the room
    // wherever this pass wrote no depth, so "nothing behind the loading screen"
    // has to be drawn: one opaque black quad fixed to the head, far behind any
    // panel a guest places (its own are tens to a few hundred metres away) and
    // wide enough to fill the view, through the panels' own pipeline so that it
    // writes depth like they do.
    //
    // The same black in every mixed space (Immersive.mixedBlack — which is every
    // game since they all open one): each frame without an eye picture gets it,
    // whoever the guest is. Beat Saber 1.45 submits nothing for 17 to 50 frames
    // at each change between menu and level, and the room showed through them.
    // Not behind the loading panorama (encodeOverlays' `backdrop`), which is its
    // own backdrop. Read once, at the first such frame.
    private lazy var mixedBlack: Bool = {
        let on = Immersive.mixedBlackAtBoot
        if on { NSLog("[cp] mixed space: black wherever the guest shows no picture") }
        return on
    }()
    private var blackTexture: MTLTexture?
    private func encodeBlackBackdrop(_ enc: MTLRenderCommandEncoder,
                                     pipe: MTLRenderPipelineState,
                                     viewIndices: [Int],
                                     drawable: KLDrawable,
                                     originFromHead: simd_float4x4) {
        if blackTexture == nil {
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1,
                                                                height: 1, mipmapped: false)
            desc.usage = .shaderRead
            if let tex = device.makeTexture(descriptor: desc) {
                var px: [UInt8] = [0, 0, 0, 255]
                tex.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0,
                            withBytes: &px, bytesPerRow: 4)
                blackTexture = tex
                NSLog("[cp] frames without an eye picture: black behind their quads")
            }
        }
        guard let tex = blackTexture else { return }
        var ov = kl_ovrp_overlay()
        ov.shape = 0
        ov.tex_w = 1; ov.tex_h = 1
        ov.viewport.0 = (0, 0, 1, 1)
        ov.viewport.1 = (0, 0, 1, 1)
        // 600 m: a little behind where the eye picture itself is placed
        // (500 m, which the system is known to take as content) and behind
        // Unreal's splash quads at 200 m; 5 km across is ±76° from there.
        ov.pose = (0, 0, 0, 1, 0, 0, -600)
        ov.size = (5000, 5000)
        ov.origin_top_left = 1
        ov.head_locked = 1
        var uniforms: [kl_overlay_uniforms] = []
        for vi in viewIndices {
            uniforms.append(kl_overlay_build(&ov, Int32(vi), originFromHead,
                                             Self.headFromView(drawable.views[vi].transform,
                                                               midpoint: eyeMidpointInDevice),
                                             drawable.computeProjection(viewIndex: vi)))
        }
        guard uniforms.contains(where: { $0.visible != 0 }) else { return }
        enc.setRenderPipelineState(pipe)
        enc.setFragmentTexture(tex, index: 0)
        if let sampler { enc.setFragmentSamplerState(sampler, index: 0) }
        uniforms.withUnsafeBytes { buf in
            enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
        }
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    // The loading screen of a guest that says what it should show instead of
    // drawing it — Half-Life: Alyx (runtime/linux/kl_lx_interstitial.c). The
    // game's own is a web page that SteamVR displays, rebuilt from the game's
    // files in KleptonHologram.swift; this is the same words on a card, for as
    // long as that is not read or where it cannot be: what is loading, a tip,
    // and the line that says the level is ready and the trigger starts it.
    private var cardTexture: MTLTexture?
    private var cardGeneration: UInt32 = .max
    private var cardWorldFromQuad: simd_float4x4?
    private static let cardSize = SIMD2<Float>(1.8, 1.0)     // metres
    private static let cardPixels = (w: 1800, h: 1000)
    private static let cardEnabled = klEnvOn("KL_LOADING_CARD", default: true)
    private static let hologramEnabled = klEnvOn("KL_HOLOGRAM", default: true)
    private var hologram: KleptonHologram?

    private func encodeHologram(_ enc: MTLRenderCommandEncoder, _ st: inout kl_interstitial,
                                viewIndices: [Int], drawable: KLDrawable,
                                originFromHead: simd_float4x4) -> Bool {
        guard Self.hologramEnabled, klIsAlyx() else { return false }
        if hologram == nil {
            guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            else { return false }
            let made = KleptonHologram(device: device, root: docs.appendingPathComponent("lx/game/hlvr"))
            hologram = made
            DispatchQueue.global(qos: .utility).async { made.load() }
        }
        guard let hologram, hologram.ready else { return false }
        let now = CACurrentMediaTime()
        guard hologram.update(phase: Int(st.phase), map: Self.cardString(&st.map),
                              message: Self.cardString(&st.message),
                              loading: Self.cardString(&st.loading),
                              chapter: Self.cardString(&st.chapter),
                              now: now, originFromHead: originFromHead)
        else { return false }
        // The black behind it, with depth, in every kind of space: outside a mixed one nothing else
        // draws it, and the system drops the tiles of a drawable that hold no depth. Those were dark
        // squares in the map for as long as there was no eye picture (2026-10-30). Over an eye
        // picture, which is here again for the map's fade-out, it would hide the game.
        if kl_ovrp_eye_layer_live() == 0, let pipe = overlayPipeline {
            if let depthState { enc.setDepthStencilState(depthState) }
            encodeBlackBackdrop(enc, pipe: pipe, viewIndices: viewIndices, drawable: drawable,
                                originFromHead: originFromHead)
        }
        let views = viewIndices.map { vi -> KleptonHologram.View in
            let worldFromView = originFromHead * Self.headFromView(drawable.views[vi].transform,
                                                                   midpoint: eyeMidpointInDevice)
            return KleptonHologram.View(clipFromWorld: drawable.computeProjection(viewIndex: vi) * worldFromView.inverse,
                                        eye: SIMD3<Float>(worldFromView.columns.3.x, worldFromView.columns.3.y,
                                                          worldFromView.columns.3.z))
        }
        return hologram.encode(enc, color: pipelineFormat, amplification: amplification, views: views, now: now)
    }

    private func encodeLoadingCard(_ enc: MTLRenderCommandEncoder,
                                   viewIndices: [Int],
                                   drawable: KLDrawable,
                                   originFromHead: simd_float4x4) -> Int {
        guard Self.cardEnabled, let pipe = overlayPipeline else { return 0 }
        var st = kl_interstitial()
        _ = kl_interstitial_get(&st)
        if encodeHologram(enc, &st, viewIndices: viewIndices, drawable: drawable,
                          originFromHead: originFromHead) {
            cardWorldFromQuad = nil
            return 1
        }
        // Only while the guest shows no picture of its own: the card is what
        // there is to look at instead, not something to lay over a level. The
        // exception is the line that asks for the trigger: the game may draw
        // again while it still waits — a faded-out picture — and the card is
        // then the only thing that says what it waits for.
        guard st.phase != 0, kl_ovrp_eye_layer_live() == 0 || st.message.0 != 0 else {
            cardWorldFromQuad = nil
            return 0
        }
        if cardTexture == nil || cardGeneration != st.generation {
            cardTexture = makeLoadingCard(&st) ?? cardTexture
            cardGeneration = st.generation
        }
        guard let tex = cardTexture else { return 0 }

        // Where: two metres ahead of where the head was looking when the card
        // came up, upright, and put there again if the head has turned well
        // away from it — a card fixed to the head is hard to read, one left
        // behind is not read at all.
        let headPos = SIMD3<Float>(originFromHead.columns.3.x, originFromHead.columns.3.y,
                                   originFromHead.columns.3.z)
        var fwd = SIMD3<Float>(-originFromHead.columns.2.x, 0, -originFromHead.columns.2.z)
        fwd = simd_length(fwd) > 0.01 ? simd_normalize(fwd) : SIMD3<Float>(0, 0, -1)
        var place = cardWorldFromQuad == nil
        if let m = cardWorldFromQuad {
            var to = SIMD3<Float>(m.columns.3.x - headPos.x, 0, m.columns.3.z - headPos.z)
            let dist = simd_length(to)
            to = dist > 0.01 ? to / dist : fwd
            if simd_dot(to, fwd) < 0.5 || dist > 3.5 || dist < 1.0 { place = true }
        }
        if place {
            let c = headPos + fwd * 2.0 + SIMD3<Float>(0, -0.05, 0)
            // The quad's +z faces the viewer.
            let z = -fwd, y = SIMD3<Float>(0, 1, 0), x = simd_cross(y, z)
            cardWorldFromQuad = simd_float4x4(SIMD4<Float>(x, 0), SIMD4<Float>(y, 0),
                                              SIMD4<Float>(z, 0), SIMD4<Float>(c, 1))
        }
        guard let m = cardWorldFromQuad else { return 0 }
        let q = simd_quatf(m)
        var ov = kl_ovrp_overlay()
        ov.shape = 0
        ov.tex_w = Int32(tex.width); ov.tex_h = Int32(tex.height)
        ov.viewport.0 = (0, 0, Int32(tex.width), Int32(tex.height))
        ov.viewport.1 = (0, 0, Int32(tex.width), Int32(tex.height))
        ov.pose = (q.imag.x, q.imag.y, q.imag.z, q.real, m.columns.3.x, m.columns.3.y, m.columns.3.z)
        ov.size = (Self.cardSize.x, Self.cardSize.y)
        ov.origin_top_left = 1
        var uniforms: [kl_overlay_uniforms] = []
        for vi in viewIndices {
            uniforms.append(kl_overlay_build(&ov, Int32(vi), originFromHead,
                                             Self.headFromView(drawable.views[vi].transform,
                                                               midpoint: eyeMidpointInDevice),
                                             drawable.computeProjection(viewIndex: vi)))
        }
        guard uniforms.contains(where: { $0.visible != 0 }) else { return 0 }
        if let depthState { enc.setDepthStencilState(depthState) }
        enc.setRenderPipelineState(pipe)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        uniforms.withUnsafeBytes { buf in
            enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
        }
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        return 1
    }

    /// The game's own typeface, from the game's own files where they are staged.
    private static let cardFontNames: [String: String] = {
        var names: [String: String] = [:]
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        else { return names }
        for weight in ["regular", "light", "semibold", "bold"] {
            let url = docs.appendingPathComponent("lx/game/hlvr/webui/fonts/raju-\(weight).otf")
            guard let provider = CGDataProvider(url: url as CFURL), let font = CGFont(provider)
            else { continue }
            CTFontManagerRegisterGraphicsFont(font, nil)
            if let ps = font.postScriptName as String? { names[weight] = ps }
        }
        return names
    }()
    private static func cardFont(_ weight: String, _ size: CGFloat) -> UIFont {
        if let ps = cardFontNames[weight], let f = UIFont(name: ps, size: size) { return f }
        let w: UIFont.Weight = weight == "bold" ? .bold : weight == "semibold" ? .semibold
                             : weight == "light" ? .light : .regular
        return UIFont.systemFont(ofSize: size, weight: w)
    }
    private static func cardString<T>(_ field: inout T) -> String {
        withUnsafeBytes(of: &field) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    private func makeLoadingCard(_ st: inout kl_interstitial) -> MTLTexture? {
        let w = Self.cardPixels.w, h = Self.cardPixels.h
        let loading = Self.cardString(&st.loading), message = Self.cardString(&st.message)
        let chapter = Self.cardString(&st.chapter), map = Self.cardString(&st.map)
        let tip = Self.cardString(&st.tip), description = Self.cardString(&st.description)
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        // UIKit's drawing has its origin at the top; the bitmap's first row is
        // the top row either way, which is what origin_top_left says.
        ctx.translateBy(x: 0, y: CGFloat(h)); ctx.scaleBy(x: 1, y: -1)
        UIGraphicsPushContext(ctx)
        let orange = UIColor(red: 1.0, green: 0.62, blue: 0.16, alpha: 1)
        UIColor(red: 0.035, green: 0.04, blue: 0.05, alpha: 1).setFill()
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        orange.setFill()
        ctx.fill(CGRect(x: 90, y: 86, width: 8, height: CGFloat(h) - 172))
        let left: CGFloat = 140, width = CGFloat(w) - left - 110
        var y: CGFloat = 86
        func draw(_ text: String, _ font: UIFont, _ color: UIColor, gap: CGFloat, kern: CGFloat = 0) {
            guard !text.isEmpty else { return }
            let para = NSMutableParagraphStyle(); para.lineSpacing = font.pointSize * 0.18
            let attr: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color,
                                                       .paragraphStyle: para, .kern: kern]
            let s = NSAttributedString(string: text, attributes: attr)
            let box = s.boundingRect(with: CGSize(width: width, height: CGFloat(h)),
                                     options: [.usesLineFragmentOrigin], context: nil)
            s.draw(with: CGRect(x: left, y: y, width: width, height: ceil(box.height)),
                   options: [.usesLineFragmentOrigin], context: nil)
            y += ceil(box.height) + gap
        }
        let heading = [chapter, map].filter { !$0.isEmpty }.joined(separator: "  ·  ")
        draw(heading.uppercased(), Self.cardFont("semibold", 40), orange, gap: 26, kern: 4)
        if message.isEmpty {
            draw(loading.isEmpty ? "LOADING" : loading, Self.cardFont("bold", 124), .white, gap: 44, kern: 10)
        } else {
            draw(message.uppercased(), Self.cardFont("bold", 96), orange, gap: 44, kern: 6)
        }
        draw(tip, Self.cardFont("regular", 46), UIColor(white: 0.92, alpha: 1), gap: 34)
        draw(description, Self.cardFont("light", 38), UIColor(white: 0.62, alpha: 1), gap: 0)
        UIGraphicsPopContext()
        guard let data = ctx.data else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w,
                                                            height: h, mipmapped: false)
        desc.usage = .shaderRead
        guard let tex = device.makeTexture(descriptor: desc) else { return nil }
        tex.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data,
                    bytesPerRow: w * 4)
        NSLog("[cp] loading card drawn: \(message.isEmpty ? loading : message)")
        return tex
    }
    /// Matches `struct KLEq` in kl_msl_equirect (kl_reproject.c) byte for byte:
    /// two float4x4 then four uint, 144 bytes, 16-aligned. One per amplified
    /// view, indexed by [[amplification_id]].
    private struct KLEqUniform {
        var invProj: simd_float4x4 = matrix_identity_float4x4
        var worldRot: simd_float4x4 = matrix_identity_float4x4
        var slice: UInt32 = 0
        var flipY: UInt32 = 0
        var srgbDecode: UInt32 = 0
        var visible: UInt32 = 0
        var test: UInt32 = 0
        var hscale: Float = 1
    }

    /// The 3x3 rotation of a rigid transform, re-homed as a 4x4 with no
    /// translation — the panorama is at infinity, so only orientation maps a
    /// view-space ray into the sphere\'s world space.
    private static func rotationOnly(_ m: simd_float4x4) -> simd_float4x4 {
        var r = matrix_identity_float4x4
        r.columns.0 = SIMD4<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z, 0)
        r.columns.1 = SIMD4<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z, 0)
        r.columns.2 = SIMD4<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z, 0)
        r.columns.3 = SIMD4<Float>(0, 0, 0, 1)
        return r
    }

    /// V flips and sRGB decode differ by how the guest authored the panorama;
    /// both are one device run to settle, so they are env-switchable.
    private static let skipHeadLockedOverlays = klEnvOn("KL_OVERLAY_SKIP_HEADLOCKED", default: false)
    /// KL_OVERLAY_SUBMIT_ORDER: quad layers are composited in the order the
    /// guest submitted them, which is what a Quest's compositor does, instead of
    /// being depth-tested against each other. The eye pass leaves `.greater`
    /// with writes on in force, so without this a quad stamps its distance over
    /// its whole rectangle, transparent parts included, and a quad submitted
    /// after it that lies farther away is cut off along the nearer one's edge —
    /// where that edge falls moving with the head. Alien: Rogue Incursion's
    /// interaction labels lost a side that way behind the invisible margin of a
    /// panel held in the hand (2026-10-09). Default on for that title only.
    private static let overlaysInSubmitOrder =
        klEnvOn("KL_OVERLAY_SUBMIT_ORDER",
                default: ["x24882487241367228", "x5812519008825194"].contains(klTargetName()))
    private static let drawEquirectOverlays = klEnvOn("KL_OVERLAY_EQ", default: true)
    private static let eqFlipY = klEnvOn("KL_EQ_FLIP", default: false)
    private static let eqSrgbDecode = klEnvOn("KL_EQ_SRGB", default: false)
    // KL_EQ_TEST=1 solid magenta (pass reaches display?), 2 UV gradient
    // (mapping), 3 sampled texture brightened (dim content?), 4 world-ray
    // colour (reconstruction). 0 = normal. A no-rebuild localiser for a black
    // skybox.
    private static let eqTest: UInt32 = {
        if let s = ProcessInfo.processInfo.environment["KL_EQ_TEST"],
           let v = UInt32(s) { return v }
        return 0
    }()
    // Horizontal angular scale for the equirect: 1.0 = full 360 deg across the
    // texture width (2:1 vs the 180 deg vertical, so a square video looks
    // stretched), 2.0 = 180 deg across the width (square texture -> square
    // angular, un-stretched). KL_EQ_HSCALE overrides.
    private static let eqHScale: Float = {
        if let s = ProcessInfo.processInfo.environment["KL_EQ_HSCALE"],
           let v = Float(s) { return v }
        return 2.0
    }()

    /// Composite a 360 equirect panorama (shape 5). A fullscreen triangle whose
    /// fragment shader turns each pixel back into a world ray and samples the
    /// sphere, at far-plane reverse-Z so eye content (when there is any) sits in
    /// front. During shader warmup there is no eye content and this is the whole
    /// picture — which on Quest is the loading skybox and here was black.
    private func encodeEquirect(_ enc: MTLRenderCommandEncoder,
                                ov: kl_ovrp_overlay,
                                viewIndices: [Int],
                                drawable: KLDrawable,
                                originFromHead: simd_float4x4) -> Bool {
        guard let pipe = equirectPipeline else { return false }
        var w: Int32 = 0, h: Int32 = 0
        // texL first — AC Nexus renders its backdrop into its OWN texture and
        // submits that handle, exactly as its menu quads do; the layer swapchain
        // image is the empty fallback. Same ladder, same reason, as the quad path.
        let t = kl_vulkan_mtl_for_handle(ov.tex)
            ?? kl_vulkan_layer_mtl_texture(ov.layer_id, ov.stage,
                                            Int32(viewIndices.first ?? 0), &w, &h)
            ?? kl_glfb_layer_mtl_texture(ov.layer_id, ov.stage, &w, &h)
        guard let t else {
            if !loggedOverlayMiss.contains(ov.layer_id) {
                loggedOverlayMiss.insert(ov.layer_id)
                NSLog("[cp] equirect overlay layer \(ov.layer_id) (stage "
                      + "\(ov.stage)) has no MTLTexture — not composited")
            }
            return false
        }
        let texture = Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue()
        var uniforms: [KLEqUniform] = []
        for vi in viewIndices {
            let proj = drawable.computeProjection(viewIndex: vi)
            let originFromView = originFromHead
                * Self.headFromView(drawable.views[vi].transform,
                                    midpoint: eyeMidpointInDevice)
            var u = KLEqUniform()
            u.invProj = proj.inverse
            u.worldRot = Self.rotationOnly(originFromView)
            u.flipY = Self.eqFlipY ? 1 : 0
            u.srgbDecode = Self.eqSrgbDecode ? 1 : 0
            u.visible = 1
            u.test = Self.eqTest
            // A layer that states how much of the turn it spans (an OpenXR
            // equirect layer does) is drawn as stated; the default is for the
            // ones that cannot say.
            u.hscale = ov.eq_hscale > 0 ? ov.eq_hscale : Self.eqHScale
            uniforms.append(u)
        }
        enc.setRenderPipelineState(pipe)
        if let equirectDepthState { enc.setDepthStencilState(equirectDepthState) }
        enc.setFragmentTexture(texture, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        uniforms.withUnsafeBytes { buf in
            enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
        }
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        if !loggedOverlayDraw.contains(ov.layer_id) {
            loggedOverlayDraw.insert(ov.layer_id)
            NSLog("[cp] compositing EQUIRECT panorama layer \(ov.layer_id): "
                  + "\(w)x\(h) tex, flipY=\(Self.eqFlipY) srgb=\(Self.eqSrgbDecode)")
        }
        return true
    }

    // The host-driven loading skybox player (AC Nexus). Created lazily the
    // first frame an Equirect loading layer is live, torn down when it clears.
    private var loadingVideo: LoadingVideoPlayer?
    private var loadingVideoTried = false
    private var loadingVideoSourceGen: UInt32 = 0
    private var loadingSkyboxDraws = 0

    /// The current video frame as an MTLTexture, creating the player on first
    /// use. The loading videos are staged beside the rest of the asset tree, so
    /// the path comes from kl_app_assets_dir(). nil until a frame is decoded.
    private func loadingVideoTexture() -> MTLTexture? {
        // DIAGNOSTIC (KL_SCRATCH_DEBUG=1): draw the wild-handle scratch — the one
        // image AC Nexus actually composites its frame into — as the skybox, so
        // we can SEE what the guest produced. Spread over the sphere it is
        // distorted, but it answers whether the guest's real content is in there.
        if ProcessInfo.processInfo.environment["KL_SCRATCH_DEBUG"] == "1" {
            var sw: Int32 = 0, sh: Int32 = 0
            if let t = kl_vulkan_wild_scratch_mtl(&sw, &sh) {
                return Unmanaged<MTLTexture>.fromOpaque(t).takeUnretainedValue()
            }
        }
        // The video the guest itself asked to play onto its Equirect layer (an
        // Unreal title's GameActivity.playVideo, kl_jni_ue4.c — Medal of Honor's
        // 360 gallery films) comes first, and a new one replaces the one
        // playing. With sound: it is the film itself, narration included, where
        // AC Nexus's backdrop below is a silent loop the game never names.
        var src = [CChar](repeating: 0, count: 1024)
        let gen = kl_loadingvideo_source(&src, Int32(src.count))
        if gen != loadingVideoSourceGen {
            loadingVideoSourceGen = gen
            loadingVideo?.stop(); loadingVideo = nil; loadingVideoTried = false
            let path = String(cString: src)
            if !path.isEmpty {
                loadingVideoTried = true
                if let lv = LoadingVideoPlayer(device: device, url: URL(fileURLWithPath: path), muted: false) {
                    NSLog("[cp] loading skybox: playing the game's video \((path as NSString).lastPathComponent)")
                    lv.start()
                    loadingVideo = lv
                }
            }
        }
        if loadingVideo == nil {
            if loadingVideoTried { return nil }
            loadingVideoTried = true
            guard let dir = kl_app_assets_dir() else { return nil }
            let base = String(cString: dir)
            // The init loop is the long warmup environment; fall back to the
            // shorter background loop if the build ships only that.
            let candidates = ["Background-with-Initialisation-Loop.mp4",
                              "Background-Looping.mp4"]
            var found: URL? = nil
            for c in candidates {
                let path = base + "/" + c
                if FileManager.default.fileExists(atPath: path) {
                    found = URL(fileURLWithPath: path); break
                }
            }
            guard let url = found,
                  let lv = LoadingVideoPlayer(device: device, url: url) else {
                NSLog("[cp] loading skybox: no playable loading video under \(base)")
                return nil
            }
            NSLog("[cp] loading skybox: playing \(url.lastPathComponent)")
            lv.start()
            loadingVideo = lv
        }
        return loadingVideo?.latestTexture()
    }

    /// Draw a video frame as a 360 equirect environment filling every view -
    /// the same fullscreen-triangle equirect pass the guest-overlay path uses,
    /// but sourced from our own player instead of a (missing) guest texture.
    private func drawLoadingSkybox(_ enc: MTLRenderCommandEncoder,
                                   viewIndices: [Int],
                                   drawable: KLDrawable,
                                   layered: Bool,
                                   originFromHead: simd_float4x4,
                                   texture: MTLTexture,
                                   sampler: MTLSamplerState) {
        guard let pipe = equirectPipeline else { return }
        // Same viewport + amplification wiring the eye pass uses, so the
        // fullscreen triangle covers both eyes in the layered (amplified) path
        // rather than only view 0.
        enc.setViewports(viewIndices.map { drawable.views[$0].textureMap.viewport })
        if layered && viewIndices.count > 1 {
            var mappings = viewIndices.enumerated().map { (n, vi) in
                MTLVertexAmplificationViewMapping(
                    viewportArrayIndexOffset: UInt32(n),
                    renderTargetArrayIndexOffset: UInt32(drawable.views[vi].textureMap.sliceIndex))
            }
            enc.setVertexAmplificationCount(viewIndices.count, viewMappings: &mappings)
        }
        var uniforms: [KLEqUniform] = []
        for vi in viewIndices {
            let proj = drawable.computeProjection(viewIndex: vi)
            let originFromView = originFromHead
                * Self.headFromView(drawable.views[vi].transform,
                                    midpoint: eyeMidpointInDevice)
            var u = KLEqUniform()
            u.invProj = proj.inverse
            u.worldRot = Self.rotationOnly(originFromView)
            u.flipY = Self.eqFlipY ? 1 : 0
            u.srgbDecode = Self.eqSrgbDecode ? 1 : 0
            u.visible = 1
            u.test = Self.eqTest
            u.hscale = Self.eqHScale
            uniforms.append(u)
        }
        enc.setRenderPipelineState(pipe)
        if let loadingSkyboxDepthState { enc.setDepthStencilState(loadingSkyboxDepthState) }
        enc.setFragmentTexture(texture, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        uniforms.withUnsafeBytes { buf in
            enc.setVertexBytes(buf.baseAddress!, length: buf.count, index: 0)
            enc.setFragmentBytes(buf.baseAddress!, length: buf.count, index: 0)
        }
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        loadingSkyboxDraws += 1
        if loadingSkyboxDraws == 1 || loadingSkyboxDraws % 120 == 0 {
            NSLog("[cp] loading skybox: drew \(loadingSkyboxDraws) frames "
                  + "(\(texture.width)x\(texture.height), amp=\(viewIndices.count), "
                  + "TEST=\(Self.eqTest), FLIP=\(Self.eqFlipY), SRGB=\(Self.eqSrgbDecode))")
        }
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
                             drawable: KLDrawable) {
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
    /// The composite buffers' GPU timing since the last report, and a reset.
    private func gpuSummary() -> String {
        cmdLock.lock()
        let n = gpuTimedN
        let q = gpuQueuedSum, qm = gpuQueuedMax, r = gpuRanSum, rm = gpuRanMax
        gpuTimedN = 0; gpuQueuedSum = 0; gpuQueuedMax = 0; gpuRanSum = 0; gpuRanMax = 0
        cmdLock.unlock()
        guard n > 0 else { return "" }
        return String(format: ", composite GPU queued %.1f ms mean / %.1f max, "
                              + "ran %.1f / %.1f",
                      q / Double(n) * 1000, qm * 1000, r / Double(n) * 1000, rm * 1000)
    }

    private func cadenceSummary() -> String {
        guard cadenceN > 0 else { return "cadence n/a (no presents yet)" }
        let meanS = cadenceSum / Double(cadenceN)
        let periods = displayPeriod > 0 ? meanS / displayPeriod : 0
        let out = String(format: "cadence %.2f ms mean / %.2f max", meanS * 1000,
                         cadenceMax * 1000)
            + (displayPeriod > 0 ? String(format: " = %.2f periods", periods) : "")
            + ", \(cadenceRepeats)/\(cadenceN) held >=1.5, "
            + "\(missedDeadline) past deadline"
            + gpuSummary()
            // Only when it has happened: a permanently-zero clause on the one
            // line known to print is noise, and a NON-zero one here is the
            // tracker refusing the instant the composite asked about — which is
            // the quad going head-locked, named. See displayOriginFromDevice.
            + (anchorWalkedBack + anchorHeld + anchorUnknown > 0
               ? ", anchor \(anchorWalkedBack) walked back"
                 + " / \(anchorHeld) held / \(anchorUnknown) unknown"
               : "")
        // The latch is a BOOT measurement, and visionOS reserves the right to
        // change the panel rate mid-session — a demotion to 59.98 Hz has been
        // seen on this hardware. That recreates the mismatch at a point where
        // nothing can be re-told: the guest read its rate once, early, and
        // stored it. Detection is therefore the whole of what is available, and
        // it is still worth having, because the alternative is diagnosing the
        // same thing from scratch off a log that never remarked on it. Three
        // consecutive reports — six seconds — of the cadence disagreeing with
        // the latched period by more than 10% is not a hitch.
        if displayPeriod > 0 && cadenceN >= 30 && abs(periods - 1.0) > 0.10 {
            cadenceDisagreed += 1
            if cadenceDisagreed == 3 {
                NSLog(String(format: "[cp] THE PANEL IS NO LONGER RUNNING AT THE RATE WE "
                             + "LATCHED. We latched %.2f Hz at boot and told the guest "
                             + "%.2f Hz; the last six seconds present every %.2f ms, "
                             + "i.e. %.2f Hz. The guest cannot be re-told — it read its "
                             + "rate once, at boot — so its pacing is now incoherent with "
                             + "the panel and motion will judder. Treat any smoothness "
                             + "verdict from here on as confounded.",
                             1.0 / displayPeriod, kl_ovrp_display_frequency(),
                             meanS * 1000, meanS > 0 ? 1.0 / meanS : 0))
            }
        } else {
            cadenceDisagreed = 0
        }
        cadenceN = 0; cadenceSum = 0; cadenceMax = 0
        cadenceRepeats = 0; missedDeadline = 0
        anchorWalkedBack = 0; anchorHeld = 0; anchorUnknown = 0
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
    /// The same through the map's SECOND layer: the right eye of a guest whose
    /// map carries a curve per eye (updateGuestRateMap).
    private static let screenToPhysicalLayer1: kl_reproject_s2p = { ctx, sx, sy, px, py in
        guard let ctx,
              let map = Unmanaged<AnyObject>.fromOpaque(ctx)
                  .takeUnretainedValue() as? MTLRasterizationRateMap else { return }
        let p = map.physicalCoordinates(
            screenCoordinates: MTLSamplePosition(x: sx, y: sy), layer: map.layerCount > 1 ? 1 : 0)
        px?.pointee = p.x
        py?.pointee = p.y
    }
    /// Does this map store the two eyes differently? Asked of the map itself —
    /// a quarter of the way across, where two mirrored curves disagree most —
    /// so the answer cannot drift from what the guest was rasterized through.
    private static func layersDiffer(_ map: MTLRasterizationRateMap) -> Bool {
        guard map.layerCount > 1 else { return false }
        let at = MTLSamplePosition(x: Float(map.screenSize.width) * 0.25, y: 0)
        return abs(map.physicalCoordinates(screenCoordinates: at, layer: 0).x
                   - map.physicalCoordinates(screenCoordinates: at, layer: 1).x) > 0.5
    }

    private var gridBuffer: MTLBuffer?
    private var gridMap: UnsafeMutableRawPointer?
    private var gridTexW = -1, gridTexH = -1
    private var gridUV = SIMD2<Float>(-1, -1)
    private var gridVertexCount = 0
    private var gridViewport: SIMD4<Float> = .zero
    /// Eye 1's rect, and whether the built grid carries a block for it. Part of
    /// the cache key: a guest that starts using symmetric projection partway
    /// through (BONELAB does — the plugin turns it on once the eye textures are
    /// sized) changes only this one, and a grid keyed on eye 0's alone would
    /// never be rebuilt.
    private var gridViewportR: SIMD4<Float> = .zero
    private var gridPerEye = false
    /// The grid before the current one, with its key. Frame generation alternates two pictures of
    /// different extents (the game's eye texture and the generated copy of its drawn part), and a
    /// cache of one rebuilt a 393 216-vertex grid on the CPU every frame (70 Hz, headset 2026-10-28).
    private struct GridEntry {
        var map: UnsafeMutableRawPointer?; var tw: Int; var th: Int; var uv: SIMD2<Float>
        var vp: SIMD4<Float>; var vpR: SIMD4<Float>; var buffer: MTLBuffer?; var count: Int; var perEye: Bool
    }
    private var gridAlt: GridEntry?
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
                            viewportOf: SIMD2<Float>,
                            topLeft: Bool,
                            uvSize: SIMD2<Float>? = nil) -> (MTLBuffer?, Int, Bool) {
        let map = kl_glfb_eye_rate_map()
        let tw = source?.width ?? 0, th = source?.height ?? 0
        // What a texture coordinate of 1 is, in `source`'s pixels: the whole
        // texture, or the part a MetalFX copy holds (mfxPrepass) — the copy is
        // sampled in place of `source`, and its 0..1 spans only that part.
        let uw = uvSize.map { Float($0.x) } ?? Float(tw)
        let uh = uvSize.map { Float($0.y) } ?? Float(th)
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
        // The rect is measured from the picture's row 0. The grid below is in
        // the pass's own space, and for a picture whose row 0 is its TOP (a
        // Vulkan guest) the shader mirrors v AFTER the grid — over the whole
        // texture. A rect that does not span the texture's height therefore has
        // to be mirrored here, or the pass reads the rows the guest did NOT
        // write: Red Matter 2 (Unreal, dynamic resolution) drew into the top
        // 2244 of 3944 rows and showed a strip of its picture at the top of the
        // view over black, sinking as its resolution rose (owner, headset,
        // 2026-10-13). kl_proj_layer_grid does the same for projection layers.
        // A foveated Vulkan eye WITH a partial viewport is not covered by this:
        // the rate map is asked in the mirrored coordinates.
        // This is the ONLY place the eye rect is mirrored: the frame record
        // carries it as the guest submitted it (KL_OVRP_VIEWPORT_TOP is off).
        // The arithmetic is kl_reproject_viewport_for_pass, which `make
        // reproject` checks on real pixels and the Mac viewer shares.
        func rowZeroAtTop(_ r: SIMD4<Float>) -> SIMD4<Float> {
            var out = r
            withUnsafeMutablePointer(to: &out) {
                $0.withMemoryRebound(to: Float.self, capacity: 4) {
                    kl_reproject_viewport_for_pass($0, Float(th), topLeft ? 1 : 0)
                }
            }
            return out
        }
        let vp = rowZeroAtTop(sanitise(viewport0))
        let vpR = rowZeroAtTop(sanitise(viewport1))
        // Two blocks only when the eyes really disagree. Equal rects — every
        // guest measured here before BONELAB's symmetric projection — take the
        // single-block path unchanged, cache key and all.
        // ...or when the guest's map has a curve per eye: then each eye is read
        // through its own layer, whatever the rects.
        let layered = map.flatMap {
            (Unmanaged<AnyObject>.fromOpaque($0).takeUnretainedValue() as? MTLRasterizationRateMap)
                .map(Self.layersDiffer) } ?? false
        let perEye = vp != vpR || layered
        if vp != vpR, !loggedSplitViewports {
            loggedSplitViewports = true
            NSLog("[cp] the eyes submitted DIFFERENT render viewports "
                  + "(\(Int(vp.x)),\(Int(vp.y)) \(Int(vp.z))x\(Int(vp.w)) vs "
                  + "\(Int(vpR.x)),\(Int(vpR.y)) \(Int(vpR.z))x\(Int(vpR.w))) "
                  + "— the unwarp grid carries a block per eye")
        }
        let cropped = vp.z > 0 && vp.w > 0
        let croppedR = vpR.z > 0 && vpR.w > 0
        let eyes: UInt32 = perEye ? 2 : 1
        let current = GridEntry(map: gridMap, tw: gridTexW, th: gridTexH, uv: gridUV, vp: gridViewport,
                                vpR: gridViewportR, buffer: gridBuffer, count: gridVertexCount, perEye: gridPerEye)
        func matches(_ e: GridEntry) -> Bool {
            e.buffer != nil && e.map == map && e.tw == tw && e.th == th && e.vp == vp && e.vpR == vpR
                && e.uv == SIMD2<Float>(uw, uh)
        }
        if !matches(current), let alt = gridAlt, matches(alt) {
            // The other one of the pair: swap it in instead of building it again.
            gridAlt = current
            gridMap = alt.map; gridTexW = alt.tw; gridTexH = alt.th; gridUV = alt.uv
            gridViewport = alt.vp; gridViewportR = alt.vpR
            gridBuffer = alt.buffer; gridVertexCount = alt.count; gridPerEye = alt.perEye
        }
        if gridBuffer == nil || map != gridMap || tw != gridTexW || th != gridTexH
            || vp != gridViewport || vpR != gridViewportR || uw != gridUV.x || uh != gridUV.y {
            if current.buffer != nil { gridAlt = current }
            gridMap = map; gridTexW = tw; gridTexH = th
            gridUV = SIMD2<Float>(uw, uh)
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
                // Finer than the zones: the map is curved inside one, and a
                // cell per zone was up to 3.3 texels off (kl_reproject.h).
                let cap = Int32(KL_REPROJECT_GRID_CELLS_MAX)
                let refine = Int32(KL_REPROJECT_GRID_REFINE)
                nx = UInt32(max(1, min(cap, zx * refine)))
                ny = UInt32(max(1, min(cap, zy * refine)))
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
                                                        uw, uh,
                                                        layered && e == 1
                                                            ? Self.screenToPhysicalLayer1
                                                            : Self.screenToPhysical, map)
                        }
                    }
                }
                gridBuffer = queue?.device.makeBuffer(bytes: table,
                                                      length: n * MemoryLayout<SIMD2<Float>>.stride)
                NSLog("[cp] unwarp grid \(nx)x\(ny)\(perEye ? " x2 eyes" : "")"
                      + "\(layered ? " (a map layer per eye)" : "") for a "
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
                                                        uw, uh, nil, nil)
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

    private func pushEyeTextureSize(_ drawable: KLDrawable) {
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
        // KL_CP_EYE_AT_FULL=1: the guest is offered the display's size at FULL
        // render quality whatever quality the compositor runs at. The size
        // above shrinks with the quality, and a guest that is offered less
        // draws less — which is not what a lower compositor quality is asked
        // for by a game whose picture size is chosen separately (Alyx: the
        // offer is capped at a width below the display's anyway).
        let q = KleptonStageConfiguration.requestedQuality
        if q > 0.05, q < 1, klEnvOn("KL_CP_EYE_AT_FULL", default: false) {
            NSLog("[cp] eye size offered as at full quality: \(w)x\(h) at quality \(q) -> "
                  + "\(Int(Float(w) / q))x\(Int(Float(h) / q))")
            w = Int(Float(w) / q); h = Int(Float(h) / q)
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
        var anchor = KleptonCompositor.usable(tracker.queryDeviceAnchor(atTimestamp: t))
        var steps = 0
        while anchor == nil && steps < 10 {
            t -= 0.005
            anchor = KleptonCompositor.usable(tracker.queryDeviceAnchor(atTimestamp: t))
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
    nonisolated(unsafe) static var loggedEyeMidpoint = false

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

    private func pushEyeOffsets(_ drawable: KLDrawable) {
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
            // Once, not per frame: this fires on every drawable and the value is
            // ~constant, so an unconditional NSLog is thousands of writes/sec on
            // the render thread — a frame-pacing / watchdog hazard.
            if !Self.loggedEyeMidpoint {
                Self.loggedEyeMidpoint = true
                NSLog(String(format: "[cp] eye midpoint (%.4f, %.4f, %.4f) m from the "
                                     + "device anchor — head space is this, not the anchor "
                                     + "(KL_HEAD_MIDPOINT=0 restores the anchor)",
                             eyeMidpointInDevice.x, eyeMidpointInDevice.y,
                             eyeMidpointInDevice.z))
            }
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
        let layerRenderer = self.layerRenderer!
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
        let layerRenderer = self.layerRenderer!
        var measuredViews: [KLDrawable.View]?
        var stamps: [TimeInterval] = []
        var tangents: [SIMD4<Float>] = []

        // The settle run: a stretch of presentations that all agree on one
        // period. `runUnits` is how many PANEL SLOTS the run spans (a held
        // frame contributes 2). The span/slots mean is the run's own coarse
        // reference for deciding membership; the LATCHED value is the median of
        // the per-slot gaps, for the reason set out beside the knobs above.
        var runStart:  TimeInterval = 0
        var runEnd:    TimeInterval = 0
        var runUnits   = 0.0
        var runGaps    = 0
        var runUnitGaps: [TimeInterval] = []
        // ...and what was thrown away getting there, which is the interesting
        // half of the report: it is the rate a fixed-length measurement would
        // have latched instead.
        var deadSpan:  TimeInterval = 0
        var deadUnits  = 0.0
        var deadGaps   = 0
        var latched:   TimeInterval = 0
        var latchedMean: TimeInterval = 0
        var latchedGaps = 0
        var latchedMs  = 0.0
        let clockNow: () -> TimeInterval = {
            LayerRenderer.Clock.Instant.epoch
                .duration(to: LayerRenderer.Clock().now).timeInterval
        }
        let t0 = clockNow()

        while true {
            if hzSettle {
                if latched > 0 { break }
                if (clockNow() - t0) * 1000 >= hzSettleTimeoutMs { break }
            } else if stamps.count >= 8 {
                // Eight frames: enough for a median frame interval that a slow
                // first frame cannot drag, and under 100 ms of startup. This is
                // the pre-settle behaviour, kept byte-for-byte behind
                // KL_HZ_SETTLE=0 so the gate has a one-launch A/B.
                break
            }
            guard layerRenderer.state == .running,
                  let frame = layerRenderer.queryNextFrame() else { break }
            frame.startUpdate()
            frame.endUpdate()
            let timing = frame.predictTiming()
            if let timing { LayerRenderer.Clock().wait(until: timing.optimalInputTime) }
            // Drawable first, THEN startSubmission — the documented order, and
            // the same correction as renderFrame(). These are the layer's first
            // frames, so getting the sequence wrong here is not a local
            // matter: it is the state the whole session starts from.
            let all = frame.queryDrawables()
            guard let drawable = all.first else { continue }
            frame.startSubmission()
            let stamp = LayerRenderer.Clock.Instant.epoch
                .duration(to: drawable.frameTiming.presentationTime).timeInterval
            let prev = stamps.last ?? 0
            stamps.append(stamp)
            if hzSettle, prev > 0, stamp - prev > 0 {
                let gap = stamp - prev
                let ref = runUnits > 0 ? (runEnd - runStart) / runUnits : 0
                // How many slots this gap spans, if it belongs to the run at
                // all. Held frames (2x, 3x) are the same rate; anything else
                // is a different cadence and starts the run over.
                let mult = ref > 0 ? (gap / ref).rounded() : 0
                let belongs = ref > 0 && mult >= 1 && mult <= 3
                    && abs(gap / mult - ref) <= hzSettleTol * ref
                if belongs {
                    runEnd = stamp
                    runUnits += mult
                    runGaps += 1
                    runUnitGaps.append(gap / mult)
                } else {
                    if runGaps > 0 {
                        deadSpan  += runEnd - runStart
                        deadUnits += runUnits
                        deadGaps  += runGaps
                    }
                    runStart = prev
                    runEnd   = stamp
                    runUnits = 1
                    runGaps  = 1
                    runUnitGaps = [gap]
                }
                if (runEnd - runStart) * 1000 >= hzSettleMs
                    && runGaps >= hzSettleMinGaps && !runUnitGaps.isEmpty {
                    let sorted = runUnitGaps.sorted()
                    latched     = sorted[sorted.count / 2]
                    latchedMean = (runEnd - runStart) / runUnits
                    latchedGaps = runGaps
                    latchedMs   = (runEnd - runStart) * 1000
                }
            }
            if tangents.isEmpty {
                tangents = (0..<drawable.views.count).map {
                    kl_reproject_tangents(drawable.computeProjection(viewIndex: $0))
                }
                let measuring = KLDrawable(drawable)
                pushEyeOffsets(measuring)
                measuredViews = measuring.views
                // Before the guest boots, so it allocates the right swapchain
                // once instead of rebuilding when the size changes under it —
                // and each of those rebuilds used to leak a whole generation
                //.
                pushEyeTextureSize(KLDrawable(drawable))
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

        // For the RealityKit output, which has no drawable to read them from.
        if tangents.count >= 2, let views = measuredViews, views.count >= 2 {
            KLRealityPresent.measured = KLRealityPresent.Geometry(
                midpoint: eyeMidpointInDevice,
                offsets: views.prefix(2).map {
                    let c = $0.transform.columns.3
                    return SIMD3<Float>(c.x, c.y, c.z) - eyeMidpointInDevice
                },
                tangents: Array(tangents.prefix(2)))
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
        let median: TimeInterval? = gaps.isEmpty ? nil : gaps[gaps.count / 2]

        // What the settle gate decided, and — the part that matters when a
        // session goes wrong — what it REFUSED. Discarded frames suggesting
        // 120 Hz in front of a latch at 90 is the whole race, printed.
        var chosen = median
        var chosenGaps = gaps.count
        if hzSettle {
            let deadHz = deadUnits > 0 ? 1.0 / (deadSpan / deadUnits) : 0
            if latched > 0 {
                chosen = latched
                chosenGaps = latchedGaps
                let rawHz = 1.0 / latched
                var snapHz = rawHz
                for c in Self.canonicalHz where abs(rawHz - c) <= hzSnapTol * c {
                    snapHz = c
                    break
                }
                if snapHz != rawHz { chosen = 1.0 / snapHz }
                NSLog(String(format: "[cp] display rate SETTLED at %.3f Hz after %.0f ms "
                             + "of steady cadence (%d intervals in the settled run, "
                             + "median of one-slot gaps; their mean says %.3f Hz); "
                             + "%d pre-settle interval(s) discarded%@%@",
                             rawHz, latchedMs, latchedGaps,
                             latchedMean > 0 ? 1.0 / latchedMean : 0, deadGaps,
                             deadHz > 0 ? String(format: ", and they suggested %.2f Hz",
                                                 deadHz) : "",
                             snapHz != rawHz
                                ? String(format: " — SNAPPED to the panel's %.2f Hz "
                                         + "(within %.1f%%)", snapHz, hzSnapTol * 100)
                                : " — no canonical rate within "
                                  + String(format: "%.1f%%, keeping the measurement",
                                           hzSnapTol * 100)))
            } else {
                NSLog(String(format: "[cp] THE DISPLAY RATE NEVER SETTLED — %.0f ms went by "
                             + "without %.0f ms of cadence agreeing with itself to within "
                             + "%.0f%%, so the guest is being paced off the median of all "
                             + "%d interval(s) instead. Treat any smoothness verdict from "
                             + "this session as confounded.",
                             hzSettleTimeoutMs, hzSettleMs, hzSettleTol * 100, gaps.count))
            }
        }
        if let mid = chosen, mid > 0 {
            displayPeriod = mid          // what the cadence check measures against
            let hz = Float(1.0 / mid)
            NSLog(String(format: "[cp] display %.2f Hz measured over %d frames",
                         hz, chosenGaps))
            kl_ovrp_set_display_frequency(hz)
            // The panel's own rate, for a guest that is paced to every second
            // refresh of it (kl_app.c): the call above is ignored where the rate
            // the guest is TOLD is fixed.
            kl_app_set_panel_hz(hz)
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

    // MARK: - The RealityKit output (KL_RK_PRESENT, a proof of concept)

    /// Frames into the RealityKit target until its space closes.
    private func rkFrames(_ rk: KLRealityTarget) {
        var presented = 0, iterations = 0
        var nextReport = Date()
        var lastTarget: TimeInterval = 0
        var gaps: [TimeInterval] = []
        NSLog("[rk] render loop entered")
        let ticker = KLRealityTicker { [self] target in
            if rk.stopped { return false }
            rk.targetTimestamp = target
            if lastTarget > 0 { gaps.append(target - lastTarget) }
            lastTarget = target
            autoreleasepool { presented += renderFrame() }
            arrayViews.frameDone()
            iterations += 1
            if Date() >= nextReport {
                nextReport = Date().addingTimeInterval(2)
                let sorted = gaps.sorted()
                let hz = sorted.isEmpty ? 0 : 1 / sorted[sorted.count / 2]
                gaps.removeAll(keepingCapacity: true)
                cmdLock.lock(); let done = cmdCompleted; cmdLock.unlock()
                NSLog(String(format: "[rk] alive: %d iters, %d with a picture, %d black, display link %.1f Hz, "
                             + "guest=%d, cmdbuf %d/%d done, layers=%d", iterations, presented, blackFrames,
                             hz, kl_app_guest_state(), done, cmdCommitted, lastProjLayersSeen))
                // The guest is paced to what the display link really delivers.
                if hz > 30, abs(Float(hz) - kl_ovrp_display_frequency()) > 2 {
                    kl_ovrp_set_display_frequency(Float(hz))
                    kl_app_set_panel_hz(Float(hz))
                    displayPeriod = 1 / hz
                    NSLog(String(format: "[rk] display link runs at %.1f Hz — guest told so", hz))
                }
            }
            return true
        }
        ticker.run(preferredHz: KLRealityPresent.hz)
        NSLog("[rk] RealityKit frames ended after \(presented) presented frames")
    }

    /// One frame into the RealityKit target.
    private func rkRenderFrame(_ rk: KLRealityTarget) -> Int {
        let presentation = rk.targetTimestamp + KLRealityPresent.latency
        updatePoses(at: presentation)
        publishToGuest()
        guard let cmd = queue?.makeCommandBuffer(), pipeline != nil, sampler != nil else {
            bailNoPipeline += 1
            return 0
        }
        // The pose this picture is drawn for, and the quad's corners for it,
        // in one command buffer with the picture.
        let anchor = displayAnchor(at: presentation)
        let color = rk.beginFrame(cmd, pose: anchor?.originFromAnchorTransform)
        let d = KLDrawable(views: rk.views, projections: rk.projections, color: [color], depth: [rk.depth],
                           presentation: presentation, deadline: rk.targetTimestamp)
        return composeFrame([d], cmd: cmd, raw: [], presetAnchor: anchor, hasPresetAnchor: true) { cmd in
            rk.finishFrame(color, cmd)
        }
    }

}


/// Plays one of AC Nexus's loading mp4s (HEVC) and hands the compositor the
/// current frame as an MTLTexture. This is the visionOS stand-in for the Android
/// MediaPlayer the game drives its OVROverlay video background with: there is no
/// Android Surface here, so the host decodes the same file itself and the
/// compositor draws it as the equirect skybox. Looping, muted (the guest owns
/// audio), and single-item so the video output stays attached across loops.
final class LoadingVideoPlayer {
    private let url: URL
    private var player: AVPlayer?
    private var output: AVPlayerItemVideoOutput?
    private var cache: CVMetalTextureCache?
    // Held so the CVPixelBuffer behind `current` stays alive while the GPU
    // samples it; replaced, not accumulated, each new frame.
    private var retained: CVMetalTexture?
    private var current: MTLTexture?
    private var endObserver: NSObjectProtocol?
    private let muted: Bool

    init?(device: MTLDevice, url: URL, muted: Bool = true) {
        self.url = url
        self.muted = muted
        var c: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &c)
                == kCVReturnSuccess, let c else { return nil }
        self.cache = c
    }

    func start() {
        guard player == nil else { return }
        let item = AVPlayerItem(url: url)
        let attrs: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferPixelFormatTypeKey as String: Int(kCVPixelFormatType_32BGRA),
        ]
        let out = AVPlayerItemVideoOutput(outputSettings: attrs)
        item.add(out)
        output = out
        let p = AVPlayer(playerItem: item)
        p.isMuted = muted
        p.actionAtItemEnd = .none
        endObserver = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main) { [weak p] _ in
            p?.seek(to: .zero)
            p?.play()
        }
        player = p
        p.play()
    }

    /// The newest decoded frame, or the last one if none is ready this vsync.
    func latestTexture() -> MTLTexture? {
        guard let output, let cache else { return current }
        let host = CACurrentMediaTime()
        let t = output.itemTime(forHostTime: host)
        guard t.isValid, output.hasNewPixelBuffer(forItemTime: t),
              let pb = output.copyPixelBuffer(forItemTime: t, itemTimeForDisplay: nil) else {
            return current
        }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
        var cvtex: CVMetalTexture?
        let r = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pb, nil, .bgra8Unorm, w, h, 0, &cvtex)
        if r == kCVReturnSuccess, let cvtex, let mtl = CVMetalTextureGetTexture(cvtex) {
            retained = cvtex
            current = mtl
        }
        return current
    }

    func stop() {
        player?.pause()
        player = nil
        output = nil
        current = nil
        retained = nil
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        endObserver = nil
        if let cache { CVMetalTextureCacheFlush(cache, 0) }
    }
}
