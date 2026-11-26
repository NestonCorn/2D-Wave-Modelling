#include "ModelCanvas.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>

void ModelCanvas::initDefault(int width, int height) {
    palette = {{"Su", 1500, 0x2E6DB4},         {"Tortul 1", 2000, 0xD8C48E}, {"Tortul 2", 2400, 0xB0925E},
               {"Kireçtaşı", 3200, 0x93A9AE},  {"Tuz", 4500, 0xEEEEE6},      {"Temel kaya", 5000, 0x6E5252}};
    w = width;
    h = height;
    px.assign((size_t)w * h, 0);
    for (int y = h * 35 / 100; y < h; y++)
        for (int x = 0; x < w; x++) px[(size_t)y * w + x] = 1;
    undo_.clear();
    redo_.clear();
}

void ModelCanvas::resize(int width, int height) {
    if (width == w && height == h) return;
    std::vector<uint8_t> n((size_t)width * height);
    for (int y = 0; y < height; y++) {
        const int sy = std::min(h - 1, (int)((y + 0.5) * h / height));
        for (int x = 0; x < width; x++) n[(size_t)y * width + x] = px[(size_t)sy * w + std::min(w - 1, (int)((x + 0.5) * w / width))];
    }
    w = width;
    h = height;
    px = std::move(n);
}

void ModelCanvas::brush(float cx, float cy, float r, uint8_t c) {
    r = std::max(r, 0.5f);
    for (int y = (int)std::floor(cy - r); y <= (int)std::ceil(cy + r); y++)
        for (int x = (int)std::floor(cx - r); x <= (int)std::ceil(cx + r); x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (inside(x, y) && dx * dx + dy * dy <= r * r) px[(size_t)y * w + x] = c;
        }
}

void ModelCanvas::line(float x0, float y0, float x1, float y1, float r, uint8_t c) {
    // Stamps closer than half a pixel: the line is 4-connected and blocks a flood fill
    const float len = std::hypot(x1 - x0, y1 - y0);
    const int n = std::max(1, (int)std::ceil(len / 0.4f));
    for (int i = 0; i <= n; i++) {
        const float t = (float)i / n;
        brush(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, std::max(r, 0.75f), c);
    }
}

void ModelCanvas::polyline(const std::vector<Pt>& p, float r, uint8_t c) {
    if (p.size() == 1) brush(p[0].first, p[0].second, r, c);
    for (size_t i = 1; i < p.size(); i++) line(p[i - 1].first, p[i - 1].second, p[i].first, p[i].second, r, c);
}

void ModelCanvas::fillBelow(std::vector<Pt> p, uint8_t c) {
    if (p.empty()) return;
    std::stable_sort(p.begin(), p.end(), [](const Pt& a, const Pt& b) { return a.first < b.first; });
    for (int x = 0; x < w; x++) {
        const float cx = x + 0.5f;
        float yb;
        if (cx <= p.front().first) yb = p.front().second;
        else if (cx >= p.back().first) yb = p.back().second;
        else {
            size_t k = 1;
            while (k < p.size() && p[k].first < cx) k++;
            const Pt& a = p[k - 1];
            const Pt& b = p[k];
            const float t = b.first > a.first ? (cx - a.first) / (b.first - a.first) : 0.0f;
            yb = a.second + t * (b.second - a.second);
        }
        for (int y = std::max(0, (int)std::floor(yb)); y < h; y++) px[(size_t)y * w + x] = c;
    }
}

int ModelCanvas::floodFill(int x, int y, uint8_t c) {
    if (!inside(x, y)) return 0;
    const uint8_t from = at(x, y);
    if (from == c) return 0;
    int n = 0;
    std::vector<std::pair<int, int>> st{{x, y}};
    while (!st.empty()) {   // scanline fill
        auto [sx, sy] = st.back();
        st.pop_back();
        if (at(sx, sy) != from) continue;
        int l = sx, r = sx;
        while (l > 0 && at(l - 1, sy) == from) l--;
        while (r < w - 1 && at(r + 1, sy) == from) r++;
        for (int i = l; i <= r; i++) { px[(size_t)sy * w + i] = c; n++; }
        for (int ny : {sy - 1, sy + 1}) {
            if (ny < 0 || ny >= h) continue;
            for (int i = l; i <= r; i++)
                if (at(i, ny) == from && (i == l || at(i - 1, ny) != from)) st.push_back({i, ny});
        }
    }
    return n;
}

