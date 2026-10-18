// Main canvas: port of drawScene / drawFDTDToScreen / drawCable / drawShip /
// drawSourceIcon / drawOBS. Coordinates are canvas units (= metres, W x H_main),
// mapped to the screen with origin + v * k.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Ui.hpp"

namespace ui {

namespace {

struct Canvas {
    ImDrawList* dl;
    ImVec2 o;
    float k;
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
};

ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
    ImVec4 x = vec(a), y = vec(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

// Fill the area below a y(x) curve down to `bottom` as vertical strips (the
// curve polygon is concave, so it cannot go through AddConvexPolyFilled).
// gradTop/gradBot: optional vertical gradient between yTop and yBot.
void fillBelow(const Canvas& c, const std::vector<Pt>& pts, float bottom, ImU32 color,
               ImU32 gradBot = 0, float yTop = 0, float yBot = 1) {
    if (pts.size() < 2) return;
    ImDrawList* dl = c.dl;
    auto colAt = [&](float y) {
        if (!gradBot) return color;
        float t = std::clamp((y - yTop) / (yBot - yTop), 0.0f, 1.0f);
        return lerpCol(color, gradBot, t);
    };
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 cb = colAt(bottom);
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        const Pt& a = pts[i];
        const Pt& b = pts[i + 1];
        float ya = std::min(a.y, bottom), yb = std::min(b.y, bottom);
        dl->PrimReserve(6, 4);
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(c.P(a.x, ya), uv, colAt(ya));
        dl->PrimWriteVtx(c.P(b.x, yb), uv, colAt(yb));
        dl->PrimWriteVtx(c.P(b.x, bottom), uv, cb);
        dl->PrimWriteVtx(c.P(a.x, bottom), uv, cb);
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 1); dl->PrimWriteIdx(base + 2);
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 2); dl->PrimWriteIdx(base + 3);
    }
}

void strokeCurve(const Canvas& c, const std::vector<Pt>& pts, ImU32 color, float width) {
    std::vector<ImVec2> v;
    v.reserve(pts.size());
    for (const Pt& p : pts) v.push_back(c.P(p.x, p.y));
    c.dl->AddPolyline(v.data(), (int)v.size(), color, 0, width);
}

void drawSourceIcon(const Canvas& c, float x, float y, ImU32 color, float rX) {
    ImVec2 ctr = c.P(x, y + 2 * rX);
    GlowCircle(c.dl, ctr, 3 * rX * c.k, color, 10 * rX * c.k * 0.6f);
    c.dl->AddCircle(ctr, 6 * rX * c.k, color, 24, std::max(1.0f, 1 * rX * c.k));
    c.dl->AddCircle(ctr, 10 * rX * c.k, color, 32, std::max(1.0f, 1 * rX * c.k));
}

void drawShip(const Canvas& c, const Simulation& sim, float x, float y, ImU32 c1, ImU32 c2, const char* label) {
    const float W = sim.p.domain_W;
    const float rX = (W / 1200.0f) * 0.75f;
    const float W_S = 70 * rX, H_S = 20 * rX;
    const float bx = x - W_S / 2, by = y - H_S;
    ImDrawList* dl = c.dl;

    // Neon glow: a few translucent, slightly larger hulls
    ImVec4 g = vec(c2);
    for (int k = 3; k >= 1; k--) {
        float e = k * 3 * rX;
        ImU32 gc = ImGui::ColorConvertFloat4ToU32(ImVec4(g.x, g.y, g.z, 0.07f));
        dl->AddQuadFilled(c.P(bx + 6 * rX - e, by - e), c.P(bx + W_S - 4 * rX + e, by - e),
                          c.P(bx + W_S + 6 * rX + e, by + H_S - 2 * rX + e), c.P(bx - e, by + H_S - 2 * rX + e), gc);
    }
    dl->AddQuadFilled(c.P(bx + 6 * rX, by), c.P(bx + W_S - 4 * rX, by), c.P(bx + W_S + 6 * rX, by + H_S - 2 * rX),
                      c.P(bx, by + H_S - 2 * rX), c1);
    dl->AddRectFilled(c.P(bx + W_S * 0.55f, by - 8 * rX), c.P(bx + W_S * 0.55f + 16 * rX, by), c2);
    dl->AddRectFilled(c.P(bx + 6 * rX, by - 4 * rX), c.P(bx + 10 * rX, by + 2 * rX), c2);   // tow mast

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f Knot | %.2fm Atis", sim.p.ship_speed_knots, sim.p.shot_interval);
    float fs = std::max(6.0f, std::round(10.5f * rX)) * c.k;
    TextAt(dl, fonts.canvasBold, fs, c.P(bx + W_S / 2, by - 26 * rX), hex(0xff8c42), buf, 1);
    float ls = std::max(6.0f, std::round(10.0f * rX)) * c.k;
    TextAt(dl, fonts.uiBold, ls, c.P(x, by - 12 * rX), hex(0xe8f4fd), label, 1);
}

