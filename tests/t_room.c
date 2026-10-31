// The room on the floor (runtime/xr/kl_room.c), on a made-up room: a 5 x 4 m
// box with a table in one corner and a sofa along one wall, its floor and walls
// as the triangles a scene-reconstruction mesh would hand over.
#include "kl_room.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { float *xyz; uint8_t *kind; int n, cap; } tris;

static void tri(tris *t, float ax, float ay, float az, float bx, float by, float bz,
                float cx, float cy, float cz, uint8_t kind) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->xyz = realloc(t->xyz, sizeof(float) * 9 * (size_t)t->cap);
        t->kind = realloc(t->kind, (size_t)t->cap);
    }
    float *v = t->xyz + 9 * t->n;
    v[0] = ax; v[1] = ay; v[2] = az; v[3] = bx; v[4] = by; v[5] = bz; v[6] = cx; v[7] = cy; v[8] = cz;
    t->kind[t->n++] = kind;
}

// A quad split in tiles of about 10 cm, as a mesh would come.
static void quad(tris *t, const float o[3], const float u[3], const float v[3], uint8_t kind) {
    float lu = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    float lv = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    int nu = (int)ceilf(lu / 0.1f), nv = (int)ceilf(lv / 0.1f);
    for (int a = 0; a < nu; a++)
        for (int b = 0; b < nv; b++) {
            float s0 = (float)a / nu, s1 = (float)(a + 1) / nu, t0 = (float)b / nv, t1 = (float)(b + 1) / nv;
            float p[4][3];
            float ss[4] = { s0, s1, s1, s0 }, tt[4] = { t0, t0, t1, t1 };
            for (int k = 0; k < 4; k++)
                for (int d = 0; d < 3; d++) p[k][d] = o[d] + u[d] * ss[k] + v[d] * tt[k];
            tri(t, p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2], kind);
            tri(t, p[0][0], p[0][1], p[0][2], p[2][0], p[2][1], p[2][2], p[3][0], p[3][1], p[3][2], kind);
        }
}

static void box(tris *t, float x0, float z0, float x1, float z1, float h, uint8_t kind) {
    float a[3] = { x0, 0, z0 }, b[3] = { x1, 0, z0 }, c[3] = { x1, 0, z1 }, d[3] = { x0, 0, z1 };
    float up[3] = { 0, h, 0 };
    float ab[3] = { x1 - x0, 0, 0 }, bc[3] = { 0, 0, z1 - z0 }, cd[3] = { x0 - x1, 0, 0 }, da[3] = { 0, 0, z0 - z1 };
    quad(t, a, ab, up, kind); quad(t, b, bc, up, kind); quad(t, c, cd, up, kind); quad(t, d, da, up, kind);
    float top[3] = { x0, h, z0 };
    quad(t, top, ab, bc, kind);
}

// The room, floor at `fy`, seen only where `seen(x, z)` says so (NULL: all).
static tris room(float fy, int (*seen)(float, float)) {
    tris t = {0};
    // floor -2.5..2.5 x -2..2, in 10 cm tiles, the unseen ones left out
    for (float x = -2.5f; x < 2.5f - 1e-4f; x += 0.1f)
        for (float z = -2.0f; z < 2.0f - 1e-4f; z += 0.1f) {
            if (seen && !seen(x + 0.05f, z + 0.05f)) continue;
            tri(&t, x, fy, z, x + 0.1f, fy, z, x + 0.1f, fy, z + 0.1f, KL_SURF_FLOOR);
            tri(&t, x, fy, z, x + 0.1f, fy, z + 0.1f, x, fy, z + 0.1f, KL_SURF_FLOOR);
        }
    float h[3] = { 0, 2.5f, 0 };
    float w0[3] = { -2.5f, fy, -2.0f }, wx[3] = { 5, 0, 0 }, wz[3] = { 0, 0, 4 };
    float w1[3] = { -2.5f, fy, 2.0f }, w2[3] = { 2.5f, fy, -2.0f };
    if (!seen || seen(0, -1.99f)) quad(&t, w0, wx, h, KL_SURF_WALL);
    if (!seen || seen(0, 1.99f)) quad(&t, w1, wx, h, KL_SURF_WALL);
    if (!seen || seen(-2.49f, 0)) quad(&t, w0, wz, h, KL_SURF_WALL);
    if (!seen || seen(2.49f, 0)) quad(&t, w2, wz, h, KL_SURF_WALL);
    // a table in the corner, a sofa along the left wall, offset up by fy
    tris extra = {0};
    box(&extra, 1.4f, 1.0f, 2.5f, 2.0f, 0.75f, KL_SURF_TABLE);
    box(&extra, -2.5f, -1.0f, -1.6f, 1.2f, 0.45f, KL_SURF_SEAT);
    for (int k = 0; k < extra.n; k++) {
        float *v = extra.xyz + 9 * k;
        float cx = (v[0] + v[3] + v[6]) / 3, cz = (v[2] + v[5] + v[8]) / 3;
        if (seen && !seen(cx, cz)) continue;
        tri(&t, v[0], v[1] + fy, v[2], v[3], v[4] + fy, v[5], v[6], v[7] + fy, v[8], extra.kind[k]);
    }
    free(extra.xyz); free(extra.kind);
    // a ceiling, which must be left out
    float c0[3] = { -2.5f, fy + 2.5f, -2.0f };
    quad(&t, c0, wx, wz, KL_SURF_CEILING);
    return t;
}

