#include "App.hpp"
#include <imgui_internal.h>   // ImTextCharFromUtf8
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <portable-file-dialogs.h>
#include "core/ParamsIO.hpp"

namespace ui {

namespace {

// Text with a horizontal colour gradient (header title: white → cyan)
void gradientText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, const char* text, ImU32 c0, ImU32 c1) {
    const float total = f->CalcTextSizeA(size, FLT_MAX, 0, text).x;
    const char* s = text;
    float x = pos.x;
    ImVec4 a = vec(c0), b = vec(c1);
    while (*s) {
        unsigned int cp;
        int n = ImTextCharFromUtf8(&cp, s, nullptr);
        if (n <= 0) break;
        std::string ch(s, s + n);
        float w = f->CalcTextSizeA(size, FLT_MAX, 0, ch.c_str()).x;
        float t = total > 0 ? (x - pos.x + w * 0.5f) / total : 0;
        ImU32 c = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1));
        dl->AddText(f, size, ImVec2(x, pos.y), c, ch.c_str());
        x += w;
        s += n;
    }
}

void centeredRow(float totalW) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (totalW < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - totalW) * 0.5f);
}

ImVec2 pillSize(const char* t, ImFont* f) {
    ImGui::PushFont(f);
    ImVec2 s = ImGui::CalcTextSize(t);
    ImGui::PopFont();
    return ImVec2(s.x + S(20), s.y + S(6));
}

void coloredText(ImFont* f, ImU32 c, const char* t) {
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(c));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Grid of N equal columns (CSS repeat(auto-fit, minmax(minW, 1fr)))
int autoColumns(float minW, int maxCols) {
    float avail = ImGui::GetContentRegionAvail().x;
    return std::clamp((int)(avail / S(minW)), 1, maxCols);
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════
void App::frame(double now) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);

    const float hh = header();
    const float width = vp->WorkSize.x;
    const float margin = std::max(S(24), (width - S(1400)) * 0.5f + S(24));

    // Sticky toolbar
    ImGui::SetCursorPos(ImVec2(margin, hh + S(16)));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(width - 2 * margin);
    ImGui::BeginChild("##toolbarWrap", ImVec2(width - 2 * margin, 0), ImGuiChildFlags_AutoResizeY);
    toolbar(now);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Scrolling content
    ImGui::SetCursorPos(ImVec2(0, ImGui::GetCursorPosY() + S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin, S(8)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    tabs(now);
    cpuPanel();
    legendBar();
    statusBar();
    canvasSection();
    gatherSection();
    footerLegend();
    ImGui::Dummy(ImVec2(0, S(24)));
    ImGui::EndChild();
    ImGui::PopStyleVar();

    eikonalPopup();
    alertsModal();
    ImGui::End();
    applyDeferred();
}

void App::applyDeferred() {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (pendingNpml_) { pendingNpml_ = false; sim_.onNpml(); }
    if (pendingObs_) { pendingObs_ = false; sim_.onObsSpacing(); }
}

// ── Header ────────────────────────────────────────────────────────────────
float App::header() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth();
    const float H = S(88);
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + W, p0.y + H), hex(0x0d1117), hex(0x0d1117), hex(0x010408), hex(0x010408));
    dl->AddLine(ImVec2(p0.x, p0.y + H), ImVec2(p0.x + W, p0.y + H), col::borderDim);

    const float padX = S(48);
    // Icon
    ImVec2 i0(p0.x + padX, p0.y + (H - S(48)) * 0.5f), i1(i0.x + S(48), i0.y + S(48));
    dl->AddRectFilled(ImVec2(i0.x - S(6), i0.y - S(6)), ImVec2(i1.x + S(6), i1.y + S(6)), rgba(0, 132, 255, 0.12f), S(16));
    dl->AddRectFilled(i0, i1, hex(0x0084ff), S(12), ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilledMultiColor(ImVec2(i0.x + S(12), i0.y), ImVec2(i1.x - S(12), i1.y), hex(0x0084ff), hex(0x00f2ff), hex(0x00f2ff), hex(0x0084ff));
    dl->AddRectFilled(ImVec2(i1.x - S(12), i0.y), i1, hex(0x00f2ff), S(12), ImDrawFlags_RoundCornersRight);
    TextAt(dl, fonts.icon, fonts.icon->FontSize, ImVec2((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f - fonts.icon->FontSize * 0.5f),
           IM_COL32_WHITE, "📡", 1, false);

    // Title
    const float tx = i1.x + S(24);
    gradientText(dl, fonts.title, fonts.title->FontSize, ImVec2(tx, p0.y + S(20)),
                 "Advanced Geophysical Core – FDTD Solver", IM_COL32_WHITE, col::cyan);
    dl->AddText(fonts.ui, S(13.5f), ImVec2(tx, p0.y + S(50)), col::muted,
                "v2.4 Native C++ · OpenGL Compute · Real-Time Seismic Acquisition & Modeling Platform");

    // GPU / CPU badge (click = toggleComputeEngine)
    const Badge& b = sim_.badge;
    std::string label;
    ImU32 bg, fg, br, reasonCol;
    switch (b.kind) {
        case BadgeKind::Gpu: label = "⚡ GPU: " + (b.label.empty() ? std::string("READY") : b.label);
            bg = rgba(0, 255, 157, 0.1f); fg = col::green; br = rgba(0, 255, 157, 0.3f); reasonCol = col::muted; break;
        case BadgeKind::ForcedCpu: label = "🖥 CPU THREADS (Zorlanmış)";
            bg = rgba(0, 132, 255, 0.1f); fg = col::cyan; br = rgba(0, 132, 255, 0.3f); reasonCol = col::cyan; break;
        case BadgeKind::Warning: label = "⚠ " + b.label;
            bg = rgba(255, 145, 0, 0.1f); fg = col::orange; br = rgba(255, 145, 0, 0.3f); reasonCol = col::orange; break;
        case BadgeKind::Cpu: label = "🖥 CPU THREADS";
            bg = rgba(0, 132, 255, 0.1f); fg = col::cyan; br = rgba(0, 132, 255, 0.3f); reasonCol = col::orange; break;
        default: label = "🔌 Initialization...";
            bg = rgba(0, 132, 255, 0.1f); fg = col::cyan; br = rgba(0, 132, 255, 0.2f); reasonCol = col::muted; break;
    }
    ImGui::PushFont(fonts.label);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    ImGui::PopFont();
    const float bw = ts.x + S(28), bh = ts.y + S(12);
    const float bx = p0.x + W - padX - bw;
    const float by = p0.y + (b.reason.empty() ? (H - bh) * 0.5f : S(16));
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    ImGui::InvisibleButton("##badge", ImVec2(bw, bh));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) sim_.toggleComputeEngine();
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Tıkla CPU/GPU değiştir");
        dl->AddRectFilled(ImVec2(bx - S(3), by - S(3)), ImVec2(bx + bw + S(3), by + bh + S(3)), rgba(0, 132, 255, 0.15f), S(8));
    }
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bg, S(6));
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), br, S(6));
    dl->AddText(fonts.label, fonts.label->FontSize, ImVec2(bx + S(14), by + S(6)), fg, label.c_str());
    if (!b.reason.empty()) {
        const float wrap = S(300);
        ImVec2 rs = fonts.ui->CalcTextSizeA(S(11.5f), FLT_MAX, wrap, b.reason.c_str());
        dl->AddText(fonts.ui, S(11.5f), ImVec2(p0.x + W - padX - rs.x, by + bh + S(6)), reasonCol, b.reason.c_str(),
                    nullptr, wrap);
    }
    return H;
}

