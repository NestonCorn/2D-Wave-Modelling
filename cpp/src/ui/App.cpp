#include "App.hpp"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <portable-file-dialogs.h>
#include "core/ParamsIO.hpp"

namespace ui {

namespace {

// Strong ease-out (close to cubic-bezier(0.23, 1, 0.32, 1)): fast start, gentle settle
float easeOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u;
}

Mode modeOf(int i) { return i == 1 ? Mode::SLO : i == 2 ? Mode::CLO : Mode::Trad; }
int indexOf(Mode m) { return m == Mode::SLO ? 1 : m == Mode::CLO ? 2 : 0; }

std::string lowerExt(const std::string& path) {
    const std::u8string u = std::filesystem::u8path(path).extension().u8string();
    std::string e(u.begin(), u.end());
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}
std::string baseName(const std::string& path) {
    const std::u8string u = std::filesystem::u8path(path).filename().u8string();
    return std::string(u.begin(), u.end());
}

void caption(const char* text) {   // heading of a block inside a group, a rule above it
    ImGui::Dummy(ImVec2(0, S(6)));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(q, ImVec2(q.x + ImGui::GetContentRegionAvail().x, q.y), col::rail);
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PushFont(fonts.subhead);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Status colour for a value against two thresholds
ImU32 levelColor(int level) { return level == 0 ? col::ok : level == 1 ? col::warn : col::danger; }

void vrule(ImDrawList* dl, float x, float y0, float y1) { dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col::bulkhead); }

// Small seismic-trace mark for the title bar: one wiggle, the trace the whole app is about
void traceMark(ImDrawList* dl, ImVec2 c, float h) {
    ImVec2 pts[24];
    for (int i = 0; i < 24; i++) {
        const float t = (float)i / 23.0f;
        const float y = (t - 0.5f) * h;
        const float x = std::sin(t * 9.4f) * std::exp(-std::pow((t - 0.5f) * 4.0f, 2.0f)) * h * 0.38f;
        pts[i] = ImVec2(c.x + x, c.y + y);
    }
    dl->AddLine(ImVec2(c.x, c.y - h * 0.5f), ImVec2(c.x, c.y + h * 0.5f), withAlpha(col::muted, 0.5f), 1.0f);
    dl->AddPolyline(pts, 24, col::accent, 0, S(2.0f));
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════
void App::frame(double now) {
    now_ = now;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float titleH = S(44), cmdH = S(60), statusH = S(30);
    const float bodyY = o.y + titleH + cmdH;
    const float bodyH = std::max(S(240), H - titleH - cmdH - statusH);
    sideW_ = std::clamp(sideW_, 300.0f, std::max(300.0f, std::min(560.0f, W / dpi * 0.4f)));
    const float railW = S(56);
    const float sw = panelOpen_ ? S(sideW_) : 0.0f, gutter = panelOpen_ ? S(5) : 0.0f;

    titleBar(o, ImVec2(W, titleH));
    commandBar(ImVec2(o.x, o.y + titleH), ImVec2(W, cmdH));
    iconRail(ImVec2(o.x, bodyY), ImVec2(railW, bodyH));
    if (panelOpen_) {
        sidePanel(ImVec2(o.x + railW, bodyY), ImVec2(sw, bodyH), now);
        // panel / views splitter
        const float sx = o.x + railW + sw;
        ImGui::SetCursorScreenPos(ImVec2(sx, bodyY));
        ImGui::InvisibleButton("##sideSplit", ImVec2(gutter, bodyH));
        const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (act) sideW_ += ImGui::GetIO().MouseDelta.x / dpi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(sx, bodyY), ImVec2(sx + gutter, bodyY + bodyH), col::abyss);
        dl->AddLine(ImVec2(sx + gutter * 0.5f, bodyY), ImVec2(sx + gutter * 0.5f, bodyY + bodyH),
                    act ? col::accent : hov ? col::rail : col::bulkhead, act || hov ? S(2) : 1.0f);
    }
    const float vx = o.x + railW + sw + gutter;
    views(ImVec2(vx, bodyY), ImVec2(o.x + W - vx, bodyH));
    statusBar(ImVec2(o.x, bodyY + bodyH), ImVec2(W, statusH));
    toasts(ImVec2(o.x + W - S(16), bodyY + bodyH - S(14)));

    modelImportPopup();
    editor_.frame(sim_, now, [this](const std::string& m) { notify(m); }, [this](const std::string& m) { alert(m); });
    alertsModal();
    ImGui::End();
    keyboard();
    applyDeferred();
}

void App::applyDeferred() {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (pendingNpml_) { pendingNpml_ = false; sim_.onNpml(); }
    if (pendingObs_) { pendingObs_ = false; sim_.onObsSpacing(); }
}

void App::keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    // Keyboard actions are instant: no animation on anything they trigger
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) startOrPause();
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) view_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) view_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) view_ = 2;
    }
}

void App::startOrPause() {
    if (sim_.lineActive()) return;
    if (sim_.mode == Mode::None) sim_.startMode(modeOf(selMode_));
    else sim_.togglePause();
}

// ── Title bar ─────────────────────────────────────────────────────────────
void App::titleBar(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    dl->AddLine(ImVec2(o.x, o.y + size.y - 1), ImVec2(o.x + size.x, o.y + size.y - 1), col::bulkhead);
    const float cy = o.y + size.y * 0.5f;
    traceMark(dl, ImVec2(o.x + S(26), cy), S(24));
    float x = o.x + S(44);
    const float th = fonts.title->FontSize;
    dl->AddText(fonts.title, th, ImVec2(x, cy - th * 0.55f), col::textHi, "Seismic FDTD");
    x += fonts.title->CalcTextSizeA(th, FLT_MAX, 0, "Seismic FDTD").x + S(14);
    const char* sub = "2B akustik dalga modelleme ve veri toplama";
    dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x, cy - fonts.ui->FontSize * 0.5f), col::muted, sub);
    x += fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, sub).x;
    if (sim_.p.geology == GeologyModel::Imported && sim_.hasModel()) {
        const std::string m = "Model: " + sim_.modelVelocity().source;
        x += S(14);
        vrule(dl, x, cy - S(9), cy + S(9));
        x += S(14);
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x, cy - fonts.ui->FontSize * 0.5f), col::text, m.c_str());
    }

    // Compute engine (click to switch between GPU and CPU)
    const Badge& b = sim_.badge;
    std::string label;
    ImU32 dot = col::muted;
    switch (b.kind) {
        case BadgeKind::Gpu: label = "GPU: " + (b.label.empty() ? std::string("hazır") : b.label); dot = col::ok; break;
        case BadgeKind::ForcedCpu: label = "CPU (elle seçildi)"; dot = col::data; break;
        case BadgeKind::Warning: label = b.label; dot = col::warn; break;
        case BadgeKind::Cpu: label = "CPU"; dot = col::warn; break;
        default: label = "Başlatılıyor"; break;
    }
    const ImVec2 ts = fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, label.c_str());
    const float bw = ts.x + S(34), bh = S(28);
    const ImVec2 b0(o.x + size.x - S(14) - bw, cy - bh * 0.5f);
    ImGui::SetCursorScreenPos(b0);
    ImGui::InvisibleButton("##engine", ImVec2(bw, bh));
    const bool hov = ImGui::IsItemHovered();
    std::string tip = b.reason.empty() ? std::string() : b.reason + "\n\n";
    tip += "Tıklayınca hesaplama GPU ile CPU arasında değişir; çalışan simülasyon yeniden başlar.";
    Tip(tip.c_str());
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked() && !sim_.lineActive()) sim_.toggleComputeEngine();
    dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), hov ? col::deck : col::hull, S(4));
    dl->AddRect(b0, ImVec2(b0.x + bw, b0.y + bh), hov ? col::rail : col::bulkhead, S(4));
    dl->AddCircleFilled(ImVec2(b0.x + S(13), cy), S(3.5f), dot, 12);
    dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(b0.x + S(24), cy - ts.y * 0.5f), col::text, label.c_str());
}

