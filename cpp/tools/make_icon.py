"""App / shortcut icon for Seismic FDTD Solver.
Usage: python make_icon.py <out.ico> [preview.png]
Large sizes show the full scene (sea surface, source, wavefronts, streamer,
salt dome); 16-32 px use a simplified, thicker drawing so they stay legible."""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

def lerp(a, b, t): return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(len(a)))

def render(size, detail):
    N = 1024                      # draw at 1024, downsample → clean anti-aliasing
    s = N / 256.0
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))

    # Rounded-square tile with a deep-ocean gradient
    pad = 8 * s if detail else 4 * s
    r = 56 * s
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    t = np.clip((yy * 0.85 + xx * 0.15) / N, 0, 1)[..., None]
    top, bot = np.array([16, 86, 170], np.float32), np.array([3, 16, 40], np.float32)
    bg = np.concatenate([top + (bot - top) * t, np.full((N, N, 1), 255, np.float32)], axis=2).astype(np.uint8)
    tile = Image.fromarray(bg, 'RGBA')
    mask = Image.new('L', (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, N - pad, N - pad], radius=r, fill=255)
    img.paste(tile, (0, 0), mask)

    clip = mask                    # everything below is clipped to the tile
    layer = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    sea = 98 * s                   # sea surface
    cx = 128 * s                   # source position
    # Sky band
    d.rectangle([0, 0, N, sea], fill=(1, 6, 16, 150))

    # Salt dome (only when there is room)
    if detail:
        pts = [(x, (240 - 62 * math.exp(-((x / s - 150) ** 2) / (2 * 34 ** 2))) * s) for x in np.linspace(0, N, 120)]
        d.polygon(pts + [(N, N), (0, N)], fill=(150, 176, 204, 235))
        d.line(pts, fill=(236, 246, 255, 255), width=int(3 * s))
        # seafloor
        d.line([(0, 196 * s), (N, 182 * s)], fill=(47, 93, 150, 255), width=int(3 * s))

    # Wavefronts: lower half-circles around the source, brightest nearest
    rings = [(38, 255, 10), (76, 255, 9), (114, 215, 8), (152, 150, 7)] if detail else [(56, 255, 24), (114, 235, 22)]
    # Glow first (blur premultiplies against transparent black, so it must sit
    # underneath the crisp strokes or it greys them out)
    glow = Image.new('RGBA', (N, N), (0, 229, 255, 0))
    gd = ImageDraw.Draw(glow)
    for rad, a, w in rings:
        R = rad * s
        gd.arc([cx - R, sea - R, cx + R, sea + R], start=0, end=180, fill=(0, 229, 255, a // 2), width=int(w * 2 * s))
    layer.alpha_composite(glow.filter(ImageFilter.GaussianBlur(10 * s)))
    d = ImageDraw.Draw(layer, 'RGBA')
    for rad, a, w in rings:
        R = rad * s
        col = lerp((255, 255, 255), (0, 229, 255), min(1.0, rad / 110))
        d.arc([cx - R, sea - R, cx + R, sea + R], start=0, end=180, fill=col + (a,), width=int(w * s))

    # Sea surface line
    d.line([(0, sea), (N, sea)], fill=(64, 168, 255, 255), width=int((5 if detail else 12) * s))

    # Streamer (gold) with receivers, trailing left of the source
    if detail:
        d.line([(26 * s, sea + 9 * s), (108 * s, sea + 9 * s)], fill=(255, 196, 40, 255), width=int(5 * s))
        for x in range(30, 108, 16):
            d.ellipse([(x - 4) * s, sea + 5 * s, (x + 4) * s, sea + 13 * s], fill=(255, 255, 255, 255))

    # Source: bright dot with a halo, sitting on the sea surface
    R0 = (12 if detail else 22) * s
    halo = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([cx - R0 * 2.2, sea - R0 * 2.2, cx + R0 * 2.2, sea + R0 * 2.2], fill=(0, 230, 255, 150))
    layer.alpha_composite(halo.filter(ImageFilter.GaussianBlur(12 * s)))
    d.ellipse([cx - R0, sea - R0, cx + R0, sea + R0], fill=(255, 255, 255, 255))

    # Ship silhouette above the source (large sizes)
    if detail:
        sx = cx + 46 * s   # ship ahead of the towed source
        hull = [(sx - 30 * s, sea - 22 * s), (sx + 26 * s, sea - 22 * s), (sx + 36 * s, sea - 4 * s), (sx - 34 * s, sea - 4 * s)]
        d.polygon(hull, fill=(30, 144, 255, 255))
        d.rectangle([sx + 2 * s, sea - 36 * s, sx + 20 * s, sea - 22 * s], fill=(0, 212, 255, 255))
        d.line([(cx + 10 * s, sea - 2 * s), (sx - 30 * s, sea - 6 * s)], fill=(255, 255, 255, 170), width=int(2 * s))

    la = np.array(layer)   # clip to the tile without touching colours
    la[..., 3] = (la[..., 3].astype(np.uint16) * np.array(clip) // 255).astype(np.uint8)
    img.alpha_composite(Image.fromarray(la, 'RGBA'))

    # Rim highlight
    rim = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([pad, pad, N - pad, N - pad], radius=r, outline=(0, 242, 255, 120),
                                          width=int((3 if detail else 8) * s))
    img.alpha_composite(rim)
    return img.resize((size, size), Image.LANCZOS)

if __name__ == '__main__':
    out = sys.argv[1]
    sizes = [256, 128, 64, 48, 32, 24, 16]
    imgs = [render(sz, detail=sz >= 48) for sz in sizes]
    imgs[0].save(out, format='ICO', sizes=[(s, s) for s in sizes], append_images=imgs[1:])
    if len(sys.argv) > 2:
        # Preview sheet: every size on a light and a dark background
        W = sum(sizes) + 20 * (len(sizes) + 1)
        sheet = Image.new('RGBA', (W, 2 * 276 + 20), (0, 0, 0, 0))
        for row, bgc in enumerate([(236, 239, 244, 255), (32, 32, 36, 255)]):
            ImageDraw.Draw(sheet).rectangle([0, row * 286, W, row * 286 + 276], fill=bgc)
            x = 20
            for im in imgs:
                sheet.alpha_composite(im, (x, row * 286 + (276 - im.size[1]) // 2))
                x += im.size[0] + 20
        sheet.save(sys.argv[2])
    print('saved', out)