// ── Toolbar ───────────────────────────────────────────────────────────────
void App::toolbar(double now) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(col::bgPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(16)));
    ImGui::BeginChild("##toolbar", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    Flow fl = Flow::begin();
    SimParams& p = sim_.p;

    auto btn = [&](const char* label, ImU32 bg, ImU32 br, ImU32 fg, ImU32 hover = 0) {
        fl.next(ButtonWidth(label) + S(12));
        return StyledButton(label, bg, br, fg, ImVec2(0, 0), hover);
    };
    if (btn("⚡ CLO Modu", rgba(0, 132, 255, 0.4f), rgba(0, 132, 255, 0.6f), IM_COL32_WHITE, col::blue)) sim_.startMode(Mode::CLO);
    if (btn("🔴 SLO Modu", rgba(255, 60, 92, 0.4f), rgba(255, 60, 92, 0.6f), IM_COL32_WHITE, col::red)) sim_.startMode(Mode::SLO);
    if (btn("🚢 Geleneksel Mod", rgba(255, 145, 0, 0.4f), rgba(255, 145, 0, 0.6f), IM_COL32_WHITE, col::orange)) sim_.startMode(Mode::Trad);
    if (sim_.paused) {
        if (btn("▶ Devam Et###pause", rgba(0, 229, 160, 0.1f), rgba(0, 229, 160, 0.3f), hex(0x00e5a0))) sim_.togglePause();
    } else {
        if (btn("⏸ Duraklat###pause", rgba(255, 193, 7, 0.1f), rgba(255, 193, 7, 0.3f), hex(0xffc107))) sim_.togglePause();
    }
    if (btn("⏹ Sistemi Sıfırla", rgba(0, 0, 0, 0), col::borderDim, col::muted, rgba(255, 255, 255, 0.06f))) sim_.stopMode();
    if (p.sismo_only) {
        if (btn("👁 Sadece Sismogram (Turbo)###sismoOnly", rgba(255, 145, 0, 0.1f), rgba(255, 145, 0, 0.3f), col::orange)) sim_.toggleSismoOnly();
    } else {
        if (btn("🌊 Modeli Göster (Aktif)###sismoOnly", rgba(0, 255, 157, 0.1f), rgba(0, 255, 157, 0.3f), col::green)) sim_.toggleSismoOnly();
    }
    if (p.expanding_box) {
        if (btn("🟩 Expanding Box (Açık)###ebox", rgba(0, 255, 128, 0.1f), rgba(0, 255, 128, 0.3f), hex(0x00ff80))) sim_.toggleExpandingBox();
    } else {
        if (btn("🔲 Expanding Box (Kapalı)###ebox", rgba(255, 255, 255, 0.1f), rgba(255, 255, 255, 0.3f), IM_COL32_WHITE)) sim_.toggleExpandingBox();
    }
    if (p.free_surface) {
        if (btn("🌊 Deniz Yüzeyi Yansıması (Açık)###fs", rgba(0, 191, 255, 0.1f), rgba(0, 191, 255, 0.3f), hex(0x00bfff))) sim_.toggleFreeSurface();
    } else {
        if (btn("🌫 Deniz Yüzeyi Yansıması (Kapalı)###fs", rgba(255, 255, 255, 0.1f), rgba(255, 255, 255, 0.3f), IM_COL32_WHITE)) sim_.toggleFreeSurface();
    }

    // KAYIT UZUNLUĞU (ms) — shared with the sismogram Y-axis limit
    {
        const float groupW = S(250);
        fl.next(groupW);
        ImVec2 g0 = ImGui::GetCursorScreenPos();
        const float gh = ImGui::GetFrameHeight() + S(6);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(g0, ImVec2(g0.x + groupW, g0.y + gh), rgba(168, 85, 247, 0.1f), S(6));
        dl->AddRect(g0, ImVec2(g0.x + groupW, g0.y + gh), rgba(168, 85, 247, 0.2f), S(6));
        ImGui::BeginGroup();
        ImGui::SetCursorScreenPos(ImVec2(g0.x + S(10), g0.y + S(3)));
        ImGui::AlignTextToFramePadding();
        coloredText(fonts.label, hex(0xc084fc), "KAYIT UZUNLUĞU:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(70));
        ImGui::PushFont(fonts.monoBold);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(hex(0xc084fc)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, vec(hex(0xc084fc, 0.6f)));
        int ms = p.sismo_ms;
        ImGui::InputInt("##segyLen", &ms, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()) p.sismo_ms = std::clamp(ms, 500, 10000);
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
        ImGui::SameLine();
        coloredText(fonts.label, hex(0xc084fc), "ms");
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(g0);
        ImGui::Dummy(ImVec2(groupW, gh));
    }
    segyButton(now);
    (void)fl;
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::segyButton(double now) {
    const std::string& label = sim_.segyButtonLabel(now);
    const std::string id = label + "###segy";
    ImGui::SameLine();
    if (ImGui::GetItemRectMax().x + S(12) + ButtonWidth(label.c_str()) + S(32) >
        ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - S(24))
        ImGui::NewLine();
    bool rec = sim_.segyRecording();
    bool clicked = rec ? StyledButton(id.c_str(), rgba(239, 68, 68, 0.2f), rgba(239, 68, 68, 0.4f), hex(0xf87171))
                       : StyledButton(id.c_str(), rgba(168, 85, 247, 0.15f), rgba(168, 85, 247, 0.3f), hex(0xc084fc));
    if (!clicked) return;
    if (rec) { sim_.segyStop(now); return; }
    if (sim_.mode == Mode::None) {
        alert("Önce CLO veya SLO modunu başlatın, ardından butona tekrar basın.");
        return;
    }
    std::string dir = pfd::select_folder("SEG-Y kayıt klasörünü seçin").result();
    if (!dir.empty()) sim_.segyStart(dir);
}

// ── Settings tabs ─────────────────────────────────────────────────────────
void App::tabs(double now) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(col::bgPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##tabs", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (ImGui::BeginTabBar("##tabbar", ImGuiTabBarFlags_FittingPolicyScroll)) {
        auto tab = [&](const char* label, auto&& fn) {
            // Header measured and drawn with the same font/padding (otherwise labels get clipped)
            ImGui::PushFont(fonts.uiBold);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(24), S(12)));
            const bool open = ImGui::BeginTabItem(label);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            if (!open) return;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(24)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("##tabBody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            fn();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        };
        tab("🔬 FİZİKSEL & SİNYAL", [&] { tabPhysics(now); });
        tab("📐 SAHA GEOMETRİSİ", [&] { tabGeometry(); });
        tab("🌍 JEOLOJİ & OBS", [&] { tabGeology(now); });
        tab("💾 KAYIT & YÜKLE", [&] { tabRecord(); });
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(8)));
}