// ── Command bar ───────────────────────────────────────────────────────────
void App::commandBar(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    dl->AddLine(ImVec2(o.x, o.y + size.y - 1), ImVec2(o.x + size.x, o.y + size.y - 1), col::bulkhead);
    const float bh = S(36), y = o.y + (size.y - bh) * 0.5f, gap = S(12);
    float x = o.x + S(20);
    auto at = [&](float px) { ImGui::SetCursorScreenPos(ImVec2(px, y)); };
    auto next = [&](float extra = 0) { x = ImGui::GetItemRectMax().x + gap + extra; };
    auto rule = [&]() { vrule(dl, x + S(8), y + S(6), y + bh - S(6)); x += S(28); };

    if (sim_.lineActive()) {
        // A line simulation drives the modes and the ship: its progress replaces the controls
        char b[96];
        std::snprintf(b, sizeof(b), "Hat simülasyonu: atış %d / %d", std::min(sim_.lineShotsDone() + 1, sim_.lineShots()),
                      sim_.lineShots());
        const ImVec2 ts = fonts.uiBold->CalcTextSizeA(fonts.uiBold->FontSize, FLT_MAX, 0, b);
        dl->AddText(fonts.uiBold, fonts.uiBold->FontSize, ImVec2(x, y + (bh - ts.y) * 0.5f), col::textHi, b);
        x += ts.x + S(16);
        const float stopW = ButtonWidth("Durdur");
        const float barW = std::max(S(120), size.x * 0.4f);
        const ImVec2 p0(x, y + bh * 0.5f - S(3)), p1(x + barW, y + bh * 0.5f + S(3));
        dl->AddRectFilled(p0, p1, col::deck, S(3));
        const float pr = (float)sim_.lineProgress();
        dl->AddRectFilled(p0, ImVec2(p0.x + barW * pr, p1.y), col::accent, S(3));
        char pc[16];
        std::snprintf(pc, sizeof(pc), "%%%d", (int)std::lround(pr * 100));
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(p1.x + S(10), y + (bh - fonts.mono->FontSize) * 0.5f), col::text, pc);
        x = p1.x + S(60);
        at(x);
        if (ToolButton("Durdur", Btn::Danger, ImVec2(stopW, bh), "Hattı burada durdurur; o ana kadarki atışlar dosyada kalır."))
            sim_.cancelLine();
        next(S(6));
        const std::string folder = "Klasör: " + baseName(sim_.lineFolder());
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x, y + (bh - fonts.ui->FontSize) * 0.5f), col::muted, folder.c_str());
        return;
    }

    // Mode: Start runs the selected one; picking another while running switches to it
    const bool running = sim_.mode != Mode::None;
    int cur = running ? indexOf(sim_.mode) : selMode_;
    static const char* modes[] = {"Geleneksel", "SLO", "CLO"};
    static const char* modeTips[] = {"Tek gemi, tek kaynak.",
                                     "Eşzamanlı atış: iki kaynak aynı kayıtta (karışık veri).",
                                     "Ardışık atış: önce ana gemi, sonra kaynak gemisi."};
    at(x);
    if (Segmented("##mode", modes, 3, &cur, S(300), modeTips, bh)) {
        selMode_ = cur;
        if (running) sim_.startMode(modeOf(cur));
    }
    next(S(4));
    at(x);
    const char* runLabel = !running ? "  Başlat" : sim_.paused ? "  Devam et" : "  Duraklat";
    const float runW = ButtonWidth("  Devam et");
    if (ToolButton(runLabel, Btn::Primary, ImVec2(runW, bh), "Boşluk tuşu")) startOrPause();
    next();
    at(x);
    ImGui::BeginDisabled(!running);
    if (ToolButton("Sıfırla", Btn::Secondary, ImVec2(0, bh), "Simülasyonu durdurur; dalga alanını ve kayıtları temizler."))
        sim_.stopMode();
    ImGui::EndDisabled();
    next();
    rule();

    // Record length (also the time axis of the recordings)
    {
        const char* lbl = "Kayıt uzunluğu";
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(x, y + (bh - ts.y) * 0.5f), col::text, lbl);
        x += ts.x + S(8);
        ImGui::SetCursorScreenPos(ImVec2(x, y + (bh - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(S(72));
        ImGui::PushFont(fonts.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
        int ms = sim_.p.sismo_ms;
        ImGui::InputInt("##recLen", &ms, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()) sim_.p.sismo_ms = std::clamp(ms, 500, 10000);
        Tip("Her atışın kayıt süresi (500 – 10000 ms). Kayıtların zaman ekseni de budur.");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        x = ImGui::GetItemRectMax().x + S(6);
        dl->AddText(ImVec2(x, y + (bh - ts.y) * 0.5f), col::muted, "ms");
        x += ImGui::CalcTextSize("ms").x + gap;
    }
    rule();
    at(x);
    toggleSegy();
}

void App::toggleSegy() {
    const bool rec = sim_.segyRecording();
    char lbl[96];
    if (rec) std::snprintf(lbl, sizeof(lbl), "  Kaydı durdur (%d atış)###segy", sim_.segyShotsSaved());
    else std::snprintf(lbl, sizeof(lbl), "  SEG-Y'ye kaydet###segy");
    const bool idle = sim_.mode == Mode::None;
    ImGui::BeginDisabled(!rec && idle);
    const bool clicked = ToolButton(lbl, rec ? Btn::Danger : Btn::Secondary, ImVec2(0, S(36)),
                                    idle && !rec ? "Önce bir kip başlatın; her atış kayıt uzunluğuna ulaştığında ayrı SEG-Y dosyalarına yazılır."
                                                 : "Her atış kayıt uzunluğuna ulaştığında streamer ve OBS kayıtlarını ayrı SEG-Y dosyalarına yazar.");
    ImGui::EndDisabled();
    if (!clicked) return;
    if (rec) {
        const int n = sim_.segyShotsSaved();
        sim_.segyStop(now_);
        notify("SEG-Y kaydı durduruldu: " + std::to_string(n) + " atış kaydedildi.");
        return;
    }
    std::string dir = pfd::select_folder("SEG-Y dosyalarının kaydedileceği klasörü seçin").result();
    if (!dir.empty()) sim_.segyStart(dir);
}

// ── Parameter groups: icon rail + one group at a time ──────────────────────
namespace {
struct GroupInfo { const char* icon; const char* name; const char* blurb; };
// Segoe Fluent / MDL2 glyphs
const GroupInfo kGroups[] = {
    {"", "Kaynak ve dalgacık", "Kaynağın yaydığı darbe"},
    {"", "Ortam ve yeraltı modeli", "Hız, yoğunluk ve alan boyutu"},
    {"", "Saha geometrisi", "Kablo, alıcılar ve gemiler"},
    {"", "Deniz tabanı alıcıları (OBS)", "Deniz tabanına yerleşen alıcılar"},
    {"", "Sayısal çözüm", "Izgara, zaman adımı ve kararlılık"},
    {"", "Görünüm", "Kazanç ve oynatma hızı"},
    {"", "Hat simülasyonu", "Bir hattın bütün atışlarını SEG-Y'ye yazar"},
    {"", "Parametre dosyası", "Ayarları kaydet ve aç"},
};
constexpr int kGroupCount = (int)(sizeof(kGroups) / sizeof(kGroups[0]));
} // namespace

void App::iconRail(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    dl->AddLine(ImVec2(o.x + size.x - 1, o.y), ImVec2(o.x + size.x - 1, o.y + size.y), col::bulkhead);
    const float cell = S(48), btn = S(40);
    const float bx = o.x + (size.x - btn) * 0.5f;
    // The accent bar slides to the selected group (interruptible ease-out)
    const float barY = Animate(ImGui::GetID("##railBar"), o.y + S(10) + group_ * cell, 180);
    for (int i = 0; i < kGroupCount; i++) {
        const ImVec2 a(bx, o.y + S(10) + i * cell), b(a.x + btn, a.y + btn);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##grp", ImVec2(btn, btn));
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const bool sel = i == group_ && panelOpen_;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        char tip[160];
        std::snprintf(tip, sizeof(tip), "%s\n%s", kGroups[i].name, sel ? "Tıklayınca panel gizlenir." : kGroups[i].blurb);
        Tip(tip);
        if (clicked) {
            if (sel) panelOpen_ = false;
            else { group_ = i; panelOpen_ = true; }
        }
        const float in = held ? S(1) : 0.0f;   // pressed: shrinks a little
        const ImVec2 A(a.x + in, a.y + in), B(b.x - in, b.y - in);
        if (sel) dl->AddRectFilled(A, B, col::deck, S(6));
        else if (hov) dl->AddRectFilled(A, B, withAlpha(col::deck, 0.6f), S(6));
        const float fs = S(19);
        const ImVec2 ts = fonts.icon->CalcTextSizeA(fs, FLT_MAX, 0, kGroups[i].icon);
        dl->AddText(fonts.icon, fs, ImVec2(a.x + (btn - ts.x) * 0.5f, a.y + (btn - ts.y) * 0.5f),
                    sel ? col::textHi : hov ? col::text : col::muted, kGroups[i].icon);
    }
    if (panelOpen_)
        dl->AddRectFilled(ImVec2(o.x, barY + S(9)), ImVec2(o.x + S(3), barY + btn - S(9)), col::accent, S(1.5f));
}

void App::sidePanel(ImVec2 o, ImVec2 size, double now) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    // Group header
    const GroupInfo& g = kGroups[std::clamp(group_, 0, kGroupCount - 1)];
    const float hH = S(80), px = S(22);
    dl->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(o.x + px, o.y + S(13)), col::textHi, g.name);
    dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(o.x + px, o.y + S(13) + fonts.heading->FontSize + S(4)), col::muted, g.blurb);
    dl->AddLine(ImVec2(o.x, o.y + hH), ImVec2(o.x + size.x, o.y + hH), col::bulkhead);

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + hH + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px, S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
    ImGui::BeginChild("##side", ImVec2(size.x, size.y - hH - 1), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::BeginDisabled(sim_.lineActive());
    switch (group_) {
        case 0: sectionSource(); break;
        case 1: sectionMedium(now); break;
        case 2: sectionGeometry(); break;
        case 3: sectionObs(); break;
        case 4: sectionNumerics(now); sectionCpu(); break;
        case 5: sectionView(); break;
        case 6: sectionLine(); break;
        default: sectionFiles(); break;
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(12)));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ── View tabs ─────────────────────────────────────────────────────────────
void App::viewTabs(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    dl->AddLine(ImVec2(o.x, o.y + size.y - 1), ImVec2(o.x + size.x, o.y + size.y - 1), col::bulkhead);
    static const char* names[] = {"Model", "Kayıtlar", "Yan yana"};
    static const char* tips[] = {"Model ve dalga alanı bütün alanı kaplar.", "Streamer ve OBS kayıtları bütün alanı kaplar.",
                                 "Model üstte, kayıtlar altta."};
    ImFont* f = fonts.subhead;
    float x = o.x + S(10);
    float ux0 = 0, ux1 = 0;
    for (int i = 0; i < 3; i++) {
        const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, names[i]);
        const float w = ts.x + S(32);
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##view", ImVec2(w, size.y - 1))) view_ = i;
        ImGui::PopID();
        Tip(tips[i]);
        const bool hov = ImGui::IsItemHovered(), sel = view_ == i;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddText(f, f->FontSize, ImVec2(x + S(16), o.y + (size.y - ts.y) * 0.5f), sel ? col::textHi : hov ? col::text : col::muted,
                    names[i]);
        if (sel) { ux0 = x + S(10); ux1 = x + w - S(10); }
        x += w;
    }
    // Underline glides between tabs
    const float a0 = Animate(ImGui::GetID("##tabU0"), ux0, 160), a1 = Animate(ImGui::GetID("##tabU1"), ux1, 160);
    dl->AddRectFilled(ImVec2(a0, o.y + size.y - S(3)), ImVec2(a1, o.y + size.y - 1), col::accent, S(1));
    // Keyboard hint on the right
    const char* hint = "Ctrl+1 / 2 / 3";
    const ImVec2 hs = fonts.mono->CalcTextSizeA(fonts.mono->FontSize, FLT_MAX, 0, hint);
    if (o.x + size.x - S(16) - hs.x > x + S(20))
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(o.x + size.x - S(16) - hs.x, o.y + (size.y - hs.y) * 0.5f), col::muted, hint);
}

