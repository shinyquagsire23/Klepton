// The room on the floor: scan map, suggested play area, drawn areas, the
// distance to the area's edge, and finding a room measured before. See
// kl_room.h for what each call promises; this file is the how.
#include "kl_room.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KLR_INF 1e20f

kl_room_grid kl_room_grid_make(float cx, float cz, float half, float cell) {
    kl_room_grid g;
    g.cell = cell > 0.005f ? cell : 0.05f;
    int32_t n = (int32_t)ceilf(2.0f * half / g.cell);
    if (n < 4) n = 4;
    if (n > 1024) n = 1024;
    g.w = g.h = n;
    g.x0 = cx - 0.5f * (float)n * g.cell;
    g.z0 = cz - 0.5f * (float)n * g.cell;
    return g;
}

static inline int klr_cell(kl_room_grid g, float x, float z, int32_t *i, int32_t *j) {
    float fi = (x - g.x0) / g.cell, fj = (z - g.z0) / g.cell;
    if (!(fi >= 0.0f) || !(fj >= 0.0f)) return 0;
    int32_t a = (int32_t)fi, b = (int32_t)fj;
    if (a >= g.w || b >= g.h) return 0;
    *i = a; *j = b;
    return 1;
}
static inline float klr_cx(kl_room_grid g, int32_t i) { return g.x0 + ((float)i + 0.5f) * g.cell; }
static inline float klr_cz(kl_room_grid g, int32_t j) { return g.z0 + ((float)j + 0.5f) * g.cell; }

// --- The floor's height ------------------------------------------------------

float kl_room_floor_estimate(const float *xyz, const uint8_t *kinds, int32_t ntris, float fallback) {
    // 2 cm bins from -3 m to +3 m, weighted by area.
    enum { NB = 300 };
    float hist[NB];
    memset(hist, 0, sizeof hist);
    float total = 0.0f;
    for (int32_t t = 0; t < ntris; t++) {
        if (!kinds || kinds[t] != KL_SURF_FLOOR) continue;
        const float *v = xyz + 9 * t;
        float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2];
        float bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        float area = 0.5f * sqrtf(nx * nx + ny * ny + nz * nz);
        float y = (v[1] + v[4] + v[7]) / 3.0f;
        int b = (int)floorf((y + 3.0f) / 0.02f);
        if (b < 0 || b >= NB) continue;
        hist[b] += area;
        total += area;
    }
    if (total < 0.2f) return fallback;
    int best = 0;
    for (int b = 1; b < NB; b++) if (hist[b] > hist[best]) best = b;
    // The weighted middle of the peak and its neighbours.
    float w = 0.0f, s = 0.0f;
    for (int b = best - 1; b <= best + 1; b++) {
        if (b < 0 || b >= NB) continue;
        w += hist[b];
        s += hist[b] * (-3.0f + ((float)b + 0.5f) * 0.02f);
    }
    return w > 0.0f ? s / w : fallback;
}

// --- Rasterising the mesh ----------------------------------------------------

void kl_room_raster_tris(kl_room_grid g, uint8_t *cells, const float *xyz, const uint8_t *kinds,
                         int32_t ntris, float floor_y) {
    const float lo = floor_y + 0.10f, hi = floor_y + 1.90f;
    for (int32_t t = 0; t < ntris; t++) {
        const float *v = xyz + 9 * t;
        uint8_t kind = kinds ? kinds[t] : KL_SURF_NONE;
        if (kind == KL_SURF_CEILING) continue;
        float ax = v[3] - v[0], ay = v[4] - v[1], az = v[5] - v[2];
        float bx = v[6] - v[0], by = v[7] - v[1], bz = v[8] - v[2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-9f) continue;
        float ymin = fminf(v[1], fminf(v[4], v[7])), ymax = fmaxf(v[1], fmaxf(v[4], v[7]));
        if (ymin > hi) continue;    // above head height: a shelf, a lamp, the ceiling
        int floorish = kind == KL_SURF_FLOOR
            || (fabsf(ny / nl) > 0.9f && ymin > floor_y - 0.08f && ymax < floor_y + 0.07f);
        if (floorish && ymax > floor_y + 0.09f) floorish = 0;   // "floor" on top of something
        if (!floorish && ymax < lo) continue;                   // a threshold, a rug's edge
        // Points over the triangle at half a cell apart, so that a wall seen
        // edge-on (its footprint a line) still marks every cell it passes.
        float e0 = sqrtf(ax * ax + az * az), e1 = sqrtf(bx * bx + bz * bz);
        float cxv = v[6] - v[3], czv = v[8] - v[5];
        float e2 = sqrtf(cxv * cxv + czv * czv);
        float vy = fmaxf(fabsf(ay), fabsf(by));
        float longest = fmaxf(fmaxf(e0, e1), fmaxf(e2, vy));
        int steps = (int)ceilf(longest / (0.5f * g.cell));
        if (steps < 1) steps = 1;
        if (steps > 64) steps = 64;
        uint8_t bit = floorish ? KL_ROOM_FLOOR : KL_ROOM_OBSTACLE;
        for (int a = 0; a <= steps; a++) {
            for (int b = 0; a + b <= steps; b++) {
                float u = (float)a / (float)steps, w = (float)b / (float)steps;
                float px = v[0] + ax * u + bx * w;
                float py = v[1] + ay * u + by * w;
                float pz = v[2] + az * u + bz * w;
                if (!floorish && (py < lo || py > hi)) continue;
                int32_t i, j;
                if (klr_cell(g, px, pz, &i, &j)) cells[j * g.w + i] |= bit;
            }
        }
    }
}

// --- How much has been looked at ----------------------------------------------

int32_t kl_room_coverage(kl_room_grid g, const uint8_t *cells, float ux, float uz,
                         int32_t nsectors, float *sector_frac) {
    if (nsectors < 1) nsectors = 1;
    if (nsectors > 64) nsectors = 64;
    const int per = 12;
    const int rays = nsectors * per;
    int answered_all = 0;
    int answered[64] = {0};
    const float step = 0.5f * g.cell, rmax = 3.0f, gap_max = 0.25f;
    for (int r = 0; r < rays; r++) {
        float th = 6.2831853f * ((float)r + 0.5f) / (float)rays;
        float dx = cosf(th), dz = sinf(th);
        float gap = 0.0f;
        int ok = 0;
        float last_known = 0.0f;
        for (float d = 0.25f; d <= rmax; d += step) {
            int32_t i, j;
            if (!klr_cell(g, ux + dx * d, uz + dz * d, &i, &j)) { ok = d - last_known < gap_max; break; }
            uint8_t c = cells[j * g.w + i];
            if (c & KL_ROOM_OBSTACLE) { ok = 1; break; }
            if (c & KL_ROOM_FLOOR) { gap = 0.0f; last_known = d; continue; }
            gap += step;
            if (gap > gap_max) break;
        }
        if (!ok && last_known >= rmax - gap_max) ok = 1;
        if (ok) { answered_all++; answered[r / per]++; }
    }
    if (sector_frac)
        for (int k = 0; k < nsectors; k++) sector_frac[k] = (float)answered[k] / (float)per;
    return (int32_t)(100.0f * (float)answered_all / (float)rays + 0.5f);
}