void App::tabPhysics(double now) {
    SimParams& p = sim_.p;
    const int cols = autoColumns(340, 2);
    if (!ImGui::BeginTable("##physGrid", cols, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextColumn();
    if (BeginCard("##cardPhys", "🔬 FİZİKSEL PARAMETRELER")) {
        static const char* orders[] = {"2. Derece", "4. Derece", "6. Derece", "8. Derece", "10. Derece"};
        int idx = p.fd_order / 2 - 1;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Türev Derecesi");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(150));
        if (StyledCombo("##fdOrder", &idx, orders, 5, S(150))) { p.fd_order = (idx + 1) * 2; sim_.onFdOrder(); }
        if (ParamRow("Izgara Aralığı (m)", "dh", &p.dh, 0.5f, 8.0f, 0.5f, "%.1f")) sim_.onGridSize(now);
        if (ParamRow("Zaman Katsayısı (ms)", "dt", &p.dt, 0.1f, 1.5f, 0.1f, "%.1f")) sim_.onTimeStep();
        if (ParamRowInt("CPML Yutucu Hücre", "npml", &p.npml, 5, 60, 5)) pendingNpml_ = true;
    }
    EndCard();
    ImGui::TableNextColumn();
    if (BeginCard("##cardSig", "🌊 SİNYAL ÖZELLİKLERİ")) {
        static const char* wav[] = {"Ricker (Sıfır Faz)", "Minimum Faz"};
        int w = (int)p.wavelet;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Dalgacık Tipi");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(180));
        if (StyledCombo("##wavelet", &w, wav, 2, S(180))) p.wavelet = (Wavelet)w;
        ParamRow("Dominant Frekans (Hz)", "freq", &p.freq, 0.5f, 250.0f, 0.5f, "%.1f");
        ParamRow("Kaynak Amplitüdü", "amp", &p.amp, 0.2f, 20.0f, 0.2f, "%.1f");
    }
    EndCard();
    ImGui::EndTable();
}

void App::tabGeometry() {
    SimParams& p = sim_.p;
    if (BeginCard("##cardGeo", "📐 SAHA GEOMETRİSİ")) {
        const int cols = autoColumns(320, 2);
        if (ImGui::BeginTable("##geoGrid", cols, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            if (ParamRow("Kablo Boyu (m)", "cable", &p.cable_len, 50, 1500, 10, "%.0f")) sim_.onCableLen();
            {
                static const char* sp[] = {"3.125 m", "6.25 m", "12.5 m", "25 m", "50 m", "100 m"};
                static const float spv[] = {3.125f, 6.25f, 12.5f, 25.f, 50.f, 100.f};
                int idx = 1;
                for (int i = 0; i < 6; i++) if (std::fabs(spv[i] - p.receiver_spacing) < 1e-3f) idx = i;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Alıcı Grup Aralığı (m)");
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(120));
                if (StyledCombo("##groupSpace", &idx, sp, 6, S(120))) { p.receiver_spacing = spv[idx]; sim_.onReceiverSpacing(true); }
            }
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Alıcı Sayısı (N_REC)");
                char b[16]; std::snprintf(b, sizeof(b), "%d", (int)std::floor(p.cable_len / p.receiver_spacing) + 1);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(b).x - S(8));
                coloredText(fonts.mono, col::cyan, b);
            }
            ParamRow("Kablo Baş Derinliği (m)", "cHead", &p.cable_head_depth, 2, 100, 1, "%.0f");
            ParamRow("Kablo Son Derinliği (m)", "cTail", &p.cable_tail_depth, 2, 250, 1, "%.0f");
            ParamRow("Kaynak Derinliği (m)", "srcDepth", &p.source_depth, 2, 50, 1, "%.0f");

            ImGui::TableNextColumn();
            float shipX = (float)sim_.sxShip;
            const float shipMax = (float)(sim_.W_sim * sim_.SCALE);
            if (ParamRow("Gemi X Konumu (m)", "shipX", &shipX, 100, std::max(110.0f, shipMax), 10, "%.0f")) sim_.onShipX(shipX);
            if (ParamRow("2. Gemi Ofseti (m)", "srcOff", &p.src_offset, 0, 800, 10, "%.0f")) sim_.onSourceOffset();
            ParamRow("Gemi - Kaynak (m)", "shipSrc", &p.ship_src_dist, 0, 300, 10, "%.0f");
            ParamRow("Kaynak - Alıcı (m)", "srcRec", &p.src_rec_dist, 0, 500, 10, "%.0f");
            ParamRow("Gemi Hızı (Knot)", "speed", &p.ship_speed_knots, 0, 12, 0.5f, "%.1f");
            ParamRow("Atış Aralığı (m)", "shot", &p.shot_interval, 6.25f, 100, 6.25f, "%.2f");
            ImGui::EndTable();
        }
    }
    EndCard();
}

