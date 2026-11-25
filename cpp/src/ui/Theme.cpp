#include "Ui.hpp"

namespace ui {

// Ocean-instrument theme: flat panels separated by hairlines, small radii on controls only,
// one accent (streamer yellow) for what is active or primary.
void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 14;
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 5;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    // Tooltips: short delay on the first one, then instant while moving between items
    s.HoverDelayShort = 0.25f;
    s.HoverDelayNormal = 0.45f;
    s.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = vec(col::text);
    c[ImGuiCol_TextDisabled] = vec(col::muted);
    c[ImGuiCol_WindowBg] = vec(col::abyss);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = vec(col::hull);
    c[ImGuiCol_Border] = vec(col::bulkhead);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = vec(col::deck);
    c[ImGuiCol_FrameBgHovered] = vec(hex(0x203547));
    c[ImGuiCol_FrameBgActive] = vec(hex(0x243B4F));
    c[ImGuiCol_TitleBg] = vec(col::hull);
    c[ImGuiCol_TitleBgActive] = vec(col::hull);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(col::bulkhead);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(col::rail);
    c[ImGuiCol_ScrollbarGrabActive] = vec(col::muted);
    c[ImGuiCol_CheckMark] = vec(col::accent);
    c[ImGuiCol_SliderGrab] = vec(col::accent);
    c[ImGuiCol_SliderGrabActive] = vec(col::accent);
    c[ImGuiCol_Button] = vec(col::deck);
    c[ImGuiCol_ButtonHovered] = vec(hex(0x223A4E));
    c[ImGuiCol_ButtonActive] = vec(hex(0x1A2D3E));
    c[ImGuiCol_Header] = vec(withAlpha(col::accent, 0.14f));
    c[ImGuiCol_HeaderHovered] = vec(hex(0x223A4E));
    c[ImGuiCol_HeaderActive] = vec(withAlpha(col::accent, 0.22f));
    c[ImGuiCol_Separator] = vec(col::bulkhead);
    c[ImGuiCol_SeparatorHovered] = vec(col::rail);
    c[ImGuiCol_SeparatorActive] = vec(col::accent);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = vec(col::deck);
    c[ImGuiCol_TabSelected] = vec(col::deck);
    c[ImGuiCol_TabSelectedOverline] = vec(col::accent);
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = vec(col::deck);
    c[ImGuiCol_PlotHistogram] = vec(col::accent);
    c[ImGuiCol_TextSelectedBg] = vec(withAlpha(col::accent, 0.30f));
    c[ImGuiCol_NavCursor] = vec(col::accent);
    c[ImGuiCol_ModalWindowDimBg] = vec(rgba(6, 12, 18, 0.62f));
    c[ImGuiCol_TableBorderLight] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0, 0, 0, 0);
}

} // namespace ui
