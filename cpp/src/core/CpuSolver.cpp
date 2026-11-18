#include "CpuSolver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
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


inline void rowRange(int lo, int hi, int t, int n, int& r0, int& r1) {
    int count = std::max(0, hi - lo);
    r0 = lo + (int)((long long)count * t / n);
    r1 = lo + (int)((long long)count * (t + 1) / n);
}

#if defined(_MSC_VER)
#define FDTD_INLINE __forceinline
#else
#define FDTD_INLINE inline __attribute__((always_inline))
#endif

// The stencil loops are written with AVX2/FMA intrinsics, 8 cells per iteration: MSVC
// would not auto-vectorise them (it cannot prove the field arrays do not alias), so the
// CPU used to advance one cell at a time. PX / PZ select whether the CPML memory (psi) is
// read and written: outside the absorbing layers a = 0 and b = 1, psi stays 0, and skipping
// its 4 loads + 4 stores per cell roughly halves the memory traffic there.

// ── Phase 1: velocity update on one row segment [i0, i1) (pointers at the row start) ──
template <int ORD, bool PX, bool PZ>
FDTD_INLINE void velocitySeg(int i0, int i1, int W, float dt, const float* __restrict P, float* __restrict Vx,
                             float* __restrict Vz, float* __restrict pvx, float* __restrict pvz,
                             const float* __restrict bx, const float* __restrict ax, float bzj, float azj) {
    constexpr int H2 = ORD / 2;
    constexpr const float* c = Coef<ORD>::c;
    int i = i0;
    const __m256 vdt = _mm256_set1_ps(dt), vbz = _mm256_set1_ps(bzj), vaz = _mm256_set1_ps(azj);
    for (; i + 8 <= i1; i += 8) {
        const float* p = P + i;
        const __m256 p0 = _mm256_loadu_ps(p);
        const __m256 dPx = _mm256_sub_ps(_mm256_loadu_ps(p + 1), p0);
        const __m256 dPz = _mm256_sub_ps(_mm256_loadu_ps(p + W), p0);
        __m256 sDx = _mm256_mul_ps(_mm256_set1_ps(c[0]), dPx);
        __m256 sDz = _mm256_mul_ps(_mm256_set1_ps(c[0]), dPz);
        for (int k = 1; k < H2; k++) {
            const __m256 ck = _mm256_set1_ps(c[k]);
            sDx = _mm256_fmadd_ps(ck, _mm256_sub_ps(_mm256_loadu_ps(p + k + 1), _mm256_loadu_ps(p - k)), sDx);
            sDz = _mm256_fmadd_ps(ck, _mm256_sub_ps(_mm256_loadu_ps(p + (k + 1) * W), _mm256_loadu_ps(p - k * W)), sDz);
        }
        if constexpr (PX) {
            const __m256 px = _mm256_fmadd_ps(_mm256_loadu_ps(bx + i), _mm256_loadu_ps(pvx + i),
                                              _mm256_mul_ps(_mm256_loadu_ps(ax + i), dPx));
            _mm256_storeu_ps(pvx + i, px);
            sDx = _mm256_add_ps(sDx, px);
        }
        if constexpr (PZ) {
            const __m256 pz = _mm256_fmadd_ps(vbz, _mm256_loadu_ps(pvz + i), _mm256_mul_ps(vaz, dPz));
            _mm256_storeu_ps(pvz + i, pz);
            sDz = _mm256_add_ps(sDz, pz);
        }
        _mm256_storeu_ps(Vx + i, _mm256_fnmadd_ps(vdt, sDx, _mm256_loadu_ps(Vx + i)));
        _mm256_storeu_ps(Vz + i, _mm256_fnmadd_ps(vdt, sDz, _mm256_loadu_ps(Vz + i)));
    }
    for (; i < i1; i++) {   // tail
        const float* p = P + i;
        const float dPx = p[1] - p[0], dPz = p[W] - p[0];
        float sDx = c[0] * dPx, sDz = c[0] * dPz;
        for (int k = 1; k < H2; k++) {
            sDx += c[k] * (p[k + 1] - p[-k]);
            sDz += c[k] * (p[(k + 1) * W] - p[-k * W]);
        }
        if constexpr (PX) { const float px = bx[i] * pvx[i] + ax[i] * dPx; pvx[i] = px; sDx += px; }
        if constexpr (PZ) { const float pz = bzj * pvz[i] + azj * dPz; pvz[i] = pz; sDz += pz; }
        Vx[i] -= dt * sDx;
        Vz[i] -= dt * sDz;
    }
}

