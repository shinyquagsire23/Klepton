// kl_hands.c on the Mac: synthetic ARKit hands in, OVR's hand skeleton out.
//
// A hand is built from the bind pose itself (forward kinematics), placed
// somewhere in the room, and handed over the way the frontend does: one
// position and one orientation per ARKit joint. The joint orientations carry an
// arbitrary axis convention (ARKit's is not documented), which the calibration
// has to find. Then:
//   - a flat left hand must come back as the bind pose: every bone rotation
//     equal to the bind rotation, the root equal to where the hand was put;
//   - its mirror image, read as a right hand, must give the SAME bone
//     rotations (the mirror relation of OVR's two hands);
//   - a finger curled about its own joint must turn about the bone's Z axis
//     only, by the same angle;
//   - thumb and index tip together must pinch.
#include "../runtime/xr/kl_hands.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { float m[3][3]; } M;           // m[col][row]: columns are the axes

static void qmat(const float q[4], M *r) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float c[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w) },
        { 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w) },
        { 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) },
    };
    memcpy(r->m, c, sizeof c);
}
static void mmul(const M *a, const M *b, M *r) {
    M t;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++)
            t.m[c][i] = a->m[0][i] * b->m[c][0] + a->m[1][i] * b->m[c][1] + a->m[2][i] * b->m[c][2];
    *r = t;
}
static void mv(const M *a, const float v[3], float r[3]) {
    float t[3];
    for (int i = 0; i < 3; i++) t[i] = a->m[0][i] * v[0] + a->m[1][i] * v[1] + a->m[2][i] * v[2];
    memcpy(r, t, sizeof t);
}
static void mq(const M *m, float q[4]) {
    float t = m->m[0][0] + m->m[1][1] + m->m[2][2];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4; q[0] = (m->m[1][2] - m->m[2][1]) / s;
        q[1] = (m->m[2][0] - m->m[0][2]) / s; q[2] = (m->m[0][1] - m->m[1][0]) / s;
    } else if (m->m[0][0] > m->m[1][1] && m->m[0][0] > m->m[2][2]) {
        float s = sqrtf(1 + m->m[0][0] - m->m[1][1] - m->m[2][2]) * 2;
        q[3] = (m->m[1][2] - m->m[2][1]) / s; q[0] = s / 4;
        q[1] = (m->m[1][0] + m->m[0][1]) / s; q[2] = (m->m[2][0] + m->m[0][2]) / s;
    } else if (m->m[1][1] > m->m[2][2]) {
        float s = sqrtf(1 + m->m[1][1] - m->m[0][0] - m->m[2][2]) * 2;
        q[3] = (m->m[2][0] - m->m[0][2]) / s; q[0] = (m->m[1][0] + m->m[0][1]) / s;
        q[1] = s / 4; q[2] = (m->m[2][1] + m->m[1][2]) / s;
    } else {
        float s = sqrtf(1 + m->m[2][2] - m->m[0][0] - m->m[1][1]) * 2;
        q[3] = (m->m[0][1] - m->m[1][0]) / s; q[0] = (m->m[2][0] + m->m[0][2]) / s;
        q[1] = (m->m[2][1] + m->m[1][2]) / s; q[2] = s / 4;
    }
}
static void axis_angle(float ax, float ay, float az, float deg, M *r) {
    float l = sqrtf(ax * ax + ay * ay + az * az), h = deg * 3.14159265f / 360;
    float q[4] = { ax / l * sinf(h), ay / l * sinf(h), az / l * sinf(h), cosf(h) };
    qmat(q, r);
}
// Angle between two rotations, degrees.
static float qdiff(const float a[4], const float b[4]) {
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1) d = 1;
    return 2 * acosf(d) * 57.29578f;
}

// World frames and positions of the 24 bones of a hand placed at (R, T),
// by forward kinematics over the bind pose. `mirror` reflects the result
// through the world's YZ plane, with frames -S·F (still rotations).
typedef struct { M f[KLO_BONES]; float p[KLO_BONES][3]; } world_hand;

