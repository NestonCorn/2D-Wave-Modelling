#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Per-step source / receiver data, precomputed on the CPU once per batch and
// consumed by both engines (64 bytes, std430-compatible; mirrors StepData in gpu_engine.js).
// Point-source gain: sum of the 7x7 Gaussian footprint weights the HTML used (amplitude parity)
constexpr float kSourceGain = 6.21336f;

// Receiver interpolation along x: Kaiser-windowed sinc over 2 * kRecvHalf nodes (the GPU
// sismo kernel evaluates the same formula). Measured on a 6.25 m streamer over a 4 m grid,
// 50 Hz Ricker: trace-to-trace jumps of the direct-arrival amplitude 0.2 % (level) / 0.5 %
// (ghosted) - as small as receivers sitting exactly on nodes. 6-point Lagrange left 2.3 % /
// 4.2 %: at 4 m the upper half of the wavelet's band has only ~3 nodes per wavelength.
constexpr int kRecvHalf = 6;
constexpr double kRecvBeta = 6.0;
inline double besselI0(double x) {
    double s = 1, t = 1;
    for (int n = 1; n < 25; n++) { t *= (x * 0.5) / n; s += t * t; }
    return s;
}
// Unnormalised weight of a node at distance x (in cells) from the receiver, |x| < kRecvHalf
inline double recvKernel(double x) {
    const double u = x / kRecvHalf;
    if (std::fabs(u) >= 1.0) return 0.0;
    const double sx = std::fabs(x) < 1e-9 ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
    return sx * besselI0(kRecvBeta * std::sqrt(1.0 - u * u)) / besselI0(kRecvBeta);
}
// recvKernel tabulated at kRecvRes points per cell over [-kRecvHalf, kRecvHalf]; both engines
// interpolate it linearly: weight of the node at distance x = table at (x + kRecvHalf) * kRecvRes
constexpr int kRecvRes = 512;
inline const std::vector<float>& recvKernelTable() {
    static const std::vector<float> tb = [] {
        std::vector<float> t((size_t)2 * kRecvHalf * kRecvRes + 1);
        for (size_t q = 0; q < t.size(); q++) t[q] = (float)recvKernel((double)q / kRecvRes - kRecvHalf);
        return t;
    }();
    return tb;
}

// Weights of the 2 * kRecvHalf nodes along one axis for a point at grid coordinate f: nodes
// i0 - kRecvHalf + 1 .. i0 + kRecvHalf (i0 = floor(f)), normalised to sum 1. On a node
// (within 1e-4 cell) exactly that node: the sinc's zeros are not exact in floating point.
inline void axisWeights(float f, int& i0, float* c) {
    const float x0 = std::floor(f);
    i0 = (int)x0;
    const float t = f - x0;
    constexpr int NT = 2 * kRecvHalf;
    if (t < 1e-4f || t > 1.0f - 1e-4f) {
        for (int k = 0; k < NT; k++) c[k] = 0.0f;
        if (t > 0.5f) i0++;   // the next node up
        c[kRecvHalf - 1] = 1.0f;
        return;
    }
    const std::vector<float>& table = recvKernelTable();
    float sum = 0.0f;
    for (int k = 0; k < NT; k++) {
        const float x = (t - (float)(k - kRecvHalf + 1) + kRecvHalf) * kRecvRes;
        const int q = std::min((int)x, NT * kRecvRes - 1);
        c[k] = table[q] + (x - q) * (table[q + 1] - table[q]);
        sum += c[k];
    }
    for (int k = 0; k < NT; k++) c[k] /= sum;
}
// A point source at (si, sj) (grid coordinates, between nodes) spread over the nodes around
// it: weight of node (i0 - kRecvHalf + 1 + a, j0 - kRecvHalf + 1 + b) = cx[a] * cz[b]
inline void sourceWeights(float si, float sj, int& i0, int& j0, float* cx, float* cz) {
    axisWeights(si, i0, cx);
    axisWeights(sj, j0, cz);
}

struct StepData {
    float si0, sj0, val0, act0;     // main shot position (grid coordinates, between nodes), wavelet * dt * 0.4, active flag
    float si1, sj1, val1, act1;     // secondary shot (SLO src / CLO second ship)
    float sxShip;                   // ship x (m) after this step's movement
    float gy_pix;                   // sismogram sample for this step
    float sismo_off;                // 0 or N_REC*kSismoStride (CLO second shot block)
    float rec_valid;                // 1 = record this step
    float ry_head, ry_tail;         // cable head / tail depth (absolute, m)
    float cable_head_x;             // x of the cable head (m)
    float rec_w;                    // 1 / (steps that fall in row gy_pix): a row stores the mean of its steps
};
static_assert(sizeof(StepData) == 64, "StepData must be 64 bytes");

struct BatchJob {
    int steps = 0;
    float dt = 0.5f;
    int fd_order = 10;
    bool freeSurface = true;
    int sj_surf = 0;
    // Expanding box (inclusive bounds)
    int bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
    const StepData* stepData = nullptr;
    // Row of P checked for NaN/Inf/blow-up after the batch (-1 = none). Works in
    // turbo mode and after the recording window, unlike the pixel/sismo checks.
    int probeRow = -1;

    // Receivers
    int N_REC = 0, N_OBS = 0;
    float cable_len = 0, SCALE = 1;
    const float* obsX = nullptr;    // N_OBS receiver x (m)
    const float* obsY = nullptr;    // N_OBS receiver depth (m, already +4)

    // Wavefield colouring (P -> RGBA), done inside the batch
    bool renderPixels = false;
    float gainNorm = 1.0f;          // display_gain / 15
    int drawMinJ = 0, drawMaxJ = -1;      // rows to colour (inclusive)
    int clearMinJ = 0, clearMaxJ = -1;    // rows of the dirty rectangle (zeroed first, inclusive)
    int dirtyX0 = 0, dirtyX1 = -1;        // dirty rectangle columns (inclusive)
    const int* rowMin = nullptr;    // optional per-row column limits (xMinZ/xMaxZ)
    const int* rowMax = nullptr;
};

struct ThreadStat { double compute = 0, wait = 0; };
