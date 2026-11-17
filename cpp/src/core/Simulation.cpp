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
#include "ParamsIO.hpp"
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
    return g == GeologyModel::Flat ? "FLAT" : g == GeologyModel::Fault ? "FAULT" : "SALT";
}
} // namespace

Simulation::Simulation() = default;

Simulation::~Simulation() {
    waitEikonal();
    cpu_.wait();
    if (glAvailable_) {
        gpu_.destroy();
        if (waveTex) glDeleteTextures(1, &waveTex);
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
    // (~56 bytes per cell: 8 float fields, pixels, V^2 copy, eikonal tables).
    SCALE = p.dh;
    bool coarsened = false;
    for (;;) {
        W_sim = (int)std::floor(p.domain_W / SCALE) + 1;
        H_sim = (int)std::floor(p.domain_H / SCALE) + 1;
        if ((double)W_sim * H_sim * 56.0 <= 0.5 * physicalRamBytes() || SCALE >= 8.0) break;
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
    N_REC = (int)std::floor(p.cable_len / p.receiver_spacing) + 1;
    // The HTML only recomputed N_OBS when the OBS spacing changed; keeping it in
    // sync with the domain width avoids OBS stations outside the model.
    N_OBS = std::max(0, (int)std::floor((W_sim * SCALE) / p.obs_spacing));

    fillVsq(vsq, W_sim, H_sim, SCALE, p.geology, p.domain_W, p.domain_H);
    layout = buildLayout(p.geology, p.domain_W, p.domain_H);
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
    const size_t sismoSize = (size_t)(N_REC * 2 + N_OBS) * GH;
    int hc = (int)std::thread::hardware_concurrency();
    if (hc <= 0) hc = 4;
    // Measured on hybrid CPUs: beyond ~half the logical cores the per-step barriers
    // cost more than the extra (E-)cores add, for small and large grids alike.
    const int nThreads = std::clamp(hc / 2 + 1, 1, std::max(1, H_sim / 4));
    cpu_.init(W_sim, H_sim, nThreads, sismoSize);
    cpu_.setVsq(vsq);
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
    const long long bufBytes = (long long)W_sim * H_sim * 4;
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
    if (gpu_.matches(W_sim, H_sim, N_REC, N_OBS)) {
        gpu_.setVsq(vsq);
        gpu_.setCpml(bx_, ax_, bz_, az_);
        gpu_.setObs(obsX_, obsY_);
        if (gpu_.ensurePrograms(p.fd_order, err)) {
            badge = {BadgeKind::Gpu, gpuRenderer_, "Hazır: Önceki OpenGL yapılandırması yeniden kullanılıyor."};
            gpuReady = true;
            return;
        }
    } else if (gpu_.init(W_sim, H_sim, N_REC, N_OBS, err)) {
        gpu_.setVsq(vsq);
        gpu_.setCpml(bx_, ax_, bz_, az_);
        gpu_.setObs(obsX_, obsY_);
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
    cpmlX(W_sim, p.npml, p.dt, bx_, ax_);
    cpmlZ(H_sim, p.npml, p.dt, OCEAN_Y, SCALE, p.free_surface, bz_, az_);
}

void Simulation::computeObsPositions() {
    obsX_.resize(N_OBS);
    obsY_.resize(N_OBS);
    for (int i = 0; i < N_OBS; i++) {
        double rx = (i + 1) * (double)p.obs_spacing;
        obsX_[i] = (float)rx;
        obsY_[i] = (float)(obsSeafloorDepth(p.geology, rx, p.domain_W, p.domain_H) + 4.0);   // slight offset below seabed
    }
}

void Simulation::createWaveTexture() {
    if (waveTex) glDeleteTextures(1, &waveTex);
    glGenTextures(1, &waveTex);
    glBindTexture(GL_TEXTURE_2D, waveTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W_sim, H_sim);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    clearWaveTexture();
}

void Simulation::clearWaveTexture() {
    if (!glAvailable_ || !waveTex) return;
    zeroTex_.assign((size_t)W_sim * H_sim, 0u);
    glBindTexture(GL_TEXTURE_2D, waveTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W_sim, H_sim, GL_RGBA, GL_UNSIGNED_BYTE, zeroTex_.data());
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
    cpmlZ(H_sim, p.npml, p.dt, OCEAN_Y, SCALE, p.free_surface, bz_, az_);
    cpu_.setCpmlZ(bz_, az_);
    // The HTML rewrote the GPU params here without the free-surface/geology arguments,
    // silently disabling the free surface on GPU. Both engines read the flag per batch now.
    if (gpuReady) gpu_.setCpmlZ(bz_, az_);
}

void Simulation::toggleObs() { p.obs_enabled = !p.obs_enabled; }

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
    if (gpuReady) gpu_.setCpml(bx_, ax_, bz_, az_);
}

void Simulation::onNpml() {
    waitInFlight(false);
    updateCpml();
    cpu_.setCpml(bx_, ax_, bz_, az_);
    if (gpuReady) gpu_.setCpml(bx_, ax_, bz_, az_);
    stopMode();
}

void Simulation::onCableLen() { onReceiverSpacing(false); }

void Simulation::onReceiverSpacing(bool forceReset) {
    const int n = (int)std::floor(p.cable_len / p.receiver_spacing) + 1;
    if (n == N_REC && !forceReset) return;
    N_REC = n;
    if (mode != Mode::None) stopMode();
    rebuildEngines();
}

void Simulation::onShipX(double x) {
    waitInFlight(true);   // otherwise the running batch overwrites the new position when it completes
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
    fillVsq(vsq, W_sim, H_sim, SCALE, p.geology, p.domain_W, p.domain_H);
    layout = buildLayout(p.geology, p.domain_W, p.domain_H);
    computeObsPositions();
    cpu_.setVsq(vsq);
    if (gpuReady) { gpu_.setVsq(vsq); gpu_.setObs(obsX_, obsY_); }
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
    loopIteration();
}

void Simulation::loopIteration() {
    const double dt_phy = dtPhy();
    const double spf = (p.speed_ms / 1000.0) / dt_phy;
    accumulated_ += spf;
    double stepsD = std::floor(accumulated_);
    if (stepsD < 1) return;
    accumulated_ -= stepsD;

    const double mps = p.ship_speed_knots * 0.514444;
    const double cur_t_max = mps > 0 ? std::floor((p.shot_interval * 6000.0) / (mps * SCALE * p.dt))
                                     : std::numeric_limits<double>::infinity();
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
    const double cur_t_max = mps > 0 ? std::floor((p.shot_interval * 6000.0) / (mps * SCALE * p.dt))
                                     : std::numeric_limits<double>::infinity();
    const double T_sismo = std::floor((p.sismo_ms * 6.0) / (SCALE * p.dt));

    boxFor(simTime_ + steps * dt_phy, batchBox_);
    const bool rowsValid = p.expanding_box && batchBox_.hasRows;

    // ── Per-step source / receiver data (_prepareStepData) ──
    stepData_.assign(steps, StepData{});
    const double ship_step = mps * dt_phy;
    double sx = sxShip, sxs = sxSrc;
    const double f = p.freq;
    if (!pulse_.matches(p.wavelet, f)) pulse_.build(p.wavelet, f);
    const int sj_src = (int)std::floor((OCEAN_Y + p.source_depth) / SCALE);
    for (int s = 0; s < steps; s++) {
        StepData& d = stepData_[s];
        const double cyc = cycle_t + s + 1;
        sx += ship_step;
        sxs += ship_step;
        for (const Shot& sh : shots) {
            const double elapsed = (cyc - sh.t0) * dt_phy;
            const double val = pulse_(elapsed) * p.amp * p.dt * 0.4;
            const double src_pos = (sh.main ? sx : sxs) - p.ship_src_dist;
            const float si = (float)std::floor(src_pos / SCALE);
            if (sh.main) { d.si0 = si; d.sj0 = (float)sj_src; d.val0 = (float)val; d.act0 = val != 0 ? 1.f : 0.f; }
            else         { d.si1 = si; d.sj1 = (float)sj_src; d.val1 = (float)val; d.act1 = val != 0 ? 1.f : 0.f; }
        }
        const bool shot2 = (mode == Mode::CLO && cyc >= cur_t_max);
        const double t_local = shot2 ? cyc - cur_t_max : cyc;
        int gy = (int)std::floor((t_local / T_sismo) * GH);
        d.rec_valid = (mode != Mode::None && t_local <= T_sismo && gy >= 0 && gy < GH) ? 1.f : 0.f;
        d.gy_pix = (float)gy;
        {   // number of recorded steps (integer t_local in [1, T_sismo]) that map to row gy
            const double lo = std::max(1.0, std::ceil(gy * T_sismo / GH));
            const double hi = std::min(T_sismo, std::ceil((gy + 1) * T_sismo / GH) - 1.0);
            d.rec_w = (float)(1.0 / std::max(1.0, hi - lo + 1.0));
        }
        d.sismo_off = shot2 ? (float)(N_REC * GH) : 0.f;
        d.sxShip = (float)sx;
        d.ry_head = (float)(OCEAN_Y + p.cable_head_depth);
        d.ry_tail = (float)(OCEAN_Y + p.cable_tail_depth);
        d.cable_head_x = (float)(sx - p.ship_src_dist - p.src_rec_dist);
    }

    BatchJob& J = inflight_.job;
    J = BatchJob{};
    J.steps = steps;
    J.dt = p.dt;
    J.fd_order = p.fd_order;
    J.freeSurface = p.free_surface;
    J.sj_surf = sjSurf();
    J.bxmin = batchBox_.min_i; J.bxmax = batchBox_.max_i;
    J.bymin = batchBox_.min_j; J.bymax = batchBox_.max_j;
    J.stepData = stepData_.data();
    J.probeRow = std::clamp(sj_src, 0, H_sim - 1);
    J.N_REC = N_REC; J.N_OBS = N_OBS;
    J.cable_len = p.cable_len;
    J.SCALE = (float)SCALE;
    J.obsX = obsX_.data(); J.obsY = obsY_.data();
    J.renderPixels = !p.sismo_only;
    J.gainNorm = p.gain / 15.0f;

    // Region coloured on screen (drawFDTDToScreen)
    const bool active = p.expanding_box && !shots.empty();
    int drawMin = sjSurf(), drawMax = H_sim - 1;
    if (active) {
        drawMin = p.free_surface ? std::max(drawMin, batchBox_.min_j) : std::max(0, batchBox_.min_j);
        drawMax = std::min(H_sim - 1, batchBox_.max_j);
    }
    J.drawMinJ = drawMin; J.drawMaxJ = drawMax;
    if (rowsValid) { J.rowMin = batchBox_.xMinZ.data(); J.rowMax = batchBox_.xMaxZ.data(); }
    if (active) {
        J.dirtyX0 = std::max(0, batchBox_.min_i); J.dirtyX1 = std::min(W_sim - 1, batchBox_.max_i);
        J.clearMinJ = drawMin; J.clearMaxJ = drawMax;
    } else {
        J.dirtyX0 = 0; J.dirtyX1 = W_sim - 1;
        J.clearMinJ = 0; J.clearMaxJ = H_sim - 1;
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
                glPixelStorei(GL_UNPACK_ROW_LENGTH, W_sim);
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0 + 1, y1 - y0 + 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                cpu_.pixels().data() + (size_t)y0 * W_sim + x0);
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
    shots.push_back({main, cycle_t + delay});
    eikDirty_ = true;
}

void Simulation::startEikonal() {
    eikDirty_ = false;
    waitEikonal();
    if (shots.empty()) { eik_.clear(); return; }
    std::vector<std::pair<int, int>> cells;
    const int sj = (int)std::floor((OCEAN_Y + p.source_depth) / SCALE);
    for (const Shot& sh : shots) {
        double src_pos = (sh.main ? sxShip : sxSrc) - p.ship_src_dist;
        cells.push_back({(int)std::floor(src_pos / SCALE), sj});
    }
    const double scale = SCALE, dt = p.dt;
    const int W = W_sim, H = H_sim;
    if ((long long)W * H < 400000) {   // small grids: solve inline (sub-millisecond)
        eik_.compute(vsq, W, H, scale, dt, cells, &eikProgress_);
        return;
    }
    eikProgress_ = 0;
    eikBusy_ = true;
    eikFuture_ = std::async(std::launch::async, [this, cells, scale, dt, W, H] {
        eik_.compute(vsq, W, H, scale, dt, cells, &eikProgress_);
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
        out.min_i = 0; out.max_i = W_sim - 1; out.min_j = 0; out.max_j = H_sim - 1;
        out.hasRows = false;
        return false;
    }
    eik_.getBox(t_sec, p.fd_order, p.free_surface, sjSurf(), out);
    return true;
}

bool Simulation::drawBox(EikBox& out) const { return boxFor(simTime_, out); }

NumericalStatus Simulation::numericalStatus() const {
    double c_sum = 1.0;
    switch (p.fd_order) { case 4: c_sum = 1.166; break; case 6: c_sum = 1.241; break;
                          case 8: c_sum = 1.28; break; case 10: c_sum = 1.31; break; }
    int min_ppw = 12;
    switch (p.fd_order) { case 4: min_ppw = 8; break; case 6: min_ppw = 6; break;
                          case 8: min_ppw = 5; break; case 10: min_ppw = 4; break; }
    NumericalStatus s;
    s.dt_max = 1.04 / c_sum;
    s.cfl_ratio = p.dt / s.dt_max;
    s.ppw = (1500.0 / p.freq) / SCALE;   // min velocity = water 1500 m/s
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

    auto snap1 = resampleGather(sismo.data(), 0, N_REC, GH, targetSamples);
    auto snap2 = resampleGather(sismo.data(), (size_t)N_REC * GH, N_REC, GH, targetSamples);
    auto snapO = resampleGather(sismo.data(), (size_t)N_REC * 2 * GH, N_OBS, GH, targetSamples);
    auto anyNonZero = [](const std::vector<float>& v) {
        return std::any_of(v.begin(), v.end(), [](float x) { return x != 0.0f; });
    };

    SegySpec base;
    base.nSamples = targetSamples; base.dtMicros = dtMicros; base.shotNum = sn; base.modeLabel = modeLabel;
    base.sourceDepth = p.source_depth; base.fd_order = p.fd_order; base.freq = p.freq; base.SCALE = SCALE;
    base.domainW = p.domain_W; base.domainH = p.domain_H; base.oceanY = OCEAN_Y; base.geology = geoUpper(p.geology);

    const double head_x = sxShip - p.ship_src_dist - p.src_rec_dist;
    const double cable = p.cable_len, tail = p.cable_tail_depth, headD = p.cable_head_depth;
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
        const GeologyModel g = p.geology;
        const double sp = p.obs_spacing, W = p.domain_W, H = p.domain_H;
        s.receiver = [=](int i) {
            double rx = (i + 1) * sp;
            return SegyReceiver{rx, 0.0, obsSeafloorDepth(g, rx, W, H)};
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
    np.receiver_spacing = std::clamp(np.receiver_spacing, 3.125f, 100.0f);
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
    np.shot_interval = std::clamp(np.shot_interval, 6.25f, 100.0f);
    np.domain_W = std::clamp(np.domain_W, 800.0f, 25000.0f);   // a zero/negative domain crashed the grid setup
    np.domain_H = std::clamp(np.domain_H, 400.0f, 12000.0f);
    np.gain = std::clamp(np.gain, 1.0f, 100.0f);
    np.speed_ms = std::clamp(np.speed_ms, 1.0f, 250.0f);
    np.sismo_ms = std::clamp(np.sismo_ms, 500, 10000);

    stopMode();
    p = np;
    OCEAN_Y = p.domain_H * 0.166;
    regridPending_ = false;
    regridNow();
    sxShip = shipX;
    sxSrc = sxShip + p.src_offset;
    return true;
}