void App::tabGeology(double now) {
    SimParams& p = sim_.p;
    const int cols = autoColumns(340, 2);
    if (!ImGui::BeginTable("##geoTab", cols, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextColumn();
    if (BeginCard("##cardGeol", "🌍 JEOLOJİ VE ALAN KAPSAMI", hex(0xfbbf24))) {
        static const char* models[] = {"Düz Katmanlar", "Fay Kırığı Sistemi", "Tuz Domu (Kompleks)"};
        int g = (int)p.geology;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Hız Modeli (Geology)");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(190));
        if (StyledCombo("##geoModel", &g, models, 3, S(190))) { p.geology = (GeologyModel)g; sim_.onGeology(); }
        if (ParamRow("Alan Genişliği (m)", "uiW", &p.domain_W, 800, 25000, 100, "%.0f")) sim_.onDomainWidth(now);
        if (ParamRow("Alan Derinliği (m)", "uiH", &p.domain_H, 400, 12000, 40, "%.0f")) sim_.onDomainHeight(now);
    }
    EndCard();
    ImGui::TableNextColumn();
    if (BeginCard("##cardObs", "🕹 OBS KONTROL (SEABED)")) {
        if (ParamRow("OBS Aralığı (m)", "obsSp", &p.obs_spacing, 50, 500, 10, "%.0f")) pendingObs_ = true;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Alıcı Sayısı");
        char b[16]; std::snprintf(b, sizeof(b), "%d", sim_.N_OBS);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(b).x - S(8));
        coloredText(fonts.mono, col::cyan, b);
        const float w = ImGui::GetContentRegionAvail().x;
        if (p.obs_enabled) {
            if (StyledButton("🟡 OBS Aktif###obsT", rgba(251, 191, 36, 0.15f), rgba(251, 191, 36, 0.3f), hex(0xfbbf24), ImVec2(w, 0))) sim_.toggleObs();
        } else {
            if (StyledButton("⚪ OBS Kapalı###obsT", rgba(255, 255, 255, 0.1f), rgba(255, 255, 255, 0.2f), hex(0xaaaaaa), ImVec2(w, 0))) sim_.toggleObs();
        }
    }
    EndCard();
    ImGui::EndTable();
}

void App::tabRecord() {
    const int cols = autoColumns(340, 2);
    if (!ImGui::BeginTable("##recTab", cols, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextColumn();
    if (BeginCard("##cardRec", "💾 PARAMETRE YÖNETİMİ", hex(0x10b981))) {
        const float w = ImGui::GetContentRegionAvail().x;
        if (StyledButton("💾 Yerel Kayda Kaydet (AppData)", rgba(16, 185, 129, 0.15f), rgba(16, 185, 129, 0.3f), hex(0x10b981), ImVec2(w, 0))) saveLocal();
        if (StyledButton("📂 Yerel Kayıttan Yükle (AppData)", rgba(0, 0, 0, 0), col::borderDim, hex(0x10b981), ImVec2(w, 0))) loadLocal();
        ImGui::Dummy(ImVec2(0, S(10)));
        if (StyledButton("📤 Dosyaya Kaydet (.json)", rgba(59, 130, 246, 0.15f), rgba(59, 130, 246, 0.3f), hex(0x3b82f6), ImVec2(w, 0))) saveFile();
        if (StyledButton("📥 Dosyadan Yükle (.json)", rgba(0, 0, 0, 0), col::borderDim, hex(0x3b82f6), ImVec2(w, 0))) loadFile();
    }
    EndCard();
    ImGui::EndTable();
}

void App::saveLocal() {
    if (writeTextFile(localParamsPath(), sim_.configJson())) alert("Parametreler yerel kayda (AppData) başarıyla kaydedildi.");
    else alert("Parametreler kaydedilemedi: " + localParamsPath());
}

void App::loadLocal() {
    std::string text, err;
    if (!readTextFile(localParamsPath(), text)) { alert("Yerel hafızada kayıtlı parametre bulunamadı."); return; }
    if (sim_.applyConfigJson(text, err)) alert("Parametreler yerel hafızadan yüklendi!");
    else alert("Kayıt okunamadı veya bozuk!");
}

void App::saveFile() {
    std::string path = pfd::save_file("Parametreleri Kaydet", "seismic_parameters.json", {"JSON (*.json)", "*.json"}).result();
    if (path.empty()) return;
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json") path += ".json";
    if (!writeTextFile(path, sim_.configJson())) alert("Dosya yazılamadı: " + path);
}

void App::loadFile() {
    auto files = pfd::open_file("Parametreleri Yükle", "", {"JSON (*.json)", "*.json"}).result();
    if (files.empty()) return;
    std::string text, err;
    if (!readTextFile(files[0], text)) { alert("Dosya okunamadı!"); return; }
    if (sim_.applyConfigJson(text, err)) alert("Parametreler dosyadan başarıyla yüklendi!");
    else alert("Geçersiz JSON dosyası!");
}

// ── CPU thread load bars ──────────────────────────────────────────────────
void App::cpuPanel() {
    if (sim_.mode == Mode::None || sim_.usingGpu() || sim_.threadStats.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(rgba(13, 17, 23, 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(10)));
    ImGui::BeginChild("##cpuPanel", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        ImVec2 c = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(c.x + S(3), c.y + S(7)), S(3), col::cyan);
        ImGui::SetCursorScreenPos(ImVec2(c.x + S(14), c.y));
        coloredText(fonts.label, col::cyan, "THREAD BAZLI İŞLEMCİ YÜKÜ (%)");
    }
    const int n = (int)sim_.threadStats.size();
    const int cols = std::max(1, std::min(n, (int)(ImGui::GetContentRegionAvail().x / S(150))));
    if (ImGui::BeginTable("##cpuBars", cols, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < n; i++) {
            ImGui::TableNextColumn();
            const ThreadStat& st = sim_.threadStats[i];
            const double total = st.compute + st.wait;
            const float load = total > 0 ? (float)(st.compute / total * 100.0) : 0.0f;
            char lbl[16], val[16];
            std::snprintf(lbl, sizeof(lbl), "CPU%d", i);
            std::snprintf(val, sizeof(val), "%.0f%%", load);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = S(16);
            const float lw = S(38), vw = S(32);
            dl->AddText(fonts.mono, S(11), ImVec2(p0.x, p0.y + S(2)), IM_COL32_WHITE, lbl);
            ImVec2 b0(p0.x + lw, p0.y + S(5)), b1(p0.x + w - vw, p0.y + S(11));
            dl->AddRectFilled(b0, b1, rgba(255, 255, 255, 0.1f), S(3));
            ImU32 c0 = load > 85 ? hex(0xff4d6d) : load > 50 ? hex(0xffd700) : hex(0x1e90ff);
            ImU32 c1 = load > 50 ? hex(0xff8c42) : hex(0x00d4ff);
            float fw = (b1.x - b0.x) * std::clamp(load / 100.0f, 0.0f, 1.0f);
            if (fw > 1) dl->AddRectFilledMultiColor(b0, ImVec2(b0.x + fw, b1.y), c0, c1, c1, c0);
            TextAt(dl, fonts.mono, S(10), ImVec2(p0.x + w, p0.y + S(2)), hex(0x00d4ff), val, 2, false);
            ImGui::Dummy(ImVec2(w, h));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ── Velocity legend + numerical status ───────────────────────────────────
void App::legendBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(rgba(13, 17, 23, 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(10)));
    ImGui::BeginChild("##legendBar", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    struct Item { const char* t; ImU32 c; };
    const Item items[] = {{"VELOCITY MODEL:", col::muted}, {"SU (WATER) 1500m/s", col::cyan},
                          {"TORTUL (SEDIMENT) 2100m/s", hex(0x528bff)}, {"TEMEL KAYA (BASEMENT) 2880m/s", hex(0x6a8db3)},
                          {"TUZ DOMU (SALT) 4080m/s", hex(0xbdcdde)}};
    ImGui::PushFont(fonts.mono);
    float total = 0;
    for (const Item& it : items) total += ImGui::CalcTextSize(it.t).x + S(24);
    total -= S(24);
    centeredRow(total);
    for (int i = 0; i < 5; i++) {
        if (i) ImGui::SameLine(0, S(24));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(items[i].c));
        ImGui::TextUnformatted(items[i].t);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::statusBar() {
    const NumericalStatus ns = sim_.numericalStatus();
    char cfl[64], disp[64], ppw[64], dtm[32], cpu[64], timer[48];
    ImU32 cflC, dispC;
    const int pct = (int)std::lround(ns.cfl_ratio * 100);
    if (ns.cfl_ratio < 0.85) { std::snprintf(cfl, sizeof(cfl), "CFL: STABIL (%d%%)", pct); cflC = col::green; }
    else if (ns.cfl_ratio < 1.0) { std::snprintf(cfl, sizeof(cfl), "CFL: SINIRDA (%d%%)", pct); cflC = col::orange; }
    else { std::snprintf(cfl, sizeof(cfl), "CFL: KRITIK (%d%%)", pct); cflC = col::red; }
    if (ns.ppw >= ns.min_ppw * 1.5) { std::snprintf(disp, sizeof(disp), "DAGILIM: TEMIZ (%.1f)", ns.ppw); dispC = col::green; }
    else if (ns.ppw >= ns.min_ppw) { std::snprintf(disp, sizeof(disp), "DAGILIM: MARJINAL (%.1f)", ns.ppw); dispC = col::orange; }
    else { std::snprintf(disp, sizeof(disp), "DAGILIM: YETERSIZ (%.1f)", ns.ppw); dispC = col::red; }
    std::snprintf(ppw, sizeof(ppw), "PPW: %.1f (min: %d)", ns.ppw, ns.min_ppw);
    std::snprintf(dtm, sizeof(dtm), "dt_max: %.2f", ns.dt_max);
    std::snprintf(cpu, sizeof(cpu), "🧠 CPU: %d Cekirdek Aktif", sim_.cpuThreads());
    std::snprintf(timer, sizeof(timer), "Gecen Sure: %d sn", (int)sim_.elapsedSec);

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = S(38);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), rgba(2, 8, 18, 0.88f));
    dl->AddLine(p0, ImVec2(p0.x + w, p0.y), hex(0x0d2040));
    dl->AddLine(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + w, p0.y + h), hex(0x0d2040));

    auto alpha = [](ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(a * 255) << 24); };
    const char* lead = "Numerik Kontrol:";
    ImGui::PushFont(fonts.mono);
    const float gap = S(18);
    float total = ImGui::CalcTextSize(lead).x + gap + pillSize(cfl, fonts.monoBold).x + gap + pillSize(disp, fonts.monoBold).x +
                  gap + ImGui::CalcTextSize(ppw).x + gap + ImGui::CalcTextSize(dtm).x + gap + ImGui::CalcTextSize("|").x + gap;
    ImGui::PopFont();
    ImGui::PushFont(fonts.monoBold);
    total += ImGui::CalcTextSize(cpu).x + gap + ImGui::CalcTextSize(timer).x;
    ImGui::PopFont();
    float x = p0.x + std::max(S(8), (w - total) * 0.5f);
    const float ty = p0.y + (h - fonts.mono->FontSize) * 0.5f;
    auto txt = [&](ImFont* f, ImU32 c, const char* t) {
        dl->AddText(f, f->FontSize, ImVec2(x, ty), c, t);
        x += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t).x + gap;
    };
    auto pill = [&](ImU32 c, const char* t) {
        ImFont* f = fonts.monoBold;
        float fs = f->FontSize;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
        ImVec2 a(x, ty - S(3)), b(x + ts.x + S(20), ty + ts.y + S(3));
        dl->AddRectFilled(a, b, alpha(c, 0.1f), S(5));
        dl->AddRect(a, b, alpha(c, 0.3f), S(5));
        dl->AddText(f, fs, ImVec2(x + S(10), ty), c, t);
        x = b.x + gap;
    };
    txt(fonts.mono, rgba(255, 255, 255, 0.45f), lead);
    pill(cflC, cfl);
    pill(dispC, disp);
    txt(fonts.mono, rgba(255, 255, 255, 0.4f), ppw);
    txt(fonts.mono, rgba(255, 255, 255, 0.4f), dtm);
    txt(fonts.mono, rgba(255, 255, 255, 0.2f), "|");
    txt(fonts.monoBold, col::green, cpu);
    txt(fonts.monoBold, hex(0xffcc00), timer);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, S(4)));
}

