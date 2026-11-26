#include <windows.h>
#include <glad/glad.h>
#include "ModelEditor.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "core/Material.hpp"

namespace ui {

namespace {

ImU32 rgbCol(uint32_t c, float a = 1.0f) { return hex(c, a); }

void heading(const char* t) {
    ImGui::Dummy(ImVec2(0, S(4)));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(q, ImVec2(q.x + ImGui::GetContentRegionAvail().x, q.y), col::rail);
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushFont(fonts.subhead);
    ImGui::TextColored(vec(col::textHi), "%s", t);
    ImGui::PopFont();
}

double niceStep(double range, int target) {
    const double raw = range / std::max(1, target);
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * p >= raw) return m * p;
    return 10 * p;
}

// Canvas raster size for a model: the longer side 800 px, square pixels
void dimsFor(float W, float D, int& w, int& h) {
    if (W >= D) { w = 800; h = std::clamp((int)std::lround(800.0 * D / W), 40, 800); }
    else { h = 800; w = std::clamp((int)std::lround(800.0 * W / D), 40, 800); }
}

const char* kToolNames[] = {"Sınır", "Çizgi", "Fırça", "Kova"};
const char* kToolTips[] = {"Katman sınırı: noktaları tıklayın; sınırın altı seçili katmanla boyanır.",
                           "Seçili katmanla kalın bir çizgi; kova dolgusunda bölgeleri ayırır.",
                           "Serbest boyama.",
                           "Tıklanan bölgeyi (aynı katmandan bitişik pikseller) doldurur."};
const char* kToolHelp[] = {"Soldan sağa noktaları tıklayın; çift tık, sağ tık ya da Enter bitirir, Esc vazgeçer. Sınır kenarlara "
                           "yatay uzatılır ve altı seçili katmanla boyanır.",
                           "Noktaları tıklayın; çift tık, sağ tık ya da Enter bitirir, Esc vazgeçer.",
                           "Basılı tutup sürükleyin.",
                           "Doldurulacak bölgeye tıklayın."};

} // namespace

void ModelEditor::open(Simulation& sim, double now) {
    if (!init_) {
        widthM_ = std::round(sim.p.domain_W);
        depthM_ = std::round(sim.p.domain_H * (1.0f - 0.166f));
        int w, h;
        dimsFor(widthM_, depthM_, w, h);
        cv_.initDefault(w, h);
        init_ = true;
    }
    wantOpen_ = true;
    openedAt_ = now;
    dirty_ = true;
}

void ModelEditor::upload() {
    if (!dirty_) return;
    dirty_ = false;
    rgba_.resize((size_t)cv_.w * cv_.h);
    for (size_t i = 0; i < rgba_.size(); i++) {
        const uint8_t k = cv_.px[i];
        const uint32_t c = k < cv_.palette.size() ? cv_.palette[k].rgb : 0xFF00FF;
        rgba_[i] = 0xFF000000u | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);   // ABGR in memory = RGBA bytes
    }
    if (!tex_) glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (texW_ != cv_.w || texH_ != cv_.h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cv_.w, cv_.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
        texW_ = cv_.w;
        texH_ = cv_.h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cv_.w, cv_.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
    }
}

void ModelEditor::resizeToModel() {
    int w, h;
    dimsFor(widthM_, depthM_, w, h);
    if (w == cv_.w && h == cv_.h) return;
    cv_.snapshot();
    cv_.resize(w, h);
    dirty_ = true;
}

void ModelEditor::finishPoints() {
    if (pts_.empty()) return;
    cv_.snapshot();
    if (tool_ == Boundary) cv_.fillBelow(pts_, (uint8_t)sel_);
    else cv_.polyline(pts_, radius_, (uint8_t)sel_);
    pts_.clear();
    dirty_ = true;
}

