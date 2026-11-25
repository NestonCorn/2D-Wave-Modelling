#pragma once
#include <cmath>
#include <vector>

// Velocities are stored normalised: v_n = v / kVelocityNorm (V_sq = v_n^2, water 1500 m/s
// = 0.25). Densities are in g/cm^3.
constexpr double kVelocityNorm = 6000.0;

// Density from P velocity when no density model is given: Gardner et al. (1974),
// rho = 0.31 V^0.25 (V in m/s, rho in g/cm^3), and sea water (V <= 1600 m/s) 1.03.
inline float gardnerDensity(double v_ms) {
    return v_ms <= 1600.0 ? 1.03f : (float)(0.31 * std::pow(v_ms, 0.25));
}

inline void densityFromVsq(const std::vector<float>& vsq, std::vector<float>& rho) {
    rho.resize(vsq.size());
    for (size_t i = 0; i < vsq.size(); i++) rho[i] = gardnerDensity(std::sqrt((double)vsq[i]) * kVelocityNorm);
}

// What the solvers need for variable-density acoustics on the staggered grid:
//   dVx/dt = -buoyX * dP/dx,  dVz/dt = -buoyZ * dP/dz,  dP/dt = -K * div V
// K = rho v^2 at the pressure nodes; the buoyancy (1/rho) at the velocity nodes, half a cell
// to the right (Vx) / below (Vz) of the pressure node with the same index, from the mean of
// the two densities around it. With a uniform density the scheme is the constant-density
// one: P is unchanged, only V scales by 1/rho.
struct Material {
    std::vector<float> K, buoyX, buoyZ;
};

inline Material makeMaterial(const std::vector<float>& vsq, const std::vector<float>& rho, int W, int H) {
    Material m;
    const size_t N = (size_t)W * H;
    m.K.resize(N); m.buoyX.resize(N); m.buoyZ.resize(N);
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            const size_t idx = (size_t)j * W + i;
            const float r = rho[idx];
            m.K[idx] = r * vsq[idx];
            m.buoyX[idx] = i + 1 < W ? 2.0f / (r + rho[idx + 1]) : 1.0f / r;
            m.buoyZ[idx] = j + 1 < H ? 2.0f / (r + rho[idx + W]) : 1.0f / r;
        }
    }
    return m;
}
