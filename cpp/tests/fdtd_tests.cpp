// Headless checks + benchmark for the FDTD core.
//   fdtd_tests            → run all tests (exit code 0 = pass)
//   fdtd_tests --bench    → also run the steps/s benchmark
//   fdtd_tests --snap DIR / --simsnap DIR MODE [minphase] / --pml / --trace FILE [minphase]  → wavefield snapshots (PGM) and CPML absorption check
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "core/Cpml.hpp"
#include "core/CpuSolver.hpp"
#include "core/Eikonal.hpp"
#include "core/Geology.hpp"
#include "core/GpuSolver.hpp"
#include "core/ParamsIO.hpp"
#include "core/Segy.hpp"
#include "core/Simulation.hpp"
#include "core/Wavelet.hpp"
#include "core/ImageModel.hpp"
#include "core/ModelCanvas.hpp"
#include "core/Report.hpp"

static int failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); failures++; } \
    } while (0)

struct Setup {
    int W, H, nRec = 57, nObs = 24;
    double SCALE, dt = 0.5, oceanY = 80, domW, domH;
    std::vector<float> vsq, bx, ax, bz, az, obsX, obsY;
};

// Solver material for a test setup: density from the velocity (Gardner), as the app does
static Material materialOf(const Setup& s) {
    std::vector<float> rho;
    densityFromVsq(s.vsq, rho);
    return makeMaterial(s.vsq, rho, s.W, s.H);
}

static Setup makeSetup(double domW, double domH, double scale, int npml = 40) {
    Setup s;
    s.domW = domW; s.domH = domH; s.SCALE = scale;
    s.W = (int)std::floor(domW / scale) + 1;
    s.H = (int)std::floor(domH / scale) + 1;
    s.oceanY = domH * 0.166;
    fillVsq(s.vsq, s.W, s.H, scale, GeologyModel::Salt, domW, domH);
    cpmlX(s.W, npml, s.dt, s.bx, s.ax);
    cpmlZ(s.H, npml, s.dt, s.oceanY, scale, true, s.bz, s.az);
    s.nObs = (int)std::floor(s.W * scale / 50.0);
    for (int i = 0; i < s.nObs; i++) {
        double rx = (i + 1) * 50.0;
        s.obsX.push_back((float)rx);
        s.obsY.push_back((float)(obsSeafloorDepth(GeologyModel::Salt, rx, domW, domH) + 4.0));
    }
    return s;
}

// Ricker shot at the domain centre, streamer recording, full-domain box
static std::vector<StepData> makeSteps(const Setup& s, int start, int steps, double T_sismo) {
    std::vector<StepData> v(steps);
    const double dt_phy = s.SCALE / 6000.0 * s.dt, f = 50.0;
    for (int k = 0; k < steps; k++) {
        StepData& d = v[k];
        std::memset(&d, 0, sizeof(d));
        const double cyc = start + k + 1;
        const double el = cyc * dt_phy;
        if (el < 3.0 / f) {
            double t = el - 1.5 / f, fp = M_PI * M_PI * f * f * t * t;
            d.val0 = (float)((1 - 2 * fp) * std::exp(-fp) * 2.0 * s.dt * 0.4);
            d.act0 = d.val0 != 0 ? 1.f : 0.f;
        }
        d.si0 = (float)(s.W / 2);
        d.sj0 = (float)std::floor((s.oceanY + 8) / s.SCALE);
        int gy = (int)std::floor(cyc / T_sismo * 550);
        d.rec_valid = (cyc <= T_sismo && gy < 550) ? 1.f : 0.f;
        d.gy_pix = (float)gy;
        d.ry_head = d.ry_tail = (float)(s.oceanY + 10);
        d.cable_head_x = (float)(s.domW * 0.5 + 200);
    }
    return v;
}

static BatchJob makeJob(const Setup& s, const std::vector<StepData>& st, int order) {
    BatchJob J;
    J.steps = (int)st.size(); J.dt = (float)s.dt; J.fd_order = order; J.freeSurface = true;
    J.sj_surf = (int)std::floor(s.oceanY / s.SCALE);
    J.bxmin = 0; J.bxmax = s.W - 1; J.bymin = 0; J.bymax = s.H - 1;
    J.stepData = st.data();
    J.N_REC = s.nRec; J.N_OBS = s.nObs; J.cable_len = 350; J.SCALE = (float)s.SCALE;
    J.obsX = s.obsX.data(); J.obsY = s.obsY.data();
    J.renderPixels = false;
    return J;
}

// Re-created GPU solvers must start from zero, and clearFields / clearPsi / clearSismo must
// zero their buffers. On Intel Graphics glClearBufferData was ignored on the GPU-only
// buffers: every second new solver had Vz holding the previous velocity model, and the
// CPU/GPU parity cases after the first two failed.
static void testGpuReinit(bool haveGpu) {
    std::printf("[gpu reinit] new solvers start from zero, clears zero the buffers\n");
    if (!haveGpu) { std::printf("  SKIP: no OpenGL 4.3 context\n"); return; }
    Setup s = makeSetup(1200, 480, 4);
    const size_t N = (size_t)s.W * s.H, nSismo = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
    const int fields[] = {0, 1, 2, 4, 5, 6, 7};   // P, Vx, Vz, psi x4 (Binding in GpuSolver.cpp)
    double worstInit = 0, worstClear = 0, ran = 0;
    for (int inst = 0; inst < 4; inst++) {
        GpuSolver gpu;
        std::string err;
        const bool ok = gpu.init(s.W, s.H, s.nRec, s.nObs, err) && gpu.ensurePrograms(6, err);
        CHECK(ok, "GPU init: %s", err.c_str());
        if (!ok) return;
        gpu.setMaterial(materialOf(s));
        gpu.setCpml(s.bx, s.ax, s.bz, s.az);
        gpu.setObs(s.obsX, s.obsY);
        std::vector<float> v;
        auto maxOf = [&](int b, size_t n) { gpu.readBuf(b, n, v); double m = 0; for (float x : v) m = std::max(m, (double)std::fabs(x)); return m; };
        for (int b : fields) worstInit = std::max(worstInit, maxOf(b, N));
        auto st = makeSteps(s, 0, 100, 3000);
        gpu.submit(makeJob(s, st, 6), 0);
        gpu.wait();
        ran = std::max(ran, maxOf(0, N));
        gpu.clearFields(); gpu.clearPsi(); gpu.clearSismo();
        for (int b : fields) worstClear = std::max(worstClear, maxOf(b, N));
        worstClear = std::max(worstClear, maxOf(9, nSismo));
        gpu.destroy();
    }
    std::printf("  4 solvers: max |field| after init %.3g, after a batch %.3g, after clear %.3g\n", worstInit, ran, worstClear);
    CHECK(ran > 1e-3, "the batch did not move the wavefield");
    CHECK(worstInit == 0, "a new GPU solver started with non-zero fields (%.3g)", worstInit);
    CHECK(worstClear == 0, "GPU clears left non-zero values (%.3g)", worstClear);
}

static void testParity(bool haveGpu) {
    std::printf("[parity] CPU vs GPU, 1200x480 m, 500 steps\n");
    if (!haveGpu) { std::printf("  SKIP: no OpenGL 4.3 context\n"); return; }
    struct Case { int order; bool fs; double scale; };
    const Case cases[] = {{2, true, 4}, {4, true, 4}, {6, true, 4}, {8, true, 4}, {10, true, 4},
                          {10, false, 4}, {4, false, 4}, {10, false, 1}};   // free surface off: top CPML
    for (const Case& cs : cases) {
        const int order = cs.order;
        Setup s = makeSetup(1200, 480, cs.scale);
        if (!cs.fs) cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, false, s.bz, s.az);
        const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
        CpuSolver cpu;
        cpu.init(s.W, s.H, 4, sismoSize);
        cpu.setMaterial(materialOf(s));
        cpu.setCpml(s.bx, s.ax, s.bz, s.az);
        GpuSolver gpu;
        std::string err;
        bool ok = gpu.init(s.W, s.H, s.nRec, s.nObs, err) && gpu.ensurePrograms(order, err);
        CHECK(ok, "GPU init order %d: %s", order, err.c_str());
        if (!ok) continue;
        gpu.setMaterial(materialOf(s));
        gpu.setCpml(s.bx, s.ax, s.bz, s.az);
        gpu.setObs(s.obsX, s.obsY);
        const int perBatch = cs.scale < 2 ? 400 : 100;   // same physical time at 1 m
        for (int b = 0; b < 5; b++) {
            auto st = makeSteps(s, b * perBatch, perBatch, 3000 * 4 / cs.scale);
            BatchJob J = makeJob(s, st, order);
            J.freeSurface = cs.fs;
            cpu.submit(J);
            gpu.submit(J, 0);
            cpu.wait();
            gpu.wait();
        }
        std::vector<float> gp;
        gpu.readP(gp);
        const auto& cp = cpu.P();
        double maxAbs = 0, maxDiff = 0;
        for (size_t i = 0; i < cp.size(); i++) {
            maxAbs = std::max(maxAbs, (double)std::fabs(cp[i]));
            maxDiff = std::max(maxDiff, (double)std::fabs(cp[i] - gp[i]));
        }
        double sMax = 0, sDiff = 0;
        for (size_t i = 0; i < sismoSize; i++) {
            sMax = std::max(sMax, (double)std::fabs(cpu.sismo()[i]));
            sDiff = std::max(sDiff, (double)std::fabs(cpu.sismo()[i] - gpu.sismo()[i]));
        }
        const double rel = maxAbs > 0 ? maxDiff / maxAbs : 1, srel = sMax > 0 ? sDiff / sMax : 1;
        std::printf("  O%-2d fs=%d dh=%g  max|P|=%.4g  P rel diff=%.2e  sismo max=%.4g rel diff=%.2e\n", order, cs.fs, cs.scale,
                    maxAbs, rel, sMax, srel);
        CHECK(maxAbs > 1e-6 && std::isfinite(maxAbs), "wavefield empty or non-finite (O%d)", order);
        CHECK(rel < 1e-4, "P mismatch O%d fs=%d (%.2e)", order, cs.fs, rel);
        CHECK(srel < 1e-4, "sismo mismatch O%d fs=%d (%.2e)", order, cs.fs, srel);
        gpu.destroy();
    }
}

static void testPixels() {
    std::printf("[pixels] CPU colormap pass produces visible wavefield\n");
    Setup s = makeSetup(1200, 480, 4.0);
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
    CpuSolver cpu;
    cpu.init(s.W, s.H, 4, sismoSize);
    cpu.setMaterial(materialOf(s));
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    auto st = makeSteps(s, 0, 300, 3000);
    BatchJob J = makeJob(s, st, 10);
    J.renderPixels = true;
    J.gainNorm = 1.0f;
    J.drawMinJ = J.sj_surf; J.drawMaxJ = s.H - 1;
    J.clearMinJ = 0; J.clearMaxJ = s.H - 1;
    J.dirtyX0 = 0; J.dirtyX1 = s.W - 1;
    cpu.submit(J);
    cpu.wait();
    size_t visible = 0;
    uint32_t maxA = 0;
    for (uint32_t px : cpu.pixels()) { uint32_t a = px >> 24; if (a > 20) visible++; maxA = std::max(maxA, a); }
    float pmax = 0;
    for (float v : cpu.P()) pmax = std::max(pmax, std::fabs(v));
    std::printf("  max|P|=%.4f  visible pixels=%zu  max alpha=%u\n", pmax, visible, maxA);
    CHECK(visible > 100, "wavefield colormap empty");
}

static void testInstability(bool haveGpu) {
    std::printf("[instability] probe row flags a CFL blow-up without pixels or recording\n");
    Setup s = makeSetup(1200, 480, 4.0);
    s.dt = 1.5;   // far above dt_max = 0.79 for O10
    cpmlX(s.W, 40, s.dt, s.bx, s.ax);
    cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, true, s.bz, s.az);
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
    auto st = makeSteps(s, 0, 600, 1e9);
    for (auto& d : st) d.rec_valid = 0;   // recording window over: sismo check alone would miss it
    BatchJob J = makeJob(s, st, 10);
    J.probeRow = (int)st[0].sj0;
    CpuSolver cpu;
    cpu.init(s.W, s.H, 4, sismoSize);
    cpu.setMaterial(materialOf(s));
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    cpu.submit(J); cpu.wait();
    CHECK(cpu.instabilityDetected(), "CPU blow-up not detected");
    J.dt = 0.5f;   // stable run must stay clean
    for (auto& d : st) d.val0 *= 0.5f / 1.5f;
    cpu.clearFields(); cpu.clearPsi();
    cpmlX(s.W, 40, 0.5, s.bx, s.ax);
    cpmlZ(s.H, 40, 0.5, s.oceanY, s.SCALE, true, s.bz, s.az);
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    cpu.submit(J); cpu.wait();
    CHECK(!cpu.instabilityDetected(), "false alarm on a stable CPU run");
    if (!haveGpu) return;
    GpuSolver gpu;
    std::string err;
    if (!gpu.init(s.W, s.H, s.nRec, s.nObs, err) || !gpu.ensurePrograms(10, err)) { CHECK(false, "GPU init: %s", err.c_str()); return; }
    gpu.setMaterial(materialOf(s));
    gpu.setCpml(s.bx, s.ax, s.bz, s.az);
    gpu.setObs(s.obsX, s.obsY);
    gpu.submit(J, 0); gpu.wait();
    CHECK(!gpu.instabilityDetected(), "false alarm on a stable GPU run");
    J.dt = 1.5f;
    gpu.clearFields(); gpu.clearPsi();
    gpu.submit(J, 0); gpu.wait();
    CHECK(gpu.instabilityDetected(), "GPU blow-up not detected");
}

