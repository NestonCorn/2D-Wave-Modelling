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

// Headerless float32. Column order: value(ix, iz) at ix * nz + iz (z fastest, the FWI
// program's layout); row order: at iz * nx + ix (x fastest).
bool readRawModel(const std::string& path, int nx, int nz, double dx, double dz, bool rowOrder, bool bigEndian,
                  GridModel& out, std::string& err);

// Text grid: one line per depth sample, one column per x position. Separators: ';' (then
// ',' may be the decimal mark), ',', tab or spaces. Lines starting with '#' and a
// non-numeric first line (column titles) are skipped.
bool probeAsciiModel(const std::string& path, int& nx, int& nz, std::string& err);
bool readAsciiModel(const std::string& path, double dx, double dz, GridModel& out, std::string& err);

// Unit check after reading: velocities given in km/s (max < 20) become m/s, densities in
// kg/m^3 (max > 100) become g/cm^3. Fails on values no rock or water can have. note says
// what was converted (empty if nothing).
bool normalizeModelUnits(GridModel& m, ModelQuantity q, std::string& note, std::string& err);
