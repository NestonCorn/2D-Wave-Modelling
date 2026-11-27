#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // before glad (APIENTRY); GlobalMemoryStatusEx
#endif
#include "Simulation.hpp"
#include <glad/glad.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include "Cpml.hpp"
#include "Material.hpp"
#include "ParamsIO.hpp"
#include "Colormap.hpp"
#include "Report.hpp"
#include "Segy.hpp"

namespace fs = std::filesystem;

namespace {
inline bool badFloat(float v, float limit) {
    uint32_t bits; std::memcpy(&bits, &v, 4);
    return (bits & 0x7f800000u) == 0x7f800000u || std::fabs(v) > limit;
}
std::string isoStampForFolder() {
    // new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tm);
    return buf;
}
double physicalRamBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return (double)m.ullTotalPhys;
#endif
    return 8e9;
}
const char* geoUpper(GeologyModel g) {
    return g == GeologyModel::Flat ? "FLAT" : g == GeologyModel::Fault ? "FAULT" : g == GeologyModel::Imported ? "IMPORTED" : "SALT";
}
// Sum of |FD coefficients| (CFL limit: v_n dt sqrt(2) sum <= 1)
double cSum(int order) {
    switch (order) { case 4: return 1.166; case 6: return 1.241; case 8: return 1.28; case 10: return 1.31; }
    return 1.0;
}
} // namespace

Simulation::Simulation() = default;

Simulation::~Simulation() {
    waitEikonal();
    cpu_.wait();
    if (glAvailable_) {
        gpu_.destroy();
        if (waveTex) glDeleteTextures(1, &waveTex);
        if (modelTex) glDeleteTextures(1, &modelTex);
    }
}

void Simulation::init(bool glAvailable) {
    glAvailable_ = glAvailable;
    if (glAvailable_) gpuCapable_ = GpuSolver::queryCapabilities(gpuRenderer_, gpuReason_, maxSsbo_);
    sxShip = p.cable_len + 20;
    sxSrc = sxShip + p.src_offset;
    regridNow();
}

// ══════════════════════════════════════════════════════════════════════════
// Grid / engines
// ══════════════════════════════════════════════════════════════════════════
void Simulation::regridNow() {
    waitInFlight(false);
    pendingSteps_ = 0;
    waitEikonal();

    // Memory guard: the sliders allow e.g. 25 km x 12 km at 0.5 m (1.2 billion cells,
    // ~65 GB). Coarsen the grid until the fields fit in half of the physical RAM
    // (~80 bytes per cell: 10 float fields, pixels, V^2 / density / material copies, eikonal tables).
    SCALE = p.dh;
    bool coarsened = false;
    for (;;) {
        // The sea surface on a grid row: the free surface (P = 0) is that row, and the model's
        // depth 0, the source and the receivers are measured from it (it was up to a cell off
        // the row the solver used). 16.6 % of the domain above it as before, to the nearest node;
        // a loaded model keeps its full depth below it.
        const bool loaded = p.geology == GeologyModel::Imported && hasModel();
        const double nominal = loaded ? modelVel_.depth() * 0.166 / (1.0 - 0.166) : p.domain_H * 0.166;
        OCEAN_Y = std::round(nominal / SCALE) * SCALE;
        if (loaded) p.domain_H = (float)(OCEAN_Y + modelVel_.depth());
        W_sim = (int)std::floor(p.domain_W / SCALE) + 1;
        H_sim = (int)std::floor(p.domain_H / SCALE) + 1;
        // The absorbing layers lie outside the model: npml cells on every side of the solver grid
        padX_ = padT_ = padB_ = std::max(0, p.npml);
        gW_ = W_sim + 2 * padX_;
        gH_ = padT_ + H_sim + padB_;
        if ((double)gW_ * gH_ * 80.0 <= 0.5 * physicalRamBytes() || SCALE >= 8.0) break;
        SCALE = std::min(8.0, SCALE * 2.0);
        coarsened = true;
    }
    if (coarsened) {
        p.dh = (float)SCALE;
        char msg[256];
        std::snprintf(msg, sizeof(msg), "Bu alan boyutu ve ızgara aralığı bilgisayarın belleğine sığmıyor. "
                      "Izgara aralığı otomatik olarak %g m yapıldı (%d x %d hücre).", SCALE, W_sim, H_sim);
        alerts.push_back(msg);
    }
    N_REC = (int)std::floor(p.cable_len / p.receiver_spacing + 1e-6) + 1;
    // The HTML only recomputed N_OBS when the OBS spacing changed; keeping it in
    // sync with the domain width avoids OBS stations outside the model.
    N_OBS = std::max(0, (int)std::floor((W_sim * SCALE) / p.obs_spacing));

    fillModel();
    computeObsPositions();
    updateCpml();
    if (glAvailable_) createWaveTexture();
    rebuildEngines();
    instability = false;
    eik_.clear();
    if (!shots.empty()) eikDirty_ = true;
}

void Simulation::rebuildEngines() {
    waitInFlight(false);
    const size_t sismoSize = (size_t)(N_REC * 2 + N_OBS) * kSismoStride;
    int hc = (int)std::thread::hardware_concurrency();
    if (hc <= 0) hc = 4;
    // The CPU engine runs the pipelined (temporally blocked) scheme: each thread advances its
    // own time step a few row bands behind the previous one. Measured on a Ryzen 5 3600 it is
    // fastest with 6-8 threads on every grid size (no per-step barriers any more); beyond
    // half the logical cores + 1, SMT / efficiency cores only add contention. Each thread
    // needs a few dozen rows of its own to stay out of its neighbours' way.
    const int nThreads = std::clamp(hc / 2 + 1, 1, std::max(1, gH_ / 16));
    cpu_.init(gW_, gH_, nThreads, sismoSize);
    cpu_.setMaterial(mat_);
    cpu_.setCpml(bx_, ax_, bz_, az_);
    threadStats.assign(cpu_.threadCount(), {});
    sismo.assign(sismoSize, 0.0f);
    initGPU();
    if (gpuReady) { gpu_.clearFields(); gpu_.clearPsi(); gpu_.clearSismo(); }
}

void Simulation::initGPU() {
    if (!glAvailable_) {
        gpuReady = false;
        badge = {BadgeKind::Cpu, "", "OpenGL bağlamı oluşturulamadı; CPU ile devam ediliyor."};
        return;
    }
    if (userForceCPU) {
        gpuReady = false;
        badge = {BadgeKind::ForcedCpu, "", "Kullanıcı isteğiyle GPU devre dışı bırakıldı."};
        return;
    }
    if (!gpuCapable_) {
        gpuReady = false;
        badge = {BadgeKind::Cpu, "", gpuReason_};
        return;
    }
    const long long bufBytes = (long long)gW_ * gH_ * 4;
    if (bufBytes > maxSsbo_ || bufBytes * 9.0 > 0.35 * physicalRamBytes()) {
        gpuReady = false;
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "ızgara çok büyük (%lldMB > %lldMB)", bufBytes / 1048576, maxSsbo_ / 1048576);
        badge = {BadgeKind::Warning, lbl,
                 "Donanım Sınırı Aşıldı: Modelin çözünürlüğü mevcut ekran kartınızın maksimum bellek tahsis "
                 "sınırını (Buffer Limit) aşıyor. CPU ile devam ediliyor."};
        return;
    }
    std::string err;
    if (gpu_.matches(gW_, gH_, N_REC, N_OBS)) {
        gpu_.setMaterial(mat_);
        gpu_.setCpml(bx_, ax_, bz_, az_);
        gpu_.setObs(obsXg_, obsYg_);
        if (gpu_.ensurePrograms(p.fd_order, err)) {
            badge = {BadgeKind::Gpu, gpuRenderer_, "Hazır: Önceki OpenGL yapılandırması yeniden kullanılıyor."};
            gpuReady = true;
            return;
        }
    } else if (gpu_.init(gW_, gH_, N_REC, N_OBS, err)) {
        gpu_.setMaterial(mat_);
        gpu_.setCpml(bx_, ax_, bz_, az_);
        gpu_.setObs(obsXg_, obsYg_);
        if (gpu_.ensurePrograms(p.fd_order, err)) {
            badge = {BadgeKind::Gpu, gpuRenderer_, "OpenGL compute motoru başarıyla başlatıldı ve çalışıyor."};
            gpuReady = true;
            return;
        }
    }
    gpu_.destroy();
    gpuReady = false;
    badge = {BadgeKind::Cpu, "", "GPU Başlatılamadı: " + err};
}

void Simulation::updateCpml() {
    cpmlX(gW_, padX_, p.dt, bx_, ax_, vmaxNorm_);
    cpmlZ(gH_, padB_, p.dt, OCEAN_Y + padT_ * SCALE, SCALE, p.free_surface, bz_, az_, vmaxNorm_);
}