static void testEikonal() {
    std::printf("[eikonal] expanding box grows monotonically and contains the source\n");
    Setup s = makeSetup(1200, 480, 4.0);
    Eikonal e;
    const int si = 100, sj = (int)std::floor((s.oceanY + 8) / s.SCALE);
    e.compute(s.vsq, s.W, s.H, s.SCALE, s.dt, {{si, sj}});
    CHECK(e.valid(), "eikonal not valid");
    EikBox prev, box;
    const double dt_phy = s.SCALE / 6000.0 * s.dt;
    bool mono = true, contains = true;
    e.getBox(0, 10, true, (int)(s.oceanY / s.SCALE), prev);
    for (int step = 50; step <= 5000; step += 50) {
        e.getBox(step * dt_phy, 10, true, (int)(s.oceanY / s.SCALE), box);
        if (box.min_i > prev.min_i || box.max_i < prev.max_i || box.max_j < prev.max_j) mono = false;
        if (!(box.min_i <= si && si <= box.max_i && box.min_j <= sj && sj <= box.max_j)) contains = false;
        prev = box;
    }
    CHECK(mono, "box shrank");
    CHECK(contains, "box lost the source");
    std::printf("  final box i[%d,%d] j[%d,%d] of %dx%d\n", box.min_i, box.max_i, box.min_j, box.max_j, s.W, s.H);
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Target pulse value at time t (linear interpolation of the kPulseRate-per-period table)
static double pulseAt(const std::vector<double>& tp, double f, double t) {
    const double u = t * f * kPulseRate;
    const size_t i = (size_t)u;
    if (t < 0 || i + 1 >= tp.size()) return 0.0;
    return tp[i] + (tp[i + 1] - tp[i]) * (u - (double)i);
}

static const Wavelet kAllWavelets[] = {Wavelet::Ricker, Wavelet::MinPhase, Wavelet::Gauss, Wavelet::GaussDeriv};
static const char* waveletName(Wavelet w) {
    return w == Wavelet::Ricker ? "ricker" : w == Wavelet::MinPhase ? "minphase" : w == Wavelet::Gauss ? "gauss" : "gaussder";
}
static Wavelet waveletFromArg(const std::string& a) {
    return a == "minphase" ? Wavelet::MinPhase : a == "gauss" ? Wavelet::Gauss : a == "gaussder" ? Wavelet::GaussDeriv : Wavelet::Ricker;
}

// Every wavelet except the Gaussian: zero area (no static pressure left at the source), unit peak, spectrum
// peaking at the dominant frequency, and band-limited enough for the grid.
static void testWavelets() {
    std::printf("[wavelet] unit peak; zero DC and spectral peak at f (all but the Gaussian)\n");
    for (Wavelet w : kAllWavelets)
        for (double f : {10.0, 50.0, 120.0}) {
            const double dt = 1e-5;
            std::vector<double> v;
            const std::vector<double> tp = targetPulse(w, 3 * kPulseRate);
            for (double t = 0; t < 3.0 / f; t += dt) v.push_back(pulseAt(tp, f, t));
            double area = 0, peak = 0;
            for (double x : v) { area += x * dt; peak = std::max(peak, std::fabs(x)); }
            // Amplitude spectrum by direct DFT on a frequency grid
            double best = 0, fBest = 0, e95 = 0, eTot = 0;
            std::vector<double> spec;
            for (double fr = 0; fr <= 8 * f; fr += f / 50) {
                double re = 0, im = 0;
                for (size_t k = 0; k < v.size(); k++) {
                    const double ph = 2 * M_PI * fr * k * dt;
                    re += v[k] * std::cos(ph); im -= v[k] * std::sin(ph);
                }
                const double a = std::hypot(re, im);
                spec.push_back(a * a);
                eTot += a * a;
                if (a > best) { best = a; fBest = fr; }
            }
            double acc = 0; int k95 = 0;
            for (; k95 < (int)spec.size() && acc < 0.95 * eTot; k95++) acc += spec[k95];
            e95 = k95 * f / 50;
            const char* name = waveletName(w);
            const bool gauss = w == Wavelet::Gauss;   // single peak: spectrum peaks at DC, area != 0
            std::printf("  %-8s f=%5.1f  peak=%.3f  area*f=%.1e  spectral peak=%.1f Hz  f95=%.0f Hz\n", name, f, peak,
                        area * f, fBest, e95);
            CHECK(std::fabs(peak - 1.0) < 1e-3, "%s f=%g: peak %.4f", name, f, peak);
            if (!gauss) CHECK(std::fabs(area * f) < 1e-3, "%s f=%g: non-zero area %.2e", name, f, area * f);
            if (!gauss) CHECK(std::fabs(fBest - f) <= 0.05 * f, "%s f=%g: spectral peak at %.1f Hz", name, f, fBest);
            CHECK(e95 < 3.0 * f, "%s f=%g: too broadband (95%% energy below %.0f Hz)", name, f, e95);
            {   // lobe structure: Gauss = one positive lobe, GaussDeriv = peak then equal trough
                double mn = 0, mx = 0; size_t imn = 0, imx = 0;
                for (size_t k = 0; k < v.size(); k++) { if (v[k] > mx) { mx = v[k]; imx = k; } if (v[k] < mn) { mn = v[k]; imn = k; } }
                if (gauss) CHECK(mn > -1e-6, "gauss f=%g: negative lobe %.3g", f, mn);
                if (w == Wavelet::GaussDeriv)
                    CHECK(imx < imn && std::fabs(mn + mx) < 1e-3, "gaussder f=%g: not peak-then-trough (%.3f / %.3f)", f, mx, mn);
            }
            // Shape: the minimum-phase pulse is front-loaded, the others are centred at 1.5/f
            double eAll = 0, eHalf = 0;
            for (size_t k = 0; k < v.size(); k++) { eAll += v[k] * v[k]; if (k * dt < 0.5 / f) eHalf += v[k] * v[k]; }
            const double frac = eHalf / eAll;
            std::printf("           energy in the first half period: %.0f%%\n", 100 * frac);
            if (w == Wavelet::MinPhase) CHECK(frac > 0.2, "minphase f=%g: not front-loaded (%.0f%% in 0.5/f)", f, 100 * frac);
            else CHECK(frac < 0.01, "%s f=%g: energy before 0.5/f (%.0f%%)", name, f, 100 * frac);
        }
}

// Temporal blocking must not change a single bit: pipelined vs barrier scheme, same batches
// (moving source, expanding-box rows, free surface, recording).
static void testPipelineMatchesBarrier() {
    std::printf("[pipeline] temporally blocked CPU scheme == barrier scheme\n");
    for (bool fs : {true, false}) {
        Setup s = makeSetup(1200, 480, 4.0);
        if (!fs) cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, false, s.bz, s.az);
        const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
        CpuSolver a, b;
        for (CpuSolver* c : {&a, &b}) {
            c->init(s.W, s.H, 6, sismoSize);
            c->setMaterial(materialOf(s));
            c->setCpml(s.bx, s.ax, s.bz, s.az);
        }
        a.setPipelined(false);
        b.setPipelined(true);
        for (int batch = 0; batch < 6; batch++) {
            auto st = makeSteps(s, batch * 97, 97, 3000);   // odd batch size: steps % threads != 0
            BatchJob J = makeJob(s, st, 10);
            J.freeSurface = fs;
            J.bymin = 3 + batch; J.bymax = s.H - 2;          // band narrower than the grid
            a.submit(J); a.wait();
            b.submit(J); b.wait();
        }
        double maxP = 0, dP = 0, dS = 0;
        for (size_t i = 0; i < a.P().size(); i++) {
            maxP = std::max(maxP, (double)std::fabs(a.P()[i]));
            dP = std::max(dP, (double)std::fabs(a.P()[i] - b.P()[i]));
        }
        for (size_t i = 0; i < sismoSize; i++) dS = std::max(dS, (double)std::fabs(a.sismo()[i] - b.sismo()[i]));
        std::printf("  fs=%d  max|P|=%.4g  |P diff|=%.3g  |sismo diff|=%.3g\n", fs, maxP, dP, dS);
        CHECK(maxP > 1e-6, "pipeline test: empty wavefield");
        CHECK(dP == 0.0 && dS == 0.0, "pipelined result differs from the barrier scheme (fs=%d)", fs);
    }
}

// Source wavelet through the whole controller (Simulation: wavelet table, half-integral
// compensation, point source, expanding box / eikonal, free surface, seismogram recording):
// Ricker and minimum phase x expanding box on/off x free surface on/off.
//   free surface off: the recorded pulse is the target wavelet (one arrival)
//   free surface on : direct pulse minus its surface ghost (image source mirrored about the
//                     Dirichlet row), each with 2D far-field spreading 1/sqrt(r)
//   expanding box on/off: identical traces
static void testSourceWaveletController() {
    std::printf("[source] wavelet shape through the controller: eikonal box x free surface\n");
    const double f = 50.0;
    for (Wavelet w : kAllWavelets) {
        const char* name = waveletName(w);
        for (bool fs : {false, true}) {
            std::vector<double> traces[2];
            double corr = 0, fPeak = 0;
            for (int box = 0; box < 2; box++) {
                Simulation sim;
                SimParams& p = sim.p;
                p.domain_W = 1200; p.domain_H = 2000; p.dh = 4; p.dt = 0.5f; p.fd_order = 10;
                p.geology = GeologyModel::Flat;          // seafloor at 1040 m: its echo arrives after 0.8 s
                p.wavelet = w; p.freq = (float)f; p.amp = 2;
                p.free_surface = fs; p.expanding_box = box == 1;
                p.ship_speed_knots = 0;                  // one shot, stationary geometry
                p.sismo_ms = 300; p.cable_len = 100; p.receiver_spacing = 12.5f;
                p.cable_head_depth = p.cable_tail_depth = 150; p.source_depth = 8;
                p.ship_src_dist = 50; p.src_rec_dist = 100;
                sim.OCEAN_Y = p.domain_H * 0.166;
                sim.init(false);
                sim.onShipX(750);                        // source at x = 700 m, head receiver at 600 m
                sim.startMode(Mode::Trad);
                const double T_sismo = std::floor((p.sismo_ms * 6.0) / (sim.SCALE * p.dt));
                double now = 0;
                while (sim.cycle_t < T_sismo + 5) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }
                sim.cpuSolver().wait();
                const int head = sim.N_REC - 1;
                const int NSm = sim.sismoSamples();
                traces[box].assign(sim.sismo.begin() + (size_t)head * kSismoStride, sim.sismo.begin() + (size_t)head * kSismoStride + NSm);

                if (box == 0) {
                    // Expected trace from the grid positions of source, receiver and mirror plane
                    const double S = sim.SCALE, dtp = sim.dtPhy();
                    const int si = (int)std::floor((750.0 - 50.0) / S), sj = (int)std::floor((sim.OCEAN_Y + 8.0) / S);
                    const int ri = (int)std::floor((750.0 - 50.0 - 100.0) / S), rj = (int)std::floor((sim.OCEAN_Y + 150.0) / S);
                    const int sjImg = 2 * sim.sjSurf() - sj;
                    const double r1 = S * std::hypot(si - ri, sj - rj), r2 = S * std::hypot(si - ri, sjImg - rj);
                    const std::vector<double> tp = targetPulse(w, 3 * kPulseRate);
                    std::vector<double> e(NSm);
                    for (int g = 0; g < NSm; g++) {
                        const int KS = sim.sismoStepsPerSample();   // sample centre: mean of its steps
                        const double t = 0.5 * (std::max(1, g * KS) + std::min((int)T_sismo, g * KS + KS - 1)) * dtp;
                        e[g] = pulseAt(tp, f, t - r1 / 1500.0) / std::sqrt(r1);
                        if (fs) e[g] -= pulseAt(tp, f, t - r2 / 1500.0) / std::sqrt(r2);
                    }
                    const std::vector<double>& m = traces[0];
                    double ne = 0, nm = 0;
                    for (int g = 0; g < NSm; g++) { ne += e[g] * e[g]; nm += m[g] * m[g]; }
                    for (int lag = -40; lag <= 40; lag++) {   // small global timing offset allowed
                        double c = 0;
                        for (int g = 0; g < NSm; g++) if (g + lag >= 0 && g + lag < NSm) c += e[g] * m[g + lag];
                        corr = std::max(corr, c / std::sqrt(ne * nm + 1e-300));
                    }
                    // Spectral peak of the recorded trace (free surface off: should be f)
                    const double dts = sim.sismoStepsPerSample() * dtp;
                    double best = 0;
                    for (double fr = 5; fr <= 150; fr += 0.5) {
                        double re = 0, im = 0;
                        for (int g = 0; g < NSm; g++) { re += m[g] * std::cos(2 * M_PI * fr * g * dts); im -= m[g] * std::sin(2 * M_PI * fr * g * dts); }
                        if (std::hypot(re, im) > best) { best = std::hypot(re, im); fPeak = fr; }
                    }
                }
            }
            double mx = 0, d = 0;
            for (int g = 0; g < (int)traces[0].size(); g++) { mx = std::max(mx, std::fabs(traces[0][g])); d = std::max(d, std::fabs(traces[0][g] - traces[1][g])); }
            std::printf("  %-8s fs=%d  corr with expected=%.4f  peak=%.1f Hz  box on/off diff=%.2e (of %.3g)\n", name, fs, corr,
                        fPeak, d / std::max(mx, 1e-30), mx);
            CHECK(mx > 1e-6, "%s fs=%d: nothing recorded", name, fs);
            CHECK(corr > (w == Wavelet::Gauss ? 0.90 : 0.98), "%s fs=%d: recorded pulse does not match the expected shape (%.4f)", name, fs, corr);
            CHECK(d <= 1e-5 * mx, "%s fs=%d: expanding box changes the trace (%.2e)", name, fs, d / mx);
            if (!fs && w != Wavelet::Gauss) CHECK(std::fabs(fPeak - f) <= 0.06 * f, "%s: recorded dominant frequency %.1f Hz", name, fPeak);
        }
    }
}

