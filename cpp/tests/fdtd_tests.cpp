// Headless checks + benchmark for the FDTD core.
//   fdtd_tests            → run all tests (exit code 0 = pass)
//   fdtd_tests --bench    → also run the steps/s benchmark
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

static void testParity(bool haveGpu) {
    std::printf("[parity] CPU vs GPU, 1200x480 m @ 4 m, 500 steps per FD order\n");
    if (!haveGpu) { std::printf("  SKIP: no OpenGL 4.3 context\n"); return; }
    Setup s = makeSetup(1200, 480, 4.0);
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * 550;
    for (int order : {2, 4, 6, 8, 10}) {
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
        for (int b = 0; b < 5; b++) {
            auto st = makeSteps(s, b * 100, 100, 3000);
            BatchJob J = makeJob(s, st, order);
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
        std::printf("  O%-2d  max|P|=%.4g  P rel diff=%.2e  sismo max=%.4g rel diff=%.2e\n", order, maxAbs, rel, sMax, srel);
        CHECK(maxAbs > 1e-6 && std::isfinite(maxAbs), "wavefield empty or non-finite (O%d)", order);
        CHECK(rel < 1e-4, "P mismatch O%d (%.2e)", order, rel);
        CHECK(srel < 1e-4, "sismo mismatch O%d (%.2e)", order, srel);
        gpu.destroy();
    }
}

static void testPixels() {
    std::printf("[pixels] CPU colormap pass produces visible wavefield\n");
    Setup s = makeSetup(1200, 480, 4.0);
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * 550;
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
    const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * 550;
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
    auto rs = resampleGather(tr.data(), 0, 1, 100, 199);
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
        const size_t sismoSize = (size_t)(s.nRec * 2 + s.nObs) * 550;
        const int steps = c.W * c.H / (c.dh * c.dh) > 5e6 ? 40 : 400;
        auto st = makeSteps(s, 0, steps, 1e9);
        BatchJob J = makeJob(s, st, 10);
        for (int thr : {1, 2, 4, 8}) {
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
    testParity(haveGpu);
    testPixels();
    testInstability(haveGpu);
    testEikonal();
    testSegy();
    testJson();
    if (doBench) bench(haveGpu);
    if (win) glfwDestroyWindow(win);
    glfwTerminate();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
