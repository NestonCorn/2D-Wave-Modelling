#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "Ui.hpp"
#include <imgui_internal.h>   // FindRenderedTextEnd, item flags

namespace ui {

// ── Motion ───────────────────────────────────────────────────────────────
// Animated value per id that eases towards its target: an exponential approach, so a new
// target mid-way retargets smoothly (interruptible, like a CSS transition) and the curve is
// an ease-out (fast start, gentle settle). `ms` ~ time to cover ~98 % of the distance.
float Animate(ImGuiID id, float target, float ms) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(id, target);
    const float dt = ImGui::GetIO().DeltaTime;
    const float k = 1.0f - std::exp(-dt * 4.0f / std::max(1e-3f, ms / 1000.0f));
    *v += (target - *v) * k;
    if (std::fabs(target - *v) < 1e-3f) *v = target;
    return *v;
}

static ImU32 U(ImU32 c) { return ImGui::GetColorU32(c); }   // applies the disabled alpha

// ── Range slider: thin track, filled to the value, round thumb ───────────
bool RangeSlider(const char* id, float* v, float vmin, float vmax, float step, float width) {
    ImGuiIO& io = ImGui::GetIO();
    const float h = S(18);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float r = S(6.0f);
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
    const float t = vmax > vmin ? std::clamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
    const ImVec2 c(x0 + t * (x1 - x0), cy);
    dl->AddRectFilled(ImVec2(pos.x, cy - S(1.5f)), ImVec2(pos.x + width, cy + S(1.5f)), U(col::bulkhead), S(2));
    dl->AddRectFilled(ImVec2(pos.x, cy - S(1.5f)), ImVec2(c.x, cy + S(1.5f)), U(withAlpha(col::accent, 0.85f)), S(2));
    if (hovered || active) dl->AddCircleFilled(c, r + S(4), U(withAlpha(col::accent, active ? 0.28f : 0.16f)), 24);
    dl->AddCircleFilled(c, r, U(col::textHi), 24);
    dl->AddCircle(c, r, U(withAlpha(col::accent, 0.9f)), 24, S(1.5f));
    return changed;
}

static bool numberBox(const char* id, float* v, float vmin, float vmax, const char* fmt, float w) {
    ImGui::SetNextItemWidth(w);
    ImGui::PushFont(fonts.mono);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(col::deck));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, vec(hex(0x203547)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(3)));
    float tmp = *v;
    ImGui::InputFloat(id, &tmp, 0, 0, fmt);
    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + S(2), b.y - 1), ImVec2(b.x - S(2), b.y - 1), U(col::accent), S(1.5f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    if (commit) {
        // Clamped to the slider range (out-of-range values could break the grid setup)
        tmp = std::clamp(tmp, vmin, vmax);
        if (tmp != *v) { *v = tmp; return true; }
    }
    return false;
}

// Label and value on one line, slider under them (fits a narrow side panel)
bool ParamRow(const char* label, const char* id, float* v, float vmin, float vmax, float step, const char* fmt) {
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float inputW = S(74);
    const ImVec2 p = ImGui::GetCursorPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(p.x + avail - inputW, p.y));
    bool ch = numberBox("##n", v, vmin, vmax, fmt, inputW);
    ImGui::SetCursorPos(ImVec2(p.x, ImGui::GetCursorPosY() - S(4)));
    ch |= RangeSlider("##s", v, vmin, vmax, step, avail);
    ImGui::Dummy(ImVec2(0, S(6)));
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

// Label left, read-only value right
void ReadoutRow(const char* label, const char* value, ImU32 c) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PushFont(fonts.mono);
    const float w = ImGui::CalcTextSize(value).x;
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(p.x + avail - w - S(6), p.y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, vec(c ? c : col::textHi));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Hint(const char* text, ImU32 c) {
    ImGui::PushStyleColor(ImGuiCol_Text, vec(c ? c : col::muted));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(6)));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(320));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

// ── Buttons ──────────────────────────────────────────────────────────────
ImVec2 ToolButtonSize(const char* label, ImFont* f) {
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label, ImGui::FindRenderedTextEnd(label));
    return ImVec2(ts.x + S(32), std::max(S(32), ts.y + S(14)));
}

bool ToolButton(const char* label, Btn kind, ImVec2 size, const char* tip) {
    ImFont* f = fonts.uiBold;
    const ImVec2 auto_ = ToolButtonSize(label, f);
    if (size.x <= 0) size.x = auto_.x;
    if (size.y <= 0) size.y = auto_.y;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    if (tip) Tip(tip);
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (hov && !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImU32 bg = 0, br = 0, fg = col::text;
    switch (kind) {
        case Btn::Primary:
            bg = hov ? hex(0xF6CF52) : col::accent; br = bg; fg = col::accentInk; break;
        case Btn::Danger:
            bg = withAlpha(col::danger, hov ? 0.26f : 0.16f); br = withAlpha(col::danger, 0.55f); fg = hex(0xF3A19C); break;
        case Btn::Ghost:
            bg = hov ? col::deck : 0; br = 0; fg = hov ? col::textHi : col::text; break;
        default:
            bg = hov ? hex(0x223A4E) : col::deck; br = hov ? col::rail : col::bulkhead; fg = hov ? col::textHi : col::text; break;
    }
    // Pressed: the face sinks one pixel on every side (the 0.97 press scale of a web button)
    const float in = held ? S(1.0f) : 0.0f;
    ImVec2 a(p0.x + in, p0.y + in), b(p0.x + size.x - in, p0.y + size.y - in);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (bg) dl->AddRectFilled(a, b, U(bg), S(4));
    if (br) dl->AddRect(a, b, U(br), S(4));
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, label, end);
    dl->AddText(f, f->FontSize, ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)), U(fg), label, end);
    return pressed;
}

