#include "Report.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"   // from Dear ImGui (stb_truetype 1.26)

namespace report {

// ══════════════════════════════════════════════════════════════════════════
// zlib: LZ77 (hash chains, 32 KB window) + fixed Huffman codes, one block
// ══════════════════════════════════════════════════════════════════════════
namespace {

struct Bits {
    std::vector<uint8_t>& o;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int cnt) {   // LSB first
        acc |= v << n;
        n += cnt;
        while (n >= 8) { o.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; n -= 8; }
    }
    void huff(uint32_t code, int len) {   // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
        put(r, len);
    }
    void flush() { if (n > 0) { o.push_back((uint8_t)(acc & 0xFF)); acc = 0; n = 0; } }
};

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void literal(Bits& b, int v) {
    if (v < 144) b.huff(0x30 + v, 8);
    else if (v < 256) b.huff(0x190 + (v - 144), 9);
    else if (v < 280) b.huff(v - 256, 7);
    else b.huff(0xC0 + (v - 280), 8);
}

void match(Bits& b, int len, int dist) {
    int li = 28;
    if (len < 258) { li = 0; while (li < 27 && kLenBase[li + 1] <= len) li++; }
    literal(b, 257 + li);
    if (kLenExtra[li]) b.put((uint32_t)(len - kLenBase[li]), kLenExtra[li]);
    int di = 0;
    while (di < 29 && kDistBase[di + 1] <= dist) di++;
    b.huff((uint32_t)di, 5);
    if (kDistExtra[di]) b.put((uint32_t)(dist - kDistBase[di]), kDistExtra[di]);
}

uint32_t adler32(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> r{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            r[i] = k;
        }
        return r;
    }();
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

void be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v);
}

} // namespace

std::vector<uint8_t> zlibCompress(const uint8_t* d, size_t n) {
    std::vector<uint8_t> out{0x78, 0x9C};
    Bits b{out};
    b.put(1, 1);   // final block
    b.put(1, 2);   // fixed Huffman
    constexpr int W = 32768, HB = 16;
    std::vector<int> head((size_t)1 << HB, -1), prev(W, -1);
    auto hash = [&](size_t i) { return (((uint32_t)d[i] << 10) ^ ((uint32_t)d[i + 1] << 5) ^ d[i + 2]) & ((1u << HB) - 1); };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        const uint32_t h = hash(i);
        prev[i & (W - 1)] = head[h];
        head[h] = (int)i;
    };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int p = head[hash(i)], chain = 48;
            const int maxL = (int)std::min<size_t>(258, n - i);
            while (p >= 0 && (int)i - p <= W && chain-- > 0) {
                int L = 0;
                while (L < maxL && d[p + L] == d[i + L]) L++;
                if (L > bestLen) { bestLen = L; bestDist = (int)i - p; if (L == maxL) break; }
                const int q = prev[p & (W - 1)];
                if (q >= p) break;   // slot reused by a newer position: end of this chain
                p = q;
            }
        }
        if (bestLen >= 3) {
            match(b, bestLen, bestDist);
            for (int k = 0; k < bestLen; k++) insert(i + k);
            i += bestLen;
        } else {
            literal(b, d[i]);
            insert(i);
            i++;
        }
    }
    literal(b, 256);
    b.flush();
    be32(out, adler32(d, n));
    return out;
}

bool writePng(const std::string& path, const Image& img, std::string& err) {
    // Sub filter on every row: the smooth colour ramps compress far better as differences
    const size_t stride = (size_t)img.w * 3;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * img.h);
    for (int y = 0; y < img.h; y++) {
        raw.push_back(1);
        const uint8_t* r = img.rgb.data() + y * stride;
        for (size_t x = 0; x < stride; x++) raw.push_back((uint8_t)(r[x] - (x >= 3 ? r[x - 3] : 0)));
    }
    const std::vector<uint8_t> z = zlibCompress(raw.data(), raw.size());
    std::vector<uint8_t> f{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(f, (uint32_t)data.size());
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        f.insert(f.end(), td.begin(), td.end());
        be32(f, crc32(td.data(), td.size()));
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)img.w);
    be32(ihdr, (uint32_t)img.h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream o(std::filesystem::u8path(path), std::ios::binary);
    if (!o) { err = "Dosya yazılamadı: " + path; return false; }
    o.write((const char*)f.data(), (std::streamsize)f.size());
    return (bool)o;
}

