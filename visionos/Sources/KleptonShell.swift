import SwiftUI
import UIKit
import Metal
import QuartzCore

// Steam Link's 2D configuration shell, in a visionOS window.
//
// This is the other half of PLANNING §11.9's front-door pair. The VR half runs
// in an ImmersiveSpace (KleptonCompositor) and cannot draw anything without an
// authorized session; the shell is what *gets* one — it lists the hosts on the
// LAN, takes the PIN, and hands off. On the host that is `kl_view.c`, SDL3 and
// macOS-only, so none of it was available here.
//
// What a flat guest needs from a frontend is small and kl_mono.h is the whole
// of it: the newest presented frame going out, and a pointer and a keyboard
// coming in. Both cross as plain C, and kl_view.c calls the same two functions,
// so there is one description of "what a click is" rather than a host one and a
// device one that will disagree.
//
// **The frame path is the READBACK, deliberately.** The zero-copy route the
// stereo path takes (kl_glfb's eye MTLTextures, sampled by the compositor)
// depends on eye textures keyed by (eye, stage), and a flat guest has none of
// that: no ovrp, no eye textures, no stages. Its picture is the default
// framebuffer of an EGL window surface. At 1280x800 that is 4 MB a frame — a
// real cost, and the right one to pay for a configuration UI that exists for
// about a minute. Giving it a zero-copy route means backing the window surface
// with an EGLImage-bound MTLTexture, which is the same interop §12.9 already
// built for the eye textures and is worth doing when the shell is the thing
// being optimised, not before.

// MARK: - the Metal view

