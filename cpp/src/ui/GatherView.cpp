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
        // Variable density: one texel per (trace, sample), positive = trace colour, negative = white
        const int w = count, h = max_y + 1;
        gv.vdBuf.assign((size_t)w * h, 0u);
        ImVec4 cf = vec(color);
        const uint32_t cr = (uint32_t)(cf.x * 255), cg = (uint32_t)(cf.y * 255), cb = (uint32_t)(cf.z * 255);
        for (int i = 0; i < count; i++) {
            const float* tr = data + (size_t)i * GH;
            for (int y = 0; y <= max_y; y++) {
                float val = tr[y] * gain * 1.5f;
                uint32_t r, g, b; float a;
                if (val > 0) { val = std::min(1.0f, val); r = cr; g = cg; b = cb; a = val * 255.0f; }
                else { val = std::min(1.0f, -val); r = g = b = 255; a = val * 150.0f; }
                if (a > 0) gv.vdBuf[(size_t)y * w + i] = r | (g << 8) | (b << 16) | ((uint32_t)std::lround(a) << 24);
            }
        }
        ensureTexture(gv, slot, w, h, gv.smooth);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gv.vdBuf.data());
        float x = baseX + gv.panX, ww = displayW * gv.zoomX;
        float y = MARGIN_TOP + gv.panY, hh = ((max_y + 1) / (float)GH) * DRAW_H * gv.zoomY;
        dl->AddImage((ImTextureID)(intptr_t)gv.vdTex[slot], pn.P(x, y), pn.P(x + ww, y + hh));
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
