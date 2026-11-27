#include "ModelImport.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include "Material.hpp"
#include "Segy.hpp"

namespace fs = std::filesystem;

// ── GridModel ────────────────────────────────────────────────────────────
float GridModel::sample(double x, double z) const {
    if (v.empty()) return 0.0f;
    const double fx = nx > 1 && dx > 0 ? std::clamp(x / dx, 0.0, (double)(nx - 1)) : 0.0;
    const double fz = nz > 1 && dz > 0 ? std::clamp(z / dz, 0.0, (double)(nz - 1)) : 0.0;
    const int i0 = std::min((int)fx, std::max(0, nx - 2)), k0 = std::min((int)fz, std::max(0, nz - 2));
    const int i1 = std::min(i0 + 1, nx - 1), k1 = std::min(k0 + 1, nz - 1);
    const double tx = fx - i0, tz = fz - k0;
    const double top = at(i0, k0) + tx * (at(i1, k0) - at(i0, k0));
    const double bot = at(i0, k1) + tx * (at(i1, k1) - at(i0, k1));
    return (float)(top + tz * (bot - top));
}

float GridModel::minValue() const { return v.empty() ? 0.0f : *std::min_element(v.begin(), v.end()); }
float GridModel::maxValue() const { return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end()); }

namespace {

bool readAll(const std::string& path, std::vector<uint8_t>& buf, std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) { err = "Dosya açılamadı: " + path; return false; }
    const std::streamsize n = f.tellg();
    f.seekg(0);
    buf.resize((size_t)std::max<std::streamsize>(0, n));
    if (n > 0 && !f.read((char*)buf.data(), n)) { err = "Dosya okunamadı: " + path; return false; }
    return true;
}

std::string fileName(const std::string& path) {
    const std::u8string u = fs::u8path(path).filename().u8string();
    return std::string(u.begin(), u.end());
}

struct Reader {
    const uint8_t* p;
    bool le;
    uint16_t u16(size_t o) const { return le ? (uint16_t)(p[o] | p[o + 1] << 8) : (uint16_t)(p[o] << 8 | p[o + 1]); }
    int16_t i16(size_t o) const { return (int16_t)u16(o); }
    uint32_t u32(size_t o) const {
        return le ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24
                  : (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | (uint32_t)p[o + 3];
    }
    int32_t i32(size_t o) const { return (int32_t)u32(o); }
};

float ibmToFloat(uint32_t b) {
    const uint32_t frac = b & 0x00ffffffu;
    if (frac == 0) return 0.0f;
    const int ex = (int)((b >> 24) & 0x7f) - 64;
    const double v = (double)frac / 16777216.0 * std::pow(16.0, ex);
    return (float)((b >> 31) ? -v : v);
}

bool validFormat(int f) { return f == 1 || f == 2 || f == 3 || f == 5 || f == 8; }
int bytesPerSample(int f) { return f == 3 ? 2 : f == 8 ? 1 : 4; }

struct SegyLayout {
    SegyModelInfo info;
    size_t dataStart = 3600, traceBytes = 0;
};

bool parseSegy(const std::vector<uint8_t>& buf, SegyLayout& L, std::string& err) {
    if (buf.size() < 3600 + 240) { err = "Dosya bir SEG-Y için çok küçük."; return false; }
    Reader be{buf.data(), false}, le{buf.data(), true};
    Reader r = be;
    if (!validFormat(be.i16(3224))) {
        if (validFormat(le.i16(3224))) r = le;
        else { err = "SEG-Y veri biçimi tanınmadı (biçim kodu " + std::to_string(be.i16(3224)) + ")."; return false; }
    }
    L.info.littleEndian = r.le;
    L.info.format = r.i16(3224);
    const int ext = r.i16(3200 + 304);
    L.dataStart = 3600 + (ext > 0 && ext < 100 ? (size_t)ext * 3200 : 0);
    if (buf.size() < L.dataStart + 240) { err = "SEG-Y'de iz yok."; return false; }
    int ns = r.i16(3220);
    if (ns <= 0) ns = r.i16(L.dataStart + 114);
    if (ns <= 1) { err = "SEG-Y'de iz başına örnek sayısı okunamadı."; return false; }
    int dtField = r.u16(3216);
    if (dtField == 0) dtField = r.u16(L.dataStart + 116);
    L.traceBytes = 240 + (size_t)ns * bytesPerSample(L.info.format);
    const size_t nx = (buf.size() - L.dataStart) / L.traceBytes;
    if (nx < 2) { err = "SEG-Y'de en az iki iz (iki sütun) olmalı."; return false; }
    L.info.nx = (int)nx;
    L.info.nz = ns;
    L.info.dz = dtField / 1000.0;
    // dx from the coordinates of the first two traces
    const size_t t0 = L.dataStart, t1 = L.dataStart + L.traceBytes;
    const int sc = r.i16(t0 + 70);
    const double scale = sc > 0 ? sc : sc < 0 ? 1.0 / -sc : 1.0;
    for (size_t off : {180u, 72u, 80u}) {   // CDP X, source X, group X
        const int32_t a = r.i32(t0 + off), b = r.i32(t1 + off);
        if (a != b) { L.info.dx = std::fabs((double)b - a) * scale; break; }
    }
    return true;
}

bool checkSpacing(double dx, double dz, std::string& err) {
    if (!(dx > 0) || !(dz > 0)) { err = "Yatay (dx) ve düşey (dz) örnek aralığı sıfırdan büyük olmalı."; return false; }
    return true;
}

} // namespace

