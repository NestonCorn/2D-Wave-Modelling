#pragma once
#include <imgui.h>
#include <string>
#include "core/Simulation.hpp"

namespace ui {

// ── Colours (tokens from the HTML :root) ─────────────────────────────────
inline ImU32 rgba(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f)); }
inline ImU32 hex(unsigned v, float a = 1.0f) { return rgba((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a); }
inline ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

namespace col {
inline const ImU32 bgDeep = hex(0x010408);
inline const ImU32 bgPanel = rgba(13, 17, 23, 0.7f);
inline const ImU32 borderDim = rgba(255, 255, 255, 0.08f);
inline const ImU32 blue = hex(0x0084ff);
inline const ImU32 cyan = hex(0x00f2ff);
inline const ImU32 red = hex(0xff3c5c);
inline const ImU32 orange = hex(0xff9100);
inline const ImU32 green = hex(0x00ff9d);
inline const ImU32 muted = hex(0xafbaca);
inline const ImU32 text = hex(0xe6edf3);
} // namespace col

// ── Fonts ────────────────────────────────────────────────────────────────
struct Fonts {
    ImFont* ui = nullptr;        // Segoe UI 15
    ImFont* uiBold = nullptr;    // Segoe UI Bold 15
    ImFont* label = nullptr;     // Segoe UI Bold 12 (card titles)
    ImFont* title = nullptr;     // Segoe UI Bold 20
    ImFont* mono = nullptr;      // Consolas 13
    ImFont* monoBold = nullptr;  // Consolas Bold 13
    ImFont* canvas = nullptr;    // Consolas 18 (scaled canvas text)
    ImFont* canvasBold = nullptr;
    ImFont* icon = nullptr;      // emoji 24 (header icon)
};
extern Fonts fonts;
extern float dpi;               // content scale
inline float S(float v) { return v * dpi; }

void loadFonts(float scale);
void applyTheme(float scale);

// ── Widgets (Widgets.cpp) ────────────────────────────────────────────────
// Thin-track range slider with round thumb (styled like input[type=range]).
bool RangeSlider(const char* id, float* v, float vmin, float vmax, float step, float width);
// "label  ───●───  [value]" row; the number box commits on Enter / focus loss.
bool ParamRow(const char* label, const char* id, float* v, float vmin, float vmax, float step, const char* fmt);
bool ParamRowInt(const char* label, const char* id, int* v, int vmin, int vmax, int step);
bool StyledButton(const char* label, ImU32 bg, ImU32 border, ImU32 text, ImVec2 size = ImVec2(0, 0), ImU32 hoverBg = 0);
// Flow layout: puts the next item on a new line when it would not fit (flex-wrap).
struct Flow {
    float right;
    bool first = true;
    static Flow begin() { return Flow{ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x}; }
    void next(float w) {
        if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right) ImGui::SameLine();
        first = false;
    }
};
float ButtonWidth(const char* label);
void Pill(const char* text, ImU32 bg, ImU32 border, ImU32 fg, ImFont* font);
bool BeginCard(const char* id, const char* title, ImU32 titleCol = 0);
void EndCard();
bool StyledCombo(const char* id, int* current, const char* const items[], int count, float width);

// Draw-list text helpers. align: 0 = left, 1 = centre, 2 = right; baseline = canvas fillText semantics.
void TextAt(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* text, int align = 0,
            bool baseline = true);
void DashedPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c, float thickness, float dash, float gap);
void GlowCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float glow);

// ── Scene / gathers ──────────────────────────────────────────────────────
// k = screen pixels per canvas unit (canvas units = domain metres, W x H_main)
// ky = vertical scale (>= k when a wide, shallow domain is stretched to a usable height)
void drawScene(Simulation& sim, ImDrawList* dl, ImVec2 origin, float k, float ky, int overlayState);

struct GatherView {
    float zoomX = 1, zoomY = 1, panX = 0, panY = 0;
    int dragTarget = 0;          // 0 none, 1 plot, 2 x-axis, 3 y-axis
    float startMx = 0, startMy = 0;
    bool vd = true, smooth = true;   // smooth = cubic (Catmull-Rom): VD resampling, wiggle curves
    unsigned vdTex[3] = {0, 0, 0};
    int vdW[3] = {0, 0, 0}, vdH[3] = {0, 0, 0};
    std::vector<uint32_t> vdBuf;
    std::vector<float> vdTmp, colW, rowW;   // bicubic scratch
    std::vector<int> colIdx, rowIdx;
    void reset() { zoomX = zoomY = 1; panX = panY = 0; }
};
// panel 0 = streamer gather, 1 = OBS gather. Handles pan/zoom input on the panel.
void drawGatherPanel(Simulation& sim, GatherView& gv, int panel, ImVec2 origin, float k);

} // namespace ui