// ══════════════════════════════════════════════════════════════════════════
// Fonts
// ══════════════════════════════════════════════════════════════════════════
namespace {

struct Face {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
    std::string psName;
    int upem = 2048;
};

Face loadFace(std::initializer_list<std::pair<const char*, const char*>> candidates) {
    Face f;
    const char* win = std::getenv("WINDIR");
    const std::filesystem::path dir = std::filesystem::path(win ? win : "C:\\Windows") / "Fonts";
    for (const auto& c : candidates) {
        std::ifstream in(dir / c.first, std::ios::binary);
        if (!in) continue;
        f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (f.data.size() < 12 || !stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) continue;
        f.ok = true;
        f.psName = c.second;
        f.upem = ttUSHORT(f.data.data() + f.info.head + 18);
        return f;
    }
    return Face{};
}

Face& regularFace() {
    static Face f = loadFace({{"segoeui.ttf", "SegoeUI"}, {"arial.ttf", "Arial"}});
    return f;
}
Face& boldFace() {
    static Face f = loadFace({{"seguisb.ttf", "SegoeUI-Semibold"}, {"arialbd.ttf", "Arial-Bold"}});
    return f;
}

std::vector<uint32_t> utf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = (uint8_t)s[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 0x07; n = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        i++;
        for (int k = 0; k < n && i < s.size(); k++, i++) cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
        out.push_back(cp);
    }
    return out;
}

// Advance width of a string in font units
double unitsWidth(Face& f, const std::string& s) {
    double w = 0;
    for (uint32_t cp : utf8(s)) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f.info, stbtt_FindGlyphIndex(&f.info, (int)cp), &adv, &lsb);
        w += adv;
    }
    return w;
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════
// Raster drawing (model image)
// ══════════════════════════════════════════════════════════════════════════
namespace {

struct RGB { uint8_t r, g, b; };
const RGB kInk{0x1E, 0x29, 0x33}, kMuted{0x5C, 0x6B, 0x78}, kFrame{0x3A, 0x47, 0x52};
const RGB kShot{0xD6, 0x3F, 0x36}, kShot2{0x1F, 0x8F, 0xB8}, kCable{0xF2, 0xC2, 0x30}, kObs{0xE8, 0xA5, 0x4B};

struct Canvas {
    Image& im;
    void blend(int x, int y, RGB c, float a) {
        if (x < 0 || y < 0 || x >= im.w || y >= im.h || a <= 0) return;
        a = std::min(a, 1.0f);
        uint8_t* p = im.rgb.data() + ((size_t)y * im.w + x) * 3;
        p[0] = (uint8_t)std::lround(p[0] + (c.r - p[0]) * a);
        p[1] = (uint8_t)std::lround(p[1] + (c.g - p[1]) * a);
        p[2] = (uint8_t)std::lround(p[2] + (c.b - p[2]) * a);
    }
    void rect(int x0, int y0, int x1, int y1, RGB c) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) blend(x, y, c, 1);
    }
    void frame(int x0, int y0, int x1, int y1, RGB c, int t = 1) {
        rect(x0, y0, x1, y0 + t, c); rect(x0, y1 - t, x1, y1, c);
        rect(x0, y0, x0 + t, y1, c); rect(x1 - t, y0, x1, y1, c);
    }
    void disc(float cx, float cy, float r, RGB c) {   // anti-aliased edge
        for (int y = (int)std::floor(cy - r - 1); y <= (int)std::ceil(cy + r + 1); y++)
            for (int x = (int)std::floor(cx - r - 1); x <= (int)std::ceil(cx + r + 1); x++) {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                blend(x, y, c, std::clamp(r - d + 0.5f, 0.0f, 1.0f));
            }
    }
    void line(float x0, float y0, float x1, float y1, float w, RGB c) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int n = std::max(1, (int)std::ceil(len / 0.5f));
        for (int i = 0; i <= n; i++) {
            const float t = (float)i / n;
            disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w * 0.5f, c);
        }
    }
    void triangle(float cx, float cy, float s, RGB fill, RGB edge) {   // pointing up, centred on (cx, cy)
        const float h = s * 0.866f;
        const float ax = cx, ay = cy - h * 0.6f, bx = cx - s * 0.5f, by = cy + h * 0.4f, qx = cx + s * 0.5f, qy = by;
        for (int y = (int)std::floor(ay) - 1; y <= (int)std::ceil(by) + 1; y++)
            for (int x = (int)std::floor(bx) - 1; x <= (int)std::ceil(qx) + 1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
                const float d2 = (px - ax) * (qy - ay) - (qx - ax) * (py - ay);
                const float d3 = (px - qx) * (by - qy) - (bx - qx) * (py - qy);
                if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) blend(x, y, fill, 1);
            }
        line(ax, ay, bx, by, 1.4f, edge); line(bx, by, qx, qy, 1.4f, edge); line(qx, qy, ax, ay, 1.4f, edge);
    }
    float textWidth(Face& f, float px, const std::string& s) {
        return (float)(unitsWidth(f, s) * stbtt_ScaleForMappingEmToPixels(&f.info, px));
    }
    // align: 0 left, 1 centre, 2 right; y = baseline. vertical: rotated 90 deg counter-clockwise
    // (reads bottom to top), x = baseline, y = the text's anchor along the line.
    void text(Face& f, float px, float x, float y, RGB c, const std::string& s, int align = 0, bool vertical = false) {
        if (!f.ok) return;
        const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, px);
        const float w = textWidth(f, px, s);
        float pen = align == 1 ? -w * 0.5f : align == 2 ? -w : 0.0f;
        std::vector<uint8_t> bmp;
        for (uint32_t cp : utf8(s)) {
            const int g = stbtt_FindGlyphIndex(&f.info, (int)cp);
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&f.info, g, &adv, &lsb);
            const float fx = pen - std::floor(pen);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f.info, g, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
            const int bw = x1 - x0, bh = y1 - y0;
            if (bw > 0 && bh > 0) {
                bmp.assign((size_t)bw * bh, 0);
                stbtt_MakeGlyphBitmapSubpixel(&f.info, bmp.data(), bw, bh, bw, scale, scale, fx, 0, g);
                const int ox = (int)std::floor(pen) + x0;
                for (int j = 0; j < bh; j++)
                    for (int i = 0; i < bw; i++) {
                        const float a = bmp[(size_t)j * bw + i] / 255.0f;
                        if (!vertical) blend((int)std::lround(x) + ox + i, (int)std::lround(y) + y0 + j, c, a);
                        else blend((int)std::lround(x) + y0 + j, (int)std::lround(y) - (ox + i), c, a);
                    }
            }
            pen += adv * scale;
        }
    }
};

