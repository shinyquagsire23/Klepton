#!/usr/bin/env python3
"""The icon and start picture of a target that has no APK to take them from.

AVP Play's installer leaves a target's icon (AppIcon-<target>.solidimagestack)
and start picture (StartHero-<target>.imageset) in visionos/Assets.xcassets,
taken from the APK or the store page. A Linux target (alyx) has neither: the
game comes from Steam, and the installer looks for Steam's artwork only in a
Steam client's library cache on the Mac. Where no Steam client is installed it
leaves nothing, and the game lands on the headset with the system's placeholder
icon and a start screen that is just the title on black.

This builds both from Steam's public store artwork for the app the target names
(targets.py "steam_app"), the same pictures the Steam library shows:

  library_hero_2x.jpg   the wide key art: its centre square is the icon's back
                        layer, its 16:9 centre the start picture
  logo.png              the title treatment on a transparent ground: the icon's
                        front layer, so the depth visionOS gives an icon lifts
                        the title off the art

Written to <out>/AppIcon-<target>.solidimagestack and
<out>/StartHero-<target>.imageset in the shapes the installer writes, and
gen_xcodeproj.py takes them from <out> when visionos/Assets.xcassets has none
for the target. The downloads stay in <out>/src, so a rebuild without a network
keeps its icon. The image work is `sips`, which every Mac has. Any failure
leaves <out> without the stack and the build goes on as before: iconless. All
of it reports on stderr: run.sh keeps only the first stdout line of the
project generator, which must stay "wrote ...".

  python3 steam_art.py <steam_app> <target> <out>
"""
import os, re, shutil, subprocess, sys, urllib.request

CDN = "https://shared.steamstatic.com/store_item_assets/steam/apps/%s/%s"
ICON_PX = 1024          # a visionOS icon layer
LOGO_PX = 720           # the title's longest edge on it, so art shows around it
HERO_W, HERO_H = 2560, 1440
MAX_BYTES = 20 * 1024 * 1024   # a download larger than this is not art
TIMEOUT = 30                   # seconds, per request and per read
JPEG_MAGIC = b"\xff\xd8\xff"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _open(url):
    req = urllib.request.Request(url, headers={"User-Agent": "klepton-steam-art/1"})
    return urllib.request.urlopen(req, timeout=TIMEOUT)


def looks_like_image(head, name):
    """The file's first bytes say it is the kind of image its name claims."""
    return head.startswith(PNG_MAGIC if name.endswith(".png") else JPEG_MAGIC)


def fetch(app, name, dest):
    if os.path.isfile(dest) and os.path.getsize(dest) > 0:
        with open(dest, "rb") as f:
            if looks_like_image(f.read(8), name):
                return True
        os.remove(dest)     # a cached file that is not an image is not kept
    part = dest + ".part"
    try:
        with _open(CDN % (app, name)) as r:
            length = r.headers.get("Content-Length")
            if length and length.isdigit() and int(length) > MAX_BYTES:
                raise ValueError(f"{length} bytes is over the {MAX_BYTES} byte limit")
            got = 0
            first = b""
            with open(part, "wb") as f:
                while True:
                    chunk = r.read(65536)
                    if not chunk:
                        break
                    if not first:
                        first = chunk[:8]
                        if not looks_like_image(first, name):
                            raise ValueError("not a " + ("PNG" if name.endswith(".png") else "JPEG") + " image")
                    got += len(chunk)
                    if got > MAX_BYTES:
                        raise ValueError(f"more than {MAX_BYTES} bytes")
                    f.write(chunk)
            if not got:
                raise ValueError("empty response")
        os.replace(part, dest)
        return True
    except Exception as e:
        print(f"  steam art: {name} of app {app}: {e}", file=sys.stderr)
        try:
            os.remove(part)
        except OSError:
            pass
        return False


def sips(*args):
    subprocess.run(["sips", *args], check=True, stdout=subprocess.DEVNULL)


