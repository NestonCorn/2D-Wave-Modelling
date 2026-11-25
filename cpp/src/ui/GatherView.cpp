// Sismogram panels: port of clearGather / drawGatherLabels / drawSismogram /
// renderGeneralWiggle plus the pan/zoom mouse handling (setupSismoEvents).
// Coordinates are logical panel pixels (LW x LH: the panel size / content scale), mapped with origin + v * k.
#include <glad/glad.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "Ui.hpp"

namespace ui {

namespace {

// Panel size in logical pixels (screen pixels / content scale), set per panel by drawGatherPanel
float LW = 598.0f, LH = 550.0f;
constexpr float MARGIN_TOP = 26.0f;     // offset axis
constexpr float MARGIN_LEFT = 50.0f;    // time axis
float DRAW_H = 524.0f;                  // the whole record at zoom 1
float PLOT_W = 536.0f;

// canvas 'lighter' composite → additive blending for the trace layer
void setAdditive(const ImDrawList*, const ImDrawCmd*) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); }
struct Panel {
    ImDrawList* dl;
    ImVec2 o;
    float k;
    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
};

void setNormal(const ImDrawList*, const ImDrawCmd*) {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

// Catmull-Rom cubic convolution kernel (Keys, a = -0.5); x = distance to the tap
float cubicW(float x) {
    x = std::fabs(x);
    if (x < 1) return (1.5f * x - 2.5f) * x * x + 1.0f;
    if (x < 2) return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    return 0.0f;
}

// Gray colorbar: +1 → black, 0 → mid gray, -1 → white (opaque RGBA8)
uint32_t grayRGBA(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    const uint32_t g = (uint32_t)std::lround(127.5f - v * 127.5f);
    return g | (g << 8) | (g << 16) | 0xFF000000u;
}

// Vertical gray colorbar in the top-right corner of the plot area (VD mode only)
void drawColorbar(const Panel& pn, float clip) {
    ImDrawList* dl = pn.dl;
    // Scales with the panel: a short panel keeps its data visible
    const float x1 = LW - 26.0f, x0 = x1 - 10.0f, y0 = MARGIN_TOP + 22.0f, y1 = y0 + std::clamp(DRAW_H * 0.35f, 44.0f, 110.0f);
    dl->AddRectFilled(pn.P(x0 - 48, y0 - 18), pn.P(x1 + 4, y1 + 8), withAlpha(col::hull, 0.88f), 4 * pn.k);
    dl->AddRectFilledMultiColor(pn.P(x0, y0), pn.P(x1, (y0 + y1) / 2), hex(0x000000), hex(0x000000), hex(0x808080),
                                hex(0x808080));
    dl->AddRectFilledMultiColor(pn.P(x0, (y0 + y1) / 2), pn.P(x1, y1), hex(0x808080), hex(0x808080), hex(0xffffff),
                                hex(0xffffff));
    dl->AddRect(pn.P(x0, y0), pn.P(x1, y1), rgba(255, 255, 255, 0.5f), 0, 0, 1.0f);
    char buf[32];
    const ImU32 tc = rgba(255, 255, 255, 0.9f);
    TextAt(dl, fonts.canvas, 10 * pn.k, pn.P((x0 + x1) / 2 - 18, y0 - 6), tc, "Genlik", 1);
    std::snprintf(buf, sizeof(buf), "+%.2g", clip);
    TextAt(dl, fonts.canvas, 9 * pn.k, pn.P(x0 - 4, y0 + 3), tc, buf, 2);
    TextAt(dl, fonts.canvas, 9 * pn.k, pn.P(x0 - 4, (y0 + y1) / 2 + 3), tc, "0", 2);
    std::snprintf(buf, sizeof(buf), "-%.2g", clip);
    TextAt(dl, fonts.canvas, 9 * pn.k, pn.P(x0 - 4, y1 + 3), tc, buf, 2);
}

float totalOffsetMax(const Simulation& sim) {
    const SimParams& p = sim.p;
    return (sim.lastMode == Mode::None || sim.lastMode == Mode::Trad) ? (p.src_rec_dist + p.cable_len)
                                                                      : (p.src_rec_dist + p.src_offset + p.cable_len);
}


void ensureTexture(GatherView& gv, int slot, int w, int h, bool smooth) {
    if (!gv.vdTex[slot]) glGenTextures(1, &gv.vdTex[slot]);
    glBindTexture(GL_TEXTURE_2D, gv.vdTex[slot]);
    if (gv.vdW[slot] != w || gv.vdH[slot] != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gv.vdW[slot] = w; gv.vdH[slot] = h;
    }
    GLint f = smooth ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void renderTraces(const Panel& pn, Simulation& sim, GatherView& gv, int slot, size_t offset, int count, int max_y,
                  float baseX, float displayW, ImU32 color) {
    if (max_y <= 0 || count <= 0) return;
    constexpr size_t S = kSismoStride;   // trace stride; sample g is at time g * KS steps
    if (offset + (size_t)(count - 1) * S + max_y >= sim.sismo.size()) return;
    const float gain = sim.p.gain;
    const float* data = sim.sismo.data() + offset;
    // panel pixels per sample (zoom 1: the whole record spans DRAW_H)
    const float pxPerSample = DRAW_H * sim.sismoStepsPerSample() / (float)std::max(1.0, sim.sismoSteps());
    ImDrawList* dl = pn.dl;

    if (gv.vd) {
        // Variable density, gray colormap (positive = black, zero = mid gray, negative = white).
        // The amplitude is interpolated first and mapped to gray afterwards.
        const int w = count, h = max_y + 1;
        const float scale = gain * 1.5f;
        const float X0 = baseX + gv.panX, ww = displayW * gv.zoomX;
        const float Y0 = MARGIN_TOP + gv.panY, hh = h * pxPerSample * gv.zoomY;
        dl->AddCallback(setNormal, nullptr);   // opaque image: no additive blending
        if (gv.smooth) {
            // Bicubic (Catmull-Rom) resampling at screen resolution over the visible part only
            const float vx0 = std::max(X0, MARGIN_LEFT), vx1 = std::min(X0 + ww, LW - 12.0f);
            const float vy0 = std::max(Y0, MARGIN_TOP), vy1 = std::min(Y0 + hh, (float)LH);
            if (vx1 <= vx0 || vy1 <= vy0) { dl->AddCallback(setAdditive, nullptr); return; }
            const int ow = std::clamp((int)std::ceil((vx1 - vx0) * pn.k), 1, 4096);
            const int oh = std::clamp((int)std::ceil((vy1 - vy0) * pn.k), 1, 4096);

            // Per-column / per-row source taps
            auto taps = [](int n, int count, float start, float len, float src0, float srcLen, std::vector<int>& idx,
                           std::vector<float>& wt) {
                idx.resize((size_t)n * 4); wt.resize((size_t)n * 4);
                for (int o = 0; o < n; o++) {
                    const float pos = start + (o + 0.5f) * len / n;                 // panel coords
                    const float u = (pos - src0) / srcLen * count - 0.5f;         // texel-centre coords
                    const float fu = std::floor(u), t = u - fu;
                    const float wts[4] = {cubicW(t + 1), cubicW(t), cubicW(1 - t), cubicW(2 - t)};
                    for (int k = 0; k < 4; k++) {
                        idx[(size_t)o * 4 + k] = std::clamp((int)fu - 1 + k, 0, count - 1);
                        wt[(size_t)o * 4 + k] = wts[k];
                    }
                }
            };
            taps(ow, w, vx0, vx1 - vx0, X0, ww, gv.colIdx, gv.colW);
            // Rows: cubic when zoomed in; when an output row covers more than one sample, the
            // mean of the samples it covers (point sampling would alias the wavelet again).
            const bool boxRows = h / (float)oh * ((vy1 - vy0) / hh) > 1.0f;
            if (boxRows) {
                gv.rowIdx.resize((size_t)oh * 2);
                for (int o = 0; o < oh; o++) {
                    const float u0 = (vy0 + o * (vy1 - vy0) / oh - Y0) / hh * h;
                    const float u1 = (vy0 + (o + 1) * (vy1 - vy0) / oh - Y0) / hh * h;
                    const int a = std::clamp((int)std::floor(u0), 0, h - 1);
                    gv.rowIdx[(size_t)o * 2] = a;
                    gv.rowIdx[(size_t)o * 2 + 1] = std::clamp((int)std::ceil(u1), a + 1, h);
                }
            } else {
                taps(oh, h, vy0, vy1 - vy0, Y0, hh, gv.rowIdx, gv.rowW);
            }
            const int t0 = gv.colIdx.front(), t1 = gv.colIdx.back();   // traces actually referenced
            const int nt = t1 - t0 + 1;

            gv.vdTmp.resize((size_t)nt);
            gv.vdBuf.resize((size_t)ow * oh);
            for (int oy = 0; oy < oh; oy++) {
                if (boxRows) {
                    const int a = gv.rowIdx[(size_t)oy * 2], b = gv.rowIdx[(size_t)oy * 2 + 1];
                    const float inv = 1.0f / (float)(b - a);
                    for (int i = 0; i < nt; i++) {   // vertical pass: mean
                        const float* tr = data + (size_t)(t0 + i) * S;
                        float acc = 0.0f;
                        for (int y = a; y < b; y++) acc += tr[y];
                        gv.vdTmp[i] = acc * inv;
                    }
                } else {
                    const int* ri = &gv.rowIdx[(size_t)oy * 4];
                    const float* rw = &gv.rowW[(size_t)oy * 4];
                    for (int i = 0; i < nt; i++) {   // vertical pass: cubic
                        const float* tr = data + (size_t)(t0 + i) * S;
                        gv.vdTmp[i] = rw[0] * tr[ri[0]] + rw[1] * tr[ri[1]] + rw[2] * tr[ri[2]] + rw[3] * tr[ri[3]];
                    }
                }
                uint32_t* out = &gv.vdBuf[(size_t)oy * ow];
                for (int ox = 0; ox < ow; ox++) {   // horizontal pass + colormap
                    const int* ci = &gv.colIdx[(size_t)ox * 4];
                    const float* cw = &gv.colW[(size_t)ox * 4];
                    const float v = cw[0] * gv.vdTmp[ci[0] - t0] + cw[1] * gv.vdTmp[ci[1] - t0] +
                                    cw[2] * gv.vdTmp[ci[2] - t0] + cw[3] * gv.vdTmp[ci[3] - t0];
                    out[ox] = grayRGBA(v * scale);
                }
            }
            ensureTexture(gv, slot, ow, oh, false);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, gv.vdBuf.data());
            dl->AddImage((ImTextureID)(intptr_t)gv.vdTex[slot], pn.P(vx0, vy0), pn.P(vx1, vy1));
        } else {
            // One texel per (trace, sample), nearest-neighbour
            gv.vdBuf.resize((size_t)w * h);
            for (int i = 0; i < count; i++) {
                const float* tr = data + (size_t)i * S;
                for (int y = 0; y < h; y++) gv.vdBuf[(size_t)y * w + i] = grayRGBA(tr[y] * scale);
            }
            ensureTexture(gv, slot, w, h, false);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gv.vdBuf.data());
            dl->AddImage((ImTextureID)(intptr_t)gv.vdTex[slot], pn.P(X0, Y0), pn.P(X0 + ww, Y0 + hh));
        }
        dl->AddCallback(setAdditive, nullptr);
        return;
    }

    // Wiggle traces
    const float scaledW = displayW * gv.zoomX;
    const float trSpacing = scaledW / std::max(1, count);
    int step = 1;
    if (trSpacing < 2.0f) step = (int)std::ceil(2.0f / trSpacing);
    const float eff = trSpacing * step;
    const float maxDx = std::min(eff * 1.5f, 30.0f);
    const float yScale = pxPerSample * gv.zoomY;   // panel pixels per sample
    const float y0 = MARGIN_TOP + gv.panY;

    // Cubic interpolation: when the samples are more than about 1.5 screen pixels apart, every
    // interval is drawn with `sub` points on the cubic curve through the samples.
    const int sub = gv.smooth ? std::clamp((int)std::ceil(yScale * pn.k / 1.5f), 1, 32) : 1;
    const float qScale = yScale / sub;
    // Only the points inside the visible time range (one sample of margin on both sides).
    const int qBeg = std::clamp(((int)std::floor((MARGIN_TOP - y0) / yScale) - 1) * sub, 0, max_y * sub);
    const int qEnd = std::clamp(((int)std::ceil((LH - y0) / yScale) + 1) * sub, 0, max_y * sub);
    if (qEnd <= qBeg) return;

    const ImDrawListFlags oldFlags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // thousands of tiny quads: no AA fringe
    std::vector<ImVec2> line;
    line.reserve(max_y + 3);
    const ImU32 strokeCol = eff < 2.0f ? (color & 0x00FFFFFF) | ((ImU32)(0.7f * 255) << 24) : rgba(232, 244, 253, 0.6f);
    const float strokeW = (eff < 2.0f ? 0.4f : 0.6f) * pn.k;

    // Zoomed out, several samples share a screen pixel. Drawing every one would be hundreds of
    // thousands of quads, and drawing every dec-th would alias the wavelet - each trace would
    // show a different part of it. Instead each group of `dec` samples (about one device pixel)
    // is drawn as its min / max: the band-limited trace's true extent at that pixel.
    const int dec = yScale * pn.k < 0.5f ? (int)std::floor(1.0f / (yScale * pn.k)) : 1;
    auto drawEnvelope = [&](const float* tr, float gx) {
        auto dxOf = [&](float v) { return std::clamp(v * gain * 2.5f, -maxDx, maxDx); };
        line.clear();
        if (qBeg == 0) line.push_back(pn.P(gx, y0));
        for (int g = qBeg; g <= qEnd; g += dec) {
            const int e = std::min(g + dec, qEnd + 1);
            int imn = g, imx = g;
            for (int y = g + 1; y < e; y++) {
                if (tr[y] < tr[imn]) imn = y;
                if (tr[y] > tr[imx]) imx = y;
            }
            const float mn = dxOf(tr[imn]), mx = dxOf(tr[imx]);
            const float ya = y0 + g * yScale, yb = y0 + e * yScale, ym = 0.5f * (ya + yb);
            if (eff >= 1.5f) {
                if (mx > 0) dl->AddRectFilled(pn.P(gx, ya), pn.P(gx + mx, yb), color);
                if (mn < 0) dl->AddRectFilled(pn.P(gx + mn, ya), pn.P(gx, yb), color);
            }
            const bool minFirst = imn <= imx;
            line.push_back(pn.P(gx + (minFirst ? mn : mx), ym));
            if (mn != mx) line.push_back(pn.P(gx + (minFirst ? mx : mn), ym));
        }
        if (line.size() > 1) dl->AddPolyline(line.data(), (int)line.size(), strokeCol, 0, std::max(strokeW, 0.5f));
    };

    for (int i = 0; i < count; i += step) {
        const float gx = baseX + gv.panX + i * trSpacing;
        if (gx < MARGIN_LEFT - maxDx || gx > LW - 12 + maxDx) continue;
        const float* tr = data + (size_t)i * S;
        if (dec > 1) {   // several samples per screen pixel: min / max of each group
            drawEnvelope(tr, gx);
            continue;
        }
        // Point q of the trace: sample q / sub, cubic (Catmull-Rom) between the recorded samples.
        // Samples after max_y are not recorded yet, so the last one is repeated there.
        auto dxAt = [&](int q) {
            const int y = q / sub, f = q % sub;
            float v = tr[y];
            if (f) {
                const float t = f / (float)sub;
                const float a = tr[std::max(y - 1, 0)], b = tr[y], c = tr[std::min(y + 1, max_y)], d = tr[std::min(y + 2, max_y)];
                v = cubicW(t + 1) * a + cubicW(t) * b + cubicW(1 - t) * c + cubicW(2 - t) * d;
            }
            return std::clamp(v * gain * 2.5f, -maxDx, maxDx);
        };

        if (eff >= 1.5f) {
            // Area between the trace and its baseline (canvas nonzero fill of the trace path)
            float pdx = dxAt(qBeg), py = y0 + qBeg * qScale;
            for (int y = qBeg + 1; y <= qEnd; y++) {
                float dx = dxAt(y), yy = y0 + y * qScale;
                if (pdx == 0 && dx == 0) { pdx = dx; py = yy; continue; }
                if ((pdx >= 0) == (dx >= 0)) {
                    dl->AddQuadFilled(pn.P(gx, py), pn.P(gx + pdx, py), pn.P(gx + dx, yy), pn.P(gx, yy), color);
                } else {
                    float yc = py + (yy - py) * (pdx / (pdx - dx));
                    dl->AddTriangleFilled(pn.P(gx, py), pn.P(gx + pdx, py), pn.P(gx, yc), color);
                    dl->AddTriangleFilled(pn.P(gx, yc), pn.P(gx + dx, yy), pn.P(gx, yy), color);
                }
                pdx = dx; py = yy;
            }
        }

        // Outline with point decimation (same criteria as the HTML)
        line.clear();
        if (qBeg == 0) line.push_back(pn.P(gx, y0));
        float lastDx = 0, lastY = -999;
        for (int y = qBeg; y <= qEnd; y++) {
            float dx = dxAt(y), yy = y0 + y * qScale;
            if (y == qBeg || y == qEnd || std::fabs(yy - lastY) >= 1.0f || std::fabs(dx - lastDx) > 0.5f) {
                line.push_back(pn.P(gx + dx, yy));
                lastDx = dx; lastY = yy;
            }
        }
        if (line.size() > 1) dl->AddPolyline(line.data(), (int)line.size(), strokeCol, 0, std::max(strokeW, 0.5f));
    }
    dl->Flags = oldFlags;
}