// --- Distance transform ------------------------------------------------------
//
// Felzenszwalb and Huttenlocher's exact squared Euclidean transform, in cells,
// separably: columns, then rows. `f` holds 0 at the cells measured from and
// KLR_INF elsewhere; it is overwritten with the squared distance.

static void klr_dt1(const float *f, float *d, int n, int *v, double *z) {
    int k = 0;
    v[0] = 0; z[0] = -1e30; z[1] = 1e30;
    for (int q = 1; q < n; q++) {
        double s = (((double)f[q] + (double)q * q) - ((double)f[v[k]] + (double)v[k] * v[k]))
                   / (2.0 * (double)(q - v[k]));
        while (s <= z[k]) {
            k--;
            s = (((double)f[q] + (double)q * q) - ((double)f[v[k]] + (double)v[k] * v[k]))
                / (2.0 * (double)(q - v[k]));
        }
        k++;
        v[k] = q; z[k] = s; z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (double)q) k++;
        double dq = (double)(q - v[k]);
        double r = dq * dq + (double)f[v[k]];
        d[q] = r > KLR_INF ? KLR_INF : (float)r;
    }
}

static void klr_edt(int w, int h, float *f) {
    int n = w > h ? w : h;
    float *col = malloc(sizeof(float) * (size_t)n), *out = malloc(sizeof(float) * (size_t)n);
    double *z = malloc(sizeof(double) * (size_t)(n + 1));
    int *v = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) col[j] = f[j * w + i];
        klr_dt1(col, out, h, v, z);
        for (int j = 0; j < h; j++) f[j * w + i] = out[j];
    }
    for (int j = 0; j < h; j++) {
        klr_dt1(f + j * w, out, w, v, z);
        memcpy(f + j * w, out, sizeof(float) * (size_t)w);
    }
    free(col); free(out); free(z); free(v);
}

/// Distance in metres from every cell to the nearest cell for which `is_src`
/// is true. A grid with none is KLR_INF everywhere.
static float *klr_dist(kl_room_grid g, const uint8_t *m, uint8_t bits, int want) {
    int32_t n = kl_room_cells(g);
    float *f = malloc(sizeof(float) * (size_t)n);
    int any = 0;
    for (int32_t c = 0; c < n; c++) {
        int on = (m[c] & bits) != 0;
        f[c] = on == want ? 0.0f : KLR_INF;
        any |= on == want;
    }
    if (!any) return f;
    klr_edt(g.w, g.h, f);
    for (int32_t c = 0; c < n; c++) f[c] = sqrtf(f[c]) * g.cell;
    return f;
}

// --- Pieces ------------------------------------------------------------------

/// Label the 4-connected pieces of `in`; returns the number, labels from 1.
static int32_t klr_label(kl_room_grid g, const uint8_t *in, int32_t *lab) {
    int32_t n = kl_room_cells(g), next = 0;
    int32_t *stack = malloc(sizeof(int32_t) * (size_t)n);
    memset(lab, 0, sizeof(int32_t) * (size_t)n);
    for (int32_t s = 0; s < n; s++) {
        if (!in[s] || lab[s]) continue;
        next++;
        int32_t top = 0;
        stack[top++] = s; lab[s] = next;
        while (top) {
            int32_t c = stack[--top];
            int32_t i = c % g.w, j = c / g.w;
            int32_t nb[4] = { i > 0 ? c - 1 : -1, i < g.w - 1 ? c + 1 : -1,
                              j > 0 ? c - g.w : -1, j < g.h - 1 ? c + g.w : -1 };
            for (int k = 0; k < 4; k++) {
                int32_t d = nb[k];
                if (d >= 0 && in[d] && !lab[d]) { lab[d] = next; stack[top++] = d; }
            }
        }
    }
    free(stack);
    return next;
}

void kl_room_keep_main(kl_room_grid g, uint8_t *mask, float ux, float uz) {
    int32_t n = kl_room_cells(g);
    int32_t *lab = malloc(sizeof(int32_t) * (size_t)n);
    int32_t pieces = klr_label(g, mask, lab);
    if (pieces > 1) {
        int32_t keep = 0, i, j;
        if (klr_cell(g, ux, uz, &i, &j)) keep = lab[j * g.w + i];
        if (!keep) {
            int32_t *size = calloc((size_t)pieces + 1, sizeof(int32_t));
            for (int32_t c = 0; c < n; c++) size[lab[c]]++;
            for (int32_t p = 1; p <= pieces; p++) if (!keep || size[p] > size[keep]) keep = p;
            free(size);
        }
        for (int32_t c = 0; c < n; c++) mask[c] = lab[c] == keep;
    }
    free(lab);
}

// --- The suggestion ------------------------------------------------------------

int32_t kl_room_suggest(kl_room_grid g, const uint8_t *cells, float ux, float uz,
                        float clearance, float max_radius, uint8_t *mask) {
    int32_t n = kl_room_cells(g);
    // Pinholes in the floor mesh are floor: a cell nobody saw with seen floor
    // on most sides of it.
    uint8_t *c2 = malloc((size_t)n);
    memcpy(c2, cells, (size_t)n);
    for (int pass = 0; pass < 2; pass++) {
        for (int32_t j = 1; j < g.h - 1; j++)
            for (int32_t i = 1; i < g.w - 1; i++) {
                int32_t c = j * g.w + i;
                if (c2[c]) continue;
                int fl = 0;
                for (int dj = -1; dj <= 1; dj++)
                    for (int di = -1; di <= 1; di++)
                        fl += (c2[c + dj * g.w + di] & KL_ROOM_FLOOR) != 0;
                if (fl >= 5) c2[c] |= KL_ROOM_FLOOR;
            }
    }
    float *d_obst = klr_dist(g, c2, KL_ROOM_OBSTACLE, 1);
    float *d_unknown = klr_dist(g, c2, KL_ROOM_FLOOR | KL_ROOM_OBSTACLE, 0);
    uint8_t *free_ = malloc((size_t)n);
    for (int32_t c = 0; c < n; c++) {
        int32_t i = c % g.w, j = c / g.w;
        float dx = klr_cx(g, i) - ux, dz = klr_cz(g, j) - uz;
        free_[c] = (c2[c] & KL_ROOM_FLOOR) && !(c2[c] & KL_ROOM_OBSTACLE)
                   && d_obst[c] >= clearance && d_unknown[c] >= 0.10f
                   && dx * dx + dz * dz <= max_radius * max_radius;
    }
    // No passage narrower than 0.6 m: what stays of the free floor 0.3 m in
    // from its edge, the piece of that where the player is, grown back by
    // 0.3 m inside the free floor.
    const float open = 0.30f;
    float *d_edge = klr_dist(g, free_, 1, 0);
    uint8_t *core = malloc((size_t)n);
    for (int32_t c = 0; c < n; c++) core[c] = free_[c] && d_edge[c] > open;
    int32_t *lab = malloc(sizeof(int32_t) * (size_t)n);
    int32_t pieces = klr_label(g, core, lab);
    int32_t keep = 0;
    if (pieces > 0) {
        // The piece under the player, else the nearest one within 1.5 m, else
        // the largest within reach.
        int32_t i, j;
        if (klr_cell(g, ux, uz, &i, &j)) keep = lab[j * g.w + i];
        if (!keep) {
            float best = 1.5f * 1.5f;
            for (int32_t c = 0; c < n; c++) {
                if (!lab[c]) continue;
                float dx = klr_cx(g, c % g.w) - ux, dz = klr_cz(g, c / g.w) - uz;
                float d2 = dx * dx + dz * dz;
                if (d2 < best) { best = d2; keep = lab[c]; }
            }
        }
        if (!keep) {
            int32_t *size = calloc((size_t)pieces + 1, sizeof(int32_t));
            for (int32_t c = 0; c < n; c++) size[lab[c]]++;
            for (int32_t p = 1; p <= pieces; p++) if (!keep || size[p] > size[keep]) keep = p;
            free(size);
        }
    }
    int32_t count = 0;
    if (keep) {
        for (int32_t c = 0; c < n; c++) core[c] = lab[c] == keep;
        float *d_core = klr_dist(g, core, 1, 1);
        for (int32_t c = 0; c < n; c++) {
            mask[c] = free_[c] && d_core[c] <= open + 0.5f * g.cell;
        }
        free(d_core);
        kl_room_keep_main(g, mask, ux, uz);
        // No holes: the scan's gaps are floor; round something standing inside,
        // the area is opened to the outside (a Quest boundary has none either).
        kl_room_fill_holes(g, mask, c2, 0.5f);
        kl_room_keep_main(g, mask, ux, uz);
        count = kl_room_count(g, mask);
    } else {
        memset(mask, 0, (size_t)n);
    }
    free(c2); free(d_obst); free(d_unknown); free(free_); free(d_edge); free(core); free(lab);
    return count;
}