// ── Phase 2: pressure update on one row segment ──
template <int ORD, bool PX, bool PZ>
FDTD_INLINE void pressureSeg(int i0, int i1, int W, float dt, float* __restrict P, const float* __restrict Vx,
                             const float* __restrict Vz, const float* __restrict vsq, float* __restrict ppx,
                             float* __restrict ppz, const float* __restrict bx, const float* __restrict ax, float bzj,
                             float azj) {
    constexpr int H2 = ORD / 2;
    constexpr const float* c = Coef<ORD>::c;
    int i = i0;
    const __m256 vdt = _mm256_set1_ps(dt), vbz = _mm256_set1_ps(bzj), vaz = _mm256_set1_ps(azj);
    for (; i + 8 <= i1; i += 8) {
        const float* vx = Vx + i;
        const float* vz = Vz + i;
        const __m256 dVx1 = _mm256_sub_ps(_mm256_loadu_ps(vx), _mm256_loadu_ps(vx - 1));
        const __m256 dVz1 = _mm256_sub_ps(_mm256_loadu_ps(vz), _mm256_loadu_ps(vz - W));
        __m256 dVx = _mm256_mul_ps(_mm256_set1_ps(c[0]), dVx1);
        __m256 dVz = _mm256_mul_ps(_mm256_set1_ps(c[0]), dVz1);
        for (int k = 1; k < H2; k++) {
            const __m256 ck = _mm256_set1_ps(c[k]);
            dVx = _mm256_fmadd_ps(ck, _mm256_sub_ps(_mm256_loadu_ps(vx + k), _mm256_loadu_ps(vx - k - 1)), dVx);
            dVz = _mm256_fmadd_ps(ck, _mm256_sub_ps(_mm256_loadu_ps(vz + k * W), _mm256_loadu_ps(vz - (k + 1) * W)), dVz);
        }
        __m256 sum = _mm256_add_ps(dVx, dVz);
        if constexpr (PX) {
            const __m256 px = _mm256_fmadd_ps(_mm256_loadu_ps(bx + i), _mm256_loadu_ps(ppx + i),
                                              _mm256_mul_ps(_mm256_loadu_ps(ax + i), dVx1));
            _mm256_storeu_ps(ppx + i, px);
            sum = _mm256_add_ps(sum, px);
        }
        if constexpr (PZ) {
            const __m256 pz = _mm256_fmadd_ps(vbz, _mm256_loadu_ps(ppz + i), _mm256_mul_ps(vaz, dVz1));
            _mm256_storeu_ps(ppz + i, pz);
            sum = _mm256_add_ps(sum, pz);
        }
        const __m256 k2 = _mm256_mul_ps(vdt, _mm256_loadu_ps(vsq + i));
        _mm256_storeu_ps(P + i, _mm256_fnmadd_ps(k2, sum, _mm256_loadu_ps(P + i)));
    }
    for (; i < i1; i++) {   // tail
        const float* vx = Vx + i;
        const float* vz = Vz + i;
        const float dVx1 = vx[0] - vx[-1], dVz1 = vz[0] - vz[-W];
        float dVx = c[0] * dVx1, dVz = c[0] * dVz1;
        for (int k = 1; k < H2; k++) {
            dVx += c[k] * (vx[k] - vx[-k - 1]);
            dVz += c[k] * (vz[k * W] - vz[-(k + 1) * W]);
        }
        float sum = dVx + dVz;
        if constexpr (PX) { const float px = bx[i] * ppx[i] + ax[i] * dVx1; ppx[i] = px; sum += px; }
        if constexpr (PZ) { const float pz = bzj * ppz[i] + azj * dVz1; ppz[i] = pz; sum += pz; }
        P[i] -= dt * vsq[i] * sum;
    }
}

// Split [is, ie] into left CPML / interior (ax == 0 on [xi0, xi1]) / right CPML segments
template <typename F>
FDTD_INLINE void forSegments(int is, int ie, int xi0, int xi1, F&& seg) {
    if (ie < is) return;
    if (xi0 > xi1) { seg(is, ie + 1, true); return; }   // no interior column at all
    const int a = std::min(xi0, ie + 1), b0 = std::max(is, xi0), b1 = std::min(xi1, ie) + 1, c0 = std::max(is, xi1 + 1);
    if (a > is) seg(is, a, true);
    if (b1 > b0) seg(b0, b1, false);
    if (ie + 1 > c0) seg(c0, ie + 1, true);
}

