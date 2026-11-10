#include "CpuSolver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>
#include "Params.hpp"

namespace {

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Staggered-grid FD coefficients c_k (k = 1..ORD/2)
template <int ORD> struct Coef;
template <> struct Coef<2>  { static constexpr float c[1] = {1.0f}; };
template <> struct Coef<4>  { static constexpr float c[2] = {9.0f / 8.0f, -1.0f / 24.0f}; };
template <> struct Coef<6>  { static constexpr float c[3] = {75.0f / 64.0f, -25.0f / 384.0f, 3.0f / 640.0f}; };
template <> struct Coef<8>  { static constexpr float c[4] = {1225.0f / 1024.0f, -245.0f / 3072.0f, 49.0f / 5120.0f, -5.0f / 7168.0f}; };
template <> struct Coef<10> { static constexpr float c[5] = {19845.0f / 16384.0f, -735.0f / 8192.0f, 567.0f / 40960.0f,
                                                             -405.0f / 229376.0f, 35.0f / 294912.0f}; };

// Seismic colormap LUT: black for negative pressure, white for positive,
// alpha = min(255, |v|*700), cut below 0.001 (initColormap in the HTML).
struct Colormap {
    uint32_t lut[2048];
    Colormap() {
        for (int i = 0; i < 2048; i++) {
            double v = (i / 1023.5) - 1.0;
            double intensity = std::fabs(v);
            double a = std::min(255.0, intensity * 700.0);
            if (intensity < 0.001) a = 0;
            uint32_t ai = (uint32_t)std::nearbyint(std::clamp(a, 0.0, 255.0));   // Uint8ClampedArray rounding
            uint32_t c = v < 0 ? 0u : 255u;
            lut[i] = c | (c << 8) | (c << 16) | (ai << 24);
        }
    }
};
const Colormap& colormap() { static Colormap cm; return cm; }

// 7x7 Gaussian source footprint, radius 3
struct Gauss {
    float w[7][7];
    Gauss() {
        for (int dj = -3; dj <= 3; dj++)
            for (int di = -3; di <= 3; di++) {
                int d = di * di + dj * dj;
                w[dj + 3][di + 3] = d <= 9 ? (float)std::exp(-d / 2.0) : 0.0f;
            }
    }
};
const Gauss& gauss() { static Gauss g; return g; }

inline void rowRange(int lo, int hi, int t, int n, int& r0, int& r1) {
    int count = std::max(0, hi - lo);
    r0 = lo + (int)((long long)count * t / n);
    r1 = lo + (int)((long long)count * (t + 1) / n);
}

// ── Phase 1: velocity update with CPML (rows [j0, j1)) ──────────────────────
template <int ORD>
void velocityRows(int j0, int j1, int bx0, int bx1, int W, int H, float dt,
                  const float* __restrict P, float* __restrict Vx, float* __restrict Vz,
                  float* __restrict psiVx, float* __restrict psiVz,
                  const float* __restrict bx, const float* __restrict ax,
                  const float* __restrict bz, const float* __restrict az) {
    constexpr int H2 = ORD / 2;
    constexpr const float* c = Coef<ORD>::c;
    const int js = std::max(H2 - 1, j0), je = std::min(j1, H - H2);
    const int is = std::max(H2 - 1, bx0), ie = std::min(W - H2 - 1, bx1);
    for (int j = js; j < je; j++) {
        const size_t row = (size_t)j * W;
        const float bzj = bz[j], azj = az[j];
        const float* Pr = P + row;
        float* Vxr = Vx + row; float* Vzr = Vz + row;
        float* pvx = psiVx + row; float* pvz = psiVz + row;
        for (int i = is; i <= ie; i++) {
            const float* p = Pr + i;
            float dPx = p[1] - p[0];
            float dPz = p[W] - p[0];
            float px = bx[i] * pvx[i] + ax[i] * dPx;
            float pz = bzj * pvz[i] + azj * dPz;
            pvx[i] = px; pvz[i] = pz;
            float sDx = c[0] * dPx, sDz = c[0] * dPz;
            for (int k = 1; k < H2; k++) {
                sDx += c[k] * (p[k + 1] - p[-k]);
                sDz += c[k] * (p[(k + 1) * W] - p[-k * W]);
            }
            Vxr[i] -= dt * (sDx + px);
            Vzr[i] -= dt * (sDz + pz);
        }
    }
}

// ── Phase 2: pressure update with CPML (rows [j0, j1)) ──────────────────────
template <int ORD>
void pressureRows(int j0, int j1, int bx0, int bx1, int W, int H, float dt,
                  float* __restrict P, const float* __restrict Vx, const float* __restrict Vz,
                  const float* __restrict Vsq, float* __restrict psiPx, float* __restrict psiPz,
                  const float* __restrict bx, const float* __restrict ax,
                  const float* __restrict bz, const float* __restrict az) {
    constexpr int H2 = ORD / 2;
    constexpr const float* c = Coef<ORD>::c;
    const int js = std::max(H2, j0), je = std::min(j1, H - H2);
    const int is = std::max(H2, bx0), ie = std::min(W - H2 - 1, bx1);
    for (int j = js; j < je; j++) {
        const size_t row = (size_t)j * W;
        const float bzj = bz[j], azj = az[j];
        float* Pr = P + row; const float* vsq = Vsq + row;
        const float* Vxr = Vx + row; const float* Vzr = Vz + row;
        float* ppx = psiPx + row; float* ppz = psiPz + row;
        for (int i = is; i <= ie; i++) {
            const float* vx = Vxr + i;
            const float* vz = Vzr + i;
            float dVx1 = vx[0] - vx[-1];
            float dVz1 = vz[0] - vz[-W];
            float px = bx[i] * ppx[i] + ax[i] * dVx1;
            float pz = bzj * ppz[i] + azj * dVz1;
            ppx[i] = px; ppz[i] = pz;
            float dVx = c[0] * dVx1, dVz = c[0] * dVz1;
            for (int k = 1; k < H2; k++) {
                dVx += c[k] * (vx[k] - vx[-k - 1]);
                dVz += c[k] * (vz[k * W] - vz[-(k + 1) * W]);
            }
            Pr[i] -= dt * vsq[i] * (dVx + px + dVz + pz);
        }
    }
}

} // namespace