// --- Drawn areas ---------------------------------------------------------------

void kl_room_fill_polygon(kl_room_grid g, uint8_t *mask, const float *xz, int32_t n, uint8_t value) {
    if (n < 3) return;
    float *xs = malloc(sizeof(float) * (size_t)n);
    for (int32_t j = 0; j < g.h; j++) {
        float z = klr_cz(g, j);
        int32_t k = 0;
        for (int32_t e = 0; e < n; e++) {
            float ax = xz[2 * e], az = xz[2 * e + 1];
            float bx = xz[2 * ((e + 1) % n)], bz = xz[2 * ((e + 1) % n) + 1];
            if ((az <= z && bz > z) || (bz <= z && az > z)) {
                float t = (z - az) / (bz - az);
                xs[k++] = ax + t * (bx - ax);
            }
        }
        // insertion sort: a handful a row
        for (int32_t a = 1; a < k; a++) {
            float v = xs[a]; int32_t b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < k; a += 2) {
            int32_t i0 = (int32_t)ceilf((xs[a] - g.x0) / g.cell - 0.5f);
            int32_t i1 = (int32_t)floorf((xs[a + 1] - g.x0) / g.cell - 0.5f);
            if (i0 < 0) i0 = 0;
            if (i1 > g.w - 1) i1 = g.w - 1;
            for (int32_t i = i0; i <= i1; i++) mask[j * g.w + i] = value;
        }
    }
    free(xs);
}

void kl_room_combine(kl_room_grid g, uint8_t *dst, const uint8_t *src, int32_t op) {
    int32_t n = kl_room_cells(g);
    for (int32_t c = 0; c < n; c++) {
        switch (op) {
        case KL_ROOM_UNION:     dst[c] = dst[c] || src[c]; break;
        case KL_ROOM_SUBTRACT:  dst[c] = dst[c] && !src[c]; break;
        default:                dst[c] = dst[c] && src[c]; break;
        }
    }
}

float kl_room_add_piece(kl_room_grid g, uint8_t *area, const uint8_t *piece, float max_gap) {
    int32_t n = kl_room_cells(g);
    if (kl_room_count(g, area) == 0) {
        for (int32_t c = 0; c < n; c++) area[c] = area[c] || piece[c];
        return -1.0f;
    }
    float *d_area = klr_dist(g, area, 1, 1), *d_piece = klr_dist(g, piece, 1, 1);
    float gap = KLR_INF;
    for (int32_t c = 0; c < n; c++) if (piece[c] && d_area[c] < gap) gap = d_area[c];
    // The lens between the two: cells whose distances to both add up to little
    // more than the gap — along two edges that run side by side, all of it.
    const float slack = 0.08f;
    for (int32_t c = 0; c < n; c++) {
        int on = area[c] || piece[c];
        if (!on && gap <= max_gap && d_area[c] + d_piece[c] <= gap + slack) on = 1;
        area[c] = (uint8_t)on;
    }
    free(d_area); free(d_piece);
    return gap >= KLR_INF * 0.5f ? -1.0f : gap;
}

void kl_room_fill_polyline(kl_room_grid g, uint8_t *mask, const float *xz, int32_t n, float half_width,
                           uint8_t value) {
    for (int32_t s = 0; s + 1 < n || (n == 1 && s == 0); s++) {
        float ax = xz[2 * s], az = xz[2 * s + 1];
        float bx = n > 1 ? xz[2 * s + 2] : ax, bz = n > 1 ? xz[2 * s + 3] : az;
        float x0 = fminf(ax, bx) - half_width, x1 = fmaxf(ax, bx) + half_width;
        float z0 = fminf(az, bz) - half_width, z1 = fmaxf(az, bz) + half_width;
        int32_t i0 = (int32_t)floorf((x0 - g.x0) / g.cell), i1 = (int32_t)floorf((x1 - g.x0) / g.cell);
        int32_t j0 = (int32_t)floorf((z0 - g.z0) / g.cell), j1 = (int32_t)floorf((z1 - g.z0) / g.cell);
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 > g.w - 1) i1 = g.w - 1;
        if (j1 > g.h - 1) j1 = g.h - 1;
        float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
        for (int32_t j = j0; j <= j1; j++)
            for (int32_t i = i0; i <= i1; i++) {
                float px = klr_cx(g, i) - ax, pz = klr_cz(g, j) - az;
                float t = l2 > 0 ? (px * dx + pz * dz) / l2 : 0;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                float ex = px - t * dx, ez = pz - t * dz;
                if (ex * ex + ez * ez <= half_width * half_width) mask[j * g.w + i] = value;
            }
    }
}

