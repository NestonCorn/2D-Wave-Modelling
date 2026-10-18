#include "Ui.hpp"

namespace ui {

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowRounding = 0;
    s.ChildRounding = 12;
    s.FrameRounding = 8;
    s.PopupRounding = 10;
    s.GrabRounding = 8;
    s.TabRounding = 0;
    s.ScrollbarSize = 12;
    s.ScrollbarRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = vec(col::text);
    c[ImGuiCol_TextDisabled] = vec(col::muted);
    c[ImGuiCol_WindowBg] = vec(col::bgDeep);
    c[ImGuiCol_ChildBg] = vec(rgba(0, 0, 0, 0.2f));
    c[ImGuiCol_PopupBg] = vec(rgba(13, 17, 23, 0.97f));
    c[ImGuiCol_Border] = vec(col::borderDim);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = vec(hex(0x0d1117));
    c[ImGuiCol_FrameBgHovered] = vec(hex(0x141b25));
    c[ImGuiCol_FrameBgActive] = vec(hex(0x1a2330));
    c[ImGuiCol_TitleBg] = vec(hex(0x0d1117));
    c[ImGuiCol_TitleBgActive] = vec(hex(0x0d1117));
    c[ImGuiCol_ScrollbarBg] = vec(rgba(0, 0, 0, 0.2f));
    c[ImGuiCol_ScrollbarGrab] = vec(rgba(255, 255, 255, 0.12f));
    c[ImGuiCol_ScrollbarGrabHovered] = vec(rgba(255, 255, 255, 0.2f));
    c[ImGuiCol_ScrollbarGrabActive] = vec(rgba(0, 242, 255, 0.4f));
    c[ImGuiCol_CheckMark] = vec(col::cyan);
    c[ImGuiCol_SliderGrab] = vec(col::cyan);
    c[ImGuiCol_SliderGrabActive] = vec(col::cyan);
    c[ImGuiCol_Button] = vec(rgba(255, 255, 255, 0.05f));
    c[ImGuiCol_ButtonHovered] = vec(rgba(255, 255, 255, 0.1f));
    c[ImGuiCol_ButtonActive] = vec(rgba(255, 255, 255, 0.15f));
    c[ImGuiCol_Header] = vec(rgba(0, 242, 255, 0.12f));
    c[ImGuiCol_HeaderHovered] = vec(rgba(0, 242, 255, 0.2f));
    c[ImGuiCol_HeaderActive] = vec(rgba(0, 242, 255, 0.28f));
    c[ImGuiCol_Separator] = vec(col::borderDim);
    c[ImGuiCol_Tab] = vec(rgba(13, 17, 23, 0.9f));
    c[ImGuiCol_TabHovered] = vec(rgba(255, 255, 255, 0.05f));
    c[ImGuiCol_TabSelected] = vec(rgba(0, 242, 255, 0.05f));
    c[ImGuiCol_TabSelectedOverline] = vec(col::cyan);
    c[ImGuiCol_TabDimmed] = vec(rgba(13, 17, 23, 0.9f));
    c[ImGuiCol_TabDimmedSelected] = vec(rgba(0, 242, 255, 0.05f));
    c[ImGuiCol_TextSelectedBg] = vec(rgba(0, 242, 255, 0.3f));
    c[ImGuiCol_ModalWindowDimBg] = vec(rgba(0, 0, 0, 0.6f));
    c[ImGuiCol_TableBorderLight] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0, 0, 0, 0);
}

} // namespace ui