float ButtonWidth(const char* label) { return ToolButtonSize(label, fonts.uiBold).x; }

// Older call sites: colour arguments are mapped onto the button kinds
bool StyledButton(const char* label, ImU32 bg, ImU32 border, ImU32 text, ImVec2 size, ImU32 hoverBg) {
    (void)border; (void)text; (void)hoverBg;
    const ImVec4 c = vec(bg);
    Btn kind = Btn::Secondary;
    if (c.w > 0.3f && c.x > 0.5f && c.y < 0.35f) kind = Btn::Danger;
    return ToolButton(label, kind, size);
}

// ── Switch: label left, track right; the knob slides over 120 ms ─────────
bool Switch(const char* label, bool* v, const char* tip) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##sw", ImVec2(avail, h));
    ImGui::PopID();
    if (tip) Tip(tip);
    if (pressed) *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(p0.x, p0.y + (h - ts.y) * 0.5f), U(hov ? col::textHi : col::text), label, end);
    const float tw = S(32), th = S(18);
    const ImVec2 t0(p0.x + avail - tw - S(2), p0.y + (h - th) * 0.5f), t1(t0.x + tw, t0.y + th);
    const float k = Animate(ImGui::GetItemID(), *v ? 1.0f : 0.0f, 120.0f);
    const ImVec4 off = vec(col::bulkhead), on = vec(col::accent);
    const ImU32 track = ImGui::ColorConvertFloat4ToU32(ImVec4(off.x + (on.x - off.x) * k, off.y + (on.y - off.y) * k,
                                                              off.z + (on.z - off.z) * k, 1.0f));
    dl->AddRectFilled(t0, t1, U(track), th * 0.5f);
    const float r = th * 0.5f - S(3);
    const ImVec2 kc(t0.x + th * 0.5f + k * (tw - th), (t0.y + t1.y) * 0.5f);
    dl->AddCircleFilled(kc, r, U(*v ? col::accentInk : col::text), 20);
    return pressed;
}

// ── Segmented control: the selection slides to the clicked segment ──────
bool Segmented(const char* id, const char* const items[], int n, int* cur, float width, const char* const tips[], float height) {
    ImFont* f = fonts.uiBold;
    float w = width;
    if (w <= 0) {
        w = 0;
        for (int i = 0; i < n; i++) w += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, items[i]).x + S(28);
    }
    const float h = height > 0 ? height : S(32), segW = w / n;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), U(col::deck), S(4));
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), U(col::bulkhead), S(4));
    ImGui::PushID(id);
    const float x = Animate(ImGui::GetID("##sel"), (float)std::max(0, *cur), 180.0f);
    bool changed = false;
    if (*cur >= 0) {
        const ImVec2 a(p0.x + x * segW + S(2), p0.y + S(2)), b(a.x + segW - S(4), p0.y + h - S(2));
        dl->AddRectFilled(a, b, U(col::accent), S(3));
    }
    for (int i = 0; i < n; i++) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + i * segW, p0.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h)) && *cur != i) { *cur = i; changed = true; }
        ImGui::PopID();
        if (tips && tips[i]) Tip(tips[i]);
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool sel = i == *cur && std::fabs(x - i) < 0.5f;
        const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, items[i]);
        dl->AddText(f, f->FontSize, ImVec2(std::floor(p0.x + i * segW + (segW - ts.x) * 0.5f), std::floor(p0.y + (h - ts.y) * 0.5f)),
                    U(sel ? col::accentInk : hov ? col::textHi : col::text), items[i]);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));
    return changed;
}

// ── Collapsible panel section ────────────────────────────────────────────
bool Section(const char* title, bool defaultOpen) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(title);
    bool open = st->GetBool(id, defaultOpen);
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = S(34);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p0.x - S(16), p0.y), ImVec2(p0.x + avail + S(16), p0.y), U(col::bulkhead));
    ImGui::PushID(title);
    if (ImGui::InvisibleButton("##sec", ImVec2(avail, h))) { open = !open; st->SetBool(id, open); }
    ImGui::PopID();
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImU32 c = U(hov ? col::textHi : col::text);
    dl->AddText(fonts.icon, S(11), ImVec2(p0.x, p0.y + (h - S(11)) * 0.5f), U(col::muted), open ? icon::ChevronDown : icon::ChevronRight);
    const ImVec2 ts = fonts.uiBold->CalcTextSizeA(fonts.uiBold->FontSize, FLT_MAX, 0, title);
    dl->AddText(fonts.uiBold, fonts.uiBold->FontSize, ImVec2(p0.x + S(20), p0.y + (h - ts.y) * 0.5f), c, title);
    if (open) ImGui::Dummy(ImVec2(0, S(2)));
    return open;
}

void Pill(const char* text, ImU32 bg, ImU32 border, ImU32 fg, ImFont* font) {
    ImGui::PushFont(font);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(8), S(2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), U(bg), S(4));
    if (border) dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), U(border), S(4));
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), U(fg), text);
    ImGui::Dummy(sz);
    ImGui::PopFont();
}

bool BeginCard(const char* id, const char* title, ImU32 titleCol) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    bool open = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (title) {
        ImGui::PushFont(fonts.uiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(titleCol ? titleCol : col::muted));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool StyledCombo(const char* id, int* current, const char* const items[], int count, float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(col::deck));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(col::bulkhead));
    ImGui::PushStyleColor(ImGuiCol_Button, vec(col::deck));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool r = ImGui::Combo(id, current, items, count);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// Label left, combo right-aligned at `width`
bool ComboRow(const char* label, const char* id, int* current, const char* const items[], int count, float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(p.x + avail - width, p.y));
    return StyledCombo(id, current, items, count, width);
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
