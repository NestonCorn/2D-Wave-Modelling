#pragma once
#include <vector>
#include "Params.hpp"

struct Pt { float x, y; };

// Polylines used for the background drawing (layout.seafloor/deep/salt in the HTML)
struct GeoLayout {
    std::vector<Pt> seafloor, deep, salt;
};

// Seafloor / basement / salt-top depth (m) at horizontal position x for a model.
void layerDepths(GeologyModel g, double x, double W, double H, double& sf, double& dp, double& sc);

GeoLayout buildLayout(GeologyModel g, double W, double H);

// Fills V_sq (normalized velocity squared: 0.25 water, 0.35 sediment, 0.48 basement, 0.68 salt)
void fillVsq(std::vector<float>& vsq, int W_sim, int H_sim, double SCALE, GeologyModel g, double W, double H);

// Seafloor depth used for OBS placement (without the +4 m recording offset).
// Single shared helper: the HTML repeated this formula in four places.
double obsSeafloorDepth(GeologyModel g, double rx, double W, double H);