CpuSolver::~CpuSolver() { shutdown(); }

void CpuSolver::shutdown() {
    if (threads_.empty()) return;
    wait();
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& th : threads_) th.join();
    threads_.clear();
    quit_ = false;
}

void CpuSolver::init(int W, int H, int nThreads, size_t sismoSize) {
    wait();
    if (nThreads < 1) nThreads = 1;
    if (nThreads != nThreads_ || threads_.empty()) {
        shutdown();
        nThreads_ = nThreads;
        barrier_ = std::make_unique<std::barrier<>>(nThreads_);
        stats_.assign(nThreads_, {});
        for (int t = 0; t < nThreads_; t++) threads_.emplace_back(&CpuSolver::workerMain, this, t);
    }
    W_ = W; H_ = H;
    const size_t N = (size_t)W * H;
    for (auto* v : {&P_, &Vx_, &Vz_, &Vsq_, &psiVx_, &psiVz_, &psiPx_, &psiPz_}) v->assign(N, 0.0f);
    pixels_.assign(N, 0u);
    sismo_.assign(sismoSize, 0.0f);
    bx_.assign(W, 1.0f); ax_.assign(W, 0.0f); bz_.assign(H, 1.0f); az_.assign(H, 0.0f);
    unstable_ = false;
}

void CpuSolver::setVsq(const std::vector<float>& vsq) { wait(); Vsq_ = vsq; }

void CpuSolver::setCpml(const std::vector<float>& bx, const std::vector<float>& ax,
                        const std::vector<float>& bz, const std::vector<float>& az) {
    wait(); bx_ = bx; ax_ = ax; bz_ = bz; az_ = az;
}
void CpuSolver::setCpmlZ(const std::vector<float>& bz, const std::vector<float>& az) { wait(); bz_ = bz; az_ = az; }

void CpuSolver::clearFields() {
    wait();
    std::fill(P_.begin(), P_.end(), 0.0f);
    std::fill(Vx_.begin(), Vx_.end(), 0.0f);
    std::fill(Vz_.begin(), Vz_.end(), 0.0f);
    unstable_ = false;
}
void CpuSolver::clearPsi() {
    wait();
    for (auto* v : {&psiVx_, &psiVz_, &psiPx_, &psiPz_}) std::fill(v->begin(), v->end(), 0.0f);
}
void CpuSolver::clearSismo() { wait(); std::fill(sismo_.begin(), sismo_.end(), 0.0f); }
void CpuSolver::clearPixels() { wait(); std::fill(pixels_.begin(), pixels_.end(), 0u); }

