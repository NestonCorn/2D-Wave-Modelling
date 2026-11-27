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

// Velocity and density of a built-in model on the grid. average = false: the layer at each
// node (a dipping boundary becomes a staircase of grid cells). average = true: each cell
// (SCALE x SCALE around its node) takes the mean of the layers over its area - exact in depth,
// 8 sub-columns across: the mean bulk modulus and the mean density of the cell. Density from
// the velocity (Gardner) in each layer.
//
// Measured against a 1 m grid (50 Hz, 4 m grid, tests --scatter): the error of the node value
// swings from 12 % to 30 % of the record with where a boundary falls between two nodes, and a
// 12 degree dipping one scatters off its steps; the mean K stays at 14-15 % in all three. The
// textbook effective medium (harmonic mean of K, the long-wavelength limit) did worse here:
// 22-25 %, and it made a boundary lying on a node worse than the node value.
void fillPresetModel(std::vector<float>& vsq, std::vector<float>& rho, int W_sim, int H_sim, double SCALE, GeologyModel g,
                     double W, double H, bool average);

// Seafloor depth used for OBS placement (without the +4 m recording offset).
// Single shared helper: the HTML repeated this formula in four places.
double obsSeafloorDepth(GeologyModel g, double rx, double W, double H);
