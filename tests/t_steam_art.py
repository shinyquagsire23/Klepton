"""visionos/steam_art.py refuses what it should, with no network: a Steam app
id that is not digits, a download that is not the image its name says, and a
download larger than the limit. `_open` is replaced by a local fake. `make
testbuild`.
"""
import importlib.util, io, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("steam_art", os.path.join(HERE, "..", "visionos", "steam_art.py"))
sa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sa)


class Fake:
    def __init__(self, body, length=None):
        self.body, self.headers = io.BytesIO(body), {}
        if length is not None:
            self.headers["Content-Length"] = str(length)
        self.read_bytes = 0
    def read(self, n=-1):
        b = self.body.read(n)
        self.read_bytes += len(b)
        return b
    def __enter__(self): return self
    def __exit__(self, *a): return False


failed = 0
def check(label, got, want):
    global failed
    ok = got == want
    failed += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {label}" + ("" if ok else f": got {got!r}, want {want!r}"))


print("=== steam_art: input checks, no network ===")
saved, sys.stderr = sys.stderr, io.StringIO()   # the refusals log to stderr
try:
    with tempfile.TemporaryDirectory() as d:
        calls = []
        def opener(body, length=None):
            def f(url):
                calls.append(url)
                return Fake(body, length)
            return f
        png = sa.PNG_MAGIC + b"x" * 100
        jpg = sa.JPEG_MAGIC + b"x" * 100

        sa._open = opener(png)
        for bad in ("", "12a", "../1", "1 2", "٣"):
            calls.clear()
            check(f"app id {bad!r} skipped, no request, nothing written",
                  (sa.main(bad, "t", os.path.join(d, "o")), calls, os.path.exists(os.path.join(d, "o"))),
                  (1, [], False))

        dest = os.path.join(d, "logo.png")
        sa._open = opener(png)
        check("a PNG is cached", (sa.fetch("1", "logo.png", dest), os.path.isfile(dest)), (True, True))
        os.remove(dest)
        sa._open = opener(jpg)
        check("a JPEG named .png is rejected",
              (sa.fetch("1", "logo.png", dest), os.path.exists(dest), os.path.exists(dest + ".part")),
              (False, False, False))
        dest = os.path.join(d, "hero.jpg")
        sa._open = opener(jpg)
        check("a JPEG is cached", (sa.fetch("1", "hero.jpg", dest), os.path.isfile(dest)), (True, True))
        os.remove(dest)
        sa._open = opener(b"<html>nope</html>")
        check("HTML named .jpg is rejected", (sa.fetch("1", "hero.jpg", dest), os.path.exists(dest)), (False, False))
        sa._open = opener(b"")
        check("an empty response is rejected", sa.fetch("1", "hero.jpg", dest), False)

        sa._open = opener(jpg, length=sa.MAX_BYTES + 1)
        check("Content-Length over the limit is rejected", (sa.fetch("1", "hero.jpg", dest), os.path.exists(dest)), (False, False))
        # No Content-Length: the stream is cut off at the limit, not read to the end.
        big = Fake(sa.JPEG_MAGIC + b"x" * (sa.MAX_BYTES + 10 * 1024 * 1024))
        sa._open = lambda url: big
        check("an endless stream is aborted", (sa.fetch("1", "hero.jpg", dest), os.path.exists(dest), os.path.exists(dest + ".part")),
              (False, False, False))
        check("  ...after about the limit, not the whole body", big.read_bytes <= sa.MAX_BYTES + 65536, True)

        with open(dest, "wb") as f:
            f.write(b"not an image")
        sa._open = opener(jpg)
        check("a cached non-image is replaced, not trusted", (sa.fetch("1", "hero.jpg", dest), open(dest, "rb").read(3)), (True, sa.JPEG_MAGIC))
finally:
    sys.stderr = saved
print("steam_art: " + ("FAIL" if failed else "PASS"))
sys.exit(1 if failed else 0)
