// The room the player stands in, flat on the floor: what the start window's
// "Measure room" scan saw, the play area found or drawn in it, and the
// distance to that area's edge that the game's virtual wall is drawn from.
//
// Everything here is a square grid lying on the floor (x across, z the other
// way, metres, the room's own frame with the floor at y = 0). The Swift side
// (KleptonRoom*.swift) owns ARKit and the drawing; this file owns the
// arithmetic, in C because the app's Swift is built unoptimised and a scan map
// is tens of thousands of cells touched several times a second. No state: every
// call takes the grid and the arrays it works on. tests/t_room.c runs it on the
// Mac.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_room_grid {
    float   cell;       // metres per cell
    float   x0, z0;     // the outer corner of cell (0, 0)
    int32_t w, h;       // cells across x and z
} kl_room_grid;

// What a cell of a scan map holds (bits).
enum {
    KL_ROOM_FLOOR    = 1,   // floor was seen here
    KL_ROOM_OBSTACLE = 2,   // something stands here between ankle and head height
};

// ARKit's surface classification, by raw value (SurfaceClassification).
enum {
    KL_SURF_NONE = 0, KL_SURF_WALL = 1, KL_SURF_FLOOR = 2, KL_SURF_CEILING = 3,
    KL_SURF_TABLE = 4, KL_SURF_SEAT = 5, KL_SURF_WINDOW = 6, KL_SURF_DOOR = 7,
};

/// A grid `2 * half` metres square around (cx, cz).
kl_room_grid kl_room_grid_make(float cx, float cz, float half, float cell);
static inline int32_t kl_room_cells(kl_room_grid g) { return g.w * g.h; }

/// The floor's height from triangles (9 floats each, `kinds` one per
/// triangle): the height most floor-classified area lies at, or `fallback`
/// when less than a fifth of a square metre of floor was seen.
float kl_room_floor_estimate(const float *xyz, const uint8_t *kinds, int32_t ntris, float fallback);

/// Mark the cells the triangles cover: FLOOR where they are floor, OBSTACLE
/// where any part of them is between 10 cm and 1.9 m above it. Ceilings are
/// left out. Adds to what `cells` holds.
void kl_room_raster_tris(kl_room_grid g, uint8_t *cells, const float *xyz, const uint8_t *kinds,
                         int32_t ntris, float floor_y);

/// How much of the room round (ux, uz) is known: rays out to 3 m, each either
/// reaching something, or 3 m of seen floor, or stopping at a stretch nobody
/// has looked at. `sector_frac[k]` (when not NULL) is the share of rays that
/// were answered in sector k of `nsectors`; sector k starts at the angle
/// 2*pi*k/nsectors measured from +x towards +z. Returns the whole share, 0..100.
int32_t kl_room_coverage(kl_room_grid g, const uint8_t *cells, float ux, float uz,
                         int32_t nsectors, float *sector_frac);

/// The play area to suggest round (ux, uz): the free floor that is connected to
/// where the player stands, `clearance` metres from anything standing, no
/// passage narrower than 0.6 m, no further than `max_radius`. `mask` (one byte
/// a cell, 1 = inside) is overwritten. Returns its cell count.
int32_t kl_room_suggest(kl_room_grid g, const uint8_t *cells, float ux, float uz,
                        float clearance, float max_radius, uint8_t *mask);

/// Set every cell whose centre lies inside the polygon (even-odd) to `value`.
void kl_room_fill_polygon(kl_room_grid g, uint8_t *mask, const float *xz, int32_t n, uint8_t value);

enum { KL_ROOM_UNION = 0, KL_ROOM_SUBTRACT = 1, KL_ROOM_INTERSECT = 2 };
void kl_room_combine(kl_room_grid g, uint8_t *dst, const uint8_t *src, int32_t op);

