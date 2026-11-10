"""Professional thumbnail for the SLO/CLO FDTD simulator.
A real 2D acoustic simulation (salt-dome model, two simultaneous sources)
is composited with the app's visual language at 2x and downsampled."""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
W, H = 1280, 720
SS = 2                      # supersampling
SW, SH = W * SS, H * SS
F = r"C:\Windows\Fonts\\"

def font(name, size): return ImageFont.truetype(F + name, int(size * SS))
def rgb(h, a=255): h = h.lstrip('#'); return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

# ── Geometry (image pixels) ──────────────────────────────────────────────
SEA = 336                                       # sea surface y
def seafloor(x): return 470 + (x / W) * 60      # gentle dip right
def deep(x):     return 660 - (x / W) * 40
def salt(x):
    s = 820 - 320 * math.exp(-((x - 820) ** 2) / (2 * 150 ** 2))
    return max(s, seafloor(x) + 18)

# ── FDTD simulation on a half-resolution grid (1 cell = 2 px) ───────────
nx, nz = W // 2, H // 2
c = np.ones((nz, nx), np.float32)
for i in range(nx):
    x = i * 2
    for j in range(nz):
        y = j * 2
        c[j, i] = 2.72 if y >= salt(x) else 1.92 if y >= deep(x) else 1.40 if y >= seafloor(x) else 1.0
dt = 0.21                     # 4th-order 2D CFL: c_max*dt <= ~0.61
c2 = (c * dt) ** 2
# sponge on left/right/bottom; above the sea line the field is forced to zero (free surface)
damp = np.ones((nz, nx), np.float32)
nb = 40
for k in range(nb):
    a = math.exp(-((0.012 * (nb - k)) ** 2))
    damp[:, k] *= a; damp[:, nx - 1 - k] *= a; damp[nz - 1 - k, :] *= a
surf = SEA // 2

def ricker(t, f=0.021, t0=58.0):
    a = (math.pi * f * (t - t0)) ** 2
    return (1 - 2 * a) * math.exp(-a)

srcs = [(300 // 2, surf + 4, 0.0), (760 // 2, surf + 4, 60.0)]   # (i, j, delay) — SLO blended shots
p0 = np.zeros((nz, nx), np.float32); p1 = p0.copy()
rec_x = np.arange(40, 280, 4) // 2                                 # streamer behind ship 1
rec_j = surf + 5
nsteps, snap_step = 1900, 800
gather = np.zeros((nsteps, len(rec_x)), np.float32)
snap = None
for n in range(nsteps):
    lap = np.zeros_like(p1)
    c_ = p1[2:-2, 2:-2]
    lap[2:-2, 2:-2] = (4 / 3) * (p1[2:-2, 3:-1] + p1[2:-2, 1:-3] + p1[3:-1, 2:-2] + p1[1:-3, 2:-2]) \
        - (1 / 12) * (p1[2:-2, 4:] + p1[2:-2, :-4] + p1[4:, 2:-2] + p1[:-4, 2:-2]) - 5.0 * c_
    p2 = (2 * p1 - p0 + c2 * lap) * damp
    for (si, sj, d) in srcs:
        p2[sj - 1:sj + 2, si - 1:si + 2] += ricker(n - d) * 0.5
    p2[:surf + 1, :] = 0.0
    p0, p1 = p1 * damp, p2
    gather[n] = p1[rec_j, rec_x]
    if n == snap_step: snap = p1.copy()

# ── Canvas ───────────────────────────────────────────────────────────────
img = Image.new('RGBA', (SW, SH), rgb('#010408'))
d = ImageDraw.Draw(img, 'RGBA')   # blend, don't overwrite alpha
def S(v): return int(round(v * SS))

def vgrad(box, top, bot):
    x0, y0, x1, y1 = [S(v) for v in box]
    g = Image.new('RGBA', (1, max(1, y1 - y0)))
    for y in range(y1 - y0):
        t = y / max(1, y1 - y0 - 1)
        g.putpixel((0, y), tuple(int(top[k] + (bot[k] - top[k]) * t) for k in range(4)))
    img.alpha_composite(g.resize((x1 - x0, y1 - y0)), (x0, y0))

vgrad((0, 0, W, SEA), rgb('#02060f'), rgb('#06152b'))
vgrad((0, SEA, W, H), rgb('#071a33'), rgb('#040d1c'))

def layer(fn, fill_top, fill_bot, line, lw, gtop=None):
    pts = [(S(x), S(fn(x))) for x in range(-10, W + 11, 4)]
    poly = pts + [(S(W + 10), SH + 10), (S(-10), SH + 10)]
    mask = Image.new('L', (SW, SH), 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)
    top = min(p[1] for p in pts) // SS if gtop is None else gtop
    grad = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(S(top), SH):
        t = (y - S(top)) / max(1, SH - S(top))
        gd.line([(0, y), (SW, y)], fill=tuple(int(fill_top[k] + (fill_bot[k] - fill_top[k]) * t) for k in range(4)))
    img.paste(grad, (0, 0), mask)
    d.line(pts, fill=line, width=S(lw), joint='curve')

layer(seafloor, rgb('#13284a'), rgb('#0a1830'), rgb('#2f5d96'), 2)
layer(deep, rgb('#0c1b31'), rgb('#08111f'), rgb('#22385a'), 2)
layer(salt, rgb('#c3d3e4'), rgb('#3e5674'), rgb('#eaf5ff'), 3, gtop=470)

# ── Wavefield overlay (app colormap: white = +, black = −, alpha ∝ |P|) ─
amp = snap / (np.percentile(np.abs(snap[surf + 2:]), 99.0) + 1e-9)
amp = np.tanh(amp * 1.8)
a = np.clip(np.abs(amp) * 255 * np.where(amp > 0, 1.15, 0.55), 0, 255)
a[np.abs(amp) < 0.02] = 0
rgba = np.zeros((nz, nx, 4), np.uint8)
pos = amp > 0
rgba[..., 0] = np.where(pos, 235, 40); rgba[..., 1] = np.where(pos, 250, 110); rgba[..., 2] = np.where(pos, 255, 255)   # + white, − blue
rgba[..., 3] = a.astype(np.uint8)
wave = Image.fromarray(rgba, 'RGBA').resize((SW, SH), Image.BICUBIC)
img.alpha_composite(wave)
# cyan bloom on the strongest crests
bloom = Image.fromarray(np.where(pos[..., None], rgba, 0).astype(np.uint8), 'RGBA').resize((SW, SH), Image.BICUBIC)
br = np.array(bloom); br[..., 0] = 0; br[..., 1] = 230; br[..., 2] = 255; br[..., 3] = (br[..., 3] * 0.35).astype(np.uint8)
img.alpha_composite(Image.fromarray(br).filter(ImageFilter.GaussianBlur(S(4))))

# sea line glow
glow = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
ImageDraw.Draw(glow).line([(0, S(SEA)), (SW, S(SEA))], fill=rgb('#1e90ff', 200), width=S(4))
img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(S(4))))
d.line([(0, S(SEA)), (SW, S(SEA))], fill=rgb('#4aa8ff', 170), width=S(1.5))

