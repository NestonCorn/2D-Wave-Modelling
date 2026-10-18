#include <cmath>
#include "Segy.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>

namespace {

// Complete ASCII → EBCDIC (IBM CP037) lookup table
const uint8_t A2E[128] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2D, 0x2E, 0x2F, 0x16, 0x05, 0x25, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x3C, 0x3D, 0x32, 0x26, 0x18, 0x19, 0x3F, 0x27, 0x1C, 0x1D, 0x1E, 0x1F,
    0x40, 0x5A, 0x7F, 0x7B, 0x5B, 0x6C, 0x50, 0x7D, 0x4D, 0x5D, 0x5C, 0x4E, 0x6B, 0x60, 0x4B, 0x61,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0x7A, 0x5E, 0x4C, 0x7E, 0x6E, 0x6F,
    0x7C, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6,
    0xD7, 0xD8, 0xD9, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0x4A, 0xE0, 0x5A, 0x5F, 0x6D,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xC0, 0x4F, 0xD0, 0xA1, 0x07};

struct BE {
    uint8_t* p;
    void i16(size_t o, int v) { uint16_t u = (uint16_t)(int16_t)v; p[o] = u >> 8; p[o + 1] = u & 0xFF; }
    void u16(size_t o, unsigned v) { p[o] = (v >> 8) & 0xFF; p[o + 1] = v & 0xFF; }
    void i32(size_t o, long long v) {
        uint32_t u = (uint32_t)(int32_t)v;
        p[o] = u >> 24; p[o + 1] = (u >> 16) & 0xFF; p[o + 2] = (u >> 8) & 0xFF; p[o + 3] = u & 0xFF;
    }
    void f32(size_t o, float f) { uint32_t u; std::memcpy(&u, &f, 4); i32(o, (int32_t)u); }
};

std::string utcNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

} // namespace