bool probeSegyModel(const std::string& path, SegyModelInfo& info, std::string& err) {
    std::vector<uint8_t> buf;
    if (!readAll(path, buf, err)) return false;
    SegyLayout L;
    if (!parseSegy(buf, L, err)) return false;
    info = L.info;
    return true;
}

bool readSegyModel(const std::string& path, double dx, double dz, GridModel& out, std::string& err) {
    if (!checkSpacing(dx, dz, err)) return false;
    std::vector<uint8_t> buf;
    if (!readAll(path, buf, err)) return false;
    SegyLayout L;
    if (!parseSegy(buf, L, err)) return false;
    Reader r{buf.data(), L.info.littleEndian};
    GridModel m;
    m.nx = L.info.nx; m.nz = L.info.nz; m.dx = dx; m.dz = dz;
    m.v.resize((size_t)m.nx * m.nz);
    const int f = L.info.format, bps = bytesPerSample(f);
    for (int ix = 0; ix < m.nx; ix++) {
        const size_t d = L.dataStart + (size_t)ix * L.traceBytes + 240;
        float* col = m.v.data() + (size_t)ix * m.nz;
        for (int iz = 0; iz < m.nz; iz++) {
            const size_t o = d + (size_t)iz * bps;
            float val;
            switch (f) {
                case 1: val = ibmToFloat(r.u32(o)); break;
                case 2: val = (float)r.i32(o); break;
                case 3: val = (float)r.i16(o); break;
                case 8: val = (float)(int8_t)buf[o]; break;
                default: { uint32_t b = r.u32(o); std::memcpy(&val, &b, 4); }
            }
            col[iz] = val;
        }
    }
    m.source = fileName(path);
    out = std::move(m);
    return true;
}

bool readRawModel(const std::string& path, int nx, int nz, double dx, double dz, bool rowOrder, bool bigEndian,
                  GridModel& out, std::string& err) {
    if (nx < 2 || nz < 2) { err = "nx ve nz en az 2 olmalı."; return false; }
    if (!checkSpacing(dx, dz, err)) return false;
    std::vector<uint8_t> buf;
    if (!readAll(path, buf, err)) return false;
    const size_t need = (size_t)nx * nz * 4;
    if (buf.size() != need) {
        err = "Dosya boyutu " + std::to_string(buf.size()) + " bayt; nx x nz x 4 = " + std::to_string(need) +
              " bayt olmalı. nx ve nz değerlerini kontrol edin.";
        return false;
    }
    Reader r{buf.data(), !bigEndian};
    GridModel m;
    m.nx = nx; m.nz = nz; m.dx = dx; m.dz = dz;
    m.v.resize((size_t)nx * nz);
    for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++) {
            const size_t k = rowOrder ? (size_t)iz * nx + ix : (size_t)ix * nz + iz;
            const uint32_t b = r.u32(k * 4);
            float val;
            std::memcpy(&val, &b, 4);
            m.v[(size_t)ix * nz + iz] = val;
        }
    m.source = fileName(path);
    out = std::move(m);
    return true;
}

