#include "ModelImport.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

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