// A streamer whose spacing (6.25 m) is not a multiple of the grid (4 m) must still record an
// even direct-arrival moveout and a smooth amplitude decay: receivers are interpolated, not
// snapped to the nearest node. Two geometries: horizontal (cable at the source depth, no
// free surface: time linear in offset, amplitude ~ offset^-1/2) and the default marine one
// (free surface, source 8 m, cable 10 m: the ghost shapes the decay, so only trace-to-trace
// jumps are checked there - the second difference along the cable).
static void testReceiverInterpolation() {
    std::printf("[receivers] direct arrival on a 6.25 m streamer over a 4 m grid (CPU; testParity covers the GPU)\n");
    for (bool marine : {false, true}) {
        Simulation sim;
        SimParams& p = sim.p;
        p.domain_W = 1600; p.domain_H = 2000; p.dh = 4; p.dt = 0.5f; p.fd_order = 10;
        p.geology = GeologyModel::Flat; p.wavelet = Wavelet::Ricker; p.freq = 50; p.amp = 2;
        p.free_surface = marine; p.expanding_box = false; p.ship_speed_knots = 0;
        // The default 4000 ms record: with one sample per panel pixel (7.3 ms) the 50 Hz
        // arrival changed shape and amplitude from trace to trace
        p.sismo_ms = 4000; p.cable_len = 350; p.receiver_spacing = 6.25f;
        p.cable_head_depth = p.cable_tail_depth = marine ? 10.0f : 8.0f; p.source_depth = 8;
        p.ship_src_dist = 50; p.src_rec_dist = 100;
        sim.OCEAN_Y = p.domain_H * 0.166;
        sim.init(false);
        sim.onShipX(1300);                       // source at 1250 m, receivers 900..1150 m
        sim.startMode(Mode::Trad);
        const double T_sismo = sim.sismoSteps();
        const int NS = sim.sismoSamples();
        double now = 0;
        while (sim.cycle_t < 0.33 / sim.dtPhy()) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }   // past 300 m
        sim.cpuSolver().wait();
        const double dts = sim.sismoStepsPerSample() * sim.dtPhy() * 1000.0;   // ms per sample
        // Strongest lobe of the direct arrival (its sign from the nearest trace): time and
        // amplitude of its interpolated peak, offsets 100..300 m
        std::vector<double> off, tt, aa;
        double sgn = 0;
        for (int r = sim.N_REC - 1; r >= 0; r--) {
            const double o = p.src_rec_dist + p.cable_len - r * p.receiver_spacing;
            if (o > 300.0) break;
            const float* tr = sim.sismo.data() + (size_t)r * kSismoStride;
            const int g0 = (int)(o / 1.5 / dts), g1 = std::min(NS - 2, (int)((o / 1.5 + 70) / dts));
            if (sgn == 0) {
                int k = g0;
                for (int g = g0; g <= g1; g++) if (std::fabs(tr[g]) > std::fabs(tr[k])) k = g;
                sgn = tr[k] > 0 ? 1.0 : -1.0;
            }
            int k = g0;
            for (int g = g0; g <= g1; g++) if (sgn * tr[g] > sgn * tr[k]) k = g;
            // band-limited peak: 6-point Lagrange interpolation, 1/64-sample steps around k
            double bestV = -1e30, bestT = k;
            for (int q = -64; q <= 64; q++) {
                const double x = k + q / 64.0, x0 = std::floor(x), t = x - x0;
                double v = 0;
                for (int c = 0; c < 6; c++) {
                    double wgt = 1;
                    for (int m = 0; m < 6; m++) if (m != c) wgt *= (t - (m - 2)) / (double)(c - m);
                    v += wgt * sgn * tr[(int)x0 - 2 + c];
                }
                if (v > bestV) { bestV = v; bestT = x; }
            }
            off.push_back(o); tt.push_back(bestT * dts); aa.push_back(std::log(bestV));
        }
        // residuals from least-squares fits: t = a + b*offset, ln|A| = a + b*ln(offset)
        auto fitRes = [](const std::vector<double>& x, const std::vector<double>& y) {
            const size_t n = x.size();
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
            const double b = (n * sxy - sx * sy) / (n * sxx - sx * sx), a = (sy - b * sx) / n;
            double m = 0;
            for (size_t i = 0; i < n; i++) m = std::max(m, std::fabs(y[i] - a - b * x[i]));
            return m;
        };
        auto jump = [](const std::vector<double>& y) {   // max |second difference|
            double m = 0;
            for (size_t i = 1; i + 1 < y.size(); i++) m = std::max(m, std::fabs(y[i - 1] - 2 * y[i] + y[i + 1]));
            return m;
        };
        std::vector<double> lo(off.size());
        for (size_t i = 0; i < off.size(); i++) lo[i] = std::log(off[i]);
        const char* name = marine ? "marine" : "level";
        const double tJ = jump(tt), aJ = jump(aa);
        std::printf("  %-6s %zu traces  2nd difference: time %.3f ms  amplitude %.2f%%", name, off.size(), tJ, 100 * aJ);
        CHECK(off.size() >= 30, "%s: too few traces (%zu)", name, off.size());
        CHECK(tJ < 0.1, "%s: trace-to-trace jumps in the direct-arrival time (%.3f ms)", name, tJ);
        // marine: most of it is the curvature of the ghosted offset^-3/2 decay itself
        CHECK(aJ < (marine ? 0.012 : 0.006), "%s: trace-to-trace jumps in the direct-arrival amplitude (%.2f%%)", name, 100 * aJ);
        if (!marine) {
            const double tRes = fitRes(off, tt), aRes = fitRes(lo, aa);
            std::printf("  fit residual: time %.3f ms  amplitude %.2f%%", tRes, 100 * (std::exp(aRes) - 1));
            CHECK(tRes < 0.05, "level: uneven direct-arrival moveout (%.3f ms)", tRes);
            CHECK(aRes < 0.003, "level: uneven direct-arrival amplitude (%.2f%%)", 100 * (std::exp(aRes) - 1));
        }
        std::printf("\n");
    }
}

// Variable density. A density step with no velocity step reflects with the same coefficient
// at every angle, R = (rho2 - rho1) / (rho2 + rho1), so the trace above the interface must be
// the direct wave plus R times the wave of the mirror source - which a homogeneous run gives
// at the mirror image of the receiver. A uniform density of any value must leave P as it is.
// RMS of the (pi, pi) checkerboard component of a field: the grid-scale noise a step in a
// source time function leaves behind (it hardly propagates on the grid, so it stays visible).
static double checkerRms(const std::vector<float>& P, int W, int H) {
    double e = 0;
    long long n = 0;
    for (int j = 1; j < H - 1; j++)
        for (int i = 1; i < W - 1; i++) {
            double c = 0;
            for (int b = -1; b <= 1; b++)
                for (int a = -1; a <= 1; a++)
                    c += (a == 0 ? -2.0 : 1.0) * (b == 0 ? -2.0 : 1.0) / 16.0 * P[(size_t)(j + b) * W + i + a];
            e += c * c;
            n++;
        }
    return std::sqrt(e / std::max(1LL, n));
}

// Live run, record shorter than the sailing time between shots: with skip_gap the ship glides
// to the next shot instead of sailing through the idle part. Shot positions and recordings
// must be the same as sailing in real time; only the number of frames changes.
static void testSkipGap() {
    std::printf("[skip gap] jumping to the next shot once the record is complete\n");
    for (Mode m : {Mode::Trad, Mode::CLO}) {
        std::vector<float> rec[2];
        double shipX[2] = {0, 0};
        int frames[2] = {0, 0};
        for (int skip = 0; skip < 2; skip++) {
            Simulation sim;
            sim.p.sismo_ms = 300; sim.p.speed_ms = 40; sim.p.skip_gap = skip == 1;
            sim.init(false);
            sim.startMode(m);
            double now = 0;
            // Trad: until shot 2's record is complete. CLO: until the source vessel's record is.
            const double T = sim.sismoSteps();
            auto done = [&] {
                if (m == Mode::Trad) return sim.shotsMain >= 2 && sim.cycle_t >= T;
                const double mps = sim.p.ship_speed_knots * 0.514444;
                const double sw = std::floor((sim.p.shot_interval * 6000.0) / (mps * sim.SCALE * sim.p.dt));
                return sim.shotsSrc >= 1 && sim.cycle_t >= sw + T;
            };
            while (!done() && frames[skip] < 20000) {   // one batch per frame
                now += 1.0 / 60;
                sim.update(now, 1.0 / 60);
                sim.cpuSolver().wait();
                frames[skip]++;
                for (const Shot& sh : sim.shots)   // the shot of interest (cleared when the glide starts)
                    if (sh.main == (m == Mode::Trad)) shipX[skip] = sh.x;
            }
            sim.cpuSolver().wait();
            rec[skip] = sim.sismo;
        }
        // skip off leaves the CLO main shot's (decayed) wave in the field when the source vessel
        // fires; skip on clears it: compare with a tolerance there
        double mx = 0, d = 0;
        for (size_t i = 0; i < rec[0].size(); i++) {
            mx = std::max(mx, (double)std::fabs(rec[0][i]));
            d = std::max(d, (double)std::fabs(rec[0][i] - rec[1][i]));
        }
        std::printf("  %s: frames %d -> %d, shot source x %.2f / %.2f m, recordings max |diff| %.2e of %.3g\n",
                    m == Mode::Trad ? "Trad" : "CLO ", frames[0], frames[1], shipX[0], shipX[1], d, mx);
        CHECK(frames[1] * 2 < frames[0], "skip_gap did not shorten the cycle (%d vs %d frames)", frames[1], frames[0]);
        CHECK(std::fabs(shipX[0] - shipX[1]) < 1e-6, "shot fired at %.4f instead of %.4f m", shipX[1], shipX[0]);
        CHECK(mx > 0 && d <= (m == Mode::Trad ? 1e-6 : 1e-3) * mx, "recordings changed (%.2e of %.3g)", d, mx);
    }
}