void App::sectionSource() {
    SimParams& p = sim_.p;
    static const char* wav[] = {"Ricker (sıfır faz)", "Minimum faz", "Gauss", "Gauss 1. türevi"};
    int w = (int)p.wavelet;
    if (ComboRow("Dalgacık", "##wavelet", &w, wav, 4, S(170))) p.wavelet = (Wavelet)w;
    Tip("Kaynağın yaydığı darbenin biçimi. Ricker ve Gauss türevi sıfır alanlıdır; Gauss tek tepeli bir darbedir.");
    ParamRow("Baskın frekans (Hz)", "freq", &p.freq, 0.5f, 250.0f, 0.5f, "%.1f");
    ParamRow("Kaynak genliği", "amp", &p.amp, 0.2f, 20.0f, 0.2f, "%.1f");
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionMedium(double now) {
    SimParams& p = sim_.p;
    static const char* models[] = {"Düz katmanlar", "Fay", "Tuz domu", "Dosyadan yüklenen"};
    int g = (int)p.geology;
    if (ComboRow("Hız modeli", "##geoModel", &g, models, sim_.hasModel() ? 4 : 3, S(170))) {
        p.geology = (GeologyModel)g;
        sim_.onGeology();
    }
    bool fs = p.free_surface;
    if (Switch("Deniz yüzeyi yansıması", &fs,
               "Açıkken deniz yüzeyi serbest yüzey (P = 0) olarak yansıtır. Kapalıyken yüzeyin üstünde de soğurucu sınır vardır."))
        sim_.toggleFreeSurface();
    bool avg = p.cell_average;
    if (Switch("Hücre ortalaması", &avg,
               "Her ızgara hücresi, içine düşen model parçalarının ortalamasını alır: eğimli ve kavisli sınırlar ızgarada "
               "basamak gibi davranıp saçılma üretmez. Kapalıyken her hücre düğüm noktasındaki değeri alır.")) {
        p.cell_average = avg;
        sim_.onGeology();
    }
    bool sb = p.smooth_boundaries;
    if (Switch("Sınırları basamaksız yap", &sb,
               "Görselden, çizimden ya da az sayıda hızdan oluşan dosyadan yüklenen modelde katman sınırları piksel basamakları "
               "olmadan çözülür. Az eğimli bir sınır piksellerde düzenli basamaklara dönüşür ve bunlar kayıtta birbirini kesen "
               "yapay eğimli olaylar üretir. Kaydedilen model değişmez.")) {
        p.smooth_boundaries = sb;
        sim_.onSmoothBoundaries();
    }
    if (p.smooth_boundaries && p.geology == GeologyModel::Imported) {
        const BoundaryStats& st = sim_.smoothStats();
        char sbuf[160];
        if (!st.applicable) Hint("Yüklü modelde çok sayıda farklı hız var (gradyan); sınırlar olduğu gibi kullanılıyor.");
        else {
            std::snprintf(sbuf, sizeof(sbuf), "%d sınır basamaksız çizildi (%zu piksel).", st.boundaries, st.pixels);
            Hint(sbuf);
        }
    }
    ImGui::BeginDisabled(p.geology == GeologyModel::Imported);   // a loaded model sets the domain
    if (ParamRow("Alan genişliği (m)", "uiW", &p.domain_W, 800, 25000, 100, "%.0f")) sim_.onDomainWidth(now);
    if (ParamRow("Alan derinliği (m)", "uiH", &p.domain_H, 400, 12000, 40, "%.0f")) sim_.onDomainHeight(now);
    ImGui::EndDisabled();

    // Velocities, with the colours the scene draws them in
    caption("Hızlar");
    auto swatchRow = [&](ImU32 c, const char* name, const char* value) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x, q.y + h * 0.2f), ImVec2(q.x + h * 0.6f, q.y + h * 0.8f), c, S(2));
        ImGui::GetWindowDrawList()->AddRect(ImVec2(q.x, q.y + h * 0.2f), ImVec2(q.x + h * 0.6f, q.y + h * 0.8f), col::rail, S(2));
        ImGui::SetCursorScreenPos(ImVec2(q.x + h * 0.6f + S(8), q.y));
        ImGui::TextUnformatted(name);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y));
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(fonts.mono);
        const float vw = ImGui::CalcTextSize(value).x;
        ImGui::SetCursorScreenPos(ImVec2(q.x + avail - vw - S(6), q.y));
        ImGui::TextColored(vec(col::textHi), "%s", value);
        ImGui::PopFont();
    };
    if (p.geology == GeologyModel::Imported && sim_.hasModel()) {
        char b[64];
        const auto vr = sim_.modelVelocityRange();
        std::snprintf(b, sizeof(b), "%.0f – %.0f m/s", vr.first, vr.second);
        ReadoutRow("Aralık", b);
    } else {
        swatchRow(hex(0x112538), "Su", "1500 m/s");
        swatchRow(hex(0x1A3247), "Tortul", "2100 m/s");
        swatchRow(hex(0x22394B), "Temel kaya", "2880 m/s");
        if (p.geology == GeologyModel::Salt) swatchRow(hex(0xA9B8C6), "Tuz", "4080 m/s");
    }
    ReadoutRow("Yoğunluk", !sim_.modelDensity().empty() && p.geology == GeologyModel::Imported ? "dosyadan" : "Gardner (hızdan)");
    Tip("Yoğunluk modeli verilmezse ρ = 0.31·V^0.25 (Gardner), su için 1.03 g/cm³.");

    // Model files
    caption("Model dosyası");
    const float w = ImGui::GetContentRegionAvail().x;
    if (sim_.hasModel()) {
        const GridModel& v = sim_.modelVelocity();
        char b[160];
        std::snprintf(b, sizeof(b), "%s\n%d x %d örnek, dx %g m, dz %g m", v.source.c_str(), v.nx, v.nz, v.dx, v.dz);
        Hint(b, col::text);
        const GridModel& r = sim_.modelDensity();
        if (!r.empty()) {
            const auto rr = sim_.modelDensityRange();
            std::snprintf(b, sizeof(b), "Yoğunluk: %s (%.2f – %.2f g/cm³)", r.source.c_str(), rr.first, rr.second);
            Hint(b, col::text);
        }
        if (p.geology != GeologyModel::Imported) Hint("Şu an hazır bir model seçili; yüklenen modele dönmek için listeden seçin.", col::warn);
    } else {
        Hint("SEG-Y (her iz bir sütun), ham float32 ya da CSV. Derinlik 0 deniz yüzeyidir; yoğunluk verilmezse hızdan hesaplanır.");
    }
    if (ToolButton("  Hız modeli yükle…", Btn::Secondary, ImVec2(w, 0))) pickModelFile(ModelQuantity::Velocity);
    if (ToolButton("  Görselden hız modeli…", Btn::Secondary, ImVec2(w, 0),
                   "PNG, JPG, BMP, GIF ya da TIFF. Her piksel 0–255 arası gri değerine indirgenir; her gri tona bir hız verirsiniz."))
        pickImageModel();
    if (ToolButton("  Model çiz…", Btn::Secondary, ImVec2(w, 0),
                   "Katman sınırlarını çizip boyayarak kendi hız modelinizi kurun; her renk bir hızdır."))
        editor_.open(sim_, now_);
    if (sim_.hasModel()) {
        if (ToolButton("  Yoğunluk modeli yükle…", Btn::Secondary, ImVec2(w, 0))) pickModelFile(ModelQuantity::Density);
        if (!sim_.modelDensity().empty() && ToolButton("Yoğunluğu hızdan hesapla", Btn::Ghost, ImVec2(w, 0))) {
            std::string err;
            GridModel vel = sim_.modelVelocity();
            if (sim_.loadModel(vel, GridModel{}, err)) notify("Yoğunluk artık hızdan (Gardner) hesaplanıyor.");
            else alert(err);
        }
        if (ToolButton("  Modeli kaydet…", Btn::Secondary, ImVec2(w, 0),
                       "Yüklü hız modelini (görselden, çizimden ya da dosyadan) kendi çözünürlüğünde SEG-Y olarak kaydeder. "
                       "\"Hız modeli yükle…\" ile doğrudan geri açılır; yoğunluk modeli de yüklüyse yanına ayrı dosya yazılır."))
            saveModelFile();
        if (ToolButton("  Modeli kaldır", Btn::Ghost, ImVec2(w, 0))) {
            sim_.clearModel();
            notify("Yüklenen model kaldırıldı; düz katman modeli kullanılıyor.");
        }
    }
    if (imgModel_.loaded) sectionImageModel();
    ImGui::Dummy(ImVec2(0, S(4)));
}

// ── Velocity model from an image: one velocity per gray level ──────────────
void App::saveModelFile() {
    const GridModel& v = sim_.modelVelocity();
    const std::u8string su = std::filesystem::u8path(v.source).stem().u8string();
    std::string stem(su.begin(), su.end());
    if (stem.empty() || stem == "Çizim") stem = "hiz_modeli";
    std::string path = pfd::save_file("Hız modelini kaydet", stem + ".segy", {"SEG-Y (*.segy *.sgy)", "*.segy *.sgy"}).result();
    if (path.empty()) return;
    const std::string ext = lowerExt(path);
    if (ext != ".segy" && ext != ".sgy") path += ".segy";
    std::string err;
    if (!writeSegyModel(path, v, ModelQuantity::Velocity, err)) { alert(err); return; }
    std::string note = "Hız modeli kaydedildi: " + baseName(path);
    const GridModel& r = sim_.modelDensity();
    if (!r.empty()) {   // the density model next to it: name_yogunluk.segy
        const std::filesystem::path pp = std::filesystem::u8path(path);
        const std::filesystem::path rp = pp.parent_path() / (pp.stem().u8string() + u8"_yogunluk" + pp.extension().u8string());
        const std::u8string ru = rp.u8string();
        const std::string rs(ru.begin(), ru.end());
        if (!writeSegyModel(rs, r, ModelQuantity::Density, err)) { alert(err); return; }
        note += " (yoğunluk: " + baseName(rs) + ")";
    }
    notify(note + ". \"Hız modeli yükle…\" ile geri açabilirsiniz.");
}