void handleInput(GatherView& gv, ImVec2 o, float k) {
    ImGuiIO& io = ImGui::GetIO();
    const float mx = (io.MousePos.x - o.x) / k, my = (io.MousePos.y - o.y) / k;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        if (my <= MARGIN_TOP) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (mx <= MARGIN_LEFT) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        else ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemActivated()) {
        gv.dragTarget = my <= MARGIN_TOP ? 2 : (mx <= MARGIN_LEFT ? 3 : 1);
        gv.startMx = mx; gv.startMy = my;
    }
    if (ImGui::IsItemActive()) {
        const float dx = io.MouseDelta.x / k, dy = io.MouseDelta.y / k;
        if (gv.dragTarget == 1) {
            gv.panX += dx; gv.panY += dy;
        } else if (gv.dragTarget == 2) {
            float z = 1.0f + dx * 0.005f;
            if (z > 0.05f) { gv.panX = gv.startMx - (gv.startMx - gv.panX) * z; gv.zoomX *= z; }
        } else if (gv.dragTarget == 3) {
            float z = 1.0f - dy * 0.005f;
            if (z > 0.05f) { gv.panY = gv.startMy - (gv.startMy - gv.panY) * z; gv.zoomY *= z; }
        }
    } else if (ImGui::IsItemDeactivated()) {
        // Only the panel being released clears the drag: both panels share one GatherView,
        // and clearing on "not active" let the idle panel cancel every drag after one frame.
        gv.dragTarget = 0;
    }
    if (hovered && io.MouseWheel != 0 && (my <= MARGIN_TOP || mx <= MARGIN_LEFT)) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);   // don't scroll the page while zooming an axis
        float z = io.MouseWheel > 0 ? 1.1f : 0.90909f;
        if (my <= MARGIN_TOP) { gv.panX = mx - (mx - gv.panX) * z; gv.zoomX *= z; }
        else { gv.panY = my - (my - gv.panY) * z; gv.zoomY *= z; }
    }
}

} // namespace

