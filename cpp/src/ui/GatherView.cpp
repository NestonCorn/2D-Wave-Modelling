// Sismogram panels: port of clearGather / drawGatherLabels / drawSismogram /
// renderGeneralWiggle plus the pan/zoom mouse handling (setupSismoEvents).
// Coordinates are logical panel pixels (GW x GH), mapped with origin + v * k.
#include <glad/glad.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "Ui.hpp"

namespace ui {

namespace {

constexpr float MARGIN_TOP = 40.0f;
constexpr float MARGIN_LEFT = 45.0f;
constexpr float DRAW_H = GH - MARGIN_TOP;
constexpr float PLOT_W = GW - MARGIN_LEFT - 20.0f;

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
    const float x1 = GW - 26.0f, x0 = x1 - 10.0f, y0 = MARGIN_TOP + 22.0f, y1 = y0 + 110.0f;
    dl->AddRectFilled(pn.P(x0 - 48, y0 - 18), pn.P(x1 + 4, y1 + 8), rgba(1, 4, 8, 0.75f), 4 * pn.k);
    dl->AddRectFilledMultiColor(pn.P(x0, y0), pn.P(x1, (y0 + y1) / 2), hex(0x000000), hex(0x000000), hex(0x808080),
                                hex(0x808080));
    dl->AddRectFilledMultiColor(pn.P(x0, (y0 + y1) / 2), pn.P(x1, y1), hex(0x808080), hex(0x808080), hex(0xffffff),
                                hex(0xffffff));
    dl->AddRect(pn.P(x0, y0), pn.P(x1, y1), rgba(255, 255, 255, 0.5f), 0, 0, 1.0f);
    char buf[32];
    const ImU32 tc = rgba(255, 255, 255, 0.9f);
    TextAt(dl, fonts.canvasBold, 9 * pn.k, pn.P((x0 + x1) / 2 - 18, y0 - 6), tc, "AMP", 1);
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

void textShadow(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* t, int align) {
    TextAt(dl, f, size, ImVec2(pos.x + 1, pos.y + 1), rgba(0, 0, 0, 0.8f), t, align);
    TextAt(dl, f, size, pos, c, t, align);
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
    if (offset + (size_t)(count - 1) * GH + max_y >= sim.sismo.size()) return;
    const float gain = sim.p.gain;
    const float* data = sim.sismo.data() + offset;
    ImDrawList* dl = pn.dl;

    if (gv.vd) {
        // Variable density, gray colormap (positive = black, zero = mid gray, negative = white).
        // The amplitude is interpolated first and mapped to gray afterwards.
        const int w = count, h = max_y + 1;
        const float scale = gain * 1.5f;
        const float X0 = baseX + gv.panX, ww = displayW * gv.zoomX;
        const float Y0 = MARGIN_TOP + gv.panY, hh = (h / (float)GH) * DRAW_H * gv.zoomY;
        dl->AddCallback(setNormal, nullptr);   // opaque image: no additive blending
        if (gv.smooth) {
            // Bicubic (Catmull-Rom) resampling at screen resolution over the visible part only
            const float vx0 = std::max(X0, MARGIN_LEFT), vx1 = std::min(X0 + ww, GW - 20.0f);
            const float vy0 = std::max(Y0, MARGIN_TOP), vy1 = std::min(Y0 + hh, (float)GH);
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
            taps(oh, h, vy0, vy1 - vy0, Y0, hh, gv.rowIdx, gv.rowW);
            const int t0 = gv.colIdx.front(), t1 = gv.colIdx.back();   // traces actually referenced
            const int nt = t1 - t0 + 1;

            gv.vdTmp.resize((size_t)nt);
            gv.vdBuf.resize((size_t)ow * oh);
            for (int oy = 0; oy < oh; oy++) {
                const int* ri = &gv.rowIdx[(size_t)oy * 4];
                const float* rw = &gv.rowW[(size_t)oy * 4];
                for (int i = 0; i < nt; i++) {   // vertical pass
                    const float* tr = data + (size_t)(t0 + i) * GH;
                    gv.vdTmp[i] = rw[0] * tr[ri[0]] + rw[1] * tr[ri[1]] + rw[2] * tr[ri[2]] + rw[3] * tr[ri[3]];
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
                const float* tr = data + (size_t)i * GH;
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
    const float yScale = DRAW_H * gv.zoomY / GH;
    const float y0 = MARGIN_TOP + gv.panY;

    const ImDrawListFlags oldFlags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // thousands of tiny quads: no AA fringe
    std::vector<ImVec2> line;
    line.reserve(max_y + 3);
    const ImU32 strokeCol = eff < 2.0f ? (color & 0x00FFFFFF) | ((ImU32)(0.7f * 255) << 24) : rgba(232, 244, 253, 0.6f);
    const float strokeW = (eff < 2.0f ? 0.4f : 0.6f) * pn.k;

    for (int i = 0; i < count; i += step) {
        const float gx = baseX + gv.panX + i * trSpacing;
        if (gx < MARGIN_LEFT - maxDx || gx > GW - 20 + maxDx) continue;
        const float* tr = data + (size_t)i * GH;
        auto dxAt = [&](int y) { return std::clamp(tr[y] * gain * 2.5f, -maxDx, maxDx); };

        if (eff >= 1.5f) {
            // Area between the trace and its baseline (canvas nonzero fill of the trace path)
            float pdx = dxAt(0), py = y0;
            for (int y = 1; y <= max_y; y++) {
                float dx = dxAt(y), yy = y0 + y * yScale;
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
        line.push_back(pn.P(gx, y0));
        float lastDx = 0, lastY = -999;
        for (int y = 0; y <= max_y; y++) {
            float dx = dxAt(y), yy = y0 + y * yScale;
            if (y == 0 || y == max_y || std::fabs(yy - lastY) >= 1.0f || std::fabs(dx - lastDx) > 0.5f) {
                line.push_back(pn.P(gx + dx, yy));
                lastDx = dx; lastY = yy;
            }
        }
        dl->AddPolyline(line.data(), (int)line.size(), strokeCol, 0, std::max(strokeW, 0.5f));
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
    } else {
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

void drawGatherPanel(Simulation& sim, GatherView& gv, int panel, ImVec2 o, float k) {
    const bool isOBS = panel == 1;
    const SimParams& p = sim.p;
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton(isOBS ? "##obsGather" : "##gather", ImVec2(GW * k, GH * k));
    handleInput(gv, o, k);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    Panel pn{dl, o, k};
    const float maxMs = (float)p.sismo_ms;
    dl->PushClipRect(pn.P(0, 0), pn.P(GW, GH), true);

    // ── Background + grid (clearGather) ──
    dl->AddRectFilledMultiColor(pn.P(0, 0), pn.P(GW, GH), hex(0x020812), hex(0x020812), hex(0x000105), hex(0x000105));
    dl->PushClipRect(pn.P(MARGIN_LEFT, MARGIN_TOP), pn.P(GW - 20, GH), true);
    for (int ms = 0; ms <= p.sismo_ms; ms += 50) {
        float y = MARGIN_TOP + gv.panY + (ms / maxMs) * DRAW_H * gv.zoomY;
        if (y < MARGIN_TOP - 10 || y > GH + 10) continue;
        bool major = ms % 500 == 0, secondary = ms % 100 == 0;
        ImU32 c = major ? rgba(255, 255, 255, 0.15f) : secondary ? rgba(255, 255, 255, 0.08f) : rgba(255, 255, 255, 0.03f);
        float w = major ? 1.2f : secondary ? 0.8f : 0.5f;
        dl->AddLine(pn.P(0, y), pn.P(GW, y), c, std::max(1.0f, w * k));
    }
    if (!isOBS) {
        const float total = totalOffsetMax(sim);
        const float step = total > 1200 ? 500.f : (total > 600 ? 200.f : 100.f);
        for (float d = 0; d <= total; d += step) {
            float x = MARGIN_LEFT + gv.panX + (1.0f - d / total) * PLOT_W * gv.zoomX;
            dl->AddLine(pn.P(x, MARGIN_TOP), pn.P(x, GH), rgba(255, 255, 255, 0.06f), 1.0f);
        }
    } else {
        const float maxDist = (float)(sim.W_sim * sim.SCALE);
        for (float d = 0; d <= maxDist; d += 200) {
            float x = MARGIN_LEFT + gv.panX + (d / maxDist) * PLOT_W * gv.zoomX;
            dl->AddLine(pn.P(x, MARGIN_TOP), pn.P(x, GH), rgba(255, 255, 255, 0.05f), 1.0f);
        }
    }

    // ── Traces (drawSismogram, additive) ──
    const double T_ticks = std::floor((p.sismo_ms * 6.0) / (sim.SCALE * p.dt));
    const int yProgress = sim.cycle_t >= T_ticks ? GH - 1 : (int)std::floor((sim.cycle_t / T_ticks) * GH);
    dl->AddCallback(setAdditive, nullptr);
    if (!isOBS) {
        const float total = totalOffsetMax(sim);
        const float xr1 = MARGIN_LEFT + (1.0f - p.src_rec_dist / total) * PLOT_W;
        const float xl1 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.cable_len) / total) * PLOT_W;
        renderTraces(pn, sim, gv, 0, 0, sim.N_REC, yProgress, xl1, xr1 - xl1, hex(0xff1744));
        if (sim.lastMode == Mode::CLO) {
            const float xr2 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.src_offset) / total) * PLOT_W;
            const float xl2 = MARGIN_LEFT + (1.0f - (p.src_rec_dist + p.src_offset + p.cable_len) / total) * PLOT_W;
            renderTraces(pn, sim, gv, 1, (size_t)sim.N_REC * GH, sim.N_REC, yProgress, xl2, xr2 - xl2, hex(0x1e90ff));
        }
    } else if (p.obs_enabled) {
        renderTraces(pn, sim, gv, 2, (size_t)sim.N_REC * 2 * GH, sim.N_OBS, yProgress, MARGIN_LEFT, PLOT_W, hex(0xfbbf24));
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();
    if (gv.vd && (!isOBS || p.obs_enabled)) drawColorbar(pn, 1.0f / std::max(1e-6f, p.gain * 1.5f));

    // ── Labels (drawGatherLabels) ──
    char buf[64];
    for (int ms = 0; ms <= p.sismo_ms; ms += 50) {
        float y = MARGIN_TOP + gv.panY + (ms / maxMs) * DRAW_H * gv.zoomY;
        if (y < MARGIN_TOP - 5 || y > GH + 10) continue;
        bool major = ms % 500 == 0, secondary = ms % 100 == 0;
        bool isLast = std::abs(ms - p.sismo_ms) < 1;
        if (major || (secondary && ms < 1000) || isLast) {
            std::snprintf(buf, sizeof(buf), "%dms", ms);
            textShadow(dl, (major || isLast) ? fonts.canvasBold : fonts.canvas, 10 * k, pn.P(8, isLast ? y - 2 : y - 4),
                       (major || isLast) ? rgba(255, 255, 255, 0.95f) : rgba(255, 255, 255, 0.7f), buf, 0);
        }
    }
    if (!isOBS) {
        const float total = totalOffsetMax(sim);
        textShadow(dl, fonts.canvasBold, 9 * k, pn.P(MARGIN_LEFT + PLOT_W / 2, 12), rgba(255, 255, 255, 0.9f),
                   "UNIFIED OFFSET GATHER (METERS)", 1);
        const float step = total > 1200 ? 500.f : (total > 600 ? 200.f : 100.f);
        for (float d = 0; d <= total; d += step) {
            float x = MARGIN_LEFT + gv.panX + (1.0f - d / total) * PLOT_W * gv.zoomX;
            if (x < MARGIN_LEFT - 20 || x > GW) continue;
            std::snprintf(buf, sizeof(buf), "%dm", (int)std::lround(d));
            textShadow(dl, fonts.canvasBold, 10 * k, pn.P(x, 30), rgba(138, 180, 212, 0.95f), buf, 1);
        }
    } else {
        const float maxDist = (float)(sim.W_sim * sim.SCALE);
        for (float d = 0; d <= maxDist; d += 200) {
            float x = MARGIN_LEFT + gv.panX + (d / maxDist) * PLOT_W * gv.zoomX;
            if (x < MARGIN_LEFT - 20 || x > GW) continue;
            std::snprintf(buf, sizeof(buf), "%gm", d);
            if (x < GW - 20) textShadow(dl, fonts.canvasBold, 10 * k, pn.P(x, 30), rgba(251, 191, 36, 0.95f), buf, 1);
        }
    }
    std::snprintf(buf, sizeof(buf), "FS:%dHz", (int)std::lround(1000.0 / p.dt));
    textShadow(dl, fonts.canvasBold, 10 * k, pn.P(GW - 60, GH - 25), rgba(0, 229, 160, 0.95f), buf, 0);
    std::snprintf(buf, sizeof(buf), "DX:%gm", sim.SCALE);
    textShadow(dl, fonts.canvasBold, 10 * k, pn.P(GW - 60, GH - 12), rgba(0, 229, 160, 0.95f), buf, 0);

    // Pane label (bottom-right badge in the HTML)
    const char* lbl = isOBS ? "OBS SİSMOGRAM (GATHER)" : "STREAMER SİSMOGRAM (GATHER)";
    ImVec2 ts = fonts.label->CalcTextSizeA(10 * k, FLT_MAX, 0, lbl);
    ImVec2 b1 = pn.P(GW - 15, GH - 15), b0(b1.x - ts.x - 20 * k, b1.y - ts.y - 8 * k);
    b0.y -= 26 * k; b1.y -= 26 * k;   // keep clear of the FS/DX annotation
    dl->AddRectFilled(b0, b1, rgba(1, 4, 8, 0.85f), 6 * k);
    dl->AddRect(b0, b1, rgba(30, 144, 255, 0.25f), 6 * k);
    dl->AddText(fonts.label, 10 * k, ImVec2(b0.x + 10 * k, b0.y + 4 * k), hex(0x528bff), lbl);

    dl->PopClipRect();
    dl->AddRect(pn.P(0, 0), pn.P(GW, GH), hex(0x0d2040), 12 * k, 0, 1.0f);
}

} // namespace ui