void App::pickImageModel() {
    auto files = pfd::open_file("Hız modeli için görsel seç", "",
                                {"Görseller", "*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff", "Tüm dosyalar", "*"}).result();
    if (files.empty()) return;
    GrayImage img;
    std::string err;
    if (!loadGrayImage(files[0], img, err)) { alert(err); return; }
    ImageModelState s;
    s.loaded = true;
    s.name = baseName(files[0]);
    s.img = std::move(img);
    // Default: a straight ramp, black 1500 m/s (water) to white 4500 m/s
    for (int g = 0; g < 256; g++) s.vel[g] = std::round(1500.0f + 3000.0f * g / 255.0f);
    // Keep the domain the model sits in now (the image spans it from the sea surface down)
    s.widthM = std::round(sim_.p.domain_W);
    s.depthM = std::round(sim_.p.domain_H * (1.0f - 0.166f));
    imgModel_ = std::move(s);
}

void App::sectionImageModel() {
    ImageModelState& s = imgModel_;
    const std::vector<int> levels = s.img.levels();
    caption("Görselden model");
    char b[200];
    std::snprintf(b, sizeof(b), "%s\n%d × %d piksel, %zu farklı gri ton", s.name.c_str(), s.img.w, s.img.h, levels.size());
    Hint(b, col::text);
    if (s.img.colours > (int)levels.size()) {
        std::snprintf(b, sizeof(b), "Görselde %d renk var, %zu gri tona düştü: aynı gri değerdeki renkler aynı hızı alır.", s.img.colours,
                      levels.size());
        Hint(b, col::warn);
    }
    if (levels.size() > 24)
        Hint("Çok sayıda ton var; kenar yumuşatma (anti-aliasing) ya da JPEG sıkıştırması ara tonlar üretmiş olabilir.", col::warn);
    Hint("1600 m/s ve altı su sayılır: deniz tabanı ve OBS konumları buna göre bulunur.");

    // One row per gray level: swatch, level and share of the image, velocity box
    const float avail = ImGui::GetContentRegionAvail().x, boxW = S(96), rowH = ImGui::GetFrameHeight();
    const double total = (double)s.img.w * s.img.h;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int g : levels) {
        ImGui::PushID(g);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float sw = rowH * 0.8f, sy = q.y + (rowH - sw) * 0.5f;
        dl->AddRectFilled(ImVec2(q.x, sy), ImVec2(q.x + sw, sy + sw), IM_COL32(g, g, g, 255), S(3));
        dl->AddRect(ImVec2(q.x, sy), ImVec2(q.x + sw, sy + sw), col::rail, S(3));
        std::snprintf(b, sizeof(b), "Gri %d", g);
        dl->AddText(ImVec2(q.x + sw + S(10), q.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), col::text, b);
        std::snprintf(b, sizeof(b), "%%%.1f", 100.0 * s.img.count[g] / total);
        const float pw = fonts.mono->CalcTextSizeA(fonts.mono->FontSize, FLT_MAX, 0, b).x;
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(q.x + avail - boxW - S(12) - pw, q.y + (rowH - fonts.mono->FontSize) * 0.5f),
                    col::muted, b);
        ImGui::SetCursorScreenPos(ImVec2(q.x + avail - boxW, q.y));
        ImGui::SetNextItemWidth(boxW);
        ImGui::PushFont(fonts.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(col::textHi));
        float v = s.vel[g];
        if (ImGui::InputFloat("##vel", &v, 0, 0, "%.0f")) s.vel[g] = v;
        if (ImGui::IsItemDeactivatedAfterEdit()) s.vel[g] = std::clamp(std::round(s.vel[g]), 300.0f, 8000.0f);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        Tip("Bu gri tondaki piksellerin hızı (m/s, 300 – 8000).");
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rowH));
        ImGui::Dummy(ImVec2(avail, S(2)));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    ParamRow("Model genişliği (m)", "imgW", &s.widthM, 100, 25000, 10, "%.0f");
    Tip("Görselin sol kenarı x = 0, sağ kenarı bu uzaklık.");
    ParamRow("Model derinliği (m)", "imgD", &s.depthM, 50, 12000, 10, "%.0f");
    Tip("Görselin üst kenarı deniz yüzeyi, alt kenarı bu derinlik.");
    std::snprintf(b, sizeof(b), "Piksel boyu: %.2f m (yatay) × %.2f m (düşey)", s.widthM / (s.img.w - 1), s.depthM / (s.img.h - 1));
    Hint(b);
    // The model is solved on the simulation grid, not at the pixel size
    const float pix = std::min(s.widthM / (s.img.w - 1), s.depthM / (s.img.h - 1));
    std::snprintf(b, sizeof(b), "Simülasyon ızgarası: %g m (Sayısal çözüm → Izgara aralığı). Model bu ızgarada çözülür.", sim_.p.dh);
    Hint(b, col::text);
    if (pix < 0.5f * sim_.p.dh) {
        std::snprintf(b, sizeof(b), "Pikseller ızgaradan %.0f kat küçük: %g m'den ince ayrıntılar simülasyonda görünmez. Gerekirse ızgara "
                      "aralığını küçültün (hesap süresi artar).", sim_.p.dh / pix, sim_.p.dh);
        Hint(b, col::warn);
    }
    const float w = ImGui::GetContentRegionAvail().x;
    if (ToolButton("Hız modelini oluştur", Btn::Secondary, ImVec2(w, 0), "Hız modeli bu görselden kurulur; yoğunluk hızdan (Gardner) hesaplanır.")) {
        std::string err;
        bool ok = false;
        try {
            GridModel m = modelFromGray(s.img, s.vel, s.widthM, s.depthM, s.name);
            ok = sim_.loadModel(std::move(m), GridModel{}, err);
        } catch (const std::bad_alloc&) {
            err = "Model belleğe sığmadı; daha küçük bir görselle deneyin.";
        }
        if (ok) {
            std::snprintf(b, sizeof(b), "Görselden hız modeli kuruldu: %s, %zu ton, %.0f × %.0f m.", s.name.c_str(), levels.size(), s.widthM,
                          s.depthM);
            notify(b);
        } else alert(err);
    }
    if (ToolButton("Görseli kapat", Btn::Ghost, ImVec2(w, 0), "Bu listeyi kapatır; kurulan model yerinde kalır.")) imgModel_ = ImageModelState{};
}