static float rect_dist(float x, float z, float x0, float z0, float x1, float z1) {
    float dx = fmaxf(fmaxf(x0 - x, x - x1), 0), dz = fmaxf(fmaxf(z0 - z, z - z1), 0);
    return sqrtf(dx * dx + dz * dz);
}

static int front_half(float x, float z) { (void)x; return z < 0.3f; }

static float signed_area(const float *xz, int n) {
    float a = 0;
    for (int k = 0; k < n; k++) {
        int m = (k + 1) % n;
        a += xz[2 * k] * xz[2 * m + 1] - xz[2 * m] * xz[2 * k + 1];
    }
    return 0.5f * a;
}

int main(void) {
    const float fy = -0.03f;
    tris t = room(fy, NULL);

    // --- floor ---
    float f = kl_room_floor_estimate(t.xyz, t.kind, t.n, 99);
    CHECK(fabsf(f - fy) < 0.015f, "floor at %.3f, expected %.3f", f, fy);
    CHECK(kl_room_floor_estimate(t.xyz, NULL, t.n, 7.0f) == 7.0f, "no classes: the fallback");

    // --- the map ---
    kl_room_grid g = kl_room_grid_make(0, 0, 4.0f, 0.05f);
    int32_t n = kl_room_cells(g);
    uint8_t *cells = calloc((size_t)n, 1);
    kl_room_raster_tris(g, cells, t.xyz, t.kind, t.n, f);
    int obst = 0, flo = 0;
    for (int c = 0; c < n; c++) { obst += (cells[c] & KL_ROOM_OBSTACLE) != 0; flo += (cells[c] & KL_ROOM_FLOOR) != 0; }
    CHECK(flo > 7000, "floor cells %d", flo);
    CHECK(obst > 500 && obst < 2600, "obstacle cells %d", obst);
    int32_t ci, cj;
    ci = (int32_t)((0.0f - g.x0) / g.cell); cj = (int32_t)((0.0f - g.z0) / g.cell);
    CHECK(!(cells[cj * g.w + ci] & KL_ROOM_OBSTACLE), "the middle of the room is free");
    ci = (int32_t)((2.0f - g.x0) / g.cell); cj = (int32_t)((1.5f - g.z0) / g.cell);
    CHECK(cells[cj * g.w + ci] & KL_ROOM_OBSTACLE, "the table top is an obstacle");

    // --- coverage ---
    float sec[8];
    int cov = kl_room_coverage(g, cells, 0, 0, 8, sec);
    CHECK(cov >= 95, "all seen: coverage %d", cov);
    tris half = room(fy, front_half);
    uint8_t *hcells = calloc((size_t)n, 1);
    kl_room_raster_tris(g, hcells, half.xyz, half.kind, half.n, f);
    int hcov = kl_room_coverage(g, hcells, 0, 0, 8, sec);
    CHECK(hcov < 75, "half seen: coverage %d", hcov);
    // sectors 1 and 2 point towards +z (45..135 degrees), the unseen half
    CHECK(sec[1] < 0.5f && sec[2] < 0.5f, "+z sectors %.2f %.2f", sec[1], sec[2]);
    CHECK(sec[5] > 0.9f && sec[6] > 0.9f, "-z sectors %.2f %.2f", sec[5], sec[6]);

    // --- the suggestion ---
    uint8_t *mask = calloc((size_t)n, 1);
    int32_t area = kl_room_suggest(g, cells, 0.1f, -0.2f, 0.25f, 4.0f, mask);
    float m2 = area * g.cell * g.cell;
    CHECK(m2 > 6.0f && m2 < 14.0f, "suggested %.2f m2", m2);
    ci = (int32_t)((0.1f - g.x0) / g.cell); cj = (int32_t)((-0.2f - g.z0) / g.cell);
    CHECK(mask[cj * g.w + ci], "the player stands inside");
    // nothing inside closer than 25 cm to an obstacle
    int too_close = 0;
    for (int c = 0; c < n; c++) {
        if (!mask[c]) continue;
        float x = g.x0 + (c % g.w + 0.5f) * g.cell, z = g.z0 + (c / g.w + 0.5f) * g.cell;
        if (rect_dist(x, z, 1.4f, 1.0f, 2.5f, 2.0f) < 0.2f) too_close++;      // the table
        if (rect_dist(x, z, -2.5f, -1.0f, -1.6f, 1.2f) < 0.2f) too_close++;   // the sofa
        if (fabsf(x) > 2.5f - 0.2f || fabsf(z) > 2.0f - 0.2f) too_close++;   // walls
    }
    CHECK(too_close == 0, "%d cells inside are next to something", too_close);
    // the half-seen room suggests only what was seen
    uint8_t *hmask = calloc((size_t)n, 1);
    kl_room_suggest(g, hcells, 0, -0.5f, 0.25f, 4.0f, hmask);
    int beyond = 0;
    for (int c = 0; c < n; c++) if (hmask[c] && g.z0 + (c / g.w + 0.5f) * g.cell > 0.3f) beyond++;
    CHECK(beyond == 0, "%d cells suggested where nobody looked", beyond);

    // --- drawing ---
    uint8_t *sq = calloc((size_t)n, 1);
    float square[8] = { -1, -1, 1, -1, 1, 1, -1, 1 };
    kl_room_fill_polygon(g, sq, square, 4, 1);
    int sqn = kl_room_count(g, sq);
    CHECK(abs(sqn - 1600) <= 80, "2 x 2 m square is %d cells", sqn);
    uint8_t *bite = calloc((size_t)n, 1);
    float notch[8] = { 0.5f, -0.25f, 1.5f, -0.25f, 1.5f, 0.25f, 0.5f, 0.25f };
    kl_room_fill_polygon(g, bite, notch, 4, 1);
    uint8_t *cut = malloc((size_t)n);
    memcpy(cut, sq, (size_t)n);
    kl_room_combine(g, cut, bite, KL_ROOM_SUBTRACT);
    CHECK(abs(kl_room_count(g, cut) - (1600 - 100)) <= 40, "notch cut: %d", kl_room_count(g, cut));
    uint8_t *joined = malloc((size_t)n);
    memcpy(joined, sq, (size_t)n);
    uint8_t *side = calloc((size_t)n, 1);
    float next_to[8] = { 1.04f, -0.5f, 2.0f, -0.5f, 2.0f, 0.5f, 1.04f, 0.5f };   // 4 cm gap
    kl_room_fill_polygon(g, side, next_to, 4, 1);
    kl_room_combine(g, joined, side, KL_ROOM_UNION);
    kl_room_close(g, joined, 0.06f);
    uint8_t *one = malloc((size_t)n);
    memcpy(one, joined, (size_t)n);
    kl_room_keep_main(g, one, 0, 0);
    CHECK(kl_room_count(g, one) == kl_room_count(g, joined), "a 4 cm gap is closed: %d of %d",
          kl_room_count(g, one), kl_room_count(g, joined));

    // a piece 30 cm beside the square is joined to it
    uint8_t *grown = malloc((size_t)n);
    memcpy(grown, sq, (size_t)n);
    uint8_t *apart = calloc((size_t)n, 1);
    float beside[8] = { 1.3f, -0.6f, 2.3f, -0.6f, 2.3f, 0.6f, 1.3f, 0.6f };
    kl_room_fill_polygon(g, apart, beside, 4, 1);
    float gap = kl_room_add_piece(g, grown, apart, 0.6f);
    CHECK(fabsf(gap - 0.3f) < 0.06f, "gap %.2f", gap);
    uint8_t *one2 = malloc((size_t)n);
    memcpy(one2, grown, (size_t)n);
    kl_room_keep_main(g, one2, 0, 0);
    CHECK(kl_room_count(g, one2) == kl_room_count(g, grown), "the piece 30 cm away is joined: %d of %d",
          kl_room_count(g, one2), kl_room_count(g, grown));
    CHECK(kl_room_count(g, grown) > 1600 + 480 + 100, "the gap between them is filled: %d", kl_room_count(g, grown));
    // ...and one 1 m away is not
    memcpy(grown, sq, (size_t)n);
    memset(apart, 0, (size_t)n);
    float far_[8] = { 2.0f, -0.6f, 3.0f, -0.6f, 3.0f, 0.6f, 2.0f, 0.6f };
    kl_room_fill_polygon(g, apart, far_, 4, 1);
    kl_room_add_piece(g, grown, apart, 0.6f);
    memcpy(one2, grown, (size_t)n);
    kl_room_keep_main(g, one2, 0, 0);
    CHECK(kl_room_count(g, one2) < kl_room_count(g, grown), "a piece 1 m away stays apart");

    // no holes: a ring is filled; a hole with something in it is opened instead
    uint8_t *holey = malloc((size_t)n);
    memcpy(holey, sq, (size_t)n);
    float hole2[8] = { -0.3f, -0.3f, 0.3f, -0.3f, 0.3f, 0.3f, -0.3f, 0.3f };
    kl_room_fill_polygon(g, holey, hole2, 4, 0);
    CHECK(kl_room_fill_holes(g, holey, NULL, 0.5f) == 1 && kl_room_count(g, holey) == sqn, "an empty hole is filled");
    kl_room_fill_polygon(g, holey, hole2, 4, 0);
    uint8_t *pillar = calloc((size_t)n, 1);
    pillar[((int)((0.0f - g.z0) / g.cell)) * g.w + (int)((0.0f - g.x0) / g.cell)] = KL_ROOM_OBSTACLE;
    kl_room_fill_holes(g, holey, pillar, 0.5f);
    float hpts[8192]; int32_t hl[16];
    CHECK(kl_room_contours(g, holey, hpts, 4096, hl, 16) == 1, "a hole round a pillar is opened: one outline");
    CHECK(!holey[((int)((0.0f - g.z0) / g.cell)) * g.w + (int)((0.0f - g.x0) / g.cell)], "the pillar stays outside");
    // a line across a bay closes it
    uint8_t *bay = malloc((size_t)n);
    memcpy(bay, sq, (size_t)n);
    float notch2[8] = { -0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 1.2f, -0.4f, 1.2f };   // a bay from the top edge
    kl_room_fill_polygon(g, bay, notch2, 4, 0);
    int baycells = kl_room_count(g, bay);
    float across[4] = { -0.6f, 0.95f, 0.6f, 0.95f };
    kl_room_fill_polyline(g, bay, across, 2, 0.03f, 1);
    kl_room_fill_holes(g, bay, NULL, 0.5f);
    CHECK(kl_room_count(g, bay) > baycells + 150, "a line across the bay closes it: %d -> %d", baycells, kl_room_count(g, bay));

    // --- distance ---
    float *sdf = malloc(sizeof(float) * (size_t)n);
    kl_room_sdf(g, sq, sdf);
    float d0 = kl_room_sdf_at(g, sdf, 0, 0), d1 = kl_room_sdf_at(g, sdf, 0.8f, 0), d2 = kl_room_sdf_at(g, sdf, 1.5f, 0);
    CHECK(fabsf(d0 - 1.0f) < 0.06f, "middle %.3f", d0);
    CHECK(fabsf(d1 - 0.2f) < 0.06f, "20 cm in %.3f", d1);
    CHECK(fabsf(d2 + 0.5f) < 0.06f, "50 cm out %.3f", d2);
    CHECK(kl_room_sdf_at(g, sdf, 40, 0) < -30, "far outside the grid");

    // --- outlines ---
    float pts[8192];
    int32_t lens[16];
    int loops = kl_room_contours(g, sq, pts, 4096, lens, 16);
    CHECK(loops == 1, "one loop round the square, %d", loops);
    if (loops == 1) {
        float a = signed_area(pts, lens[0]);
        CHECK(a > 3.8f && a < 4.2f, "counter-clockwise, %.2f m2", a);
    }
    loops = kl_room_contours(g, cut, pts, 4096, lens, 16);
    CHECK(loops == 1, "the notched square is still one loop, %d", loops);
    // a hole
    uint8_t *ring = malloc((size_t)n);
    memcpy(ring, sq, (size_t)n);
    float hole[8] = { -0.3f, -0.3f, 0.3f, -0.3f, 0.3f, 0.3f, -0.3f, 0.3f };
    kl_room_fill_polygon(g, ring, hole, 4, 0);
    loops = kl_room_contours(g, ring, pts, 4096, lens, 16);
    CHECK(loops == 2, "a ring has two loops, %d", loops);
    if (loops == 2) {
        float a0 = signed_area(pts, lens[0]), a1 = signed_area(pts + 2 * lens[0], lens[1]);
        CHECK((a0 > 0) != (a1 > 0), "the hole runs the other way (%.2f, %.2f)", a0, a1);
    }
    CHECK(kl_room_contours(g, sq, pts, 10, lens, 16) == -1, "too small a buffer says so");

    // --- packing ---
    uint8_t *bits = malloc((size_t)(n + 7) / 8), *back = calloc((size_t)n, 1);
    kl_room_pack(n, cells, KL_ROOM_OBSTACLE, bits);
    kl_room_unpack(n, bits, KL_ROOM_OBSTACLE, back);
    int diff = 0;
    for (int c = 0; c < n; c++) diff += ((cells[c] & KL_ROOM_OBSTACLE) != 0) != (back[c] != 0);
    CHECK(diff == 0, "pack/unpack lost %d cells", diff);

    // --- finding the room again ---
    // Today's scan: the same room seen from the front half only, in a frame
    // turned by 0.6 rad and shifted — the kind of frame a new immersive space has.
    const float yaw = 0.6f, sx = 1.2f, sz = -0.7f;
    float *op = malloc(sizeof(float) * 2 * 4000);
    int32_t no = kl_room_obstacle_points(g, hcells, op, 4000);
    CHECK(no > 200, "obstacle points %d", no);
    // room -> today: p_today = R(-yaw) (p_room - s); the fit must find room = R(yaw) p + t
    float co = cosf(-yaw), si = sinf(-yaw);
    for (int k = 0; k < no; k++) {
        float x = op[2 * k] - sx, z = op[2 * k + 1] - sz;
        op[2 * k] = co * x + si * z;
        op[2 * k + 1] = -si * x + co * z;
    }
    kl_room_fit fit;
    int rc = kl_room_register(g, cells, op, no, &fit);
    CHECK(rc == 0, "register ran");
    // check by mapping the points back
    float err = 0;
    float c2 = cosf(fit.yaw), s2 = sinf(fit.yaw);
    float c0 = cosf(yaw), s0 = sinf(yaw);
    for (int k = 0; k < no; k++) {
        float x = op[2 * k], z = op[2 * k + 1];
        float rx = c2 * x + s2 * z + fit.tx, rz = -s2 * x + c2 * z + fit.tz;
        float ex = c0 * x + s0 * z + sx, ez = -s0 * x + c0 * z + sz;
        err += sqrtf((rx - ex) * (rx - ex) + (rz - ez) * (rz - ez));
    }
    err /= no;
    CHECK(fabsf(remainderf(fit.yaw - yaw, 6.2831853f)) < 0.03f, "yaw %.3f, expected %.3f", fit.yaw, yaw);
    CHECK(err < 0.05f, "mean error %.3f m", err);
    CHECK(fit.score > 0.6f, "score %.2f", fit.score);
    CHECK(fit.score - fit.second > 0.08f, "score %.2f against %.2f", fit.score, fit.second);
    printf("  register: yaw %.3f (%.3f), t (%.2f %.2f), score %.2f, second %.2f, %d points, error %.3f m\n",
           fit.yaw, yaw, fit.tx, fit.tz, fit.score, fit.second, no, err);

    // The same with today's free floor: a room with space in it still fits as well.
    {
        float *fp = malloc(sizeof(float) * 2 * 4000);
        int32_t nfl = kl_room_floor_points(g, hcells, fp, 4000);
        for (int k = 0; k < nfl; k++) {
            float x = fp[2 * k] - sx, z = fp[2 * k + 1] - sz;
            fp[2 * k] = co * x + si * z;
            fp[2 * k + 1] = -si * x + co * z;
        }
        kl_room_fit ff;
        kl_room_register_scene(g, cells, op, no, fp, nfl, &ff);
        CHECK(fabsf(remainderf(ff.yaw - yaw, 6.2831853f)) < 0.03f && ff.score > 0.6f && ff.score - ff.second > 0.08f,
              "with the floor: yaw %.3f, score %.2f against %.2f", ff.yaw, ff.score, ff.second);
        printf("  register with the floor: yaw %.3f, score %.2f, second %.2f, %d floor points\n",
               ff.yaw, ff.score, ff.second, nfl);
        free(fp);
    }

    // From a guess 20 degrees and 40 cm off (an anchor that came back wrong).
    kl_room_fit guess = fit, near;
    guess.yaw += 0.35f; guess.tx += 0.3f; guess.tz -= 0.25f;
    CHECK(kl_room_register_near(g, cells, op, no, &guess, 0.4f, 0.6f, &near) == 0, "register_near ran");
    CHECK(fabsf(remainderf(near.yaw - yaw, 6.2831853f)) < 0.03f && near.score > 0.6f,
          "from a guess: yaw %.3f, score %.2f", near.yaw, near.score);

    // A different room does not fit: an L-shaped one 6 x 6 with nothing in it.
    tris other = {0};
    float hh[3] = { 0, 2.5f, 0 };
    float L[7][2] = { { -3, -3 }, { 3, -3 }, { 3, 0 }, { 0, 0 }, { 0, 3 }, { -3, 3 }, { -3, -3 } };
    for (int k = 0; k < 6; k++) {
        float o[3] = { L[k][0], 0, L[k][1] }, u[3] = { L[k + 1][0] - L[k][0], 0, L[k + 1][1] - L[k][1] };
        quad(&other, o, u, hh, KL_SURF_WALL);
    }
    uint8_t *ocells = calloc((size_t)n, 1);
    kl_room_raster_tris(g, ocells, other.xyz, other.kind, other.n, 0);
    int32_t no2 = kl_room_obstacle_points(g, ocells, op, 4000);
    kl_room_fit fit2;
    kl_room_register(g, cells, op, no2, &fit2);
    CHECK(fit2.score < 0.55f, "another room scores %.2f", fit2.score);
    printf("  another room: score %.2f, second %.2f\n", fit2.score, fit2.second);

    // --- a room full of things (2026-10-17, his headset) ---
    // Nearly the whole grid is obstacle; only an L of free floor round the
    // player is not. The obstacles alone fit placements 25 cm off almost as well
    // as the right one; with today's free floor the right one stands out.
    {
        kl_room_grid cg = kl_room_grid_make(0, 0, 3, 0.05f);
        int32_t cn = kl_room_cells(cg);
        uint8_t *full = calloc((size_t)cn, 1), *today = calloc((size_t)cn, 1);
        for (int32_t c = 0; c < cn; c++) {
            float x = cg.x0 + ((float)(c % cg.w) + 0.5f) * cg.cell, z = cg.z0 + ((float)(c / cg.w) + 0.5f) * cg.cell;
            int freec = (x > -0.8f && x < 0.8f && z > -0.6f && z < 0.6f)      // where the player stands
                     || (x > 0.8f && x < 1.5f && z > -0.6f && z < 0.0f)       // a bay to one side
                     || (x > -0.3f && x < 0.2f && z > 0.6f && z < 1.6f);      // a gangway to the door
            full[c] = freec ? KL_ROOM_FLOOR : KL_ROOM_OBSTACLE;
        }
        // Today: the same room in a frame turned by 0.5 rad and shifted, seen within 2.6 m.
        const float cy = 0.5f, ctx = 0.3f, ctz = -0.2f;
        float cc = cosf(cy), cs = sinf(cy);
        for (int32_t c = 0; c < cn; c++) {
            float x = cg.x0 + ((float)(c % cg.w) + 0.5f) * cg.cell, z = cg.z0 + ((float)(c / cg.w) + 0.5f) * cg.cell;
            if (x * x + z * z > 2.6f * 2.6f) continue;
            float rx = cc * x + cs * z + ctx, rz = -cs * x + cc * z + ctz;
            int32_t i = (int32_t)floorf((rx - cg.x0) / cg.cell), j = (int32_t)floorf((rz - cg.z0) / cg.cell);
            if (i < 0 || j < 0 || i >= cg.w || j >= cg.h) continue;
            today[c] = full[j * cg.w + i];
        }
        float *cp = malloc(sizeof(float) * 2 * 1500), *fp = malloc(sizeof(float) * 2 * 1500);
        int32_t cno = kl_room_obstacle_points(cg, today, cp, 1500);
        int32_t cnf = kl_room_floor_points(cg, today, fp, 1500);
        CHECK(cno > 1000 && cnf > 300, "cluttered: %d obstacle and %d floor points", cno, cnf);
        kl_room_fit only, both;
        kl_room_register(cg, full, cp, cno, &only);
        kl_room_register_scene(cg, full, cp, cno, fp, cnf, &both);
        printf("  cluttered room, obstacles only: yaw %.3f (%.3f), score %.2f, second %.2f\n",
               only.yaw, cy, only.score, only.second);
        printf("  cluttered room, with the floor: yaw %.3f (%.3f), t (%.2f %.2f), score %.2f, second %.2f\n",
               both.yaw, cy, both.tx, both.tz, both.score, both.second);
        CHECK(only.score - only.second < 0.1f, "the case seen on the headset: obstacles alone %.2f against %.2f",
              only.score, only.second);
        CHECK(fabsf(remainderf(both.yaw - cy, 6.2831853f)) < 0.03f && fabsf(both.tx - ctx) < 0.06f
              && fabsf(both.tz - ctz) < 0.06f, "with the floor: yaw %.3f t (%.2f %.2f)", both.yaw, both.tx, both.tz);
        CHECK(both.score >= 0.6f && both.score - both.second >= 0.1f,
              "with the floor the right placement stands out: %.2f against %.2f", both.score, both.second);
        // and from a guess (an anchor), the same
        kl_room_fit g2 = both, nb;
        g2.yaw += 0.2f; g2.tx -= 0.25f;
        kl_room_register_near_scene(cg, full, cp, cno, fp, cnf, &g2, 0.4f, 0.6f, &nb);
        // within 3 degrees: the free floor is a small target, about 5 cm at 1.5 m
        CHECK(fabsf(remainderf(nb.yaw - cy, 6.2831853f)) < 0.05f && nb.score >= 0.6f,
              "near, with the floor: yaw %.3f score %.2f", nb.yaw, nb.score);
        free(full); free(today); free(cp); free(fp);
    }

    // --- a mesh as ARKit hands it over: 16-byte vertices, 16-bit indices ---
    float verts[4][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 1, 0 }, { 0, 0, 1, 0 } };
    uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    uint8_t cls[2] = { KL_SURF_FLOOR, KL_SURF_TABLE };
    float mv[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0.5f, -1, 1 };   // moved by (2, 0.5, -1)
    float mo[18];
    uint8_t mk[2];
    int mt = kl_room_mesh_tris(verts, 16, 4, idx, 2, 2, cls, 1, 1, mv, mo, mk);
    CHECK(mt == 2 && mk[0] == KL_SURF_FLOOR && mk[1] == KL_SURF_TABLE, "mesh faces %d", mt);
    CHECK(fabsf(mo[3] - 3.0f) < 1e-6f && fabsf(mo[4] - 0.5f) < 1e-6f && fabsf(mo[5] + 1.0f) < 1e-6f,
          "vertex 1 moved to (%.2f %.2f %.2f)", mo[3], mo[4], mo[5]);
    uint16_t bad[3] = { 0, 1, 9 };
    CHECK(kl_room_mesh_tris(verts, 16, 4, bad, 2, 1, NULL, 1, 1, mv, mo, mk) == 0, "an index past the end is dropped");

    // --- the room as a box, from a scan's wall planes ---
    // A 4.2 x 3.4 m room turned by 20 degrees, middle (0.5, -0.3), 2.5 m high.
    // Two pieces of the +u wall, a window in the -u wall (also a wall plane), the
    // side of a wardrobe standing 0.6 m in front of the +v wall but beside the
    // play area, and no -v wall at all. The play area: 2 x 1.6 m in the middle.
    {
        const float yaw = 0.349066f, mx = 0.5f, mz = -0.3f, hu = 2.1f, hv = 1.7f;
        const float ux = cosf(yaw), uz = sinf(yaw), vx = -uz, vz = ux;
        kl_room_wallplane w[8];
        int nw = 0;
#define WALL(du, dv, nu, nv, wid, hgt, y) do { \
            w[nw].cx = mx + (du) * ux + (dv) * vx; w[nw].cz = mz + (du) * uz + (dv) * vz; w[nw].cy = (y); \
            w[nw].nx = (nu) * ux + (nv) * vx; w[nw].nz = (nu) * uz + (nv) * vz; \
            w[nw].width = (wid); w[nw].height = (hgt); nw++; } while (0)
        WALL( hu, -0.8f, -1, 0, 1.4f, 2.5f, 1.25f);     // +u wall, two pieces
        WALL( hu,  0.9f, -1, 0, 1.2f, 2.3f, 1.15f);
        WALL(-hu,  0.0f,  1, 0, 3.0f, 2.5f, 1.25f);     // -u wall
        WALL(-hu,  0.3f,  1, 0, 1.0f, 1.2f, 1.30f);     // the window in it
        WALL(0.9f, hv - 0.6f, 0, -1, 0.6f, 2.0f, 1.0f); // the wardrobe's side, small
        WALL(-0.3f, hv,  0, -1, 3.2f, 2.5f, 1.25f);     // +v wall
