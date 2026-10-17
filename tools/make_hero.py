#!/usr/bin/env python3
"""
The README's title picture from the screenshots (tools/screenshots.mjs): the panel, drawn as the
NSPanel's white square with its two buttons, the home screen on it, in front of the web interface.

    tools/make_hero.py [screens] [out]    # default: docs/images/screens, docs/images/hero.png
"""

import pathlib
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).resolve().parent.parent


def rounded(im, radius):
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius, fill=255)
    out = Image.new("RGBA", im.size)
    out.paste(im, (0, 0), mask)
    return out


def shadow(size, radius, blur, alpha=90):
    w, h = size
    s = Image.new("RGBA", (w + 4 * blur, h + 4 * blur), (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle((2 * blur, 2 * blur, 2 * blur + w, 2 * blur + h), radius, fill=(0, 0, 0, alpha))
    return s.filter(ImageFilter.GaussianBlur(blur))


def main():
    screens = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "docs" / "images" / "screens"
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "docs" / "images" / "hero.png"
    W, H = 1600, 860
    hero = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    # the web interface, a browser window behind on the right
    web = Image.open(screens / "web-panel.png").convert("RGB")
    web = web.resize((1080, int(web.height * 1080 / web.width)))
    bar = 34
    window = Image.new("RGB", (web.width, web.height + bar), (236, 236, 240))
    d = ImageDraw.Draw(window)
    for k, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse((16 + k * 22, 11, 28 + k * 22, 23), fill=c)
    window.paste(web, (0, bar))
    window = rounded(window, 14)
    wx, wy = W - window.width - 20, 40
    hero.alpha_composite(shadow(window.size, 14, 18, 70), (wx - 36, wy - 30))
    hero.alpha_composite(window, (wx, wy))
    # the panel: a white square, the screen in its upper part, two buttons under it
    side = 620
    panel = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    d = ImageDraw.Draw(panel)
    d.rounded_rectangle((0, 0, side - 1, side - 1), 46, fill=(250, 250, 250), outline=(222, 222, 226), width=3)
    screen = Image.open(screens / "display-home.png").convert("RGB")
    sw = side - 2 * 46
    screen = screen.resize((sw, int(screen.height * sw / screen.width)))
    d.rounded_rectangle((40, 40, side - 41, 52 + screen.height), 14, fill=(18, 18, 20))
    panel.alpha_composite(rounded(screen, 8), (46, 46))
    by = 46 + screen.height + 40
    for x0 in (46, side // 2 + 12):
        d.rounded_rectangle((x0, by, x0 + side // 2 - 58, side - 52), 22, fill=(242, 242, 244), outline=(226, 226, 230), width=2)
    px, py = 40, H - side - 30
    hero.alpha_composite(shadow(panel.size, 46, 22, 110), (px - 44, py - 30))
    hero.alpha_composite(panel, (px, py))
    hero.save(out, optimize=True)
    print(f"hero: {out} ({W} x {H})")


if __name__ == "__main__":
    main()