void App::sectionGeometry() {
    SimParams& p = sim_.p;
    if (ParamRow("Kablo boyu (m)", "cable", &p.cable_len, 50, 1500, 10, "%.0f")) sim_.onCableLen();
    {
        static const char* sp[] = {"3.125 m", "6.25 m", "12.5 m", "25 m", "50 m", "100 m"};
        static const float spv[] = {3.125f, 6.25f, 12.5f, 25.f, 50.f, 100.f};
        int idx = 1;
        for (int i = 0; i < 6; i++) if (std::fabs(spv[i] - p.receiver_spacing) < 1e-3f) idx = i;
        if (ComboRow("Alıcı grup aralığı", "##groupSpace", &idx, sp, 6, S(110))) {
            p.receiver_spacing = spv[idx];
            sim_.onReceiverSpacing(true);
        }
        char b[16];
        std::snprintf(b, sizeof(b), "%d", (int)std::floor(p.cable_len / p.receiver_spacing) + 1);
        ReadoutRow("Alıcı sayısı", b);
    }
    ParamRow("Kablo baş derinliği (m)", "cHead", &p.cable_head_depth, 2, 100, 1, "%.0f");
    ParamRow("Kablo son derinliği (m)", "cTail", &p.cable_tail_depth, 2, 250, 1, "%.0f");
    ParamRow("Kaynak derinliği (m)", "srcDepth", &p.source_depth, 2, 50, 1, "%.0f");
    caption("Gemiler");
    float shipX = (float)sim_.sxShip;
    const float shipMax = (float)(sim_.W_sim * sim_.SCALE);
    if (ParamRow("Ana gemi konumu X (m)", "shipX", &shipX, 100, std::max(110.0f, shipMax), 10, "%.0f")) sim_.onShipX(shipX);
    if (ParamRow("Kaynak gemisi uzaklığı (m)", "srcOff", &p.src_offset, 0, 800, 10, "%.0f")) sim_.onSourceOffset();
    Tip("SLO ve CLO kiplerinde ikinci geminin ana gemiye uzaklığı.");
    ParamRow("Gemi – kaynak (m)", "shipSrc", &p.ship_src_dist, 0, 300, 10, "%.0f");
    ParamRow("Kaynak – ilk alıcı (m)", "srcRec", &p.src_rec_dist, 0, 500, 10, "%.0f");
    ParamRow("Gemi hızı (knot)", "speed", &p.ship_speed_knots, 0, 12, 0.5f, "%.1f");
    ParamRow("Atış aralığı (m)", "shot", &p.shot_interval, 6.25f, 1000, 6.25f, "%.2f");
    Tip("Ardışık atışlar arasındaki uzaklık. Hat simülasyonu da bu aralığı kullanır.");
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionObs() {
    SimParams& p = sim_.p;
    bool on = p.obs_enabled;
    if (Switch("OBS kaydı", &on, "Deniz tabanına eşit aralıklarla yerleşen alıcılar; kayıtları sağdaki panelde.")) sim_.toggleObs();
    if (ParamRow("OBS aralığı (m)", "obsSp", &p.obs_spacing, 50, 500, 10, "%.0f")) pendingObs_ = true;
    char b[16];
    std::snprintf(b, sizeof(b), "%d", sim_.N_OBS);
    ReadoutRow("OBS sayısı", b);
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionNumerics(double now) {
    SimParams& p = sim_.p;
    static const char* orders[] = {"2. derece", "4. derece", "6. derece", "8. derece", "10. derece"};
    int idx = p.fd_order / 2 - 1;
    if (ComboRow("Türev derecesi", "##fdOrder", &idx, orders, 5, S(130))) { p.fd_order = (idx + 1) * 2; sim_.onFdOrder(); }
    if (ParamRow("Izgara aralığı (m)", "dh", &p.dh, 0.5f, 8.0f, 0.5f, "%.1f")) sim_.onGridSize(now);
    if (ParamRow("Zaman adımı katsayısı", "dt", &p.dt, 0.1f, 1.5f, 0.05f, "%.2f")) sim_.onTimeStep();
    if (ParamRowInt("Soğurucu sınır (hücre)", "npml", &p.npml, 5, 60, 5)) pendingNpml_ = true;
    bool box = p.expanding_box;
    if (Switch("Genişleyen hesap kutusu", &box,
               "Yalnız dalganın ulaştığı bölge hesaplanır (eikonal). Sonucu değiştirmez, simülasyonu hızlandırır."))
        sim_.toggleExpandingBox();
    caption("Kararlılık ve çözünürlük");
    const NumericalStatus ns = sim_.numericalStatus();
    char b[64];
    const int cflLevel = ns.cfl_ratio < 0.85 ? 0 : ns.cfl_ratio < 1.0 ? 1 : 2;
    std::snprintf(b, sizeof(b), "%%%d", (int)std::lround(ns.cfl_ratio * 100));
    ReadoutRow("CFL oranı", b, levelColor(cflLevel));
    Tip("Zaman adımının kararlılık sınırına oranı. %100'ü aşarsa simülasyon patlar.");
    std::snprintf(b, sizeof(b), "en fazla %.2f", ns.dt_max);
    ReadoutRow("Zaman katsayısı sınırı", b);
    const int pLevel = ns.ppw >= ns.min_ppw * 1.5 ? 0 : ns.ppw >= ns.min_ppw ? 1 : 2;
    std::snprintf(b, sizeof(b), "%.1f (en az %d)", ns.ppw, ns.min_ppw);
    ReadoutRow("Dalga boyu başına nokta", b, levelColor(pLevel));
    Tip("En yavaş hızdaki en kısa dalga boyunu kaç ızgara noktasının örneklediği. Azsa sayısal dağılım (dispersion) görülür.");
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionView() {
    SimParams& p = sim_.p;
    ParamRow("Görsel kazanç", "gain", &p.gain, 1, 100, 1, "%.0f");
    ParamRow("Kare başına ilerleme (ms)", "spd", &p.speed_ms, 1, 250, 0.5f, "%.1f");
    Tip("Her ekran karesinde simüle edilen süre. Büyüdükçe simülasyon gerçek zamandan hızlı akar.");
    bool draw = !p.sismo_only;
    if (Switch("Dalga alanını çiz", &draw, "Kapatınca yalnız kayıtlar güncellenir; simülasyon daha hızlı ilerler."))
        sim_.toggleSismoOnly();
    {   // Model colours and the colour bar
        static const char* maps[] = {"Deniz", "Turbo", "Viridis", "Magma", "Gri"};
        int cm = std::clamp(p.model_colormap, 0, 4);
        if (ComboRow("Model renk haritası", "##cmap", &cm, maps, 5, S(150))) {
            p.model_colormap = cm;
            sim_.onModelColormap();
        }
        Tip("Modeli hıza göre renklendiren harita. Turbo ve Viridis hız farklarını en net gösterir; Gri yazdırmaya uygundur.");
        bool cbar = p.show_colorbar;
        if (Switch("Hız renk ölçeği", &cbar, "Model görünümünün sağ altında hangi rengin hangi hız olduğunu gösterir.")) p.show_colorbar = cbar;
    }
    bool skip = p.skip_gap;
    if (Switch("Kayıt bitince sonraki atışa geç", &skip,
               "Kayıt uzunluğu dolunca gemi bir sonraki atış konumuna kısa bir geçişle gelir ve yeni atış hemen başlar. "
               "Kapalıyken gemi atış aralığını gerçek hızıyla kat eder. Atış konumları ve kayıtlar ikisinde de aynıdır."))
        p.skip_gap = skip;
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionLine() {
    SimParams& p = sim_.p;
    const bool active = sim_.lineActive();
    const float W = (float)((sim_.W_sim - 1) * sim_.SCALE);
    const Mode lineMode = active ? sim_.lineMode() : modeOf(selMode_);
    const bool twoShips = lineMode != Mode::Trad;

    // Everything but the shot count comes from the other groups: the first shot is the main
    // source as placed now (ship position minus ship - source), the spacing is the shot interval
    const float firstX = active ? (float)sim_.lineFirstX() : (float)(sim_.sxShip - p.ship_src_dist);
    ParamRowInt("Atış sayısı", "lnN", &lineShots_, 1, 2000, 1);
    Hint("İlk atış ana kaynağın şu anki konumunda (Saha geometrisi → Ana gemi konumu); atışlar arası uzaklık Saha "
         "geometrisindeki atış aralığı.");
    const double lastX = firstX + (lineShots_ - 1) * (double)p.shot_interval;
    const double tailX = firstX - p.src_rec_dist - p.cable_len;
    const double srcLastX = lastX + (twoShips ? p.src_offset : 0.0);
    const bool outside = lastX > W || srcLastX > W;
    char b[160];
    if (lastX > W) {
        std::snprintf(b, sizeof(b), "Son atışlar modelin dışında (model %.0f m genişliğinde).", W);
        Hint(b, col::danger);
    } else if (srcLastX > W) {
        std::snprintf(b, sizeof(b), "Kaynak gemisi son atışlarda %.0f m'de, modelin dışında (model %.0f m). Atış sayısını ya da "
                      "kaynak gemisi uzaklığını azaltın.", srcLastX, W);
        Hint(b, col::danger);
    } else if (tailX < 0) {
        std::snprintf(b, sizeof(b), "İlk atışlarda kablonun %.0f m'si modelin dışında; o kanallar boş kaydedilir.", -tailX);
        Hint(b, col::warn);
    }
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(outside);
    if (ToolButton("  Hattı simüle et ve kaydet…", Btn::Secondary, ImVec2(w, 0))) {
        std::string dir = pfd::select_folder("Hat SEG-Y dosyalarının kaydedileceği klasörü seçin").result();
        if (!dir.empty()) {
            Simulation::LineSettings ls;
            ls.firstX = firstX; ls.nShots = lineShots_; ls.interval = p.shot_interval; ls.mode = lineMode; ls.folder = dir;
            std::string err;
            if (!sim_.startLine(ls, err)) alert(err);
        }
    }
    ImGui::EndDisabled();

    // ── What the line will run with: every setting it takes from the other groups ──
    ImGui::Dummy(ImVec2(0, S(6)));
    {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(q.x, q.y), ImVec2(q.x + w, q.y), col::bulkhead);
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::PushFont(fonts.heading);
    ImGui::TextColored(vec(col::textHi), "Hattın ayarları");
    ImGui::PopFont();
    Hint("Hat, öteki gruplardaki bu değerlerle çalışır.");

    caption("Hat");
    static const char* modeNames[] = {"Geleneksel", "SLO (eşzamanlı)", "CLO (ardışık)"};
    ReadoutRow("Atış kipi", modeNames[indexOf(lineMode)]);
    Tip("Komut çubuğunda seçili kip. SLO'da kaynak gemisi 25 adım sonra aynı kayda atar; CLO'da ana kayıt bitince "
        "kaynak gemisi atar ve ikinci bir kayıt alınır.");
    std::snprintf(b, sizeof(b), "%.0f – %.0f m", firstX, lastX);
    ReadoutRow("Atış konumları", b);
    std::snprintf(b, sizeof(b), "%.2f m", p.shot_interval);
    ReadoutRow("Atış aralığı", b);
    std::snprintf(b, sizeof(b), "%.0f m", lastX - firstX);
    ReadoutRow("Hat uzunluğu", b);
    std::snprintf(b, sizeof(b), "%.1f knot", p.ship_speed_knots);
    ReadoutRow("Gemi hızı", b);
    {
        const double recSec = p.sismo_ms / 1000.0 * (lineMode == Mode::CLO ? 2.0 : 1.0);
        std::snprintf(b, sizeof(b), "Gemi her kayıt boyunca %.1f m ilerler; kaynak attığı yerde kalır, kablo gemiyle gider.",
                      p.ship_speed_knots * 0.514444 * recSec);
        if (p.ship_speed_knots > 0) Hint(b);
    }
    if (twoShips) {
        std::snprintf(b, sizeof(b), "%.0f m önde", p.src_offset);
        ReadoutRow("Kaynak gemisi", b);
    }

    caption("Kayıt");
    std::snprintf(b, sizeof(b), "%d ms, 1 ms örnek", p.sismo_ms);
    ReadoutRow("Uzunluk", b);
    if (p.obs_enabled) std::snprintf(b, sizeof(b), "streamer %d, OBS %d", sim_.N_REC, sim_.N_OBS);
    else std::snprintf(b, sizeof(b), "streamer %d", sim_.N_REC);
    ReadoutRow("Kanal", b);
    std::string files = "line_streamer.segy";
    if (lineMode == Mode::CLO) files += ", line_streamer_src.segy";
    if (p.obs_enabled) files += ", line_obs.segy";
    Hint(files.c_str(), col::text);

    caption("Kaynak");
    static const char* wav[] = {"Ricker", "Minimum faz", "Gauss", "Gauss 1. türevi"};
    std::snprintf(b, sizeof(b), "%s, %.1f Hz", wav[std::clamp((int)p.wavelet, 0, 3)], p.freq);
    ReadoutRow("Dalgacık", b);
    std::snprintf(b, sizeof(b), "%.1f", p.amp);
    ReadoutRow("Genlik", b);
    std::snprintf(b, sizeof(b), "%.0f m", p.source_depth);
    ReadoutRow("Derinlik", b);

    caption("Geometri");
    std::snprintf(b, sizeof(b), "%.0f m, %g m aralık", p.cable_len, p.receiver_spacing);
    ReadoutRow("Kablo", b);
    std::snprintf(b, sizeof(b), "%.0f → %.0f m", p.cable_head_depth, p.cable_tail_depth);
    ReadoutRow("Kablo derinliği (baş → son)", b);
    std::snprintf(b, sizeof(b), "%.0f – %.0f m", p.src_rec_dist, p.src_rec_dist + p.cable_len);
    ReadoutRow("Ofset (yakın – uzak)", b);
    std::snprintf(b, sizeof(b), "%.0f m", p.ship_src_dist);
    ReadoutRow("Gemi – kaynak", b);
    if (p.obs_enabled) {
        std::snprintf(b, sizeof(b), "%d, %.0f m aralık", sim_.N_OBS, p.obs_spacing);
        ReadoutRow("OBS", b);
    }

    caption("Model ve sayısal çözüm");
    static const char* models[] = {"Düz katmanlar", "Fay", "Tuz domu", "Dosyadan"};
    std::string model = models[std::clamp((int)p.geology, 0, 3)];
    if (p.geology == GeologyModel::Imported && sim_.hasModel()) model = sim_.modelVelocity().source;
    ReadoutRow("Hız modeli", model.c_str());
    std::snprintf(b, sizeof(b), "%.0f × %.0f m", W, (sim_.H_sim - 1) * sim_.SCALE);
    ReadoutRow("Alan", b);
    ReadoutRow("Deniz yüzeyi yansıması", p.free_surface ? "açık" : "kapalı");
    std::snprintf(b, sizeof(b), "%d × %d, Δh %g m", sim_.W_sim, sim_.H_sim, sim_.SCALE);
    ReadoutRow("Izgara", b);
    std::snprintf(b, sizeof(b), "%d. derece, katsayı %.2f", p.fd_order, p.dt);
    ReadoutRow("Türev, zaman adımı", b);
    const NumericalStatus ns = sim_.numericalStatus();
    const int cflLevel = ns.cfl_ratio < 0.85 ? 0 : ns.cfl_ratio < 1.0 ? 1 : 2;
    std::snprintf(b, sizeof(b), "%%%d", (int)std::lround(ns.cfl_ratio * 100));
    ReadoutRow("CFL oranı", b, levelColor(cflLevel));
    const int pLevel = ns.ppw >= ns.min_ppw * 1.5 ? 0 : ns.ppw >= ns.min_ppw ? 1 : 2;
    std::snprintf(b, sizeof(b), "%.1f", ns.ppw);
    ReadoutRow("Dalga boyu başına nokta", b, levelColor(pLevel));
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionFiles() {
    const float w = ImGui::GetContentRegionAvail().x, half = (w - S(8)) * 0.5f;
    if (ToolButton("  Kaydet…", Btn::Secondary, ImVec2(half, 0))) saveFile();
    ImGui::SameLine(0, S(8));
    if (ToolButton("  Aç…", Btn::Secondary, ImVec2(half, 0))) loadFile();
    if (ToolButton("Son ayarları sakla", Btn::Ghost, ImVec2(half, 0), "Uygulama verisi klasörüne (AppData) kaydeder.")) saveLocal();
    ImGui::SameLine(0, S(8));
    if (ToolButton("Son ayarları geri yükle", Btn::Ghost, ImVec2(half, 0))) loadLocal();
    Hint("Model dosyaları parametrelerle birlikte saklanmaz; yüklenen modeli ayrıca açmak gerekir.");
    ImGui::Dummy(ImVec2(0, S(4)));
}

void App::sectionCpu() {
    if (sim_.mode == Mode::None || sim_.usingGpu() || sim_.threadStats.empty()) return;
    caption("İşlemci yükü");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = (int)sim_.threadStats.size();
    for (int i = 0; i < n; i++) {
        const ThreadStat& st = sim_.threadStats[i];
        const double total = st.compute + st.wait;
        const float load = total > 0 ? (float)(st.compute / total * 100.0) : 0.0f;
        char lbl[16], val[16];
        std::snprintf(lbl, sizeof(lbl), "İş %d", i + 1);
        std::snprintf(val, sizeof(val), "%%%.0f", load);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = S(18);
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(p0.x, p0.y), col::muted, lbl);
        const ImVec2 b0(p0.x + S(44), p0.y + S(7)), b1(p0.x + w - S(48), p0.y + S(11));
        dl->AddRectFilled(b0, b1, col::deck, S(2));
        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * std::clamp(load / 100.0f, 0.0f, 1.0f), b1.y),
                          load > 85 ? col::danger : load > 50 ? col::warn : col::data, S(2));
        TextAt(dl, fonts.mono, fonts.mono->FontSize, ImVec2(p0.x + w, p0.y), col::text, val, 2, false);
        ImGui::Dummy(ImVec2(w, h));
    }
    ImGui::Dummy(ImVec2(0, S(4)));
}

// ── Views: model / wavefield over the recordings ─────────────────────────
void App::views(ImVec2 o, ImVec2 size) {
    const float tabH = S(44);
    viewTabs(o, ImVec2(size.x, tabH));
    o.y += tabH;
    size.y -= tabH;
    if (view_ == 0) { scenePane(o, size); return; }
    if (view_ == 1) { gatherPanes(o, size); return; }
    const float splitH = S(6);
    const float sceneH = std::round((size.y - splitH) * std::clamp(split_, 0.2f, 0.8f));
    scenePane(o, ImVec2(size.x, sceneH));
    {
        const ImVec2 s0(o.x, o.y + sceneH);
        ImGui::SetCursorScreenPos(s0);
        ImGui::InvisibleButton("##viewSplit", ImVec2(size.x, splitH));
        const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (act) split_ = std::clamp(split_ + ImGui::GetIO().MouseDelta.y / std::max(1.0f, size.y - splitH), 0.2f, 0.8f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(s0, ImVec2(s0.x + size.x, s0.y + splitH), col::abyss);
        dl->AddLine(ImVec2(s0.x, s0.y + splitH * 0.5f), ImVec2(s0.x + size.x, s0.y + splitH * 0.5f),
                    act ? col::accent : hov ? col::rail : col::bulkhead, act || hov ? S(2) : 1.0f);
    }
    gatherPanes(ImVec2(o.x, o.y + sceneH + splitH), ImVec2(size.x, size.y - sceneH - splitH));
}

void App::scenePane(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hH = S(40);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + hH), col::abyss);
    dl->AddText(fonts.subhead, fonts.subhead->FontSize, ImVec2(o.x + S(14), o.y + (hH - fonts.subhead->FontSize) * 0.5f), col::textHi,
                "Model ve dalga alanı");
    {   // state on the right: mode, pause, simulated time
        char t[64];
        std::snprintf(t, sizeof(t), "T %.1f ms", sim_.cycle_t * sim_.dtPhy() * 1000.0);
        const ImVec2 ts = fonts.mono->CalcTextSizeA(fonts.mono->FontSize, FLT_MAX, 0, t);
        float x = o.x + size.x - S(14) - ts.x;
        const float cy = o.y + hH * 0.5f;
        dl->AddText(fonts.mono, fonts.mono->FontSize, ImVec2(x, cy - ts.y * 0.5f), col::textHi, t);
        const char* state = sim_.mode == Mode::None ? "Beklemede"
                          : sim_.paused ? "Duraklatıldı"
                          : sim_.gliding() ? "Sonraki atışa geçiliyor"
                          : sim_.mode == Mode::SLO ? "SLO çalışıyor"
                          : sim_.mode == Mode::CLO ? "CLO çalışıyor" : "Geleneksel çalışıyor";
        const ImU32 sc = sim_.mode == Mode::None ? col::muted : sim_.paused ? col::warn : col::ok;
        const ImVec2 ss = fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, state);
        x -= ss.x + S(22);
        dl->AddCircleFilled(ImVec2(x - S(8), cy), S(3.5f), sc, 12);
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x, cy - ss.y * 0.5f), col::text, state);
    }
    const ImVec2 c0(o.x, o.y + hH), c1(o.x + size.x, o.y + size.y);
    const SimParams& p = sim_.p;
    // The domain fills the pane: horizontal scale from the width, vertical from the height
    const float k = (c1.x - c0.x) / p.domain_W, ky = (c1.y - c0.y) / p.domain_H;
    dl->PushClipRect(c0, c1, true);
    drawScene(sim_, dl, c0, k, ky, 1);
    dl->PopClipRect();
    if (sim_.instability) instabilityCard(c0, ImVec2(c1.x - c0.x, c1.y - c0.y));
}