# ── Text-side vignette (left) ────────────────────────────────────────────
vig = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
vd = ImageDraw.Draw(vig)
for x in range(0, S(760)):
    t = x / S(760)
    vd.line([(x, 0), (x, S(SEA - 8))], fill=(1, 4, 8, int(235 * (1 - t) ** 1.6)))
img.alpha_composite(vig)
# bottom-left backing for chips + footer
bv = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
bd = ImageDraw.Draw(bv)
for y in range(S(560), SH):
    ty = (y - S(560)) / (SH - S(560))
    bd.line([(0, y), (S(880), y)], fill=(1, 4, 8, int(230 * ty ** 0.8)))
mask = Image.new('L', (SW, SH), 0)
md = ImageDraw.Draw(mask)
for x in range(0, S(880)):
    md.line([(x, 0), (x, SH)], fill=int(255 * min(1.0, (S(880) - x) / S(260))))
img.paste(Image.alpha_composite(img.copy(), bv), (0, 0), mask)

# ── Acquisition: streamer, ships, sources, OBS ──────────────────────────
def glow_draw(fn, color, blur):
    lay = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
    fn(ImageDraw.Draw(lay))
    img.alpha_composite(lay.filter(ImageFilter.GaussianBlur(S(blur))))
    img.alpha_composite(lay)

def streamer(dd):
    pts = [(S(x), S(SEA + 10 + 2 * math.sin(x * 0.04))) for x in range(40, 281, 3)]
    for k in range(len(pts) - 1):
        t = k / (len(pts) - 1)
        dd.line([pts[k], pts[k + 1]], fill=(255, int(140 + 75 * t), 0, int(110 + 145 * t)), width=S(3))
    for x in range(40, 281, 16):
        y = SEA + 10 + 2 * math.sin(x * 0.04)
        dd.ellipse([S(x - 2.6), S(y - 2.6), S(x + 2.6), S(y + 2.6)], fill=(255, 255, 255, 255))
glow_draw(streamer, None, 4)
d.line([(S(285), S(SEA + 10)), (S(318), S(SEA))], fill=(255, 255, 255, 110), width=S(1))

