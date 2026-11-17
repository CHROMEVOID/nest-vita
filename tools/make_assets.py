"""Builds Nest's bundled art from the user's reference folder.

Run on a PC (needs Pillow):  python tools/make_assets.py "D:/psvita app"

Outputs
  assets/lofi/<scene>.nla   palette-compressed animation frames for the Vita
  assets/lofi/<scene>_soft.jpg  blurred, dimmed still used behind dialogs
  sce_sys/icon0.png, sce_sys/pic0.png, sce_sys/livearea/contents/{bg0,startup}.png

NLA format (little endian):
  "NLA1" u16 w u16 h u16 frames u16 flags(bit0 = pixel art -> nearest filter)
  u32 palette[256] (R,G,B,A bytes)
  frames x { u16 delay_ms, u32 zlen, zlib(w*h palette indices) }
"""
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont, ImageSequence

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(ROOT)
OUT = os.path.join(ROOT, "assets", "lofi")

# scene id: (source file, pixel art?, max frames, default delay ms)
SCENES = {
    "window": ("fetchpik.com-1uN0QXcFJw.gif", True, 64, 100),
    "cafe": ("lofi.gif", True, 32, 60),
    "city": ("fetchpik.com-KysJZjIR7g.gif", True, 60, 30),
    "plants": ("Vikurai_pindown.io_1794793555.gif", True, 42, 80),
    "lake": ("adilsonjsantiago_pindown.io_1794793597.gif", True, 14, 90),
    "alley": ("jbnetbiz0076_pindown.io_1794786336.gif", True, 12, 140),
    "rooftop": ("lofi-rooftop-study-night-chill-lqcvkej9ymld5zbv.gif", True, 20, 250),
    "study": ("fetchpik.com-Xpr4oFi8MS.gif", True, 41, 60),
    "ramen": ("uncafelatte_pindown.io_1794786417.gif", True, 24, 160),
    "dreamer": ("anime-lofi.gif", False, 65, 60),
    "forest": ("ttbob.gif", True, 1, 1000),
    "garden": ("OIP (1).webp", True, 1, 1000),
    "castle": ("OIP (2).webp", True, 1, 1000),
    "koi": ("OIP (3).webp", True, 1, 1000),
    "rainy": ("OIP (4).webp", True, 1, 1000),
    "ruins": ("dark-fantasy-pixel-art-v0-1gj9xlgbw19c1.webp", True, 1, 1000),
}

MAX_W = 720  # keeps memory sane; larger art is scaled down first


def load_frames(path, max_frames, default_delay):
    im = Image.open(path)
    frames = []
    for fr in ImageSequence.Iterator(im):
        d = fr.info.get("duration") or default_delay
        if d < 20:
            d = default_delay
        frames.append((fr.convert("RGB"), d))
    if len(frames) > max_frames:
        step = len(frames) / max_frames
        picked = []
        for i in range(max_frames):
            a, b = int(i * step), int((i + 1) * step)
            picked.append((frames[a][0], sum(f[1] for f in frames[a:b])))
        frames = picked
    w, h = frames[0][0].size
    if w > MAX_W:
        nh = round(h * MAX_W / w)
        frames = [(f.resize((MAX_W, nh), Image.LANCZOS), d) for f, d in frames]
    return frames