void App::instabilityCard(ImVec2 o, ImVec2 size) {
    const float w = std::min(S(500), size.x - S(40));
    ImGui::SetCursorScreenPos(ImVec2(o.x + (size.x - w) * 0.5f, o.y + S(24)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(col::hull));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(withAlpha(col::danger, 0.7f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(16)));
    ImGui::BeginChild("##unstable", ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(fonts.uiBold);
    ImGui::TextColored(vec(col::danger), "%s", icon::Error);
    ImGui::SameLine();
    ImGui::TextColored(vec(col::textHi), "Simülasyon kararsızlaştı");
    ImGui::PopFont();
    const NumericalStatus ns = sim_.numericalStatus();
    char b[256];
    std::snprintf(b, sizeof(b), "Sayısal enerji sınırı aştı. %d. derece türevle zaman adımı katsayısı en fazla %.2f olabilir; "
                  "şu an %.2f. Sıfırlayıp katsayıyı düşürün ya da türev derecesini azaltın.", sim_.p.fd_order, ns.dt_max, sim_.p.dt);
    Hint(b, col::text);
    if (ToolButton("Sıfırla", Btn::Primary)) sim_.stopMode();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void App::gatherPanes(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hH = S(40);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + hH), col::abyss);
    const float cy = o.y + hH * 0.5f;
    dl->AddText(fonts.subhead, fonts.subhead->FontSize, ImVec2(o.x + S(14), cy - fonts.subhead->FontSize * 0.5f), col::textHi, "Kayıtlar");
    {   // legend: what each trace colour is
        float x = o.x + S(14) + fonts.subhead->CalcTextSizeA(fonts.subhead->FontSize, FLT_MAX, 0, "Kayıtlar").x + S(18);
        auto key = [&](ImU32 c, const char* t) {
            dl->AddRectFilled(ImVec2(x, cy - S(1.5f)), ImVec2(x + S(14), cy + S(1.5f)), c, S(1));
            x += S(20);
            dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x, cy - fonts.ui->FontSize * 0.5f), col::text, t);
            x += fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, t).x + S(16);
        };
        key(hex(0xD9E2EA), sim_.lastMode == Mode::CLO ? "Ana gemi" : "Streamer");
        if (sim_.lastMode == Mode::CLO) key(col::data, "Kaynak gemisi");
        if (sim_.p.obs_enabled) key(col::warn, "OBS");
    }
    {   // display controls (right)
        static const char* kinds[] = {"Wiggle", "Yoğunluk"};
        static const char* kindTips[] = {"İzler eğri olarak, pozitif ve negatif loblar dolu.",
                                         "Gri tonlu yoğunluk gösterimi (beyaz negatif, siyah pozitif)."};
        const float resetW = ButtonWidth("  Görünümü sıfırla");
        const float segW = S(170);
        const float cubicW = ImGui::CalcTextSize("Kübik").x + ImGui::GetFrameHeight() + S(8);
        float x = o.x + size.x - S(14) - resetW;
        ImGui::SetCursorScreenPos(ImVec2(x, cy - S(15)));
        if (ToolButton("  Görünümü sıfırla", Btn::Ghost, ImVec2(resetW, S(30)), "Kayıtlardaki kaydırma ve yakınlaştırmayı sıfırlar.")) gv_.reset();
        x -= S(12) + cubicW;
        ImGui::SetCursorScreenPos(ImVec2(x, cy - ImGui::GetFrameHeight() * 0.5f));
        ImGui::Checkbox("Kübik", &gv_.smooth);
        Tip("Örnekler arasında kübik (Catmull-Rom) ara değerleme.");
        x -= S(14) + segW;
        ImGui::SetCursorScreenPos(ImVec2(x, cy - S(16)));
        int k = gv_.vd ? 1 : 0;
        if (Segmented("##gatherKind", kinds, 2, &k, segW, kindTips)) gv_.vd = k == 1;
    }
    const float gap = S(5), capH = S(32);
    const float py = o.y + hH, ph = size.y - hH;
    const float pw = (size.x - gap) * 0.5f;
    // Each pane: a caption strip naming it, then the plot
    drawGatherPanel(sim_, gv_, 0, ImVec2(o.x, py + capH), ImVec2(pw, ph - capH));
    drawGatherPanel(sim_, gv_, 1, ImVec2(o.x + pw + gap, py + capH), ImVec2(size.x - pw - gap, ph - capH));
    dl->AddRectFilled(ImVec2(o.x + pw, py), ImVec2(o.x + pw + gap, py + ph), col::abyss);
    auto paneLabel = [&](float x0, float w, const char* name, bool empty, const char* emptyText) {
        dl->AddRectFilled(ImVec2(x0, py), ImVec2(x0 + w, py + capH), col::hull);
        dl->AddText(fonts.uiBold, fonts.uiBold->FontSize, ImVec2(x0 + S(12), py + (capH - fonts.uiBold->FontSize) * 0.5f), col::textHi, name);
        if (empty) {
            const ImVec2 es = fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, emptyText);
            dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(x0 + (w - es.x) * 0.5f + S(19), py + ph * 0.5f), col::muted, emptyText);
        }
    };
    const bool idle = sim_.mode == Mode::None && sim_.cycle_t == 0;
    paneLabel(o.x, pw, "Streamer kaydı, kaynaktan uzaklık (m)", idle, "Başlat'a basınca izler burada görünür");
    paneLabel(o.x + pw + gap, size.x - pw - gap, "OBS kaydı, konum (m)", idle && sim_.p.obs_enabled,
              "Deniz tabanı alıcılarının kayıtları burada görünür");
    if (!sim_.p.obs_enabled) {
        const char* t = "OBS kaydı kapalı";
        const ImVec2 es = fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, 0, t);
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(o.x + pw + gap + (size.x - pw - gap - es.x) * 0.5f, py + ph * 0.5f), col::muted, t);
    }
}