RGB turbo(double t) {   // Google's Turbo colour map (polynomial fit)
    t = std::clamp(t, 0.0, 1.0);
    const double r = 0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943))));
    const double g = 0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604))));
    const double b = 0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973))));
    auto u8 = [](double v) { return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); };
    return {u8(r), u8(g), u8(b)};
}

double niceStep(double range, int target) {
    const double raw = range / std::max(1, target);
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * p >= raw) return m * p;
    return 10 * p;
}

std::string fmtNum(double v, double step) {
    char b[32];
    if (step >= 1) std::snprintf(b, sizeof(b), "%.0f", v);
    else std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

} // namespace

bool renderModel(const ModelPlot& m, Image& out, std::string& err) {
    if (m.nx < 2 || m.nz < 2 || !m.vel) { err = "Model boş."; return false; }
    Face& reg = regularFace();
    Face& bold = boldFace();
    const double widthM = (m.nx - 1) * m.dx, depthM = (m.nz - 1) * m.dz;
    const int PW = 1500;
    const int PH = std::clamp((int)std::lround(PW * depthM / widthM), 260, 820);
    const double exag = (double)PH / PW / (depthM / widthM);
    const int L = 120, R = 200, T = 150, B = 120;
    out.w = L + PW + R;
    out.h = T + PH + B;
    out.rgb.assign((size_t)out.w * out.h * 3, 255);
    Canvas cv{out};

    float vmin = 1e30f, vmax = -1e30f;
    for (size_t i = 0; i < (size_t)m.nx * m.nz; i++) { vmin = std::min(vmin, m.vel[i]); vmax = std::max(vmax, m.vel[i]); }
    if (vmax - vmin < 1) vmax = vmin + 1;
    // Turbo from its deep blue on (its first 8 % run into near black, which made water look empty)
    auto colourOf = [&](double v) { return turbo(0.08 + 0.92 * (v - vmin) / (vmax - vmin)); };

    // Model (nearest grid node: the grid the solver used, not smoothed)
    for (int py = 0; py < PH; py++) {
        const int k = std::clamp((int)std::lround(py / (double)(PH - 1) * depthM / m.dz), 0, m.nz - 1);
        for (int px = 0; px < PW; px++) {
            const int i = std::clamp((int)std::lround(px / (double)(PW - 1) * widthM / m.dx), 0, m.nx - 1);
            const RGB c = colourOf(m.vel[(size_t)k * m.nx + i]);
            uint8_t* p = out.rgb.data() + ((size_t)(T + py) * out.w + L + px) * 3;
            p[0] = c.r; p[1] = c.g; p[2] = c.b;
        }
    }
    auto X = [&](double x) { return (float)(L + x / widthM * (PW - 1)); };
    auto Z = [&](double z) { return (float)(T + z / depthM * (PH - 1)); };

    // Geometry
    if (m.cable) cv.line(X(m.cableX0), Z(m.cableZ0), X(m.cableX1), Z(m.cableZ1), 4.0f, kCable);
    for (double x : m.shotsSrc) cv.disc(X(x), Z(m.sourceDepth), 4.5f, kShot2);
    for (double x : m.shotsMain) cv.disc(X(x), Z(m.sourceDepth), 4.5f, kShot);
    for (const auto& o : m.obs) cv.triangle(X(o.first), Z(o.second) - 6, 15, kObs, kInk);

    cv.frame(L - 1, T - 1, L + PW + 1, T + PH + 1, kFrame, 2);

    // Titles
    cv.text(bold, 28, (float)L, 44, kInk, m.title);
    if (!m.subtitle.empty()) cv.text(reg, 18, (float)L, 74, kMuted, m.subtitle);

    // Distance axis (top, as in seismic sections)
    const double xs = niceStep(widthM, 10);
    for (double x = 0; x <= widthM + 1e-6; x += xs) {
        cv.rect((int)X(x), T - 9, (int)X(x) + 2, T - 1, kFrame);
        cv.text(reg, 17, X(x), (float)(T - 16), kInk, fmtNum(x, xs), 1);
    }
    cv.text(bold, 18, X(widthM * 0.5), (float)(T - 46), kInk, "Uzaklık (m)", 1);
    // Depth axis
    const double zs = niceStep(depthM, std::max(4, PH / 70));
    for (double z = 0; z <= depthM + 1e-6; z += zs) {
        cv.rect(L - 9, (int)Z(z), L - 1, (int)Z(z) + 2, kFrame);
        cv.text(reg, 17, (float)(L - 14), Z(z) + 6, kInk, fmtNum(z, zs), 2);
    }
    cv.text(bold, 18, (float)(L - 72), Z(depthM * 0.5), kInk, "Derinlik (m)", 1, true);

    // Colour bar
    const int cx0 = L + PW + 46, cx1 = cx0 + 28;
    for (int py = 0; py < PH; py++) {
        const RGB c = colourOf(vmax - (vmax - vmin) * py / (double)(PH - 1));
        cv.rect(cx0, T + py, cx1, T + py + 1, c);
    }
    cv.frame(cx0 - 1, T - 1, cx1 + 1, T + PH + 1, kFrame, 1);
    cv.text(bold, 18, (float)(cx0 + 14), (float)(T - 46), kInk, "Hız (m/s)", 1);
    const double vs = niceStep(vmax - vmin, std::max(4, PH / 80));
    for (double v = std::ceil(vmin / vs) * vs; v <= vmax + 1e-6; v += vs) {
        const float y = (float)(T + (vmax - v) / (vmax - vmin) * (PH - 1));
        cv.rect(cx1, (int)y, cx1 + 7, (int)y + 2, kFrame);
        cv.text(reg, 17, (float)(cx1 + 12), y + 6, kInk, fmtNum(v, vs));
    }

    // Legend
    float lx = (float)L;
    const float ly = (float)(T + PH + 56);
    auto key = [&](const char* label, int kind, RGB c) {
        if (kind == 0) cv.disc(lx + 8, ly - 6, 5, c);
        else if (kind == 1) cv.line(lx, ly - 6, lx + 26, ly - 6, 4, c);
        else cv.triangle(lx + 9, ly - 6, 15, c, kInk);
        lx += kind == 1 ? 36 : 24;
        cv.text(reg, 17, lx, ly, kInk, label);
        lx += cv.textWidth(reg, 17, label) + 34;
    };
    char b[96];
    std::snprintf(b, sizeof(b), "Ana kaynak atışları (%zu)", m.shotsMain.size());
    key(b, 0, kShot);
    if (!m.shotsSrc.empty()) {
        std::snprintf(b, sizeof(b), "Kaynak gemisi atışları (%zu)", m.shotsSrc.size());
        key(b, 0, kShot2);
    }
    if (m.cable) key("Streamer (ilk atış)", 1, kCable);
    if (!m.obs.empty()) {
        std::snprintf(b, sizeof(b), "OBS (%zu)", m.obs.size());
        key(b, 2, kObs);
    }
    if (std::fabs(exag - 1.0) > 0.05) {
        std::snprintf(b, sizeof(b), "Düşey ölçek %.1f kat abartılı", exag);
        cv.text(reg, 17, (float)(L + PW), ly, kMuted, b, 2);
    }
    if (!reg.ok) { err = "Yazı tipi bulunamadı; görsel yazısız çizildi."; }
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// PDF: A4 pages, embedded TrueType subsets (Identity-H), Flate streams
// ══════════════════════════════════════════════════════════════════════════
namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void wr16(std::vector<uint8_t>& o, size_t at, uint16_t v) { o[at] = (uint8_t)(v >> 8); o[at + 1] = (uint8_t)v; }
void wr32(std::vector<uint8_t>& o, size_t at, uint32_t v) {
    o[at] = (uint8_t)(v >> 24); o[at + 1] = (uint8_t)(v >> 16); o[at + 2] = (uint8_t)(v >> 8); o[at + 3] = (uint8_t)v;
}
uint32_t tableSum(const uint8_t* p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint8_t q[4] = {0, 0, 0, 0};
        for (size_t k = 0; k < 4 && i + k < n; k++) q[k] = p[i + k];
        s += rd32(q);
    }
    return s;
}