// The injected target must end without a step: the minimum-phase pulse was cut at 3 periods,
// where it is still 1.2e-3 of its peak, and the step sent grid-scale rings out of the source
// (well visible at low frequency, where the pulse itself is smooth on the grid).
static void testPulseEnd() {
    std::printf("[pulse end] injected targets end smoothly and match the full pulse\n");
    const int n = 8 * kPulseRate;
    for (Wavelet w : kAllWavelets) {
        const std::vector<double> full = targetPulse(w, n), inj = injectedTarget(w, n);
        double dev = 0, jump = 0;
        for (int i = 0; i < n; i++) dev = std::max(dev, std::fabs(full[i] - inj[i]));
        // largest sample-to-sample change after the pulse (a smooth tail changes slowly)
        for (int i = 3 * kPulseRate; i < n; i++) jump = std::max(jump, std::fabs(inj[i] - inj[i - 1]));
        std::printf("  %-8s max |injected - full| %.1e  largest step after 3 periods %.1e\n", waveletName(w), dev, jump);
        CHECK(dev < 1e-5, "%s: injected pulse differs from the target by %.1e", waveletName(w), dev);
        CHECK(inj[n - 1] == 0.0, "%s: injected pulse does not end", waveletName(w));
    }
}

// A towed source moves while its pulse is injected; at low frequency the injection lasts
// seconds (8 periods: 4 s at 2 Hz). The source used to follow the ship from cell to cell, so
// crossing a cell boundary mid-pulse cut the source time function in one cell and started it
// in the next: a step that filled the wave field and the recordings with grid-scale noise.
static void testMovingSource() {
    std::printf("[moving source] 2 Hz shot from a towed source that crosses a cell boundary mid-pulse\n");
    double rel[2];
    for (int moving = 0; moving < 2; moving++) {
        Simulation sim;
        SimParams& p = sim.p;
        p.freq = 2;
        p.ship_speed_knots = moving ? 4.5f : 0.0f;
        sim.init(false);
        // source 1 m before a cell boundary: at 4.5 kn crossed ~0.4 s into the 4 s injection
        sim.onShipX((float)(std::floor((sim.sxShip - p.ship_src_dist) / sim.SCALE) * sim.SCALE + sim.SCALE - 1.0 + p.ship_src_dist));
        sim.startMode(Mode::Trad);
        double now = 0;
        while (sim.cycle_t * sim.dtPhy() < 2.0) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }
        sim.cpuSolver().wait();
        const std::vector<float>& P = sim.cpuSolver().P();
        double rms = 0;
        for (float v : P) rms += (double)v * v;
        rms = std::sqrt(rms / P.size());
        rel[moving] = checkerRms(P, sim.W_sim, sim.H_sim) / rms;
    }
    std::printf("  checkerboard / field rms at 2 s: fixed ship %.2e, moving ship %.2e\n", rel[0], rel[1]);
    CHECK(rel[1] < 3 * rel[0] + 1e-4, "moving source leaves grid-scale noise (%.2e vs %.2e)", rel[1], rel[0]);
}

static void testDensity() {
    std::printf("[density] uniform density leaves P unchanged; a density step reflects with R = (r2 - r1) / (r2 + r1)\n");
    Setup s = makeSetup(1600, 1200, 2);
    std::fill(s.vsq.begin(), s.vsq.end(), 0.0625f);                      // 1500 m/s everywhere
    cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, false, s.bz, s.az);          // no free surface
    const int is = s.W / 2, js = (int)std::lround(300.0 / s.SCALE);      // source 300 m below the top
    const int jIf = js + (int)std::lround(150.0 / s.SCALE);              // density step 150 m below it
    const int ir = is + (int)std::lround(200.0 / s.SCALE);               // receiver 200 m to the side
    const int jImg = 2 * jIf - 1 - js;                                   // receiver mirrored in z = (jIf - 1/2) dh
    const int nSteps = 1800;                                             // 0.3 s: past the reflection
    auto run = [&](const std::vector<float>& rho, std::vector<double>& rec, std::vector<double>& img,
                   std::vector<float>* field) {
        CpuSolver cpu;
        cpu.init(s.W, s.H, 4, (size_t)(s.nRec * 2 + s.nObs) * kSismoStride);
        cpu.setMaterial(makeMaterial(s.vsq, rho, s.W, s.H));
        cpu.setCpml(s.bx, s.ax, s.bz, s.az);
        rec.clear(); img.clear();
        for (int k = 0; k < nSteps; k += 1) {
            auto st = makeSteps(s, k, 1, 1e9);
            st[0].sj0 = (float)js; st[0].si0 = (float)is; st[0].rec_valid = 0;
            BatchJob J = makeJob(s, st, 10);
            J.freeSurface = false;
            cpu.submit(J);
            cpu.wait();
            rec.push_back(cpu.P()[(size_t)js * s.W + ir]);
            img.push_back(cpu.P()[(size_t)jImg * s.W + ir]);
        }
        if (field) *field = cpu.P();
    };
    const size_t N = (size_t)s.W * s.H;
    std::vector<double> a, aImg, c, cImg, b, bImg;
    std::vector<float> fa, fc;
    run(std::vector<float>(N, 1.0f), a, aImg, &fa);
    run(std::vector<float>(N, 2.5f), c, cImg, &fc);
    double mx = 0, d = 0;
    for (size_t i = 0; i < N; i++) { mx = std::max(mx, (double)std::fabs(fa[i])); d = std::max(d, (double)std::fabs(fa[i] - fc[i])); }
    std::vector<float> rho(N, 1.0f);
    const float r1 = 1.0f, r2 = 2.0f;
    for (int j = jIf; j < s.H; j++) std::fill(rho.begin() + (size_t)j * s.W, rho.begin() + (size_t)(j + 1) * s.W, r2);
    run(rho, b, bImg, nullptr);
    // least-squares R from b - a = R * aImg
    double num = 0, den = 0, peak = 0;
    for (int k = 0; k < nSteps; k++) { num += (b[k] - a[k]) * aImg[k]; den += aImg[k] * aImg[k]; peak = std::max(peak, std::fabs(b[k])); }
    const double R = num / den, Rth = (r2 - r1) / (r2 + r1);
    double res = 0, refl = 0;
    for (int k = 0; k < nSteps; k++) {
        res = std::max(res, std::fabs(b[k] - a[k] - R * aImg[k]));
        refl = std::max(refl, std::fabs(R * aImg[k]));
    }
    std::printf("  uniform rho 1 vs 2.5: max |P diff| %.2e of %.3g\n", d, mx);
    std::printf("  density step 1 -> 2: R = %.4f (theory %.4f), misfit %.2f %% of the reflection\n", R, Rth, 100 * res / refl);
    CHECK(d <= 1e-5 * mx, "a uniform density changes P (%.2e)", d / mx);
    CHECK(std::fabs(R - Rth) < 0.01, "density reflection coefficient %.4f instead of %.4f", R, Rth);
    CHECK(res < 0.05 * refl, "reflected wave does not match the mirror source (%.1f %%)", 100 * res / refl);
}

// ── Model files ─────────────────────────────────────────────────────────────
namespace modeltest {
void put16(std::vector<uint8_t>& b, size_t o, int v) { b[o] = (uint8_t)((v >> 8) & 0xFF); b[o + 1] = (uint8_t)(v & 0xFF); }
void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o] = (uint8_t)(v >> 24); b[o + 1] = (uint8_t)(v >> 16); b[o + 2] = (uint8_t)(v >> 8); b[o + 3] = (uint8_t)v;
}
uint32_t toIbm(float f) {
    if (f == 0.0f) return 0;
    uint32_t sign = f < 0 ? 0x80000000u : 0;
    double a = std::fabs((double)f);
    int ex = 64;
    while (a >= 1.0) { a /= 16.0; ex++; }
    while (a < 1.0 / 16.0) { a *= 16.0; ex--; }
    return sign | (uint32_t)ex << 24 | (uint32_t)std::lround(a * 16777216.0);
}
// The FWI program's layout: one trace per column, interval = dz * 1000, source X = x (scalar -10)
std::vector<uint8_t> segyModel(const GridModel& m, int format) {
    const size_t TB = 240 + (size_t)m.nz * 4;
    std::vector<uint8_t> b(3600 + (size_t)m.nx * TB, 0);
    put16(b, 3216, (int)std::lround(m.dz * 1000));
    put16(b, 3220, m.nz);
    put16(b, 3224, format);
    for (int ix = 0; ix < m.nx; ix++) {
        const size_t t = 3600 + (size_t)ix * TB;
        put16(b, t + 70, -10);
        put32(b, t + 72, (uint32_t)(int32_t)std::lround(ix * m.dx * 10));
        put16(b, t + 114, m.nz);
        for (int iz = 0; iz < m.nz; iz++) {
            const float v = m.at(ix, iz);
            uint32_t bits;
            if (format == 1) bits = toIbm(v); else std::memcpy(&bits, &v, 4);
            put32(b, t + 240 + (size_t)iz * 4, bits);
        }
    }
    return b;
}
void writeBytes(const std::filesystem::path& p, const void* d, size_t n) {
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d, (std::streamsize)n);
}
double maxDiff(const GridModel& a, const GridModel& b) {
    if (a.nx != b.nx || a.nz != b.nz) return 1e30;
    double d = 0;
    for (size_t i = 0; i < a.v.size(); i++) d = std::max(d, (double)std::fabs(a.v[i] - b.v[i]));
    return d;
}
} // namespace modeltest