void ModelCanvas::fillAll(uint8_t c) { std::fill(px.begin(), px.end(), c); }

void ModelCanvas::snapshot() {
    undo_.push_back({w, h, px});
    if (undo_.size() > 40) undo_.erase(undo_.begin());
    redo_.clear();
}

bool ModelCanvas::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({w, h, px});
    Snap s = std::move(undo_.back());
    undo_.pop_back();
    w = s.w; h = s.h; px = std::move(s.px);
    return true;
}

bool ModelCanvas::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({w, h, px});
    Snap s = std::move(redo_.back());
    redo_.pop_back();
    w = s.w; h = s.h; px = std::move(s.px);
    return true;
}

int ModelCanvas::addEntry(const PaletteEntry& e) {
    if (palette.size() >= 256) return -1;
    palette.push_back(e);
    return (int)palette.size() - 1;
}

void ModelCanvas::removeEntry(int i, int replaceWith) {
    if (i < 0 || i >= (int)palette.size() || palette.size() < 2) return;
    if (replaceWith == i) replaceWith = i == 0 ? 1 : 0;
    for (uint8_t& v : px) {
        if (v == i) v = (uint8_t)replaceWith;
        if (v > i) v--;   // indices above the removed entry move down
    }
    palette.erase(palette.begin() + i);
    // undo snapshots hold the old numbering
    undo_.clear();
    redo_.clear();
}

std::vector<int> ModelCanvas::counts() const {
    std::vector<int> c(palette.size(), 0);
    for (uint8_t v : px)
        if (v < c.size()) c[v]++;
    return c;
}

GridModel ModelCanvas::toModel(double widthM, double depthM, const std::string& name) const {
    GridModel m;
    m.nx = w;
    m.nz = h;
    m.dx = widthM / (w - 1);
    m.dz = depthM / (h - 1);
    m.source = name;
    m.v.resize((size_t)w * h);
    for (int x = 0; x < w; x++)
        for (int y = 0; y < h; y++) {
            const uint8_t i = px[(size_t)y * w + x];
            m.v[(size_t)x * h + y] = i < palette.size() ? palette[i].velocity : palette[0].velocity;
        }
    return m;
}

uint32_t colourForVelocity(float v) {
    // Water blue, then sand to rock browns, salt and basement light / dark
    struct Stop { float v; uint32_t c; };
    static const Stop s[] = {{1500, 0x2E6DB4}, {1800, 0x5FA3C7}, {2100, 0xD8C48E}, {2600, 0xB0925E},
                             {3200, 0x93A9AE}, {4000, 0xD9D2C0}, {4600, 0xEEEEE6}, {5500, 0x6E5252}};
    if (v <= s[0].v) return s[0].c;
    for (size_t k = 1; k < sizeof(s) / sizeof(s[0]); k++)
        if (v <= s[k].v) {
            const float t = (v - s[k - 1].v) / (s[k].v - s[k - 1].v);
            uint32_t out = 0;
            for (int sh : {16, 8, 0}) {
                const float a = (float)((s[k - 1].c >> sh) & 0xFF), b = (float)((s[k].c >> sh) & 0xFF);
                out |= (uint32_t)std::lround(a + (b - a) * t) << sh;
            }
            return out;
        }
    return s[7].c;
}

void ModelCanvas::fromVelocity(const float* vel, int nx, int nz, int width, int height) {
    // Distinct velocities (10 m/s steps) and how often they occur on the canvas
    std::vector<int> q((size_t)width * height);
    std::map<int, int> freq;
    for (int y = 0; y < height; y++) {
        const int k = std::min(nz - 1, (int)((y + 0.5) * nz / height));
        for (int x = 0; x < width; x++) {
            const int i = std::min(nx - 1, (int)((x + 0.5) * nx / width));
            const int v = (int)std::lround(vel[(size_t)k * nx + i] / 10.0f) * 10;
            q[(size_t)y * width + x] = v;
            freq[v]++;
        }
    }
    std::vector<std::pair<int, int>> byCount;   // (count, velocity)
    for (auto& [v, c] : freq) byCount.push_back({c, v});
    std::sort(byCount.rbegin(), byCount.rend());
    if (byCount.size() > 32) byCount.resize(32);
    std::vector<int> vs;
    for (auto& [c, v] : byCount) vs.push_back(v);
    std::sort(vs.begin(), vs.end());
    palette.clear();
    for (int v : vs) {
        char name[48];
        std::snprintf(name, sizeof(name), "%s %d", v <= 1600 ? "Su" : "Katman", v);
        palette.push_back({name, (float)v, colourForVelocity((float)v)});
    }
    w = width;
    h = height;
    px.resize((size_t)w * h);
    for (size_t p = 0; p < q.size(); p++) {
        int best = 0;
        for (int i = 1; i < (int)vs.size(); i++)
            if (std::abs(vs[i] - q[p]) < std::abs(vs[best] - q[p])) best = i;
        px[p] = (uint8_t)best;
    }
    undo_.clear();
    redo_.clear();
}