namespace {

// Rows of numbers; false with a message naming the line on a malformed one
bool parseAscii(const std::string& path, std::vector<std::vector<float>>& rows, std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) { err = "Dosya açılamadı: " + path; return false; }
    std::string line;
    int lineNo = 0;
    bool firstData = true;
    while (std::getline(f, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.size() >= 3 && (uint8_t)line[0] == 0xEF) line.erase(0, 3);   // UTF-8 BOM
        const size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#') continue;
        const bool semi = line.find(';') != std::string::npos;
        std::vector<float> row;
        bool ok = true;
        size_t i = 0;
        while (i < line.size()) {
            auto isSep = [&](char c) { return c == ';' || c == '\t' || c == ' ' || (!semi && c == ','); };
            while (i < line.size() && isSep(line[i])) i++;
            if (i >= line.size()) break;
            size_t j = i;
            while (j < line.size() && !isSep(line[j])) j++;
            std::string tok = line.substr(i, j - i);
            if (semi) std::replace(tok.begin(), tok.end(), ',', '.');
            char* end = nullptr;
            const double val = std::strtod(tok.c_str(), &end);
            if (end == tok.c_str() || *end != '\0' || !std::isfinite(val)) { ok = false; break; }
            row.push_back((float)val);
            i = j;
        }
        if (!ok) {
            if (firstData && rows.empty()) { firstData = false; continue; }   // column titles
            err = "Satır " + std::to_string(lineNo) + " sayı olmayan bir değer içeriyor.";
            return false;
        }
        firstData = false;
        if (row.empty()) continue;
        if (!rows.empty() && row.size() != rows[0].size()) {
            err = "Satır " + std::to_string(lineNo) + " " + std::to_string(row.size()) + " değer içeriyor, ilk satır " +
                  std::to_string(rows[0].size()) + ". Bütün satırlarda aynı sayıda sütun olmalı.";
            return false;
        }
        rows.push_back(std::move(row));
    }
    if (rows.size() < 2 || rows[0].size() < 2) { err = "Dosyada en az 2 satır ve 2 sütun sayı olmalı."; return false; }
    return true;
}

} // namespace

bool probeAsciiModel(const std::string& path, int& nx, int& nz, std::string& err) {
    std::vector<std::vector<float>> rows;
    if (!parseAscii(path, rows, err)) return false;
    nz = (int)rows.size();
    nx = (int)rows[0].size();
    return true;
}

bool readAsciiModel(const std::string& path, double dx, double dz, GridModel& out, std::string& err) {
    if (!checkSpacing(dx, dz, err)) return false;
    std::vector<std::vector<float>> rows;
    if (!parseAscii(path, rows, err)) return false;
    GridModel m;
    m.nz = (int)rows.size(); m.nx = (int)rows[0].size(); m.dx = dx; m.dz = dz;
    m.v.resize((size_t)m.nx * m.nz);
    for (int iz = 0; iz < m.nz; iz++)
        for (int ix = 0; ix < m.nx; ix++) m.v[(size_t)ix * m.nz + iz] = rows[iz][ix];
    m.source = fileName(path);
    out = std::move(m);
    return true;
}