// Subset: every glyph not in `keep` (plus the components of kept composites) becomes empty;
// glyph ids are unchanged, so the content streams can use them directly (CIDToGIDMap Identity).
std::vector<uint8_t> subsetTrueType(const std::vector<uint8_t>& font, std::set<int> keep) {
    const uint8_t* f = font.data();
    struct Tab { std::string tag; uint32_t off, len; };
    std::map<std::string, Tab> tabs;
    const int nt = rd16(f + 4);
    for (int i = 0; i < nt; i++) {
        const uint8_t* r = f + 12 + 16 * i;
        Tab t{std::string((const char*)r, 4), rd32(r + 8), rd32(r + 12)};
        tabs[t.tag] = t;
    }
    if (!tabs.count("glyf") || !tabs.count("loca") || !tabs.count("head") || !tabs.count("maxp")) return font;
    const int numGlyphs = rd16(f + tabs["maxp"].off + 4);
    const bool longLoca = rd16(f + tabs["head"].off + 50) == 1;
    auto loc = [&](int g) -> uint32_t {
        const uint8_t* l = f + tabs["loca"].off;
        return longLoca ? rd32(l + 4 * g) : 2u * rd16(l + 2 * g);
    };
    const uint8_t* glyf = f + tabs["glyf"].off;
    keep.insert(0);
    std::vector<int> stack(keep.begin(), keep.end());
    while (!stack.empty()) {   // add composite components
        const int g = stack.back();
        stack.pop_back();
        if (g < 0 || g >= numGlyphs) continue;
        const uint32_t a = loc(g), b = loc(g + 1);
        if (b <= a || (int16_t)rd16(glyf + a) >= 0) continue;
        size_t p = a + 10;
        for (;;) {
            const uint16_t flags = rd16(glyf + p), comp = rd16(glyf + p + 2);
            if (!keep.count(comp)) { keep.insert(comp); stack.push_back(comp); }
            p += 4 + ((flags & 0x0001) ? 4 : 2);
            if (flags & 0x0008) p += 2;
            else if (flags & 0x0040) p += 4;
            else if (flags & 0x0080) p += 8;
            if (!(flags & 0x0020)) break;
        }
    }
    std::vector<uint8_t> newGlyf, newLoca;
    for (int g = 0; g < numGlyphs; g++) {
        newLoca.resize(newLoca.size() + 4);
        wr32(newLoca, newLoca.size() - 4, (uint32_t)newGlyf.size());
        if (keep.count(g)) {
            newGlyf.insert(newGlyf.end(), glyf + loc(g), glyf + loc(g + 1));
            while (newGlyf.size() % 4) newGlyf.push_back(0);
        }
    }
    newLoca.resize(newLoca.size() + 4);
    wr32(newLoca, newLoca.size() - 4, (uint32_t)newGlyf.size());

    std::map<std::string, std::vector<uint8_t>> out;
    for (const char* t : {"head", "hhea", "maxp", "hmtx", "cvt ", "fpgm", "prep", "OS/2", "cmap", "name"})
        if (tabs.count(t)) out[t] = std::vector<uint8_t>(f + tabs[t].off, f + tabs[t].off + tabs[t].len);
    out["glyf"] = newGlyf;
    out["loca"] = newLoca;
    wr16(out["head"], 50, 1);   // long loca
    wr32(out["head"], 8, 0);    // checkSumAdjustment, set below

    const uint16_t n = (uint16_t)out.size();
    uint16_t sr = 1, es = 0;
    while (sr * 2 <= n) { sr *= 2; es++; }
    std::vector<uint8_t> r(12 + 16 * (size_t)n, 0);
    wr32(r, 0, rd32(f));
    wr16(r, 4, n); wr16(r, 6, (uint16_t)(sr * 16)); wr16(r, 8, es); wr16(r, 10, (uint16_t)(n * 16 - sr * 16));
    size_t headAt = 0, i = 0;
    for (auto& [tag, data] : out) {   // std::map: tags in ascending order, as required
        const size_t at = r.size();
        if (tag == "head") headAt = at;
        std::memcpy(&r[12 + 16 * i], tag.data(), 4);
        wr32(r, 12 + 16 * i + 4, tableSum(data.data(), data.size()));
        wr32(r, 12 + 16 * i + 8, (uint32_t)at);
        wr32(r, 12 + 16 * i + 12, (uint32_t)data.size());
        r.insert(r.end(), data.begin(), data.end());
        while (r.size() % 4) r.push_back(0);
        i++;
    }
    wr32(r, headAt + 8, 0xB1B0AFBAu - tableSum(r.data(), r.size()));
    return r;
}