/// Add `piece` to `area`, and when the two are apart by no more than `max_gap`
/// metres, the floor between them where they are that close: a piece drawn next
/// to the area makes it bigger rather than standing beside it. Returns the gap
/// found (0 where they touch, -1 for an empty area).
float kl_room_add_piece(kl_room_grid g, uint8_t *area, const uint8_t *piece, float max_gap);

/// Every cell within `half_width` of the polyline (not closed) set to `value`.
void kl_room_fill_polyline(kl_room_grid g, uint8_t *mask, const float *xz, int32_t n, float half_width,
                           uint8_t value);

/// No holes in an area: every piece of floor the area encloses becomes part of
/// it — unless `cells` (a scan map, may be NULL) has something standing in it;
/// such a hole is opened to the outside by a channel `channel` metres wide along
/// the shortest way, so the area runs round the obstacle instead. Returns the
/// number of holes handled.
int32_t kl_room_fill_holes(kl_room_grid g, uint8_t *mask, const uint8_t *cells, float channel);

/// Close gaps narrower than 2 * radius (dilate, then erode).
void kl_room_close(kl_room_grid g, uint8_t *mask, float radius);
/// Drop every piece but the largest (or, when (ux, uz) is inside one, that one).
void kl_room_keep_main(kl_room_grid g, uint8_t *mask, float ux, float uz);
int32_t kl_room_count(kl_room_grid g, const uint8_t *mask);

/// Signed distance to the edge of the area, metres per cell: positive inside.
void kl_room_sdf(kl_room_grid g, const uint8_t *mask, float *sdf);
/// ...sampled between cells. Outside the grid: minus the distance to it, less 1 m.
float kl_room_sdf_at(kl_room_grid g, const float *sdf, float x, float z);

/// The area's outlines as closed loops, the area on the left of the direction
/// they run in (counter-clockwise round it, clockwise round a hole, seen with
/// x right and z up). Points go to `xz` (pairs), each loop's length to `lens`.
/// Returns the number of loops, -1 when a buffer was too small.
int32_t kl_room_contours(kl_room_grid g, const uint8_t *mask, float *xz, int32_t cap_pts,
                         int32_t *lens, int32_t cap_loops);

/// Centres of the OBSTACLE cells, evenly thinned to at most `cap`.
int32_t kl_room_obstacle_points(kl_room_grid g, const uint8_t *cells, float *xz, int32_t cap);
/// Centres of the cells where floor was seen and nothing stands, the same way.
int32_t kl_room_floor_points(kl_room_grid g, const uint8_t *cells, float *xz, int32_t cap);

/// Where a scan of today stands in a room measured before: the turn about the
/// vertical and the shift that put `pts` (obstacle points in today's frame)
/// onto that room's obstacles (`cells` of grid `g`):
/// room = R(yaw) * p + (tx, tz), R turning +x towards -z as a positive turn
/// about +y does. `score` is the share of points that land on something
/// (0..1, softly); `second` the best score of a placement clearly different from
/// it (a symmetric room has two). Returns 0, or -1 when there is too little to go on.
typedef struct kl_room_fit {
    float yaw, tx, tz;
    float score, second;
    int32_t npts;
} kl_room_fit;
int32_t kl_room_register(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                         kl_room_fit *out);
/// The same, searched only round a placement already believed (`guess`, e.g. a
/// world anchor's): within `yaw_range` radians and `t_range` metres of it.
/// `out->second` is not set.
int32_t kl_room_register_near(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                              const kl_room_fit *guess, float yaw_range, float t_range, kl_room_fit *out);
/// Both, with today's free floor as well (`floor`, `nfloor` points from
/// kl_room_floor_points; NULL/0 is the calls above): the score is then half the
/// obstacles' and half the share of that floor which lands where nothing stood
/// in the room. In a room full of things the obstacles alone fit too many
/// placements near the right one; the free floor tells them apart.
int32_t kl_room_register_scene(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                               const float *floor, int32_t nfloor, kl_room_fit *out);
int32_t kl_room_register_near_scene(kl_room_grid g, const uint8_t *cells, const float *pts, int32_t n,
                                    const float *floor, int32_t nfloor,
                                    const kl_room_fit *guess, float yaw_range, float t_range, kl_room_fit *out);