// ── Drawing file ──────────────────────────────────────────────────────────
std::string ModelCanvas::toJson(double widthM, double depthM) const {
    nlohmann::ordered_json j;
    j["format"] = "seismic-fdtd-model-drawing";
    j["version"] = 1;
    j["width_m"] = widthM;
    j["depth_m"] = depthM;
    j["columns"] = w;
    j["rows"] = h;
    auto pal = nlohmann::ordered_json::array();
    for (const PaletteEntry& e : palette) {
        char c[8];
        std::snprintf(c, sizeof(c), "#%06X", e.rgb & 0xFFFFFF);
        pal.push_back({{"name", e.name}, {"velocity_ms", e.velocity}, {"color", c}});
    }
    j["layers"] = pal;
    // Run lengths over the raster in row order: value, count, value, count, ...
    auto rle = nlohmann::ordered_json::array();
    for (size_t i = 0; i < px.size();) {
        size_t k = i;
        while (k < px.size() && px[k] == px[i]) k++;
        rle.push_back(px[i]);
        rle.push_back(k - i);
        i = k;
    }
    j["pixels_rle"] = rle;
    return j.dump(1);
}

bool ModelCanvas::fromJson(const std::string& text, double& widthM, double& depthM, std::string& err) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        err = "Dosya okunamadı (geçerli bir JSON değil).";
        return false;
    }
    try {
        if (j.value("format", std::string()) != "seismic-fdtd-model-drawing") { err = "Bu dosya bir model çizimi değil."; return false; }
        const int cw = j.at("columns").get<int>(), ch = j.at("rows").get<int>();
        const double mw = j.at("width_m").get<double>(), md = j.at("depth_m").get<double>();
        if (cw < 2 || ch < 2 || (long long)cw * ch > 100000000ll || !(mw > 0) || !(md > 0)) { err = "Çizimin boyutu geçersiz."; return false; }
        std::vector<PaletteEntry> pal;
        for (const auto& e : j.at("layers")) {
            PaletteEntry p;
            p.name = e.value("name", std::string("Katman"));
            p.velocity = e.at("velocity_ms").get<float>();
            const std::string c = e.value("color", std::string("#808080"));
            p.rgb = c.size() == 7 && c[0] == '#' ? (uint32_t)std::strtoul(c.c_str() + 1, nullptr, 16) : 0x808080;
            if (!(p.velocity > 0)) { err = "Bir katmanın hızı geçersiz: " + p.name; return false; }
            pal.push_back(p);
        }
        if (pal.empty() || pal.size() > 256) { err = "Katman sayısı 1 ile 256 arasında olmalı."; return false; }
        const auto& rle = j.at("pixels_rle");
        if (!rle.is_array() || rle.size() % 2) { err = "Çizimin piksel verisi bozuk."; return false; }
        std::vector<uint8_t> raster;
        raster.reserve((size_t)cw * ch);
        for (size_t i = 0; i < rle.size(); i += 2) {
            const int v = rle[i].get<int>();
            const long long n = rle[i + 1].get<long long>();
            if (v < 0 || v >= (int)pal.size() || n < 1 || (long long)raster.size() + n > (long long)cw * ch) {
                err = "Çizimin piksel verisi bozuk.";
                return false;
            }
            raster.insert(raster.end(), (size_t)n, (uint8_t)v);
        }
        if (raster.size() != (size_t)cw * ch) { err = "Çizimin piksel verisi eksik."; return false; }
        w = cw;
        h = ch;
        px = std::move(raster);
        palette = std::move(pal);
        widthM = mw;
        depthM = md;
        undo_.clear();
        redo_.clear();
        return true;
    } catch (const std::exception&) {
        err = "Dosyada beklenen alanlar eksik ya da hatalı.";
        return false;
    }
}