namespace {

double bulkOf(double v) { return (double)gardnerDensity(v) * v * v; }

// Velocity whose bulk modulus is (1 - fb) K(a) + fb K(b)
float mixVelocity(float a, float b, double fb) {
    if (fb <= 0) return a;
    if (fb >= 1) return b;
    const double K = (1 - fb) * bulkOf(a) + fb * bulkOf(b);
    double lo = std::min(a, b), hi = std::max(a, b);
    for (int i = 0; i < 40; i++) {
        const double mid = 0.5 * (lo + hi);
        (bulkOf(mid) < K ? lo : hi) = mid;
    }
    return (float)(0.5 * (lo + hi));
}

// Gaussian smoothing of a staircase y (constant runs, the steps). The width follows the steps
// where it is: sigma = half the local step length (the run's own and its neighbours' mean;
// leaves 0.7 % of the step ripple) - a boundary whose dip changes is smoothed over its own
// steps everywhere, not over one mean length that is too short on its flat parts and blurs
// its bends. At least 4 step heights: on steeper boundaries the steps' lengths alternate
// (20 degrees: 3, 3, 3, 2 pixels, repeating every 11 columns; 4 times that in an image scaled
// up 4 times), a slow ripple a narrow filter lets through. Beyond the ends the curve is
// continued by point reflection about the end of a straight line fitted to the first (last)
// steps, so that a straight boundary stays straight up to its ends.
std::vector<double> smoothSteps(const std::vector<double>& y) {
    const int n = (int)y.size();
    // runs of equal values: start index, length; step heights to the neighbours
    std::vector<int> rs, rl;
    for (int k = 0; k < n; k++)
        if (k == 0 || y[k] != y[k - 1]) { rs.push_back(k); rl.push_back(1); }
        else rl.back()++;
    const int nr = (int)rs.size();
    std::vector<double> sig(n);
    double periodFirst = rl[0], periodLast = rl[nr - 1];
    for (int r = 0; r < nr; r++) {
        double len = 0, hgt = 0;
        int cl = 0, ch = 0;
        for (int q = std::max(0, r - 1); q <= std::min(nr - 1, r + 1); q++) {
            len += rl[q]; cl++;
            if (q > 0) { hgt += std::fabs(y[rs[q]] - y[rs[q - 1]]); ch++; }
        }
        const double period = std::min(400.0, len / cl), height = ch ? hgt / ch : 1.0;
        if (r == 0) periodFirst = period;
        if (r == nr - 1) periodLast = period;
        const double s = std::min(200.0, std::max(0.5 * period, 4.0 * std::max(1.0, height)));
        for (int k = rs[r]; k < rs[r] + rl[r]; k++) sig[k] = s;
    }
    auto endValue = [&](bool first) {
        const int m = std::min(n, std::max(3, (int)std::ceil(2 * (first ? periodFirst : periodLast))));
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int k = 0; k < m; k++) {
            const double x = k, v = first ? y[k] : y[n - 1 - k];
            sx += x; sy += v; sxx += x * x; sxy += x * v;
        }
        const double d = m * sxx - sx * sx;
        return d > 0 ? (sy * sxx - sx * sxy) / d : sy / m;   // the line's value at the end
    };
    const double e0 = endValue(true), e1 = endValue(false);
    auto at = [&](int j) {
        if (j < 0) return 2 * e0 - y[std::min(-j, n - 1)];
        if (j > n - 1) return 2 * e1 - y[std::max(2 * (n - 1) - j, 0)];
        return y[j];
    };
    auto gauss = [&](const std::vector<double>& sg) {
        std::vector<double> out(n);
        for (int k = 0; k < n; k++) {
            const double sigma = sg[k];
            const int h = (int)std::ceil(3 * sigma);
            double s = 0, ws = 0;
            for (int j = -h; j <= h; j++) {   // symmetric about k: a straight line stays straight
                const double wj = std::exp(-0.5 * j * j / (sigma * sigma));
                s += wj * at(k + j);
                ws += wj;
            }
            out[k] = s / ws;
        }
        return out;
    };
    // A mean over a bend pulls the curve inwards by about sigma^2 / 2R: where the first pass
    // shows a bend, sigma is cut so that this stays under 0.1 pixel (at the top of an arc the
    // steps are long, but there is only one of them to smooth)
    const std::vector<double> first = gauss(sig);
    std::vector<double> sig2 = sig;
    for (int k = 0; k < n; k++) {
        const int d = std::max(1, (int)std::lround(2 * sig[k]));   // over two widths: the steps' leftover ripple is no bend
        if (k - d < 0 || k + d > n - 1) continue;
        const double curv = std::fabs(first[k + d] - 2 * first[k] + first[k - d]) / ((double)d * d);
        if (curv > 0) sig2[k] = std::max(2.0, std::min(sig[k], std::sqrt(0.2 / curv)));
    }
    return gauss(sig2);
}