void Simulation::computeObsPositions() {
    obsX_.resize(N_OBS);
    obsY_.resize(N_OBS);
    for (int i = 0; i < N_OBS; i++) {
        double rx = (i + 1) * (double)p.obs_spacing;
        obsX_[i] = (float)rx;
        obsY_[i] = (float)(seafloorDepth(rx) + 4.0);   // slight offset below seabed
    }
    obsXg_.resize(N_OBS);
    obsYg_.resize(N_OBS);
    for (int i = 0; i < N_OBS; i++) {
        obsXg_[i] = obsX_[i] + (float)(padX_ * SCALE);
        obsYg_[i] = obsY_[i] + (float)(padT_ * SCALE);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Model: velocity, density and material on the grid
// ══════════════════════════════════════════════════════════════════════════
void Simulation::fillModel() {
    const size_t N = (size_t)W_sim * H_sim;
    if (p.geology == GeologyModel::Imported && !hasModel()) p.geology = GeologyModel::Flat;
    if (p.geology == GeologyModel::Imported) {
        // z = 0 of the model is the sea surface; the strip above it takes the top values
        vsq.resize(N);
        rho.resize(N);
        const GridModel& mv = modelVelS_.empty() ? modelVel_ : modelVelS_;   // boundaries without the pixel steps
        const GridModel& mr = modelRho_.empty() ? modelRhoS_ : modelRho_;
        const bool haveRho = !mr.empty();
        // Cell average: n x n samples of the (bilinear) model over the cell around each node,
        // two per model sample at least; the cell takes the mean bulk modulus and the mean
        // density (see fillPresetModel for why not the harmonic mean). n = 1 is the value at
        // the node.
        int n = 1;
        if (p.cell_average) {
            const double pix = std::max(1e-6, std::min(modelVel_.dx, modelVel_.dz));
            n = std::clamp((int)std::ceil(2.0 * SCALE / pix - 1e-9), 1, 8);
            while (n > 2 && (double)N * n * n > 2.5e8) n--;   // keep the load under a few seconds
        }
        auto rows = [&](int j0, int j1) {
            for (int j = j0; j < j1; j++)
                for (int i = 0; i < W_sim; i++) {
                    double sr = 0, sK = 0;
                    for (int a = 0; a < n; a++) {
                        const double z = std::max(0.0, j * SCALE - OCEAN_Y + (n > 1 ? SCALE * ((a + 0.5) / n - 0.5) : 0.0));
                        for (int b = 0; b < n; b++) {
                            const double x = i * SCALE + (n > 1 ? SCALE * ((b + 0.5) / n - 0.5) : 0.0);
                            const double v = mv.sample(x, z), vn = v / kVelocityNorm;
                            const double r = haveRho ? mr.sample(x, z) : gardnerDensity(v);
                            sr += r;
                            sK += r * vn * vn;
                        }
                    }
                    const size_t idx = (size_t)j * W_sim + i;
                    vsq[idx] = (float)(sK / sr);   // mean K / mean rho
                    rho[idx] = (float)(sr / ((double)n * n));
                }
        };
        const int nt = n > 1 ? std::clamp((int)std::thread::hardware_concurrency(), 1, 16) : 1;
        std::vector<std::thread> pool;
        for (int t = 1; t < nt; t++) pool.emplace_back(rows, H_sim * t / nt, H_sim * (t + 1) / nt);
        rows(0, H_sim / nt);
        for (auto& th : pool) th.join();
        // Seafloor: first depth below the surface where the velocity leaves the water range
        layout = GeoLayout{};
        const int nPts = 400;
        const double step = std::max(0.25, std::min(modelVel_.dz, SCALE) * 0.5);
        for (int k = 0; k < nPts; k++) {
            const double x = p.domain_W * k / (nPts - 1);
            double z = 0;
            while (z < modelVel_.depth() && modelVel_.sample(x, z) <= 1600.0) z += step;
            layout.seafloor.push_back({(float)x, (float)(OCEAN_Y + z)});
        }
    } else {
        fillPresetModel(vsq, rho, W_sim, H_sim, SCALE, p.geology, p.domain_W, p.domain_H, p.cell_average);
        layout = buildLayout(p.geology, p.domain_W, p.domain_H);
    }
    float mx = 0.0f, mn = 1e30f;
    for (float x : vsq) { mx = std::max(mx, x); mn = std::min(mn, x); }
    vmaxNorm_ = std::max(0.68, std::sqrt((double)mx));
    vminMs_ = std::min(1500.0, std::sqrt((double)mn) * kVelocityNorm);
    // Solver grid: the model's edge values carried out into the absorbing pads
    vsqG_.resize((size_t)gW_ * gH_);
    rhoG_.resize((size_t)gW_ * gH_);
    for (int j = 0; j < gH_; j++) {
        const int mj = std::clamp(j - padT_, 0, H_sim - 1);
        for (int i = 0; i < gW_; i++) {
            const size_t m = (size_t)mj * W_sim + std::clamp(i - padX_, 0, W_sim - 1), g = (size_t)j * gW_ + i;
            vsqG_[g] = vsq[m];
            rhoG_[g] = rho[m];
        }
    }
    mat_ = makeMaterial(vsqG_, rhoG_, gW_, gH_);
    if (glAvailable_) createModelTexture();
}

void Simulation::rebuildSmoothModel() {
    modelVelS_ = GridModel{};
    modelRhoS_ = GridModel{};
    smoothStats_ = BoundaryStats{};
    if (!p.smooth_boundaries || modelVel_.empty()) return;
    GridModel s = modelVel_, r;
    // Without a density model of its own, the density goes with the smoothed copy: the boundary
    // pixels need the mixed density too (the Gardner density jumps at 1600 m/s)
    smoothStats_ = smoothBoundaries(s, modelRho_.empty() ? &r : nullptr);
    if (smoothStats_.pixels > 0) {
        modelVelS_ = std::move(s);
        modelRhoS_ = std::move(r);
    }
}

void Simulation::onSmoothBoundaries() {
    rebuildSmoothModel();
    if (p.geology == GeologyModel::Imported) onGeology();
}

std::vector<float> Simulation::nodeVelocities(int& j0, int& nz) const {
    j0 = std::clamp((int)std::ceil(OCEAN_Y / SCALE - 1e-9), 0, std::max(0, H_sim - 2));
    nz = H_sim - j0;
    std::vector<float> out((size_t)W_sim * nz);
    if (p.geology == GeologyModel::Imported && hasModel()) {
        for (int k = 0; k < nz; k++)
            for (int i = 0; i < W_sim; i++)
                out[(size_t)k * W_sim + i] = modelVel_.sample(i * SCALE, std::max(0.0, (j0 + k) * SCALE - OCEAN_Y));
    } else {
        std::vector<float> v;
        fillVsq(v, W_sim, H_sim, SCALE, p.geology, p.domain_W, p.domain_H);
        for (int k = 0; k < nz; k++)
            for (int i = 0; i < W_sim; i++)
                out[(size_t)k * W_sim + i] = (float)(std::sqrt((double)v[(size_t)(j0 + k) * W_sim + i]) * kVelocityNorm);
    }
    return out;
}

void Simulation::pushMaterial() {
    updateCpml();
    cpu_.setMaterial(mat_);
    cpu_.setCpml(bx_, ax_, bz_, az_);
    if (gpuReady) {
        gpu_.setMaterial(mat_);
        gpu_.setCpml(bx_, ax_, bz_, az_);
    }
}

double Simulation::seafloorDepth(double x) const {
    if (p.geology != GeologyModel::Imported || layout.seafloor.size() < 2)
        return obsSeafloorDepth(p.geology, x, p.domain_W, p.domain_H);
    const auto& s = layout.seafloor;
    if (x <= s.front().x) return s.front().y;
    if (x >= s.back().x) return s.back().y;
    for (size_t k = 1; k < s.size(); k++)
        if (x <= s[k].x) {
            const double t = (x - s[k - 1].x) / std::max(1e-9, (double)(s[k].x - s[k - 1].x));
            return s[k - 1].y + t * (s[k].y - s[k - 1].y);
        }
    return s.back().y;
}

// Velocity image of an imported model (the built-in ones are drawn as layer polygons)
void Simulation::createModelTexture() {
    if (modelTex) { glDeleteTextures(1, &modelTex); modelTex = 0; }
    if (vsq.empty()) return;
    // Every model, preset or loaded, coloured by velocity, so the colour bar reads true
    const int jSurf = sjSurf();
    float lo = 1e30f, hi = 0;
    for (int j = std::max(0, jSurf); j < H_sim; j++)
        for (int i = 0; i < W_sim; i++) {
            const float v = (float)(std::sqrt((double)vsq[(size_t)j * W_sim + i]) * kVelocityNorm);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (hi < lo) lo = hi = 1500;
    viewVelRange_ = {lo, hi};
    const double span = std::max(1.0, (double)hi - lo);
    const ModelColormap cm = (ModelColormap)std::clamp(p.model_colormap, 0, (int)ModelColormap::Count - 1);
    std::vector<uint32_t> px((size_t)W_sim * H_sim);
    for (int j = 0; j < H_sim; j++)
        for (int i = 0; i < W_sim; i++) {
            const size_t idx = (size_t)j * W_sim + i;
            if (j < jSurf) { px[idx] = 0; continue; }   // sky stays the scene's own
            const double v = std::sqrt((double)vsq[idx]) * kVelocityNorm;
            const uint32_t c = colormapRGB(cm, (v - lo) / span);   // 0xRRGGBB -> RGBA bytes
            px[idx] = (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16 | 0xFFu << 24;
        }
    glGenTextures(1, &modelTex);
    glBindTexture(GL_TEXTURE_2D, modelTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W_sim, H_sim, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool Simulation::loadModel(GridModel vel, GridModel rhoModel, std::string& err) {
    if (vel.empty()) { err = "Hız modeli boş."; return false; }
    const double W = vel.width(), D = vel.depth();
    if (W < 100.0 || D < 50.0) {
        err = "Model en az 100 m genişliğinde ve 50 m derinliğinde olmalı (okunan: " + jsNum(std::round(W)) + " m x " +
              jsNum(std::round(D)) + " m). dx ve dz değerlerini kontrol edin.";
        return false;
    }
    const double H = D / (1.0 - 0.166);   // the domain keeps 16.6 % above the sea surface
    if (W > 50000.0 || H > 20000.0) {
        err = "Model çok büyük (" + jsNum(std::round(W)) + " m x " + jsNum(std::round(D)) +
              " m); en fazla 50 km genişlik ve yaklaşık 16 km derinlik desteklenir.";
        return false;
    }
    if (!rhoModel.empty() && (std::fabs(rhoModel.width() - W) > 0.01 * W + rhoModel.dx ||
                              std::fabs(rhoModel.depth() - D) > 0.01 * D + rhoModel.dz)) {
        alerts.push_back("Yoğunluk modelinin boyutu (" + jsNum(std::round(rhoModel.width())) + " m x " +
                         jsNum(std::round(rhoModel.depth())) + " m) hız modelinden (" + jsNum(std::round(W)) + " m x " +
                         jsNum(std::round(D)) + " m) farklı. Yoğunluk aynı fiziksel konumlardan okundu; "
                         "eksik kalan kenarlar en yakın değerle uzatıldı.");
    }
    stopMode();
    modelVel_ = std::move(vel);
    modelRho_ = std::move(rhoModel);
    rebuildSmoothModel();
    // Ranges once here: the panel shows them every frame, and a scan of a large image model per
    // frame (6 Mpx: ~2 ms) dropped the frame rate, and with it the simulation speed
    velRange_ = {modelVel_.minValue(), modelVel_.maxValue()};
    rhoRange_ = modelRho_.empty() ? std::pair<float, float>{0, 0} : std::pair<float, float>{modelRho_.minValue(), modelRho_.maxValue()};
    p.geology = GeologyModel::Imported;
    p.domain_W = (float)W;
    p.domain_H = (float)H;
    OCEAN_Y = p.domain_H * 0.166;
    // Stability with the model's fastest velocity
    const double vmaxN = std::max(0.68, velRange_.second / kVelocityNorm);
    const double dtMax = 1.0 / (vmaxN * std::sqrt(2.0) * cSum(p.fd_order));
    if (p.dt > 0.95 * dtMax) {
        const float old = p.dt;
        p.dt = (float)(std::floor(0.9 * dtMax * 100.0) / 100.0);
        char b[256];
        std::snprintf(b, sizeof(b), "Modelin en yüksek hızı %.0f m/s. Kararlılık için zaman katsayısı %.2f'den %.2f'ye "
                      "düşürüldü (sınır %.2f).", velRange_.second, old, p.dt, dtMax);
        alerts.push_back(b);
    }
    regridPending_ = false;
    regridNow();
    sxShip = std::clamp(sxShip, 0.0, (double)p.domain_W);
    sxSrc = sxShip + p.src_offset;
    return true;
}

void Simulation::clearModel() {
    stopMode();
    modelVel_ = GridModel{};
    modelRho_ = GridModel{};
    modelVelS_ = GridModel{};
    modelRhoS_ = GridModel{};
    smoothStats_ = BoundaryStats{};
    velRange_ = rhoRange_ = {0, 0};
    const SimParams def;
    p.geology = GeologyModel::Flat;
    p.domain_W = def.domain_W;
    p.domain_H = def.domain_H;
    OCEAN_Y = p.domain_H * 0.166;
    regridPending_ = false;
    regridNow();
    sxShip = std::clamp(sxShip, 0.0, (double)p.domain_W);
    sxSrc = sxShip + p.src_offset;
}

void Simulation::createWaveTexture() {
    if (waveTex) glDeleteTextures(1, &waveTex);
    glGenTextures(1, &waveTex);
    glBindTexture(GL_TEXTURE_2D, waveTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, gW_, gH_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    clearWaveTexture();
}

void Simulation::clearWaveTexture() {
    if (!glAvailable_ || !waveTex) return;
    zeroTex_.assign((size_t)gW_ * gH_, 0u);
    glBindTexture(GL_TEXTURE_2D, waveTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gW_, gH_, GL_RGBA, GL_UNSIGNED_BYTE, zeroTex_.data());
    zeroTex_.clear();
    zeroTex_.shrink_to_fit();
}

void Simulation::clearAllFields() {
    waitInFlight(false);
    cpu_.clearFields();
    cpu_.clearPsi();
    cpu_.clearPixels();
    if (gpuReady) { gpu_.clearFields(); gpu_.clearPsi(); }
    clearWaveTexture();
}

void Simulation::clearSismoAll() {
    cpu_.clearSismo();
    if (gpuReady) gpu_.clearSismo();
    std::fill(sismo.begin(), sismo.end(), 0.0f);
}

// ══════════════════════════════════════════════════════════════════════════
// Modes
// ══════════════════════════════════════════════════════════════════════════
void Simulation::startMode(Mode m) {
    glide_.active = false;
    elapsedSec = 0;
    waitInFlight(false);
    pendingSteps_ = 0;
    instability = false;
    paused = false;
    mode = m;
    cycle_t = 0;
    simTime_ = 0;
    accumulated_ = 0;
    shots.clear();
    shotsMain = shotsSrc = 0;

    if (!userForceCPU && !gpuReady) initGPU();
    clearAllFields();
    clearSismoAll();
    lastMode = m;

    fireShot(true, 0);
    if (m == Mode::SLO) fireShot(false, 25);
}

void Simulation::stopMode() {
    glide_.active = false;
    elapsedSec = 0;
    waitInFlight(false);
    pendingSteps_ = 0;
    mode = Mode::None;
    shots.clear();
    shotsMain = shotsSrc = 0;
    cycle_t = 0;
    simTime_ = 0;
    accumulated_ = 0;
    instability = false;
    paused = false;
    clearAllFields();
    clearSismoAll();
}

void Simulation::togglePause() {
    if (mode == Mode::None) return;
    paused = !paused;
}

void Simulation::toggleComputeEngine() {
    userForceCPU = !userForceCPU;
    const Mode cur = mode;
    if (cur != Mode::None) stopMode();
    if (userForceCPU) {
        gpuReady = false;
        badge = {BadgeKind::ForcedCpu, "", "Kullanıcı isteğiyle GPU devre dışı bırakıldı."};
    } else {
        initGPU();
        if (gpuReady) { gpu_.clearFields(); gpu_.clearPsi(); gpu_.clearSismo(); }
    }
    if (cur != Mode::None) startMode(cur);
}

void Simulation::toggleFreeSurface() {
    p.free_surface = !p.free_surface;
    waitInFlight(true);
    cpmlZ(gH_, padB_, p.dt, OCEAN_Y + padT_ * SCALE, SCALE, p.free_surface, bz_, az_, vmaxNorm_);
    cpu_.setCpmlZ(bz_, az_);
    // The HTML rewrote the GPU params here without the free-surface/geology arguments,
    // silently disabling the free surface on GPU. Both engines read the flag per batch now.
    if (gpuReady) gpu_.setCpmlZ(bz_, az_);
}

void Simulation::toggleObs() { p.obs_enabled = !p.obs_enabled; }

void Simulation::onModelColormap() {
    if (glAvailable_) createModelTexture();
}

// ══════════════════════════════════════════════════════════════════════════
// Setting handlers
// ══════════════════════════════════════════════════════════════════════════
void Simulation::onFdOrder() {
    clearAllFields();
    if (gpuReady) {
        std::string err;
        if (!gpu_.ensurePrograms(p.fd_order, err)) {
            gpuReady = false;
            badge = {BadgeKind::Cpu, "", "GPU Başlatılamadı: " + err};
        }
    }
}

void Simulation::onGridSize(double now) { regridPending_ = true; regridAt_ = now + 0.1; }
void Simulation::onDomainWidth(double now) { onGridSize(now); }
void Simulation::onDomainHeight(double now) {
    OCEAN_Y = p.domain_H * 0.166;
    onGridSize(now);
}

void Simulation::onTimeStep() {
    waitInFlight(true);
    updateCpml();
    cpu_.setCpml(bx_, ax_, bz_, az_);
    cpu_.clearPsi();   // respawned workers start with fresh CPML memory
    if (gpuReady) { gpu_.setCpml(bx_, ax_, bz_, az_); gpu_.clearPsi(); }   // the GPU kept stale psi before
}

void Simulation::onNpml() {
    // The absorbing layers are pads around the model: their thickness sets the solver grid
    waitInFlight(false);
    stopMode();
    regridNow();
}

void Simulation::onCableLen() { onReceiverSpacing(false); }

void Simulation::onReceiverSpacing(bool forceReset) {
    const int n = (int)std::floor(p.cable_len / p.receiver_spacing + 1e-6) + 1;
    if (n == N_REC && !forceReset) return;
    N_REC = n;
    if (mode != Mode::None) stopMode();
    rebuildEngines();
}

void Simulation::onShipX(double x) {
    waitInFlight(true);   // otherwise the running batch overwrites the new position when it completes
    if (glide_.active) { glide_.fromX += x - sxShip; glide_.toX += x - sxShip; }   // the glide moves with the ship
    sxShip = x;
    sxSrc = sxShip + p.src_offset;
}

void Simulation::onSourceOffset() {
    waitInFlight(true);
    sxSrc = sxShip + p.src_offset;
}

void Simulation::onGeology() {
    waitInFlight(true);
    waitEikonal();
    fillModel();
    computeObsPositions();
    pushMaterial();
    if (gpuReady) gpu_.setObs(obsXg_, obsYg_);
    // Reset the wavefield to avoid instant blow-ups on layout changes over existing energy
    clearAllFields();
    if (!shots.empty()) eikDirty_ = true;
}

void Simulation::onObsSpacing() {
    N_OBS = std::max(0, (int)std::floor((W_sim * SCALE) / p.obs_spacing));
    computeObsPositions();
    const Mode cur = mode;
    if (cur != Mode::None) stopMode();
    rebuildEngines();
    if (cur != Mode::None) startMode(cur);
}

// ══════════════════════════════════════════════════════════════════════════
// Main loop (port of loop/cpuLoop/gpuLoop)
// ══════════════════════════════════════════════════════════════════════════
void Simulation::update(double now, double frameDt) {
    now_ = now;
    if (mode != Mode::None && !paused && !instability) elapsedSec += frameDt;

    if (regridPending_ && now >= regridAt_) {
        regridPending_ = false;
        regridNow();
    }

    if (inflight_.active) {
        bool done = inflight_.gpu ? (gpu_.poll() || !gpu_.busy()) : !cpu_.busy();
        if (!done) return;
        completeBatch();
    }

    if (line_.active && instability) {
        finishLine(false, "Simülasyon kararsızlaştı (sayısal patlama). Zaman katsayısını düşürüp yeniden deneyin");
        return;
    }
    if (mode == Mode::None || paused || instability) return;

    if (eikBusy_) {
        if (eikFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        finishEikonal();
    }
    if (pendingSteps_ > 0) {
        if (eikDirty_) { startEikonal(); if (eikBusy_) return; }
        int s = pendingSteps_;
        pendingSteps_ = 0;
        submitBatch(s);
        return;
    }
    if (line_.active) lineIteration();
    else loopIteration();
}

bool Simulation::glideStep() {
    if (!glide_.active) return false;
    constexpr double kGlideSec = 0.4;
    const double u = std::clamp((now_ - glide_.t0) / kGlideSec, 0.0, 1.0);
    const double e = 1.0 - std::pow(1.0 - u, 3.0);   // ease-out
    sxShip = glide_.fromX + (glide_.toX - glide_.fromX) * e;
    sxSrc = sxShip + p.src_offset;
    if (u < 1.0) return true;
    glide_.active = false;
    cycle_t = glide_.target;
    simTime_ = cycle_t * dtPhy();
    accumulated_ = 0;
    if (glide_.fireSrc) fireShot(false, 0);
    return false;
}

void Simulation::loopIteration() {
    if (glideStep()) return;
    const double dt_phy = dtPhy();
    const double spf = (p.speed_ms / 1000.0) / dt_phy;
    accumulated_ += spf;
    double stepsD = std::floor(accumulated_);
    if (stepsD < 1) return;
    accumulated_ -= stepsD;

    const double mps = p.ship_speed_knots * 0.514444;
    const double cur_t_max = cloSwitchStep(mps);
    const double max_time = mode == Mode::CLO ? cur_t_max * 2 : cur_t_max;
    const double T_sismo = std::floor((p.sismo_ms * 6.0) / (SCALE * p.dt));

    const bool is_shot2 = (mode == Mode::CLO && cycle_t >= cur_t_max);
    const double t_local = is_shot2 ? cycle_t - cur_t_max : cycle_t;

    // Auto-save SEG-Y exactly when the recording length is reached. In CLO the save
    // waits for the second ship's record: the HTML saved after the first half only,
    // so the second ship's streamer gather was never written.
    const bool recordDone = mode == Mode::CLO ? (is_shot2 && t_local >= T_sismo) : (t_local >= T_sismo);
    if (segyRecording_ && recordDone && !segyShotSaved_) {
        segyShotSaved_ = true;
        segySnapshotAndSave();
    }

    // Record complete, ship still short of the next shot: skip the idle sailing (the next
    // shot clears the field and the recordings anyway). CLO has two records per cycle.
    if (p.skip_gap && std::isfinite(cur_t_max)) {
        const bool second = mode == Mode::CLO && cycle_t >= cur_t_max;
        const double recEnd = second ? cur_t_max + T_sismo : T_sismo;
        const double target = mode == Mode::CLO && !second ? cur_t_max : max_time;
        if (target > recEnd) {
            if (cycle_t >= recEnd && cycle_t < target) {
                shots.clear();          // a long low-frequency pulse must not go on into a cleared field
                clearAllFields();
                glide_.active = true;
                glide_.t0 = now_;
                glide_.fromX = sxShip;
                glide_.toX = sxShip + (target - cycle_t) * mps * dt_phy;
                glide_.target = target;
                glide_.fireSrc = mode == Mode::CLO && !second;
                accumulated_ = 0;
                return;
            }
            if (cycle_t < recEnd && cycle_t + stepsD > recEnd) {   // end the batch on the record end
                accumulated_ += cycle_t + stepsD - recEnd;
                stepsD = recEnd - cycle_t;
            }
        }
    }

    if (cycle_t >= max_time) {
        cycle_t = std::fmod(cycle_t, max_time);
        simTime_ = cycle_t * dt_phy;
        shots.clear();
        // Shot interval shorter than the recording length: save what we have first
        if (segyRecording_ && !segyShotSaved_) segySnapshotAndSave();
        segyShotSaved_ = false;
        clearAllFields();
        clearSismoAll();
        fireShot(true, 0);
        if (mode == Mode::SLO) fireShot(false, 25);
    }

    // Never overshoot max_time in a single batch (no clear mid-propagation)
    if (cycle_t + stepsD > max_time) {
        double overflow = (cycle_t + stepsD) - max_time;
        stepsD -= overflow;
        accumulated_ += overflow;
        if (stepsD < 1) return;
    }
    if (stepsD > MAX_STEPS) {
        accumulated_ += stepsD - MAX_STEPS;
        stepsD = MAX_STEPS;
    }
    accumulated_ = std::min(accumulated_, (double)MAX_STEPS);   // don't build an endless backlog
    const int steps = (int)stepsD;

    // CLO: fire the second ship when the first half completes (range check, multi-step jumps)
    // The shot starts exactly at cur_t_max, where its record starts (the HTML used the batch
    // start, shifting the second gather by up to one batch).
    if (mode == Mode::CLO && cycle_t < cur_t_max && cycle_t + steps >= cur_t_max) fireShot(false, cur_t_max - cycle_t);

    if (eikDirty_) {
        startEikonal();
        if (eikBusy_) { pendingSteps_ = steps; return; }
    }
    submitBatch(steps);
}

void Simulation::submitBatch(int steps) {
    const double dt_phy = dtPhy();
    const double mps = p.ship_speed_knots * 0.514444;
    const double cur_t_max = cloSwitchStep(mps);
    const double T_sismo = std::floor((p.sismo_ms * 6.0) / (SCALE * p.dt));
    const int NS = sismoSamples(), KS = sismoStepsPerSample();

    boxFor(simTime_ + steps * dt_phy, batchBox_);
    const bool rowsValid = p.expanding_box && batchBox_.hasRows;

    // ── Per-step source / receiver data (_prepareStepData) ──
    stepData_.assign(steps, StepData{});
    const double ship_step = mps * dt_phy;
    double sx = sxShip, sxs = sxSrc;
    const double f = p.freq;
    if (!pulse_.matches(p.wavelet, f)) pulse_.build(p.wavelet, f);
    // The source between the nodes, where it is (the solvers spread it over the nodes around it);
    // it was moved down/left to the node below it, up to a cell (4 m on a 4 m grid)
    const double sj_srcF = (OCEAN_Y + p.source_depth) / SCALE + padT_;
    const int sj_src = (int)std::lround(sj_srcF);
    const double padXM = padX_ * SCALE, padTM = padT_ * SCALE;
    for (int s = 0; s < steps; s++) {
        StepData& d = stepData_[s];
        const double cyc = cycle_t + s + 1;
        sx += ship_step;
        sxs += ship_step;
        for (const Shot& sh : shots) {
            const double elapsed = (cyc - sh.t0) * dt_phy;
            const double val = pulse_(elapsed) * p.amp * p.dt * 0.4;
            const float si = (float)(sh.x / SCALE + padX_);   // fixed for the whole shot
            if (sh.main) { d.si0 = si; d.sj0 = (float)sj_srcF; d.val0 = (float)val; d.act0 = val != 0 ? 1.f : 0.f; }
            else         { d.si1 = si; d.sj1 = (float)sj_srcF; d.val1 = (float)val; d.act1 = val != 0 ? 1.f : 0.f; }
        }
        const bool shot2 = (mode == Mode::CLO && cyc >= cur_t_max);
        const double t_local = shot2 ? cyc - cur_t_max : cyc;
        int gy = (int)std::floor(t_local / KS);
        d.rec_valid = (mode != Mode::None && t_local <= T_sismo && gy >= 0 && gy < NS) ? 1.f : 0.f;
        d.gy_pix = (float)gy;
        {   // number of recorded steps (integer t_local in [1, T_sismo]) that map to sample gy
            const double lo = std::max(1.0, (double)gy * KS);
            const double hi = std::min(T_sismo, (double)gy * KS + KS - 1.0);
            d.rec_w = (float)(1.0 / std::max(1.0, hi - lo + 1.0));
        }
        d.sismo_off = shot2 ? (float)((size_t)N_REC * kSismoStride) : 0.f;
        d.sxShip = (float)sx;
        d.ry_head = (float)(OCEAN_Y + p.cable_head_depth + padTM);   // receivers in solver-grid metres
        d.ry_tail = (float)(OCEAN_Y + p.cable_tail_depth + padTM);
        d.cable_head_x = (float)(sx - p.ship_src_dist - p.src_rec_dist + padXM);
    }

    BatchJob& J = inflight_.job;
    J = BatchJob{};
    J.steps = steps;
    J.dt = p.dt;
    J.fd_order = p.fd_order;
    J.freeSurface = p.free_surface;
    J.sj_surf = sjSurfGrid();
    J.bxmin = batchBox_.min_i; J.bxmax = batchBox_.max_i;
    J.bymin = batchBox_.min_j; J.bymax = batchBox_.max_j;
    J.stepData = stepData_.data();
    J.probeRow = std::clamp(sj_src, 0, gH_ - 1);
    J.N_REC = N_REC; J.N_OBS = N_OBS;
    J.cable_len = (float)receiverSpan();   // the receivers exactly receiver_spacing apart, from the head back
    J.SCALE = (float)SCALE;
    J.obsX = obsXg_.data(); J.obsY = obsYg_.data();
    J.renderPixels = !p.sismo_only;
    J.gainNorm = p.gain / 15.0f;

    // Region coloured on screen (drawFDTDToScreen)
    const bool active = p.expanding_box && !shots.empty();
    // With the free surface off the sea surface is transparent and the field above it is drawn
    // (the CPU path already did this when the expanding box was on; the GPU never did).
    int drawMin = p.free_surface ? sjSurfGrid() : 0, drawMax = gH_ - 1;
    if (active) {
        drawMin = p.free_surface ? std::max(drawMin, batchBox_.min_j) : std::max(0, batchBox_.min_j);
        drawMax = std::min(gH_ - 1, batchBox_.max_j);
    }
    J.drawMinJ = drawMin; J.drawMaxJ = drawMax;
    if (rowsValid) { J.rowMin = batchBox_.xMinZ.data(); J.rowMax = batchBox_.xMaxZ.data(); }
    if (active) {
        J.dirtyX0 = std::max(0, batchBox_.min_i); J.dirtyX1 = std::min(gW_ - 1, batchBox_.max_i);
        J.clearMinJ = drawMin; J.clearMaxJ = drawMax;
    } else {
        J.dirtyX0 = 0; J.dirtyX1 = gW_ - 1;
        J.clearMinJ = 0; J.clearMaxJ = gH_ - 1;
    }

    bool useGpu = gpuReady;
    if (useGpu) {
        std::string err;
        if (!gpu_.ensurePrograms(p.fd_order, err)) {
            gpuReady = false;
            useGpu = false;
            badge = {BadgeKind::Cpu, "", "GPU Başlatılamadı: " + err};
        }
    }
    inflight_.active = true;
    inflight_.gpu = useGpu;
    inflight_.steps = steps;
    inflight_.dtPhy = dt_phy;
    inflight_.sxShip = sx;
    inflight_.sxSrc = sxs;
    if (useGpu) gpu_.submit(J, waveTex);
    else cpu_.submit(J);
}

void Simulation::completeBatch() {
    inflight_.active = false;
    cycle_t += inflight_.steps;
    simTime_ += inflight_.steps * inflight_.dtPhy;
    sxShip = inflight_.sxShip;
    sxSrc = inflight_.sxSrc;

    if (inflight_.gpu) {
        sismo = gpu_.sismo();
        if (gpu_.instabilityDetected()) setInstability();
    } else {
        sismo = cpu_.sismo();
        const auto& st = cpu_.lastStats();
        if (threadStats.size() != st.size()) threadStats.assign(st.size(), {});
        for (size_t i = 0; i < st.size(); i++) {
            threadStats[i].compute = threadStats[i].compute * 0.7 + st[i].compute * 0.3;
            threadStats[i].wait = threadStats[i].wait * 0.7 + st[i].wait * 0.3;
        }
        const BatchJob& J = inflight_.job;
        if (J.renderPixels && glAvailable_ && waveTex) {
            const int y0 = std::min(J.clearMinJ, J.drawMinJ), y1 = std::max(J.clearMaxJ, J.drawMaxJ);
            const int x0 = J.dirtyX0, x1 = J.dirtyX1;
            if (y1 >= y0 && x1 >= x0) {
                glBindTexture(GL_TEXTURE_2D, waveTex);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, gW_);
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0 + 1, y1 - y0 + 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                cpu_.pixels().data() + (size_t)y0 * gW_ + x0);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
        }
        if (cpu_.instabilityDetected()) setInstability();
    }
    checkInstabilityFromSismo();
}

void Simulation::checkInstabilityFromSismo() {
    if (instability) return;
    for (float v : sismo)
        if (badFloat(v, 1e10f)) { setInstability(); return; }
}

void Simulation::setInstability() { instability = true; }

void Simulation::waitInFlight(bool apply) {
    if (!inflight_.active) return;
    if (inflight_.gpu) gpu_.wait();
    else cpu_.wait();
    if (apply) completeBatch();
    else inflight_.active = false;
}

// ══════════════════════════════════════════════════════════════════════════
// Shots / eikonal expanding box
// ══════════════════════════════════════════════════════════════════════════
void Simulation::fireShot(bool main, double delay) {
    if (main) shotsMain++;
    else shotsSrc++;
    // Where the source will be when it fires (the ship keeps moving through the delay)
    const double x = (main ? sxShip : sxSrc) + delay * p.ship_speed_knots * 0.514444 * dtPhy() - p.ship_src_dist;
    shots.push_back({main, cycle_t + delay, x});
    eikDirty_ = true;
}

void Simulation::startEikonal() {
    eikDirty_ = false;
    waitEikonal();
    if (shots.empty()) { eik_.clear(); return; }
    std::vector<std::pair<int, int>> cells;
    const int sj = (int)std::lround((OCEAN_Y + p.source_depth) / SCALE) + padT_;   // solver grid, nearest node
    for (const Shot& sh : shots) {
        cells.push_back({(int)std::lround(sh.x / SCALE) + padX_, sj});
    }
    const double scale = SCALE, dt = p.dt;
    const int W = gW_, H = gH_;
    if ((long long)W * H < 400000) {   // small grids: solve inline (sub-millisecond)
        eik_.compute(vsqG_, W, H, scale, dt, cells, &eikProgress_);
        return;
    }
    eikProgress_ = 0;
    eikBusy_ = true;
    eikFuture_ = std::async(std::launch::async, [this, cells, scale, dt, W, H] {
        eik_.compute(vsqG_, W, H, scale, dt, cells, &eikProgress_);
    });
}

void Simulation::finishEikonal() {
    if (!eikBusy_) return;
    eikFuture_.get();
    eikBusy_ = false;
}

void Simulation::waitEikonal() {
    if (!eikBusy_) return;
    eikFuture_.wait();
    finishEikonal();
}

bool Simulation::boxFor(double t_sec, EikBox& out) const {
    if (!p.expanding_box || shots.empty() || eikBusy_ || !eik_.valid()) {
        out.min_i = 0; out.max_i = gW_ - 1; out.min_j = 0; out.max_j = gH_ - 1;
        out.hasRows = false;
        return false;
    }
    // Half a period of head start: the minimum-phase pulse carries energy from the shot
    // instant (the Ricker only from ~0.6/f), and the box edge clipped its weak numerical
    // precursor (box on/off traces differed by 2e-5; now within float noise).
    eik_.getBox(t_sec + 0.5 / std::max(0.1f, p.freq), p.fd_order, p.free_surface, sjSurfGrid(), out);
    return true;
}

bool Simulation::drawBox(EikBox& out) const { return boxFor(simTime_, out); }

NumericalStatus Simulation::numericalStatus() const {
    const double c_sum = cSum(p.fd_order);
    int min_ppw = 12;
    switch (p.fd_order) { case 4: min_ppw = 8; break; case 6: min_ppw = 6; break;
                          case 8: min_ppw = 5; break; case 10: min_ppw = 4; break; }
    NumericalStatus s;
    s.dt_max = 1.0 / (vmaxNorm_ * std::sqrt(2.0) * c_sum);   // 1.04 / c_sum for the built-in models (salt 4080 m/s)
    s.cfl_ratio = p.dt / s.dt_max;
    s.ppw = (vminMs_ / p.freq) / SCALE;   // slowest velocity in the model (water 1500 m/s for the built-in ones)
    s.min_ppw = min_ppw;
    return s;
}

// ══════════════════════════════════════════════════════════════════════════
// SEG-Y live recording
// ══════════════════════════════════════════════════════════════════════════
void Simulation::segyStart(const std::string& root) {
    if (mode == Mode::None) {
        alerts.push_back("Önce CLO veya SLO modunu başlatın, ardından butona tekrar basın.");
        return;
    }
    std::string m = modeName(mode);
    std::transform(m.begin(), m.end(), m.begin(), ::toupper);
    fs::path dir = fs::u8path(root) / ("SEGY_Live_" + m + "_" + isoStampForFolder());
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        alerts.push_back("Klasör seçme hatası: " + ec.message());
        return;
    }
    const std::u8string u8 = dir.u8string();
    segyDir_.assign(u8.begin(), u8.end());
    segyRecording_ = true;
    segyShotNum_ = 0;
    segyShotSaved_ = false;
    segyLabel_ = "🔴 Kayıt Aktif — Durdurmak için tıkla";
    segyLabelRevertAt_ = -1;
}

void Simulation::segyStop(double now) {
    segyRecording_ = false;
    segyDir_.clear();
    segyLabel_ = "💾 SEG-Y Dışa Aktar  (" + std::to_string(segyShotNum_) + " atış kaydedildi)";
    segyLabelRevertAt_ = now + 4.0;
}

const std::string& Simulation::segyButtonLabel(double now) {
    static const std::string idle = "💾 SEG-Y Dışa Aktar";
    if (segyRecording_) return segyLabel_;
    if (segyLabelRevertAt_ > 0 && now < segyLabelRevertAt_) return segyLabel_;
    return idle;
}

void Simulation::segySnapshotAndSave() {
    if (!segyRecording_ || sismo.empty()) return;
    const int sn = ++segyShotNum_;
    const int targetSamples = std::max(1, p.sismo_ms);   // 1.0 ms sample rate
    const int dtMicros = 1000;
    const std::string modeLabel = mode == Mode::CLO ? "CLO" : mode == Mode::SLO ? "SLO" : "NONE";

    const int ns = sismoSamples();
    auto snap1 = resampleGather(sismo.data(), 0, N_REC, kSismoStride, ns, targetSamples);
    auto snap2 = resampleGather(sismo.data(), (size_t)N_REC * kSismoStride, N_REC, kSismoStride, ns, targetSamples);
    auto snapO = resampleGather(sismo.data(), (size_t)N_REC * 2 * kSismoStride, N_OBS, kSismoStride, ns, targetSamples);
    auto anyNonZero = [](const std::vector<float>& v) {
        return std::any_of(v.begin(), v.end(), [](float x) { return x != 0.0f; });
    };

    SegySpec base;
    base.nSamples = targetSamples; base.dtMicros = dtMicros; base.shotNum = sn; base.modeLabel = modeLabel;
    base.sourceDepth = p.source_depth; base.fd_order = p.fd_order; base.freq = p.freq; base.SCALE = SCALE;
    base.domainW = p.domain_W; base.domainH = p.domain_H; base.oceanY = OCEAN_Y; base.geology = geoUpper(p.geology);

    const double head_x = sxShip - p.ship_src_dist - p.src_rec_dist;
    const double cable = receiverSpan(), tail = p.cable_tail_depth, headD = p.cable_head_depth;
    const int nrec = N_REC;
    auto streamerRx = [=](int i) {
        double n = nrec > 1 ? (double)i / (nrec - 1) : 0.0;
        return SegyReceiver{head_x - cable + i * (nrec > 1 ? cable / (nrec - 1) : 0.0), 0.0, tail + (headD - tail) * n};
    };

    char num[8];
    std::snprintf(num, sizeof(num), "%04d", sn);
    struct OutFile { std::string name; std::vector<uint8_t> data; };
    std::vector<OutFile> files;
    if (anyNonZero(snap1)) {
        SegySpec s = base;
        s.traces = snap1.data(); s.nTraces = N_REC; s.label = "MAIN STREAMER";
        s.sourceX = sxShip - p.ship_src_dist; s.receiver = streamerRx;
        files.push_back({std::string("shot_") + num + "_streamer_main.segy", buildSegy(s)});
    }
    if (anyNonZero(snap2)) {
        SegySpec s = base;
        s.traces = snap2.data(); s.nTraces = N_REC; s.label = "SOURCE STREAMER (CLO)";
        s.sourceX = sxSrc - p.ship_src_dist; s.receiver = streamerRx;
        files.push_back({std::string("shot_") + num + "_streamer_src.segy", buildSegy(s)});
    }
    if (p.obs_enabled && anyNonZero(snapO)) {
        SegySpec s = base;
        s.traces = snapO.data(); s.nTraces = N_OBS; s.label = "OBS GATHER";
        s.sourceX = (sxShip + sxSrc) / 2;
        const double sp = p.obs_spacing;
        s.receiver = [this, sp](int i) {
            double rx = (i + 1) * sp;
            return SegyReceiver{rx, 0.0, seafloorBelowSurface(rx)};
        };
        files.push_back({std::string("shot_") + num + "_obs.segy", buildSegy(s)});
    }
    if (files.empty()) return;

    for (const auto& f : files) {
        std::ofstream o(fs::u8path(segyDir_) / f.name, std::ios::binary);
        if (o) o.write((const char*)f.data.data(), (std::streamsize)f.data.size());
    }
    segyLabel_ = "🔴 Kayıt Aktif  |  " + std::to_string(sn) + ". Atış ✓";
}

// ══════════════════════════════════════════════════════════════════════════
// Line simulation
// ══════════════════════════════════════════════════════════════════════════
bool Simulation::startLine(const LineSettings& s, std::string& err) {
    if (line_.active) { err = "Bir hat simülasyonu zaten çalışıyor."; return false; }
    if (s.nShots < 1 || s.nShots > 100000) { err = "Atış sayısı 1 ile 100000 arasında olmalı."; return false; }
    if (!(s.interval > 0)) { err = "Atış aralığı sıfırdan büyük olmalı."; return false; }
    if (s.mode == Mode::None) { err = "Hat için bir atış kipi seçilmeli."; return false; }
    const double W = W_sim > 0 ? (W_sim - 1) * SCALE : p.domain_W;
    const double lastX = s.firstX + (s.nShots - 1) * s.interval;
    if (s.firstX < 0 || lastX > W) {
        char b[200];
        std::snprintf(b, sizeof(b), "Atışlar %.1f – %.1f m arasında; modelin içinde (0 – %.0f m) kalmalı.", s.firstX, lastX, W);
        err = b;
        return false;
    }
    if (s.mode != Mode::Trad && lastX + p.src_offset > W) {
        char b[220];
        std::snprintf(b, sizeof(b), "Kaynak gemisi ana kaynağın %.0f m önünde; son atışta %.0f m'de, modelin dışında kalıyor (model %.0f m).",
                      p.src_offset, lastX + p.src_offset, W);
        err = b;
        return false;
    }
    if (segyRecording_) segyStop(0);
    fs::path dir = fs::u8path(s.folder) / ("SEGY_Line_" + isoStampForFolder());
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { err = "Klasör oluşturulamadı: " + ec.message(); return false; }
    const std::u8string u8 = dir.u8string();
    line_.dir.assign(u8.begin(), u8.end());
    line_.streamer.close();
    line_.streamerSrc.close();
    line_.obs.close();
    line_.firstX = s.firstX;
    line_.interval = s.interval;
    line_.n = s.nShots;
    line_.mode = s.mode;
    line_.shot = 0;
    line_.savedShipX = sxShip;
    line_.active = true;
    writeLineExtras();   // model image, model SEG-Y and the PDF report
    beginLineShot();
    return true;
}

void Simulation::cancelLine() {
    if (!line_.active) return;
    finishLine(true, "");   // stopped on purpose: a confirmation, not an error
    if (!notices.empty()) notices.back() = "Hat simülasyonu durduruldu: " + std::to_string(line_.shot) + " / " +
                                          std::to_string(line_.n) + " atış kaydedildi.\n" + line_.dir;
}

double Simulation::lineProgress() const {
    if (!line_.active || line_.n <= 0) return 0.0;
    const double T = std::max(1.0, sismoSteps());
    const double total = line_.mode == Mode::CLO ? 2 * T : T;
    return std::clamp((line_.shot + std::min(1.0, cycle_t / total)) / line_.n, 0.0, 1.0);
}

double Simulation::cloSwitchStep(double mps) const {
    // A line records the source vessel right after the main record; the live loop fires it
    // when the ship has sailed one shot interval.
    if (line_.active) return sismoSteps();
    return mps > 0 ? std::floor((p.shot_interval * 6000.0) / (mps * SCALE * p.dt)) : std::numeric_limits<double>::infinity();
}

void Simulation::beginLineShot() {
    const double srcX = line_.firstX + line_.shot * line_.interval;
    waitInFlight(false);
    sxShip = srcX + p.ship_src_dist;   // the source hangs ship_src_dist behind the ship
    sxSrc = sxShip + p.src_offset;
    line_.phase = 0;
    line_.shotShipX = sxShip;
    line_.obsMain.clear();
    startMode(line_.mode);             // quiet wavefield, empty sismogram, fires the shot(s)
}

void Simulation::lineIteration() {
    const double T = sismoSteps();
    const bool clo = line_.mode == Mode::CLO;
    if (clo && line_.phase == 0 && cycle_t >= T) {
        // Main record complete: write it, keep its OBS gather (the source vessel's record
        // adds onto the same OBS samples), then record the source vessel's shot
        std::string err;
        if (!appendLineGathers(false, err)) { finishLine(false, err); return; }
        const size_t o = (size_t)N_REC * 2 * kSismoStride;
        line_.obsMain.assign(sismo.begin() + o, sismo.begin() + o + (size_t)N_OBS * kSismoStride);
        line_.phase = 1;
        line_.shotShipX = sxShip;
    }
    const double end = clo ? 2 * T : T;
    if (cycle_t >= end) {   // record complete (the last batch has been applied)
        std::string err;
        if (!appendLineGathers(clo, err)) { finishLine(false, err); return; }
        line_.shot++;
        if (line_.shot >= line_.n) { finishLine(true, ""); return; }
        beginLineShot();
        return;
    }
    // Batches end exactly at the record boundaries
    const double stop = clo && cycle_t < T ? T : end;
    const int steps = (int)std::clamp(stop - cycle_t, 1.0, (double)MAX_STEPS);
    if (clo && cycle_t < T && cycle_t + steps >= T) fireShot(false, T - cycle_t);
    if (eikDirty_) {
        startEikonal();
        if (eikBusy_) { pendingSteps_ = steps; return; }
    }
    submitBatch(steps);
}

// One record of the current station. srcVessel: the CLO source vessel's record (streamer
// slot 1, OBS = everything recorded minus the main record's gather).
bool Simulation::appendLineGathers(bool srcVessel, std::string& err) {
    const int sn = line_.shot + 1;
    const int targetSamples = std::max(1, p.sismo_ms);   // 1.0 ms sample rate, as the live export
    const int ns = sismoSamples();
    auto snapS = resampleGather(sismo.data(), srcVessel ? (size_t)N_REC * kSismoStride : 0, N_REC, kSismoStride, ns, targetSamples);

    // Source of this record: where it fired (the ship has moved on since)
    double srcX = line_.firstX + line_.shot * line_.interval;
    for (const Shot& sh : shots)
        if (sh.main == !srcVessel) { srcX = sh.x; break; }
    const char* kind = line_.mode == Mode::CLO ? "CLO" : line_.mode == Mode::SLO ? "SLO" : "TRAD";

    SegySpec base;
    base.nSamples = targetSamples; base.dtMicros = 1000; base.shotNum = sn; base.modeLabel = std::string("LINE ") + kind;
    base.sourceDepth = p.source_depth; base.fd_order = p.fd_order; base.freq = p.freq; base.SCALE = SCALE;
    base.domainW = p.domain_W; base.domainH = p.domain_H; base.oceanY = OCEAN_Y; base.geology = geoUpper(p.geology);
    base.sourceX = srcX;

    char note[200];
    std::snprintf(note, sizeof(note), "%d SHOTS  FIRST SOURCE X %.1fm  INTERVAL %.2fm  MODE %s  SPEED %.1fKN", line_.n,
                  line_.firstX, line_.interval, kind, p.ship_speed_knots);
    const std::string base_dir = line_.dir;

    {   // streamer, at its position when the shot fired
        const double head_x = line_.shotShipX - p.ship_src_dist - p.src_rec_dist;
        const double cable = receiverSpan(), tail = p.cable_tail_depth, headD = p.cable_head_depth;
        const int nrec = N_REC;
        SegySpec s = base;
        s.traces = snapS.data(); s.nTraces = N_REC; s.label = srcVessel ? "STREAMER (SOURCE VESSEL SHOT)" : "STREAMER";
        s.receiver = [=](int i) {
            double n = nrec > 1 ? (double)i / (nrec - 1) : 0.0;
            return SegyReceiver{head_x - cable + i * (nrec > 1 ? cable / (nrec - 1) : 0.0), 0.0, tail + (headD - tail) * n};
        };
        SegyLineWriter& w = srcVessel ? line_.streamerSrc : line_.streamer;
        if (!w.isOpen()) {
            const std::u8string u = (fs::u8path(base_dir) / (srcVessel ? "line_streamer_src.segy" : "line_streamer.segy")).u8string();
            if (!w.open(std::string(u.begin(), u.end()), s, note, err)) return false;
        }
        if (!w.append(s, err)) return false;
    }
    if (p.obs_enabled && N_OBS > 0) {
        const size_t o = (size_t)N_REC * 2 * kSismoStride;
        std::vector<float> obs(sismo.begin() + o, sismo.begin() + o + (size_t)N_OBS * kSismoStride);
        if (srcVessel && line_.obsMain.size() == obs.size())
            for (size_t i = 0; i < obs.size(); i++) obs[i] -= line_.obsMain[i];
        auto snapO = resampleGather(obs.data(), 0, N_OBS, kSismoStride, ns, targetSamples);
        SegySpec s = base;
        s.traces = snapO.data(); s.nTraces = N_OBS; s.label = srcVessel ? "OBS (SOURCE VESSEL SHOT)" : "OBS";
        const double sp = p.obs_spacing;
        s.receiver = [this, sp](int i) {
            double rx = (i + 1) * sp;
            return SegyReceiver{rx, 0.0, seafloorBelowSurface(rx)};
        };
        if (!line_.obs.isOpen()) {
            const std::u8string u = (fs::u8path(base_dir) / "line_obs.segy").u8string();
            if (!line_.obs.open(std::string(u.begin(), u.end()), s, note, err)) return false;
        }
        if (!line_.obs.append(s, err)) return false;
    }
    return true;
}

void Simulation::finishLine(bool ok, const std::string& why) {
    line_.streamer.close();
    line_.streamerSrc.close();
    line_.obs.close();
    line_.active = false;
    const int done = line_.shot;
    stopMode();
    sxShip = line_.savedShipX;
    sxSrc = sxShip + p.src_offset;
    if (ok)
        notices.push_back("Hat simülasyonu tamamlandı: " + std::to_string(done) + " atış kaydedildi.\n" + line_.dir);
    else
        alerts.push_back("Hat simülasyonu durdu: " + why + ".\n" + std::to_string(done) + " / " + std::to_string(line_.n) +
                         " atış kaydedildi: " + line_.dir);
}

// ══════════════════════════════════════════════════════════════════════════
// Parameter files
// ══════════════════════════════════════════════════════════════════════════
std::string Simulation::configJson() const { return paramsToJson(p, sxShip); }

bool Simulation::applyConfigJson(const std::string& text, std::string& err) {
    SimParams np = p;
    double shipX = sxShip;
    if (!paramsFromJson(text, np, shipX, err)) return false;
    // Clamp to the ranges the HTML inputs enforce for structural values
    np.dh = std::clamp(np.dh, 0.5f, 8.0f);
    np.dt = std::clamp(np.dt, 0.05f, 1.5f);
    np.npml = std::clamp(np.npml, 5, 60);
    if (np.fd_order != 2 && np.fd_order != 4 && np.fd_order != 6 && np.fd_order != 8) np.fd_order = 10;
    np.receiver_spacing = std::clamp(np.receiver_spacing, 1.0f, 100.0f);
    np.obs_spacing = std::clamp(np.obs_spacing, 50.0f, 500.0f);
    np.freq = std::clamp(np.freq, 0.5f, 250.0f);
    np.amp = std::clamp(np.amp, 0.2f, 20.0f);
    np.cable_len = std::clamp(np.cable_len, 50.0f, 1500.0f);
    np.cable_head_depth = std::clamp(np.cable_head_depth, 2.0f, 100.0f);
    np.cable_tail_depth = std::clamp(np.cable_tail_depth, 2.0f, 250.0f);
    np.source_depth = std::clamp(np.source_depth, 2.0f, 50.0f);
    np.src_offset = std::clamp(np.src_offset, 0.0f, 800.0f);
    np.ship_src_dist = std::clamp(np.ship_src_dist, 0.0f, 300.0f);
    np.src_rec_dist = std::clamp(np.src_rec_dist, 0.0f, 500.0f);
    np.ship_speed_knots = std::clamp(np.ship_speed_knots, 0.0f, 12.0f);
    np.shot_interval = std::clamp(np.shot_interval, 6.25f, 1000.0f);
    np.domain_W = std::clamp(np.domain_W, 800.0f, 25000.0f);   // a zero/negative domain crashed the grid setup
    np.domain_H = std::clamp(np.domain_H, 400.0f, 12000.0f);
    np.gain = std::clamp(np.gain, 1.0f, 100.0f);
    np.speed_ms = std::clamp(np.speed_ms, 1.0f, 250.0f);
    np.sismo_ms = std::clamp(np.sismo_ms, 500, 10000);

    stopMode();
    p = np;
    if (p.geology == GeologyModel::Imported) {
        if (hasModel()) {   // the domain is the loaded model's
            p.domain_W = (float)modelVel_.width();
            p.domain_H = (float)(modelVel_.depth() / (1.0 - 0.166));
        } else {
            p.geology = GeologyModel::Flat;
            alerts.push_back("Bu parametre dosyası dosyadan yüklenen bir model kullanıyor; model dosyaları parametrelerle "
                             "birlikte saklanmaz. Hız (ve yoğunluk) modelini Jeoloji sekmesinden yeniden yükleyin. "
                             "Şimdilik düz katman modeli kullanılıyor.");
        }
    }
    OCEAN_Y = p.domain_H * 0.166;
    regridPending_ = false;
    regridNow();
    sxShip = shipX;
    sxSrc = sxShip + p.src_offset;
    return true;
}

// Next to the line's SEG-Y: the velocity model as an image and as SEG-Y (the grid the solver
// uses, depth 0 = sea surface) and a PDF report of every parameter with the model image.
// Failures here do not stop the line; they are reported as notices.
void Simulation::writeLineExtras() {
    const fs::path dir = fs::u8path(line_.dir);
    auto u8 = [](const fs::path& q) { const std::u8string s = q.u8string(); return std::string(s.begin(), s.end()); };
    std::vector<std::string> problems;

    // Velocity on the grid from the sea surface down
    const int j0 = std::clamp((int)std::ceil(OCEAN_Y / SCALE - 1e-9), 0, H_sim - 2);
    const int nx = W_sim, nz = H_sim - j0;
    std::vector<float> rows((size_t)nx * nz), cols((size_t)nx * nz);
    float vmin = 1e30f, vmax = 0;
    for (int k = 0; k < nz; k++)
        for (int i = 0; i < nx; i++) {
            const float v = (float)(std::sqrt((double)vsq[(size_t)(j0 + k) * W_sim + i]) * kVelocityNorm);
            rows[(size_t)k * nx + i] = v;
            cols[(size_t)i * nz + k] = v;
            vmin = std::min(vmin, v);
            vmax = std::max(vmax, v);
        }

    static const char* geoNames[] = {"Düz katmanlar", "Fay", "Tuz domu", "Dosyadan yüklenen"};
    static const char* wavNames[] = {"Ricker (sıfır faz)", "Minimum faz", "Gauss", "Gauss 1. türevi"};
    const std::string modelName = p.geology == GeologyModel::Imported && hasModel() ? modelVelocity().source
                                                                                     : geoNames[std::clamp((int)p.geology, 0, 3)];
    const bool twoShips = line_.mode != Mode::Trad;
    const char* modeName = line_.mode == Mode::CLO ? "CLO (ardışık)" : line_.mode == Mode::SLO ? "SLO (eşzamanlı)" : "Geleneksel";
    const double mps = p.ship_speed_knots * 0.514444;
    const double lastX = line_.firstX + (line_.n - 1) * line_.interval;
    char b[256];
    auto fmt = [&](const char* f, auto... a) { std::snprintf(b, sizeof(b), f, a...); return std::string(b); };

    // ── SEG-Y ──
    {
        const std::vector<std::string> text = {
            "QUANTITY: P-WAVE VELOCITY (M/S)  MODEL: " + std::string(p.geology == GeologyModel::Imported ? "IMPORTED" : geoUpper(p.geology)),
            fmt("COLUMNS (TRACES): %d  DX: %g M  (CDP X IN CM, SCALAR -100)", nx, SCALE),
            fmt("SAMPLES PER TRACE: %d  DZ: %g M  (SAMPLE INTERVAL FIELD IN MM)", nz, SCALE),
            fmt("DEPTH OF FIRST SAMPLE: %.2f M BELOW SEA SURFACE", j0 * SCALE - OCEAN_Y),
            fmt("VELOCITY RANGE: %.0f - %.0f M/S", vmin, vmax),
            "DATA FORMAT: IEEE 32-BIT FLOAT (FORMAT CODE 5)"};
        const std::vector<uint8_t> seg = buildModelSegy(cols.data(), nx, nz, SCALE, SCALE, text);
        std::ofstream o(dir / "model_velocity.segy", std::ios::binary);
        if (o) o.write((const char*)seg.data(), (std::streamsize)seg.size());
        if (!o) problems.push_back("model_velocity.segy");
    }

    // ── Image ──
    report::ModelPlot mp;
    mp.nx = nx; mp.nz = nz; mp.dx = SCALE; mp.dz = SCALE; mp.vel = rows.data();
    mp.title = "Hız modeli: " + modelName;
    mp.subtitle = fmt("%.0f × %.0f m, ızgara %g m  ·  hat: %d atış, %.0f – %.0f m, %s", (nx - 1) * SCALE, (nz - 1) * SCALE, SCALE,
                      line_.n, line_.firstX, lastX, modeName);
    mp.sourceDepth = p.source_depth;
    for (int k = 0; k < line_.n; k++) {
        const double x = line_.firstX + k * line_.interval;
        mp.shotsMain.push_back(x);
        // Where the source vessel fires: src_offset ahead, plus what the ship sails until then
        if (line_.mode == Mode::SLO) mp.shotsSrc.push_back(x + p.src_offset + 25 * dtPhy() * mps);
        if (line_.mode == Mode::CLO) mp.shotsSrc.push_back(x + p.src_offset + sismoSteps() * dtPhy() * mps);
    }
    mp.cable = true;
    mp.cableX1 = line_.firstX - p.src_rec_dist;
    mp.cableX0 = mp.cableX1 - p.cable_len;
    mp.cableZ1 = p.cable_head_depth;
    mp.cableZ0 = p.cable_tail_depth;
    if (p.obs_enabled)
        for (int i = 0; i < N_OBS; i++) {
            const double x = (i + 1) * p.obs_spacing;
            mp.obs.push_back({x, seafloorBelowSurface(x)});
        }
    report::Image img;
    std::string err;
    const bool haveImg = report::renderModel(mp, img, err);
    if (!haveImg || !report::writePng(u8(dir / "model_velocity.png"), img, err))
        problems.push_back("model_velocity.png");

    // ── PDF ──
    report::Document doc;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char when[64];
    std::strftime(when, sizeof(when), "%d.%m.%Y %H:%M", &tm);
    doc.title = "Hat simülasyonu raporu";
    doc.subtitle = std::string("Oluşturma: ") + when + "  ·  Klasör: " + line_.dir;
    doc.image = haveImg ? &img : nullptr;
    doc.imageCaption = "Hız modeli (çözücünün kullandığı ızgara, derinlik deniz yüzeyinden) ve hat geometrisi: kırmızı noktalar ana kaynak "
                       "atışları" + std::string(twoShips ? ", mavi noktalar kaynak gemisi atışları" : "") +
                       ", sarı çizgi ilk atıştaki streamer" + (p.obs_enabled ? ", üçgenler OBS." : ".");
    using report::Row;
    const NumericalStatus ns = numericalStatus();
    report::Section line{"Hat", {}};
    line.rows = {{"Atış kipi", modeName},
                 {"Atış sayısı", std::to_string(line_.n)},
                 {"İlk atış konumu (ana kaynak)", fmt("%.1f m", line_.firstX)},
                 {"Son atış konumu", fmt("%.1f m", lastX)},
                 {"Atış aralığı", fmt("%.2f m", line_.interval)},
                 {"Hat uzunluğu", fmt("%.1f m", lastX - line_.firstX)},
                 {"Gemi hızı", fmt("%.1f knot (%.2f m/s)", p.ship_speed_knots, mps)},
                 {"Geminin bir kayıtta aldığı yol", fmt("%.1f m", mps * p.sismo_ms / 1000.0 * (line_.mode == Mode::CLO ? 2 : 1))}};
    if (twoShips) line.rows.push_back({"Kaynak gemisi (ana kaynağın önünde)", fmt("%g m", p.src_offset)});
    report::Section rec{"Kayıt", {}};
    rec.rows = {{"Kayıt uzunluğu", fmt("%d ms", p.sismo_ms)},
                {"SEG-Y örnek aralığı", "1 ms"},
                {"Streamer kanal sayısı", std::to_string(N_REC)},
                {"OBS kanal sayısı", p.obs_enabled ? std::to_string(N_OBS) : std::string("kapalı")}};
    report::Section src{"Kaynak ve dalgacık", {}};
    src.rows = {{"Dalgacık", wavNames[std::clamp((int)p.wavelet, 0, 3)]},
                {"Baskın frekans", fmt("%.1f Hz", p.freq)},
                {"Kaynak genliği", fmt("%.1f", p.amp)},
                {"Kaynak derinliği", fmt("%g m", p.source_depth)}};
    report::Section geo{"Saha geometrisi", {}};
    geo.rows = {{"Kablo boyu", fmt("%g m", p.cable_len)},
                {"Alıcı grup aralığı", fmt("%g m", p.receiver_spacing)},
                {"Kablo baş derinliği", fmt("%g m", p.cable_head_depth)},
                {"Kablo son derinliği", fmt("%g m", p.cable_tail_depth)},
                {"Gemi – kaynak uzaklığı", fmt("%g m", p.ship_src_dist)},
                {"Kaynak – ilk alıcı uzaklığı (yakın ofset)", fmt("%g m", p.src_rec_dist)},
                {"Uzak ofset", fmt("%g m", p.src_rec_dist + receiverSpan())},
                {"OBS aralığı", p.obs_enabled ? fmt("%g m", p.obs_spacing) : std::string("OBS kapalı")}};
    report::Section med{"Ortam ve yeraltı modeli", {}};
    med.rows = {{"Hız modeli", modelName},
                {"Hız aralığı", fmt("%.0f – %.0f m/s", vmin, vmax)},
                {"Yoğunluk", p.geology == GeologyModel::Imported && !modelDensity().empty() ? modelDensity().source
                                                                                          : std::string("Gardner, ρ = 0.31·V^0.25; su 1.03 g/cm³")},
                {"Alan genişliği", fmt("%.0f m", (W_sim - 1) * SCALE)},
                {"Alan derinliği (deniz yüzeyinden)", fmt("%.0f m", (nz - 1) * SCALE)},
                {"Deniz yüzeyi yansıması", p.free_surface ? "açık (serbest yüzey)" : "kapalı (soğurucu)"}};
    report::Section numSec{"Sayısal çözüm", {}};
    numSec.rows = {{"Türev derecesi", fmt("%d. derece", p.fd_order)},
                {"Izgara aralığı", fmt("%g m", SCALE)},
                {"Izgara", fmt("%d × %d hücre", W_sim, H_sim)},
                {"Modelin ızgaraya aktarımı", p.cell_average ? "hücre ortalaması" : "düğüm noktasındaki değer"},
                {"Model sınırları", !modelVelS_.empty() ? fmt("basamaksız (%d sınır)", smoothStats_.boundaries) : std::string("modeldeki gibi")},
                {"Zaman adımı katsayısı", fmt("%.2f", p.dt)},
                {"Zaman adımı", fmt("%.4f ms", dtPhy() * 1000.0)},
                {"Soğurucu sınır", fmt("%d hücre", p.npml)},
                {"Genişleyen hesap kutusu", p.expanding_box ? "açık" : "kapalı"},
                {"CFL oranı", fmt("%%%d (sınır katsayı %.2f)", (int)std::lround(ns.cfl_ratio * 100), ns.dt_max)},
                {"Dalga boyu başına nokta", fmt("%.1f (en az %d)", ns.ppw, ns.min_ppw)},
                {"Hesaplama motoru", usingGpu() ? "GPU" : "CPU"}};
    report::Section files{"Klasördeki dosyalar", {}};
    files.rows = {{"line_streamer.segy", "ana gemi streamer kayıtları, her atış bir ensemble"}};
    if (line_.mode == Mode::CLO) files.rows.push_back({"line_streamer_src.segy", "kaynak gemisi atışlarının streamer kayıtları"});
    if (p.obs_enabled) files.rows.push_back({"line_obs.segy", "OBS kayıtları"});
    files.rows.push_back({"model_velocity.segy", "hız modeli, her iz bir sütun, örnek aralığı dz (mm)"});
    files.rows.push_back({"model_velocity.png", "hız modeli ve hat geometrisi görseli"});
    files.rows.push_back({"line_report.pdf", "bu rapor"});
    doc.sections = {line, rec, src, geo, med, numSec, files};
    if (!report::writePdf(u8(dir / "line_report.pdf"), doc, err)) problems.push_back("line_report.pdf");

    if (!problems.empty()) {
        std::string s = "Hat raporu dosyalarından bazıları yazılamadı:";
        for (const auto& q : problems) s += " " + q;
        notices.push_back(s + (err.empty() ? "" : " (" + err + ")"));
    }
}