static void testModelImport() {
    using namespace modeltest;
    std::printf("[model import] SEG-Y / raw / CSV velocity models, units, loading into the simulation\n");
    // 2 km x 600 m: water down to 200 m on the left, 120 m on the right, then 1800 m/s + gradient
    GridModel m;
    m.nx = 101; m.nz = 61; m.dx = 20; m.dz = 10;
    m.v.resize((size_t)m.nx * m.nz);
    auto seafloor = [&](double x) { return 200.0 - 80.0 * x / 2000.0; };
    for (int ix = 0; ix < m.nx; ix++)
        for (int iz = 0; iz < m.nz; iz++) {
            const double x = ix * m.dx, z = iz * m.dz;
            m.v[(size_t)ix * m.nz + iz] = z < seafloor(x) ? 1500.0f : (float)(1800.0 + 1.5 * (z - seafloor(x)));
        }
    const auto dir = std::filesystem::temp_directory_path() / "fdtd_model_test";
    std::filesystem::create_directories(dir);
    auto u8 = [](const std::filesystem::path& p) { const std::u8string s = p.u8string(); return std::string(s.begin(), s.end()); };
    std::string err;

    for (int fmt : {5, 1}) {   // IEEE and IBM floats
        auto bytes = segyModel(m, fmt);
        const auto p = dir / (fmt == 5 ? "vel_ieee.sgy" : "vel_ibm.sgy");
        writeBytes(p, bytes.data(), bytes.size());
        SegyModelInfo info;
        const bool ok = probeSegyModel(u8(p), info, err);
        CHECK(ok, "SEG-Y probe (format %d): %s", fmt, err.c_str());
        GridModel r;
        const bool ok2 = ok && readSegyModel(u8(p), info.dx, info.dz, r, err);
        CHECK(ok2, "SEG-Y read (format %d): %s", fmt, err.c_str());
        const double d = ok2 ? maxDiff(m, r) : 1e30;
        std::printf("  SEG-Y format %d: %dx%d dx=%g dz=%g  max diff %.3g m/s\n", fmt, info.nx, info.nz, info.dx, info.dz, d);
        CHECK(info.nx == m.nx && info.nz == m.nz && info.dx == m.dx && info.dz == m.dz, "SEG-Y header (format %d)", fmt);
        CHECK(d < (fmt == 1 ? 1e-3 : 1e-6), "SEG-Y values (format %d): %.3g", fmt, d);
    }
    {   // raw float32, both orders
        std::vector<float> colOrder = m.v, rowOrder(m.v.size());
        for (int ix = 0; ix < m.nx; ix++)
            for (int iz = 0; iz < m.nz; iz++) rowOrder[(size_t)iz * m.nx + ix] = m.at(ix, iz);
        writeBytes(dir / "vel_col.bin", colOrder.data(), colOrder.size() * 4);
        writeBytes(dir / "vel_row.bin", rowOrder.data(), rowOrder.size() * 4);
        GridModel a, b;
        const bool okA = readRawModel(u8(dir / "vel_col.bin"), m.nx, m.nz, m.dx, m.dz, false, false, a, err);
        const bool okB = readRawModel(u8(dir / "vel_row.bin"), m.nx, m.nz, m.dx, m.dz, true, false, b, err);
        CHECK(okA && okB, "raw read: %s", err.c_str());
        std::printf("  raw float32: column order diff %.3g, row order diff %.3g\n", maxDiff(m, a), maxDiff(m, b));
        CHECK(maxDiff(m, a) == 0 && maxDiff(m, b) == 0, "raw values");
        GridModel bad;
        CHECK(!readRawModel(u8(dir / "vel_col.bin"), m.nx + 1, m.nz, m.dx, m.dz, false, false, bad, err), "raw size mismatch accepted");
    }
    {   // CSV in km/s with ';' and a decimal comma, plus a title row
        std::ofstream f(dir / "vel.csv", std::ios::binary);
        f << "# velocity model\n";
        for (int ix = 0; ix < m.nx; ix++) f << (ix ? ";" : "") << "x" << ix;
        f << "\n";
        char b[32];
        for (int iz = 0; iz < m.nz; iz++) {
            for (int ix = 0; ix < m.nx; ix++) {
                std::snprintf(b, sizeof(b), "%.4f", m.at(ix, iz) / 1000.0);
                std::string t = b;
                std::replace(t.begin(), t.end(), '.', ',');
                f << (ix ? ";" : "") << t;
            }
            f << "\r\n";
        }
        f.close();
        int nx = 0, nz = 0;
        const bool okP = probeAsciiModel(u8(dir / "vel.csv"), nx, nz, err);
        GridModel a;
        const bool ok = okP && readAsciiModel(u8(dir / "vel.csv"), m.dx, m.dz, a, err);
        CHECK(ok, "CSV read: %s", err.c_str());
        std::string note;
        const bool okU = ok && normalizeModelUnits(a, ModelQuantity::Velocity, note, err);
        CHECK(okU, "CSV units: %s", err.c_str());
        std::printf("  CSV (km/s, ';', decimal comma): %dx%d  max diff %.3g m/s  note: %s\n", nx, nz, okU ? maxDiff(m, a) : -1.0, note.c_str());
        CHECK(nx == m.nx && nz == m.nz && okU && maxDiff(m, a) < 0.01, "CSV values");
    }
    {   // load into the simulation (CPU only): domain, grid velocity, Gardner density, seafloor / OBS
        Simulation sim;
        sim.p.dh = 4;
        sim.OCEAN_Y = sim.p.domain_H * 0.166;
        sim.init(false);
        GridModel vel = m;
        const bool ok = sim.loadModel(vel, GridModel{}, err);
        CHECK(ok, "loadModel: %s", err.c_str());
        if (ok) {
            const double W = sim.p.domain_W, D = sim.p.domain_H - sim.OCEAN_Y;
            // grid node at x = 1000 m, 300 m below the sea surface
            const int i = (int)std::lround(1000.0 / sim.SCALE), j = (int)std::lround((sim.OCEAN_Y + 300.0) / sim.SCALE);
            const double zNode = j * sim.SCALE - sim.OCEAN_Y;
            const double vGrid = std::sqrt(sim.vsq[(size_t)j * sim.W_sim + i]) * 6000.0;
            const double vTrue = 1800.0 + 1.5 * (zNode - seafloor(i * sim.SCALE));
            const double rGrid = sim.rho[(size_t)j * sim.W_sim + i], rTrue = 0.31 * std::pow(vTrue, 0.25);
            const double rWater = sim.rho[(size_t)(sim.sjSurf() + 5) * sim.W_sim + i];
            const double sf = sim.seafloorDepth(1000.0) - sim.OCEAN_Y;
            std::printf("  loaded: domain %.0f m x %.0f m below the surface, v(1000, %.0f) = %.1f (model %.1f) m/s, "
                        "rho %.4f (Gardner %.4f), water rho %.2f, seafloor at x=1000: %.1f m (model %.1f)\n",
                        W, D, zNode, vGrid, vTrue, rGrid, rTrue, rWater, sf, seafloor(1000.0));
            CHECK(std::fabs(W - 2000) < 1e-3 && std::fabs(D - 600) < 0.5, "domain size %.1f x %.1f", W, D);
            CHECK(std::fabs(vGrid - vTrue) < 1.0, "grid velocity %.2f vs %.2f", vGrid, vTrue);
            CHECK(std::fabs(rGrid - rTrue) < 1e-3 && std::fabs(rWater - 1.03) < 1e-6, "Gardner density");
            // the model only knows the seafloor to within its sample interval dz
            CHECK(std::fabs(sf - seafloor(1000.0)) <= m.dz, "seafloor from the model %.1f vs %.1f", sf, seafloor(1000.0));
            sim.clearModel();
            CHECK(!sim.hasModel() && sim.p.geology == GeologyModel::Flat, "clearModel");
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Line simulation: one SEG-Y per receiver type holding every shot (FFID, source X, traces),
// and each shot independent of the ones before it (same traces as a line of that one shot).
namespace linetest {
int32_t be32(const std::vector<uint8_t>& b, size_t o) {
    return (int32_t)((uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3]);
}
int be16(const std::vector<uint8_t>& b, size_t o) { return (int16_t)(b[o] << 8 | b[o + 1]); }
float bef(const std::vector<uint8_t>& b, size_t o) { uint32_t u = (uint32_t)be32(b, o); float f; std::memcpy(&f, &u, 4); return f; }
std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// Runs a line on the CPU; returns the line folder ("" on failure)
std::string runLine(double firstX, int n, double interval, const std::filesystem::path& root, std::string& alert,
                    Mode mode = Mode::Trad, float* srcOffset = nullptr) {
    Simulation sim;
    sim.p.sismo_ms = 300;
    sim.OCEAN_Y = sim.p.domain_H * 0.166;
    sim.init(false);
    Simulation::LineSettings ls;
    ls.firstX = firstX; ls.nShots = n; ls.interval = interval; ls.mode = mode;
    if (srcOffset) *srcOffset = sim.p.src_offset;
    const std::u8string u = root.u8string();
    ls.folder.assign(u.begin(), u.end());
    std::string err;
    if (!sim.startLine(ls, err)) { alert = err; return ""; }
    double now = 0;
    const auto t0 = std::chrono::steady_clock::now();   // wall clock: update() returns at once while a batch runs
    while (sim.lineActive() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) {
        now += 1.0 / 60;
        sim.update(now, 1.0 / 60);
        std::this_thread::yield();
    }
    alert = !sim.notices.empty() ? sim.notices.back() : sim.alerts.empty() ? "" : sim.alerts.back();
    return sim.lineActive() ? "" : sim.lineFolder();
}
} // namespace linetest

// Velocity model drawn as an image: gray levels, merged colours, the model in the simulation
static void testImageModel() {
    std::printf("[image model] gray levels of an image -> velocities -> simulation\n");
    const auto dir = std::filesystem::temp_directory_path() / "fdtd_image_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // 60 x 30: rows 0-9 black, 10-19 red (200,50,50), 20-29 green (0,162,0) - red and green both
    // have luma 95 - and a white 10 x 5 block at the bottom right
    report::Image im;
    im.w = 60; im.h = 30;
    im.rgb.assign((size_t)im.w * im.h * 3, 0);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            uint8_t* p = &im.rgb[((size_t)y * im.w + x) * 3];
            if (y >= 25 && x >= 50) { p[0] = p[1] = p[2] = 255; }
            else if (y >= 20) { p[1] = 162; }
            else if (y >= 10) { p[0] = 200; p[1] = 50; p[2] = 50; }
        }
    std::string err;
    const auto png = dir / "model.png";
    CHECK(report::writePng(png.string(), im, err), "test image not written: %s", err.c_str());
    GrayImage g;
    const bool ok = loadGrayImage(png.string(), g, err);
    CHECK(ok, "image not read: %s", err.c_str());
    if (!ok) return;
    const std::vector<int> lv = g.levels();
    std::printf("  %dx%d px, levels:", g.w, g.h);
    for (int l : lv) std::printf(" %d (%d px)", l, g.count[l]);
    std::printf(", %d colours\n", g.colours);
    CHECK(g.w == 60 && g.h == 30, "size %dx%d", g.w, g.h);
    CHECK(lv == std::vector<int>({0, 95, 255}), "gray levels not 0 / 95 / 255");
    CHECK(g.count[0] == 600 && g.count[95] == 1150 && g.count[255] == 50, "pixel counts %d / %d / %d", g.count[0], g.count[95], g.count[255]);
    CHECK(g.colours == 4, "%d colours (red and green share gray 95: expected 4 colours for 3 levels)", g.colours);

    std::array<float, 256> vel{};
    vel[0] = 1500; vel[95] = 2500; vel[255] = 4000;
    GridModel m = modelFromGray(g, vel, 1180, 580, "model.png");   // 20 m per pixel
    CHECK(m.nx == 60 && m.nz == 30 && std::fabs(m.dx - 20) < 1e-9 && std::fabs(m.dz - 20) < 1e-9, "grid %dx%d dx %g dz %g", m.nx, m.nz, m.dx, m.dz);
    Simulation sim;
    sim.init(false);
    CHECK(sim.loadModel(m, GridModel{}, err), "model not loaded: %s", err.c_str());
    sim.alerts.clear();
    const GridModel& v = sim.modelVelocity();
    // Inside each band (away from the bilinear edges) and in the white block
    const double a = v.sample(300, 100), b = v.sample(300, 300), c = v.sample(300, 500), d = v.sample(1150, 560);
    std::printf("  model: %.0f / %.0f / %.0f / %.0f m/s; domain %.0f x %.0f m, seafloor at x=300: %.0f m\n", a, b, c, d, sim.p.domain_W,
                sim.p.domain_H, sim.seafloorBelowSurface(300));
    CHECK(a == 1500 && b == 2500 && c == 2500 && d == 4000, "velocities %.0f %.0f %.0f %.0f", a, b, c, d);
    CHECK(std::fabs(sim.p.domain_W - 1180) < 1 && std::fabs(sim.seafloorBelowSurface(300) - 190) <= 12,
          "domain / seafloor not taken from the image (%.0f m, %.0f m)", sim.p.domain_W, sim.seafloorBelowSurface(300));
    {   // Larger than the 40 million pixels once allowed, and read in several bands: a 24-bit
        // BMP of 24000 x 3000 (72 Mpx), left half black, right half white, one gray row in the middle
        const int W = 24000, H = 3000, stride = W * 3;
        const auto bmp = dir / "big.bmp";
        {
            std::ofstream f(bmp, std::ios::binary);
            auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
            auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
            f.write("BM", 2); u32(54u + (uint32_t)stride * H); u32(0); u32(54);
            u32(40); u32(W); u32((uint32_t)-H); u16(1); u16(24); u32(0); u32((uint32_t)stride * H); u32(2835); u32(2835); u32(0); u32(0);
            std::vector<uint8_t> row(stride);
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) std::memset(&row[(size_t)x * 3], y == H / 2 ? 128 : x < W / 2 ? 0 : 255, 3);
                f.write((const char*)row.data(), stride);
            }
        }
        GrayImage big;
        const bool okBig = loadGrayImage(bmp.string(), big, err);
        std::printf("  %dx%d BMP (%.0f Mpx): %s, levels 0: %d, 128: %d, 255: %d px\n", W, H, W * (double)H / 1e6, okBig ? "read" : err.c_str(),
                    big.count[0], big.count[128], big.count[255]);
        CHECK(okBig && big.w == W && big.h == H && big.count[128] == W && big.count[0] == (H - 1) * (W / 2) && big.count[255] == (H - 1) * (W / 2)
                  && big.gray[(size_t)(H / 2) * W + 7] == 128 && big.gray[(size_t)(H - 1) * W + W - 1] == 255,
              "large image not read correctly: %s", err.c_str());
        std::filesystem::remove(bmp, ec);
    }
    // Not an image
    const auto txt = dir / "not_an_image.png";
    { std::ofstream(txt) << "hello"; }
    GrayImage bad;
    CHECK(!loadGrayImage(txt.string(), bad, err) && !err.empty(), "a text file was read as an image");
    std::filesystem::remove_all(dir, ec);
}

