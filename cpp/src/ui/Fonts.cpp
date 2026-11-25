#include <filesystem>
#include "Ui.hpp"

namespace ui {

Fonts fonts;
float dpi = 1.0f;

namespace {

std::string winFont(const char* name) {
    const char* windir = std::getenv("WINDIR");
    std::filesystem::path p = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts" / name;
    return std::filesystem::exists(p) ? p.string() : std::string();
}

// Text glyphs: Latin + Latin Extended-A (Turkish ğ ş ı İ), Greek (Δ), punctuation, math (≤)
const ImWchar* textRanges() {
    static ImVector<ImWchar> r;
    if (r.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        static const ImWchar extra[] = {0x0100, 0x017F, 0x0370, 0x03FF, 0x2000, 0x206F, 0x2190, 0x21FF,
                                        0x2200, 0x22FF, 0x2500, 0x25FF, 0};
        b.AddRanges(extra);
        b.BuildRanges(&r);
    }
    return r.Data;
}

// Symbols / emoji used in the labels (monochrome outlines from Segoe UI Symbol / Emoji)
const ImWchar* symbolRanges() {
    static const ImWchar r[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x27BF,
                                0x2B00, 0x2BFF, 0x1F300, 0x1F6FF, 0x1F7E0, 0x1F7FF, 0x1F900, 0x1F9FF, 0};
    return r;
}

// Icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets (Windows 10)
std::string iconFontFile() {
    std::string f = winFont("SegoeIcons.ttf");
    return f.empty() ? winFont("segmdl2.ttf") : f;
}
const ImWchar* iconRanges() {
    static const ImWchar r[] = {0xE700, 0xF8FF, 0};
    return r;
}

ImFont* addFont(const std::string& file, float size, bool withSymbols) {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    if (!file.empty()) f = io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, textRanges());
    if (!f) {
        ImFontConfig d;
        d.SizePixels = size;
        f = io.Fonts->AddFontDefault(&d);
    }
    if (withSymbols) {
        ImFontConfig m;
        m.MergeMode = true;
        m.OversampleH = 1;
        m.GlyphMinAdvanceX = size;
        m.GlyphOffset = ImVec2(0, size * 0.05f);
        std::string sym = winFont("seguisym.ttf");
        if (!sym.empty()) io.Fonts->AddFontFromFileTTF(sym.c_str(), size * 0.95f, &m, symbolRanges());
        // Line icons for buttons and headers (private-use code points, no clash with text)
        ImFontConfig ic;
        ic.MergeMode = true;
        ic.OversampleH = 2;
        ic.GlyphMinAdvanceX = size * 1.05f;
        ic.GlyphOffset = ImVec2(0, size * 0.16f);
        const std::string icons = iconFontFile();
        if (!icons.empty()) io.Fonts->AddFontFromFileTTF(icons.c_str(), size * 0.92f, &ic, iconRanges());
    }
    return f;
}

} // namespace

void loadFonts(float scale) {
    dpi = scale;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Bundled fonts (assets/fonts) take precedence over the Windows ones when present
    auto pick = [](const char* bundled, const char* win) {
        std::filesystem::path p = std::filesystem::path("assets") / "fonts" / bundled;
        return std::filesystem::exists(p) ? p.string() : winFont(win);
    };
    const std::string regular = pick("Inter-Regular.ttf", "segoeui.ttf");
    const std::string semibold = pick("Inter-SemiBold.ttf", "seguisb.ttf");
    std::string numeric = winFont("bahnschrift.ttf");   // DIN-style engineering face (Windows 10 1709+)
    if (numeric.empty()) numeric = regular;

    fonts.ui = addFont(regular, 14.5f * scale, true);
    fonts.uiBold = addFont(semibold, 14.5f * scale, true);
    fonts.label = addFont(semibold, 12.5f * scale, true);
    fonts.title = addFont(numeric, 17.0f * scale, true);
    fonts.mono = addFont(numeric, 13.5f * scale, true);
    fonts.monoBold = fonts.mono;
    fonts.canvas = addFont(numeric, 18.0f * scale, true);        // scene / gather text (drawn scaled)
    fonts.canvasBold = fonts.canvas;
    {
        ImFontConfig c;
        c.OversampleH = 2;
        const std::string icons = iconFontFile();
        fonts.icon = icons.empty() ? fonts.ui : io.Fonts->AddFontFromFileTTF(icons.c_str(), 16.0f * scale, &c, iconRanges());
    }
    io.FontDefault = fonts.ui;
}

} // namespace ui