int32_t kl_room_fill_holes(kl_room_grid g, uint8_t *mask, const uint8_t *cells, float channel) {
    int32_t n = kl_room_cells(g), handled = 0;
    uint8_t *outside = malloc((size_t)n);
    int32_t *lab = malloc(sizeof(int32_t) * (size_t)n);
    for (int32_t c = 0; c < n; c++) outside[c] = !mask[c];
    int32_t pieces = klr_label(g, outside, lab);
    if (pieces > 1) {
        uint8_t *border = calloc((size_t)pieces + 1, 1), *solid = calloc((size_t)pieces + 1, 1);
        for (int32_t c = 0; c < n; c++) {
            if (!lab[c]) continue;
            int32_t i = c % g.w, j = c / g.w;
            if (i == 0 || j == 0 || i == g.w - 1 || j == g.h - 1) border[lab[c]] = 1;
            if (cells && (cells[c] & KL_ROOM_OBSTACLE)) solid[lab[c]] = 1;
        }
        // The outer floor, for the channels.
        uint8_t *outer = calloc((size_t)n, 1);
        for (int32_t c = 0; c < n; c++) outer[c] = lab[c] && border[lab[c]];
        float *d_outer = NULL;
        for (int32_t p = 1; p <= pieces; p++) {
            if (border[p]) continue;
            handled++;
            if (!solid[p]) {
                for (int32_t c = 0; c < n; c++) if (lab[c] == p) mask[c] = 1;
                continue;
            }
            // Something stands in this hole: open it to the outside the shortest way.
            if (!d_outer) d_outer = klr_dist(g, outer, 1, 1);
            int32_t h = -1;
            for (int32_t c = 0; c < n; c++)
                if (lab[c] == p && (h < 0 || d_outer[c] < d_outer[h])) h = c;
            if (h < 0) continue;
            float hx = klr_cx(g, h % g.w), hz = klr_cz(g, h / g.w);
            int32_t o = -1;
            float best = 1e30f;
            for (int32_t c = 0; c < n; c++) {
                if (!outer[c]) continue;
                float dx = klr_cx(g, c % g.w) - hx, dz = klr_cz(g, c / g.w) - hz;
                float d2 = dx * dx + dz * dz;
                if (d2 < best) { best = d2; o = c; }
            }
            if (o < 0) continue;
            float seg[4] = { hx, hz, klr_cx(g, o % g.w), klr_cz(g, o / g.w) };
            kl_room_fill_polyline(g, mask, seg, 2, 0.5f * channel, 0);
        }
        free(border); free(solid); free(outer); free(d_outer);
    }
    free(outside); free(lab);
    return handled;
}

void kl_room_close(kl_room_grid g, uint8_t *mask, float radius) {
    int32_t n = kl_room_cells(g);
    float *d_in = klr_dist(g, mask, 1, 1);
    uint8_t *grown = malloc((size_t)n);
    for (int32_t c = 0; c < n; c++) grown[c] = d_in[c] <= radius;
    float *d_out = klr_dist(g, grown, 1, 0);
    for (int32_t c = 0; c < n; c++) mask[c] = d_out[c] > radius;
    free(d_in); free(grown); free(d_out);
}

int32_t kl_room_count(kl_room_grid g, const uint8_t *mask) {
    int32_t n = kl_room_cells(g), k = 0;
    for (int32_t c = 0; c < n; c++) k += mask[c] != 0;
    return k;
}

// --- Distance to the edge -------------------------------------------------------

void kl_room_sdf(kl_room_grid g, const uint8_t *mask, float *sdf) {
    int32_t n = kl_room_cells(g);
    float *to_in = klr_dist(g, mask, 1, 1);     // 0 inside
    float *to_out = klr_dist(g, mask, 1, 0);    // 0 outside
    const float half = 0.5f * g.cell;
    for (int32_t c = 0; c < n; c++) {
        if (mask[c]) {
            float d = to_out[c] >= KLR_INF * 0.5f ? 1e3f : to_out[c];
            // A grid's own edge is an edge of the area too.
            int32_t i = c % g.w, j = c / g.w;
            float e = fminf(fminf((float)i, (float)(g.w - 1 - i)), fminf((float)j, (float)(g.h - 1 - j)));
            d = fminf(d, (e + 1.0f) * g.cell);
            sdf[c] = d - half;
        } else {
            float d = to_in[c] >= KLR_INF * 0.5f ? 1e3f : to_in[c];
            sdf[c] = -(d - half);
        }
    }
    free(to_in); free(to_out);
}

float kl_room_sdf_at(kl_room_grid g, const float *sdf, float x, float z) {
    float fi = (x - g.x0) / g.cell - 0.5f, fj = (z - g.z0) / g.cell - 0.5f;
    float wx = (float)g.w * g.cell, wz = (float)g.h * g.cell;
    if (fi < -0.5f || fj < -0.5f || fi > (float)g.w - 0.5f || fj > (float)g.h - 0.5f) {
        float dx = fmaxf(fmaxf(g.x0 - x, x - (g.x0 + wx)), 0.0f);
        float dz = fmaxf(fmaxf(g.z0 - z, z - (g.z0 + wz)), 0.0f);
        return -sqrtf(dx * dx + dz * dz) - 1.0f;
    }
    if (fi < 0) fi = 0;
    if (fj < 0) fj = 0;
    if (fi > (float)(g.w - 1)) fi = (float)(g.w - 1);
    if (fj > (float)(g.h - 1)) fj = (float)(g.h - 1);
    int32_t i = (int32_t)fi, j = (int32_t)fj;
    int32_t i1 = i + 1 < g.w ? i + 1 : i, j1 = j + 1 < g.h ? j + 1 : j;
    float tx = fi - (float)i, tz = fj - (float)j;
    float a = sdf[j * g.w + i], b = sdf[j * g.w + i1];
    float c = sdf[j1 * g.w + i], d = sdf[j1 * g.w + i1];
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz;
}

// --- Outlines --------------------------------------------------------------------
//
// Marching squares over the cell centres, the grid padded with one empty cell
// all round so every outline closes. Corners of a square: c0 (i, j), c1 (i+1, j),
// c2 (i+1, j+1), c3 (i, j+1); its edges e0 c0-c1, e1 c1-c2, e2 c3-c2, e3 c0-c3.
// Each case's segments run with the inside on their left; a saddle is two
// corners apart.

static const int8_t klr_seg[16][4] = {
    {-1,-1,-1,-1}, {0,3,-1,-1}, {1,0,-1,-1}, {1,3,-1,-1},
    {2,1,-1,-1},   {0,3,2,1},   {2,0,-1,-1}, {2,3,-1,-1},
    {3,2,-1,-1},   {0,2,-1,-1}, {1,0,3,2},   {1,2,-1,-1},
    {3,1,-1,-1},   {0,1,-1,-1}, {3,0,-1,-1}, {-1,-1,-1,-1},
};

