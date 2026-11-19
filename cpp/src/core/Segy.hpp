#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// SEG-Y Rev 1 writer (IEEE float32, big-endian), byte-compatible with buildSEGYFile in the HTML.
struct SegyReceiver { double x, y, depth; };

struct SegySpec {
    const float* traces = nullptr;   // nTraces * nSamples, trace-major
    int nTraces = 0, nSamples = 0, dtMicros = 1000, shotNum = 1;
    std::string label, modeLabel;
    double sourceX = 0, sourceY = 0, sourceDepth = 0;
    std::function<SegyReceiver(int)> receiver;
    // Textual header metadata
    int fd_order = 10;
    double freq = 50, SCALE = 4, domainW = 1200, domainH = 480, oceanY = 80;
    std::string geology = "SALT";
};

std::vector<uint8_t> buildSegy(const SegySpec& s);

// Linear resampler: traces of oldSamples samples (stored `stride` apart) → newSamples samples
// (1 ms each in practice)
std::vector<float> resampleGather(const float* sismo, size_t offset, int nTraces, int stride, int oldSamples, int newSamples);

// JavaScript-style number → string (shortest round-trip, no trailing ".0")
std::string jsNum(double v);
// float overload: 0.1f prints as "0.1", not as its double expansion 0.10000000149011612
std::string jsNum(float v);
// Math.round semantics
inline long long jsRound(double v) { return (long long)std::floor(v + 0.5); }