void CpuSolver::wait() {
    while (busy_.load(std::memory_order_acquire)) std::this_thread::yield();
}

void CpuSolver::submit(const BatchJob& job) {
    wait();
    job_ = job;
    steps_.assign(job.stepData, job.stepData + job.steps);
    obsSi_.resize(job.N_OBS); obsSj_.resize(job.N_OBS);
    for (int i = 0; i < job.N_OBS; i++) {
        obsSi_[i] = (int)std::floor(job.obsX[i] / job.SCALE);
        obsSj_[i] = (int)std::floor(job.obsY[i] / job.SCALE);
    }
    if (job.rowMin && job.rowMax) {
        rowMin_.assign(job.rowMin, job.rowMin + H_);
        rowMax_.assign(job.rowMax, job.rowMax + H_);
    } else {
        rowMin_.clear(); rowMax_.clear();
    }
    for (auto& s : stats_) s = {};
    remaining_ = nThreads_;
    busy_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_);
        gen_++;
    }
    cv_.notify_all();
}

void CpuSolver::workerMain(int t) {
    // Flush denormals to zero: decaying wave tails otherwise fall into the denormal
    // range and make every arithmetic op ~100x slower (GPUs flush them already).
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ | DAZ
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
            if (quit_) return;
            seen = gen_;
        }
        switch (job_.fd_order) {
            case 2: runJob<2>(t); break;
            case 4: runJob<4>(t); break;
            case 6: runJob<6>(t); break;
            case 8: runJob<8>(t); break;
            default: runJob<10>(t); break;
        }
        if (job_.renderPixels) pixelPhase(t);
        if (t == 0 && job_.probeRow >= 0 && job_.probeRow < H_) {
            const float* row = P_.data() + (size_t)job_.probeRow * W_;
            for (int i = 0; i < W_; i++) {
                uint32_t bits; std::memcpy(&bits, &row[i], 4);
                if ((bits & 0x7f800000u) == 0x7f800000u || std::fabs(row[i]) > 100000.0f) { unstable_ = true; break; }
            }
        }
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            busy_.store(false, std::memory_order_release);
    }
}

template <int ORD>
void CpuSolver::runJob(int t) {
    const BatchJob& J = job_;
    const int W = W_, H = H_;
    const int y0 = std::max(0, J.bymin), y1 = std::min(H, J.bymax);   // JS: local_endY = min(endY, bymax) (exclusive)
    int r0, r1;
    rowRange(y0, y1, t, nThreads_, r0, r1);
    const int bx0 = std::max(0, J.bxmin), bx1 = J.bxmax;
    const int fs1 = J.freeSurface ? std::min(r1, J.sj_surf + 1) : 0;
    const int fsx0 = std::max(0, J.bxmin), fsx1 = std::min(W - 1, J.bxmax);
    ThreadStat& st = stats_[t];

    for (int s = 0; s < J.steps; s++) {
        auto t0 = Clock::now();
        velocityRows<ORD>(r0, r1, bx0, bx1, W, H, J.dt, P_.data(), Vx_.data(), Vz_.data(),
                          psiVx_.data(), psiVz_.data(), bx_.data(), ax_.data(), bz_.data(), az_.data());
        auto t1 = Clock::now();
        barrier_->arrive_and_wait();
        auto t2 = Clock::now();

        pressureRows<ORD>(r0, r1, bx0, bx1, W, H, J.dt, P_.data(), Vx_.data(), Vz_.data(), Vsq_.data(),
                          psiPx_.data(), psiPz_.data(), bx_.data(), ax_.data(), bz_.data(), az_.data());
        // Dirichlet free surface (P = 0 above and at the sea surface), distributed by rows
        for (int j = r0; j < fs1; j++) {
            float* row = P_.data() + (size_t)j * W;
            std::fill(row + fsx0, row + fsx1 + 1, 0.0f);
        }
        auto t3 = Clock::now();
        barrier_->arrive_and_wait();      // everyone's pressure is final before the source touches P
        auto t4 = Clock::now();
        if (t == 0) serialPhase(s);
        auto t5 = Clock::now();
        barrier_->arrive_and_wait();      // end of tick
        auto t6 = Clock::now();

        st.compute += msSince(t0, t1) + msSince(t2, t3) + msSince(t4, t5);
        st.wait += msSince(t1, t2) + msSince(t3, t4) + msSince(t5, t6);
    }
}

