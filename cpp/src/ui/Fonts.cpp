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
        std::string sym = winFont("seguisym.ttf"), emo = winFont("seguiemj.ttf");
        if (!sym.empty()) io.Fonts->AddFontFromFileTTF(sym.c_str(), size * 0.95f, &m, symbolRanges());
        if (!emo.empty()) io.Fonts->AddFontFromFileTTF(emo.c_str(), size * 0.95f, &m, symbolRanges());
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
    const std::string regular = pick("Outfit-Regular.ttf", "segoeui.ttf");
    const std::string bold = pick("Outfit-Bold.ttf", "segoeuib.ttf");
    const std::string mono = pick("JetBrainsMono-Regular.ttf", "consola.ttf");
    const std::string monoB = pick("JetBrainsMono-Bold.ttf", "consolab.ttf");

    fonts.ui = addFont(regular, 15.0f * scale, true);
    fonts.uiBold = addFont(bold, 15.0f * scale, true);
    fonts.label = addFont(bold, 12.0f * scale, true);
    fonts.title = addFont(bold, 21.0f * scale, true);
    fonts.mono = addFont(mono, 13.0f * scale, true);
    fonts.monoBold = addFont(monoB, 13.0f * scale, true);
    fonts.canvas = addFont(mono, 18.0f * scale, true);        // canvas / gather text (drawn scaled)
    fonts.canvasBold = addFont(monoB, 18.0f * scale, true);
    fonts.icon = addFont(regular, 26.0f * scale, true);
    io.FontDefault = fonts.ui;
}

} // namespace ui