// ── Status bar ────────────────────────────────────────────────────────────
void App::statusBar(ImVec2 o, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::hull);
    dl->AddLine(o, ImVec2(o.x + size.x, o.y), col::bulkhead);
    ImFont* f = fonts.mono;
    const float fs = f->FontSize, cy = o.y + size.y * 0.5f;
    float x = o.x + S(14);
    auto item = [&](const char* t, ImU32 c, ImU32 dot = 0) {
        if (dot) {
            dl->AddCircleFilled(ImVec2(x + S(3), cy), S(3.5f), dot, 12);
            x += S(12);
        }
        dl->AddText(f, fs, ImVec2(x, cy - fs * 0.5f), c, t);
        x += f->CalcTextSizeA(fs, FLT_MAX, 0, t).x + S(12);
        vrule(dl, x, cy - S(7), cy + S(7));
        x += S(12);
    };
    const NumericalStatus ns = sim_.numericalStatus();
    char b[128];
    std::snprintf(b, sizeof(b), "Izgara %d × %d, Δh %g m", sim_.W_sim, sim_.H_sim, sim_.SCALE);
    item(b, col::text);
    const int cflLevel = ns.cfl_ratio < 0.85 ? 0 : ns.cfl_ratio < 1.0 ? 1 : 2;
    std::snprintf(b, sizeof(b), "CFL %%%d", (int)std::lround(ns.cfl_ratio * 100));
    item(b, col::text, levelColor(cflLevel));
    const int pLevel = ns.ppw >= ns.min_ppw * 1.5 ? 0 : ns.ppw >= ns.min_ppw ? 1 : 2;
    std::snprintf(b, sizeof(b), "Dalga boyu başına %.1f nokta", ns.ppw);
    item(b, col::text, levelColor(pLevel));
    if (sim_.usingGpu()) item("GPU", col::text);
    else {
        std::snprintf(b, sizeof(b), "CPU, %d iş parçacığı", sim_.cpuThreads());
        item(b, col::text);
    }
    if (sim_.mode != Mode::None) {
        if (sim_.mode == Mode::Trad) std::snprintf(b, sizeof(b), "Atış %d", sim_.shotsMain);
        else std::snprintf(b, sizeof(b), "Atış: ana %d, kaynak gemisi %d", sim_.shotsMain, sim_.shotsSrc);
        item(b, col::text);
    }
    if (sim_.eikonalBusy()) {
        std::snprintf(b, sizeof(b), "Eikonal hesaplanıyor %%%d", sim_.eikonalProgress());
        item(b, col::warn);
    }
    if (sim_.segyRecording()) {
        std::snprintf(b, sizeof(b), "SEG-Y kaydı: %d atış", sim_.segyShotsSaved());
        item(b, col::text, col::danger);
    }
    // right: elapsed wall time
    std::snprintf(b, sizeof(b), "Geçen süre %d s", (int)sim_.elapsedSec);
    const float w = f->CalcTextSizeA(fs, FLT_MAX, 0, b).x;
    if (o.x + size.x - S(14) - w > x) dl->AddText(f, fs, ImVec2(o.x + size.x - S(14) - w, cy - fs * 0.5f), col::muted, b);
}

// ── Toasts: confirmations that slide in at the bottom right and leave on their own ──
void App::notify(const std::string& msg) { toasts_.push_back({msg, now_}); }

void App::toasts(ImVec2 br) {
    for (auto& n : sim_.notices) notify(n);
    sim_.notices.clear();
    const double life = 4.5, enter = 0.22, exit = 0.16;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float w = S(360), pad = S(12);
    float y = br.y;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int i = (int)toasts_.size() - 1; i >= 0; i--) {
        Toast& t = toasts_[i];
        const ImVec2 ts = fonts.ui->CalcTextSizeA(fonts.ui->FontSize, FLT_MAX, w - pad * 2 - S(22), t.text.c_str());
        const float h = ts.y + pad * 2;
        const ImVec2 a(br.x - w, y - h), b(br.x, y);
        const bool hov = mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y;
        double age = now_ - t.t0;
        if (hov && age > enter) t.t0 += ImGui::GetIO().DeltaTime;   // hovering keeps it open
        age = now_ - t.t0;
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) t.t0 = now_ - (life - exit);   // click dismisses
        const float in = easeOut((float)(age / enter));
        const float out = std::clamp((float)((life - age) / exit), 0.0f, 1.0f);
        const float alpha = in * out;
        const float dx = (1.0f - in) * S(18);
        const ImVec2 A(a.x + dx, a.y), B(b.x + dx, b.y);
        dl->AddRectFilled(A, B, withAlpha(col::hull, alpha), S(6));
        dl->AddRect(A, B, withAlpha(col::rail, alpha), S(6));
        dl->AddRectFilled(ImVec2(A.x, A.y + S(8)), ImVec2(A.x + S(3), B.y - S(8)), withAlpha(col::ok, alpha), S(1.5f));
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(A.x + pad + S(4), A.y + pad), withAlpha(col::ok, alpha), icon::Check);
        dl->AddText(fonts.ui, fonts.ui->FontSize, ImVec2(A.x + pad + S(26), A.y + pad), withAlpha(col::textHi, alpha),
                    t.text.c_str(), nullptr, w - pad * 2 - S(22));
        y = a.y - S(8);
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return now_ - t.t0 > life; }), toasts_.end());
}

