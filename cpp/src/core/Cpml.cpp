#include "Cpml.hpp"
#include <cmath>

static const double V_N = 0.68;

void cpmlX(int W_sim, int npml, double dt, std::vector<float>& b, std::vector<float>& a) {
    b.assign(W_sim, 1.0f);
    a.assign(W_sim, 0.0f);
    const double sigma_max = -3.0 * V_N * std::log(0.001) / (2.0 * npml);
    for (int i = 0; i < W_sim; i++) {
        double d = 0;
        if (i < npml) d = npml - i;
        else if (i > W_sim - 1 - npml) d = i - (W_sim - 1 - npml);
        double sigma = d > 0 ? sigma_max * (d / npml) * (d / npml) : 0;
        b[i] = (float)std::exp(-sigma * dt);
        a[i] = sigma > 0 ? (float)((double)b[i] - 1.0) : 0.0f;
    }
}

void cpmlZ(int H_sim, int npml, double dt, double OCEAN_Y, double SCALE, bool freeSurface,
           std::vector<float>& b, std::vector<float>& a) {
    b.assign(H_sim, 1.0f);
    a.assign(H_sim, 0.0f);
    const double sigma_max_base = -3.0 * V_N * std::log(0.001) / 2.0;
    const int sj_surf = (int)std::floor(OCEAN_Y / SCALE);
    for (int j = 0; j < H_sim; j++) {
        double d = 0, L = npml;
        if (j > H_sim - 1 - npml) {
            d = j - (H_sim - 1 - npml);
        } else if (!freeSurface && j < sj_surf) {
            d = sj_surf - j;
            L = sj_surf;   // top CPML thickness adapts to the space above sea level
        }
        double sigma = d > 0 ? (sigma_max_base / L) * (d / L) * (d / L) : 0;
        b[j] = (float)std::exp(-sigma * dt);
        a[j] = sigma > 0 ? (float)((double)b[j] - 1.0) : 0.0f;
    }
}
