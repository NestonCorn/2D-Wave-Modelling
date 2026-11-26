#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "ModelImport.hpp"

// A velocity model drawn by hand: a raster of palette indices (one palette entry = one
// layer: name, display colour, velocity). Row 0 is the sea surface, the last row the model
// depth; column 0 is x = 0. Drawing operations, undo and the conversion to a GridModel live
// here so they can be tested without the UI.
struct PaletteEntry {
    std::string name;
    float velocity = 2000;   // m/s
    uint32_t rgb = 0x808080; // display colour 0xRRGGBB
};

class ModelCanvas {
public:
    using Pt = std::pair<float, float>;   // canvas pixel coordinates (x right, y down)

    int w = 0, h = 0;
    std::vector<uint8_t> px;             // palette index per pixel, row-major
    std::vector<PaletteEntry> palette;

    // Water over a sediment layer, with a default palette
    void initDefault(int width, int height);
    void resize(int width, int height);              // nearest-neighbour resample
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    uint8_t at(int x, int y) const { return px[(size_t)y * w + x]; }

    // Drawing (call snapshot() first to make it undoable)
    void brush(float cx, float cy, float radius, uint8_t c);
    void line(float x0, float y0, float x1, float y1, float radius, uint8_t c);   // round brush stamped along
    void polyline(const std::vector<Pt>& pts, float radius, uint8_t c);
    // Layer boundary: the line through pts (sorted by x, extended flat to both edges) and
    // everything below it take colour c
    void fillBelow(std::vector<Pt> pts, uint8_t c);
    int floodFill(int x, int y, uint8_t c);          // 4-connected; pixels changed
    void fillAll(uint8_t c);

    // Undo / redo (whole-raster snapshots, the last 40)
    void snapshot();
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // Palette
    int addEntry(const PaletteEntry& e);              // index, -1 when full (256)
    void removeEntry(int i, int replaceWith);         // pixels of i take replaceWith
    std::vector<int> counts() const;

    // Model: x = 0 .. widthM over the columns, depth 0 .. depthM over the rows
    GridModel toModel(double widthM, double depthM, const std::string& name) const;
    // From a velocity grid (rows from the sea surface down, vel[k * nx + i], m/s): velocities
    // rounded to 10 m/s, each distinct one a palette entry (the most frequent 32 at most; the
    // rest go to the nearest of those)
    void fromVelocity(const float* vel, int nx, int nz, int width, int height);

    // Drawing file (JSON): model size, palette (name, colour, velocity) and the raster as
    // run lengths over the rows (a layered drawing is a few thousand numbers). fromJson() checks
    // everything before it changes the canvas and clears undo / redo on success.
    std::string toJson(double widthM, double depthM) const;
    bool fromJson(const std::string& text, double& widthM, double& depthM, std::string& err);

private:
    struct Snap { int w, h; std::vector<uint8_t> px; };
    std::vector<Snap> undo_, redo_;
};

// Display colour for a velocity (for palette entries made from a model)
uint32_t colourForVelocity(float v);
