#pragma once
#include <vector>

// Komatitsch & Martin (2007) CPML coefficients, target reflection R = 0.001,
// V_N = 0.68 (salt, the fastest normalized velocity), quadratic profile.

// x-direction: left PML [0, NPML) and right PML (W_sim-1-NPML, W_sim)
void cpmlX(int W_sim, int npml, double dt, std::vector<float>& b, std::vector<float>& a);

// z-direction: bottom PML; with the free surface disabled also a top PML that
// spans the water-column rows above the sea surface (port of recalcCPML).
void cpmlZ(int H_sim, int npml, double dt, double OCEAN_Y, double SCALE, bool freeSurface,
           std::vector<float>& b, std::vector<float>& a);