#undef WALL
        kl_room_grid bg = kl_room_grid_make(mx, mz, 3.0f, 0.05f);
        uint8_t *pm = calloc((size_t)kl_room_cells(bg), 1);
        float poly[8];
        const float pu[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, pv[4] = { -0.8f, -0.8f, 0.8f, 0.8f };
        for (int k = 0; k < 4; k++) { poly[2 * k] = mx + pu[k] * ux + pv[k] * vx; poly[2 * k + 1] = mz + pu[k] * uz + pv[k] * vz; }
        kl_room_fill_polygon(bg, pm, poly, 4, 1);
        kl_room_box b;
        int seen = kl_room_box_from_walls(w, nw, bg, pm, &b);
        double dyaw = remainder((double)b.yaw - yaw, 1.5707963);
        CHECK(seen == 3, "box: %d sides seen (want 3)", seen);
        CHECK(fabs(dyaw) < 0.02, "box: yaw %.3f (want %.3f)", b.yaw, yaw);
        // Taken along its own axes, which may be the room's quarter-turned.
        int swap = fabs(remainder((double)b.yaw - yaw, 3.1415927)) > 0.7;
        float bu = swap ? b.half_v : b.half_u, bv = swap ? b.half_u : b.half_v;
        CHECK(fabsf(bu - hu) < 0.05f, "box: half along the long side %.2f (want %.2f)", bu, hu);
        // -v not seen: just beyond the play area (0.8 + 0.3), the +v wall at 1.7
        CHECK(fabsf(bv - (hv + 1.1f) / 2) < 0.06f, "box: half across %.2f (want %.2f)", bv, (hv + 1.1f) / 2);
        CHECK(fabsf(b.height - 2.5f) < 0.01f, "box: height %.2f", b.height);
        int wardrobe = 4;
        for (int s = 0; s < 4; s++) CHECK(b.side[s] != wardrobe && b.side[s] != 3, "box: side %d stands on plane %d", s, b.side[s]);
        // The planes, into a frame moved by (1, 0, 2): the floor's middle there, walls facing in.
        float mm[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 2, 1 };
        int32_t kinds[6];
        float pl[96], ex[12];
        CHECK(kl_room_box_planes(&b, mm, kinds, pl, ex) == 6, "box: six planes");
        CHECK(kinds[0] == 0 && kinds[1] == 1 && kinds[2] == 2 && kinds[5] == 2, "box: kinds");
        CHECK(fabsf(pl[12] - (b.cx + 1)) < 1e-5f && fabsf(pl[14] - (b.cz + 2)) < 1e-5f && fabsf(pl[9] - 1) < 1e-5f,
              "box: floor at (%.2f %.2f), normal y %.2f", pl[12], pl[14], pl[9]);
        CHECK(fabsf(pl[16 + 9] + 1) < 1e-5f && fabsf(pl[16 + 13] - b.height) < 1e-5f, "box: ceiling faces down at its height");
        for (int s = 0; s < 4; s++) {
            const float *q = pl + 16 * (2 + s);
            // into the room: from the wall towards the floor's middle
            float tx = (b.cx + 1) - q[12], tz = (b.cz + 2) - q[14];
            CHECK(q[8] * tx + q[10] * tz > 0 && fabsf(q[5] - 1) < 1e-5f && fabsf(q[13] - b.height / 2) < 1e-5f,
                  "box: wall %d faces in and stands upright", s);
            // right-handed: X x Y = Z
            float cx = q[1] * q[6] - q[2] * q[5], cy = q[2] * q[4] - q[0] * q[6], cz = q[0] * q[5] - q[1] * q[4];
            CHECK(fabsf(cx - q[8]) < 1e-4f && fabsf(cy - q[9]) < 1e-4f && fabsf(cz - q[10]) < 1e-4f, "box: wall %d frame", s);
        }
        CHECK(fabsf(ex[0] * ex[1] - 4 * b.half_u * b.half_v) < 1e-4f, "box: floor size");
        // No walls at all: the play area's own axes, a box just round it.
        kl_room_box b0;
        CHECK(kl_room_box_from_walls(w, 0, bg, pm, &b0) == 0, "box without walls: none seen");
        CHECK(fabs(remainder((double)b0.yaw - yaw, 1.5707963)) < 0.03, "box without walls: yaw %.3f", b0.yaw);
        CHECK(fabsf(b0.half_u * b0.half_v - 1.3f * 1.1f) < 0.08f, "box without walls: %.2f x %.2f", b0.half_u, b0.half_v);
        CHECK(kl_room_box_from_walls(w, 0, bg, NULL, &b0) == -1, "box from nothing");
        printf("box: yaw %.1f deg, %.2f x %.2f m, %.2f high, centre (%.2f %.2f), sides %d %d %d %d\n",
               b.yaw * 57.29578f, 2 * b.half_u, 2 * b.half_v, b.height, b.cx, b.cz, b.side[0], b.side[1], b.side[2], b.side[3]);
        free(pm);
    }

    if (fails) { printf("t_room: %d FAILED\n", fails); return 1; }
    printf("t_room: ok (suggested %.1f m2, coverage %d%% / %d%%)\n", m2, cov, hcov);
    return 0;
}