def write_nla(name, frames, pixel):
    w, h = frames[0][0].size
    # one shared palette from a mosaic of sampled frames
    sample = frames[:: max(1, len(frames) // 8)][:8]
    mosaic = Image.new("RGB", (w, h * len(sample)))
    for i, (f, _) in enumerate(sample):
        mosaic.paste(f, (0, i * h))
    pal_img = mosaic.quantize(256, method=Image.Quantize.MEDIANCUT)
    pal = pal_img.getpalette()[: 256 * 3]
    pal += [0] * (768 - len(pal))
    dither = Image.Dither.NONE if pixel else Image.Dither.FLOYDSTEINBERG
    body = bytearray()
    for f, d in frames:
        q = f.quantize(palette=pal_img, dither=dither)
        body += struct.pack("<HI", min(int(d), 65535), 0)
        z = zlib.compress(q.tobytes(), 9)
        struct.pack_into("<I", body, len(body) - 4, len(z))
        body += z
    hdr = b"NLA1" + struct.pack("<HHHH", w, h, len(frames), 1 if pixel else 0)
    palb = bytearray()
    for i in range(256):
        palb += bytes((pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], 255))
    with open(os.path.join(OUT, name + ".nla"), "wb") as fp:
        fp.write(hdr + palb + body)
    return w, h


def soft_still(name, frame):
    s = frame.copy()
    s.thumbnail((320, 180))
    s = s.filter(ImageFilter.GaussianBlur(6))
    s = ImageEnhance.Brightness(s).enhance(0.55)
    s.save(os.path.join(OUT, name + "_soft.jpg"), quality=88)


def font(size, bold=True):
    f = os.path.join(ROOT, "assets", "fonts", "Inter-Bold.ttf" if bold else "Inter-Regular.ttf")
    return ImageFont.truetype(f, size)


def cover(img, w, h):
    iw, ih = img.size
    s = max(w / iw, h / ih)
    img = img.resize((max(w, round(iw * s)), max(h, round(ih * s))), Image.NEAREST if s >= 1.5 else Image.LANCZOS)
    x, y = (img.width - w) // 2, (img.height - h) // 2
    return img.crop((x, y, x + w, y + h))


def vignette(img, strength=0.65):
    w, h = img.size
    grad = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(grad)
    for i in range(h):
        t = i / h
        d.line([(0, i), (w, i)], fill=int(255 * strength * (t ** 1.6)))
    black = Image.new("RGB", (w, h), (12, 12, 20))
    return Image.composite(black, img, grad)


def livearea(window_frame, cafe_frame):
    sys_dir = os.path.join(ROOT, "sce_sys")
    la = os.path.join(sys_dir, "livearea", "contents")
    os.makedirs(la, exist_ok=True)

    # icon: scene crop + small wordmark, indexed PNG as the Vita prefers
    icon = cover(window_frame, 128, 128)
    icon = vignette(icon, 0.75)
    d = ImageDraw.Draw(icon)
    f = font(30)
    tw = d.textlength("nest", font=f)
    d.text(((128 - tw) / 2, 82), "nest", font=f, fill=(246, 241, 255))
    icon.convert("P", palette=Image.ADAPTIVE, colors=255).save(os.path.join(sys_dir, "icon0.png"))

    bg = vignette(cover(window_frame, 840, 500), 0.55)
    bg.convert("P", palette=Image.ADAPTIVE, colors=255).save(os.path.join(la, "bg0.png"))

    st = vignette(cover(cafe_frame, 280, 158), 0.8)
    d = ImageDraw.Draw(st)
    f = font(40)
    tw = d.textlength("nest", font=f)
    d.text(((280 - tw) / 2, 52), "nest", font=f, fill=(250, 246, 255))
    f2 = font(13, False)
    sub = "your library, softly"
    tw2 = d.textlength(sub, font=f2)
    d.text(((280 - tw2) / 2, 104), sub, font=f2, fill=(200, 196, 220))
    st.convert("P", palette=Image.ADAPTIVE, colors=255).save(os.path.join(la, "startup.png"))

    pic = vignette(cover(window_frame, 960, 544), 0.5)
    pic.convert("P", palette=Image.ADAPTIVE, colors=255).save(os.path.join(sys_dir, "pic0.png"))


def main():
    os.makedirs(OUT, exist_ok=True)
    firsts = {}
    for name, (fn, pixel, maxf, dd) in SCENES.items():
        p = os.path.join(SRC, fn)
        if not os.path.exists(p):
            print("missing", fn)
            continue
        frames = load_frames(p, maxf, dd)
        w, h = write_nla(name, frames, pixel)
        soft_still(name, frames[0][0])
        firsts[name] = frames[len(frames) // 3][0]
        size = os.path.getsize(os.path.join(OUT, name + ".nla"))
        print(f"{name:8s} {w}x{h} {len(frames):3d}f {size/1024:.0f}KB")
    livearea(firsts["window"], firsts["cafe"])


if __name__ == "__main__":
    main()