/// The triangles of one mesh, in the frame `m` (column-major 4x4) takes its
/// vertices to: `verts` is `nverts` vertices of three floats `vstride` bytes
/// apart, `idx` `nfaces` * 3 indices of `bpi` bytes (2 or 4), `cls` (may be
/// NULL) one classification a face, `cbytes` (1 or 4) wide and `cstride` apart.
/// `xyz` takes 9 floats a face, `kinds` one byte. Returns the faces written.
int32_t kl_room_mesh_tris(const void *verts, int32_t vstride, int32_t nverts,
                          const void *idx, int32_t bpi, int32_t nfaces,
                          const void *cls, int32_t cbytes, int32_t cstride,
                          const float m[16], float *xyz, uint8_t *kinds);

/// Apply `m` (column-major 4x4) to `ntris` triangles in place.
void kl_room_transform_tris(float *xyz, int32_t ntris, const float m[16]);

// --- The room as a box, for games that pin their mixed reality to the walls ----------
//
// A Quest's Space Setup gives a game its room: a floor whose outline closes,
// walls standing exactly on that outline, a ceiling. What the scan here keeps
// is less tidy — the wall planes visionOS's plane detection named (pieces of a
// wall, the side of a wardrobe, a window) and the play area. These turn that
// into one rectangle the walls stand on, so a game that draws on its walls
// (Synth Riders in mixed reality) draws where the real walls are.

/// One wall plane of a scan, in the room's frame (floor at y = 0): its centre,
/// its horizontal normal into the room, its size.
typedef struct kl_room_wallplane {
    float cx, cy, cz;
    float nx, nz;
    float width, height;
} kl_room_wallplane;

/// The room fitted to the walls: a rectangle on the floor round (cx, cz) whose
/// sides run along u = (cos yaw, sin yaw) and v = (-sin yaw, cos yaw) in x/z,
/// half_u and half_v from the middle to a side, `height` up to the ceiling.
/// side[k] (k: +u, -u, +v, -v) is the wall plane that side stands on, or -1
/// where none was seen and the side was put just beyond the play area.
typedef struct kl_room_box {
    float cx, cz, yaw;
    float half_u, half_v, height;
    int32_t side[4];
} kl_room_box;

/// Fit the box. The direction is the one the wall planes agree on (each counted
/// by its area, a quarter turn the same direction); with no wall, the play
/// area's long axis. On each of the four sides the largest plane facing into
/// the room that does not cut into the play area is that side's wall. `play`
/// (one byte a cell, 1 = inside, grid `g`) may be NULL; then the walls' middle
/// stands in for it. Returns how many sides were seen (0..4), -1 when there is
/// neither a play area nor a wall.
int32_t kl_room_box_from_walls(const kl_room_wallplane *walls, int32_t n, kl_room_grid g,
                               const uint8_t *play, kl_room_box *out);

/// The box as six planes in the scene convention (the plane in its local XY,
/// +Z its normal: up for the floor, down for the ceiling, into the room for a
/// wall), taken to another frame by `m` (column-major 4x4, room -> that frame):
/// kinds[i] 0 floor, 1 ceiling, 2 wall; m16 16 floats a plane (column-major);
/// ext2 its width and height. Floor, ceiling, then the walls +u, -u, +v, -v.
/// Returns 6.
int32_t kl_room_box_planes(const kl_room_box *b, const float m[16], int32_t *kinds, float *m16, float *ext2);

/// Pack one bit a cell (byte i holds cells 8i..8i+7, low bit first), and back.
/// `bits` holds (cells + 7) / 8 bytes.
void kl_room_pack(int32_t ncells, const uint8_t *mask, uint8_t bit, uint8_t *bits);
void kl_room_unpack(int32_t ncells, const uint8_t *bits, uint8_t bit, uint8_t *mask);

#ifdef __cplusplus
}
#endif