// Hand-drawn model: boundary fill, lines that stop a flood fill, undo, palette, model
static void testModelCanvas() {
    std::printf("[model drawing] boundary, line, fill, undo, palette -> model\n");
    ModelCanvas cv;
    cv.initDefault(200, 100);   // water over "Tortul 1" from row 35
    CHECK(cv.at(10, 10) == 0 && cv.at(10, 50) == 1, "default canvas: water over sediment");
    // Boundary from (0, 60) to (200, 80): below it layer 3
    cv.snapshot();
    cv.fillBelow({{150, 75}, {50, 65}}, 3);   // unsorted points, extended flat to the edges
    CHECK(cv.at(0, 64) == 1 && cv.at(0, 66) == 3, "left edge: flat extension of the first point (y 65)");
    CHECK(cv.at(100, 69) == 1 && cv.at(100, 71) == 3, "x = 100: boundary at y 70");
    CHECK(cv.at(199, 74) == 1 && cv.at(199, 76) == 3, "right edge: flat extension of the last point (y 75)");
    // A vertical line splits the water; a fill on the left stays on the left
    cv.snapshot();
    cv.line(100, 0, 100, 34, 1, 2);
    const int n = cv.floodFill(10, 10, 4);
    CHECK(cv.at(10, 10) == 4 && cv.at(150, 10) == 0, "the line did not stop the fill");
    CHECK(n > 0 && n < 100 * 35, "filled %d pixels", n);
    // Undo the fill and the line, redo one step
    CHECK(cv.undo() && cv.at(10, 10) == 0 && cv.at(100, 10) == 0, "undo did not restore the canvas");
    CHECK(cv.redo() && cv.at(100, 10) == 2 && cv.at(10, 10) == 4, "redo did not reapply");
    // Remove palette entry 2 (the line): its pixels go to 0, entries above move down
    const std::vector<int> before = cv.counts();
    cv.removeEntry(2, 0);
    const std::vector<int> after = cv.counts();
    CHECK(cv.palette.size() == 5 && after[0] == before[0] + before[2] && after[2] == before[3] && cv.palette[2].name == "Kireçtaşı",
          "palette removal renumbered wrongly");
    // Model: velocities by layer, 1 m per pixel
    GridModel m = cv.toModel(199, 99, "Çizim");
    CHECK(m.nx == 200 && m.nz == 100 && std::fabs(m.dx - 1) < 1e-9, "model grid %dx%d dx %g", m.nx, m.nz, m.dx);
    CHECK(m.at(150, 10) == 1500 && m.at(10, 10) == cv.palette[3].velocity && m.at(100, 90) == cv.palette[2].velocity,
          "model velocities %g %g %g", m.at(150, 10), m.at(10, 10), m.at(100, 90));
    // Back from a velocity grid (the salt model): one layer per velocity
    Simulation sim;
    sim.init(false);
    const int j0 = (int)std::ceil(sim.OCEAN_Y / sim.SCALE - 1e-9), nz = sim.H_sim - j0;
    std::vector<float> rows((size_t)sim.W_sim * nz);
    for (int k = 0; k < nz; k++)
        for (int i = 0; i < sim.W_sim; i++) rows[(size_t)k * sim.W_sim + i] = (float)(std::sqrt((double)sim.vsq[(size_t)(j0 + k) * sim.W_sim + i]) * 6000.0);
    ModelCanvas c2;
    c2.fromVelocity(rows.data(), sim.W_sim, nz, 300, 100);
    std::printf("  from the salt model: %zu layers:", c2.palette.size());
    for (auto& pe : c2.palette) std::printf(" %.0f", pe.velocity);
    std::printf("\n");
    CHECK(c2.palette.size() == 4 && c2.palette.front().velocity == 1500 && c2.palette.back().velocity == 4080,
          "salt model gave %zu layers", c2.palette.size());

    // Drawing file: save, open into another canvas, identical; bad files rejected untouched
    cv.palette[1].name = "Şeyl ğüıöç";
    cv.palette[1].rgb = 0x12AB34;
    const std::string js = cv.toJson(1234.5, 567);
    ModelCanvas c3;
    double w3 = 0, d3 = 0;
    std::string err;
    const bool opened = c3.fromJson(js, w3, d3, err);
    bool same = opened && c3.w == cv.w && c3.h == cv.h && c3.px == cv.px && c3.palette.size() == cv.palette.size();
    for (size_t i = 0; same && i < cv.palette.size(); i++)
        same = c3.palette[i].name == cv.palette[i].name && c3.palette[i].velocity == cv.palette[i].velocity && c3.palette[i].rgb == cv.palette[i].rgb;
    std::printf("  drawing file: %zu bytes for %dx%d pixels, %zu layers; read back %s\n", js.size(), cv.w, cv.h, cv.palette.size(),
                same ? "identical" : "DIFFERENT");
    CHECK(same && w3 == 1234.5 && d3 == 567, "drawing file did not round-trip: %s", err.c_str());
    ModelCanvas c4 = c3;
    double w4 = 1, d4 = 1;
    CHECK(!c4.fromJson("{not json", w4, d4, err) && !c4.fromJson("{\"format\":\"something else\"}", w4, d4, err), "bad files accepted");
    std::string cut = js;
    cut.replace(cut.find("\"pixels_rle\""), std::string::npos, "\"pixels_rle\": [0, 5]}");
    CHECK(!c4.fromJson(cut, w4, d4, err) && c4.px == c3.px && w4 == 1, "a truncated drawing changed the canvas (%s)", err.c_str());
}

static void testLineExport() {
    using namespace linetest;
    std::printf("[line] every shot of a line into line_streamer.segy / line_obs.segy\n");
    const auto root = std::filesystem::temp_directory_path() / "fdtd_line_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    std::string alert;
    const std::string dir3 = runLine(500, 3, 50, root / "a", alert);
    CHECK(!dir3.empty(), "line did not finish: %s", alert.c_str());
    if (dir3.empty()) return;
    CHECK(alert.find("tamamland") != std::string::npos, "unexpected final message: %s", alert.c_str());
    const auto st = readFile(std::filesystem::u8path(dir3) / "line_streamer.segy");
    const auto ob = readFile(std::filesystem::u8path(dir3) / "line_obs.segy");
    CHECK(st.size() > 3600 && ob.size() > 3600, "line files missing or empty");
    if (st.size() <= 3600 || ob.size() <= 3600) return;
    const int ns = be16(st, 3220), dtUs = be16(st, 3216);
    const size_t TB = 240 + (size_t)ns * 4;
    const size_t nTr = (st.size() - 3600) / TB, nObsTr = (ob.size() - 3600) / TB;
    const int nRec = be16(st, 3212);   // traces per ensemble
    int ffidOk = 0, sxOk = 0, seqOk = 0;
    for (size_t t = 0; t < nTr; t++) {
        const size_t h = 3600 + t * TB;
        const int shot = (int)(t / nRec);
        ffidOk += be32(st, h + 8) == shot + 1;
        sxOk += be32(st, h + 72) == (int32_t)std::lround(500 + 50.0 * shot);
        seqOk += be32(st, h + 0) == (int32_t)(t + 1);
    }
    std::printf("  streamer: %zu traces (%d per shot), %d samples at %d us; OBS: %zu traces\n", nTr, nRec, ns, dtUs, nObsTr);
    CHECK(nTr == (size_t)3 * nRec && (st.size() - 3600) % TB == 0, "streamer trace count %zu", nTr);
    CHECK(nObsTr > 0 && nObsTr % 3 == 0, "OBS trace count %zu", nObsTr);
    CHECK(ns == 300 && dtUs == 1000, "samples %d / interval %d", ns, dtUs);
    CHECK(ffidOk == (int)nTr && sxOk == (int)nTr && seqOk == (int)nTr, "headers: FFID %d, source X %d, sequence %d of %zu",
          ffidOk, sxOk, seqOk, nTr);
    {   // OBS water depth (bytes 65-68): the seafloor below the sea surface, not from the top of
        // the domain (that counted the ~80 m air strip above the sea in)
        Simulation ref;
        ref.p.sismo_ms = 300;
        ref.OCEAN_Y = ref.p.domain_H * 0.166;
        ref.init(false);
        const size_t nObs1 = nObsTr / 3;
        int ok = 0;
        double worst = 0;
        for (size_t i = 0; i < nObs1; i++) {
            const double x = (i + 1) * ref.p.obs_spacing;
            const double want = ref.seafloorDepth(x) - ref.OCEAN_Y;
            const double got = be32(ob, 3600 + i * TB + 64);
            worst = std::max(worst, std::fabs(got - want));
            ok += std::fabs(got - want) <= 0.5 && got > 0;
        }
        std::printf("  OBS water depth in the headers: %d of %zu match the seafloor below the sea surface (worst %.2f m)\n", ok, nObs1, worst);
        CHECK(ok == (int)nObs1, "OBS depth headers off by up to %.1f m", worst);
    }
    {   // model image, model SEG-Y (read back with the app's import) and the PDF report
        const auto d = std::filesystem::u8path(dir3);
        const auto png = readFile(d / "model_velocity.png"), pdf = readFile(d / "line_report.pdf");
        const bool pngOk = png.size() > 1000 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G';
        const std::string pdfHead(pdf.begin(), pdf.begin() + std::min<size_t>(8, pdf.size()));
        const std::string pdfTail(pdf.end() - std::min<size_t>(6, pdf.size()), pdf.end());
        const bool pdfOk = pdf.size() > 1000 && pdfHead.rfind("%PDF-", 0) == 0 && pdfTail.find("%%EOF") != std::string::npos;
        GridModel m;
        std::string err;
        SegyModelInfo info;
        const std::u8string mp = (d / "model_velocity.segy").u8string();
        const std::string mps(mp.begin(), mp.end());
        const bool probed = probeSegyModel(mps, info, err);
        const bool read = probed && readSegyModel(mps, info.dx, info.dz, m, err);
        std::printf("  report: png %zu bytes, pdf %zu bytes; model SEG-Y %dx%d dx=%g dz=%g, %.0f - %.0f m/s\n", png.size(), pdf.size(),
                    m.nx, m.nz, m.dx, m.dz, read ? m.minValue() : 0.0, read ? m.maxValue() : 0.0);
        CHECK(pngOk, "model_velocity.png missing or not a PNG");
        CHECK(pdfOk, "line_report.pdf missing or not a PDF");
        CHECK(read && m.nx == 301 && m.dx == 4 && m.dz == 4 && std::fabs(m.minValue() - 1500) < 1 && std::fabs(m.maxValue() - 4080) < 1,
              "model SEG-Y does not read back: %s", err.c_str());
        if (const char* keep = std::getenv("FDTD_KEEP_LINE")) {   // copy the outputs out for a look
            for (const char* n : {"model_velocity.png", "line_report.pdf", "model_velocity.segy"})
                std::filesystem::copy_file(d / n, std::filesystem::u8path(keep) / n, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    // shot 2 of the line == a line of only that shot
    const std::string dir1 = runLine(550, 1, 50, root / "b", alert);
    CHECK(!dir1.empty(), "single-shot line did not finish: %s", alert.c_str());
    if (dir1.empty()) return;
    const auto one = readFile(std::filesystem::u8path(dir1) / "line_streamer.segy");
    double mx = 0, d = 0;
    for (int i = 0; i < nRec && one.size() >= 3600 + (size_t)nRec * TB; i++)
        for (int k = 0; k < ns; k++) {
            const float a = bef(st, 3600 + ((size_t)nRec + i) * TB + 240 + (size_t)k * 4);
            const float b = bef(one, 3600 + (size_t)i * TB + 240 + (size_t)k * 4);
            mx = std::max(mx, (double)std::fabs(b));
            d = std::max(d, (double)std::fabs(a - b));
        }
    std::printf("  shot 2 of the line vs the same shot alone: max |diff| %.3g of %.3g\n", d, mx);
    CHECK(mx > 0 && d <= 1e-6 * mx, "shot 2 depends on shot 1 (%.3g of %.3g)", d, mx);

    // CLO: per station a main record and a source-vessel record (its own streamer file, both
    // in the OBS file); the source vessel's source X is src_offset ahead, plus the distance
    // the ship sailed through the main record (300 ms at 4.5 kn)
    float off = 0;
    const std::string dirC = runLine(500, 2, 50, root / "c", alert, Mode::CLO, &off);
    CHECK(!dirC.empty(), "CLO line did not finish: %s", alert.c_str());
    if (!dirC.empty()) {
        const auto m = readFile(std::filesystem::u8path(dirC) / "line_streamer.segy");
        const auto sv = readFile(std::filesystem::u8path(dirC) / "line_streamer_src.segy");
        const auto ob2 = readFile(std::filesystem::u8path(dirC) / "line_obs.segy");
        const size_t nm = m.size() > 3600 ? (m.size() - 3600) / TB : 0, nsv = sv.size() > 3600 ? (sv.size() - 3600) / TB : 0;
        const size_t nob = ob2.size() > 3600 ? (ob2.size() - 3600) / TB : 0;
        int sxv = 0;
        double peak = 0;
        for (size_t t = 0; t < nsv; t++) {
            const int shot = (int)(t / nRec);
            sxv += std::abs(be32(sv, 3600 + t * TB + 72) - (int32_t)std::lround(500 + 50.0 * shot + off + 4.5 * 0.514444 * 0.3)) <= 1;
            for (int k = 0; k < ns; k++) peak = std::max(peak, (double)std::fabs(bef(sv, 3600 + t * TB + 240 + (size_t)k * 4)));
        }
        std::printf("  CLO: main %zu, source vessel %zu streamer traces (source X ok %d), OBS %zu traces; source vessel peak %.3g\n",
                    nm, nsv, sxv, nob, peak);
        CHECK(nm == (size_t)2 * nRec && nsv == (size_t)2 * nRec, "CLO streamer traces %zu / %zu", nm, nsv);
        CHECK(sxv == (int)nsv, "CLO source vessel source X in %d of %zu headers", sxv, nsv);
        CHECK(nob == 2 * nObsTr / 3 * 2, "CLO OBS traces %zu (expected %zu)", nob, 2 * nObsTr / 3 * 2);
        CHECK(peak > 0, "CLO source vessel record is empty");
    }
    // SLO: one blended record per station, different from the single-source one (compared on
    // the OBS gather: the source vessel is ahead of the ship, its wave reaches the streamer
    // only at the end of the 300 ms record)
    const std::string dirS = runLine(500, 1, 50, root / "d", alert, Mode::SLO);
    CHECK(!dirS.empty(), "SLO line did not finish: %s", alert.c_str());
    if (!dirS.empty()) {
        const auto sl = readFile(std::filesystem::u8path(dirS) / "line_obs.segy");
        const size_t nObs1 = nObsTr / 3;
        double dd = 0;
        for (size_t i = 0; i < nObs1 && sl.size() >= 3600 + nObs1 * TB; i++)
            for (int k = 0; k < ns; k++) {
                const size_t o = 3600 + i * TB + 240 + (size_t)k * 4;
                dd = std::max(dd, (double)std::fabs(bef(sl, o) - bef(ob, o)));
            }
        std::printf("  SLO: blended record differs from the single-source one by %.3g\n", dd);
        CHECK(dd > 1e-3, "SLO line recorded no second source");
    }
    std::filesystem::remove_all(root, ec);
}

static void testSegy() {
    std::printf("[segy] Rev1 layout\n");
    std::vector<float> tr(3 * 100);
    for (size_t i = 0; i < tr.size(); i++) tr[i] = (float)i * 0.5f;
    SegySpec s;
    s.traces = tr.data(); s.nTraces = 3; s.nSamples = 100; s.dtMicros = 1000; s.shotNum = 7;
    s.label = "MAIN STREAMER"; s.modeLabel = "CLO"; s.sourceX = 320; s.sourceDepth = 8;
    s.receiver = [](int i) { return SegyReceiver{100.0 + i * 10, 0, 10}; };
    auto buf = buildSegy(s);
    CHECK(buf.size() == 3600 + 3 * (240 + 400), "file size %zu", buf.size());
    CHECK(buf[0] == 0xC3, "EBCDIC 'C' expected at byte 0 (got %02X)", buf[0]);
    CHECK(be16(&buf[3200 + 24]) == 5, "format code");
    CHECK(be16(&buf[3200 + 20]) == 100, "samples per trace");
    CHECK(be16(&buf[3200 + 300]) == 0x0100, "revision");
    const uint8_t* th = &buf[3600 + 240 + 400];   // trace 2 header
    CHECK(be32(th) == 2, "trace sequence");
    CHECK((int32_t)be32(th + 36) == 110 - 320, "offset %d", (int32_t)be32(th + 36));
    CHECK((int32_t)be32(th + 48) == -8, "source depth");
    uint32_t u = be32(th + 240 + 4);
    float f; std::memcpy(&f, &u, 4);
    CHECK(f == tr[100 + 1], "sample value %g", f);
    auto rs = resampleGather(tr.data(), 0, 1, 100, 100, 199);
    CHECK(std::fabs(rs[1] - 0.25f) < 1e-6f && rs[198] == tr[99], "resampler");
}

static void testJson() {
    std::printf("[json] round trip + HTML-format file\n");
    SimParams p;
    p.fd_order = 6; p.dh = 2.5f; p.geology = GeologyModel::Fault; p.obs_enabled = false; p.sismo_ms = 3500;
    std::string j = paramsToJson(p, 512.0);
    SimParams q;
    double ship = 0;
    std::string err;
    CHECK(paramsFromJson(j, q, ship, err), "parse own output: %s", err.c_str());
    CHECK(q.fd_order == 6 && q.dh == 2.5f && q.geology == GeologyModel::Fault && !q.obs_enabled && q.sismo_ms == 3500 && ship == 512,
          "round trip values");
    // A file exactly as written by the browser version (all values are strings)
    const char* html = R"({ "sl_fdOrder": "8", "sl_dh": "2", "sl_dt": "0.4", "sl_npml": "30", "sl_freq": "35",
        "sl_amp": "2", "sl_cable": "500", "sl_groupSpace": "12.5", "sl_cableHead": "10", "sl_cableTail": "20",
        "sl_srcDepth": "8", "shipXSlider": "600", "sl_srcOff": "350", "sl_shipSrc": "50", "sl_srcRec": "100",
        "sl_shipSpeed": "4.5", "sl_shot": "25", "sl_geoModel": "flat", "sl_uiWidth": "2000", "sl_uiHeight": "800",
        "sl_obsSpace": "100", "sl_gain": "20", "sl_speed": "16.6", "sismoTimeSlider": "2000", "segyLenVal": "3000",
        "obs_enabled": true })";
    SimParams h;
    CHECK(paramsFromJson(html, h, ship, err), "parse HTML file: %s", err.c_str());
    CHECK(h.fd_order == 8 && h.dh == 2.0f && std::fabs(h.dt - 0.4f) < 1e-6f && h.receiver_spacing == 12.5f &&
              h.geology == GeologyModel::Flat && h.domain_W == 2000 && h.sismo_ms == 3000 && ship == 600,
          "HTML values");
    CHECK(!paramsFromJson("{ bad json", h, ship, err), "malformed JSON must fail");
    CHECK(jsNum(0.1f) == "0.1" && jsNum(6.25f) == "6.25" && jsNum(40.0f) == "40", "float formatting: %s", jsNum(0.1f).c_str());
    SimParams n;
    const float dh0 = n.dh;
    CHECK(paramsFromJson(R"({"sl_dh": "nan", "sl_uiWidth": "inf", "sl_freq": 1e999})", n, ship, err) || true, "non-finite parse");
    CHECK(n.dh == dh0 && n.domain_W == 1200.0f && n.freq == 50.0f, "non-finite values must be ignored");
}

static void bench(bool haveGpu) {
    std::printf("[bench] steps per second (O10, full-domain box, no pixel pass)\n");
    struct Case { const char* name; double W, H, dh; };
    const Case cases[] = {{"default 1200x480 m @ 4 m", 1200, 480, 4}, {"medium 6000x2400 m @ 2 m", 6000, 2400, 2},
                          {"large 25000x12000 m @ 4 m", 25000, 12000, 4}};
    for (const Case& c : cases) {
        Setup s = makeSetup(c.W, c.H, c.dh);
        const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
        const int steps = c.W * c.H / (c.dh * c.dh) > 5e6 ? 40 : 400;
        auto st = makeSteps(s, 0, steps, 1e9);
        BatchJob J = makeJob(s, st, 10);
        for (int thr : {1, 2, 3, 4, 5, 6, 7, 8, 10, 12}) {
            if (std::getenv("FDTD_THREAD_SWEEP") == nullptr) break;
            CpuSolver cpu;
            cpu.init(s.W, s.H, thr, sismoSize);
            cpu.setMaterial(materialOf(s));
            cpu.setCpml(s.bx, s.ax, s.bz, s.az);
            cpu.submit(J); cpu.wait();
            auto t0 = std::chrono::steady_clock::now();
            cpu.submit(J); cpu.wait();
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %-28s CPU(%2d thr): %9.1f steps/s\n", c.name, thr, steps / sec);
        }
        {
            CpuSolver cpu;
            int hc = (int)std::thread::hardware_concurrency();
            cpu.init(s.W, s.H, hc, sismoSize);
            cpu.setMaterial(materialOf(s));
            cpu.setCpml(s.bx, s.ax, s.bz, s.az);
            cpu.submit(J); cpu.wait();   // warm-up
            auto t0 = std::chrono::steady_clock::now();
            cpu.submit(J); cpu.wait();
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %-28s %6dx%-5d CPU(%2d thr): %9.1f steps/s  (%.1f Mcells/s)\n", c.name, s.W, s.H, hc,
                        steps / sec, steps / sec * s.W * s.H / 1e6);
        }
        if (haveGpu) {
            GpuSolver gpu;
            std::string err;
            if (!gpu.init(s.W, s.H, s.nRec, s.nObs, err) || !gpu.ensurePrograms(10, err)) {
                std::printf("  %-28s GPU: %s\n", c.name, err.c_str());
                continue;
            }
            gpu.setMaterial(materialOf(s));
            gpu.setCpml(s.bx, s.ax, s.bz, s.az);
            gpu.setObs(s.obsX, s.obsY);
            gpu.submit(J, 0); gpu.wait();
            auto t0 = std::chrono::steady_clock::now();
            gpu.submit(J, 0); gpu.wait();
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %-28s %6dx%-5d GPU          : %9.1f steps/s  (%.1f Mcells/s)\n", c.name, s.W, s.H, steps / sec,
                        steps / sec * s.W * s.H / 1e6);
            gpu.destroy();
        }
    }
}