std::string jsNum(double v) {
    if (std::isnan(v)) return "NaN";
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string((long long)v);
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

std::vector<float> resampleGather(const float* sismo, size_t offset, int nTraces, int oldS, int newS) {
    std::vector<float> out((size_t)nTraces * newS);
    for (int tr = 0; tr < nTraces; tr++) {
        for (int s = 0; s < newS; s++) {
            double mapped = s * (double)(oldS - 1) / std::max(1, newS - 1);
            int s0 = (int)std::floor(mapped);
            int s1 = std::min(s0 + 1, oldS - 1);
            double f = mapped - s0;
            double v0 = sismo[offset + (size_t)tr * oldS + s0];
            double v1 = sismo[offset + (size_t)tr * oldS + s1];
            out[(size_t)tr * newS + s] = (float)(v0 * (1 - f) + v1 * f);
        }
    }
    return out;
}

std::vector<uint8_t> buildSegy(const SegySpec& S) {
    const int TH = 240;
    const size_t SB = (size_t)S.nSamples * 4;
    const size_t total = 3200 + 400 + (size_t)S.nTraces * (TH + SB);
    std::vector<uint8_t> buf(total, 0);
    BE dv{buf.data()};

    // ── Textual File Header (3200 bytes, 40 lines × 80 chars EBCDIC) ──
    std::string now = utcNow("%Y-%m-%d %H:%M:%S");
    char tmp[128];
    std::vector<std::string> lines;
    lines.push_back("C 1 SEG-Y REV1 - SLO/CLO ACOUSTIC FDTD SIMULATOR");
    lines.push_back("C 2 SHOT NUMBER: " + std::to_string(S.shotNum) + "  TYPE: " + S.label);
    lines.push_back("C 3 MODE: " + S.modeLabel + "  DATE: " + now);
    lines.push_back("C 4 SOURCE X: " + jsNum(S.sourceX) + "m  SOURCE Y: " + jsNum(S.sourceY) + "m");
    lines.push_back("C 5 SOURCE DEPTH BHD: " + jsNum(S.sourceDepth) + "m");
    lines.push_back("C 6 NUMBER OF TRACES: " + std::to_string(S.nTraces));
    lines.push_back("C 7 SAMPLES PER TRACE: " + std::to_string(S.nSamples));
    lines.push_back("C 8 SAMPLE INTERVAL: " + std::to_string(S.dtMicros) + " MICROSECONDS");
    lines.push_back("C 9 DATA FORMAT: IEEE 32-BIT FLOAT (FORMAT CODE 5)");
    std::snprintf(tmp, sizeof(tmp), "C10 FDTD ORDER: %d  DOMINANT FREQ: %.1f HZ", S.fd_order, S.freq);
    lines.push_back(tmp);
    lines.push_back("C11 GRID SCALE: " + jsNum(S.SCALE) + "m/CELL  DH=" + jsNum(S.SCALE) + "m");
    lines.push_back("C12 PHYSICAL DOMAIN: " + jsNum(S.domainW) + "m x " + jsNum(S.domainH) + "m");
    lines.push_back("C13 GEOLOGY MODEL: " + S.geology);
    for (int i = 0; i < 26; i++) lines.push_back("C" + std::to_string(i + 14));
    lines.push_back("C40 END TEXTUAL HEADER");
    for (int ln = 0; ln < 40; ln++) {
        std::string text = ln < (int)lines.size() ? lines[ln] : "";
        text.resize(80, ' ');
        for (int c = 0; c < 80; c++) {
            unsigned char ch = (unsigned char)text[c];
            buf[ln * 80 + c] = ch < 128 ? A2E[ch] : 0x40;
        }
    }

    // ── Binary File Header (400 bytes @ 3200) ──
    const size_t bh = 3200;
    dv.i32(bh + 0, S.shotNum);      // Job ID
    dv.i32(bh + 4, S.shotNum);      // Line number
    dv.i32(bh + 8, S.shotNum);      // Reel number
    dv.i16(bh + 12, S.nTraces);     // Traces per ensemble
    dv.i16(bh + 14, 0);             // Aux traces
    dv.i16(bh + 16, S.dtMicros);    // Sample interval (μs)
    dv.i16(bh + 18, S.dtMicros);
    dv.i16(bh + 20, S.nSamples);    // Samples per trace
    dv.i16(bh + 22, S.nSamples);
    dv.i16(bh + 24, 5);             // Format 5 = IEEE float32
    dv.i16(bh + 26, S.nTraces);     // Ensemble fold
    dv.i16(bh + 28, 2);             // Sorting: CDP
    dv.i16(bh + 54, 1);             // Meters
    dv.u16(bh + 300, 0x0100);       // SEG-Y Rev 1.0
    dv.u16(bh + 302, 1);            // Fixed length traces
    dv.u16(bh + 304, 0);            // No extended text headers

    // ── Trace headers + data ──
    for (int i = 0; i < S.nTraces; i++) {
        SegyReceiver rec = S.receiver(i);
        const long long offset = jsRound(rec.x - S.sourceX);
        const size_t th = 3600 + (size_t)i * (TH + SB);
        dv.i32(th + 0, i + 1);
        dv.i32(th + 4, i + 1);
        dv.i32(th + 8, S.shotNum);
        dv.i32(th + 12, i + 1);
        dv.i32(th + 16, S.shotNum);
        dv.i32(th + 20, i + 1);
        dv.i32(th + 24, i + 1);
        dv.i16(th + 28, 1);
        dv.i16(th + 30, 1);
        dv.i16(th + 32, 1);
        dv.i16(th + 34, 1);
        dv.i32(th + 36, offset);
        dv.i32(th + 40, 0);
        dv.i32(th + 44, 0);
        dv.i32(th + 48, -jsRound(S.sourceDepth));
        dv.i32(th + 52, 0);
        dv.i32(th + 56, 0);
        dv.i32(th + 60, jsRound(S.oceanY));
        dv.i32(th + 64, jsRound(rec.depth != 0 ? rec.depth : S.oceanY));
        dv.i16(th + 68, 1);
        dv.i16(th + 70, 1);
        dv.i32(th + 72, jsRound(S.sourceX));
        dv.i32(th + 76, jsRound(S.sourceY));
        dv.i32(th + 80, jsRound(rec.x));
        dv.i32(th + 84, jsRound(rec.y));
        dv.i16(th + 88, 1);
        dv.i16(th + 114, S.nSamples);
        dv.i16(th + 116, S.dtMicros);
        dv.i16(th + 156, S.shotNum);   // (the original stores the shot number in the "year" field)
        const size_t ds = th + TH;
        const float* tr = S.traces + (size_t)i * S.nSamples;
        for (int s = 0; s < S.nSamples; s++) dv.f32(ds + (size_t)s * 4, tr[s]);
    }
    return buf;
}