def ship(x, c1, c2, label):
    def f(dd):
        w, h = 86, 22
        bx, by = x - w / 2, SEA - h
        dd.polygon([(S(bx + 6), S(by)), (S(bx + w - 6), S(by)), (S(bx + w + 8), S(SEA - 2)), (S(bx), S(SEA - 2))], fill=c1)
        dd.rectangle([S(bx + w * 0.52), S(by - 11), S(bx + w * 0.52 + 22), S(by)], fill=c2)
        dd.rectangle([S(bx + 8), S(by - 6), S(bx + 13), S(by + 2)], fill=c2)
    glow_draw(f, c2, 7)
    t = font('seguisb.ttf', 10.5)
    tw = d.textlength(label, font=t)
    d.text((S(x) - tw / 2, S(SEA - 52)), label, font=t, fill=rgb('#e8f4fd'))

def source(x, color):
    def f(dd):
        cx, cy = S(x), S(SEA + 3)
        dd.ellipse([cx - S(4), cy - S(4), cx + S(4), cy + S(4)], fill=color)
        for r in (8, 13):
            dd.ellipse([cx - S(r), cy - S(r), cx + S(r), cy + S(r)], outline=color, width=S(1.4))
    glow_draw(f, color, 5)

ship(350, rgb('#1e90ff'), rgb('#00d4ff'), 'ANA GEMİ')
source(300, rgb('#2a9dff'))
ship(810, rgb('#ff4d6d'), rgb('#ff8c42'), '2. GEMİ')
source(760, rgb('#ff5c7a'))

def obs(dd):
    for x in range(560, W, 64):
        y = seafloor(x)
        dd.polygon([(S(x), S(y - 1)), (S(x - 6), S(y + 9)), (S(x + 6), S(y + 9))], fill=rgb('#fbbf24'))
glow_draw(obs, None, 4)

# ── Title block ──────────────────────────────────────────────────────────
X0 = 64
# app icon tile
tile = Image.new('RGBA', (S(64), S(64)), (0, 0, 0, 0))
g = np.zeros((S(64), S(64), 4), np.uint8)
yy, xx = np.mgrid[0:S(64), 0:S(64)]
t = (xx + yy) / (2 * S(64))
g[..., 0] = (0 + 0 * t).astype(np.uint8); g[..., 1] = (132 + 110 * t).astype(np.uint8); g[..., 2] = 255; g[..., 3] = 255
tile = Image.fromarray(g)
m = Image.new('L', tile.size, 0); ImageDraw.Draw(m).rounded_rectangle([0, 0, S(64) - 1, S(64) - 1], radius=S(16), fill=255)
halo = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
ImageDraw.Draw(halo).rounded_rectangle([S(X0 - 6), S(58 - 6), S(X0 + 70), S(58 + 70)], radius=S(20), fill=rgb('#0084ff', 120))
img.alpha_composite(halo.filter(ImageFilter.GaussianBlur(S(14))))
img.paste(tile, (S(X0), S(58)), m)
# icon glyph: wavefronts from a source point
ig = ImageDraw.Draw(img, 'RGBA')
cx, cy = S(X0 + 20), S(58 + 44)
for r, a_ in ((10, 255), (20, 200), (30, 140), (40, 80)):
    ig.arc([cx - S(r), cy - S(r), cx + S(r), cy + S(r)], start=-90, end=0, fill=(255, 255, 255, a_), width=S(3.2))
ig.ellipse([cx - S(4), cy - S(4), cx + S(4), cy + S(4)], fill=(255, 255, 255, 255))

kick = font('seguisb.ttf', 13)
d.text((S(X0 + 84), S(64)), 'ADVANCED GEOPHYSICAL CORE', font=kick, fill=rgb('#7fe9ff'))
d.text((S(X0 + 84), S(86)), 'v2.4  ·  Native C++ / OpenGL', font=font('segoeui.ttf', 13), fill=rgb('#8a9bb3'))

def gradient_text(xy, text, f, c0, c1):
    x, y = xy
    bbox = d.textbbox((0, 0), text, font=f)
    w_, h_ = bbox[2], bbox[3]
    mask = Image.new('L', (w_ + 4, h_ + 4), 0)
    ImageDraw.Draw(mask).text((0, 0), text, font=f, fill=255)
    gr = np.zeros((h_ + 4, w_ + 4, 4), np.uint8)
    tt = np.linspace(0, 1, w_ + 4)[None, :]
    for k in range(3): gr[..., k] = (c0[k] + (c1[k] - c0[k]) * tt).astype(np.uint8)
    gr[..., 3] = 255
    img.paste(Image.fromarray(gr), (x, y), mask)

title = font('segoeuib.ttf', 60)
gradient_text((S(X0 - 4), S(140)), 'Seismic FDTD Solver', title, (255, 255, 255), (0, 236, 255))
d.text((S(X0), S(226)), 'Marine seismic acquisition simulator  —  SLO · CLO · Streamer · OBS', font=font('segoeui.ttf', 18), fill=rgb('#b7c5d8'))

