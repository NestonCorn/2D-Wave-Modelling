#pragma once
#include <cmath>
#include <cstdint>
#include <fstream>
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

// Depth model: one trace per column (columns[i * nz + k], x = i * dx, depth = k * dz), IEEE
// float32. CDP, source and group X hold the column x in centimetres (coordinate scalar -100);
// the sample-interval field holds dz in millimetres, the usual convention for depth data
// (4000 = 4 m), so the app's model import reads the file back as it is. `text`: lines 2..
// of the textual header (without the "Cnn " prefix).
std::vector<uint8_t> buildModelSegy(const float* columns, int nx, int nz, double dx, double dz, const std::vector<std::string>& text);

// One SEG-Y file holding every shot of a line: the file header is written from the first
// shot, then append() adds each shot's traces (FFID = shotNum, trace sequence numbers run on
// through the file). All shots must have the same samples per trace and sample interval.
class SegyLineWriter {
public:
    ~SegyLineWriter() { close(); }
    bool open(const std::string& path, const SegySpec& first, const std::string& lineNote, std::string& err);
    bool append(const SegySpec& shot, std::string& err);
    void close();
    bool isOpen() const { return f_.is_open(); }
    long long traceCount() const { return seq_; }

private:
    std::ofstream f_;
    int nSamples_ = 0, dtMicros_ = 0;
    long long seq_ = 0;
};

// Linear resampler: traces of oldSamples samples (stored `stride` apart) → newSamples samples
// (1 ms each in practice)
std::vector<float> resampleGather(const float* sismo, size_t offset, int nTraces, int stride, int oldSamples, int newSamples);

// JavaScript-style number → string (shortest round-trip, no trailing ".0")
std::string jsNum(double v);
// float overload: 0.1f prints as "0.1", not as its double expansion 0.10000000149011612
std::string jsNum(float v);
// Math.round semantics
inline long long jsRound(double v) { return (long long)std::floor(v + 0.5); }