// ── Phase 1: velocity update with CPML (rows [j0, j1)) ──────────────────────
template <int ORD>
void velocityRows(int j0, int j1, int bx0, int bx1, int W, int H, float dt, int xi0, int xi1,
                  const float* P, float* Vx, float* Vz, float* psiVx, float* psiVz,
                  const float* bx, const float* ax, const float* bz, const float* az) {
    constexpr int H2 = ORD / 2;
    const int js = std::max(H2 - 1, j0), je = std::min(j1, H - H2);
    const int is = std::max(H2 - 1, bx0), ie = std::min(W - H2 - 1, bx1);
    for (int j = js; j < je; j++) {
        const size_t row = (size_t)j * W;
        const float bzj = bz[j], azj = az[j];
        const bool pz = azj != 0.0f;
        const float* Pr = P + row;
        float *vx = Vx + row, *vz = Vz + row, *pvx = psiVx + row, *pvz = psiVz + row;
        forSegments(is, ie, xi0, xi1, [&](int i0, int i1, bool px) {
            if (px) {
                if (pz) velocitySeg<ORD, true, true>(i0, i1, W, dt, Pr, vx, vz, pvx, pvz, bx, ax, bzj, azj);
                else    velocitySeg<ORD, true, false>(i0, i1, W, dt, Pr, vx, vz, pvx, pvz, bx, ax, bzj, azj);
            } else {
                if (pz) velocitySeg<ORD, false, true>(i0, i1, W, dt, Pr, vx, vz, pvx, pvz, bx, ax, bzj, azj);
                else    velocitySeg<ORD, false, false>(i0, i1, W, dt, Pr, vx, vz, pvx, pvz, bx, ax, bzj, azj);
            }
        });
    }
}

// ── Phase 2: pressure update with CPML (rows [j0, j1)) ──────────────────────
template <int ORD>
void pressureRows(int j0, int j1, int bx0, int bx1, int W, int H, float dt, int xi0, int xi1,
                  float* P, const float* Vx, const float* Vz, const float* Vsq, float* psiPx, float* psiPz,
                  const float* bx, const float* ax, const float* bz, const float* az) {
    constexpr int H2 = ORD / 2;
    const int js = std::max(H2, j0), je = std::min(j1, H - H2);
    const int is = std::max(H2, bx0), ie = std::min(W - H2 - 1, bx1);
    for (int j = js; j < je; j++) {
        const size_t row = (size_t)j * W;
        const float bzj = bz[j], azj = az[j];
        const bool pz = azj != 0.0f;
        float* Pr = P + row;
        const float *vx = Vx + row, *vz = Vz + row, *vs = Vsq + row;
        float *ppx = psiPx + row, *ppz = psiPz + row;
        forSegments(is, ie, xi0, xi1, [&](int i0, int i1, bool px) {
            if (px) {
                if (pz) pressureSeg<ORD, true, true>(i0, i1, W, dt, Pr, vx, vz, vs, ppx, ppz, bx, ax, bzj, azj);
                else    pressureSeg<ORD, true, false>(i0, i1, W, dt, Pr, vx, vz, vs, ppx, ppz, bx, ax, bzj, azj);
            } else {
                if (pz) pressureSeg<ORD, false, true>(i0, i1, W, dt, Pr, vx, vz, vs, ppx, ppz, bx, ax, bzj, azj);
                else    pressureSeg<ORD, false, false>(i0, i1, W, dt, Pr, vx, vz, vs, ppx, ppz, bx, ax, bzj, azj);
            }
        });
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
        prog_ = std::make_unique<std::atomic<int64_t>[]>(nThreads_);
        for (int t = 0; t < nThreads_; t++) threads_.emplace_back(&CpuSolver::workerMain, this, t);
    }
    W_ = W; H_ = H;
    const size_t N = (size_t)W * H;
    for (auto* v : {&P_, &Vx_, &Vz_, &Vsq_, &psiVx_, &psiVz_, &psiPx_, &psiPz_}) v->assign(N, 0.0f);
    pixels_.assign(N, 0u);
    sismo_.assign(sismoSize, 0.0f);
    bx_.assign(W, 1.0f); ax_.assign(W, 0.0f); bz_.assign(H, 1.0f); az_.assign(H, 0.0f);
    xi0_ = 0; xi1_ = W - 1;
    unstable_ = false;
}

