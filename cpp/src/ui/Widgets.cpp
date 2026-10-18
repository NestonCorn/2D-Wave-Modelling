#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "Ui.hpp"

namespace ui {

bool RangeSlider(const char* id, float* v, float vmin, float vmax, float step, float width) {
    ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float r = S(8.0f);
    ImGui::InvisibleButton(id, ImVec2(std::max(width, 2 * r + 4), h));
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    bool changed = false;
    const float x0 = pos.x + r, x1 = pos.x + width - r;
    if (active && x1 > x0) {
        float t = std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        float nv = vmin + t * (vmax - vmin);
        if (step > 0) nv = vmin + std::round((nv - vmin) / step) * step;
        nv = std::clamp(nv, vmin, vmax);
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + h * 0.5f;
    dl->AddRectFilled(ImVec2(pos.x, cy - S(2)), ImVec2(pos.x + width, cy + S(2)), rgba(255, 255, 255, 0.1f), S(2));
    float t = vmax > vmin ? std::clamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
    ImVec2 c(x0 + t * (x1 - x0), cy);
    float rr = active ? r * 1.2f : r;
    ImU32 thumb = active ? col::cyan : IM_COL32_WHITE;
    dl->AddCircleFilled(c, rr + S(4), rgba(0, 242, 255, active ? 0.25f : 0.12f), 24);
    dl->AddCircleFilled(c, rr, thumb, 24);
    return changed;
}

static bool numberBox(const char* id, float* v, float vmin, float vmax, const char* fmt, float w) {
    ImGui::SetNextItemWidth(w);
    ImGui::PushFont(fonts.mono);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(col::cyan));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    float tmp = *v;
    ImGui::InputFloat(id, &tmp, 0, 0, fmt);
    bool commit = ImGui::IsItemDeactivatedAfterEdit();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImU32 line = ImGui::IsItemActive() ? col::cyan : col::borderDim;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), line, 1.0f);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    if (commit) {
        // Clamped to the slider range (the browser accepted anything, which could crash the grid)
        tmp = std::clamp(tmp, vmin, vmax);
        if (tmp != *v) { *v = tmp; return true; }
    }
    return false;
}

bool ParamRow(const char* label, const char* id, float* v, float vmin, float vmax, float step, const char* fmt) {
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float inputW = S(64), labelW = std::min(avail * 0.44f, S(190));
    const float sliderW = std::max(S(40), avail - labelW - inputW - 2 * sp);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelW + sp);
    bool ch = RangeSlider("##s", v, vmin, vmax, step, sliderW);
    ImGui::SameLine();
    ch |= numberBox("##n", v, vmin, vmax, fmt, inputW);
    ImGui::PopID();
    return ch;
}

bool ParamRowInt(const char* label, const char* id, int* v, int vmin, int vmax, int step) {
    float f = (float)*v;
    if (ParamRow(label, id, &f, (float)vmin, (float)vmax, (float)step, "%.0f")) {
        int nv = (int)std::lround(f);
        if (nv != *v) { *v = nv; return true; }
    }
    return false;
}

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

bool StyledButton(const char* label, ImU32 bg, ImU32 border, ImU32 text, ImVec2 size, ImU32 hoverBg) {
    ImVec4 b = vec(bg);
    ImVec4 h = hoverBg ? vec(hoverBg) : ImVec4(std::min(1.f, b.x * 1.25f + 0.04f), std::min(1.f, b.y * 1.25f + 0.04f),
                                                std::min(1.f, b.z * 1.25f + 0.04f), std::min(1.f, b.w + 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Button, b);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, h);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, h);
    ImGui::PushStyleColor(ImGuiCol_Border, vec(border));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(text));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(16), S(9)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushFont(fonts.uiBold);
    bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return r;
}

void Pill(const char* text, ImU32 bg, ImU32 border, ImU32 fg, ImFont* font) {
    ImGui::PushFont(font);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(10), S(3));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, S(5));
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), border, S(5));
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), fg, text);
    ImGui::Dummy(sz);
    ImGui::PopFont();
}

bool BeginCard(const char* id, const char* title, ImU32 titleCol) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(rgba(0, 0, 0, 0.2f)));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(col::borderDim));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
    bool open = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                                        ImGuiChildFlags_AlwaysUseWindowPadding);
    if (title) {
        ImGui::PushFont(fonts.label);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(titleCol ? titleCol : col::muted));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

bool StyledCombo(const char* id, int* current, const char* const items[], int count, float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::PushFont(fonts.mono);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(col::cyan));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(hex(0x0d1117)));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(col::borderDim));
    bool r = ImGui::Combo(id, current, items, count);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    return r;
}

void TextAt(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const char* text, int align, bool baseline) {
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (align == 1) pos.x -= ts.x * 0.5f;
    else if (align == 2) pos.x -= ts.x;
    if (baseline) pos.y -= size * 0.8f;
    dl->AddText(f, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), c, text);
}

void DashedPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 c, float th, float dash, float gap) {
    float carry = 0;   // distance consumed within the current dash/gap cycle
    bool on = true;
    for (int i = 0; i + 1 < n; i++) {
        ImVec2 a = pts[i], b = pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) continue;
        float t = 0;
        while (t < len) {
            float seg = (on ? dash : gap) - carry;
            float e = std::min(len, t + seg);
            if (on) dl->AddLine(ImVec2(a.x + dx * t / len, a.y + dy * t / len), ImVec2(a.x + dx * e / len, a.y + dy * e / len), c, th);
            if (t + seg <= len) { on = !on; carry = 0; }
            else carry += e - t;
            t = e;
        }
    }
}

void GlowCircle(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float glow) {
    ImVec4 v = vec(color);
    for (int k = 3; k >= 1; k--) {
        float rr = r + glow * k / 3.0f;
        dl->AddCircleFilled(c, rr, ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * 0.12f)), 20);
    }
    dl->AddCircleFilled(c, r, color, 20);
}

} // namespace ui