void drawGatherPanel(Simulation& sim, GatherView& gv, int panel, ImVec2 o, ImVec2 size) {
    const bool isOBS = panel == 1;
    const SimParams& p = sim.p;
    const float k = S(1.0f);
    LW = std::max(120.0f, size.x / k);
    LH = std::max(80.0f, size.y / k);
    DRAW_H = LH - MARGIN_TOP;
    PLOT_W = LW - MARGIN_LEFT - 12.0f;
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton(isOBS ? "##obsGather" : "##gather", ImVec2(LW * k, LH * k));
    handleInput(gv, o, k);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    Panel pn{dl, o, k};
    const float maxMs = (float)p.sismo_ms;
    dl->PushClipRect(pn.P(0, 0), pn.P(LW, LH), true);

    // ── Background + grid (clearGather) ──
    dl->AddRectFilled(pn.P(0, 0), pn.P(LW, LH), hex(0x0B1620));
    dl->PushClipRect(pn.P(MARGIN_LEFT, MARGIN_TOP), pn.P(LW - 12, LH), true);
    // Axis steps: the smallest "nice" step whose labels do not touch at the current zoom
    auto niceStep = [](float pxPerUnit, float minPx) {
        static const float steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 20000};
        for (float st : steps) if (st * pxPerUnit >= minPx) return st;
        return 50000.0f;
    };
    const float pxPerMs = DRAW_H * gv.zoomY / maxMs;
    const float tStep = niceStep(pxPerMs, 28.0f);
    const float tMinor = tStep / 5.0f * pxPerMs >= 7.0f ? tStep / 5.0f : 0.0f;
    const float msLo = std::max(0.0f, -gv.panY / pxPerMs), msHi = std::min(maxMs, (LH - MARGIN_TOP - gv.panY) / pxPerMs);
    auto yOf = [&](float ms) { return MARGIN_TOP + gv.panY + ms * pxPerMs; };
    if (tMinor > 0)
        for (float ms = std::floor(msLo / tMinor) * tMinor; ms <= msHi; ms += tMinor)
            dl->AddLine(pn.P(MARGIN_LEFT, yOf(ms)), pn.P(LW, yOf(ms)), rgba(201, 214, 226, 0.035f), 1.0f);
    for (float ms = std::floor(msLo / tStep) * tStep; ms <= msHi; ms += tStep)
        dl->AddLine(pn.P(MARGIN_LEFT, yOf(ms)), pn.P(LW, yOf(ms)), rgba(201, 214, 226, 0.11f), 1.0f);

    // Horizontal axis: streamer = distance from the source (decreasing to the right), OBS = position
    const float axisTotal = isOBS ? (float)(sim.W_sim * sim.SCALE) : totalOffsetMax(sim);
    const float pxPerM = PLOT_W * gv.zoomX / std::max(1.0f, axisTotal);
    const float dStep = niceStep(pxPerM, 64.0f);
    auto xOf = [&](float d) {
        return isOBS ? MARGIN_LEFT + gv.panX + (d / axisTotal) * PLOT_W * gv.zoomX
                     : MARGIN_LEFT + gv.panX + (1.0f - d / axisTotal) * PLOT_W * gv.zoomX;
    };
    for (float d = 0; d <= axisTotal + 0.01f; d += dStep) {
        const float x = xOf(d);
        if (x < MARGIN_LEFT - 2 || x > LW) continue;
        dl->AddLine(pn.P(x, MARGIN_TOP), pn.P(x, LH), rgba(201, 214, 226, 0.06f), 1.0f);
    }

    // ── Traces (drawSismogram, additive) ──
    const double T_ticks = sim.sismoSteps();
    const int nS = sim.sismoSamples();   // last sample recorded so far
    const int yProgress = sim.cycle_t >= T_ticks ? nS - 1 : std::min(nS - 1, (int)std::floor(sim.cycle_t / sim.sismoStepsPerSample()));
    dl->AddCallback(setAdditive, nullptr);
    if (!isOBS) {
        const float total = totalOffsetMax(sim);
        const float xr1 = MARGIN_LEFT + (1.0f - p.src_rec_dist / total) * PLOT_W;
        const float xl1 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.cable_len) / total) * PLOT_W;
        renderTraces(pn, sim, gv, 0, 0, sim.N_REC, yProgress, xl1, xr1 - xl1, hex(0xD9E2EA));
        if (sim.lastMode == Mode::CLO) {
            const float xr2 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.src_offset) / total) * PLOT_W;
            const float xl2 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.src_offset + p.cable_len) / total) * PLOT_W;
            renderTraces(pn, sim, gv, 1, (size_t)sim.N_REC * kSismoStride, sim.N_REC, yProgress, xl2, xr2 - xl2, col::data);
        }
    } else if (p.obs_enabled) {
        renderTraces(pn, sim, gv, 2, (size_t)sim.N_REC * 2 * kSismoStride, sim.N_OBS, yProgress, MARGIN_LEFT, PLOT_W, col::warn);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();
    if (gv.vd && (!isOBS || p.obs_enabled)) drawColorbar(pn, 1.0f / std::max(1e-6f, p.gain * 1.5f));

    // ── Labels (drawGatherLabels) ──
    char buf[64];
    for (float ms = std::floor(msLo / tStep) * tStep; ms <= msHi; ms += tStep) {
        const float y = yOf(ms);
        if (y < MARGIN_TOP - 2 || y > LH - 6) continue;
        std::snprintf(buf, sizeof(buf), "%g ms", ms);
        TextAt(dl, fonts.canvas, 11.5f * k, pn.P(MARGIN_LEFT - 6, std::max(y + 4, MARGIN_TOP + 10)), ms == 0 ? col::muted : col::text, buf, 2);
    }
    for (float d = 0; d <= axisTotal + 0.01f; d += dStep) {
        const float x = xOf(d);
        if (x < MARGIN_LEFT + 10 || x > LW - 24) continue;
        std::snprintf(buf, sizeof(buf), "%g m", d);
        TextAt(dl, fonts.canvas, 11.5f * k, pn.P(x, 18), col::muted, buf, 1);
    }
    dl->PopClipRect();
    dl->AddLine(pn.P(MARGIN_LEFT, MARGIN_TOP), pn.P(LW - 12, MARGIN_TOP), col::bulkhead);
    dl->AddLine(pn.P(MARGIN_LEFT, MARGIN_TOP), pn.P(MARGIN_LEFT, LH), col::bulkhead);
}

} // namespace ui
