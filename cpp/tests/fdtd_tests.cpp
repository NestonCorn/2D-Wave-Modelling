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
        gpu.setVsq(s.vsq);
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
        cpu.setVsq(s.vsq);
        cpu.setCpml(s.bx, s.ax, s.bz, s.az);
        GpuSolver gpu;
        std::string err;
        bool ok = gpu.init(s.W, s.H, s.nRec, s.nObs, err) && gpu.ensurePrograms(order, err);
        CHECK(ok, "GPU init order %d: %s", order, err.c_str());
        if (!ok) continue;
        gpu.setVsq(s.vsq);
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
    cpu.setVsq(s.vsq);
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
    cpu.setVsq(s.vsq);
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
    gpu.setVsq(s.vsq);
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
            c->setVsq(s.vsq);
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
            cpu.setVsq(s.vsq);
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
            cpu.setVsq(s.vsq);
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
            gpu.setVsq(s.vsq);
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
    cpu.setVsq(s.vsq);
    cpu.setCpml(s.bx, s.ax, s.bz, s.az);
    GpuSolver gpu;
    std::string err;
    const bool g = haveGpu && gpu.init(s.W, s.H, s.nRec, s.nObs, err) && gpu.ensurePrograms(10, err);
    if (g) { gpu.setVsq(s.vsq); gpu.setCpml(s.bx, s.ax, s.bz, s.az); gpu.setObs(s.obsX, s.obsY); }
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
        cpu.setVsq(s.vsq);
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
    cpu.setVsq(s.vsq);
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
    testSegy();
    testJson();
    if (doBench) bench(haveGpu);
    if (win) glfwDestroyWindow(win);
    glfwTerminate();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