void drawCable(const Canvas& c, const Simulation& sim, float toPX) {
    const SimParams& p = sim.p;
    const float W = p.domain_W, rX = W / 1200.0f;
    const float OY = (float)sim.OCEAN_Y;
    const float head_x = (float)(sim.sxShip - p.ship_src_dist - p.src_rec_dist);
    const float sxS = head_x * toPX, cable = p.cable_len * toPX;
    const float cyHead = OY + p.cable_head_depth, cyTail = OY + p.cable_tail_depth;
    const float ct = (float)sim.cycle_t;
    auto sway = [&](float xpx) { return std::sin(xpx * 0.015f + ct * 0.004f) * 0.3f * rX; };

    std::vector<ImVec2> pts;
    std::vector<float> ts;
    for (float x = sxS; x >= sxS - cable; x -= 4 * rX) {
        float norm = cable > 0 ? (x - (sxS - cable)) / cable : 0;
        float cy = cyTail + (cyHead - cyTail) * norm;
        pts.push_back(c.P(x, cy + sway(x)));
        ts.push_back(norm);
    }
    const ImU32 tailC = rgba(255, 140, 0, 0.4f), headC = hex(0xffd700);
    const float w = std::max(1.0f, 3 * rX) * c.k;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        c.dl->AddLine(pts[i], pts[i + 1], rgba(255, 215, 0, 0.12f), w * 3.0f);   // glow
        c.dl->AddLine(pts[i], pts[i + 1], lerpCol(tailC, headC, ts[i]), w);
    }
    // Receivers (every 3rd group)
    for (int i = 0; i < sim.N_REC; i += 3) {
        float rx = (sxS - cable) + i * (sim.N_REC > 1 ? cable / (sim.N_REC - 1) : 0);
        float norm = sim.N_REC > 1 ? (float)i / (sim.N_REC - 1) : 0;
        float cy = cyTail + (cyHead - cyTail) * norm;
        ImVec2 q = c.P(rx, cy + sway(rx));
        c.dl->AddCircleFilled(q, 5 * rX * c.k, rgba(0, 212, 255, 0.15f), 12);
        c.dl->AddCircleFilled(q, 2 * rX * c.k, IM_COL32_WHITE, 12);
    }
    // Tow lines
    const ImU32 lc = rgba(255, 255, 255, 0.4f);
    ImVec2 a[2] = {c.P((float)sim.sxShip * toPX, OY), c.P(sxS, cyHead)};
    DashedPolyline(c.dl, a, 2, lc, std::max(1.0f, rX * c.k), 4 * rX * c.k, 4 * rX * c.k);
    if (sim.lastMode != Mode::Trad) {
        ImVec2 b[2] = {c.P((float)sim.sxSrc * toPX, OY), c.P((float)(sim.sxSrc - p.ship_src_dist) * toPX, OY)};
        DashedPolyline(c.dl, b, 2, lc, std::max(1.0f, rX * c.k), 4 * rX * c.k, 4 * rX * c.k);
    }
}

void drawOBS(const Canvas& c, float x, float y, float rX) {
    const ImU32 gold = hex(0xfbbf24);
    for (int k = 2; k >= 1; k--) {
        float e = k * 2.5f * rX;
        c.dl->AddTriangleFilled(c.P(x, y - e), c.P(x - 5 * rX - e, y + 8 * rX + e * 0.5f),
                                c.P(x + 5 * rX + e, y + 8 * rX + e * 0.5f), rgba(251, 191, 36, 0.12f));
    }
    c.dl->AddTriangleFilled(c.P(x, y), c.P(x - 5 * rX, y + 8 * rX), c.P(x + 5 * rX, y + 8 * rX), gold);
}

} // namespace