// --snap DIR: write P snapshots (PGM, gray = 128 + P*gain) of the default scene, CPU and GPU
static void writePgm(const std::string& path, const std::vector<float>& P, int W, int H, float gain) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P5\n%d %d\n255\n", W, H);
    std::vector<unsigned char> row(W);
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            float v = std::max(-1.0f, std::min(1.0f, P[(size_t)j * W + i] * gain));
            row[i] = (unsigned char)std::lround(127.5f + v * 127.5f);
        }
        std::fwrite(row.data(), 1, W, f);
    }
    std::fclose(f);
}

static void snapshots(const std::string& dir, bool haveGpu) {
    Setup s = makeSetup(1200, 480, 4.0);
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
    CpuSolver cpu;
    cpu.init(s.W, s.H, 4, sismoSize);
    cpu.setMaterial(materialOf(s));
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    GpuSolver gpu;
    std::string err;
    const bool g = haveGpu && gpu.init(s.W, s.H, s.nRec, s.nObs, err) && gpu.ensurePrograms(10, err);
    if (g) { gpu.setMaterial(materialOf(s)); gpu.setCpml(s.bx, s.ax, s.bz, s.az); gpu.setObs(s.obsX, s.obsY); }
    int done = 0;
    for (int target : {150, 300, 600, 1200, 2400}) {
        auto st = makeSteps(s, done, target - done, 12000);
        BatchJob J = makeJob(s, st, 10);
        cpu.submit(J); cpu.wait();
        if (g) { gpu.submit(J, 0); gpu.wait(); }
        done = target;
        writePgm(dir + "/cpu_" + std::to_string(target) + ".pgm", cpu.P(), s.W, s.H, 20.0f);
        if (g) { std::vector<float> gp; gpu.readP(gp); writePgm(dir + "/gpu_" + std::to_string(target) + ".pgm", gp, s.W, s.H, 20.0f); }
        float pmax = 0;
        for (float v : cpu.P()) pmax = std::max(pmax, std::fabs(v));
        std::printf("step %d  t=%.1f ms  max|P|=%.4g\n", target, target * s.SCALE / 6000.0 * s.dt * 1000.0, pmax);
    }
    if (g) gpu.destroy();
}

// --simsnap DIR MODE: run the full controller (CPU) with the expanding box on and off and
// dump P at a few simulated times, plus the box-on vs box-off difference.
static void simSnapshots(const std::string& dir, int modeIdx, bool minPhase, bool freeSurface = true, float dh = 4.0f,
                         std::vector<double> times = {100.0, 200.0, 350.0, 600.0}) {
    const Mode m = modeIdx == 0 ? Mode::CLO : modeIdx == 1 ? Mode::SLO : Mode::Trad;
    std::vector<std::vector<float>> runs[2];
    int W = 0, H = 0;
    for (int box = 0; box < 2; box++) {
        Simulation sim;
        sim.p.expanding_box = box == 1;
        if (minPhase) sim.p.wavelet = Wavelet::MinPhase;
        sim.p.free_surface = freeSurface;
        sim.p.dh = dh;
        sim.init(false);
        W = sim.W_sim; H = sim.H_sim;
        sim.startMode(m);
        double now = 0;
        for (double tms : times) {
            while (sim.cycle_t * sim.dtPhy() * 1000.0 < tms) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }
            sim.cpuSolver().wait();
            runs[box].push_back(sim.cpuSolver().P());
            {   // displayed RGBA texture (CPU colormap pass)
                const std::string raw = dir + "/pix_m" + std::to_string(modeIdx) + (minPhase ? "mp" : "") + (freeSurface ? "" : "_fs0") +
                                        (box ? "_box_" : "_full_") + std::to_string((int)tms) + ".rgba";
                FILE* rf = std::fopen(raw.c_str(), "wb");
                if (rf) { std::fwrite(sim.cpuSolver().pixels().data(), 4, sim.cpuSolver().pixels().size(), rf); std::fclose(rf); }
            }
            {   // raw float32 copy for analysis
                const std::string raw = dir + "/sim_m" + std::to_string(modeIdx) + (minPhase ? "mp" : "") + (freeSurface ? "" : "_fs0") +
                                        (box ? "_box_" : "_full_") + std::to_string((int)tms) + ".f32";
                FILE* rf = std::fopen(raw.c_str(), "wb");
                if (rf) { std::fwrite(sim.cpuSolver().P().data(), 4, sim.cpuSolver().P().size(), rf); std::fclose(rf); }
            }
            writePgm(dir + "/sim_m" + std::to_string(modeIdx) + (minPhase ? "mp" : "") + (freeSurface ? "" : "_fs0") + (box ? "_box_" : "_full_") + std::to_string((int)tms) + ".pgm",
                     sim.cpuSolver().P(), W, H, 20.0f);
            std::printf("box=%d t=%.1f ms (target %.0f) ship=%.1f\n", box, sim.cycle_t * sim.dtPhy() * 1000.0, tms, sim.sxShip);
        }
    }
    for (size_t k = 0; k < runs[0].size(); k++) {
        double mx = 0, md = 0;
        for (size_t i = 0; i < runs[0][k].size(); i++) {
            mx = std::max(mx, (double)std::fabs(runs[0][k][i]));
            md = std::max(md, (double)std::fabs(runs[0][k][i] - runs[1][k][i]));
        }
        std::printf("snap %zu: max|P| full=%.4g  max|full-box|=%.4g (rel %.2e)\n", k, mx, md, mx > 0 ? md / mx : 0);
    }
}