int32_t kl_room_contours(kl_room_grid g, const uint8_t *mask, float *xz, int32_t cap_pts,
                         int32_t *lens, int32_t cap_loops) {
    const int32_t W = g.w + 2, H = g.h + 2;
    const int32_t ne = W * H * 2;
    int32_t *next = malloc(sizeof(int32_t) * (size_t)ne);
    for (int32_t e = 0; e < ne; e++) next[e] = -1;
    // H(i, j): between centres (i, j) and (i+1, j); V(i, j): (i, j) and (i, j+1).
#define KLR_H(i, j) ((((j) + 1) * W + ((i) + 1)) * 2)
#define KLR_V(i, j) ((((j) + 1) * W + ((i) + 1)) * 2 + 1)
#define KLR_AT(i, j) ((i) >= 0 && (j) >= 0 && (i) < g.w && (j) < g.h && mask[(j) * g.w + (i)])
    for (int32_t j = -1; j < g.h; j++)
        for (int32_t i = -1; i < g.w; i++) {
            int k = (KLR_AT(i, j) ? 1 : 0) | (KLR_AT(i + 1, j) ? 2 : 0)
                  | (KLR_AT(i + 1, j + 1) ? 4 : 0) | (KLR_AT(i, j + 1) ? 8 : 0);
            int32_t eid[4] = { KLR_H(i, j), KLR_V(i + 1, j), KLR_H(i, j + 1), KLR_V(i, j) };
            for (int s = 0; s < 4 && klr_seg[k][s] >= 0; s += 2)
                next[eid[klr_seg[k][s]]] = eid[klr_seg[k][s + 1]];
        }
    int32_t loops = 0, pts = 0;
    for (int32_t e0 = 0; e0 < ne; e0++) {
        if (next[e0] < 0) continue;
        if (loops >= cap_loops) { free(next); return -1; }
        int32_t start = pts, e = e0;
        while (e >= 0 && next[e] >= 0) {
            if (pts >= cap_pts) { free(next); return -1; }
            int32_t cell = e / 2, vert = e & 1;
            int32_t i = cell % W - 1, j = cell / W - 1;
            float x = vert ? klr_cx(g, i) : g.x0 + ((float)i + 1.0f) * g.cell;
            float z = vert ? g.z0 + ((float)j + 1.0f) * g.cell : klr_cz(g, j);
            xz[2 * pts] = x; xz[2 * pts + 1] = z;
            pts++;
            int32_t nx = next[e];
            next[e] = -1;
            e = nx;
        }
        if (pts - start >= 3) lens[loops++] = pts - start;
        else pts = start;
    }
#undef KLR_H
#undef KLR_V
#undef KLR_AT
    free(next);
    return loops;
}

// Cell centres whose bits under `mask` are exactly `want`, evenly thinned to at most `cap`.
static int32_t klr_points(kl_room_grid g, const uint8_t *cells, uint8_t mask, uint8_t want,
                          float *xz, int32_t cap) {
    int32_t n = kl_room_cells(g), total = 0;
    for (int32_t c = 0; c < n; c++) total += (cells[c] & mask) == want;
    if (total == 0 || cap <= 0) return 0;
    int32_t k = 0, seen = 0;
    for (int32_t c = 0; c < n && k < cap; c++) {
        if ((cells[c] & mask) != want) continue;
        // keep point `seen` when it starts a new 1/cap-th of the list
        if ((int64_t)seen * cap / total >= k) {
            xz[2 * k] = klr_cx(g, c % g.w);
            xz[2 * k + 1] = klr_cz(g, c / g.w);
            k++;
        }
        seen++;
    }
    return k;
}

int32_t kl_room_obstacle_points(kl_room_grid g, const uint8_t *cells, float *xz, int32_t cap) {
    return klr_points(g, cells, KL_ROOM_OBSTACLE, KL_ROOM_OBSTACLE, xz, cap);
}

int32_t kl_room_floor_points(kl_room_grid g, const uint8_t *cells, float *xz, int32_t cap) {
    return klr_points(g, cells, KL_ROOM_FLOOR | KL_ROOM_OBSTACLE, KL_ROOM_FLOOR, xz, cap);
}

// --- Finding a room measured before ------------------------------------------------

typedef struct { float yaw, cx, cz, score; } klr_cand;

static float klr_score(kl_room_grid g, const float *field, const float *q, int32_t n, float tx, float tz) {
    float s = 0.0f;
    const float inv = 1.0f / g.cell;
    for (int32_t k = 0; k < n; k++) {
        int32_t i = (int32_t)((q[2 * k] + tx - g.x0) * inv);
        int32_t j = (int32_t)((q[2 * k + 1] + tz - g.z0) * inv);
        if ((uint32_t)i < (uint32_t)g.w && (uint32_t)j < (uint32_t)g.h) s += field[j * g.w + i];
    }
    return s / (float)n;
}

// The free floor of today's scan, `nf` points in `qf`, against the room's
// obstacles: the share that lands where nothing stood (`clear`, 1 there, 0 on an
// obstacle). Outside the room's grid counts half: nothing is known there.
static float klr_free(kl_room_grid g, const float *clear, const float *qf, int32_t nf, float tx, float tz) {
    if (nf <= 0) return 0.0f;
    float s = 0.0f;
    const float inv = 1.0f / g.cell;
    for (int32_t k = 0; k < nf; k++) {
        int32_t i = (int32_t)((qf[2 * k] + tx - g.x0) * inv);
        int32_t j = (int32_t)((qf[2 * k + 1] + tz - g.z0) * inv);
        s += ((uint32_t)i < (uint32_t)g.w && (uint32_t)j < (uint32_t)g.h) ? clear[j * g.w + i] : 0.5f;
    }
    return s / (float)nf;
}

// Why the floor counts too (2026-10-17, his headset): in a small room full of
// things the obstacles cover nearly the whole grid, so the obstacle points of
// today fit almost anywhere near the right place — Beat Saber found a room
// measured in Walkabout at a score of 1.00 eight times running and still
// refused it, the next placement 25 cm off scoring 0.94..0.98. What tells such
// placements apart is the free floor: shifted, part of it lands on furniture.
// The score is the product of the two shares (both have to fit; a mean let the
// saturated obstacle half hide half of what the floor says); without floor
// points it is the obstacles' alone, as before.
#ifndef KLR_CLEAR_C
#define KLR_CLEAR_C 0.08f
#endif
#ifndef KLR_CLEAR_F
#define KLR_CLEAR_F 0.04f
#endif
typedef struct {
    const float *field, *clear;     // obstacle closeness, floor clearance
    const float *q, *qf;            // today's points, turned
    int32_t n, nf;
} klr_set;

static float klr_score2(kl_room_grid g, const klr_set *s, float tx, float tz) {
    float so = klr_score(g, s->field, s->q, s->n, tx, tz);
    if (s->nf <= 0) return so;
    return so * klr_free(g, s->clear, s->qf, s->nf, tx, tz);
}

// Clearance from the obstacles' distance field: 0 on an obstacle, rising
// softly (over about `sigma` metres) to 1 away from it — soft so that the fine
// search still has a slope to climb.
static float *klr_clear(kl_room_grid g, const float *d, float sigma) {
    int32_t ncell = kl_room_cells(g);
    float *c = malloc(sizeof(float) * (size_t)ncell);
    for (int32_t k = 0; k < ncell; k++) c[k] = 1.0f - expf(-d[k] * d[k] / (2.0f * sigma * sigma));
    return c;
}

static void klr_rotate(const float *p, int32_t n, float cx, float cz, float yaw, float *q, int32_t stride) {
    float c = cosf(yaw), s = sinf(yaw);
    for (int32_t k = 0, m = 0; m < n; k += stride, m++) {
        float x = p[2 * k] - cx, z = p[2 * k + 1] - cz;
        // a positive turn about +y: +x goes towards -z
        q[2 * m] = c * x + s * z;
        q[2 * m + 1] = -s * x + c * z;
    }
}