void CpuSolver::setVsq(const std::vector<float>& vsq) { wait(); Vsq_ = vsq; }

void CpuSolver::setCpml(const std::vector<float>& bx, const std::vector<float>& ax,
                        const std::vector<float>& bz, const std::vector<float>& az) {
    wait(); bx_ = bx; ax_ = ax; bz_ = bz; az_ = az;
    // Interior columns (no x-CPML): psi_x is neither read nor written there
    xi0_ = W_; xi1_ = -1;
    for (int i = 0; i < (int)ax_.size(); i++)
        if (ax_[i] == 0.0f) { xi0_ = std::min(xi0_, i); xi1_ = i; }
    for (int i = xi0_; i <= xi1_; i++)
        if (ax_[i] != 0.0f) { xi0_ = W_; xi1_ = -1; break; }   // not one contiguous block: keep psi everywhere
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
    pipelined_ = usePipeline_;
    if (const char* e = std::getenv("FDTD_PIPELINE")) pipelined_ = e[0] == '1';
    for (int i = 0; i < nThreads_; i++) prog_[i].store(-1, std::memory_order_relaxed);
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
        if (pipelined_) {
            switch (job_.fd_order) {
                case 2: runPipelined<2>(t); break;
                case 4: runPipelined<4>(t); break;
                case 6: runPipelined<6>(t); break;
                case 8: runPipelined<8>(t); break;
                default: runPipelined<10>(t); break;
            }
        } else {
            switch (job_.fd_order) {
                case 2: runJob<2>(t); break;
                case 4: runJob<4>(t); break;
                case 6: runJob<6>(t); break;
                case 8: runJob<8>(t); break;
                default: runJob<10>(t); break;
            }
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
        velocityRows<ORD>(r0, r1, bx0, bx1, W, H, J.dt, xi0_, xi1_, P_.data(), Vx_.data(), Vz_.data(),
                          psiVx_.data(), psiVz_.data(), bx_.data(), ax_.data(), bz_.data(), az_.data());
        auto t1 = Clock::now();
        barrier_->arrive_and_wait();
        auto t2 = Clock::now();

        pressureRows<ORD>(r0, r1, bx0, bx1, W, H, J.dt, xi0_, xi1_, P_.data(), Vx_.data(), Vz_.data(), Vsq_.data(),
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

// ── Temporal blocking: pipelined wavefront ───────────────────────────────────
// Thread t advances steps t, t+T, t+2T, ... over all rows, in bands of kPipeRows rows,
// trailing the thread that advances the previous step. T consecutive steps are in flight
// at once and follow each other a few bands apart, so a band of rows is reused from
// cache T times before it goes back to memory (the barrier scheme streamed the whole
// grid from RAM every step), and the three global barriers per step disappear.
//
// Dependencies (H2 = stencil half-width):
//   velocity of step s on rows [q0, q1) reads P of step s-1 on rows < q1 + H2 and
//   overwrites the V that step s-1's pressure still needs on rows >= its front - H2,
//   so it waits until step s-1's pressure front (with injection / recording) >= q1 + H2;
//   pressure of step s on rows < q1 - H2 + 1 needs this step's V only up to row q1 - 1.
// Progress is published as step * (H + 1) + front (monotonic per thread).
template <int ORD>
void CpuSolver::runPipelined(int t) {
    const BatchJob& J = job_;
    const int W = W_, H = H_, T = nThreads_;
    constexpr int H2 = ORD / 2;
    const int y0 = std::max(0, J.bymin), y1 = std::min(H, J.bymax);
    const int bx0 = std::max(0, J.bxmin), bx1 = J.bxmax;
    const int fsEnd = J.freeSurface ? J.sj_surf + 1 : 0;
    const int fsx0 = std::max(0, J.bxmin), fsx1 = std::min(W - 1, J.bxmax);
    const int64_t stride = (int64_t)H + 1;
    ThreadStat& st = stats_[t];

    auto publish = [&](int s, int front) { prog_[t].store((int64_t)s * stride + front, std::memory_order_release); };
    auto waitFor = [&](int s, int rows) {   // step s-1 final on rows < rows
        if (s == 0) return;
        const int64_t need = (int64_t)(s - 1) * stride + std::min(rows, H);
        std::atomic<int64_t>& p = prog_[(s - 1) % T];
        if (p.load(std::memory_order_acquire) >= need) return;
        const auto w0 = Clock::now();
        int spins = 0;
        while (p.load(std::memory_order_acquire) < need) {
            _mm_pause();
            if (++spins > 4000) { std::this_thread::yield(); spins = 0; }
        }
        st.wait += msSince(w0, Clock::now());
    };

    for (int s = t; s < J.steps; s += T) {
        const auto c0 = Clock::now();
        const double waited0 = st.wait;
        serialRows(s, 0, y0);   // rows above the band are not updated: final already
        int front = y0;
        publish(s, front);
        for (int q0 = y0; q0 < y1; q0 += kPipeRows) {
            const int q1 = std::min(y1, q0 + kPipeRows);
            waitFor(s, q1 >= y1 ? H : q1 + H2);
            velocityRows<ORD>(q0, q1, bx0, bx1, W, H, J.dt, xi0_, xi1_, P_.data(), Vx_.data(), Vz_.data(),
                              psiVx_.data(), psiVz_.data(), bx_.data(), ax_.data(), bz_.data(), az_.data());
            const int pEnd = q1 >= y1 ? y1 : std::max(front, q1 - H2 + 1);
            if (pEnd > front) {
                pressureRows<ORD>(front, pEnd, bx0, bx1, W, H, J.dt, xi0_, xi1_, P_.data(), Vx_.data(), Vz_.data(),
                                  Vsq_.data(), psiPx_.data(), psiPz_.data(), bx_.data(), ax_.data(), bz_.data(), az_.data());
                for (int j = front; j < std::min(pEnd, fsEnd); j++) {   // Dirichlet free surface
                    float* row = P_.data() + (size_t)j * W;
                    std::fill(row + fsx0, row + fsx1 + 1, 0.0f);
                }
                serialRows(s, front, pEnd);
                front = pEnd;
                publish(s, front);
            }
        }
        serialRows(s, y1, H);
        publish(s, H);
        st.compute += msSince(c0, Clock::now()) - (st.wait - waited0);
    }
    barrier_->arrive_and_wait();   // every step done before the pixel / probe passes
}

void CpuSolver::serialPhase(int s) { serialRows(s, 0, H_); }

// Source injection + sismogram recording of step s for the sources / receivers whose grid
// row lies in [ra, rb) (rows outside the grid count as row 0 / H - 1). The pipelined
// scheme calls it as its pressure front passes each band of rows.
void CpuSolver::serialRows(int s, int ra, int rb) {
    const BatchJob& J = job_;
    const StepData& sd = steps_[s];
    const int W = W_, H = H_;
    float* P = P_.data();
    auto inRows = [&](int sj) { const int r = std::clamp(sj, 0, H - 1); return r >= ra && r < rb; };

    // Source injection (main, then secondary) at a single cell. The HTML spread it over a
    // 7x7 Gaussian (sigma = 1 cell), which low-passes the radiated wave by exp(-(k sigma)^2/2):
    // a 50 Hz Ricker left the source at ~43 Hz, more at coarse grids. kSourceGain keeps the
    // injected total (the sum of the old weights) so amplitudes stay comparable.
    // Rows at/above the free surface are held at P = 0 and never receive source energy.
    const int sjMinSrc = J.freeSurface ? J.sj_surf : -1;
    auto inject = [&](float si_f, float sj_f, float val) {
        const int si = (int)si_f, sj = (int)sj_f;
        if (!inRows(sj)) return;
        if (si > 1 && si < W - 1 && sj > 1 && sj < H - 1 && sj > sjMinSrc) P[(size_t)sj * W + si] += val * kSourceGain;
    };
    if (sd.act0 > 0.5f) inject(sd.si0, sd.sj0, sd.val0);
    if (sd.act1 > 0.5f) inject(sd.si1, sd.sj1, sd.val1);

    // Sismogram recording: each pixel row holds the mean of the ~T/GH steps that fall in it.
    // (The HTML kept the max-abs sample, which aliased the wavelet into a jagged, sign-flipping
    // pattern; the mean is a box low-pass and gives a smooth trace.)
    if (sd.rec_valid < 0.5f) return;
    const int gy = (int)sd.gy_pix;
    if (gy < 0 || gy >= GH) return;
    float* sis = sismo_.data();
    const float w = sd.rec_w > 0.0f ? sd.rec_w : 1.0f;
    auto record = [&](int si, int sj, size_t idx) {
        if (!inRows(sj)) return;
        float amp = (si > 0 && si < W && sj > 0 && sj < H) ? P[(size_t)sj * W + si] : 0.0f;
        sis[idx] += amp * w;
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
