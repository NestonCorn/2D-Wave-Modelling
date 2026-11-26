#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ModelImport.hpp"

// Velocity model drawn as an image: every pixel is reduced to one value, its gray level
// 0..255 (luma, 0.299 R + 0.587 G + 0.114 B), and each gray level present gets a velocity.
// The image spans the model: left edge x = 0, top row = sea surface, bottom row = depth.
struct GrayImage {
    int w = 0, h = 0;
    std::vector<uint8_t> gray;            // row-major, gray[y * w + x]
    std::array<int, 256> count{};          // pixels per gray level
    int colours = 0;                       // distinct RGB colours (more than levels: colours merged)
    std::vector<int> levels() const;       // gray levels present, ascending
};

// PNG, JPEG, BMP, GIF (first frame) or TIFF through the Windows Imaging Component
bool loadGrayImage(const std::string& utf8Path, GrayImage& out, std::string& err);

// value(ix, iz) = velocity[gray at column ix, row iz]; dx, dz from the given size in metres
GridModel modelFromGray(const GrayImage& img, const std::array<float, 256>& velocity, double widthM, double depthM,
                        const std::string& name);