// --pml: absorption check in a homogeneous medium (free surface off, CPML on all sides).
// Reports the largest interior |P| after the direct wave has left, relative to its peak.
static void pmlCheck() {
    for (int order : {2, 10}) for (float v : {0.25f, 0.48f, 0.68f}) {
        Setup s = makeSetup(1200, 480, 4.0);
        std::fill(s.vsq.begin(), s.vsq.end(), v * v);
        cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, false, s.bz, s.az);
        const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * kSismoStride;
        CpuSolver cpu;
        cpu.init(s.W, s.H, 4, sismoSize);
        cpu.setMaterial(materialOf(s));
        cpu.setCpml(s.bx, s.ax, s.bz, s.az);
        const int cross = (int)(260 / (v * s.dt));   // steps to travel 260 cells (well past every boundary)
        double peak = 0, late = 0;
        int done = 0;
        while (done < cross * 2) {
            int n = std::min(200, cross * 2 - done);
            auto st = makeSteps(s, done, n, 1e9);
            for (auto& d : st) d.sj0 = (float)(s.H / 2);
            BatchJob J = makeJob(s, st, order);
            J.freeSurface = false;
            cpu.submit(J); cpu.wait();
            done += n;
            double mx = 0;
            for (int j = 40; j < s.H - 40; j++)
                for (int i = 40; i < s.W - 40; i++) mx = std::max(mx, (double)std::fabs(cpu.P()[(size_t)j * s.W + i]));
            if (done <= cross / 4) peak = std::max(peak, mx);
            if (done >= cross) late = std::max(late, mx);
        }
        std::printf("O%-2d v=%.2f  peak=%.4g  residual after exit=%.4g  (%.2f%%)\n", order, v, peak, late, 100 * late / peak);
    }
}

// Radiated pulse: homogeneous water, no free surface, 50 Hz source at the centre, P recorded
// 60 cells to the right. Returns the best normalised correlation with the target pulse
// (any lag, sign kept) and optionally writes "injected target received" per step.
static double radiatedPulse(Wavelet w, const char* dumpPath = nullptr, double* peakHz = nullptr) {
    Setup s = makeSetup(1200, 480, 4.0);
    std::fill(s.vsq.begin(), s.vsq.end(), 0.0625f);
    cpmlZ(s.H, 40, s.dt, s.oceanY, s.SCALE, false, s.bz, s.az);
    CpuSolver cpu;
    cpu.init(s.W, s.H, 4, (size_t)(s.nRec * 2 + s.nObs) * kSismoStride);
    cpu.setMaterial(materialOf(s));
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    const double f = 50, dtp = s.SCALE / 6000.0 * s.dt;
    SourcePulse pulse;
    pulse.build(w, f);
    const std::vector<double> tp = targetPulse(w, 3 * kPulseRate);
    const int sj = s.H / 2, si = s.W / 2, n = 1500;
    std::vector<double> inj(n), tgt(n), rec(n), near(n);
    for (int k = 0; k < n; k++) {
        auto st = makeSteps(s, k, 1, 1e9);
        const double t = (k + 1) * dtp;
        inj[k] = pulse(t);
        tgt[k] = pulseAt(tp, f, t);
        st[0].val0 = (float)(inj[k] * 2.0 * s.dt * 0.4);
        st[0].act0 = st[0].val0 != 0 ? 1.f : 0.f;
        st[0].sj0 = (float)sj;
        BatchJob J = makeJob(s, st, 10);
        J.freeSurface = false;
        cpu.submit(J); cpu.wait();
        rec[k] = cpu.P()[(size_t)sj * s.W + si + 60];
        near[k] = cpu.P()[(size_t)sj * s.W + si + 8];
    }
    if (dumpPath) {
        FILE* fo = std::fopen(dumpPath, "w");
        for (int k = 0; k < n; k++) std::fprintf(fo, "%g %g %g %g\n", inj[k], tgt[k], rec[k], near[k]);
        std::fclose(fo);
    }
    if (peakHz) {   // spectral peak of the received pulse
        double bestA = 0;
        for (double fr = 1; fr <= 200; fr += 0.5) {
            double re = 0, im = 0;
            for (int k = 0; k < n; k++) { re += rec[k] * std::cos(2 * M_PI * fr * k * dtp); im -= rec[k] * std::sin(2 * M_PI * fr * k * dtp); }
            const double a = std::hypot(re, im);
            if (a > bestA) { bestA = a; *peakHz = fr; }
        }
    }
    double nt = 0, nr = 0, best = -2;
    for (int k = 0; k < n; k++) { nt += tgt[k] * tgt[k]; nr += rec[k] * rec[k]; }
    for (int lag = 0; lag < 800; lag++) {
        double c = 0;
        for (int k = 0; k + lag < n; k++) c += tgt[k] * rec[k + lag];
        best = std::max(best, c / std::sqrt(nt * nr));
    }
    return best;
}

static void testRadiatedPulse() {
    std::printf("[radiation] pulse 60 cells from the source matches the selected wavelet\n");
    for (Wavelet w : kAllWavelets) {
        double fPeak = 0;
        const double c = radiatedPulse(w, nullptr, &fPeak);
        const char* name = waveletName(w);
        std::printf("  %-8s correlation with target pulse = %.4f  radiated peak %.1f Hz\n", name, c, fPeak);
        // The Gaussian cannot be radiated with its net area (see SourcePulse): its main lobe matches,
        // followed by a shallow undershoot. The Gauss derivative's spectrum falls off slower than
        // the Ricker's (95 % of the energy below 2 f instead of 1.7 f): a little grid dispersion.
        const double bar = w == Wavelet::Gauss ? 0.90 : w == Wavelet::GaussDeriv ? 0.98 : 0.99;
        CHECK(c > bar, "%s: radiated pulse differs from the wavelet (corr %.4f)", name, c);
        if (w != Wavelet::Gauss) CHECK(std::fabs(fPeak - 50) <= 2.5, "%s: radiated dominant frequency %.1f Hz instead of 50", name, fPeak);
    }
}

// --gpusnap DIR GAIN [minphase]: run the controller on the GPU exactly like the app (Trad mode,
// free surface off, dh = 1 m) and dump the displayed wave texture at 100/150/200 ms.
static void gpuSnapshots(const std::string& dir, float gain, bool minPhase, bool forceCpu = false) {
    Simulation sim;
    sim.p.free_surface = false;
    sim.p.dh = 1.0f;
    sim.p.gain = gain;
    if (minPhase) sim.p.wavelet = Wavelet::MinPhase;
    sim.userForceCPU = forceCpu;
    sim.init(true);
    std::printf("engine: %s\n", sim.usingGpu() ? "GPU" : "CPU");
    sim.startMode(Mode::Trad);
    double now = 0;
    for (double tms : {100.0, 150.0, 200.0}) {
        while (sim.cycle_t * sim.dtPhy() * 1000.0 < tms) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }
        while (sim.eikonalBusy()) { now += 1.0 / 60; sim.update(now, 1.0 / 60); }
        sim.gpuSolver().wait();
        sim.cpuSolver().wait();
        now += 1.0 / 60; sim.p.speed_ms = 0.001f; sim.update(now, 1.0 / 60); sim.p.speed_ms = 16.7f;   // apply the finished batch (texture upload)
        glFinish();
        std::vector<uint8_t> px((size_t)sim.W_sim * sim.H_sim * 4);
        glBindTexture(GL_TEXTURE_2D, sim.waveTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const std::string path = dir + "/" + (forceCpu ? "cpu" : "gpu") + "_tex_" + (minPhase ? "mp_" : "rk_") + std::to_string((int)gain) + "_" + std::to_string((int)tms) + ".rgba";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
        std::printf("t=%.1f ms  %dx%d\n", sim.cycle_t * sim.dtPhy() * 1000.0, sim.W_sim, sim.H_sim);
    }
}

int main(int argc, char** argv) {
    const bool doBench = argc > 1 && std::string(argv[1]) == "--bench";
    if (argc > 1 && std::string(argv[1]) == "--moving") { testMovingSource(); return failures ? 1 : 0; }
    if (argc > 1 && std::string(argv[1]) == "--canvas") { testModelCanvas(); return failures ? 1 : 0; }
    if (argc > 1 && std::string(argv[1]) == "--image") { testImageModel(); return failures ? 1 : 0; }
    if (argc > 1 && std::string(argv[1]) == "--skipgap") { testSkipGap(); return failures ? 1 : 0; }
    bool haveGpu = false;
    GLFWwindow* win = nullptr;
    if (glfwInit()) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "fdtd_tests", nullptr, nullptr);
        if (win) {
            glfwMakeContextCurrent(win);
            haveGpu = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) && GLAD_GL_VERSION_4_3;
            if (haveGpu) {
                std::string r, why; long long mx;
                haveGpu = GpuSolver::queryCapabilities(r, why, mx);
                std::printf("GPU: %s  (max SSBO %lld MB) %s\n", r.c_str(), mx / 1048576, why.c_str());
            }
        }
    }
    if (argc > 3 && std::string(argv[1]) == "--gpusnap") {
        bool mp = false, cpuOnly = false;
        for (int a = 4; a < argc; a++) { if (std::string(argv[a]) == "minphase") mp = true; if (std::string(argv[a]) == "cpu") cpuOnly = true; }
        gpuSnapshots(argv[2], (float)std::atof(argv[3]), mp, cpuOnly);
        if (win) glfwDestroyWindow(win);
        glfwTerminate();
        return 0;
    }
    if (argc > 3 && std::string(argv[1]) == "--simsnap") {
        // optional: minphase|ricker, fs0 (free surface off), dh=<m>, t=<ms,ms,...>
        bool mp = false, fs = true; float dh = 4.0f; std::vector<double> times = {100.0, 200.0, 350.0, 600.0};
        for (int a = 4; a < argc; a++) {
            const std::string x = argv[a];
            if (x == "minphase") mp = true;
            else if (x == "fs0") fs = false;
            else if (x.rfind("dh=", 0) == 0) dh = (float)std::atof(x.c_str() + 3);
            else if (x.rfind("t=", 0) == 0) {
                times.clear();
                for (const char* c = x.c_str() + 2; *c;) { times.push_back(std::atof(c)); while (*c && *c != ',') c++; if (*c) c++; }
            }
        }
        simSnapshots(argv[2], std::atoi(argv[3]), mp, fs, dh, times);
        if (win) glfwDestroyWindow(win);
        glfwTerminate();
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--trace") {
        radiatedPulse(waveletFromArg(argc > 3 ? argv[3] : ""), argv[2]);
        glfwTerminate();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--pml") { pmlCheck(); glfwTerminate(); return 0; }
    if (argc > 2 && std::string(argv[1]) == "--snap") {
        snapshots(argv[2], haveGpu);
        if (win) glfwDestroyWindow(win);
        glfwTerminate();
        return 0;
    }
    testParity(haveGpu);
    testGpuReinit(haveGpu);
    testPixels();
    testInstability(haveGpu);
    testEikonal();
    testWavelets();
    testRadiatedPulse();
    testPipelineMatchesBarrier();
    testSourceWaveletController();
    testReceiverInterpolation();
    testPulseEnd();
    testSkipGap();
    testMovingSource();
    testDensity();
    testModelImport();
    testImageModel();
    testModelCanvas();
    testLineExport();
    testSegy();
    testJson();
    if (doBench) bench(haveGpu);
    if (win) glfwDestroyWindow(win);
    glfwTerminate();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