static void place(const kl_ovr_skel *sk, const M *R, const float T[3], int mirror,
                  world_hand *w, const float *curl /* per bone, degrees about its Z */) {
    for (int b = 0; b < KLO_BONES; b++) {
        const kl_ovr_bone *bo = &sk->bone[b];
        M local;
        qmat(bo->pose.q, &local);
        if (curl && curl[b] != 0) {
            M c; axis_angle(0, 0, 1, curl[b], &c);
            mmul(&local, &c, &local);
        }
        if (bo->parent < 0) {
            mmul(R, &local, &w->f[b]);
            memcpy(w->p[b], T, sizeof w->p[b]);
        } else {
            mmul(&w->f[bo->parent], &local, &w->f[b]);
            mv(&w->f[bo->parent], bo->pose.p, w->p[b]);
            for (int i = 0; i < 3; i++) w->p[b][i] += w->p[bo->parent][i];
        }
    }
    if (mirror) {
        for (int b = 0; b < KLO_BONES; b++) {
            w->p[b][0] = -w->p[b][0];
            for (int c = 0; c < 3; c++) {      // -S·F: S flips row 0, the minus all
                w->f[b].m[c][1] = -w->f[b].m[c][1];
                w->f[b].m[c][2] = -w->f[b].m[c][2];
            }
        }
    }
}

// ARKit joints from bone frames: each joint's orientation is its bone's frame
// times `conv`, the stand-in for ARKit's undocumented axes.
static void to_arkit(const world_hand *w, const M *conv, kl_hand_xf j[KLH_JOINTS]) {
    static const int bone_of[KLH_JOINTS] = {
        KLO_WRIST, KLO_THUMB1, KLO_THUMB2, KLO_THUMB3, KLO_THUMB_TIP,
        -KLO_INDEX1, KLO_INDEX1, KLO_INDEX2, KLO_INDEX3, KLO_INDEX_TIP,
        -KLO_MIDDLE1, KLO_MIDDLE1, KLO_MIDDLE2, KLO_MIDDLE3, KLO_MIDDLE_TIP,
        -KLO_RING1, KLO_RING1, KLO_RING2, KLO_RING3, KLO_RING_TIP,
        KLO_PINKY0, KLO_PINKY1, KLO_PINKY2, KLO_PINKY3, KLO_PINKY_TIP,
        KLO_WRIST, KLO_WRIST,
    };
    for (int i = 0; i < KLH_JOINTS; i++) {
        int b = bone_of[i] < 0 ? -bone_of[i] : bone_of[i];
        M f; mmul(&w->f[b], conv, &f);
        mq(&f, j[i].q);
        memcpy(j[i].p, w->p[b], sizeof j[i].p);
        if (bone_of[i] < 0)                     // a metacarpal: a third of the way out
            for (int k = 0; k < 3; k++)
                j[i].p[k] = w->p[KLO_WRIST][k] + (w->p[b][k] - w->p[KLO_WRIST][k]) * 0.35f;
    }
    // The forearm, behind the wrist.
    for (int k = 0; k < 3; k++) {
        float back = w->f[KLO_WRIST].m[0][k];
        j[KLH_FOREARM_ARM].p[k] -= back * 0.2f;
    }
}

static float g_worst;

static void expect_bind(const char *what, int hand, const kl_ovr_skel *bind,
                        const kl_ovr_hand *o, const M *R, const float T[3], int mirror) {
    for (int b = 2; b < KLO_BONES; b++) {
        float d = qdiff(o->rot[b], bind->bone[b].pose.q);
        if (d > g_worst) g_worst = d;
        CHECK(d < 0.5f, "%s: %s is %.2f deg off the bind rotation", what,
              kl_hands_bone_name(b), d);
    }
    float rq[4]; M r = *R;
    if (mirror)
        for (int c = 0; c < 3; c++) { r.m[c][1] = -r.m[c][1]; r.m[c][2] = -r.m[c][2]; }
    mq(&r, rq);
    CHECK(qdiff(o->root.q, rq) < 0.5f, "%s: root %.2f deg off", what, qdiff(o->root.q, rq));
    float t0 = mirror ? -T[0] : T[0];
    CHECK(fabsf(o->root.p[0] - t0) < 1e-4f && fabsf(o->root.p[1] - T[1]) < 1e-4f &&
          fabsf(o->root.p[2] - T[2]) < 1e-4f, "%s: root position", what);
    (void)hand;
}