void drawScene(Simulation& sim, ImDrawList* dl, ImVec2 o, float k, int overlay) {
    const SimParams& p = sim.p;
    const float W = p.domain_W, H = p.domain_H;
    const float rX = W / 1200.0f;
    const float OY = (float)sim.OCEAN_Y;
    Canvas c{dl, o, k};
    dl->PushClipRect(c.P(0, 0), c.P(W, H), true);

    // Sky → water
    dl->AddRectFilledMultiColor(c.P(0, 0), c.P(W, OY), hex(0x030914), hex(0x030914), hex(0x041022), hex(0x041022));
    dl->AddRectFilled(c.P(0, OY), c.P(W, H), hex(0x061225));
    dl->AddLine(c.P(0, OY), c.P(W, OY), rgba(30, 144, 255, 0.4f), 2 * rX * k);

    // Layers (painter's algorithm)
    fillBelow(c, sim.layout.seafloor, H, hex(0x102235));
    strokeCurve(c, sim.layout.seafloor, hex(0x274b7c), 2 * rX * k);
    fillBelow(c, sim.layout.deep, H, hex(0x0a1628));
    strokeCurve(c, sim.layout.deep, hex(0x1e3048), 2 * rX * k);
    if (!sim.layout.salt.empty()) {
        fillBelow(c, sim.layout.salt, H, hex(0xbdcdde), hex(0x48627e), H * 0.4f, H);
        strokeCurve(c, sim.layout.salt, hex(0xe8f4fd), 3 * rX * k);
    }

    // Wavefield texture (GPU writes it directly; CPU path uploads the dirty box)
    if (!p.sismo_only && sim.waveTex)
        dl->AddImage((ImTextureID)(intptr_t)sim.waveTex, c.P(0, 0), c.P(W, H));

    // Expanding box outline
    EikBox box;
    if (p.expanding_box && !sim.shots.empty() && sim.drawBox(box) && box.hasRows) {
        const int sj_surf = sim.sjSurf();
        const int minJ = p.free_surface ? std::max(sj_surf, box.min_j) : box.min_j;
        const int maxJ = box.max_j;
        if (maxJ > minJ) {
            const float sx = W / sim.W_sim, sy = H / sim.H_sim;
            std::vector<ImVec2> v;
            v.reserve((size_t)(maxJ - minJ + 1) * 2 + 3);
            v.push_back(c.P(box.xMinZ[minJ] * sx, minJ * sy));
            for (int j = minJ + 1; j <= maxJ; j++) v.push_back(c.P(box.xMinZ[j] * sx, j * sy));
            v.push_back(c.P(box.xMaxZ[maxJ] * sx, maxJ * sy));
            for (int j = maxJ - 1; j >= minJ; j--) v.push_back(c.P(box.xMaxZ[j] * sx, j * sy));
            v.push_back(c.P(box.xMinZ[minJ] * sx, minJ * sy));
            DashedPolyline(dl, v.data(), (int)v.size(), rgba(255, 255, 0, 0.5f), 2 * rX * k, 5 * k, 5 * k);
        }
    }

    // Overlay text (eye toggle: 0 = all, 1 = none, 2 = time only)
    char buf[128];
    if (overlay == 0 || overlay == 2) {
        const float xr = W - 16 * rX;
        if (overlay == 0) {
            std::snprintf(buf, sizeof(buf), "Staggered Grid: %d x %d | \xCE\x94h = %gm", sim.W_sim, sim.H_sim, sim.SCALE);
            TextAt(dl, fonts.canvas, std::round(12 * rX) * k, c.P(xr, 26 * rX), rgba(255, 255, 255, 0.85f), buf, 2);
            std::snprintf(buf, sizeof(buf), "Toplam Hücre: %lld", (long long)sim.W_sim * sim.H_sim);
            TextAt(dl, fonts.canvas, std::round(12 * rX) * k, c.P(xr, 44 * rX), rgba(255, 255, 255, 0.85f), buf, 2);
        }
        double t_ms = sim.cycle_t * sim.dtPhy() * 1000.0;
        std::snprintf(buf, sizeof(buf), "T: %.1f ms", t_ms);
        TextAt(dl, fonts.canvasBold, std::round(16 * rX) * k, c.P(xr, (overlay == 2 ? 26 : 68) * rX), hex(0x00e5a0), buf, 2);
        if (overlay == 0) {
            std::snprintf(buf, sizeof(buf), "ANA GEMI ATIS: %d", sim.shotsMain);
            TextAt(dl, fonts.canvas, std::round(11 * rX) * k, c.P(xr, 88 * rX), hex(0x1e90ff), buf, 2);
            std::snprintf(buf, sizeof(buf), "KAYNAK ATIS: %d", sim.shotsSrc);
            TextAt(dl, fonts.canvas, std::round(11 * rX) * k, c.P(xr, 104 * rX), hex(0xff4d6d), buf, 2);
        }
    }

    // Metres → canvas units
    const float X_max = (float)(sim.W_sim * sim.SCALE);
    const float toPX = W / X_max;

    drawCable(c, sim, toPX);
    drawShip(c, sim, (float)sim.sxShip * toPX, OY, hex(0x1e90ff), hex(0x00d4ff), "ANA GEMİ");
    drawSourceIcon(c, (float)(sim.sxShip - p.ship_src_dist) * toPX, OY, hex(0x1e90ff), rX);
    if (sim.lastMode != Mode::Trad) {
        drawShip(c, sim, (float)sim.sxSrc * toPX, OY, hex(0xff4d6d), hex(0xff8c42), "2. GEMİ");
        drawSourceIcon(c, (float)(sim.sxSrc - p.ship_src_dist) * toPX, OY, hex(0xff4d6d), rX);
    }

    if (p.obs_enabled) {
        for (int i = 0; i < sim.N_OBS; i++) {
            double rx = (i + 1) * (double)p.obs_spacing;
            double ry = obsSeafloorDepth(p.geology, rx, W, H);
            drawOBS(c, (float)rx * toPX, (float)ry, rX);
        }
    }
    dl->PopClipRect();
}

} // namespace ui