// One direction: lines are columns (positions down a column) or rows (positions along a row);
// idx(line, pos) is the index into the model's values
template <class Index>
void smoothPass(const std::vector<float>& orig, std::vector<float>& out, std::vector<float>* rhoOut, std::vector<uint8_t>& touched,
                int nLines, int nPos, Index idx, BoundaryStats& st) {
    struct Tr { int pos, chain; float a, b; };
    struct Chain { int L0; float a, b; std::vector<int> pos; };
    std::vector<Chain> chains;
    std::vector<Tr> prev, cur;
    for (int L = 0; L < nLines; L++) {
        cur.clear();
        for (int p = 1; p < nPos; p++) {
            const float a = orig[idx(L, p - 1)], b = orig[idx(L, p)];
            if (a != b) cur.push_back({p, -1, a, b});
        }
        // Continue a chain of the line before: same two values, the nearest within kJump pixels
        // (an image scaled up from a smaller drawing has steps several pixels high)
        constexpr int kJump = 8;
        size_t j0 = 0;
        for (Tr& t : cur) {
            while (j0 < prev.size() && prev[j0].pos < t.pos - kJump) j0++;
            int best = -1;
            for (size_t j = j0; j < prev.size() && prev[j].pos <= t.pos + kJump; j++) {
                const Tr& u = prev[j];
                if (u.a != t.a || u.b != t.b) continue;
                const Chain& c = chains[u.chain];
                if ((int)c.pos.size() != L - c.L0) continue;   // already continued in this line
                if (best < 0 || std::abs(u.pos - t.pos) < std::abs(prev[best].pos - t.pos)) best = (int)j;
            }
            if (best >= 0) {
                t.chain = prev[best].chain;
                chains[t.chain].pos.push_back(t.pos);
            } else {
                t.chain = (int)chains.size();
                chains.push_back({L, t.a, t.b, {t.pos}});
            }
        }
        std::swap(prev, cur);
    }
    for (const Chain& c : chains) {
        const int n = (int)c.pos.size();
        int jumps = 0, maxStep = 1;
        long travel = 0;
        for (int k = 1; k < n; k++) {
            const int d = std::abs(c.pos[k] - c.pos[k - 1]);
            jumps += d != 0;
            travel += d;
            maxStep = std::max(maxStep, d);
        }
        if (n < 3 || jumps == 0) continue;
        if (travel > n - 1) continue;   // steeper than 45 degrees on average: the other direction's pass
        std::vector<double> z(n);
        for (int k = 0; k < n; k++) z[k] = c.pos[k] - 0.5;   // between the two pixels
        const std::vector<double> zs = smoothSteps(z);
        bool any = false;
        for (int k = 0; k < n; k++) {
            // the smoothed boundary stays within the steps' height of the drawn one
            const double s = std::clamp(zs[k], z[k] - maxStep, z[k] + maxStep);
            const int L = c.L0 + k;
            // only the two layers' own pixels next to this boundary: up the a run, down the b run
            int p0 = c.pos[k] - 1, p1 = c.pos[k];
            const int lo = std::max(0, (int)std::floor(std::min(z[k], s)) - 1), hi = std::min(nPos - 1, (int)std::ceil(std::max(z[k], s)) + 1);
            while (p0 - 1 >= lo && orig[idx(L, p0 - 1)] == c.a) p0--;
            while (p1 + 1 <= hi && orig[idx(L, p1 + 1)] == c.b) p1++;
            for (int p = p0; p <= p1; p++) {
                const size_t i = idx(L, p);
                const float o = orig[i];
                if (touched[i]) continue;
                const double fb = std::clamp(p + 0.5 - s, 0.0, 1.0);   // share of the pixel past the boundary
                if (fb == (o == c.b ? 1.0 : 0.0)) continue;            // wholly on its own side, as before
                if (rhoOut) {   // both the bulk modulus and the density mixed by area
                    const double ra = gardnerDensity(c.a), rb = gardnerDensity(c.b);
                    const double r = (1 - fb) * ra + fb * rb, K = (1 - fb) * ra * c.a * c.a + fb * rb * c.b * c.b;
                    out[i] = (float)std::sqrt(K / r);
                    (*rhoOut)[i] = (float)r;
                } else {
                    out[i] = mixVelocity(c.a, c.b, fb);
                }
                touched[i] = 1;
                st.pixels++;
                any = true;
            }
        }
        st.boundaries += any;
    }
}

} // namespace

BoundaryStats smoothBoundaries(GridModel& m, GridModel* rho) {
    BoundaryStats st;
    if (m.nx < 3 || m.nz < 3) return st;
    {   // piecewise constant: at most 256 distinct values
        std::vector<float> seen;
        float last = 0;
        bool haveLast = false;   // (no NaN sentinel: comparisons with NaN are not reliable under /fp:fast)
        for (float x : m.v) {
            if (haveLast && x == last) continue;   // runs of one value: the common case
            last = x;
            haveLast = true;
            if (std::find(seen.begin(), seen.end(), x) != seen.end()) continue;
            if (seen.size() == 256) return st;
            seen.push_back(x);
        }
    }
    st.applicable = true;
    const std::vector<float> orig = m.v;
    std::vector<uint8_t> touched(orig.size(), 0);
    const int nx = m.nx, nz = m.nz;
    std::vector<float>* rv = nullptr;
    if (rho) {
        *rho = GridModel{};
        rho->nx = m.nx; rho->nz = m.nz; rho->dx = m.dx; rho->dz = m.dz;
        rho->source = m.source;
        rho->v.resize(orig.size());
        for (size_t k = 0; k < orig.size(); k++) rho->v[k] = gardnerDensity(orig[k]);
        rv = &rho->v;
    }
    // Boundaries up to 45 degrees, column by column; then the steeper ones, row by row
    smoothPass(orig, m.v, rv, touched, nx, nz, [nz](int ix, int iz) { return (size_t)ix * nz + iz; }, st);
    smoothPass(orig, m.v, rv, touched, nz, nx, [nz](int iz, int ix) { return (size_t)ix * nz + iz; }, st);
    return st;
}