int main(void) {
    kl_ovr_skel bl, br;
    kl_hands_bind(0, &bl);
    kl_hands_bind(1, &br);

    // Bind: the right hand is the left one mirrored — rotations equal,
    // positions negated; the fingers' bones lie along their parent's X.
    for (int b = 0; b < KLO_BONES; b++) {
        CHECK(qdiff(bl.bone[b].pose.q, br.bone[b].pose.q) < 1e-3f, "bind %s", kl_hands_bone_name(b));
        for (int i = 0; i < 3; i++)
            CHECK(fabsf(bl.bone[b].pose.p[i] + br.bone[b].pose.p[i]) < 1e-6f, "bind pos %d", b);
        if (b >= 3 && b != KLO_INDEX1 && b != KLO_MIDDLE1 && b != KLO_RING1 && b != KLO_PINKY0) {
            const float *p = bl.bone[b].pose.p;
            CHECK(p[0] > 0.015f && fabsf(p[1]) < 1e-5f && fabsf(p[2]) < 1e-5f,
                  "%s not along its parent's X: %.4f %.4f %.4f", kl_hands_bone_name(b), p[0], p[1], p[2]);
        }
    }
    printf("bind (left, m): Index1 %.3f %.3f %.3f  Thumb0 %.3f %.3f %.3f  Pinky0 %.3f %.3f %.3f\n",
           bl.bone[KLO_INDEX1].pose.p[0], bl.bone[KLO_INDEX1].pose.p[1], bl.bone[KLO_INDEX1].pose.p[2],
           bl.bone[KLO_THUMB0].pose.p[0], bl.bone[KLO_THUMB0].pose.p[1], bl.bone[KLO_THUMB0].pose.p[2],
           bl.bone[KLO_PINKY0].pose.p[0], bl.bone[KLO_PINKY0].pose.p[1], bl.bone[KLO_PINKY0].pose.p[2]);

    // Some placement in the room, some joint-axis convention for each hand.
    M R; axis_angle(0.3f, 1, -0.2f, 70, &R);
    float T[3] = { -0.25f, 1.1f, -0.4f };
    M convL, convR;
    axis_angle(0, 0, 1, 90, &convL);            // e.g. ARKit +x = OVR +y, ...
    axis_angle(1, 1, 0, 180, &convR);           // ...and a different one per hand

    for (int hand = 0; hand < 2; hand++) {
        const char *name = hand ? "right (mirrored left)" : "left";
        world_hand w;
        place(&bl, &R, T, hand, &w, NULL);
        kl_hand_xf j[KLH_JOINTS];
        to_arkit(&w, hand ? &convR : &convL, j);
        kl_hands_ctx ctx; kl_hands_init(&ctx);
        kl_ovr_hand o;
        int latched_at = -1;
        for (int f = 0; f < 40; f++) {
            kl_hands_to_ovr(hand, j, (1u << KLH_JOINTS) - 1, &ctx, &o);
            if (ctx.just_latched == 1) latched_at = f;
        }
        CHECK(o.tracked, "%s: not tracked", name);
        CHECK(latched_at == 29, "%s: axis latched at frame %d", name, latched_at);
        printf("%s: dorsal = ARKit %s (%.3f), distal = %s (%.3f), thumb |dot| %.3f, scale %.3f\n",
               name, kl_hands_axis_name(ctx.dorsal_axis), ctx.dorsal_score,
               kl_hands_axis_name(ctx.distal_axis), ctx.distal_score, ctx.thumb_dot, o.scale);
        expect_bind(name, hand, &bl, &o, &R, T, hand);
        CHECK(o.high_confidence && o.pinch_bits == 0, "%s: confidence/pinch", name);
        CHECK(fabsf(o.scale - 1) < 0.01f, "%s: scale %.3f", name, o.scale);

        // Curl the index finger 50 deg at each joint, the middle finger's
        // Middle1 30: the output must be the bind rotation turned about Z.
        float curl[KLO_BONES] = { 0 };
        curl[KLO_INDEX1] = 20; curl[KLO_INDEX2] = 50; curl[KLO_INDEX3] = 40; curl[KLO_MIDDLE1] = 30;
        place(&bl, &R, T, hand, &w, curl);
        to_arkit(&w, hand ? &convR : &convL, j);
        kl_hands_to_ovr(hand, j, (1u << KLH_JOINTS) - 1, &ctx, &o);
        for (int b = 2; b < KLO_BONES; b++) {
            M bm, c, e; qmat(bl.bone[b].pose.q, &bm);
            axis_angle(0, 0, 1, curl[b], &c); mmul(&bm, &c, &e);
            float eq[4]; mq(&e, eq);
            float d = qdiff(o.rot[b], eq);
            CHECK(d < 0.5f, "%s curled: %s %.2f deg off", name, kl_hands_bone_name(b), d);
        }

        // A stronger bend (kl_hands_scale_bend): 50 deg at Index2 becomes 65,
        // and a cap holds a joint at 60.
        {
            kl_ovr_hand g = o;
            kl_hands_scale_bend(hand, &g, 1.3f, 100);
            M bm, c, e; qmat(bl.bone[KLO_INDEX2].pose.q, &bm);
            axis_angle(0, 0, 1, 65, &c); mmul(&bm, &c, &e);
            float eq[4]; mq(&e, eq);
            CHECK(qdiff(g.rot[KLO_INDEX2], eq) < 0.5f, "%s: bend x1.3 %.2f deg off", name, qdiff(g.rot[KLO_INDEX2], eq));
            g = o;
            kl_hands_scale_bend(hand, &g, 1.3f, 60);
            axis_angle(0, 0, 1, 60, &c); mmul(&bm, &c, &e); mq(&e, eq);
            CHECK(qdiff(g.rot[KLO_INDEX2], eq) < 0.5f, "%s: bend cap %.2f deg off", name, qdiff(g.rot[KLO_INDEX2], eq));
            CHECK(qdiff(g.rot[KLO_THUMB2], o.rot[KLO_THUMB2]) < 1e-3f, "%s: thumb untouched", name);
        }

        // Fist assist: Index 20+50+40 = 110 deg is under the 120 start and
        // stays; the same finger at 30+60+50 = 140 (of 120..170 -> 250) gets
        // 120 + 20*130/50 = 172 in all, each joint scaled alike.
        {
            kl_ovr_hand g = o;
            kl_hands_fist_assist(hand, NULL, &g, 120, 250, 170);
            CHECK(qdiff(g.rot[KLO_INDEX2], o.rot[KLO_INDEX2]) < 1e-3f, "%s: fist assist below start", name);
        }

        // Pinch: index tip onto the thumb tip.
        place(&bl, &R, T, hand, &w, NULL);
        to_arkit(&w, hand ? &convR : &convL, j);
        memcpy(j[KLH_INDEX_TIP].p, j[KLH_THUMB_TIP].p, sizeof j[KLH_INDEX_TIP].p);
        j[KLH_INDEX_TIP].p[1] += 0.005f;
        kl_hands_to_ovr(hand, j, (1u << KLH_JOINTS) - 1, &ctx, &o);
        CHECK(o.pinch_bits == 0x3 && o.pinch[1] > 0.95f && o.pinch[0] == o.pinch[1],
              "%s: pinch bits %#x strength %.2f", name, o.pinch_bits, o.pinch[1]);

        // Lost: no wrist, no hand.
        CHECK(!kl_hands_to_ovr(hand, j, (1u << KLH_JOINTS) - 2, &ctx, &o), "%s: untracked", name);
    }

    // Before the axis is known the palm's normal stands in: a flat hand still
    // comes out right except for the thumb's twist.
    {
        world_hand w; place(&bl, &R, T, 0, &w, NULL);
        kl_hand_xf j[KLH_JOINTS]; to_arkit(&w, &convL, j);
        kl_hands_ctx ctx; kl_hands_init(&ctx);
        kl_ovr_hand o; kl_hands_to_ovr(0, j, (1u << KLH_JOINTS) - 1, &ctx, &o);
        for (int b = KLO_INDEX1; b <= KLO_PINKY3; b++)
            CHECK(qdiff(o.rot[b], bl.bone[b].pose.q) < 0.5f, "fallback %s", kl_hands_bone_name(b));
    }

    printf("worst flat-hand bone error %.3f deg\n", g_worst);
    if (fails) { printf("t_hands: %d FAILED\n", fails); return 1; }
    printf("t_hands: ok\n");
    return 0;
}