void ModelEditor::frame(Simulation& sim, double now, const std::function<void(const std::string&)>& notify,
                        const std::function<void(const std::string&)>& alert) {
    if (wantOpen_ && !ImGui::IsPopupOpen("##modelDraw")) {
        ImGui::OpenPopup("##modelDraw");
        wantOpen_ = false;
    }
    if (!ImGui::IsPopupOpen("##modelDraw")) return;
    sel_ = std::clamp(sel_, 0, (int)cv_.palette.size() - 1);
    upload();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float t = std::clamp((float)((now - openedAt_) / 0.16), 0.0f, 1.0f);
    const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const ImVec2 size(vp->WorkSize.x * 0.94f, vp->WorkSize.y * 0.92f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f + (1 - e) * S(8)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.01f, e));
    if (ImGui::BeginPopupModal("##modelDraw", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                            ImGuiWindowFlags_NoScrollbar)) {
        ImGui::PushFont(fonts.heading);
        ImGui::TextColored(vec(col::textHi), "Model çizimi");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6));
        ImGui::TextColored(vec(col::muted), "Katmanları çizip boyayın; her renk bir hızdır. Üst kenar deniz yüzeyi.");
        {   // Undo / redo, always in reach at the top right
            const char* undoL = "  Geri al";
            const char* redoL = "  İleri al";
            const float uw = ButtonWidth(undoL), rw = ButtonWidth(redoL), bh = S(34);
            const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            ImGui::SameLine();
            const float y0 = ImGui::GetCursorPosY() - S(10);
            ImGui::SetCursorPos(ImVec2(right - uw - rw - S(8), y0));
            ImGui::BeginDisabled(!cv_.canUndo());
            if (ToolButton(undoL, Btn::Secondary, ImVec2(uw, bh), "Son işlemi geri alır (Ctrl+Z). Son 40 adım saklanır.")) {
                pts_.clear();
                cv_.undo();
                dirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SetCursorPos(ImVec2(right - rw, y0));
            ImGui::BeginDisabled(!cv_.canRedo());
            if (ToolButton(redoL, Btn::Secondary, ImVec2(rw, bh), "Geri alınan işlemi yeniden yapar (Ctrl+Y).")) {
                cv_.redo();
                dirty_ = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float leftW = S(330), barH = S(56);
        ImGui::BeginChild("##drawTools", ImVec2(leftW, avail.y - barH), ImGuiChildFlags_None);
        tools(sim, notify, alert);
        ImGui::EndChild();
        ImGui::SameLine(0, S(18));
        canvasPane(ImVec2(avail.x - leftW - S(18), avail.y - barH));
        {   // Bottom bar: always visible
            const ImVec2 b0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(b0.x, b0.y + S(6)), ImVec2(b0.x + avail.x, b0.y + S(6)), col::bulkhead);
            const float bw = ButtonWidth("Modeli oluştur") + S(24), cw = ButtonWidth("Kapat") + S(16), by = b0.y + S(16);
            int used = 0;
            for (int c : cv_.counts()) used += c > 0;
            char b[160];
            std::snprintf(b, sizeof(b), "%d katman kullanılıyor  ·  %.0f × %.0f m", used, widthM_, depthM_);
            ImGui::GetWindowDrawList()->AddText(ImVec2(b0.x, by + (S(36) - ImGui::GetTextLineHeight()) * 0.5f), col::muted, b);
            ImGui::SetCursorScreenPos(ImVec2(b0.x + avail.x - bw - cw - S(10), by));
            if (ToolButton("Kapat", Btn::Secondary, ImVec2(cw, S(36)), "Çizim saklanır; yeniden açınca kaldığı yerden devam eder.")) close_ = true;
            ImGui::SetCursorScreenPos(ImVec2(b0.x + avail.x - bw, by));
            if (ToolButton("Modeli oluştur", Btn::Primary, ImVec2(bw, S(36)), "Çizimden hız modeli kurulur; yoğunluk hızdan (Gardner) hesaplanır.")) {
                std::string err;
                if (sim.loadModel(cv_.toModel(widthM_, depthM_, "Çizim"), GridModel{}, err)) {
                    std::snprintf(b, sizeof(b), "Çizilen model yüklendi: %d katman, %.0f × %.0f m.", used, widthM_, depthM_);
                    notify(b);
                    close_ = true;
                } else alert(err);
            }
        }
        if (close_) {
            close_ = false;
            pts_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void ModelEditor::tools(Simulation& sim, const std::function<void(const std::string&)>& notify,
                        const std::function<void(const std::string&)>& alert) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
    // Tools
    ImGui::PushFont(fonts.subhead);
    ImGui::TextColored(vec(col::textHi), "Araç");
    ImGui::PopFont();
    int tool = tool_;
    if (Segmented("##tool", kToolNames, 4, &tool, w, kToolTips)) { tool_ = tool; pts_.clear(); }
    Hint(kToolHelp[tool_]);
    if (tool_ == Line || tool_ == Brush) ParamRow("Kalınlık (piksel)", "drawR", &radius_, 1, 40, 1, "%.0f");

    // Layers
    heading("Katmanlar");
    Hint("Seçili katmanla çizilir. Renk kutusuna tıklayınca renk değişir; 1600 m/s ve altı su sayılır.");
    const std::vector<int> cnt = cv_.counts();
    const double total = (double)cv_.w * cv_.h;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)cv_.palette.size(); i++) {
        PaletteEntry& pe = cv_.palette[i];
        ImGui::PushID(i);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float rh = ImGui::GetFrameHeight() + S(8);
        const bool selected = i == sel_;
        // Row: click anywhere free to select it
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", ImVec2(w, rh))) sel_ = i;
        const bool hov = ImGui::IsItemHovered();
        if (selected || hov) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + rh), selected ? col::deck : withAlpha(col::deck, 0.5f), S(5));
        if (selected) dl->AddRectFilled(ImVec2(q.x, q.y + S(6)), ImVec2(q.x + S(3), q.y + rh - S(6)), col::accent, S(1.5f));
        const float fy = q.y + S(4);
        ImGui::SetCursorScreenPos(ImVec2(q.x + S(10), fy));
        float c3[3] = {((pe.rgb >> 16) & 0xFF) / 255.0f, ((pe.rgb >> 8) & 0xFF) / 255.0f, (pe.rgb & 0xFF) / 255.0f};
        if (ImGui::ColorEdit3("##col", c3, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoTooltip)) {
            pe.rgb = (uint32_t)std::lround(c3[0] * 255) << 16 | (uint32_t)std::lround(c3[1] * 255) << 8 | (uint32_t)std::lround(c3[2] * 255);
            dirty_ = true;
            sel_ = i;
        }
        ImGui::SameLine(0, S(8));
        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", pe.name.c_str());
        ImGui::SetNextItemWidth(w - S(10) - ImGui::GetFrameHeight() - S(8) - S(84) - S(52) - S(18));
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) pe.name = nameBuf;
        if (ImGui::IsItemActivated()) sel_ = i;
        ImGui::SameLine(0, S(6));
        ImGui::SetNextItemWidth(S(84));
        ImGui::PushFont(fonts.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
        ImGui::InputFloat("##v", &pe.velocity, 0, 0, "%.0f");
        if (ImGui::IsItemActivated()) sel_ = i;
        if (ImGui::IsItemDeactivatedAfterEdit()) pe.velocity = std::clamp(std::round(pe.velocity), 300.0f, 8000.0f);
        Tip("Hız (m/s), 300 – 8000");
        char pc[16];
        std::snprintf(pc, sizeof(pc), "%%%.0f", 100.0 * cnt[i] / total);
        const float pw = fonts.mono->CalcTextSizeA(fonts.mono->FontSize, FLT_MAX, 0, pc).x;
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(q.x + w - S(8) - pw, q.y + (rh - fonts.mono->FontSize) * 0.5f), col::muted, pc);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rh + S(2)));
        ImGui::Dummy(ImVec2(w, 0));
        ImGui::PopID();
    }
    const float half = (w - S(8)) * 0.5f;
    if (ToolButton("+ Katman ekle", Btn::Secondary, ImVec2(half, 0))) {
        float v = 2000;
        for (const PaletteEntry& pe : cv_.palette) v = std::max(v, pe.velocity + 300);
        v = std::min(v, 8000.0f);
        char n[32];
        std::snprintf(n, sizeof(n), "Katman %zu", cv_.palette.size() + 1);
        const int k = cv_.addEntry({n, v, colourForVelocity(v)});
        if (k >= 0) sel_ = k;
    }
    ImGui::SameLine(0, S(8));
    ImGui::BeginDisabled(cv_.palette.size() < 2);
    if (ToolButton("Seçiliyi sil", Btn::Ghost, ImVec2(half, 0), "Seçili katmanın pikselleri bir üstteki katmana geçer.")) {
        cv_.removeEntry(sel_, sel_ > 0 ? sel_ - 1 : 1);
        sel_ = std::max(0, sel_ - 1);
        dirty_ = true;
    }
    ImGui::EndDisabled();

    // Size
    heading("Model boyutu");
    auto field = [&](const char* label, const char* id, float* v, float lo, float hi) {
        const ImVec2 p = ImGui::GetCursorPos();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(p.x + w - S(110), p.y));
        ImGui::SetNextItemWidth(S(110));
        ImGui::PushFont(fonts.mono);
        ImGui::InputFloat(id, v, 0, 0, "%.0f");
        ImGui::PopFont();
        if (ImGui::IsItemDeactivatedAfterEdit()) { *v = std::clamp(std::round(*v), lo, hi); resizeToModel(); }
    };
    field("Genişlik (m)", "##mw", &widthM_, 100, 25000);
    field("Derinlik (m)", "##md", &depthM_, 50, 12000);
    char b[160];
    std::snprintf(b, sizeof(b), "%d × %d piksel, piksel %.2f × %.2f m", cv_.w, cv_.h, widthM_ / (cv_.w - 1), depthM_ / (cv_.h - 1));
    Hint(b);

    // Editing
    heading("Düzen");
    if (ToolButton("Tümünü seçili katmanla doldur", Btn::Ghost, ImVec2(w, 0))) { cv_.snapshot(); cv_.fillAll((uint8_t)sel_); dirty_ = true; }
    if (ToolButton("Mevcut modeli aç", Btn::Ghost, ImVec2(w, 0),
                   "Simülasyonun şu anki hız modelini (deniz yüzeyinden aşağı) çizime aktarır; her farklı hız bir katman olur.")) {
        const int j0 = std::clamp((int)std::ceil(sim.OCEAN_Y / sim.SCALE - 1e-9), 0, sim.H_sim - 2);
        const int nx = sim.W_sim, nz = sim.H_sim - j0;
        std::vector<float> rows((size_t)nx * nz);
        for (int k = 0; k < nz; k++)
            for (int i = 0; i < nx; i++) rows[(size_t)k * nx + i] = (float)(std::sqrt((double)sim.vsq[(size_t)(j0 + k) * sim.W_sim + i]) * kVelocityNorm);
        widthM_ = std::round((float)((nx - 1) * sim.SCALE));
        depthM_ = std::round((float)((nz - 1) * sim.SCALE));
        int cw, ch;
        dimsFor(widthM_, depthM_, cw, ch);
        cv_.fromVelocity(rows.data(), nx, nz, cw, ch);
        sel_ = std::min(1, (int)cv_.palette.size() - 1);
        pts_.clear();
        dirty_ = true;
    }

    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PopStyleVar();
}