// ── Modal for problems the user must read ────────────────────────────────
void App::alertsModal() {
    if (!sim_.alerts.empty() && !ImGui::IsPopupOpen("##alert")) {
        ImGui::OpenPopup("##alert");
        alertOpenedAt_ = now_;
    }
    // Modals stay centred; they fade and rise 8 px over 160 ms (ease-out)
    const float e = easeOut((float)((now_ - alertOpenedAt_) / 0.16));
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.38f + (1 - e) * S(8)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.01f, e));
    if (ImGui::BeginPopupModal("##alert", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        if (!sim_.alerts.empty()) {
            ImGui::PushFont(fonts.uiBold);
            ImGui::TextColored(vec(col::warn), "%s", icon::Warning);
            ImGui::PopFont();
            ImGui::SameLine(0, S(10));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(420));
            ImGui::TextUnformatted(sim_.alerts.front().c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        const float bw = S(110);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - S(20));
        if (ToolButton("Tamam", Btn::Primary, ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (!sim_.alerts.empty()) sim_.alerts.erase(sim_.alerts.begin());
            ImGui::CloseCurrentPopup();
            alertOpenedAt_ = now_;   // the next alert, if any, enters fresh
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ── Model files ───────────────────────────────────────────────────────────
void App::pickModelFile(ModelQuantity q) {
    const char* title = q == ModelQuantity::Velocity ? "Hız modeli seç" : "Yoğunluk modeli seç";
    auto files = pfd::open_file(title, "", {"Model dosyaları", "*.sgy *.segy *.bin *.raw *.f32 *.dat *.csv *.txt *.asc",
                                            "Tüm dosyalar", "*"}).result();
    if (files.empty()) return;
    ModelImportState s;
    s.q = q;
    s.path = files[0];
    s.name = baseName(s.path);
    const std::string e = lowerExt(s.path);
    s.format = (e == ".sgy" || e == ".segy") ? 0 : (e == ".csv" || e == ".txt" || e == ".asc") ? 2 : 1;
    // Defaults from the velocity model already loaded (a density model usually shares its grid)
    if (sim_.hasModel()) {
        const GridModel& v = sim_.modelVelocity();
        s.nx = v.nx; s.nz = v.nz; s.dx = (float)v.dx; s.dz = (float)v.dz;
    }
    std::string err;
    if (s.format == 0) {
        SegyModelInfo info;
        if (probeSegyModel(s.path, info, err)) {
            s.nx = info.nx; s.nz = info.nz;
            if (info.dx > 0) s.dx = (float)info.dx;
            if (info.dz > 0) s.dz = (float)info.dz;
        } else s.probeError = err;
    } else if (s.format == 2) {
        if (!probeAsciiModel(s.path, s.nx, s.nz, err)) s.probeError = err;
    }
    s.open = true;
    imp_ = s;
    importOpenedAt_ = now_;
}

void App::modelImportPopup() {
    if (imp_.open && !ImGui::IsPopupOpen("##modelImport")) ImGui::OpenPopup("##modelImport");
    const float e = easeOut((float)((now_ - importOpenedAt_) / 0.16));
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f + (1 - e) * S(8)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(500), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.01f, e));
    if (ImGui::BeginPopupModal("##modelImport", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const bool vel = imp_.q == ModelQuantity::Velocity;
        ImGui::PushFont(fonts.title);
        ImGui::TextColored(vec(col::textHi), "%s", vel ? "Hız modelini yükle" : "Yoğunluk modelini yükle");
        ImGui::PopFont();
        Hint(imp_.name.c_str(), col::text);
        ImGui::Dummy(ImVec2(0, S(4)));
        static const char* fmts[] = {"SEG-Y (her iz bir sütun)", "Ham float32 (başlıksız)", "CSV / metin ızgarası"};
        int f = imp_.format;
        if (ComboRow("Dosya biçimi", "##impFmt", &f, fmts, 3, S(260)) && f != imp_.format) {
            imp_.format = f;
            imp_.probeError.clear();
            std::string err;
            if (f == 0) {
                SegyModelInfo info;
                if (probeSegyModel(imp_.path, info, err)) {
                    imp_.nx = info.nx; imp_.nz = info.nz;
                    if (info.dx > 0) imp_.dx = (float)info.dx;
                    if (info.dz > 0) imp_.dz = (float)info.dz;
                } else imp_.probeError = err;
            } else if (f == 2 && !probeAsciiModel(imp_.path, imp_.nx, imp_.nz, err)) imp_.probeError = err;
        }
        auto field = [&](const char* label, auto draw) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const ImVec2 p = ImGui::GetCursorPos();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(p.x + avail - S(160), p.y));
            ImGui::SetNextItemWidth(S(160));
            ImGui::PushFont(fonts.mono);
            draw();
            ImGui::PopFont();
        };
        if (imp_.format == 1) {
            field("Sütun sayısı nx (x)", [&] { ImGui::InputInt("##impNx", &imp_.nx, 0, 0); });
            field("Örnek sayısı nz (z)", [&] { ImGui::InputInt("##impNz", &imp_.nz, 0, 0); });
            static const char* orders[] = {"z hızlı (sütun sütun)", "x hızlı (satır satır)"};
            ComboRow("Değer sırası", "##impOrd", &imp_.order, orders, 2, S(260));
            bool be = imp_.bigEndian;
            if (Switch("Big-endian bayt düzeni", &be)) imp_.bigEndian = be;
        } else if (imp_.probeError.empty()) {
            char b[96];
            std::snprintf(b, sizeof(b), "%d", imp_.nx);
            ReadoutRow("Sütun (x)", b);
            std::snprintf(b, sizeof(b), "%d", imp_.nz);
            ReadoutRow("Örnek (z)", b);
        }
        field("Yatay örnek aralığı dx (m)", [&] { ImGui::InputFloat("##impDx", &imp_.dx, 0, 0, "%.3f"); });
        field("Düşey örnek aralığı dz (m)", [&] { ImGui::InputFloat("##impDz", &imp_.dz, 0, 0, "%.3f"); });
        if (imp_.nx > 1 && imp_.nz > 1 && imp_.dx > 0 && imp_.dz > 0) {
            char b[96];
            std::snprintf(b, sizeof(b), "%.0f m × %.0f m", (imp_.nx - 1) * imp_.dx, (imp_.nz - 1) * imp_.dz);
            ReadoutRow("Model boyutu (genişlik × derinlik)", b);
        }
        if (imp_.format == 0) Hint("dz, SEG-Y örnek aralığı alanından okunur: 4000 \"µs\" = 4 m.");
        if (!imp_.probeError.empty()) Hint(imp_.probeError.c_str(), col::danger);
        ImGui::Dummy(ImVec2(0, S(6)));
        const float bw = S(110);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 2 * bw - S(28));
        const bool cancel = ToolButton("Vazgeç", Btn::Secondary, ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SameLine(0, S(8));
        const bool load = ToolButton("Yükle", Btn::Primary, ImVec2(bw, 0));
        if (load) {
            std::string err, note;
            GridModel m;
            bool ok = imp_.format == 0 ? readSegyModel(imp_.path, imp_.dx, imp_.dz, m, err)
                    : imp_.format == 1 ? readRawModel(imp_.path, imp_.nx, imp_.nz, imp_.dx, imp_.dz, imp_.order == 1, imp_.bigEndian, m, err)
                                       : readAsciiModel(imp_.path, imp_.dx, imp_.dz, m, err);
            ok = ok && normalizeModelUnits(m, imp_.q, note, err);
            if (ok) {
                if (vel) ok = sim_.loadModel(std::move(m), GridModel{}, err);
                else {
                    GridModel v = sim_.modelVelocity();
                    ok = sim_.loadModel(std::move(v), std::move(m), err);
                }
            }
            if (ok) {
                notify(vel ? "Hız modeli yüklendi; yoğunluk hızdan (Gardner) hesaplanıyor." : "Yoğunluk modeli yüklendi.");
                if (!note.empty()) notify(note);
                imp_.open = false;
                ImGui::CloseCurrentPopup();
            } else {
                imp_.probeError = err;   // stays open with the reason
            }
        } else if (cancel) {
            imp_.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ── Parameter files ───────────────────────────────────────────────────────
void App::saveLocal() {
    if (writeTextFile(localParamsPath(), sim_.configJson())) notify("Ayarlar uygulama verisine kaydedildi.");
    else alert("Ayarlar kaydedilemedi: " + localParamsPath());
}

void App::loadLocal() {
    std::string text, err;
    if (!readTextFile(localParamsPath(), text)) { alert("Uygulama verisinde kayıtlı ayar yok. Önce \"Son ayarları sakla\"yı kullanın."); return; }
    if (sim_.applyConfigJson(text, err)) notify("Son kaydedilen ayarlar yüklendi.");
    else alert("Kayıtlı ayarlar okunamadı (dosya bozuk olabilir).");
}

void App::saveFile() {
    std::string path = pfd::save_file("Parametreleri kaydet", "seismic_parameters.json", {"JSON (*.json)", "*.json"}).result();
    if (path.empty()) return;
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json") path += ".json";
    if (writeTextFile(path, sim_.configJson())) notify("Parametreler kaydedildi: " + baseName(path));
    else alert("Dosya yazılamadı: " + path);
}

void App::loadFile() {
    auto files = pfd::open_file("Parametre dosyası aç", "", {"JSON (*.json)", "*.json"}).result();
    if (files.empty()) return;
    std::string text, err;
    if (!readTextFile(files[0], text)) { alert("Dosya okunamadı: " + files[0]); return; }
    if (sim_.applyConfigJson(text, err)) notify("Parametreler yüklendi: " + baseName(files[0]));
    else alert("Bu dosya geçerli bir parametre dosyası değil (JSON okunamadı).");
}

} // namespace ui