bool normalizeModelUnits(GridModel& m, ModelQuantity q, std::string& note, std::string& err) {
    note.clear();
    for (float x : m.v)
        if (!std::isfinite(x)) { err = "Modelde sayı olmayan (NaN / sonsuz) değerler var."; return false; }
    double mn = m.minValue(), mx = m.maxValue();
    char b[160];
    if (q == ModelQuantity::Velocity) {
        if (mx > 0 && mx < 20) {
            for (float& x : m.v) x *= 1000.0f;
            mn *= 1000; mx *= 1000;
            note = "Hızlar km/s olarak okundu ve m/s'ye çevrildi.";
        }
        if (mn < 300 || mx > 10000) {
            std::snprintf(b, sizeof(b), "Hız değerleri %.0f – %.0f m/s; 300 – 10000 m/s aralığında olmalı.", mn, mx);
            err = b;
            return false;
        }
    } else {
        if (mx > 100) {
            for (float& x : m.v) x /= 1000.0f;
            mn /= 1000; mx /= 1000;
            note = "Yoğunluklar kg/m³ olarak okundu ve g/cm³'e çevrildi.";
        }
        if (mn < 0.5 || mx > 6) {
            std::snprintf(b, sizeof(b), "Yoğunluk değerleri %.2f – %.2f g/cm³; 0,5 – 6 g/cm³ aralığında olmalı.", mn, mx);
            err = b;
            return false;
        }
    }
    return true;
}

bool writeSegyModel(const std::string& path, const GridModel& m, ModelQuantity q, std::string& err) {
    if (m.empty() || m.nx < 2 || m.nz < 2) { err = "Kaydedilecek model yok."; return false; }
    if (m.nz > 32767) { err = "Model SEG-Y'ye yazılamıyor: iz başına en fazla 32767 örnek (düşey piksel) olabilir."; return false; }
    if (!(m.dz > 0) || m.dz * 1000.0 > 65535.0) { err = "Model SEG-Y'ye yazılamıyor: düşey örnek aralığı 0 ile 65 m arasında olmalı."; return false; }
    char b[96];
    std::vector<std::string> text;
    text.push_back(std::string("QUANTITY: ") + (q == ModelQuantity::Velocity ? "P-WAVE VELOCITY (M/S)" : "DENSITY (G/CM3)"));
    std::string src;
    for (char c : m.source) src += (unsigned char)c < 128 ? (char)std::toupper((unsigned char)c) : '_';   // EBCDIC header: ASCII only
    text.push_back("SOURCE: " + src.substr(0, 60));
    std::snprintf(b, sizeof(b), "COLUMNS (TRACES): %d  DX: %g M  (CDP X IN CM, SCALAR -100)", m.nx, m.dx);
    text.push_back(b);
    std::snprintf(b, sizeof(b), "SAMPLES PER TRACE: %d  DZ: %g M  (SAMPLE INTERVAL FIELD IN MM)", m.nz, m.dz);
    text.push_back(b);
    text.push_back("DEPTH 0 = SEA SURFACE");
    std::snprintf(b, sizeof(b), "VALUE RANGE: %g - %g", m.minValue(), m.maxValue());
    text.push_back(b);
    const std::vector<uint8_t> seg = buildModelSegy(m.v.data(), m.nx, m.nz, m.dx, m.dz, text);   // v is column-major, as SEG-Y traces
    std::ofstream o(std::filesystem::u8path(path), std::ios::binary);
    if (o) o.write((const char*)seg.data(), (std::streamsize)seg.size());
    if (!o) { err = "Dosya yazılamadı: " + path; return false; }
    return true;
}
