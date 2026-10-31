import Metal

/// The alpha of a MetalFX copy, put back from the picture it was made from.
///
/// Apple's spatial scaler upscales colour only: whatever alpha came in, every
/// pixel of its output has alpha 1 (measured on a Mac for rgba8Unorm,
/// rgba8Unorm_srgb, bgra8Unorm and rgba16Float — an input pixel (0,0,0,0)
/// comes out (0,0,0,1)). In the guest's own mixed reality the eye picture's
/// alpha says where the room shows (kl_reproject.c, kl_eye_sample_alpha and
/// the discard of empty pixels), so the copy covered the room with black.
///
/// After the scaler, one draw per slice over the copy writes alpha only
/// (colour write mask `.alpha`, the scaler's colour stays): the source's alpha,
/// sampled bilinearly from the part of it the scaler read (`content`, top
/// left) at the copy's pixel centres. The same alpha the linear path would
/// have read there, so an edge is as soft as without MetalFX, and whatever
/// the guest's alpha means (kl_reproject.h, KL_EYE_ALPHA_*) is left to the
/// composite. Every format the compositor upscales is a colour render target,
/// sRGB included — and the alpha of an sRGB format is linear in both.
///
/// The copy needs `.renderTarget` (mfxMakeTarget adds it). t_mfxalpha runs
/// this with the real scaler on a Mac. Not thread-safe: the compositor's
/// render thread is the only caller.
final class KleptonMetalFXAlpha {
    private let device: MTLDevice
    private var library: MTLLibrary?
    private var libraryFailed = false
    private var pipelines: [MTLPixelFormat: MTLRenderPipelineState] = [:]
    /// Told once what failed to build, if anything did.
    var onError: ((String) -> Void)?

    init(device: MTLDevice) { self.device = device }

    private static let source = """
    #include <metal_stdlib>
    using namespace metal;

    struct KLAlphaOut { float4 pos [[position]]; };

    vertex KLAlphaOut kl_mfx_alpha_v(uint vid [[vertex_id]])
    {
        // One triangle over the whole target.
        float2 p = float2((vid << 1) & 2, vid & 2);
        KLAlphaOut o;
        o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
        return o;
    }

    // k.xy: from the copy's pixels to the source's texture coordinates;
    // k.zw: the last texel centre inside the content, so the edge of a
    // content smaller than its texture does not reach past it.
    fragment float4 kl_mfx_alpha_f(KLAlphaOut in [[stage_in]],
                                   texture2d<float> src [[texture(0)]],
                                   constant float4 &k [[buffer(0)]])
    {
        constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
        float2 uv = min(in.pos.xy * k.xy, k.zw);
        return float4(0.0, 0.0, 0.0, src.sample(s, uv).a);
    }
    """

    /// The pipeline that writes alpha into a target of `format`, built on
    /// first use; nil if it cannot be (said once through `onError`).
    func pipeline(for format: MTLPixelFormat) -> MTLRenderPipelineState? {
        if let p = pipelines[format] { return p }
        if library == nil, !libraryFailed {
            do { library = try device.makeLibrary(source: Self.source, options: nil) }
            catch { libraryFailed = true; onError?("alpha shader: \(error)") }
        }
        guard let lib = library,
              let v = lib.makeFunction(name: "kl_mfx_alpha_v"),
              let f = lib.makeFunction(name: "kl_mfx_alpha_f") else { return nil }
        let d = MTLRenderPipelineDescriptor()
        d.label = "klepton metalfx alpha"
        d.vertexFunction = v
        d.fragmentFunction = f
        d.colorAttachments[0].pixelFormat = format
        d.colorAttachments[0].writeMask = .alpha
        do {
            let p = try device.makeRenderPipelineState(descriptor: d)
            pipelines[format] = p
            return p
        } catch {
            onError?("alpha pipeline for format \(format.rawValue): \(error)")
            return nil
        }
    }

    /// Writes `input`'s alpha into `output` (one 2D slice of the copy) on
    /// `cmd`, after the scaler that wrote `output`'s colour. `content` is the
    /// part of `input` the scaler read, in its pixels. False if nothing could
    /// be encoded — the copy then still has the scaler's alpha 1.
    @discardableResult
    func encode(on cmd: MTLCommandBuffer, from input: MTLTexture,
                content: (w: Int, h: Int), into output: MTLTexture) -> Bool {
        guard output.usage.contains(.renderTarget),
              let p = pipeline(for: output.pixelFormat) else { return false }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = output
        pass.colorAttachments[0].loadAction = .load
        pass.colorAttachments[0].storeAction = .store
        guard let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return false }
        enc.label = "klepton metalfx alpha"
        enc.setRenderPipelineState(p)
        let iw = Float(input.width), ih = Float(input.height)
        var k = SIMD4<Float>(Float(content.w) / (Float(output.width) * iw),
                             Float(content.h) / (Float(output.height) * ih),
                             (Float(content.w) - 0.5) / iw,
                             (Float(content.h) - 0.5) / ih)
        enc.setFragmentTexture(input, index: 0)
        enc.setFragmentBytes(&k, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        return true
    }
}