struct PdfFont {
    Face* face;
    std::string res;                    // resource name, "F1"
    std::map<int, uint32_t> used;       // glyph -> code point (ToUnicode)
    double width(const std::string& s, double size) { return unitsWidth(*face, s) * size / face->upem; }
    std::string hex(const std::string& s) {
        std::string h = "<";
        char b[8];
        for (uint32_t cp : utf8(s)) {
            const int g = stbtt_FindGlyphIndex(&face->info, (int)cp);
            used.emplace(g, cp);
            std::snprintf(b, sizeof(b), "%04X", g);
            h += b;
        }
        return h + ">";
    }
};

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

struct Page { std::string ops; };

struct Layout {
    static constexpr double PW = 595.28, PH = 841.89, M = 48, CW = PW - 2 * M;
    PdfFont& reg;
    PdfFont& bold;
    std::vector<Page> pages;
    double y = 0;
    void newPage() { pages.push_back({}); y = PH - M; }
    std::string& ops() { return pages.back().ops; }
    void text(PdfFont& f, double size, double x, double yy, const char* rgb, const std::string& s) {
        ops() += "BT " + std::string(rgb) + " rg /" + f.res + " " + num(size) + " Tf 1 0 0 1 " + num(x) + " " + num(yy) + " Tm " + f.hex(s) +
                 " Tj ET\n";
    }
    void rect(double x, double yy, double w, double h, const char* rgb) {
        ops() += std::string(rgb) + " rg " + num(x) + " " + num(yy) + " " + num(w) + " " + num(h) + " re f\n";
    }
    void need(double h) { if (y - h < M + 20) newPage(); }
    // Words wrapped to the width; long words (paths) broken where they overflow
    std::vector<std::string> wrap(PdfFont& f, double size, const std::string& s, double maxW) {
        std::vector<std::string> lines;
        std::string cur;
        auto pushWord = [&](std::string w) {
            std::string trial = cur.empty() ? w : cur + " " + w;
            if (f.width(trial, size) <= maxW) { cur = trial; return; }
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
            while (f.width(w, size) > maxW) {   // break inside the word
                size_t k = w.size();
                while (k > 1 && f.width(w.substr(0, k), size) > maxW) k--;
                while (k > 1 && ((uint8_t)w[k] & 0xC0) == 0x80) k--;   // not inside a UTF-8 sequence
                lines.push_back(w.substr(0, k));
                w = w.substr(k);
            }
            cur = w;
        };
        size_t i = 0;
        while (i <= s.size()) {
            const size_t j = s.find(' ', i);
            pushWord(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
        return lines;
    }
};

} // namespace

bool writePdf(const std::string& path, const Document& doc, std::string& err) {
    Face& rf = regularFace();
    Face& bf = boldFace();
    if (!rf.ok || !bf.ok) { err = "PDF için yazı tipi bulunamadı (Segoe UI ya da Arial)."; return false; }
    PdfFont reg{&rf, "F1"}, bold{&bf, "F2"};
    const char* ink = "0.118 0.161 0.2";
    const char* muted = "0.36 0.42 0.47";

    auto layout = [&](int totalPages) {
        Layout L{reg, bold};
        L.newPage();
        // Title block
        L.text(bold, 18, L.M, L.y - 18, ink, doc.title);
        L.y -= 26;
        for (const std::string& ln : L.wrap(reg, 9, doc.subtitle, L.CW)) {
            L.text(reg, 9, L.M, L.y - 9, muted, ln);
            L.y -= 12.5;
        }
        L.y -= 10;
        if (doc.image && doc.image->w > 0) {
            const double w = L.CW, h = w * doc.image->h / doc.image->w;
            L.need(h + 24);
            L.ops() += "q " + num(w) + " 0 0 " + num(h) + " " + num(L.M) + " " + num(L.y - h) + " cm /Im1 Do Q\n";
            L.y -= h + 4;
            for (const std::string& ln : L.wrap(reg, 8, doc.imageCaption, L.CW)) {
                L.text(reg, 8, L.M, L.y - 8, muted, ln);
                L.y -= 11;
            }
            L.y -= 12;
        }
        // Parameter sections: heading, rule, key / value rows with a light band on every other row
        const double rowH = 14.5, size = 9;
        for (const Section& s : doc.sections) {
            L.need(26 + 2 * rowH);
            L.y -= 6;
            L.text(bold, 11.5, L.M, L.y - 11.5, ink, s.title);
            L.y -= 17;
            L.rect(L.M, L.y, L.CW, 0.8, "0.7 0.75 0.79");
            L.y -= 3;
            int r = 0;
            for (const Row& row : s.rows) {
                const double vw = L.CW * 0.55;
                const std::vector<std::string> vl = L.wrap(reg, size, row.value, vw);
                const double h = rowH + (std::max<size_t>(1, vl.size()) - 1) * 11.5;
                if (L.y - h < L.M + 20) { L.newPage(); }
                if (r++ % 2 == 0) L.rect(L.M, L.y - h, L.CW, h, "0.955 0.965 0.975");
                L.text(reg, size, L.M + 6, L.y - 10.5, muted, row.key);
                double yy = L.y - 10.5;
                for (const std::string& v : vl) {
                    L.text(bold, size, L.M + L.CW - 6 - bold.width(v, size), yy, ink, v);
                    yy -= 11.5;
                }
                L.y -= h;
            }
            L.y -= 8;
        }
        // Footers
        for (size_t p = 0; p < L.pages.size(); p++) {
            L.pages[p].ops += "BT " + std::string(muted) + " rg /F1 8 Tf 1 0 0 1 " + num(L.M) + " 28 Tm " + reg.hex("Seismic FDTD · hat simülasyonu raporu") +
                              " Tj ET\n";
            const std::string pg = "Sayfa " + std::to_string(p + 1) + " / " + std::to_string(totalPages > 0 ? totalPages : (int)L.pages.size());
            L.pages[p].ops += "BT " + std::string(muted) + " rg /F1 8 Tf 1 0 0 1 " + num(L.M + L.CW - reg.width(pg, 8)) + " 28 Tm " + reg.hex(pg) + " Tj ET\n";
        }
        return L.pages;
    };
    const int nPages = (int)layout(0).size();
    const std::vector<Page> pages = layout(nPages);

    // ── Objects ──
    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offs(1, 0);
    auto beginObj = [&]() { offs.push_back(pdf.size()); pdf += std::to_string(offs.size() - 1) + " 0 obj\n"; return (int)offs.size() - 1; };
    auto endObj = [&]() { pdf += "\nendobj\n"; };
    auto stream = [&](const std::string& dict, const std::vector<uint8_t>& data) {
        const int id = beginObj();
        pdf += "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n";
        pdf.append((const char*)data.data(), data.size());
        pdf += "\nendstream";
        endObj();
        return id;
    };
    auto flate = [&](const std::string& s) { return zlibCompress((const uint8_t*)s.data(), s.size()); };

    // 1 catalog, 2 page tree (written at the end with their final ids)
    offs.push_back(0); offs.push_back(0);

    auto writeFont = [&](PdfFont& pf) {
        Face& fc = *pf.face;
        std::set<int> keep;
        for (auto& [g, cp] : pf.used) keep.insert(g);
        const std::vector<uint8_t> sub = subsetTrueType(fc.data, keep);
        const int file = stream("/Filter /FlateDecode /Length1 " + std::to_string(sub.size()), zlibCompress(sub.data(), sub.size()));
        const double k = 1000.0 / fc.upem;
        int x0, y0, x1, y1, asc, desc, gap;
        stbtt_GetFontBoundingBox(&fc.info, &x0, &y0, &x1, &y1);
        stbtt_GetFontVMetrics(&fc.info, &asc, &desc, &gap);
        const std::string name = std::string(pf.res == "F1" ? "SFDTDA+" : "SFDTDB+") + fc.psName;
        const int fd = beginObj();
        pdf += "<< /Type /FontDescriptor /FontName /" + name + " /Flags 32 /FontBBox [" + num(x0 * k) + " " + num(y0 * k) + " " + num(x1 * k) +
               " " + num(y1 * k) + "] /ItalicAngle 0 /Ascent " + num(asc * k) + " /Descent " + num(desc * k) + " /CapHeight " +
               num(asc * k * 0.72) + " /StemV " + (pf.res == "F1" ? "80" : "120") + " /FontFile2 " + std::to_string(file) + " 0 R >>";
        endObj();
        std::string w = "[";
        for (auto& [g, cp] : pf.used) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&fc.info, g, &adv, &lsb);
            w += std::to_string(g) + " [" + num(adv * k) + "] ";
        }
        w += "]";
        const int cid = beginObj();
        pdf += "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + name +
               " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " + std::to_string(fd) +
               " 0 R /CIDToGIDMap /Identity /DW 500 /W " + w + " >>";
        endObj();
        std::string cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                           "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::pair<int, uint32_t>> m(pf.used.begin(), pf.used.end());
        for (size_t i = 0; i < m.size(); i += 100) {
            const size_t n = std::min<size_t>(100, m.size() - i);
            cmap += std::to_string(n) + " beginbfchar\n";
            for (size_t j = i; j < i + n; j++) {
                char b[48];
                uint32_t cp = m[j].second;
                if (cp > 0xFFFF) { cp -= 0x10000; std::snprintf(b, sizeof(b), "<%04X> <%04X%04X>\n", m[j].first, 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); }
                else std::snprintf(b, sizeof(b), "<%04X> <%04X>\n", m[j].first, cp);
                cmap += b;
            }
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend";
        const int tu = stream("/Filter /FlateDecode", flate(cmap));
        const int f0 = beginObj();
        pdf += "<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts [" + std::to_string(cid) +
               " 0 R] /ToUnicode " + std::to_string(tu) + " 0 R >>";
        endObj();
        return f0;
    };
    const int fReg = writeFont(reg), fBold = writeFont(bold);
    int img = 0;
    if (doc.image && doc.image->w > 0)
        img = stream("/Type /XObject /Subtype /Image /Width " + std::to_string(doc.image->w) + " /Height " + std::to_string(doc.image->h) +
                         " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode",
                     zlibCompress(doc.image->rgb.data(), doc.image->rgb.size()));
    const std::string resources = "<< /Font << /F1 " + std::to_string(fReg) + " 0 R /F2 " + std::to_string(fBold) + " 0 R >>" +
                                  (img ? " /XObject << /Im1 " + std::to_string(img) + " 0 R >>" : std::string()) + " >>";
    std::vector<int> pageIds;
    for (const Page& p : pages) {
        const int content = stream("/Filter /FlateDecode", flate(p.ops));
        const int pg = beginObj();
        pdf += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] /Resources " + resources + " /Contents " + std::to_string(content) +
               " 0 R >>";
        endObj();
        pageIds.push_back(pg);
    }
    offs[1] = pdf.size();
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    offs[2] = pdf.size();
    pdf += "2 0 obj\n<< /Type /Pages /Count " + std::to_string(pageIds.size()) + " /Kids [";
    for (int id : pageIds) pdf += std::to_string(id) + " 0 R ";
    pdf += "] >>\nendobj\n";
    // Info dictionary (title, as UTF-16BE hex)
    std::string title = "<FEFF";
    for (uint32_t cp : utf8(doc.title)) {
        char b[8];
        std::snprintf(b, sizeof(b), "%04X", cp <= 0xFFFF ? cp : 0xFFFD);
        title += b;
    }
    title += ">";
    const int info = beginObj();
    pdf += "<< /Title " + title + " /Producer (Seismic FDTD) >>";
    endObj();

    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(offs.size()) + "\n0000000000 65535 f \n";
    for (size_t i = 1; i < offs.size(); i++) {
        char b[24];
        std::snprintf(b, sizeof(b), "%010zu 00000 n \n", offs[i]);
        pdf += b;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offs.size()) + " /Root 1 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    std::ofstream o(std::filesystem::u8path(path), std::ios::binary);
    if (!o) { err = "Dosya yazılamadı: " + path; return false; }
    o.write(pdf.data(), (std::streamsize)pdf.size());
    return (bool)o;
}

} // namespace report