// ── Main canvas ───────────────────────────────────────────────────────────
void App::canvasSection() {
    SimParams& p = sim_.p;
    const float w = ImGui::GetContentRegionAvail().x;
    const float k = w / p.domain_W;
    const float ch = p.domain_H * k;
    const float stripH = S(52);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float R = S(24);

    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + ch + stripH), hex(0x020812), R);
    dl->PushClipRect(o, ImVec2(o.x + w, o.y + ch), true);
    drawScene(sim_, dl, o, k, overlay_);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, ch));
    const ImVec2 after = ImGui::GetCursorScreenPos();

    // Mode indicator (top-left)
    {
        std::string text;
        ImU32 c;
        switch (sim_.mode) {
            case Mode::CLO: text = "CLO AKTİF"; c = col::cyan; break;
            case Mode::SLO: text = "SLO AKTİF"; c = col::red; break;
            case Mode::Trad: text = "GELENEKSEL MOD"; c = col::orange; break;
            default: text = "Beklemede"; c = hex(0x8b949e); break;
        }
        ImFont* f = fonts.uiBold;
        const float fs = S(12.5f);
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str());
        const char* pz = " (DURAKLATILDI)";
        ImVec2 ps = sim_.paused ? f->CalcTextSizeA(S(10), FLT_MAX, 0, pz) : ImVec2(0, 0);
        ImVec2 a(o.x + S(20), o.y + S(20)), b(a.x + ts.x + ps.x + S(42), a.y + ts.y + S(12));
        dl->AddRectFilled(a, b, rgba(1, 4, 8, 0.8f), S(8));
        dl->AddRect(a, b, sim_.mode == Mode::None ? col::borderDim : ((c & 0x00FFFFFF) | 0x4D000000), S(8));
        ImVec2 dot(a.x + S(18), (a.y + b.y) * 0.5f);
        dl->AddCircleFilled(dot, S(7), (c & 0x00FFFFFF) | 0x40000000);
        dl->AddCircleFilled(dot, S(4), c);
        dl->AddText(f, fs, ImVec2(a.x + S(30), a.y + S(6)), c, text.c_str());
        if (sim_.paused) dl->AddText(f, S(10), ImVec2(a.x + S(30) + ts.x, a.y + S(8)), hex(0xffc107), pz);
    }

    // Eye toggle (top-right): all text / none / time only
    {
        const char* icons[] = {"👁", "🚫", "⏱"};
        const ImU32 borders[] = {rgba(30, 144, 255, 0.3f), rgba(255, 100, 100, 0.35f), rgba(0, 229, 160, 0.3f)};
        const float bs = S(32);
        ImVec2 a(o.x + w - S(10) - bs, o.y + S(10));
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##overlay", ImVec2(bs, bs));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) overlay_ = (overlay_ + 1) % 3;
        if (hov) ImGui::SetTooltip("Yazıları Gizle / Göster");
        const float alpha = hov ? 1.0f : 0.25f;
        dl->AddRectFilled(a, ImVec2(a.x + bs, a.y + bs), rgba(4, 16, 34, 0.78f * alpha), S(8));
        ImVec4 bc = vec(borders[overlay_]);
        dl->AddRect(a, ImVec2(a.x + bs, a.y + bs), ImGui::ColorConvertFloat4ToU32(ImVec4(bc.x, bc.y, bc.z, bc.w * alpha)), S(8));
        TextAt(dl, fonts.ui, S(16), ImVec2(a.x + bs * 0.5f, a.y + S(7)), rgba(138, 180, 212, alpha), icons[overlay_], 1, false);
    }

    // Courant instability warning
    if (sim_.instability) {
        const float bw = std::min(S(600), w - S(40));
        ImGui::SetCursorScreenPos(ImVec2(o.x + (w - bw) * 0.5f, o.y + S(30)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(rgba(40, 0, 0, 0.95f)));
        ImGui::PushStyleColor(ImGuiCol_Border, vec(hex(0xff4d6d)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(15));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(30), S(24)));
        ImGui::BeginChild("##cflWarn", ImVec2(bw, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        const NumericalStatus ns = sim_.numericalStatus();
        coloredText(fonts.title, hex(0xff4d6d), "⚠ COURANT KARARSIZLIĞI (INSTABILITY) ⚠");
        ImGui::Spacing();
        ImGui::TextWrapped("Nümerik enerji limiti aştı ve model patladı (Dalga Patlaması).");
        ImGui::Spacing();
        coloredText(fonts.uiBold, hex(0xff8c42), "Neden Oldu?");
        ImGui::TextWrapped("Kullandığınız Uzamsal Hassasiyet (%d. Derece) için simülasyonun patlamaması (CFL Stabilite Şartı) "
                           "adına aşağıdakinden yüksek zaman katsayısı kullanmamalısınız:", sim_.p.fd_order);
        char b[96];
        std::snprintf(b, sizeof(b), "Maksimum Güvenli: Δt ≤ %.2f", ns.dt_max);
        Pill(b, rgba(255, 140, 66, 0.2f), hex(0xff8c42), IM_COL32_WHITE, fonts.monoBold);
        ImGui::Text("Şu an kullanılan katsayı:");
        ImGui::SameLine();
        std::snprintf(b, sizeof(b), "Δt = %g", sim_.p.dt);
        coloredText(fonts.uiBold, hex(0xff4d6d), b);
        ImGui::SameLine();
        ImGui::TextUnformatted("(Model çok hızlı atladı)");
        ImGui::Spacing();
        coloredText(fonts.uiBold, hex(0x00e5a0), "Ne Yapmalısınız?");
        ImGui::TextWrapped("Yukarıdaki kontrollerden \"Sistemi Sıfırla\" butonuna basın ve Zaman Adımı (Δt) değerini önerilen "
                           "sınırın altına çekin veya Uzamsal Hassasiyeti düşürün.");
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    // Visualisation strip
    ImGui::SetCursorScreenPos(after);
    ImVec2 s0 = after;
    dl->AddLine(s0, ImVec2(s0.x + w, s0.y), rgba(30, 144, 255, 0.15f));
    dl->AddRectFilled(s0, ImVec2(s0.x + w, s0.y + stripH), rgba(4, 16, 34, 0.95f), R, ImDrawFlags_RoundCornersBottom);
    ImGui::SetCursorScreenPos(ImVec2(s0.x + S(20), s0.y + (stripH - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::BeginGroup();
    const float half = (w - S(80)) * 0.5f;
    ImGui::BeginChild("##viz1", ImVec2(half, ImGui::GetFrameHeight()), 0, ImGuiWindowFlags_NoBackground);
    ParamRow("🖥 GÖRSEL KAZANÇ (x)", "gain", &p.gain, 1, 100, 1, "%.0f");
    ImGui::EndChild();
    ImGui::SameLine(0, S(40));
    ImGui::BeginChild("##viz2", ImVec2(half, ImGui::GetFrameHeight()), 0, ImGuiWindowFlags_NoBackground);
    ParamRow("⚡ KARE BAŞINA İLERLEME (ms)", "spd", &p.speed_ms, 1, 250, 0.5f, "%.1f");
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(s0.x, s0.y + stripH));
    dl->AddRect(o, ImVec2(o.x + w, o.y + ch + stripH), col::borderDim, R);
    ImGui::Dummy(ImVec2(w, S(16)));
}

// ── Sismogram panels ─────────────────────────────────────────────────────
void App::gatherSection() {
    SimParams& p = sim_.p;
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    const float headerH = S(56);

    // Header
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + headerH), rgba(13, 17, 23, 0.8f), S(16), ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(o.x, o.y + headerH), ImVec2(o.x + w, o.y + headerH), col::borderDim);
    dl->AddText(fonts.uiBold, S(13), ImVec2(o.x + S(24), o.y + S(12)), col::text, "Simültane Sismogram Panelleri");
    dl->AddText(fonts.ui, S(11), ImVec2(o.x + S(24), o.y + S(31)), rgba(175, 186, 202, 0.6f), "Dual Acquisition Engine v2.0");
    TextAt(dl, fonts.ui, S(11), ImVec2(o.x + w - S(24), o.y + S(13)), rgba(175, 186, 202, 0.6f), "Sol: Towed Streamer", 2, false);
    TextAt(dl, fonts.ui, S(11), ImVec2(o.x + w - S(24), o.y + S(29)), rgba(175, 186, 202, 0.6f), "Sağ: Stationary OBS", 2, false);
    {
        struct L { const char* t; ImU32 c; };
        std::vector<L> items;
        if (sim_.mode == Mode::CLO) { items.push_back({"KAYNAK GEMİSİ (2. Atış)", hex(0x1e90ff)}); items.push_back({"ANA GEMİ (1. Atış)", hex(0xff1744)}); }
        if (sim_.mode == Mode::SLO) items.push_back({"EŞZAMANLI KARIŞIK SİSMOGRAM", hex(0x00e5a0)});
        items.push_back({"OBS RECEIVERS", hex(0xfbbf24, 0.8f)});
        const float fs = S(11);
        float tw = 0;
        for (auto& it : items) tw += fonts.uiBold->CalcTextSizeA(fs, FLT_MAX, 0, it.t).x + S(14) + S(20);
        tw += S(32) - S(20);
        float x0 = o.x + (w - tw) * 0.5f, y0 = o.y + S(14);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + tw, y0 + S(28)), rgba(0, 0, 0, 0.3f), S(14));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + tw, y0 + S(28)), rgba(255, 255, 255, 0.05f), S(14));
        float x = x0 + S(16);
        for (auto& it : items) {
            dl->AddRectFilled(ImVec2(x, y0 + S(10)), ImVec2(x + S(8), y0 + S(18)), it.c, S(2));
            dl->AddText(fonts.uiBold, fs, ImVec2(x + S(14), y0 + S(7)), it.c, it.t);
            x += fonts.uiBold->CalcTextSizeA(fs, FLT_MAX, 0, it.t).x + S(14) + S(20);
        }
    }

    // Two panes
    const float gap = S(4);
    const float paneW = (w - gap * 3) * 0.5f;
    const float k = paneW / GW;
    const float paneH = GH * k;
    ImVec2 pa(o.x + gap, o.y + headerH + gap);
    dl->AddRectFilled(ImVec2(o.x, o.y + headerH), ImVec2(o.x + w, pa.y + paneH + gap), hex(0x000208));
    drawGatherPanel(sim_, gv_, 0, pa, k);
    drawGatherPanel(sim_, gv_, 1, ImVec2(pa.x + paneW + gap, pa.y), k);
    ImGui::SetCursorScreenPos(ImVec2(o.x, pa.y + paneH + gap));

    // View controls
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float rowH = S(44);
    dl->AddRectFilled(c0, ImVec2(c0.x + w, c0.y + rowH), rgba(13, 17, 23, 0.8f));
    dl->AddLine(c0, ImVec2(c0.x + w, c0.y), col::borderDim);
    {
        ImGui::PushFont(fonts.label);
        const float est = S(560);
        ImGui::SetCursorScreenPos(ImVec2(c0.x + std::max(S(16), (w - est) * 0.5f), c0.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::AlignTextToFramePadding();
        coloredText(fonts.label, col::muted, "SİSMOGRAM GÖRÜNÜMÜ:");
        ImGui::PopFont();
        ImGui::SameLine(0, S(20));
        int vd = gv_.vd ? 1 : 0;
        if (ImGui::RadioButton("Wiggle", &vd, 0)) gv_.vd = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Variable Density", &vd, 1)) gv_.vd = true;
        ImGui::SameLine(0, S(20));
        ImGui::Checkbox("Pürüzsüz (Smooth VD)", &gv_.smooth);
        ImGui::SameLine(0, S(20));
        if (StyledButton("🔍 Pan/Zoom Sıfırla", hex(0x232b3b), hex(0x3d4a60), IM_COL32_WHITE)) gv_.reset();
    }
    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + rowH));

    // Y-axis (time) limit
    ImVec2 t0 = ImGui::GetCursorScreenPos();
    const float tH = S(46);
    dl->AddRectFilled(t0, ImVec2(t0.x + w, t0.y + tH), rgba(4, 16, 34, 0.7f), S(16), ImDrawFlags_RoundCornersBottom);
    dl->AddLine(t0, ImVec2(t0.x + w, t0.y), rgba(30, 144, 255, 0.3f));
    {
        const float rowW = std::min(w - S(32), S(620));
        ImGui::SetCursorScreenPos(ImVec2(t0.x + (w - rowW) * 0.5f, t0.y + (tH - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::BeginChild("##timeRow", ImVec2(rowW, ImGui::GetFrameHeight()), 0, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(hex(0x00e5a0)));
        float ms = (float)p.sismo_ms;
        if (ParamRow("⏱ Y-Ekseni (Zaman) Sınırı (ms):", "sismoT", &ms, 1000, 6000, 500, "%.0f")) p.sismo_ms = (int)std::lround(ms);
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }
    dl->AddRect(o, ImVec2(o.x + w, t0.y + tH), col::borderDim, S(16));
    ImGui::SetCursorScreenPos(ImVec2(t0.x, t0.y + tH));
    ImGui::Dummy(ImVec2(w, S(20)));
}

// ── Footer legend ─────────────────────────────────────────────────────────
void App::footerLegend() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(col::bgPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(20)));
    ImGui::BeginChild("##legend", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    const int cols = autoColumns(300, 4);
    if (ImGui::BeginTable("##legendGrid", cols, ImGuiTableFlags_SizingStretchSame)) {
        auto dotItem = [&](ImU32 c, const char* t, bool glow) {
            ImGui::TableNextColumn();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 ctr(p.x + S(5), p.y + ImGui::GetTextLineHeight() * 0.5f);
            if (glow) dl->AddCircleFilled(ctr, S(8), (c & 0x00FFFFFF) | 0x40000000);
            dl->AddCircleFilled(ctr, S(5), c);
            ImGui::SetCursorScreenPos(ImVec2(p.x + S(20), p.y));
            ImGui::TextWrapped("%s", t);
        };
        dotItem(hex(0xff4040), "Basınç - Pozitif Faz (Kırmızı)", true);
        dotItem(hex(0x4090ff), "Basınç - Negatif Faz (Mavi)", true);
        dotItem(hex(0x597ca3), "Tuz Domu (4500 m/s - Yüksek Hız)", false);
        ImGui::TableNextColumn();
        coloredText(fonts.uiBold, hex(0x00e5a0), "Fiziksel Blending Etkisi:");
        ImGui::TextWrapped("Sismik dalgalar lineer bir denklemdir. SLO'da her iki kaynak da tek bir ortama enerji yaydığı için "
                           "alan iç içe geçip fiziksel olarak basınç girişimine uğrar, sismogramdaki örtüşme de pürüzsüz toplanır.");
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ── Overlays ─────────────────────────────────────────────────────────────
void App::eikonalPopup() {
    if (!sim_.eikonalBusy()) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(30), vp->WorkPos.y + vp->WorkSize.y - S(30)), 0, ImVec2(1, 1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(rgba(13, 17, 23, 0.9f)));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(col::cyan));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::Begin("##eikonal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const int pct = sim_.eikonalProgress();
    ImGui::Text("Eikonal Çözücü:");
    ImGui::SameLine();
    char b[16]; std::snprintf(b, sizeof(b), "%d%%", pct);
    coloredText(fonts.ui, col::cyan, b);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float bw = S(180), bh = S(6);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), rgba(255, 255, 255, 0.1f), S(3));
    if (pct > 0) dl->AddRectFilledMultiColor(p, ImVec2(p.x + bw * pct / 100.0f, p.y + bh), col::blue, col::cyan, col::cyan, col::blue);
    ImGui::Dummy(ImVec2(bw, bh));
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void App::alertsModal() {
    if (!sim_.alerts.empty() && !ImGui::IsPopupOpen("##alert")) ImGui::OpenPopup("##alert");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.35f), 0, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    if (ImGui::BeginPopupModal("##alert", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        if (!sim_.alerts.empty()) {
            ImGui::PushTextWrapPos(S(460));
            ImGui::TextUnformatted(sim_.alerts.front().c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (StyledButton("Tamam", rgba(0, 132, 255, 0.4f), rgba(0, 132, 255, 0.6f), IM_COL32_WHITE, ImVec2(S(120), 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (!sim_.alerts.empty()) sim_.alerts.erase(sim_.alerts.begin());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

} // namespace ui
