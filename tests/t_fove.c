// t_fove — the foveation profile maths (runtime/gfx/kl_fove.m): the
// straight-ahead point from a frustum and the two separable quality curves,
// the MSL patch, the pending-expand and intermediate tables, and — when a Metal
// device with rate-map support is present — GPU self-tests of the encoder
// marking, expands, re-centre and scratch cache. Design and the measurements
// behind the expected numbers: docs/FOVEATION.md.
#include "kl_fove.h"
#include "kl_vulkan.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // the [fove] lines (stderr) interleave in order
    printf("=== foveation profile maths ===\n");
    // KL_OVRP_EYE_TAN=vision, as GetProjectionRaw hands it: L 60 R 45 U 45 D 50 (eye 0).
    float t0[4] = { -tanf(60 * (float)M_PI / 180), tanf(45 * (float)M_PI / 180),
                    -tanf(50 * (float)M_PI / 180), tanf(45 * (float)M_PI / 180) };
    float t1[4] = { -t0[1], -t0[0], t0[2], t0[3] };
    float fx0, fy0, fx1, fy1;
    kl_fove_centre(t0, &fx0, &fy0);
    kl_fove_centre(t1, &fx1, &fy1);
    CHECK(fabsf(fx0 - 0.634f) < 0.002f && fabsf(fy0 - 0.456f) < 0.002f, "eye 0 centre %.4f %.4f", fx0, fy0);
    CHECK(fabsf(fx1 - 0.366f) < 0.002f && fabsf(fy1 - fy0) < 1e-6f, "eye 1 centre %.4f %.4f", fx1, fy1);
    float sym[4] = { -1, 1, -1, 1 }, sx, sy;
    kl_fove_centre(sym, &sx, &sy);
    CHECK(sx == 0.5f && sy == 0.5f, "symmetric frustum centre %.3f %.3f", sx, sy);

    // The map centre from a STALE viewport generation (Into the Radius): the
    // record still says 0,0 5088x4081 of a 5088x4081 texture when the eye
    // images have been re-created at 6784x4528. The union cone it renders with
    // (l 1.7321 r 1.7322 t 1.0001 b 1.1918) puts the point at 3392,2066 in the
    // new image; reading the old rect as pixels put it at 2544,1862.
    {
        float ut[4] = { -1.7321f, 1.7322f, -1.1918f, 1.0001f }, ufx, ufy, ux, uy;
        kl_fove_centre(ut, &ufx, &ufy);
        int stale[4] = { 0, 0, 5088, 4081 }, stale_of[2] = { 5088, 4081 };
        for (int l = 0; l < 2; l++) {
            kl_fove_eye_px(ufx, ufy, 6784, 4528, stale, stale_of, &ux, &uy);
            CHECK(fabsf(ux - 3392) < 1 && fabsf(uy - 2066) < 1,
                  "stale-generation centre (layer %d) %.0f,%.0f, want 3392,2066 (rect 5088x4081 of 5088x4081, "
                  "target 6784x4528)", l, ux, uy);
        }
        // Same generation: unchanged — a half-width rect of this texture moves the point into it.
        int same[4] = { 0, 0, 3392, 2264 }, same_of[2] = { 6784, 4528 };
        kl_fove_eye_px(ufx, ufy, 6784, 4528, same, same_of, &ux, &uy);
        CHECK(fabsf(ux - 1696) < 1 && fabsf(uy - 1033) < 1, "same-generation centre %.0f,%.0f, want 1696,1033", ux, uy);
        int full[4] = { 0, 0, 6784, 4528 };
        kl_fove_eye_px(ufx, ufy, 6784, 4528, full, same_of, &ux, &uy);
        CHECK(fabsf(ux - 3392) < 1 && fabsf(uy - 2066) < 1, "same-generation full rect %.0f,%.0f", ux, uy);
        // No generation recorded: the full image, whatever the rect says.
        int none_of[2] = { 0, 0 };
        kl_fove_eye_px(ufx, ufy, 6784, 4528, stale, none_of, &ux, &uy);
        CHECK(fabsf(ux - 3392) < 1 && fabsf(uy - 2066) < 1, "viewport_of 0 fallback %.0f,%.0f, want 3392,2066", ux, uy);
        // Degenerate rect / no record: the full image.
        int degen[4] = { 0, 0, 0, 4081 };
        kl_fove_eye_px(ufx, ufy, 6784, 4528, degen, stale_of, &ux, &uy);
        CHECK(fabsf(ux - 3392) < 1 && fabsf(uy - 2066) < 1, "degenerate rect %.0f,%.0f", ux, uy);
        kl_fove_eye_px(ufx, ufy, 6784, 4528, NULL, NULL, &ux, &uy);
        CHECK(fabsf(ux - 3392) < 1 && fabsf(uy - 2066) < 1, "no record %.0f,%.0f", ux, uy);
    }

    // Deadpool: asymmetric per-eye render viewports in a
    // 5088 texture (eye0 0,0 4012x3234, eye1 1075,0 4013x3234 of 5088x3234),
    // the map on the 5088x3236 intermediate. The frustum fractions 63.4% /
    // 36.6% of each VIEWPORT both land on column 2544; of the full image (what
    // a map built before the first record used) on 3226 / 1862 —
    // 682 px off, the ramp across the middle of the view.
    {
        int v0[4] = { 0, 0, 4012, 3234 }, v1[4] = { 1075, 0, 4013, 3234 }, of[2] = { 5088, 3234 };
        float x0, y0, x1, y1;
        CHECK(kl_fove_eye_px(fx0, fy0, 5088, 3236, v0, of, &x0, &y0) && kl_fove_eye_px(fx1, fy1, 5088, 3236, v1, of, &x1, &y1),
              "Deadpool viewports refused");
        CHECK(fabsf(x0 - 2544) < 2 && fabsf(x1 - 2544) < 2, "Deadpool centres %.0f / %.0f, want 2544 / 2544", x0, x1);
        CHECK(fabsf(y0 - 1476) < 2, "Deadpool centre y %.0f, want 1476", y0);
        kl_fove_eye_px(fx0, fy0, 5088, 3236, NULL, NULL, &x0, &y0);
        kl_fove_eye_px(fx1, fy1, 5088, 3236, NULL, NULL, &x1, &y1);
        CHECK(fabsf(x0 - 3226) < 2 && fabsf(x1 - 1862) < 2, "Deadpool no-record centres %.0f / %.0f, want 3226 / 1862", x0, x1);
    }

    // Intermediate extent (Deadpool): Unreal's scene target
    // is the eye rounded up, never smaller, never far off.
    CHECK(kl_fove_inter_extent_ok(5088, 3234, 5088, 3234), "exact eye-size intermediate refused");
    CHECK(kl_fove_inter_extent_ok(5088, 3234, 5088, 3236), "5088x3236 intermediate for a 5088x3234 eye refused");
    CHECK(kl_fove_inter_extent_ok(5086, 3234, 5088, 3236), "both axes rounded up refused");
    CHECK(!kl_fove_inter_extent_ok(5088, 3234, 5088, 3300), "5088x3300 accepted for a 5088x3234 eye");
    CHECK(!kl_fove_inter_extent_ok(5088, 3234, 5088, 3230), "5088x3230 (smaller) accepted for a 5088x3234 eye");
    CHECK(!kl_fove_inter_extent_ok(5088, 3234, 5000, 3234), "narrower intermediate accepted");
    CHECK(!kl_fove_inter_extent_ok(0, 0, 16, 16), "no eye image, yet accepted");

    // medium at the headset's scale-65 eye: 5184x2016, box 1024, ramp 384, floor 0.22
    enum { W = 5184, H = 2016, NH = W / 16 + 1, NV = H / 16 + 1 };
    float h[NH], v[NV];
    float cx[2] = { fx0 * W / 2, W / 2 + fx1 * W / 2 }, cy = fy0 * H;
    CHECK(kl_fove_curves(W, H, cx, cy, 1024, 384, 0.22f, NH, h, NV, v), "curves refused good input");
    int full = 0, floor_ = 0, bad = 0;
    double hm = 0, vm = 0;
    for (int k = 0; k < NH; k++) {
        float x = (float)k * W / (NH - 1);
        float c = x < W / 2 ? cx[0] : cx[1];
        float d = fabsf(x - c);
        if (d <= 512 && h[k] != 1.0f) bad++;
        if (d >= 512 + 384 && fabsf(h[k] - 0.22f) > 1e-6f) bad++;
        if (h[k] < 0.22f - 1e-6f || h[k] > 1.0f) bad++;
        full += h[k] == 1.0f;
        floor_ += fabsf(h[k] - 0.22f) < 1e-6f;
        hm += h[k];
    }
    for (int k = 0; k < NV; k++) {
        if (v[k] < 0.22f - 1e-6f || v[k] > 1.0f) bad++;
        vm += v[k];
    }
    hm /= NH; vm /= NV;
    CHECK(!bad, "%d samples outside the profile", bad);
    CHECK(full > 0 && floor_ > 0, "no full-rate (%d) or floor (%d) samples", full, floor_);
    // Two peaks: the curve rises again after the seam.
    int peaks = 0;
    for (int k = 1; k < NH; k++) peaks += h[k] == 1.0f && h[k - 1] < 1.0f;
    CHECK(peaks == 2, "%d peaks in the horizontal curve, want 2", peaks);
    // The conservative separable medium map: 0.634 x 0.753 (Half-Life: Alyx's
    // map re-centred per eye, docs/FOVEATION.md §3.1).
    CHECK(fabs(hm - 0.644) < 0.02 && fabs(vm - 0.765) < 0.02, "mean rates %.3f x %.3f", hm, vm);
    // The nasal seam is at the floor for medium (each eye's ramp ends before it).
    CHECK(fabsf(h[(NH - 1) / 2] - 0.22f) < 1e-6f, "seam sample %.3f", h[(NH - 1) / 2]);
    // Bad input.
    CHECK(!kl_fove_curves(W, H, cx, cy, 1024, 384, 0.0f, NH, h, NV, v), "floor 0 accepted");
    CHECK(!kl_fove_curves(W, H, cx, cy, 1024, 384, 0.2f, 1, h, NV, v), "1 sample accepted");
    CHECK(!kl_fove_curves(0, H, cx, cy, 1024, 384, 0.2f, NH, h, NV, v), "zero width accepted");
    printf("medium 5184x2016: mean rate %.3f x %.3f = %.3f of the fragments (before Metal's 32-px tiles)\n",
           hm, vm, hm * vm);
    // One eye's curves (a one-eye or array-slice target), 2400x2400,
    // high (box 914, ramp 457, floor 0.125), centred off-axis at eye 0's point.
    {
        enum { E = 2400, N = E / 16 + 1 };
        float eh[N], ev[N];
        float ecx = fx0 * E, ecy = fy0 * E;
        CHECK(kl_fove_curves1(E, E, ecx, ecy, 914, 457, 0.125f, N, eh, N, ev), "curves1 refused good input");
        int b1 = 0, pk = 0;
        for (int k = 0; k < N; k++) {
            float x = (float)k * E / (N - 1);
            if (fabsf(x - ecx) <= 457 && eh[k] != 1.0f) b1++;
            if (fabsf(x - ecx) >= 457 + 457 && fabsf(eh[k] - 0.125f) > 1e-6f) b1++;
            if (fabsf(x - ecy) <= 457 && ev[k] != 1.0f) b1++;
            if (k && eh[k] == 1.0f && eh[k - 1] < 1.0f) pk++;
        }
        CHECK(!b1 && pk == 1, "curves1: %d samples off the profile, %d peaks (want 1)", b1, pk);
        CHECK(!kl_fove_curves1(E, E, ecx, ecy, 914, 457, 0.0f, N, eh, N, ev), "curves1 floor 0 accepted");
    }
    // The envelope (kl_fove_curves_env, the default for every eye image and
    // map layer): eye 0's and eye 1's points placed in the same 2400x2400 eye.
    // The curve is the same for either eye, symmetric about the middle, both
    // points at full rate — where the single peak above leaves the eye's outer
    // side at the floor (the left eye blurred over its left half: Alien: Rogue
    // Incursion, toolchain 565).
    {
        enum { E = 2400, N = E / 16 + 1 };
        float e0[N], e1[N], ev[N], s0[N], s1[N], sv[N];
        const float a[2] = { fx0 * E, fx1 * E }, b[2] = { fx1 * E, fx0 * E }, cyy[2] = { fy0 * E, fy1 * E };
        CHECK(kl_fove_curves_env(E, E, a, cyy, 914, 457, 0.125f, N, e0, N, ev), "curves_env refused good input");
        CHECK(kl_fove_curves_env(E, E, b, cyy, 914, 457, 0.125f, N, e1, N, ev), "curves_env refused good input");
        CHECK(kl_fove_curves1(E, E, fx0 * E, fy0 * E, 914, 457, 0.125f, N, s0, N, sv), "curves1 refused good input");
        CHECK(kl_fove_curves1(E, E, fx1 * E, fy1 * E, 914, 457, 0.125f, N, s1, N, sv), "curves1 refused good input");
        int same = 0, sym = 0, pts = 0;
        double l0 = 0, r0 = 0, l1 = 0, r1 = 0, sl0 = 0, sr0 = 0, sl1 = 0, sr1 = 0;
        for (int k = 0; k < N; k++) {
            float x = (float)k * E / (N - 1);
            same += fabsf(e0[k] - e1[k]) > 1e-6f;
            sym += fabsf(e0[k] - e0[N - 1 - k]) > 0.02f;
            if ((fabsf(x - a[0]) < 8 || fabsf(x - a[1]) < 8 || fabsf(x - E / 2) < 8) && e0[k] != 1.0f) pts++;
            if (e0[k] < s0[k] - 1e-6f) pts++;   // never coarser than the single peak
            if (x < E / 2) { l0 += e0[k]; l1 += e1[k]; sl0 += s0[k]; sl1 += s1[k]; }
            else           { r0 += e0[k]; r1 += e1[k]; sr0 += s0[k]; sr1 += s1[k]; }
        }
        CHECK(!same, "curves_env: the two eyes' curves differ at %d samples", same);
        CHECK(!sym, "curves_env: %d samples not mirror-symmetric", sym);
        CHECK(!pts, "curves_env: %d samples coarser than they must be", pts);
        CHECK(fabs(l0 - r0) < 0.02 * N && fabs(l1 - r1) < 0.02 * N, "curves_env: halves %.1f / %.1f, %.1f / %.1f",
              l0, r0, l1, r1);
        // The single peak: the outer half is the coarse one, mirrored between the eyes.
        CHECK(sl0 < 0.8 * sr0 && sr1 < 0.8 * sl1, "curves1: left eye halves %.1f / %.1f, right eye %.1f / %.1f",
              sl0, sr0, sl1, sr1);
        CHECK(!kl_fove_curves_env(E, E, a, cyy, 914, 457, 0.0f, N, e0, N, ev), "curves_env floor 0 accepted");
        printf("envelope, high, 2400x2400 eye: mean horizontal rate left/right half %.3f / %.3f for either eye; "
               "one peak per eye: left eye %.3f / %.3f, right eye %.3f / %.3f\n",
               2 * l0 / N, 2 * r0 / N, 2 * sl0 / N, 2 * sr0 / N, 2 * sl1 / N, 2 * sr1 / N);
    }
    // The [[position]] patch passes the map LAYER — its own argument
    // added, a [[render_target_array_index]] argument reused, a stage_in member
    // reached through the stage_in argument, and a shader without [[position]]
    // left alone.
    {
        const char *a = "#include <metal_stdlib>\nusing namespace metal;\nstruct main0_out { float4 c [[color(0)]]; };\n"
                        "fragment main0_out main0(float4 gl_FragCoord [[position]])\n{\n    main0_out out = {};\n"
                        "    out.c = gl_FragCoord;\n    return out;\n}\n";
        char *pa = kl_fove_patch_for_test(a);
        CHECK(pa && strstr(pa, "uint kl_fove_layer [[render_target_array_index]]") &&
              strstr(pa, "map_physical_to_screen_coordinates(gl_FragCoord.xy, kl_fove_layer)") &&
              strstr(pa, "[[buffer(20)]]"), "plain shader: layer argument not added:\n%s", pa ? pa : "(null)");
        free(pa);
        const char *b = "fragment main0_out main0(float4 gl_FragCoord [[position]], uint gl_Layer [[render_target_array_index]])\n"
                        "{\n    main0_out out = {};\n    return out;\n}\n";
        char *pb2 = kl_fove_patch_for_test(b);
        CHECK(pb2 && !strstr(pb2, "kl_fove_layer") && strstr(pb2, "(gl_FragCoord.xy, gl_Layer)"),
              "layer argument not reused:\n%s", pb2 ? pb2 : "(null)");
        free(pb2);
        const char *c = "struct main0_in { uint gl_ViewIndex [[render_target_array_index]]; };\n"
                        "fragment main0_out main0(main0_in in [[stage_in]], float4 gl_FragCoord [[position]])\n"
                        "{\n    main0_out out = {};\n    return out;\n}\n";
        char *pc = kl_fove_patch_for_test(c);
        CHECK(pc && !strstr(pc, "kl_fove_layer") && strstr(pc, "(gl_FragCoord.xy, in.gl_ViewIndex)"),
              "stage_in layer member not reached:\n%s", pc ? pc : "(null)");
        free(pc);
        const char *d = "fragment main0_out main0(main0_in in [[stage_in]])\n{\n    main0_out out = {};\n    return out;\n}\n";
        char *pd = kl_fove_patch_for_test(d);
        CHECK(!pd, "a shader without [[position]] was patched");
        free(pd);
        // UE5 (Pinball FX 5.3.2): the entry point is main_<size>_<crc>, not main0.
        const char *e = "struct main_00001a2c_8f3e9b01_out { float4 c [[color(0)]]; };\n"
                        "fragment main_00001a2c_8f3e9b01_out main_00001a2c_8f3e9b01(float4 gl_FragCoord [[position]])\n"
                        "{\n    main_00001a2c_8f3e9b01_out out = {};\n    out.c = gl_FragCoord;\n    return out;\n}\n";
        char *pe = kl_fove_patch_for_test(e);
        CHECK(pe && strstr(pe, "map_physical_to_screen_coordinates(gl_FragCoord.xy, kl_fove_layer)"),
              "UE5 main_XXXXXXXX_XXXXXXXX entry point not patched:\n%s", pe ? pe : "(null)");
        free(pe);
        const char *g = "fragment helper_out helper(float4 gl_FragCoord [[position]])\n{\n    return {};\n}\n";
        char *pg = kl_fove_patch_for_test(g);
        CHECK(!pg, "a non-entry-point name was patched");
        free(pg);
        // UE4's mobile depth fetch (Moss: DepthFade translucency) reads the
        // subpass input inside a helper that takes the position by reference.
        // The call must get the PHYSICAL position; a helper that also uses the
        // position otherwise keeps the remapped one.
        const char *hs = "static inline __attribute__((always_inline))\n"
                         "float VulkanSubpassDepthFetch(texture2d_ms_array<float> D, thread float4& gl_FragCoord, thread uint& gl_ViewIndex)\n"
                         "{\n    return D.read(uint2(gl_FragCoord.xy), gl_ViewIndex, 0).x;\n}\n\n"
                         "static inline float Other(thread float4& gl_FragCoord)\n{\n    return gl_FragCoord.x + float(uint2(gl_FragCoord.xy).x);\n}\n\n"
                         "fragment main_00002aac_8392f5e1_out main_00002aac_8392f5e1(float4 gl_FragCoord [[position]], uint gl_ViewIndex [[render_target_array_index]])\n"
                         "{\n    main_00002aac_8392f5e1_out out = {};\n"
                         "    float f = VulkanSubpassDepthFetch(s.D, gl_FragCoord, gl_ViewIndex);\n"
                         "    float o = Other(gl_FragCoord);\n    return out;\n}\n";
        kl_fove_set_subpass_helpers_for_test(1);
        char *ph = kl_fove_patch_for_test(hs);
        CHECK(ph && strstr(ph, "float4 kl_fove_phys = gl_FragCoord;") &&
              strstr(ph, "VulkanSubpassDepthFetch(s.D, kl_fove_phys, gl_ViewIndex)") &&
              strstr(ph, "Other(gl_FragCoord)") &&
              strstr(ph, "D.read(uint2(gl_FragCoord.xy), gl_ViewIndex, 0)"),
              "KL_FOVE_SUBPASS_HELPERS=1: subpass read in a helper not fed the physical position:\n%s", ph ? ph : "(null)");
        free(ph);
        // KL_FOVE_SUBPASS_HELPERS=0, the A/B control: the old behaviour, the
        // helper is called with the remapped position and nothing is saved.
        kl_fove_set_subpass_helpers_for_test(0);
        char *ph0 = kl_fove_patch_for_test(hs);
        CHECK(ph0 && strstr(ph0, "VulkanSubpassDepthFetch(s.D, gl_FragCoord, gl_ViewIndex)") &&
              !strstr(ph0, "kl_fove_phys") &&
              strstr(ph0, "map_physical_to_screen_coordinates(gl_FragCoord.xy, gl_ViewIndex)"),
              "KL_FOVE_SUBPASS_HELPERS=0: the helper call was changed:\n%s", ph0 ? ph0 : "(null)");
        free(ph0);
        kl_fove_set_subpass_helpers_for_test(1);
        // Only a `.read(uint2(pos.xy)…)` from one of the helper's own TEXTURE
        // parameters counts: not a texture reached through another parameter,
        // not some other uint2 of the position. A texture type whose template
        // arguments hold a comma still counts.
        const char *hn = "static inline float ViaMember(constant Buf& b, thread float4& gl_FragCoord)\n"
                         "{\n    return b.T.read(uint2(gl_FragCoord.xy)).x;\n}\n\n"
                         "static inline float NotARead(texture2d<float> T, thread float4& gl_FragCoord)\n"
                         "{\n    return T.sample(S, float2(uint2(gl_FragCoord.xy))).x;\n}\n\n"
                         "static inline float Templ(texture2d<float, access::read> T, thread float4& gl_FragCoord)\n"
                         "{\n    return T.read(uint2(gl_FragCoord.xy)).x;\n}\n\n"
                         "fragment main0_out main0(float4 gl_FragCoord [[position]])\n"
                         "{\n    main0_out out = {};\n"
                         "    float a = ViaMember(b, gl_FragCoord);\n"
                         "    float c = NotARead(t, gl_FragCoord);\n"
                         "    float d = Templ(t, gl_FragCoord);\n    return out;\n}\n";
        char *pn = kl_fove_patch_for_test(hn);
        CHECK(pn && strstr(pn, "ViaMember(b, gl_FragCoord)") && strstr(pn, "NotARead(t, gl_FragCoord)") &&
              strstr(pn, "Templ(t, kl_fove_phys)"),
              "helper read matching: only a read from the helper's own texture parameter may count:\n%s",
              pn ? pn : "(null)");
        free(pn);
    }
    // The pending-expand table: buffers released without -commit leave nothing.
    int pb = kl_fove_pend_selftest();
    CHECK(!pb, "pending-expand table self-test: %d failed checks", pb);
    // The intermediate state machine (kl_vulkan's candidate ->
    // confirmed bookkeeping): a pass makes a candidate, a transfer / subpass /
    // following eye pass confirms it, confirmation is sticky until destroy, a
    // non-candidate is never confirmed, the oldest CANDIDATE is evicted first.
    {
        kl_fove_inter_reset_for_test();
        int via = -1;
        const uint64_t A = 0xa000, B = 0xb000, C = 0xc000;
        CHECK(kl_fove_inter_state(A, &via) == 0 && via == 0, "unknown image has a state");
        CHECK(kl_fove_inter_confirm(A, KL_FOVE_VIA_XFER) == -1, "a non-candidate was confirmed");
        CHECK(kl_fove_inter_note(A, &via) == 1 && via == 0, "first pass is not a candidate");
        CHECK(kl_fove_inter_last() == A, "last candidate is not A");
        CHECK(kl_fove_inter_confirm(A, 0) == -1, "confirm with no via accepted");
        CHECK(kl_fove_inter_confirm(A, KL_FOVE_VIA_XFER) == 1, "copy did not confirm the candidate");
        CHECK(kl_fove_inter_confirm(A, KL_FOVE_VIA_SAMPLED) == 0, "a second confirmation was 'new'");
        CHECK(kl_fove_inter_note(A, &via) == 2 && via == KL_FOVE_VIA_XFER, "confirmed state/via not sticky (via %d)", via);
        CHECK(kl_fove_inter_last() == 0, "a confirmed image is still the last candidate");
        CHECK(kl_fove_inter_note(B, NULL) == 1 && kl_fove_inter_last() == B, "B not the last candidate");
        CHECK(kl_fove_inter_note(C, NULL) == 1 && kl_fove_inter_last() == C, "C not the last candidate");
        CHECK(kl_fove_inter_confirm(B, KL_FOVE_VIA_SUBPASS) == 1 && kl_fove_inter_state(B, &via) == 2 &&
              via == KL_FOVE_VIA_SUBPASS, "subpass confirmation");
        kl_fove_inter_forget(A);
        CHECK(kl_fove_inter_state(A, NULL) == 0, "destroyed image kept its confirmation");
        CHECK(kl_fove_inter_note(A, NULL) == 1, "a reused handle came back confirmed");
        // Fill the table with candidates: confirmed B survives the evictions.
        for (uint64_t i = 1; i <= 40; i++) kl_fove_inter_note(0x100000 + i, NULL);
        CHECK(kl_fove_inter_state(B, &via) == 2, "a confirmed intermediate was evicted by candidates");
        CHECK(kl_fove_inter_state(0x100000 + 40, NULL) == 1, "the newest candidate was evicted");
        CHECK(kl_fove_inter_state(C, NULL) == 0, "the oldest candidate was not evicted");
        kl_fove_inter_reset_for_test();
    }
    // Paint the Town Red's level shape, from a real log. Unity's
    // HDR path: per frame an eye-size 1x LDR R8G8B8A8_SRGB target (render pass
    // 0x13dd5af80, colour 0x16c31b200) and the 1x HDR R16G16B16A16_SFLOAT scene
    // (0x13dd5a700, colour 0x17d1b3c00), both 5088x3234 x2 layers viewMask 0x3
    // with the shared depth 0x17d3312c0 — candidates — then a pass whose colour
    // IS the runtime eye image and which binds a depth (0x13dd5a480: colour
    // fmt 43 DONT_CARE/STORE, depth fmt 129 DONT_CARE). A rule that confirmed
    // only on a no-depth pass into the eye never confirmed either. The direct
    // pass confirms the whole window (via sampled), sticky; an MSAA
    // resolve-into-the-eye pass only closes the window.
    {
        kl_fove_inter_reset_for_test();
        int via = -1;
        uint64_t got[8] = {0};
        // Scene 1: an eye-size 1x candidate (0x13dd58a00's colour) before the
        // MSAA pass that RESOLVES into the eye: not confirmed, and a later
        // direct pass into the eye does not reach back past that window.
        const uint64_t S1 = 0x17c000a00;
        CHECK(kl_fove_inter_note(S1, NULL) == 1, "scene-1 candidate not a candidate");
        CHECK(kl_fove_inter_eye_reached(0, got, 8) == 0, "a resolve-into-the-eye pass confirmed a candidate");
        CHECK(kl_fove_inter_state(S1, NULL) == 1, "scene-1 candidate changed state at the resolve pass");
        // Transition (loading): the two HDR passes, then the eye pass with depth.
        const uint64_t T1 = 0x17d3321c0, T2 = 0x17d333340;
        kl_fove_inter_note(T1, NULL);
        kl_fove_inter_note(T2, NULL);
        int n = kl_fove_inter_eye_reached(1, got, 8);
        CHECK(n == 2 && ((got[0] == T1 && got[1] == T2) || (got[0] == T2 && got[1] == T1)),
              "transition: %d confirmed (want T1+T2)", n);
        CHECK(kl_fove_inter_state(S1, NULL) == 1, "a candidate from a closed window was confirmed");
        // Gameplay frame 1: LDR + HDR candidates, then the eye pass (with depth).
        const uint64_t L = 0x16c31b200, H = 0x17d1b3c00;
        CHECK(kl_fove_inter_note(L, &via) == 1 && kl_fove_inter_note(H, &via) == 1, "scene-2 targets not candidates");
        n = kl_fove_inter_eye_reached(1, got, 8);
        CHECK(n == 2, "scene-2 eye pass confirmed %d (want the HDR scene and the LDR hop)", n);
        CHECK(kl_fove_inter_state(H, &via) == 2 && via == KL_FOVE_VIA_SAMPLED, "HDR scene not confirmed via sampled");
        CHECK(kl_fove_inter_state(L, &via) == 2 && via == KL_FOVE_VIA_SAMPLED, "LDR hop not confirmed via sampled");
        // Frame 2: both are marked from now on (note returns 2), nothing new.
        CHECK(kl_fove_inter_note(L, &via) == 2 && kl_fove_inter_note(H, &via) == 2 && via == KL_FOVE_VIA_SAMPLED,
              "confirmation not sticky into the next frame");
        CHECK(kl_fove_inter_eye_reached(1, got, 8) == 0, "an already-confirmed target counted as new");
        // Two eye passes in a row (e.g. the overlay after the final blit): the
        // second has an empty window.
        CHECK(kl_fove_inter_eye_reached(1, got, 8) == 0, "an empty window confirmed something");
        // The no-depth post pass shape still confirms the last candidate.
        const uint64_t P = 0x1a0000;
        kl_fove_inter_note(P, NULL);
        CHECK(kl_fove_inter_eye_reached(1, got, 1) == 1 && got[0] == P, "G6 sampled shape no longer confirms");
        // vkDestroyImage still drops it; a reused handle is a candidate again.
        kl_fove_inter_forget(H);
        CHECK(kl_fove_inter_note(H, NULL) == 1, "destroyed HDR target came back confirmed");
        kl_fove_inter_reset_for_test();
        printf("PTTR scene 2 (G8): HDR 1x scene + LDR hop confirmed via sampled by the eye pass WITH depth; "
               "scene 1's resolve pass only closes the window\n");
    }
    // Paint the Town Red's level: three confirmed targets
    // from a real log (HDR A 0x7122c8ed00 — the pass that CLEARs, carries the
    // scene's draws; HDR B 0x7122c6dcc0 and LDR 0x7122c6d040), then "Combat
    // Started": the first vkCmdDrawIndirect lands inside HDR A's multiview
    // pass. MoltenVK splits that pass around a compute encoder and resumes it
    // with LOAD, so HDR A is REFUSED (sticky until destroyed); the other two
    // stay confirmed, and the next eye pass does not re-confirm HDR A.
    {
        kl_fove_inter_reset_for_test();
        uint64_t got[8] = {0};
        int via = -1;
        const uint64_t A = 0x7122c8ed00, B = 0x7122c6dcc0, LD = 0x7122c6d040;
        kl_fove_inter_note(A, NULL); kl_fove_inter_note(B, NULL); kl_fove_inter_note(LD, NULL);
        CHECK(kl_fove_inter_eye_reached(1, got, 8) == 3, "the eye-pass window did not confirm its three targets");
        // Combat: an indirect draw inside HDR A's pass.
        CHECK(kl_fove_inter_refuse(A) == 2, "refusing a confirmed target did not report CONFIRMED");
        CHECK(kl_fove_inter_state(A, NULL) == 3, "refused target not in state 3");
        CHECK(kl_fove_inter_refuse(A) == 3, "a second refusal did not see the first");
        // Next frame: A is noted again but stays refused (not a candidate,
        // not confirmed by the eye pass); B and LD stay confirmed.
        CHECK(kl_fove_inter_note(A, &via) == 3, "refused target came back as a candidate");
        CHECK(kl_fove_inter_note(B, &via) == 2 && kl_fove_inter_note(LD, &via) == 2, "unrefused targets lost confirmation");
        CHECK(kl_fove_inter_eye_reached(1, got, 8) == 0, "the eye pass re-confirmed a refused target");
        CHECK(kl_fove_inter_confirm(A, KL_FOVE_VIA_XFER) != 1, "a copy into the eye re-confirmed a refused target");
        CHECK(kl_fove_inter_state(A, NULL) == 3, "refusal not sticky");
        // A candidate refused before it is ever confirmed (an indirect draw in
        // its first frame) is never confirmed either.
        const uint64_t N = 0x7200000000;
        kl_fove_inter_note(N, NULL);
        CHECK(kl_fove_inter_refuse(N) == 1, "refusing a candidate did not report candidate");
        CHECK(kl_fove_inter_eye_reached(1, got, 8) == 0, "a refused candidate was confirmed");
        // An unseen image refused (indirect draw before its pass was noted).
        CHECK(kl_fove_inter_refuse(0x7300000000) == 0, "refusing an unseen image did not report unseen");
        CHECK(kl_fove_inter_note(0x7300000000, NULL) == 3, "unseen refused image noted as a candidate");
        // vkDestroyImage drops the refusal: a reused handle is a candidate again.
        kl_fove_inter_forget(A);
        CHECK(kl_fove_inter_note(A, NULL) == 1, "destroyed refused target did not come back as a candidate");
        kl_fove_inter_reset_for_test();
        printf("PTTR combat (G10): an indirect draw inside a confirmed (or candidate) intermediate's pass refuses it, "
               "sticky until destroyed; the other confirmed targets stay\n");
    }
    // Metal: the in-pass intermediate on a real device (Batman: Arkham
    // Shadow's shape), and the copy-into-the-eye ordering. Needs foveation on.
    {
        setenv("KL_FOVEATION", "high", 1);
        // The indirect draw arrives on a SECONDARY command buffer (Unity).
        {
            int g10b = kl_vulkan_fove_indirect_selftest();
            if (g10b < 0) printf("PTTR combat via secondaries (G10b) SKIPPED: foveation off or no MoltenVK headers\n");
            else {
                CHECK(!g10b, "G10b secondary-indirect refusal: %d failed checks", g10b);
                if (!g10b) printf("PTTR combat via secondaries (G10b): an indirect draw recorded in a secondary refuses "
                                  "the multiview intermediate whose pass executes it; a re-recorded clean secondary and a "
                                  "non-multiview pass do not\n");
            }
        }
        char why[160] = "";
        int g = kl_fove_inter_gpu_selftest(why, sizeof why);
        if (g < 0) printf("intermediate GPU self-test SKIPPED: %s\n", why);
        else {
            CHECK(!g, "intermediate GPU self-test: %d failed checks", g);
            if (!g) printf("intermediate GPU self-test: scene + eye subpass marked with one map, eye expanded at "
                           "commit, copy expanded before its blit, unmapped eye subpass counted; G7 transient MSAA (Deadpool): "
                           "both subpass encoders marked + held, resolve target expanded before the post pass\n");
        }
    }
    {
        char why[160] = "";
        int g = kl_fove_recentre_selftest(why, sizeof why);
        if (g < 0) printf("re-centre GPU self-test SKIPPED: %s\n", why);
        else {
            CHECK(!g, "re-centre GPU self-test: %d failed checks", g);
            if (!g) printf("re-centre GPU self-test: Deadpool's map built before the record (3226 / 1862) re-centred to "
                           "2544 / 2544 once the record arrived (into another slot while another buffer's expand was pending "
                           "on the old map, which that buffer keeps), not again for the same record, never for ITR's "
                           "full-image record\n");
        }
    }
    {
        char why[160] = "";
        float r[2 * 2 * 8];
        int g = kl_fove_layers_selftest(why, sizeof why, r, getenv("KL_FOVE_TEST_SHOTS"));
        if (g < 0) printf("layered eye-pair GPU self-test SKIPPED: %s\n", why);
        else {
            CHECK(!g, "layered eye-pair GPU self-test: %d failed checks", g);
            for (int m = 0; m < 2; m++)
                for (int l = 0; l < 2; l++)
                    printf("  drawn through the GPU, %-8s %-5s eye, rate by eighths of its picture: "
                           "%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f\n", m ? "envelope" : "per-eye", l ? "right" : "left",
                           r[(m * 2 + l) * 8 + 0], r[(m * 2 + l) * 8 + 1], r[(m * 2 + l) * 8 + 2], r[(m * 2 + l) * 8 + 3],
                           r[(m * 2 + l) * 8 + 4], r[(m * 2 + l) * 8 + 5], r[(m * 2 + l) * 8 + 6], r[(m * 2 + l) * 8 + 7]);
            if (!g) printf("layered eye-pair GPU self-test (Alien: Rogue Incursion's shape, Vision Pro cones): with one "
                           "peak per eye the left eye is coarse on its left, the right eye on its right; with the "
                           "envelope both eyes are drawn equally sharp, symmetric about the middle, both straight-ahead "
                           "points at full rate; the map follows 20 dynamic-resolution steps\n");
        }
    }
    {
        char why[160] = "";
        int g = kl_fove_scratch_selftest(why, sizeof why);
        if (g < 0) printf("scratch-cache GPU self-test SKIPPED: %s\n", why);
        else {
            CHECK(!g, "scratch-cache GPU self-test: %d failed checks", g);
            if (!g) printf("scratch-cache GPU self-test (G9, the jetsam case): HDR + LDR expanded alternately for 30 "
                           "frames made 4 scratch textures, not 4 per frame; a resize replaced only its format; ways bounded\n");
        }
    }
    printf("%s\n", fails ? "FAILED" : "fove maths + MSL layer patch + pending table + intermediate: all checks pass");
    return fails != 0;
}