int32_t kl_room_register_near_scene(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                                    const float *floor, int32_t nfloor,
                                    const kl_room_fit *guess, float yaw_range, float t_range, kl_room_fit *out) {
    memset(out, 0, sizeof *out);
    out->npts = n;
    int32_t ncell = kl_room_cells(g), nobst = 0;
    for (int32_t c = 0; c < ncell; c++) nobst += (cells[c] & KL_ROOM_OBSTACLE) != 0;
    if (n < 40 || nobst < 40) return -1;
    if (!floor) nfloor = 0;
    float *d = klr_dist(g, cells, KL_ROOM_OBSTACLE, 1);
    float *coarse = malloc(sizeof(float) * (size_t)ncell), *fine = malloc(sizeof(float) * (size_t)ncell);
    const float sc = 0.15f, sf = 0.07f;
    for (int32_t c = 0; c < ncell; c++) {
        coarse[c] = expf(-d[c] * d[c] / (2.0f * sc * sc));
        fine[c] = expf(-d[c] * d[c] / (2.0f * sf * sf));
    }
    float *clear_c = klr_clear(g, d, KLR_CLEAR_C), *clear_f = klr_clear(g, d, KLR_CLEAR_F);
    float cx = 0, cz = 0;
    for (int32_t k = 0; k < n; k++) { cx += pts[2 * k]; cz += pts[2 * k + 1]; }
    cx /= (float)n; cz /= (float)n;
    // Where the guess puts the centroid.
    float co = cosf(guess->yaw), si = sinf(guess->yaw);
    float gcx = co * cx + si * cz + guess->tx, gcz = -si * cx + co * cz + guess->tz;
    int32_t nc = n < 150 ? n : 150, cstride = n / nc;
    int32_t nf = n < 800 ? n : 800, fstride = n / nf;
    int32_t mc = nfloor < 150 ? nfloor : 150, mcs = mc ? nfloor / mc : 1;
    int32_t mf = nfloor < 800 ? nfloor : 800, mfs = mf ? nfloor / mf : 1;
    float *q = malloc(sizeof(float) * 2 * (size_t)(nf > nc ? nf : nc));
    float *qf = malloc(sizeof(float) * 2 * (size_t)(mf > 1 ? mf : 1));
    klr_cand top = { guess->yaw, gcx, gcz, -1 };
    int na = (int)ceilf(yaw_range / 0.026f), nt = (int)ceilf(t_range / 0.06f);
    for (int a = -na; a <= na; a++) {
        float yaw = guess->yaw + (float)a * 0.026f;
        klr_rotate(pts, nc, cx, cz, yaw, q, cstride);
        if (mc) klr_rotate(floor, mc, cx, cz, yaw, qf, mcs);
        klr_set set = { coarse, clear_c, q, qf, nc, mc };
        for (int dz = -nt; dz <= nt; dz++)
            for (int dx = -nt; dx <= nt; dx++) {
                float tx = gcx + (float)dx * 0.06f, tz = gcz + (float)dz * 0.06f;
                float s = klr_score2(g, &set, tx, tz);
                if (s > top.score) top = (klr_cand){ yaw, tx, tz, s };
            }
    }
    klr_cand best = top;
    best.score = -1;
    for (int da = -6; da <= 6; da++) {
        float yaw = top.yaw + (float)da * 0.00436f;
        klr_rotate(pts, nf, cx, cz, yaw, q, fstride);
        if (mf) klr_rotate(floor, mf, cx, cz, yaw, qf, mfs);
        klr_set set = { fine, clear_f, q, qf, nf, mf };
        for (int dz = -4; dz <= 4; dz++)
            for (int dx = -4; dx <= 4; dx++) {
                float tx = top.cx + (float)dx * 0.015f, tz = top.cz + (float)dz * 0.015f;
                float s = klr_score2(g, &set, tx, tz);
                if (s > best.score) best = (klr_cand){ yaw, tx, tz, s };
            }
    }
    float yaw = remainderf(best.yaw, 6.2831853f);
    co = cosf(yaw); si = sinf(yaw);
    out->yaw = yaw;
    out->tx = best.cx - (co * cx + si * cz);
    out->tz = best.cz - (-si * cx + co * cz);
    out->score = best.score;
    free(d); free(coarse); free(fine); free(clear_c); free(clear_f); free(q); free(qf);
    return 0;
}

int32_t kl_room_register_near(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                              const kl_room_fit *guess, float yaw_range, float t_range, kl_room_fit *out) {
    return kl_room_register_near_scene(g, cells, pts, n, NULL, 0, guess, yaw_range, t_range, out);
}

