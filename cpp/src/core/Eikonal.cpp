#include "Eikonal.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

void Eikonal::compute(const std::vector<float>& vsq, int W, int H, double SCALE, double dt,
                      const std::vector<std::pair<int, int>>& srcCells, std::atomic<int>* progress) {
    valid_ = false;
    W_ = W; H_ = H;
    const size_t N = (size_t)W * H;
    std::vector<float> T(N, 1e9f);

    const double h = SCALE / 6000.0;
    for (auto [si, sj] : srcCells)
        if (si > 0 && si < W - 1 && sj > 0 && sj < H - 1) T[(size_t)sj * W + si] = 0.0f;

    const int sweeps = 4;
    for (int s = 0; s < sweeps; s++) {
        if (progress) progress->store((int)std::lround(100.0 * s / sweeps));
        const int i_start = (s % 2 == 0) ? 1 : W - 2;
        const int i_end = (s % 2 == 0) ? W - 1 : 0;
        const int i_step = (s % 2 == 0) ? 1 : -1;
        const int j_start = ((s / 2) % 2 == 0) ? 1 : H - 2;
        const int j_end = ((s / 2) % 2 == 0) ? H - 1 : 0;
        const int j_step = ((s / 2) % 2 == 0) ? 1 : -1;
        if (W < 3 || H < 3) break;

        for (int j = j_start; j != j_end; j += j_step) {
            float* row = T.data() + (size_t)j * W;
            const float* up = row - W;
            const float* dn = row + W;
            const float* vr = vsq.data() + (size_t)j * W;
            for (int i = i_start; i != i_end; i += i_step) {
                if (row[i] == 0.0f) continue;
                double tx = std::min(row[i - 1], row[i + 1]);
                double ty = std::min(up[i], dn[i]);
                double rhs = (1.0 / std::sqrt((double)vr[i])) * h;
                double t_new;
                if (std::fabs(tx - ty) >= rhs) t_new = std::min(tx, ty) + rhs;
                else t_new = 0.5 * (tx + ty + std::sqrt(2.0 * rhs * rhs - (tx - ty) * (tx - ty)));
                if (t_new < row[i]) row[i] = (float)t_new;
            }
        }
    }

    // Arrival bucket per cell (dt_phy steps), as in the HTML lookup builder
    double max_t = 0;
    for (float t : T) if (t < 1e8f && t > max_t) max_t = t;
    bucket_ = (SCALE / 6000.0) * dt;
    numSteps_ = (int)std::ceil(max_t / bucket_) + 5;

    leftMin_.assign(N, INT_MAX);
    rightMin_.assign(N, INT_MAX);
    rowMin_.assign(H, INT_MAX);
    for (int j = 0; j < H; j++) {
        const float* row = T.data() + (size_t)j * W;
        int* L = leftMin_.data() + (size_t)j * W;
        int* R = rightMin_.data() + (size_t)j * W;
        int cur = INT_MAX;
        for (int i = 0; i < W; i++) {
            int b = INT_MAX;
            if (row[i] < 1e8f) {
                b = std::max(0, (int)std::floor(row[i] / bucket_));
                if (b >= numSteps_) b = INT_MAX;
            }
            R[i] = b;                // temporarily the raw bucket
            cur = std::min(cur, b);
            L[i] = cur;              // min bucket over [0, i]  (non-increasing)
        }
        rowMin_[j] = cur;
        cur = INT_MAX;
        for (int i = W - 1; i >= 0; i--) {
            cur = std::min(cur, R[i]);
            R[i] = cur;              // min bucket over [i, W)  (non-decreasing)
        }
    }
    if (progress) progress->store(100);
    valid_ = true;
}

void Eikonal::getBox(double t_sec, int fd_order, bool freeSurface, int sj_surf, EikBox& out) const {
    const int W = W_, H = H_;
    out.xMinZ.assign(H, W);
    out.xMaxZ.assign(H, 0);
    out.hasRows = true;

    double limit = t_sec + bucket_;
    int s = (int)std::floor(limit / bucket_);
    if (s >= numSteps_) s = numSteps_ - 1;
    if (s < 0) s = 0;

    const int pad = (int)std::ceil(fd_order / 2.0) + 1;
    int min_i = W, max_i = 0, min_j = H, max_j = 0;
    std::vector<int> rawMin(H, W), rawMax(H, 0);

    for (int j = 0; j < H; j++) {
        if (rowMin_[j] > s) continue;
        const int* L = leftMin_.data() + (size_t)j * W;
        const int* R = rightMin_.data() + (size_t)j * W;
        // first i with L[i] <= s  (L non-increasing)
        int lo = 0, hi = W - 1;
        while (lo < hi) { int m = (lo + hi) / 2; if (L[m] <= s) hi = m; else lo = m + 1; }
        int xmin = lo;
        // last i with R[i] <= s  (R non-decreasing)
        lo = 0; hi = W - 1;
        while (lo < hi) { int m = (lo + hi + 1) / 2; if (R[m] <= s) lo = m; else hi = m - 1; }
        int xmax = lo;
        rawMin[j] = xmin; rawMax[j] = xmax;
        min_i = std::min(min_i, xmin); max_i = std::max(max_i, xmax);
        min_j = std::min(min_j, j); max_j = std::max(max_j, j);
    }

    if (min_i > max_i) {
        min_i = 0; max_i = W - 1; min_j = 0; max_j = H - 1;
        std::fill(out.xMinZ.begin(), out.xMinZ.end(), 0);
        std::fill(out.xMaxZ.begin(), out.xMaxZ.end(), W - 1);
    } else {
        for (int j = 0; j < H; j++) {
            if (rawMin[j] <= rawMax[j]) {
                out.xMinZ[j] = std::max(0, rawMin[j] - pad);
                out.xMaxZ[j] = std::min(W - 1, rawMax[j] + pad);
            }
        }
        // Fill padded regions to prevent visualizer glitches
        for (int j = max_j + 1; j < H; j++) { out.xMinZ[j] = out.xMinZ[max_j]; out.xMaxZ[j] = out.xMaxZ[max_j]; }
        for (int j = min_j - 1; j >= 0; j--) { out.xMinZ[j] = out.xMinZ[min_j]; out.xMaxZ[j] = out.xMaxZ[min_j]; }
    }

    min_i = std::max(0, min_i - pad);
    max_i = std::min(W - 1, max_i + pad);
    min_j = std::max(0, min_j - pad);
    max_j = std::min(H - 1, max_j + pad);
    if (freeSurface) min_j = std::max(std::max(0, sj_surf - pad), min_j);

    out.min_i = min_i; out.max_i = max_i; out.min_j = min_j; out.max_j = max_j;
}