void CpuSolver::serialPhase(int s) {
    const BatchJob& J = job_;
    const StepData& sd = steps_[s];
    const int W = W_, H = H_;
    float* P = P_.data();
    const Gauss& g = gauss();

    // Source injection (main, then secondary)
    auto inject = [&](float si_f, float sj_f, float val) {
        int si = (int)si_f, sj = (int)sj_f;
        for (int dj = -3; dj <= 3; dj++)
            for (int di = -3; di <= 3; di++) {
                if (di * di + dj * dj > 9) continue;
                int sjc = sj + dj, sic = si + di;
                if (sic > 1 && sic < W - 1 && sjc > 1 && sjc < H - 1)
                    P[(size_t)sjc * W + sic] += val * g.w[dj + 3][di + 3];
            }
    };
    if (sd.act0 > 0.5f) inject(sd.si0, sd.sj0, sd.val0);
    if (sd.act1 > 0.5f) inject(sd.si1, sd.sj1, sd.val1);

    // Sismogram recording (max-abs per pixel row)
    if (sd.rec_valid < 0.5f) return;
    const int gy = (int)sd.gy_pix;
    if (gy < 0 || gy >= GH) return;
    float* sis = sismo_.data();
    auto record = [&](int si, int sj, size_t idx) {
        float amp = (si > 0 && si < W && sj > 0 && sj < H) ? P[(size_t)sj * W + si] : 0.0f;
        if (std::fabs(amp) > std::fabs(sis[idx])) sis[idx] = amp;
    };
    const int off = (int)sd.sismo_off;
    const float spacing = J.N_REC > 1 ? J.cable_len / (float)(J.N_REC - 1) : 0.0f;
    for (int i = 0; i < J.N_REC; i++) {
        float rx = sd.cable_head_x - J.cable_len + i * spacing;
        float norm = J.N_REC > 1 ? (float)i / (float)(J.N_REC - 1) : 0.0f;
        float ry = sd.ry_tail + (sd.ry_head - sd.ry_tail) * norm;
        record((int)std::floor(rx / J.SCALE), (int)std::floor(ry / J.SCALE), (size_t)off + (size_t)i * GH + gy);
    }
    const size_t obsBase = (size_t)J.N_REC * 2 * GH;
    for (int i = 0; i < J.N_OBS; i++) record(obsSi_[i], obsSj_[i], obsBase + (size_t)i * GH + gy);
}

void CpuSolver::pixelPhase(int t) {
    const BatchJob& J = job_;
    const int lo = std::min(J.clearMinJ, J.drawMinJ), hi = std::max(J.clearMaxJ, J.drawMaxJ);
    if (hi < lo) return;
    int r0, r1;
    rowRange(lo, hi + 1, t, nThreads_, r0, r1);
    const uint32_t* lut = colormap().lut;
    const int W = W_;
    const float g = J.gainNorm;
    bool bad = false;
    const bool rows = !rowMin_.empty();
    for (int j = r0; j < r1; j++) {
        uint32_t* px = pixels_.data() + (size_t)j * W;
        const float* p = P_.data() + (size_t)j * W;
        if (J.dirtyX1 >= J.dirtyX0 && j >= J.clearMinJ && j <= J.clearMaxJ)
            std::fill(px + J.dirtyX0, px + J.dirtyX1 + 1, 0u);
        if (j < J.drawMinJ || j > J.drawMaxJ) continue;
        int a = 0, b = W - 1;
        if (rows) {
            a = std::max(0, rowMin_[j]);
            b = std::min(W - 1, rowMax_[j]);
        }
        for (int i = a; i <= b; i++) {
            float v = p[i];
            uint32_t bits; std::memcpy(&bits, &v, 4);
            // NaN/Inf (checked on the bit pattern: /fp:fast may fold isnan) or blow-up → Courant detector
            if ((bits & 0x7f800000u) == 0x7f800000u || std::fabs(v) > 100000.0f) { bad = true; continue; }
            float sv = std::clamp(v * g, -1.0f, 1.0f);
            px[i] = lut[(int)((sv + 1.0f) * 1023.5f)];
        }
    }
    if (bad) unstable_ = true;
}