int32_t kl_room_register_scene(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                               const float *floor, int32_t nfloor, kl_room_fit *out) {
    memset(out, 0, sizeof *out);
    out->npts = n;
    int32_t ncell = kl_room_cells(g), nobst = 0;
    float bx0 = 1e9f, bz0 = 1e9f, bx1 = -1e9f, bz1 = -1e9f;
    for (int32_t c = 0; c < ncell; c++) {
        if (!(cells[c] & KL_ROOM_OBSTACLE)) continue;
        nobst++;
        float x = klr_cx(g, c % g.w), z = klr_cz(g, c / g.w);
        if (x < bx0) bx0 = x;
        if (x > bx1) bx1 = x;
        if (z < bz0) bz0 = z;
        if (z > bz1) bz1 = z;
    }
    if (n < 40 || nobst < 40) return -1;
    if (!floor) nfloor = 0;
    float *d = klr_dist(g, cells, KL_ROOM_OBSTACLE, 1);
    float *coarse = malloc(sizeof(float) * (size_t)ncell), *fine = malloc(sizeof(float) * (size_t)ncell);
    const float sc = 0.22f, sf = 0.07f;
    for (int32_t c = 0; c < ncell; c++) {
        coarse[c] = expf(-d[c] * d[c] / (2.0f * sc * sc));
        fine[c] = expf(-d[c] * d[c] / (2.0f * sf * sf));
    }
    float *clear_c = klr_clear(g, d, KLR_CLEAR_C), *clear_f = klr_clear(g, d, KLR_CLEAR_F);
    float cx = 0, cz = 0;
    for (int32_t k = 0; k < n; k++) { cx += pts[2 * k]; cz += pts[2 * k + 1]; }
    cx /= (float)n; cz /= (float)n;

    // Coarse: every 3 degrees, the centroid on a 12 cm lattice over the
    // obstacles' extent, with at most 150 of the points (and 150 of the floor's).
    int32_t nc = n < 150 ? n : 150, cstride = n / nc;
    int32_t nf = n < 800 ? n : 800, fstride = n / nf;
    int32_t mc = nfloor < 150 ? nfloor : 150, mcs = mc ? nfloor / mc : 1;
    int32_t mf = nfloor < 800 ? nfloor : 800, mfs = mf ? nfloor / mf : 1;
    float *q = malloc(sizeof(float) * 2 * (size_t)(nf > nc ? nf : nc));
    float *qf = malloc(sizeof(float) * 2 * (size_t)(mf > 1 ? mf : 1));
    enum { KEEP = 10 };
    klr_cand best[KEEP];
    int32_t nbest = 0;
    const float tstep = 0.12f;
    for (int a = 0; a < 120; a++) {
        float yaw = (float)a * 6.2831853f / 120.0f;
        klr_rotate(pts, nc, cx, cz, yaw, q, cstride);
        if (mc) klr_rotate(floor, mc, cx, cz, yaw, qf, mcs);
        klr_set set = { coarse, clear_c, q, qf, nc, mc };
        for (float tz = bz0 - 0.3f; tz <= bz1 + 0.3f; tz += tstep)
            for (float tx = bx0 - 0.3f; tx <= bx1 + 0.3f; tx += tstep) {
                float s = klr_score2(g, &set, tx, tz);
                if (nbest == KEEP && s <= best[KEEP - 1].score) continue;
                // one per neighbourhood: replace a near one if this is better
                int near = -1;
                for (int b = 0; b < nbest; b++) {
                    float dy = fabsf(remainderf(best[b].yaw - yaw, 6.2831853f));
                    float dx = best[b].cx - tx, dz = best[b].cz - tz;
                    if (dy < 0.27f && dx * dx + dz * dz < 0.5f * 0.5f) { near = b; break; }
                }
                if (near >= 0) {
                    if (s <= best[near].score) continue;
                    best[near] = (klr_cand){ yaw, tx, tz, s };
                } else if (nbest < KEEP) {
                    best[nbest++] = (klr_cand){ yaw, tx, tz, s };
                } else {
                    best[KEEP - 1] = (klr_cand){ yaw, tx, tz, s };
                }
                for (int b = nbest - 1; b > 0 && best[b].score > best[b - 1].score; b--) {
                    klr_cand t = best[b]; best[b] = best[b - 1]; best[b - 1] = t;
                }
            }
    }
    // Fine: half a degree and 2 cm round each, with the sharp field.
    klr_cand fineb[KEEP];
    for (int b = 0; b < nbest; b++) {
        klr_cand top = best[b];
        top.score = -1;
        for (int da = -8; da <= 8; da++) {
            float yaw = best[b].yaw + (float)da * 0.00873f;
            klr_rotate(pts, nf, cx, cz, yaw, q, fstride);
            if (mf) klr_rotate(floor, mf, cx, cz, yaw, qf, mfs);
            klr_set set = { fine, clear_f, q, qf, nf, mf };
            for (int dz = -9; dz <= 9; dz++)
                for (int dx = -9; dx <= 9; dx++) {
                    float tx = best[b].cx + (float)dx * 0.02f, tz = best[b].cz + (float)dz * 0.02f;
                    float s = klr_score2(g, &set, tx, tz);
                    if (s > top.score) top = (klr_cand){ yaw, tx, tz, s };
                }
        }
        fineb[b] = top;
    }
    int win = 0;
    for (int b = 1; b < nbest; b++) if (fineb[b].score > fineb[win].score) win = b;
    float second = 0.0f;
    for (int b = 0; b < nbest; b++) {
        if (b == win) continue;
        float dy = fabsf(remainderf(fineb[b].yaw - fineb[win].yaw, 6.2831853f));
        float dx = fineb[b].cx - fineb[win].cx, dz = fineb[b].cz - fineb[win].cz;
        if (dy < 0.09f && dx * dx + dz * dz < 0.25f * 0.25f) continue;   // the same placement
        if (fineb[b].score > second) second = fineb[b].score;
    }
    // room = R (p - c) + t  =  R p + (t - R c)
    float yaw = remainderf(fineb[win].yaw, 6.2831853f);
    float co = cosf(yaw), si = sinf(yaw);
    out->yaw = yaw;
    out->tx = fineb[win].cx - (co * cx + si * cz);
    out->tz = fineb[win].cz - (-si * cx + co * cz);
    out->score = fineb[win].score;
    out->second = second;
    free(d); free(coarse); free(fine); free(clear_c); free(clear_f); free(q); free(qf);
    return 0;
}

int32_t kl_room_register(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                         kl_room_fit *out) {
    return kl_room_register_scene(g, cells, pts, n, NULL, 0, out);
}

// --- Meshes ------------------------------------------------------------------------