void ModelEditor::canvasPane(ImVec2 size) {
    ImGui::BeginChild("##drawCanvas", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float L = S(64), T = S(34), B = S(40), R = S(28);
    const float aw = size.x - L - R, ah = size.y - T - B;
    // Fit with square pixels
    float dw = aw, dh = aw * cv_.h / (float)cv_.w;
    if (dh > ah) { dh = ah; dw = ah * cv_.w / (float)cv_.h; }
    const ImVec2 c0(o.x + L + (aw - dw) * 0.5f, o.y + T), c1(c0.x + dw, c0.y + dh);
    dl->AddRectFilled(ImVec2(c0.x - 1, c0.y - 1), ImVec2(c1.x + 1, c1.y + 1), col::abyss);
    dl->AddImage((ImTextureID)(intptr_t)tex_, c0, c1);
    dl->AddRect(ImVec2(c0.x - 1, c0.y - 1), ImVec2(c1.x + 1, c1.y + 1), col::rail);

    // Axes: distance on top, depth on the left
    ImFont* f = fonts.mono;
    const double xs = niceStep(widthM_, std::max(3, (int)(dw / S(90))));
    for (double x = 0; x <= widthM_ + 1e-6; x += xs) {
        const float sx = c0.x + (float)(x / widthM_) * dw;
        dl->AddLine(ImVec2(sx, c0.y - S(6)), ImVec2(sx, c0.y - 1), col::muted);
        char b[24];
        std::snprintf(b, sizeof(b), "%.0f", x);
        const float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, b).x;
        dl->AddText(f, f->FontSize, ImVec2(sx - tw * 0.5f, c0.y - S(8) - f->FontSize), col::text, b);
    }
    const double zs = niceStep(depthM_, std::max(3, (int)(dh / S(60))));
    for (double z = 0; z <= depthM_ + 1e-6; z += zs) {
        const float sy = c0.y + (float)(z / depthM_) * dh;
        dl->AddLine(ImVec2(c0.x - S(6), sy), ImVec2(c0.x - 1, sy), col::muted);
        char b[24];
        std::snprintf(b, sizeof(b), "%.0f", z);
        const float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, b).x;
        dl->AddText(f, f->FontSize, ImVec2(c0.x - S(10) - tw, sy - f->FontSize * 0.5f), col::text, b);
    }
    dl->AddText(fonts.label, fonts.label->FontSize, ImVec2(o.x, o.y), col::muted, "x (m) / derinlik (m)");

    // Input
    ImGui::SetCursorScreenPos(c0);
    ImGui::InvisibleButton("##canvas", ImVec2(dw, dh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const float kx = cv_.w / dw, ky = cv_.h / dh;
    const float mx = (io.MousePos.x - c0.x) * kx, my = (io.MousePos.y - c0.y) * ky;
    auto toScreen = [&](float x, float y) { return ImVec2(c0.x + x / kx, c0.y + y / ky); };
    const uint8_t sel = (uint8_t)sel_;
    if (tool_ == Brush) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            cv_.snapshot();
            painting_ = true;
            cv_.brush(mx, my, radius_, sel);
            lastX_ = mx; lastY_ = my;
            dirty_ = true;
        }
        if (painting_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mx != lastX_ || my != lastY_) { cv_.line(lastX_, lastY_, mx, my, radius_, sel); lastX_ = mx; lastY_ = my; dirty_ = true; }
        } else painting_ = false;
        if (hov) dl->AddCircle(io.MousePos, radius_ / kx, IM_COL32(255, 255, 255, 200), 32, 1.5f);
    } else if (tool_ == Bucket) {
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            cv_.snapshot();
            if (cv_.floodFill((int)mx, (int)my, sel) == 0) cv_.undo();
            dirty_ = true;
        }
    } else {   // boundary / line: collect points
        if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) finishPoints();
        else if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pts_.push_back({std::clamp(mx, 0.0f, (float)cv_.w), std::clamp(my, 0.0f, (float)cv_.h)});
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) finishPoints();
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter)) finishPoints();
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) pts_.clear();
        if (!pts_.empty()) {   // preview: the line so far, a rubber band to the mouse
            const ImU32 lc = col::accent;
            std::vector<ImVec2> sp;
            for (const auto& p : pts_) sp.push_back(toScreen(p.first, p.second));
            if (tool_ == Boundary) {   // flat extensions to the edges, as the fill will use them
                auto sorted = pts_;
                std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
                const ImVec2 l = toScreen(0, sorted.front().second), r = toScreen((float)cv_.w, sorted.back().second);
                dl->AddLine(l, toScreen(sorted.front().first, sorted.front().second), withAlpha(lc, 0.5f), 1.5f);
                dl->AddLine(toScreen(sorted.back().first, sorted.back().second), r, withAlpha(lc, 0.5f), 1.5f);
                sp.clear();
                for (const auto& p : sorted) sp.push_back(toScreen(p.first, p.second));
            }
            dl->AddPolyline(sp.data(), (int)sp.size(), lc, 0, S(2));
            if (hov) dl->AddLine(sp.back(), io.MousePos, withAlpha(lc, 0.6f), 1.5f);
            for (const ImVec2& p : sp) dl->AddCircleFilled(p, S(3.5f), lc, 12);
        }
    }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { pts_.clear(); if (cv_.undo()) dirty_ = true; }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) { if (cv_.redo()) dirty_ = true; }

    // Status line: position, layer under the cursor, what the tool does
    char b[200];
    if (hov && cv_.inside((int)mx, (int)my)) {
        const PaletteEntry& pe = cv_.palette[std::min<size_t>(cv_.at((int)mx, (int)my), cv_.palette.size() - 1)];
        std::snprintf(b, sizeof(b), "x %.0f m, derinlik %.0f m  ·  %s, %.0f m/s", mx / cv_.w * widthM_, my / cv_.h * depthM_, pe.name.c_str(),
                      pe.velocity);
    } else std::snprintf(b, sizeof(b), "%s", kToolHelp[tool_]);
    dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(c0.x, c1.y + S(12)), col::text, b);
    if (!pts_.empty()) {
        std::snprintf(b, sizeof(b), "%zu nokta", pts_.size());
        const float tw = fonts.mono->CalcTextSizeA(fonts.mono->FontSize, FLT_MAX, 0, b).x;
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(c1.x - tw, c1.y + S(16) + fonts.ui->FontSize), col::accent, b);
    }
    ImGui::EndChild();
}

} // namespace ui
