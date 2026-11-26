#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Files written next to a line's SEG-Y: an image of the velocity model and a PDF report of
// every parameter. No external libraries: a small deflate (zlib) encoder for PNG and PDF
// streams, text from a Windows TrueType font (rasterised with stb_truetype for the image,
// embedded as a subset for the PDF so Turkish letters are exact and the text is selectable).
namespace report {

std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t n);

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // w * h * 3
};
bool writePng(const std::string& path, const Image& img, std::string& err);

// Velocity model with its acquisition geometry
struct ModelPlot {
    int nx = 0, nz = 0;
    double dx = 4, dz = 4;
    const float* vel = nullptr;     // m/s, vel[k * nx + i]: x = i * dx, depth = k * dz
    std::string title, subtitle;
    std::vector<double> shotsMain, shotsSrc;                // source x of each shot (m)
    double sourceDepth = 0;
    bool cable = false;                                      // streamer at the first shot
    double cableX0 = 0, cableX1 = 0, cableZ0 = 0, cableZ1 = 0;   // tail -> head
    std::vector<std::pair<double, double>> obs;              // OBS (x, depth)
};
bool renderModel(const ModelPlot& m, Image& out, std::string& err);

struct Row { std::string key, value; };
struct Section {
    std::string title;
    std::vector<Row> rows;
};
struct Document {
    std::string title, subtitle;
    const Image* image = nullptr;
    std::string imageCaption;
    std::vector<Section> sections;
};
bool writePdf(const std::string& path, const Document& doc, std::string& err);

} // namespace report
