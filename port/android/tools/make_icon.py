"""Draws the Android app icon (no game artwork: a wooden stake on the
launcher's dark purple, a red slash behind it) into port/android/res:

  mipmap-*/ic_launcher.png             the legacy icon (whole, rounded square)
  mipmap-*/ic_launcher_foreground.png  the adaptive icon's layers (108 dp:
  mipmap-*/ic_launcher_monochrome.png  the art in the middle 66 dp the
  values/ic_launcher_colors.xml        launcher's mask always shows)
  mipmap-anydpi-v26/ic_launcher.xml

An adaptive icon fills the launcher's own shape; a legacy one is shrunk into
a white plate on Samsung's.   python port/android/tools/make_icon.py"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

RES = Path(__file__).resolve().parent.parent / "res"
SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
BG = (28, 18, 36, 255)
SLASH = (150, 14, 34, 255)
WOOD, SHADE = (226, 200, 156, 255), (176, 140, 96, 255)


def art(d, s, ox, oy, slash=SLASH, wood=WOOD, shade=SHADE):
    """The slash and the stake in an s x s square at (ox, oy)."""
    def p(x, y):
        return (ox + s * x, oy + s * y)
    d.polygon([p(0.08, 0.70), p(0.62, 0.08), p(0.92, 0.30), p(0.38, 0.92)], fill=slash)
    # the stake: rough at the butt, tapering into a long point
    a, tip = p(0.20, 0.82), p(0.86, 0.16)
    dx, dy = tip[0] - a[0], tip[1] - a[1]
    ln = (dx * dx + dy * dy) ** 0.5
    nx, ny = -dy / ln, dx / ln

    def at(t, w):
        return (a[0] + dx * t + nx * w * s, a[1] + dy * t + ny * w * s)
    d.polygon([at(0.0, 0.085), at(0.58, 0.060), at(1.0, 0.0), at(0.58, -0.060), at(0.0, -0.085)], fill=wood)
    d.polygon([at(0.0, -0.085), at(0.58, -0.060), at(1.0, 0.0), at(0.58, -0.010), at(0.0, -0.030)], fill=shade)
    # the whittled facets of the point
    d.line([at(0.58, 0.060), at(0.80, 0.012)], fill=shade, width=max(1, int(s * 0.012)))
    d.line([at(0.58, -0.060), at(0.80, -0.012)], fill=shade, width=max(1, int(s * 0.012)))
    # a ragged butt
    d.polygon([at(0.0, 0.085), at(-0.025, 0.04), at(0.01, 0.0), at(-0.03, -0.045), at(0.0, -0.085)], fill=wood)


def finish(img, n):
    return img.filter(ImageFilter.SMOOTH).resize((n, n), Image.LANCZOS)


def legacy(n):
    s = 4 * n                                   # drawn large, then scaled down (antialiased)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s * 0.22, fill=BG)
    art(d, s, 0, 0)
    return finish(img, n)


def layer(n, mono=False):
    """108 dp square (n = 108 dp in pixels); the art in its middle 66 dp."""
    s = 4 * n
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    a = s * 66 / 108
    o = (s - a) / 2
    if mono:
        white = (255, 255, 255, 255)
        art(d, a, o, o, slash=(255, 255, 255, 110), wood=white, shade=white)
    else:
        art(d, a, o, o)
    return finish(img, n)


for density, n in SIZES.items():
    folder = RES / f"mipmap-{density}"
    folder.mkdir(parents=True, exist_ok=True)
    legacy(n).save(folder / "ic_launcher.png")
    big = n * 108 // 48
    layer(big).save(folder / "ic_launcher_foreground.png")
    layer(big, mono=True).save(folder / "ic_launcher_monochrome.png")
    print(folder)

(RES / "values").mkdir(exist_ok=True)
(RES / "values" / "ic_launcher_colors.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
    '    <color name="ic_launcher_background">#%02X%02X%02X</color>\n</resources>\n' % BG[:3])
(RES / "mipmap-anydpi-v26").mkdir(exist_ok=True)
(RES / "mipmap-anydpi-v26" / "ic_launcher.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
    '    <background android:drawable="@color/ic_launcher_background" />\n'
    '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
    '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
    '</adaptive-icon>\n')
print("adaptive icon")
