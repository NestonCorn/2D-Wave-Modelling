#pragma once
#include <imgui.h>
#include <string>
#include "core/Simulation.hpp"

namespace ui {

// ── Colours: the "ocean instrument" palette ─────────────────────────────
inline ImU32 rgba(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f)); }
inline ImU32 hex(unsigned v, float a = 1.0f) { return rgba((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a); }
inline ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImU32 withAlpha(ImU32 c, float a) { return (c & 0x00FFFFFFu) | ((ImU32)(a * 255.0f + 0.5f) << 24); }

namespace col {
inline const ImU32 abyss = hex(0x0F1B26);      // application background
inline const ImU32 hull = hex(0x152635);       // panels, bars
inline const ImU32 deck = hex(0x1A2D3E);       // raised surfaces: fields, segments
inline const ImU32 bulkhead = hex(0x22384A);   // borders, separators, idle controls
inline const ImU32 rail = hex(0x2E4A60);       // hovered controls, stronger borders
inline const ImU32 text = hex(0xC9D6E2);
inline const ImU32 textHi = hex(0xEEF3F7);     // values, headings
inline const ImU32 muted = hex(0x7F95A8);      // secondary text, units
inline const ImU32 accent = hex(0xF2C230);     // streamer yellow: the one accent (primary action, active mode, selection)
inline const ImU32 accentInk = hex(0x1C1606);  // text on the accent
inline const ImU32 danger = hex(0xE0574F);     // recording, errors, instability
inline const ImU32 ok = hex(0x4CB782);         // healthy status
inline const ImU32 warn = hex(0xE8A54B);       // marginal status
inline const ImU32 data = hex(0x5AB8D6);       // data overlays (second vessel, OBS axis)
// Older names still used by the scene / gather drawing
inline const ImU32 bgDeep = abyss;
inline const ImU32 bgPanel = hull;
inline const ImU32 borderDim = bulkhead;
inline const ImU32 blue = data;
inline const ImU32 cyan = data;
inline const ImU32 red = danger;
inline const ImU32 orange = warn;
inline const ImU32 green = ok;
} // namespace col

// Segoe Fluent Icons / MDL2 glyphs (merged into the text fonts)
namespace icon {
inline const char* const Play = "\uE768";
inline const char* const Pause = "\uE769";
inline const char* const Stop = "\uE71A";
inline const char* const Record = "\uE7C8";
inline const char* const Folder = "\uE8B7";
inline const char* const Open = "\uE8E5";
inline const char* const Save = "\uE74E";
inline const char* const Download = "\uE896";
inline const char* const Upload = "\uE898";
inline const char* const ChevronDown = "\uE70D";
inline const char* const ChevronRight = "\uE76C";
inline const char* const Info = "\uE946";
inline const char* const Warning = "\uE7BA";
inline const char* const Error = "\uEA39";
inline const char* const Check = "\uE73E";
inline const char* const Close = "\uE711";
inline const char* const Refresh = "\uE72C";
inline const char* const View = "\uE890";
inline const char* const Delete = "\uE74D";
} // namespace icon

// ── Fonts ────────────────────────────────────────────────────────────────
struct Fonts {
    ImFont* ui = nullptr;        // Segoe UI 14.5: body text, field labels
    ImFont* uiBold = nullptr;    // Segoe UI Semibold 14.5: buttons, emphasis
    ImFont* label = nullptr;     // Segoe UI Semibold 12.5: section headings in panels, small captions
    ImFont* title = nullptr;     // Bahnschrift 17: application name, pane titles
    ImFont* mono = nullptr;      // Bahnschrift 13.5: numbers (values, readouts, status bar)
    ImFont* monoBold = nullptr;  // Bahnschrift 13.5
    ImFont* canvas = nullptr;    // Bahnschrift 18: text drawn on the scene / gathers (scaled)
    ImFont* canvasBold = nullptr;
    ImFont* icon = nullptr;      // icon font 16 on its own
};
extern Fonts fonts;
extern float dpi;               // content scale
inline float S(float v) { return v * dpi; }

void loadFonts(float scale);
void applyTheme(float scale);

// ── Widgets (Widgets.cpp) ────────────────────────────────────────────────
// Value that eases towards `target` (exponential approach: interruptible ease-out)
float Animate(ImGuiID id, float target, float ms);
// Thin track filled to the value, round thumb.
bool RangeSlider(const char* id, float* v, float vmin, float vmax, float step, float width);
// Label and editable value on one line, slider below; the number box commits on Enter / focus loss.
bool ParamRow(const char* label, const char* id, float* v, float vmin, float vmax, float step, const char* fmt);
bool ParamRowInt(const char* label, const char* id, int* v, int vmin, int vmax, int step);
void ReadoutRow(const char* label, const char* value, ImU32 c = 0);   // label left, read-only value right
bool ComboRow(const char* label, const char* id, int* current, const char* const items[], int count, float width);
void Hint(const char* text, ImU32 c = 0);                              // wrapped secondary text
void Tip(const char* text);                                            // tooltip for the last item (shared delay)

enum class Btn { Secondary, Primary, Danger, Ghost };
ImVec2 ToolButtonSize(const char* label, ImFont* f);
bool ToolButton(const char* label, Btn kind = Btn::Secondary, ImVec2 size = ImVec2(0, 0), const char* tip = nullptr);
bool StyledButton(const char* label, ImU32 bg, ImU32 border, ImU32 text, ImVec2 size = ImVec2(0, 0), ImU32 hoverBg = 0);
bool Switch(const char* label, bool* v, const char* tip = nullptr);   // full-width row; toggles *v, true when clicked
bool Segmented(const char* id, const char* const items[], int n, int* cur, float width = 0, const char* const tips[] = nullptr);
bool Section(const char* title, bool defaultOpen = true);             // collapsible panel section; true when open

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
void drawGatherPanel(Simulation& sim, GatherView& gv, int panel, ImVec2 origin, ImVec2 size);

} // namespace ui
