#pragma once
#include <string>
#include <vector>

// A 2D model on a regular grid read from a file: value(ix, iz) = v[ix * nz + iz] (one column
// per x position, z down from the sea surface). Velocities in m/s, densities in g/cm^3.
struct GridModel {
    int nx = 0, nz = 0;
    double dx = 0, dz = 0;
    std::vector<float> v;
    std::string source;   // file name, for the UI

    bool empty() const { return v.empty(); }
    double width() const { return nx > 1 ? (nx - 1) * dx : 0.0; }
    double depth() const { return nz > 1 ? (nz - 1) * dz : 0.0; }
    float at(int ix, int iz) const { return v[(size_t)ix * nz + iz]; }
    // Bilinear interpolation at (x, z) metres, clamped to the model edges
    float sample(double x, double z) const;
    float minValue() const;
    float maxValue() const;
};

enum class ModelQuantity { Velocity, Density };

// SEG-Y model: one trace per column, samples down z (the layout the FWI program writes).
// dz = sample interval field / 1000 (an interval of 4000 "us" is 4 m); dx from the trace
// coordinates (CDP X, else source X, else group X, with the coordinate scalar) - 0 when the
// coordinates do not change between the first two traces.
struct SegyModelInfo {
    int nx = 0, nz = 0, format = 0;
    double dx = 0, dz = 0;
    bool littleEndian = false;
};
bool probeSegyModel(const std::string& path, SegyModelInfo& info, std::string& err);
bool readSegyModel(const std::string& path, double dx, double dz, GridModel& out, std::string& err);
// Writes a model as SEG-Y in the layout readSegyModel reads (one trace per column, IEEE
// float, CDP X in cm, dz in mm in the sample-interval field): what "Hız modeli yükle" opens.
// The quantity only labels the textual header.
bool writeSegyModel(const std::string& path, const GridModel& m, ModelQuantity q, std::string& err);

// Headerless float32. Column order: value(ix, iz) at ix * nz + iz (z fastest, the FWI
// program's layout); row order: at iz * nx + ix (x fastest).
bool readRawModel(const std::string& path, int nx, int nz, double dx, double dz, bool rowOrder, bool bigEndian,
                  GridModel& out, std::string& err);

// Text grid: one line per depth sample, one column per x position. Separators: ';' (then
// ',' may be the decimal mark), ',', tab or spaces. Lines starting with '#' and a
// non-numeric first line (column titles) are skipped.
bool probeAsciiModel(const std::string& path, int& nx, int& nz, std::string& err);
bool readAsciiModel(const std::string& path, double dx, double dz, GridModel& out, std::string& err);

// Layer boundaries of a piecewise-constant velocity model (an image, a drawing) without the
// pixel staircase. A gently dipping boundary in pixels is a row of steps one pixel high - on
// a 2 degree dip one every 29 pixels, several pixels high in an image scaled up from a
// smaller drawing - and such a regular row of steps radiates coherent dipping events
// (measured on a 1 m image model: as strong as all the real reflections below the seafloor).
// Each boundary between two values is traced from column to column (steps up to 8 pixels;
// slopes up to 45 degrees, steeper ones from row to row), its position smoothed over the
// length of its steps (a straight boundary stays straight, and within a step's height of the
// drawn one) and the
// pixels it crosses take the area-weighted mean bulk modulus and density of the two sides.
// rho (optional): filled with the matching density model (Gardner from each side's velocity;
// needed for the exact mix, as the Gardner density jumps at 1600 m/s). Without it the
// boundary pixels' velocities alone carry the mean bulk modulus, through the Gardner density.
// Models with more than 256 distinct values (gradients) are left as they are.
struct BoundaryStats { int boundaries = 0; size_t pixels = 0; bool applicable = false; };
BoundaryStats smoothBoundaries(GridModel& m, GridModel* rho = nullptr);

// Unit check after reading: velocities given in km/s (max < 20) become m/s, densities in
// kg/m^3 (max > 100) become g/cm^3. Fails on values no rock or water can have. note says
// what was converted (empty if nothing).
bool normalizeModelUnits(GridModel& m, ModelQuantity q, std::string& note, std::string& err);