static inline void klr_xf(const float m[16], const float *p, float *o) {
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

int32_t kl_room_mesh_tris(const void *verts, int32_t vstride, int32_t nverts,
                          const void *idx, int32_t bpi, int32_t nfaces,
                          const void *cls, int32_t cbytes, int32_t cstride,
                          const float m[16], float *xyz, uint8_t *kinds) {
    const uint8_t *vb = verts, *ib = idx, *cb = cls;
    int32_t out = 0;
    for (int32_t f = 0; f < nfaces; f++) {
        uint32_t ix[3];
        for (int k = 0; k < 3; k++) {
            if (bpi == 2) { uint16_t v; memcpy(&v, ib + (size_t)(3 * f + k) * 2, 2); ix[k] = v; }
            else { uint32_t v; memcpy(&v, ib + (size_t)(3 * f + k) * 4, 4); ix[k] = v; }
        }
        if (ix[0] >= (uint32_t)nverts || ix[1] >= (uint32_t)nverts || ix[2] >= (uint32_t)nverts) continue;
        for (int k = 0; k < 3; k++) {
            float p[3];
            memcpy(p, vb + (size_t)ix[k] * (size_t)vstride, sizeof p);
            klr_xf(m, p, xyz + 9 * out + 3 * k);
        }
        uint8_t kind = KL_SURF_NONE;
        if (cb) {
            if (cbytes == 4) { uint32_t v; memcpy(&v, cb + (size_t)f * (size_t)cstride, 4); kind = (uint8_t)v; }
            else kind = cb[(size_t)f * (size_t)cstride];
        }
        kinds[out++] = kind;
    }
    return out;
}

void kl_room_transform_tris(float *xyz, int32_t ntris, const float m[16]) {
    for (int32_t k = 0; k < 3 * ntris; k++) {
        float o[3];
        klr_xf(m, xyz + 3 * k, o);
        memcpy(xyz + 3 * k, o, sizeof o);
    }
}

// --- The room as a box --------------------------------------------------------------

int32_t kl_room_box_from_walls(const kl_room_wallplane *walls, int32_t n, kl_room_grid g,
                               const uint8_t *play, kl_room_box *out) {
    if (!out) return -1;
    if (n < 0) n = 0;
    // The play area's middle and its spread, from its cells.
    double sx = 0, sz = 0, sxx = 0, szz = 0, sxz = 0;
    int32_t np = 0;
    if (play) {
        for (int32_t j = 0; j < g.h; j++)
            for (int32_t i = 0; i < g.w; i++) {
                if (!play[j * g.w + i]) continue;
                double x = g.x0 + (i + 0.5) * g.cell, z = g.z0 + (j + 0.5) * g.cell;
                sx += x; sz += z; sxx += x * x; szz += z * z; sxz += x * z; np++;
            }
    }
    // Planes too small to be a wall are not counted at all.
    double c4 = 0, s4 = 0, wx = 0, wz = 0, wa = 0;
    for (int32_t k = 0; k < n; k++) {
        double a = (double)walls[k].width * walls[k].height;
        if (a < 0.3) continue;
        double th = atan2(walls[k].nz, walls[k].nx);
        c4 += a * cos(4 * th); s4 += a * sin(4 * th);
        wx += a * walls[k].cx; wz += a * walls[k].cz; wa += a;
    }
    if (!np && wa <= 0) return -1;
    double mx = np ? sx / np : wx / wa, mz = np ? sz / np : wz / wa;
    double yaw;
    if (wa > 0 && (c4 * c4 + s4 * s4) > 1e-6) {
        yaw = atan2(s4, c4) / 4;
    } else if (np) {
        double vxx = sxx / np - mx * mx, vzz = szz / np - mz * mz, vxz = sxz / np - mx * mz;
        yaw = 0.5 * atan2(2 * vxz, vxx - vzz);
    } else {
        yaw = 0;
    }
    double ux = cos(yaw), uz = sin(yaw), vx = -uz, vz = ux;
    // How far the play area reaches on each side of its middle: +u, -u, +v, -v.
    double reach[4] = { 0, 0, 0, 0 };
    if (np) {
        for (int32_t j = 0; j < g.h; j++)
            for (int32_t i = 0; i < g.w; i++) {
                if (!play[j * g.w + i]) continue;
                double x = g.x0 + (i + 0.5) * g.cell - mx, z = g.z0 + (j + 0.5) * g.cell - mz;
                double pu = x * ux + z * uz, pv = x * vx + z * vz, h = 0.5 * g.cell;
                if (pu + h > reach[0]) reach[0] = pu + h;
                if (-pu + h > reach[1]) reach[1] = -pu + h;
                if (pv + h > reach[2]) reach[2] = pv + h;
                if (-pv + h > reach[3]) reach[3] = -pv + h;
            }
    }
    const double dir[4][2] = { { ux, uz }, { -ux, -uz }, { vx, vz }, { -vx, -vz } };
    double off[4], top = 0;
    int32_t seen = 0;
    for (int s = 0; s < 4; s++) {
        int32_t best = -1;
        double best_a = 0, best_o = 0;
        for (int32_t k = 0; k < n; k++) {
            double a = (double)walls[k].width * walls[k].height;
            if (a < 0.3) continue;
            // Facing into the room from this side: its normal against the way out.
            double nl = sqrt((double)walls[k].nx * walls[k].nx + (double)walls[k].nz * walls[k].nz);
            if (nl < 1e-6) continue;
            double facing = -(walls[k].nx * dir[s][0] + walls[k].nz * dir[s][1]) / nl;
            if (facing < 0.906) continue;                       // within 25 degrees
            double o = (walls[k].cx - mx) * dir[s][0] + (walls[k].cz - mz) * dir[s][1];
            if (o < reach[s] - 0.15) continue;                  // it would cut into the play area
            if (a > best_a || (a == best_a && o < best_o)) { best = k; best_a = a; best_o = o; }
        }
        out->side[s] = best;
        if (best >= 0) {
            off[s] = best_o;
            double t = walls[best].cy + 0.5 * walls[best].height;
            if (t > top) top = t;
            seen++;
        } else {
            off[s] = reach[s] + 0.3;                            // just beyond the play area
        }
        if (off[s] < 0.5) off[s] = 0.5;
    }
    double cu = (off[0] - off[1]) / 2, cv = (off[2] - off[3]) / 2;
    out->cx = (float)(mx + cu * ux + cv * vx);
    out->cz = (float)(mz + cu * uz + cv * vz);
    out->yaw = (float)yaw;
    out->half_u = (float)((off[0] + off[1]) / 2);
    out->half_v = (float)((off[2] + off[3]) / 2);
    out->height = (float)(top >= 2.0 && top <= 4.0 ? top : top > 4.0 ? 4.0 : 2.6);
    return seen;
}

// A plane frame: X and the normal Z given, Y = Z x X; position p.
static void klr_frame(float *o, const double X[3], const double Z[3], const double p[3]) {
    double Y[3] = { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
    const double *c[3] = { X, Y, Z };
    for (int k = 0; k < 3; k++) { o[4 * k] = (float)c[k][0]; o[4 * k + 1] = (float)c[k][1]; o[4 * k + 2] = (float)c[k][2]; o[4 * k + 3] = 0; }
    o[12] = (float)p[0]; o[13] = (float)p[1]; o[14] = (float)p[2]; o[15] = 1;
}
static void klr_mul(const float a[16], const float b[16], float o[16]) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[4 * c + r] = a[r] * b[4 * c] + a[4 + r] * b[4 * c + 1] + a[8 + r] * b[4 * c + 2] + a[12 + r] * b[4 * c + 3];
    memcpy(o, t, sizeof t);
}

int32_t kl_room_box_planes(const kl_room_box *b, const float m[16], int32_t *kinds, float *m16, float *ext2) {
    double ux = cos(b->yaw), uz = sin(b->yaw), vx = -uz, vz = ux, h = b->height;
    double u[3] = { ux, 0, uz }, up[3] = { 0, 1, 0 }, down[3] = { 0, -1, 0 };
    double mid[3] = { b->cx, 0, b->cz }, ceil_at[3] = { b->cx, h, b->cz };
    float f[16];
    klr_frame(f, u, up, mid);
    klr_mul(m, f, m16);
    kinds[0] = 0; ext2[0] = 2 * b->half_u; ext2[1] = 2 * b->half_v;
    klr_frame(f, u, down, ceil_at);
    klr_mul(m, f, m16 + 16);
    kinds[1] = 1; ext2[2] = 2 * b->half_u; ext2[3] = 2 * b->half_v;
    // The walls: on each side, normal pointing back into the room.
    const double out_dir[4][2] = { { ux, uz }, { -ux, -uz }, { vx, vz }, { -vx, -vz } };
    const double half[4] = { b->half_u, b->half_u, b->half_v, b->half_v };
    const double along[4] = { b->half_v, b->half_v, b->half_u, b->half_u };
    for (int s = 0; s < 4; s++) {
        double Z[3] = { -out_dir[s][0], 0, -out_dir[s][1] };
        double X[3] = { Z[2], 0, -Z[0] };                       // Y (up) x Z
        double p[3] = { b->cx + half[s] * out_dir[s][0], h / 2, b->cz + half[s] * out_dir[s][1] };
        klr_frame(f, X, Z, p);
        klr_mul(m, f, m16 + 16 * (2 + s));
        kinds[2 + s] = 2; ext2[2 * (2 + s)] = (float)(2 * along[s]); ext2[2 * (2 + s) + 1] = (float)h;
    }
    return 6;
}

// --- Storage -----------------------------------------------------------------------

void kl_room_pack(int32_t ncells, const uint8_t *mask, uint8_t bit, uint8_t *bits) {
    memset(bits, 0, (size_t)(ncells + 7) / 8);
    for (int32_t c = 0; c < ncells; c++)
        if (mask[c] & bit) bits[c >> 3] |= (uint8_t)(1u << (c & 7));
}

void kl_room_unpack(int32_t ncells, const uint8_t *bits, uint8_t bit, uint8_t *mask) {
    for (int32_t c = 0; c < ncells; c++)
        if (bits[c >> 3] & (1u << (c & 7))) mask[c] |= bit;
}