/// A `CAMetalLayer` that samples the guest's readback buffer.
///
/// A UIView rather than a SwiftUI `Canvas` or an `Image`: the frame arrives as
/// raw bytes at up to display rate, and rebuilding a `CGImage` per frame to
/// hand SwiftUI is a copy and a decode that a texture upload does not need.
final class KleptonShellView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipeline: MTLRenderPipelineState?
    private var sampler: MTLSamplerState?
    private var texture: MTLTexture?
    private var texW = 0, texH = 0
    private var lastSerial: UInt32 = 0
    private var link: CADisplayLink?
    private var loggedFirstFrame = false

    // The guest's surface size, as the last frame reported it. The pointer
    // scaling below is the whole reason this is kept: a click has to land where
    // the person aimed it, and the view's points are not the guest's pixels.
    private(set) var guestW = 0, guestH = 0

    init?(frame: CGRect, device: MTLDevice) {
        guard let q = device.makeCommandQueue() else { return nil }
        self.device = device
        self.queue = q
        super.init(frame: frame)

        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.isOpaque = true

        do {
            let lib = try device.makeLibrary(source: Self.shaderSource, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "shell_v")
            d.fragmentFunction = lib.makeFunction(name: "shell_f")
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try device.makeRenderPipelineState(descriptor: d)
        } catch {
            NSLog("[shell] pipeline FAILED: \(error)")
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear
        sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge
        sd.tAddressMode = .clampToEdge
        sampler = device.makeSamplerState(descriptor: sd)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil, link == nil {
            let l = CADisplayLink(target: self, selector: #selector(tick))
            l.add(to: .main, forMode: .common)
            link = l
        } else if window == nil {
            link?.invalidate()
            link = nil
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        // contentScaleFactor, not a screen: visionOS has no UIScreen at all —
        // a window is placed in space and the system decides its backing scale,
        // and there is nothing to ask for a nativeScale.
        let scale = contentScaleFactor
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: bounds.width * scale,
                                         height: bounds.height * scale)
    }

    @objc private func tick() {
        // Take the frame under kl_mono's lock and let go immediately: the sink
        // that fills it runs INSIDE the guest's frame, so anything held here is
        // guest frame time.
        var rgba: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, serial: UInt32 = 0
        guard kl_mono_frame_lock(&rgba, &w, &h, &serial) != 0 else { return }
        defer { kl_mono_frame_unlock() }
        guard let src = rgba, w > 0, h > 0 else { return }

        guestW = Int(w); guestH = Int(h)

        if serial == lastSerial, texture != nil {
            // Nothing new. Still present, because the layer's drawable is not
            // ours to keep and a window that stops presenting goes blank on
            // resize.
            present()
            return
        }
        lastSerial = serial

        if texture == nil || texW != Int(w) || texH != Int(h) {
            let td = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: Int(w), height: Int(h), mipmapped: false)
            td.usage = .shaderRead
            td.storageMode = .shared
            texture = device.makeTexture(descriptor: td)
            texW = Int(w); texH = Int(h)
            NSLog("[shell] guest surface \(w)x\(h)")
        }
        guard let tex = texture else { return }
        tex.replace(region: MTLRegionMake2D(0, 0, Int(w), Int(h)),
                    mipmapLevel: 0,
                    withBytes: src,
                    bytesPerRow: Int(w) * 4)
        if !loggedFirstFrame {
            loggedFirstFrame = true
            NSLog("[shell] first guest frame presented (\(w)x\(h))")
        }
        present()
    }

    private func present() {
        guard let pipe = pipeline, let tex = texture, let smp = sampler,
              let drawable = metalLayer.nextDrawable(),
              let cmd = queue.makeCommandBuffer() else { return }

        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = drawable.texture
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
        rp.colorAttachments[0].storeAction = .store
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: rp) else { return }
        enc.setRenderPipelineState(pipe)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(smp, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }

    // A fullscreen triangle. The V flip is in the texture coordinate and not in
    // the upload, which is what makes the readback free of a second pass: rows
    // arrive bottom-up because that is how glReadPixels produced them
    // (kl_mono.h), and a GPU consumer gets the flip for nothing.
    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut shell_v(uint vid [[vertex_id]]) {
        float2 p = float2((vid << 1) & 2, vid & 2);
        VOut o;
        o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
        o.uv  = float2(p.x, p.y);          // v not flipped: the source is bottom-up
        return o;
    }
    fragment float4 shell_f(VOut in [[stage_in]],
                            texture2d<float> src [[texture(0)]],
                            sampler smp [[sampler(0)]]) {
        return float4(src.sample(smp, in.uv).rgb, 1.0);
    }
    """
}

// MARK: - SwiftUI wrapper, and the input half

struct ShellSurface: UIViewRepresentable {
    /// Filled in by makeUIView so the gesture handlers can scale into guest
    /// pixels. A class box because a struct View is recreated constantly.
    final class Box { weak var view: KleptonShellView? }
    let box: Box

    func makeUIView(context: Context) -> UIView {
        guard let device = MTLCreateSystemDefaultDevice(),
              let v = KleptonShellView(frame: .zero, device: device) else {
            NSLog("[shell] no Metal device — the shell window will be empty")
            return UIView()
        }
        box.view = v
        return v
    }

    func updateUIView(_ uiView: UIView, context: Context) {}
}

/// The window's contents while Steam Link's shell is the thing running.
///
/// Pointer and keys go through kl_mono, i.e. the guest's own Android input path
/// (`SDLActivity.onNativeMouse` / `onNativeKeyDown` / `onNativeKeyUp`). Nothing
/// is invented here: we author the Java side, so the events arrive exactly as
/// SDLSurface.onTouch would have sent them and SDL's own Android backend does
/// the translating.
struct ShellWindow: View {
    @State private var box = ShellSurface.Box()
    @State private var typed = ""
    @FocusState private var keyboardFocused: Bool

    var body: some View {
        ShellSurface(box: box)
            .aspectRatio(CGSize(width: 1280, height: 800), contentMode: .fit)
            // A drag with zero movement is still a tap here, which is what makes
            // one recogniser enough for both: visionOS delivers an indirect
            // pinch as a drag whose location is where the person was looking.
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { g in send(g.location, phase: .down) }
                    .onEnded   { g in send(g.location, phase: .up) }
            )
            // A hidden field is how a windowed visionOS app gets characters at
            // all: there is no key-event stream for a Metal layer, and the
            // system keyboard is attached to a text input. It is the PIN and the
            // host address that need it — see kl_mono_keycode_for_char for the
            // list of keys we are willing to claim.
            .overlay(alignment: .bottom) { keyboardTap }
            .background(.black)
    }

    private enum Phase { case down, up }

    private func send(_ p: CGPoint, phase: Phase) {
        guard let v = box.view, v.guestW > 0, v.guestH > 0 else { return }
        let s = v.bounds.size
        guard s.width > 0, s.height > 0 else { return }
        let x = Float(p.x / s.width) * Float(v.guestW)
        let y = Float(p.y / s.height) * Float(v.guestH)
        switch phase {
        case .down:
            // Hover first, then press. Qt decides what a press lands on from
            // where the pointer already is, so a press with no hover before it
            // is a click the widget under it never sees (measured on the host —
            // the button took its highlight and nothing else).
            kl_mono_pointer(0, KL_MONO_HOVER_MOVE, x, y)
            kl_mono_pointer(Int32(KL_MONO_BTN_PRIMARY), KL_MONO_DOWN, x, y)
        case .up:
            kl_mono_pointer(0, KL_MONO_UP, x, y)
        }
    }

    /// Invisible, and it has to stay in the hierarchy: dismissing it is what
    /// tells the system to put the keyboard away.
    private var keyboardTap: some View {
        TextField("", text: $typed)
            .focused($keyboardFocused)
            .opacity(0.02)
            .frame(width: 1, height: 1)
            .onChange(of: typed) { old, new in
                // Characters, not key events: SwiftUI gives a windowed app the
                // resulting STRING, so a backspace shows up as the field getting
                // shorter. Both directions are translated, because a PIN entry
                // that cannot be corrected is a PIN entry that fails.
                if new.count < old.count {
                    for _ in 0..<(old.count - new.count) { key(8) }   // KEYCODE_DEL
                } else {
                    for ch in new.dropFirst(old.count) { key(Int32(ch.asciiValue ?? 0)) }
                }
                if new.count > 64 { typed = "" }   // it is a keyboard, not a buffer
            }
    }

    private func key(_ ch: Int32) {
        let code = kl_mono_keycode_for_char(ch)
        guard code != 0 else { return }
        kl_mono_key(1, code)
        kl_mono_key(0, code)
    }
}
