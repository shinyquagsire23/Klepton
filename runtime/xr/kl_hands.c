// ARKit hand joints -> OVRPlugin's 24-bone hand skeleton. See kl_hands.h for
// the convention and where each part of a bone frame comes from.
#include "kl_hands.h"

#include <math.h>
#include <string.h>

// ---- small vector and rotation helpers ------------------------------------

typedef struct { float x, y, z; } v3;

static v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static v3 vadd(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 vsub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 vmul(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static float vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 vcross(v3 a, v3 b) {
    return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float vlen(v3 a) { return sqrtf(vdot(a, a)); }
static v3 vnorm(v3 a) { float l = vlen(a); return l > 1e-9f ? vmul(a, 1.0f / l) : V(0, 0, 0); }
static v3 vp(const float *p) { return V(p[0], p[1], p[2]); }

// A rotation as its three columns: the frame's X, Y and Z axes.
typedef struct { v3 x, y, z; } m3;

static m3 mtrans_mul(m3 a, m3 b) {               // a^T * b
    m3 r;
    r.x = V(vdot(a.x, b.x), vdot(a.y, b.x), vdot(a.z, b.x));
    r.y = V(vdot(a.x, b.y), vdot(a.y, b.y), vdot(a.z, b.y));
    r.z = V(vdot(a.x, b.z), vdot(a.y, b.z), vdot(a.z, b.z));
    return r;
}
static v3 mtrans_v(m3 a, v3 v) { return V(vdot(a.x, v), vdot(a.y, v), vdot(a.z, v)); }

static void m_to_q(m3 m, float q[4]) {
    float t = m.x.x + m.y.y + m.z.z;
    if (t > 0) {
        float s = sqrtf(t + 1.0f) * 2;
        q[3] = 0.25f * s;
        q[0] = (m.y.z - m.z.y) / s;
        q[1] = (m.z.x - m.x.z) / s;
        q[2] = (m.x.y - m.y.x) / s;
    } else if (m.x.x > m.y.y && m.x.x > m.z.z) {
        float s = sqrtf(1.0f + m.x.x - m.y.y - m.z.z) * 2;
        q[3] = (m.y.z - m.z.y) / s;
        q[0] = 0.25f * s;
        q[1] = (m.y.x + m.x.y) / s;
        q[2] = (m.z.x + m.x.z) / s;
    } else if (m.y.y > m.z.z) {
        float s = sqrtf(1.0f + m.y.y - m.x.x - m.z.z) * 2;
        q[3] = (m.z.x - m.x.z) / s;
        q[0] = (m.y.x + m.x.y) / s;
        q[1] = 0.25f * s;
        q[2] = (m.z.y + m.y.z) / s;
    } else {
        float s = sqrtf(1.0f + m.z.z - m.x.x - m.y.y) * 2;
        q[3] = (m.x.y - m.y.x) / s;
        q[0] = (m.z.x + m.x.z) / s;
        q[1] = (m.z.y + m.y.z) / s;
        q[2] = 0.25f * s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
    if (q[3] < 0) for (int i = 0; i < 4; i++) q[i] = -q[i];
}

static v3 q_rot(const float q[4], v3 v) {
    v3 u = V(q[0], q[1], q[2]);
    v3 t = vmul(vcross(u, v), 2);
    return vadd(vadd(v, vmul(t, q[3])), vcross(u, t));
}

static m3 q_to_m(const float q[4]) {
    m3 m = { q_rot(q, V(1, 0, 0)), q_rot(q, V(0, 1, 0)), q_rot(q, V(0, 0, 1)) };
    return m;
}

static v3 axis_of(m3 m, int axis) {             // 0..5 = +x -x +y -y +z -z
    v3 a = axis < 2 ? m.x : axis < 4 ? m.y : m.z;
    return axis & 1 ? vmul(a, -1) : a;
}

// The OVR frame of a bone from the direction toward the fingertip and a
// direction out of the back of the hand (only its part across the bone counts).
// Left: X = distal, Y = palmar. Right: X = proximal, Y = dorsal. Z = X x Y.
static int ovr_frame(int hand, v3 distal, v3 dorsal, m3 *out) {
    v3 d = vnorm(distal);
    v3 y = vsub(dorsal, vmul(d, vdot(dorsal, d)));
    if (vlen(d) < 0.5f || vlen(y) < 1e-4f) return 0;
    y = vnorm(y);
    m3 m;
    if (hand == 0) { m.x = d; m.y = vmul(y, -1); }
    else           { m.x = vmul(d, -1); m.y = y; }
    m.z = vcross(m.x, m.y);
    *out = m;
    return 1;
}

// ---- the skeleton's shape --------------------------------------------------

static const int16_t k_parent[KLO_BONES] = {
    -1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18,
};

// The ARKit joint each OVR bone sits on, and the one its X axis aims at (-1:
// no joint of its own — Thumb0 is placed between the wrist and the thumb's
// base, the forearm stub and the tips take their parent's frame).
static const int8_t k_joint[KLO_BONES] = {
    KLH_WRIST, KLH_WRIST, -1, KLH_THUMB_KNUCKLE, KLH_THUMB_IB, KLH_THUMB_IT,
    KLH_INDEX_KNUCKLE, KLH_INDEX_IB, KLH_INDEX_IT,
    KLH_MIDDLE_KNUCKLE, KLH_MIDDLE_IB, KLH_MIDDLE_IT,
    KLH_RING_KNUCKLE, KLH_RING_IB, KLH_RING_IT,
    KLH_LITTLE_META, KLH_LITTLE_KNUCKLE, KLH_LITTLE_IB, KLH_LITTLE_IT,
    KLH_THUMB_TIP, KLH_INDEX_TIP, KLH_MIDDLE_TIP, KLH_RING_TIP, KLH_LITTLE_TIP,
};
static const int8_t k_aim[KLO_BONES] = {
    KLH_MIDDLE_KNUCKLE, -1, KLH_THUMB_KNUCKLE, KLH_THUMB_IB, KLH_THUMB_IT, KLH_THUMB_TIP,
    KLH_INDEX_IB, KLH_INDEX_IT, KLH_INDEX_TIP,
    KLH_MIDDLE_IB, KLH_MIDDLE_IT, KLH_MIDDLE_TIP,
    KLH_RING_IB, KLH_RING_IT, KLH_RING_TIP,
    KLH_LITTLE_KNUCKLE, KLH_LITTLE_IB, KLH_LITTLE_IT, KLH_LITTLE_TIP,
    -1, -1, -1, -1, -1,
};
// The joint whose ARKit orientation gives the twist (Thumb0: the thumb's base).
static const int8_t k_twist[KLO_BONES] = {
    -1, -1, KLH_THUMB_KNUCKLE, KLH_THUMB_KNUCKLE, KLH_THUMB_IB, KLH_THUMB_IT,
    KLH_INDEX_KNUCKLE, KLH_INDEX_IB, KLH_INDEX_IT,
    KLH_MIDDLE_KNUCKLE, KLH_MIDDLE_IB, KLH_MIDDLE_IT,
    KLH_RING_KNUCKLE, KLH_RING_IB, KLH_RING_IT,
    KLH_LITTLE_META, KLH_LITTLE_KNUCKLE, KLH_LITTLE_IB, KLH_LITTLE_IT,
    -1, -1, -1, -1, -1,
};
static const int8_t k_finger_of_tip[5] = { KLH_THUMB_TIP, KLH_INDEX_TIP, KLH_MIDDLE_TIP,
                                           KLH_RING_TIP, KLH_LITTLE_TIP };

const char *kl_hands_bone_name(int b) {
    static const char *n[KLO_BONES] = {
        "Wrist", "Forearm", "Thumb0", "Thumb1", "Thumb2", "Thumb3",
        "Index1", "Index2", "Index3", "Middle1", "Middle2", "Middle3",
        "Ring1", "Ring2", "Ring3", "Pinky0", "Pinky1", "Pinky2", "Pinky3",
        "ThumbTip", "IndexTip", "MiddleTip", "RingTip", "PinkyTip",
    };
    return (unsigned)b < KLO_BONES ? n[b] : "?";
}

const char *kl_hands_axis_name(int a) {
    static const char *n[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };
    return (unsigned)a < 6 ? n[a] : "?";
}

// ---- the bind pose ----------------------------------------------------------
//
// A generic adult left hand, about 19 cm wrist to middle fingertip, lying flat:
// joint positions in the wrist's frame (X toward the fingers, Y out of the
// palm, Z toward the little finger), each finger a straight line, the thumb
// relaxed to the side with its pad turned half toward the fingers. The bone
// frames are then built from it by the same rule as a live hand, so bind and
// live cannot disagree about axes.

typedef struct { v3 p[KLO_BONES]; v3 palmar[KLO_BONES]; } bind_shape;

static void chain(bind_shape *s, int first, int n_bones, v3 start, v3 dir,
                  const float *len, int tip) {
    v3 d = vnorm(dir), p = start;
    for (int i = 0; i < n_bones; i++) {
        s->p[first + i] = p;
        p = vadd(p, vmul(d, len[i]));
    }
    s->p[tip] = p;
}

static void bind_shape_left(bind_shape *s) {
    memset(s, 0, sizeof *s);
    for (int b = 0; b < KLO_BONES; b++) s->palmar[b] = V(0, 1, 0);
    // Thumb: Thumb0 (between wrist and thumb base) through the tip, bending
    // a little toward the fingers at each joint.
    {
        static const v3 dirs[4] = { { 0.60f, 0.35f, -0.72f }, { 0.75f, 0.25f, -0.60f },
                                    { 0.85f, 0.20f, -0.48f }, { 0.90f, 0.15f, -0.40f } };
        static const float len[3] = { 0.033f, 0.033f, 0.025f };
        const int bones[5] = { KLO_THUMB0, KLO_THUMB1, KLO_THUMB2, KLO_THUMB3, KLO_THUMB_TIP };
        // Thumb1 is the thumb's base (ARKit's thumbKnuckle); Thumb0 halfway
        // between it and the wrist, exactly where a live hand puts it.
        v3 p = vmul(vnorm(dirs[0]), 0.050f);
        s->p[KLO_THUMB0] = vmul(p, 0.5f);
        for (int i = 1; i < 4; i++) {
            s->p[bones[i]] = p;
            p = vadd(p, vmul(vnorm(dirs[i]), len[i - 1]));
        }
        s->p[KLO_THUMB_TIP] = p;
        for (int i = 0; i < 5; i++) s->palmar[bones[i]] = vnorm(V(0, 0.7f, 0.7f));
    }
    static const float li[3] = { 0.038f, 0.024f, 0.022f };
    static const float lm[3] = { 0.043f, 0.028f, 0.025f };
    static const float lr[3] = { 0.039f, 0.027f, 0.024f };
    static const float lp[3] = { 0.031f, 0.020f, 0.022f };
    chain(s, KLO_INDEX1, 3, V(0.095f, 0.006f, -0.024f), V(1, 0, -0.06f), li, KLO_INDEX_TIP);
    chain(s, KLO_MIDDLE1, 3, V(0.096f, 0.003f, -0.002f), V(1, 0, 0), lm, KLO_MIDDLE_TIP);
    chain(s, KLO_RING1, 3, V(0.089f, 0.005f, 0.017f), V(1, 0, 0.07f), lr, KLO_RING_TIP);
    s->p[KLO_PINKY0] = V(0.034f, 0.009f, 0.023f);
    chain(s, KLO_PINKY1, 3, V(0.079f, 0.008f, 0.033f), V(1, 0, 0.14f), lp, KLO_PINKY_TIP);
}

static int is_tip(int b) { return b >= KLO_THUMB_TIP; }

void kl_hands_bind(int hand, kl_ovr_skel *out) {
    bind_shape s;
    bind_shape_left(&s);
    // The wrist's frame by the live rule (toward the middle knuckle, the palm's
    // normal through the index and little knuckles), and the shape re-expressed
    // in it, so that a live hand held like this one reads as the bind pose.
    m3 f[KLO_BONES];
    {
        v3 n = vcross(s.p[KLO_INDEX1], s.p[KLO_PINKY1]);   // left: out of the back
        m3 w;
        ovr_frame(0, s.p[KLO_MIDDLE1], n, &w);
        for (int b = 0; b < KLO_BONES; b++) {
            s.p[b] = mtrans_v(w, s.p[b]);
            // The fingers' fronts face where the palm does (flat hand); the
            // thumb keeps its own turn.
            s.palmar[b] = b >= KLO_THUMB0 && b <= KLO_THUMB3 ? mtrans_v(w, s.palmar[b]) : V(0, 1, 0);
        }
        m3 id = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        f[KLO_WRIST] = f[KLO_FOREARM] = id;
    }
    for (int b = 2; b < KLO_BONES; b++) {
        if (is_tip(b)) { f[b] = f[k_parent[b]]; continue; }
        int child = -1;
        for (int c = 0; c < KLO_BONES; c++) if (k_parent[c] == b) { child = c; break; }
        // Left hand: Y palmar, so "dorsal" for ovr_frame is -palmar.
        if (child < 0 || !ovr_frame(0, vsub(s.p[child], s.p[b]), vmul(s.palmar[b], -1), &f[b]))
            f[b] = f[k_parent[b]];
    }
    // Thumb0 has no joint of its own; live it takes its twist from the thumb's
    // base, so here too.
    ovr_frame(0, vsub(s.p[KLO_THUMB1], s.p[KLO_THUMB0]), vmul(f[KLO_THUMB1].y, -1), &f[KLO_THUMB0]);
    for (int b = KLO_THUMB_TIP; b < KLO_BONES; b++) f[b] = f[k_parent[b]];
    memset(out, 0, sizeof *out);
    float sign = hand == 0 ? 1.0f : -1.0f;      // the mirror: positions negate
    for (int b = 0; b < KLO_BONES; b++) {
        kl_ovr_bone *o = &out->bone[b];
        o->parent = k_parent[b];
        if (b == KLO_WRIST) { o->pose.q[3] = 1; continue; }
        int pb = k_parent[b];
        v3 lp = mtrans_v(f[pb], vsub(s.p[b], s.p[pb]));
        o->pose.p[0] = sign * lp.x; o->pose.p[1] = sign * lp.y; o->pose.p[2] = sign * lp.z;
        m_to_q(mtrans_mul(f[pb], f[b]), o->pose.q);
    }
    // Capsules: four across the palm (wrist bone, wrist to each knuckle), one
    // per finger bone from its joint to the next.
    int n = 0;
    static const int knuckles[4] = { KLO_INDEX1, KLO_MIDDLE1, KLO_RING1, KLO_PINKY1 };
    static const float palm_r[4] = { 0.018f, 0.020f, 0.018f, 0.016f };
    for (int i = 0; i < 4; i++) {
        kl_ovr_capsule *c = &out->cap[n++];
        c->bone = KLO_WRIST;
        v3 k = s.p[knuckles[i]];
        v3 a = vmul(k, 0.30f), e = vmul(k, 0.85f);
        c->a[0] = sign * a.x; c->a[1] = sign * a.y; c->a[2] = sign * a.z;
        c->b[0] = sign * e.x; c->b[1] = sign * e.y; c->b[2] = sign * e.z;
        c->radius = palm_r[i];
    }
    static const int fb[15] = { KLO_THUMB1, KLO_THUMB2, KLO_THUMB3, KLO_INDEX1, KLO_INDEX2,
                                KLO_INDEX3, KLO_MIDDLE1, KLO_MIDDLE2, KLO_MIDDLE3, KLO_RING1,
                                KLO_RING2, KLO_RING3, KLO_PINKY1, KLO_PINKY2, KLO_PINKY3 };
    static const float fr[15] = { 0.015f, 0.011f, 0.010f, 0.010f, 0.008f, 0.0075f,
                                  0.011f, 0.008f, 0.0075f, 0.010f, 0.0075f, 0.007f,
                                  0.0085f, 0.007f, 0.0065f };
    for (int i = 0; i < 15; i++) {
        int b = fb[i], child = -1;
        for (int c = 0; c < KLO_BONES; c++) if (k_parent[c] == b) { child = c; break; }
        kl_ovr_capsule *c = &out->cap[n++];
        c->bone = (int16_t)b;
        if (child >= 0) memcpy(c->b, out->bone[child].pose.p, sizeof c->b);
        c->radius = fr[i];
    }
}

// ---- finding ARKit's dorsal axis ---------------------------------------------
//
// On a flat hand every finger bone's back faces the way the back of the palm
// does, so the joint axis that agrees with the palm's normal on all of them is
// the one. Flat: each of the four fingers within 25 degrees of straight, from
// its metacarpal through the tip. Thirty such frames, and the winner has to be
// clearly ahead (mean > 0.85, nothing else above 0.5) — a hand that is never
// held flat simply stays on the palm-normal fallback.

static v3 jp(const kl_hand_xf *j, int i) { return vp(j[i].p); }

static v3 palm_dorsal(int hand, const kl_hand_xf *j) {
    v3 w = jp(j, KLH_WRIST);
    v3 n = vcross(vsub(jp(j, KLH_INDEX_KNUCKLE), w), vsub(jp(j, KLH_LITTLE_KNUCKLE), w));
    // (I-W) x (L-W) points out of the palm on a right hand and out of its back
    // on a left one (right-handed space).
    return vnorm(hand == 1 ? vmul(n, -1) : n);
}

static int finger_flat(const kl_hand_xf *j, int meta) {
    v3 base = vnorm(vsub(jp(j, meta + 1), jp(j, meta)));
    for (int k = meta + 1; k < meta + 4; k++) {
        v3 d = vnorm(vsub(jp(j, k + 1), jp(j, k)));
        if (vdot(d, base) < 0.906f) return 0;           // cos 25 deg
    }
    return 1;
}

static void calibrate(int hand, const kl_hand_xf *j, uint32_t tracked, kl_hands_ctx *c) {
    c->just_latched = 0;
    if (c->dorsal_axis >= 0) return;
    if ((tracked & ((1u << KLH_FOREARM_WRIST) - 1)) != ((1u << KLH_FOREARM_WRIST) - 1)) return;
    static const int metas[4] = { KLH_INDEX_META, KLH_MIDDLE_META, KLH_RING_META, KLH_LITTLE_META };
    for (int f = 0; f < 4; f++) if (!finger_flat(j, metas[f])) return;
    v3 dorsal = palm_dorsal(hand, j);
    for (int f = 0; f < 4; f++) {
        for (int k = metas[f]; k < metas[f] + 4; k++) {
            m3 m = q_to_m(j[k].q);
            v3 d = vnorm(vsub(jp(j, k + 1), jp(j, k)));
            for (int a = 0; a < 6; a++) {
                c->acc_dorsal[a] += vdot(axis_of(m, a), dorsal) / 16.0;
                c->acc_distal[a] += vdot(axis_of(m, a), d) / 16.0;
            }
        }
    }
    for (int k = KLH_THUMB_KNUCKLE; k < KLH_THUMB_TIP; k++) {
        m3 m = q_to_m(j[k].q);
        v3 d = vnorm(vsub(jp(j, k + 1), jp(j, k)));
        for (int a = 0; a < 6; a++) c->acc_thumb[a] += fabsf(vdot(axis_of(m, a), d)) / 3.0;
    }
    if (++c->flat_frames < 30) return;
    int best = 0, second = -1, dbest = 0;
    for (int a = 1; a < 6; a++) {
        if (c->acc_dorsal[a] > c->acc_dorsal[best]) best = a;
        if (c->acc_distal[a] > c->acc_distal[dbest]) dbest = a;
    }
    for (int a = 0; a < 6; a++)
        if (a != best && (second < 0 || c->acc_dorsal[a] > c->acc_dorsal[second])) second = a;
    float n = (float)c->flat_frames;
    c->dorsal_score = (float)(c->acc_dorsal[best] / n);
    c->distal_score = (float)(c->acc_distal[dbest] / n);
    c->thumb_dot = (float)(c->acc_thumb[best] / n);
    c->distal_axis = dbest;
    if (c->dorsal_score > 0.85f && c->acc_dorsal[second] / n < 0.5f) {
        c->dorsal_axis = best;
        c->just_latched = 1;
    } else {
        // Not conclusive: start over rather than average in a bad start.
        c->dorsal_axis = -1;
        c->flat_frames = 0;
        memset(c->acc_dorsal, 0, sizeof c->acc_dorsal);
        memset(c->acc_distal, 0, sizeof c->acc_distal);
        memset(c->acc_thumb, 0, sizeof c->acc_thumb);
        c->just_latched = -1;
    }
}

void kl_hands_init(kl_hands_ctx *c) {
    memset(c, 0, sizeof *c);
    c->dorsal_axis = c->distal_axis = -1;
}

void kl_hands_force_axis(kl_hands_ctx *c, int axis) {
    if ((unsigned)axis < 6) c->dorsal_axis = axis;
}

// ---- one frame ------------------------------------------------------------------

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int kl_hands_to_ovr(int hand, const kl_hand_xf j[KLH_JOINTS], uint32_t tracked,
                    kl_hands_ctx *c, kl_ovr_hand *out) {
    memset(out, 0, sizeof *out);
    for (int b = 0; b < KLO_BONES; b++) out->rot[b][3] = 1;
    out->scale = c->scale > 0 ? c->scale : 1.0f;
    out->pinch_bits = c->pinch_bits;
    // Without the wrist and the three palm knuckles there is no hand frame.
    uint32_t need = (1u << KLH_WRIST) | (1u << KLH_INDEX_KNUCKLE) |
                    (1u << KLH_MIDDLE_KNUCKLE) | (1u << KLH_LITTLE_KNUCKLE);
    if ((tracked & need) != need) return 0;
    calibrate(hand, j, tracked, c);
    v3 dorsal = palm_dorsal(hand, j);

    m3 f[KLO_BONES];
    v3 w = jp(j, KLH_WRIST);
    if (!ovr_frame(hand, vsub(jp(j, KLH_MIDDLE_KNUCKLE), w), dorsal, &f[KLO_WRIST])) return 0;
    dorsal = hand == 0 ? vmul(f[KLO_WRIST].y, -1) : f[KLO_WRIST].y;   // square to the palm's axis
    f[KLO_FOREARM] = f[KLO_WRIST];
    for (int b = 2; b < KLO_BONES; b++) {
        int pb = k_parent[b];
        if (is_tip(b)) { f[b] = f[pb]; continue; }
        v3 from = b == KLO_THUMB0
            ? vmul(vadd(w, jp(j, KLH_THUMB_KNUCKLE)), 0.5f) : jp(j, k_joint[b]);
        v3 dir = vsub(jp(j, k_aim[b]), from);
        v3 ref = dorsal;
        int tj = k_twist[b];
        if (c->dorsal_axis >= 0 && tj >= 0 && (tracked >> tj & 1))
            ref = axis_of(q_to_m(j[tj].q), c->dorsal_axis);
        if (!ovr_frame(hand, dir, ref, &f[b]) && !ovr_frame(hand, dir, dorsal, &f[b]))
            f[b] = f[pb];
    }
    out->tracked = 1;
    memcpy(out->root.p, j[KLH_WRIST].p, sizeof out->root.p);
    m_to_q(f[KLO_WRIST], out->root.q);
    for (int b = 2; b < KLO_BONES; b++)
        m_to_q(mtrans_mul(f[k_parent[b]], f[b]), out->rot[b]);

    // Confidence. ARKit marks every joint it cannot see right now (a finger
    // behind the palm, a curled tip) as not tracked while still estimating it,
    // and on the headset that is most frames for some joint; a Quest says High
    // for a hand it is following. Requiring every joint made the hand Low
    // nearly always (Elixir, 2026-10-19), and Elixir fades a Low hand out. So:
    // the hand is sure whenever it is tracked at all (the wrist and three
    // knuckles, checked above), a finger when at least half its joints are.
    static const int8_t first[5] = { KLH_THUMB_KNUCKLE, KLH_INDEX_META, KLH_MIDDLE_META,
                                     KLH_RING_META, KLH_LITTLE_META };
    for (int fi = 0; fi < 5; fi++) {
        int n = 0, seen = 0;
        for (int k = first[fi]; k <= k_finger_of_tip[fi]; k++, n++) seen += (int)(tracked >> k & 1);
        out->finger_high[fi] = 2 * seen >= n;
    }
    out->high_confidence = 1;

    // Scale: the middle finger's three bones against the bind pose's 9.6 cm.
    float mid = vlen(vsub(jp(j, KLH_MIDDLE_IB), jp(j, KLH_MIDDLE_KNUCKLE))) +
                vlen(vsub(jp(j, KLH_MIDDLE_IT), jp(j, KLH_MIDDLE_IB))) +
                vlen(vsub(jp(j, KLH_MIDDLE_TIP), jp(j, KLH_MIDDLE_IT)));
    float s = clampf(mid / 0.096f, 0.7f, 1.3f);
    c->scale = c->scale > 0 ? c->scale + (s - c->scale) * 0.02f : s;
    out->scale = c->scale;

    // Pinch: thumb tip to each fingertip. ARKit's tips sit inside the pads, so
    // fingers pressed together still measure about 2.7 cm (the frontend's own
    // pinch-to-trigger maps 5 cm open to 3 cm closed for the same reason):
    // 1 at 3 cm, 0 at 5.5 cm. A finger pinches from 0.8 (3.5 cm) and lets go
    // below 0.5 (4.25 cm); with 1.2 cm as "closed" a pinch almost never
    // registered (Angry Birds: the ray showed, nothing clicked, 2026-10-24).
    // The thumb counts as pinching with whichever finger it pinches.
    //
    // Letting go needs the fingers clearly apart (below 0.3, 4.75 cm) for three
    // frames running: ARKit's estimate of a tip flickers, and a pinch that drops
    // for one frame is a slingshot released mid-aim (Angry Birds, 2026-10-24).
    //
    // A fist is no pinch: with the middle, ring and little fingers rolled in
    // (two of their tips within 4.5 cm of their own knuckle) thumb and index tip
    // are close as well, and the game read a pinch where a grab was meant.
    int rolled = 0;
    for (int fi = 2; fi < 5; fi++) {
        int knuckle = k_finger_of_tip[fi] - 3;
        if (vlen(vsub(jp(j, k_finger_of_tip[fi]), jp(j, knuckle))) < 0.045f) rolled++;
    }
    int fist = rolled >= 2;
    v3 tt = jp(j, KLH_THUMB_TIP);
    float thumb = 0;
    uint32_t bits = 0;
    for (int fi = 1; fi < 5; fi++) {
        float d = vlen(vsub(jp(j, k_finger_of_tip[fi]), tt));
        float v = fist ? 0 : clampf((0.055f - d) / (0.055f - 0.030f), 0, 1);
        out->pinch[fi] = v;
        if (v > thumb) thumb = v;
        int was = (int)(c->pinch_bits >> fi & 1);
        if (v > 0.8f) { bits |= 1u << fi; c->pinch_low[fi] = 0; }
        else if (was && !fist) {
            if (v > 0.3f) c->pinch_low[fi] = 0;
            else c->pinch_low[fi]++;
            if (c->pinch_low[fi] < 3) bits |= 1u << fi;
        }
    }
    out->pinch[0] = thumb;
    if (bits) bits |= 1u;
    c->pinch_bits = bits;
    out->pinch_bits = bits;
    return 1;
}

void kl_hands_pointer(const kl_hand_xf j[KLH_JOINTS], const float shoulder[3], kl_hand_xf *out) {
    v3 o = vmul(vadd(jp(j, KLH_THUMB_IB), jp(j, KLH_INDEX_KNUCKLE)), 0.5f);
    v3 fwd = vnorm(vsub(o, vp(shoulder)));
    if (vlen(fwd) < 0.5f) fwd = V(0, 0, -1);
    m3 m;
    m.z = vmul(fwd, -1);
    m.x = vnorm(vcross(V(0, 1, 0), m.z));
    if (vlen(m.x) < 0.5f) m.x = V(1, 0, 0);
    m.y = vcross(m.z, m.x);
    out->p[0] = o.x; out->p[1] = o.y; out->p[2] = o.z;
    m_to_q(m, out->q);
}

// ---- a made-up hand, for runs without ARKit ---------------------------------

void kl_hands_synth(int hand, const float pos[3], const float fwd[3], float curl_deg,
                    kl_hand_xf j[KLH_JOINTS]) {
    kl_ovr_skel sk;
    kl_hands_bind(hand, &sk);
    // Palm down, fingers along `fwd` (made horizontal).
    v3 d = vnorm(V(fwd[0], 0, fwd[2]));
    if (vlen(d) < 0.5f) d = V(0, 0, -1);
    m3 root;
    if (!ovr_frame(hand, d, V(0, 1, 0), &root)) return;
    m3 f[KLO_BONES];
    v3 p[KLO_BONES];
    for (int b = 0; b < KLO_BONES; b++) {
        const kl_ovr_bone *bo = &sk.bone[b];
        m3 local = q_to_m(bo->pose.q);
        int finger = b >= KLO_INDEX1 && b <= KLO_PINKY3 && b != KLO_PINKY0;
        if (finger || (b >= KLO_THUMB2 && b <= KLO_THUMB3)) {
            float a = curl_deg * 3.14159265f / 180 * (finger ? 1.0f : 0.5f);
            m3 c = { V(cosf(a), sinf(a), 0), V(-sinf(a), cosf(a), 0), V(0, 0, 1) };
            m3 t = { V(0, 0, 0), V(0, 0, 0), V(0, 0, 0) };
            // local * c
            t.x = vadd(vadd(vmul(local.x, c.x.x), vmul(local.y, c.x.y)), vmul(local.z, c.x.z));
            t.y = vadd(vadd(vmul(local.x, c.y.x), vmul(local.y, c.y.y)), vmul(local.z, c.y.z));
            t.z = vadd(vadd(vmul(local.x, c.z.x), vmul(local.y, c.z.y)), vmul(local.z, c.z.z));
            local = t;
        }
        if (bo->parent < 0) { f[b] = root; p[b] = vp(pos); continue; }
        m3 pf = f[bo->parent];
        v3 lp = vp(bo->pose.p);
        p[b] = vadd(p[bo->parent], vadd(vadd(vmul(pf.x, lp.x), vmul(pf.y, lp.y)), vmul(pf.z, lp.z)));
        f[b].x = vadd(vadd(vmul(pf.x, local.x.x), vmul(pf.y, local.x.y)), vmul(pf.z, local.x.z));
        f[b].y = vadd(vadd(vmul(pf.x, local.y.x), vmul(pf.y, local.y.y)), vmul(pf.z, local.y.z));
        f[b].z = vadd(vadd(vmul(pf.x, local.z.x), vmul(pf.y, local.z.y)), vmul(pf.z, local.z.z));
    }
    static const int8_t bone_of[KLH_JOINTS] = {
        KLO_WRIST, KLO_THUMB1, KLO_THUMB2, KLO_THUMB3, KLO_THUMB_TIP,
        -KLO_INDEX1, KLO_INDEX1, KLO_INDEX2, KLO_INDEX3, KLO_INDEX_TIP,
        -KLO_MIDDLE1, KLO_MIDDLE1, KLO_MIDDLE2, KLO_MIDDLE3, KLO_MIDDLE_TIP,
        -KLO_RING1, KLO_RING1, KLO_RING2, KLO_RING3, KLO_RING_TIP,
        KLO_PINKY0, KLO_PINKY1, KLO_PINKY2, KLO_PINKY3, KLO_PINKY_TIP,
        KLO_WRIST, KLO_WRIST,
    };
    for (int i = 0; i < KLH_JOINTS; i++) {
        int b = bone_of[i] < 0 ? -bone_of[i] : bone_of[i];
        v3 q = p[b];
        if (bone_of[i] < 0) q = vadd(p[KLO_WRIST], vmul(vsub(p[b], p[KLO_WRIST]), 0.35f));
        j[i].p[0] = q.x; j[i].p[1] = q.y; j[i].p[2] = q.z;
        m_to_q(f[b], j[i].q);       // "ARKit's" axes = OVR's here
    }
}

// ---- a stronger bend, for a game that wants a fist ---------------------------

static void q_mul(const float a[4], const float b[4], float r[4]) {
    float t[4] = {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
    memcpy(r, t, sizeof t);
}

void kl_hands_scale_bend(int hand, kl_ovr_hand *o, float gain, float cap_deg) {
    static kl_ovr_skel bind[2];
    static int have[2];
    if ((unsigned)hand > 1 || gain == 1.0f) return;
    if (!have[hand]) { kl_hands_bind(hand, &bind[hand]); have[hand] = 1; }
    static const int bones[12] = { KLO_INDEX1, KLO_INDEX2, KLO_INDEX3, KLO_MIDDLE1, KLO_MIDDLE2,
                                    KLO_MIDDLE3, KLO_RING1, KLO_RING2, KLO_RING3, KLO_PINKY1,
                                    KLO_PINKY2, KLO_PINKY3 };
    float cap = cap_deg * 3.14159265f / 180;
    for (int i = 0; i < 12; i++) {
        int b = bones[i];
        // The turn away from the bind pose, scaled about its own axis.
        const float *bq = bind[hand].bone[b].pose.q;
        float inv[4] = { -bq[0], -bq[1], -bq[2], bq[3] }, d[4];
        q_mul(inv, o->rot[b], d);
        if (d[3] < 0) for (int k = 0; k < 4; k++) d[k] = -d[k];
        float s = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (s < 1e-6f) continue;
        float ang = 2 * atan2f(s, d[3]) * gain;
        if (ang > cap) ang = cap;
        float k = sinf(ang / 2) / s;
        float nd[4] = { d[0] * k, d[1] * k, d[2] * k, cosf(ang / 2) };
        q_mul(bq, nd, o->rot[b]);
    }
}

// ---- a loose fist read as a fist ------------------------------------------------

void kl_hands_fist_assist(int hand, kl_hands_ctx *ctx, kl_ovr_hand *o, float from_deg,
                          float to_deg, float at_deg) {
    static kl_ovr_skel bind[2];
    static int have[2];
    if ((unsigned)hand > 1 || from_deg <= 0 || at_deg <= from_deg) return;
    if (!have[hand]) { kl_hands_bind(hand, &bind[hand]); have[hand] = 1; }
    static const int first[4] = { KLO_INDEX1, KLO_MIDDLE1, KLO_RING1, KLO_PINKY1 };
    const float r2d = 57.29578f;
    for (int f = 0; f < 4; f++) {
        float d[3][4], ang[3], total = 0;
        for (int k = 0; k < 3; k++) {
            int b = first[f] + k;
            const float *bq = bind[hand].bone[b].pose.q;
            float inv[4] = { -bq[0], -bq[1], -bq[2], bq[3] };
            q_mul(inv, o->rot[b], d[k]);
            if (d[k][3] < 0) for (int c = 0; c < 4; c++) d[k][c] = -d[k][c];
            float s = sqrtf(d[k][0] * d[k][0] + d[k][1] * d[k][1] + d[k][2] * d[k][2]);
            ang[k] = 2 * atan2f(s, d[k][3]) * r2d;
            // Only a bend toward the palm counts (+Z on either hand in OVR's frames).
            total += d[k][2] > 0 ? ang[k] : 0;
        }
        // Engaged above from_deg, kept down to from_deg - 30: a fist held while
        // ARKit's estimate wobbles stays a fist.
        int on = ctx && ctx->fist_on[f];
        float start = on ? from_deg - 30 : from_deg;
        if (total <= start) { if (ctx) ctx->fist_on[f] = 0; continue; }
        if (ctx) ctx->fist_on[f] = 1;
        if (total <= from_deg) {
            // Held below the start: bend it up to the start, not beyond.
            float scale0 = from_deg / total;
            for (int k = 0; k < 3; k++) {
                if (d[k][2] <= 0) continue;
                float s = sqrtf(d[k][0] * d[k][0] + d[k][1] * d[k][1] + d[k][2] * d[k][2]);
                if (s < 1e-6f) continue;
                float a = ang[k] * scale0;
                if (a > 110) a = 110;
                a /= r2d;
                float m = sinf(a / 2) / s;
                float nd[4] = { d[k][0] * m, d[k][1] * m, d[k][2] * m, cosf(a / 2) };
                q_mul(bind[hand].bone[first[f] + k].pose.q, nd, o->rot[first[f] + k]);
            }
            continue;
        }
        // from..at maps onto from..to, above `at` it stays at `to`.
        float t = total >= at_deg ? to_deg
                                  : from_deg + (total - from_deg) * (to_deg - from_deg) / (at_deg - from_deg);
        float scale = t / total;
        for (int k = 0; k < 3; k++) {
            if (d[k][2] <= 0) continue;
            float s = sqrtf(d[k][0] * d[k][0] + d[k][1] * d[k][1] + d[k][2] * d[k][2]);
            if (s < 1e-6f) continue;
            float a = ang[k] * scale;
            if (a > 110) a = 110;
            a /= r2d;
            float m = sinf(a / 2) / s;
            float nd[4] = { d[k][0] * m, d[k][1] * m, d[k][2] * m, cosf(a / 2) };
            q_mul(bind[hand].bone[first[f] + k].pose.q, nd, o->rot[first[f] + k]);
        }
    }
}
