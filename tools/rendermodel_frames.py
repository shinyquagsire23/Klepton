#!/usr/bin/env python3
"""Where a controller's named frames sit relative to each other.

A SteamVR render model ships a JSON that places every component of the
controller in one frame — including the four that are pose conventions rather
than geometry: `openxr_grip`, `openxr_aim`, `tip`, and the legacy `handgrip`.
The model's own origin is the pose the tracking system reports, so the file is
a complete, exact statement of the offsets between "the pose an OVRPlugin or
OpenVR guest is handed" and "the pose an OpenXR guest asks for". Those differ,
and by enough to matter: on an Oculus Touch the OpenXR grip is 10.2 cm behind
the tracked origin and pitched 20.6 degrees away from it.

Printing them in the grip's own frame is what makes the numbers usable, because
that is the frame the platform hands us on visionOS: an `AccessoryAnchor`
publishes `.grip`, so a correction is `grip -> X` and nothing else.

Usage:
    tools/rendermodel_frames.py <rendermodel>.json [base-component]

The euler convention matches the file's own `rotate_xyz`: degrees, X then Y
then Z, applied to a right-handed frame where -Z points the way the controller
points and +Y is up out of the top of it.
"""

import json
import math
import sys

# Frames worth printing, in the order that tells the story: the tracked pose
# first, then the two OpenXR conventions, then the geometry a guest aims with.
INTEREST = ("tracked", "openxr_grip", "openxr_aim", "tip", "handgrip",
            "grip", "openxr_handmodel", "openxr_pinch", "openxr_poke", "base")


def _rot(axis, deg):
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    if axis == 0:
        return ((1, 0, 0), (0, c, -s), (0, s, c))
    if axis == 1:
        return ((c, 0, s), (0, 1, 0), (-s, 0, c))
    return ((c, -s, 0), (s, c, 0), (0, 0, 1))


def _mul3(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


def _apply(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


def _transpose(m):
    return tuple(tuple(m[j][i] for j in range(3)) for i in range(3))


def _euler_xyz(m):
    """Inverse of the rotate_xyz the file states, so a printed row can be pasted back."""
    y = math.asin(max(-1.0, min(1.0, m[0][2])))
    x = math.atan2(-m[1][2], m[2][2])
    z = math.atan2(-m[0][1], m[0][0])
    return [math.degrees(v) for v in (x, y, z)]


def frames(path):
    """Every component with a placement, as (rotation, origin) in model space.

    `tracked` is the model origin under the name that says what it is: the pose
    the runtime reports for the device, which is what the render model is drawn
    around.
    """
    comps = json.load(open(path))["components"]
    out = {"tracked": (_rot(0, 0), (0.0, 0.0, 0.0))}
    for name, c in comps.items():
        local = c.get("component_local")
        if not local:
            continue
        r = local.get("rotate_xyz", [0.0, 0.0, 0.0])
        m = _mul3(_mul3(_rot(0, r[0]), _rot(1, r[1])), _rot(2, r[2]))
        out[name] = (m, tuple(local.get("origin", [0.0, 0.0, 0.0])))
    return out


def relative(f, base):
    """Every frame expressed in `base`'s frame."""
    bm, bo = f[base]
    bi = _transpose(bm)
    rel = {}
    for name, (m, o) in f.items():
        d = tuple(o[i] - bo[i] for i in range(3))
        rel[name] = (_mul3(bi, m), _apply(bi, d))
    return rel


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    base = sys.argv[2] if len(sys.argv) > 2 else "openxr_grip"
    f = frames(path)
    if base not in f:
        sys.exit("no component %r in %s; have: %s"
                 % (base, path, ", ".join(sorted(f))))
    rel = relative(f, base)

    print("%s\n  relative to %s\n" % (path.rsplit("/", 1)[-1], base))
    print("  %-18s %-34s %-24s %s" % ("frame", "origin (m)", "rot XYZ (deg)", "dist"))
    for name in INTEREST:
        if name not in rel:
            continue
        m, o = rel[name]
        e = _euler_xyz(m)
        print("  %-18s (%9.6f, %9.6f, %9.6f)  (%+7.2f, %+6.2f, %+7.2f)  %.4f"
              % (name, o[0], o[1], o[2], e[0], e[1], e[2],
                 math.sqrt(sum(v * v for v in o))))
    extra = sorted(set(rel) - set(INTEREST))
    if extra:
        print("\n  also present (moving parts, not pose conventions): %s"
              % ", ".join(extra))


if __name__ == "__main__":
    main()