# ── Feature chips ────────────────────────────────────────────────────────
def chip(x, y, text, col):
    f_ = font('consolab.ttf', 12.5)
    tw = d.textlength(text, font=f_)
    w_ = tw / SS + 26
    ov = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))   # translucent shapes must go through alpha_composite
    ImageDraw.Draw(ov).rounded_rectangle([S(x), S(y), S(x + w_), S(y + 28)], radius=S(8), fill=col[:3] + (40,),
                                         outline=col[:3] + (150,), width=S(1))
    img.alpha_composite(ov)
    d.text((S(x + 13), S(y + 6.5)), text, font=f_, fill=col)
    return x + w_ + 10

cx_ = X0
for txt, col in (('C++20', rgb('#7fe9ff')), ('OpenGL 4.3 COMPUTE', rgb('#00ff9d')), ('CPML · O2–O10', rgb('#ffb347')),
                 ('SEG-Y REV1', rgb('#c084fc'))):
    cx_ = chip(cx_, 618, txt, col)

# ── Live HUD (top-right, as on the app canvas) ──────────────────────────
def rtext(y, text, f_, col):
    d.text((S(1216) - d.textlength(text, font=f_), S(y)), text, font=f_, fill=col)
rtext(62, 'Staggered Grid: 1601 x 721 | \u0394h = 0.75m', font('consola.ttf', 13), rgb('#d6e2ee', 210))
rtext(82, 'Toplam H\u00fccre: 1154321', font('consola.ttf', 13), rgb('#d6e2ee', 210))
rtext(104, 'T: 512.0 ms', font('consolab.ttf', 20), rgb('#00e5a0'))
rtext(134, 'ANA GEMI ATIS: 14', font('consola.ttf', 12), rgb('#3aa0ff'))
rtext(152, 'KAYNAK ATIS: 14', font('consola.ttf', 12), rgb('#ff5c7a'))

# ── Sismogram inset (real gather from the simulation) ───────────────────
gx, gy, gw, gh = 900, 466, 330, 210
panel = Image.new('RGBA', (S(gw), S(gh)), (0, 0, 0, 0))
pd_ = ImageDraw.Draw(panel)
pd_.rounded_rectangle([0, 0, S(gw) - 1, S(gh) - 1], radius=S(14), fill=(2, 8, 18, 236), outline=(30, 144, 255, 90), width=S(1))
shadow = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle([S(gx), S(gy + 8), S(gx + gw), S(gy + gh + 8)], radius=S(14), fill=(0, 0, 0, 170))
img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(S(14))))
img.alpha_composite(panel, (S(gx), S(gy)))
d.text((S(gx + 16), S(gy + 11)), 'STREAMER GATHER', font=font('consolab.ttf', 10.5), fill=rgb('#8ab4d4'))
d.text((S(gx + gw - 16) - d.textlength('LIVE', font=font('consolab.ttf', 10.5)), S(gy + 11)), 'LIVE',
       font=font('consolab.ttf', 10.5), fill=rgb('#00e5a0'))
d.ellipse([S(gx + gw - 52), S(gy + 16), S(gx + gw - 46), S(gy + 22)], fill=rgb('#00e5a0'))
px0, py0, pw, ph = gx + 16, gy + 34, gw - 32, gh - 48
for ms in range(0, 6):
    yl = py0 + ph * ms / 5
    d.line([(S(px0), S(yl)), (S(px0 + pw), S(yl))], fill=(255, 255, 255, 18), width=1)
G = gather[:1300]
G = G / (np.percentile(np.abs(G), 99.5) + 1e-9)
ntr = G.shape[1]
lay = Image.new('RGBA', (SW, SH), (0, 0, 0, 0))
ld = ImageDraw.Draw(lay)
for k in range(ntr):
    x = px0 + pw * (1 - (k + 0.5) / ntr)        # far offset on the left, like the app
    tr = G[:, k]
    pts, fill_pts = [], []
    for s in range(0, len(tr), 3):
        dx = float(np.clip(tr[s] * 3.2, -6, 6))
        y = py0 + ph * s / len(tr)
        pts.append((S(x + dx), S(y)))
        if dx > 0: ld.line([(S(x), S(y)), (S(x + dx), S(y))], fill=rgb('#ff1744', 220), width=S(1.6))
    ld.line(pts, fill=(232, 244, 253, 150), width=S(0.7))
img.alpha_composite(lay)

# ── Footer strip ─────────────────────────────────────────────────────────
d.text((S(X0), S(666)), 'Real-time 2D acoustic wave modelling · staggered grid · expanding-box eikonal acceleration',
       font=font('segoeui.ttf', 13.5), fill=rgb('#9fb0c6'))

# subtle outer frame
d.rectangle([0, 0, SW - 1, SH - 1], outline=(255, 255, 255, 14), width=S(1))

img = img.resize((W, H), Image.LANCZOS).convert('RGB')
img.save(OUT, optimize=True)
print('saved', OUT)
