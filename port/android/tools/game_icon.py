"""The app icon from the game's own logo: Buffy/Binary/_bin_xb/buffytitle.xbx
(an XPR0 holding a 128x128 DXT1 image -- the PC launcher's icon and the
Android launcher's GameIcon.java read the same file) from the player's game
files, written over the stand-in icon (make_icon.py) in a copy of res/.
build_apk.sh runs it at build time; nothing of the game goes into the repo.

  python port/android/tools/game_icon.py <buffytitle.xbx> <res folder>"""
import sys
from pathlib import Path

from PIL import Image

SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def dxt1(b, at):
    c0 = b[at] | b[at + 1] << 8
    c1 = b[at + 2] | b[at + 3] << 8
    bits = int.from_bytes(b[at + 4:at + 8], "little")

    def rgb(c):
        return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    p0, p1 = rgb(c0), rgb(c1)
    if c0 > c1:
        p2 = tuple((2 * x + y) // 3 for x, y in zip(p0, p1))
        p3 = tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))
    else:
        p2 = tuple((x + y) // 2 for x, y in zip(p0, p1))
        p3 = (0, 0, 0)
    pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + ((255,) if c0 > c1 else (0,))]
    return [pal[(bits >> (i * 2)) & 3] for i in range(16)]


def logo(path):
    b = Path(path).read_bytes()
    if len(b) < 0x2800 or b[:4] != b"XPR0" or b[0x19] != 0x0C:
        raise SystemExit("%s: not the 128x128 DXT1 logo" % path)
    img = Image.new("RGBA", (128, 128))
    px = img.load()
    for by in range(32):
        for bx in range(32):
            for i, c in enumerate(dxt1(b, 0x800 + (by * 32 + bx) * 8)):
                px[bx * 4 + i % 4, by * 4 + i // 4] = c
    return img


def main():
    src, res = Path(sys.argv[1]), Path(sys.argv[2])
    im = logo(src)
    edge = im.getpixel((2, 2))[:3]
    for density, n in SIZES.items():
        folder = res / f"mipmap-{density}"
        folder.mkdir(parents=True, exist_ok=True)
        im.resize((n, n), Image.LANCZOS).save(folder / "ic_launcher.png")
        # adaptive foreground: 108 dp, the logo over the middle 76 (its edge
        # colour is the background layer, so the mask only trims plain edge)
        big = n * 108 // 48
        fg = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        inner = big * 76 // 108
        fg.paste(im.resize((inner, inner), Image.LANCZOS), ((big - inner) // 2, (big - inner) // 2))
        fg.save(folder / "ic_launcher_foreground.png")
        mono = folder / "ic_launcher_monochrome.png"
        if mono.exists():
            mono.unlink()                       # (a themed silhouette of a picture: none)
    (res / "values").mkdir(exist_ok=True)
    (res / "values" / "ic_launcher_colors.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        '    <color name="ic_launcher_background">#%02X%02X%02X</color>\n</resources>\n' % edge)
    (res / "mipmap-anydpi-v26").mkdir(exist_ok=True)
    (res / "mipmap-anydpi-v26" / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n')
    print("app icon: the game's logo (%s)" % src)


if __name__ == "__main__":
    main()