def size(path):
    out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", path],
                         check=True, capture_output=True, text=True).stdout
    w = h = 0
    for line in out.splitlines():
        k, _, v = line.strip().partition(": ")
        if k == "pixelWidth":
            w = int(v)
        elif k == "pixelHeight":
            h = int(v)
    if not (w and h):
        raise RuntimeError(f"no size for {path}")
    return w, h


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def layer(stack, name, png):
    d = os.path.join(stack, f"{name}.solidimagestacklayer", "Content.imageset")
    os.makedirs(d)
    write(os.path.join(os.path.dirname(d), "Contents.json"),
          '{ "info" : { "author" : "xcode", "version" : 1 } }\n')
    base = os.path.basename(png)
    shutil.copy(png, os.path.join(d, base))
    write(os.path.join(d, "Contents.json"),
          '{ "images" : [ { "filename" : "%s", "idiom" : "vision", "scale" : "2x" } ], '
          '"info" : { "author" : "xcode", "version" : 1 } }\n' % base)


def main(app, target, out):
    # Before the id reaches a URL or a path.
    if not re.fullmatch(r"[0-9]+", app):
        print(f"  steam art: {app!r} is not a Steam app id — the build stays iconless",
              file=sys.stderr)
        return 1
    src = os.path.join(out, "src")
    os.makedirs(src, exist_ok=True)
    stack = os.path.join(out, f"AppIcon-{target}.solidimagestack")
    hero_set = os.path.join(out, f"StartHero-{target}.imageset")
    if os.path.isdir(stack) and os.path.isdir(hero_set):
        print(f"  steam art: icon and start picture of app {app} (kept from an earlier build)",
              file=sys.stderr)
        return 0
    # The wide key art, at the larger size when the store has it.
    hero = None
    for name in ("library_hero_2x.jpg", "library_hero.jpg"):
        p = os.path.join(src, name)
        if fetch(app, name, p):
            hero = p
            break
    if hero is None:
        print(f"  steam art: no key art for app {app} — the build stays iconless", file=sys.stderr)
        return 1
    logo = os.path.join(src, "logo.png")
    if not fetch(app, "logo.png", logo):
        logo = None
    tmp = os.path.join(out, "tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        w, h = size(hero)
        # Back layer: the centre square of the key art. Crop and resample are
        # two calls — sips does not apply its options in the order given.
        side = min(w, h)
        back = os.path.join(tmp, "back.png")
        sips("-s", "format", "png", "-c", str(side), str(side), hero, "--out", back)
        sips("-z", str(ICON_PX), str(ICON_PX), back)
        # Front layer: the title, fitted into the middle of a transparent square.
        front = None
        if logo:
            front = os.path.join(tmp, "front.png")
            sips("-s", "format", "png", "-Z", str(LOGO_PX), logo, "--out", front)
            sips("-p", str(ICON_PX), str(ICON_PX), front)
        # The start picture: the 16:9 centre of the key art, no larger than the
        # installer's (2560 across), never upscaled.
        if w * HERO_H > h * HERO_W:
            cw, ch = h * HERO_W // HERO_H, h
        else:
            cw, ch = w, w * HERO_H // HERO_W
        hero_png = os.path.join(tmp, "hero.png")
        sips("-s", "format", "png", "-c", str(ch), str(cw), hero, "--out", hero_png)
        if cw > HERO_W:
            sips("-Z", str(HERO_W), hero_png)

        shutil.rmtree(stack, ignore_errors=True)
        os.makedirs(stack)
        layers = ["Back"]
        layer(stack, "Back", back)
        if front:
            layer(stack, "Front", front)
            layers.insert(0, "Front")
        write(os.path.join(stack, "Contents.json"),
              '{ "info" : { "author" : "xcode", "version" : 1 }, "layers" : [ %s ] }\n'
              % ", ".join('{ "filename" : "%s.solidimagestacklayer" }' % l for l in layers))
        shutil.rmtree(hero_set, ignore_errors=True)
        os.makedirs(hero_set)
        shutil.copy(hero_png, os.path.join(hero_set, "hero.png"))
        write(os.path.join(hero_set, "Contents.json"),
              '{\n  "images" : [ { "filename" : "hero.png", "idiom" : "universal" } ],\n'
              '  "info" : { "author" : "xcode", "version" : 1 }\n}\n')
    except Exception as e:
        print(f"  steam art: could not make the icon of app {app}: {e} — the build stays iconless",
              file=sys.stderr)
        shutil.rmtree(stack, ignore_errors=True)
        shutil.rmtree(hero_set, ignore_errors=True)
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"  steam art: icon ({'art + title' if front else 'art only'}) and start picture"
          f" of app {app} from Steam's store artwork", file=sys.stderr)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*sys.argv[1:]))
