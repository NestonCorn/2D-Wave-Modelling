#include "ParamsIO.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include "Segy.hpp"   // jsNum

using nlohmann::ordered_json;
namespace fs = std::filesystem;

static const char* geoName(GeologyModel g) {
    return g == GeologyModel::Flat ? "flat" : g == GeologyModel::Fault ? "fault" : "salt";
}

std::string paramsToJson(const SimParams& p, double shipX) {
    ordered_json j;
    j["sl_fdOrder"] = std::to_string(p.fd_order);
    j["sl_dh"] = jsNum(p.dh);
    j["sl_dt"] = jsNum(p.dt);
    j["sl_npml"] = std::to_string(p.npml);
    j["sl_freq"] = jsNum(p.freq);
    j["sl_amp"] = jsNum(p.amp);
    j["sl_cable"] = jsNum(p.cable_len);
    j["sl_groupSpace"] = jsNum(p.receiver_spacing);
    j["sl_cableHead"] = jsNum(p.cable_head_depth);
    j["sl_cableTail"] = jsNum(p.cable_tail_depth);
    j["sl_srcDepth"] = jsNum(p.source_depth);
    j["shipXSlider"] = jsNum(std::round(shipX * 10.0) / 10.0);
    j["sl_srcOff"] = jsNum(p.src_offset);
    j["sl_shipSrc"] = jsNum(p.ship_src_dist);
    j["sl_srcRec"] = jsNum(p.src_rec_dist);
    j["sl_shipSpeed"] = jsNum(p.ship_speed_knots);
    j["sl_shot"] = jsNum(p.shot_interval);
    j["sl_geoModel"] = geoName(p.geology);
    j["sl_uiWidth"] = jsNum(p.domain_W);
    j["sl_uiHeight"] = jsNum(p.domain_H);
    j["sl_obsSpace"] = jsNum(p.obs_spacing);
    j["sl_gain"] = jsNum(p.gain);
    j["sl_speed"] = jsNum(std::round(p.speed_ms * 10.0) / 10.0);
    j["sismoTimeSlider"] = std::to_string(p.sismo_ms);
    j["segyLenVal"] = std::to_string(p.sismo_ms);
    j["sl_wavelet"] = p.wavelet == Wavelet::MinPhase ? "minphase" : p.wavelet == Wavelet::Gauss ? "gauss"
                      : p.wavelet == Wavelet::GaussDeriv ? "gaussderiv" : "ricker";   // new in the C++ port
    j["obs_enabled"] = p.obs_enabled;
    return j.dump(2);
}

// Bit test: this library is built with /fp:fast, which may fold std::isfinite away
static bool finiteD(double v) {
    uint64_t b; std::memcpy(&b, &v, 8);
    return (b & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

static bool num(const ordered_json& j, const char* key, double& out) {
    auto it = j.find(key);
    if (it == j.end()) return false;
    try {
        if (it->is_number()) { double v = it->get<double>(); if (!finiteD(v)) return false; out = v; return true; }
        if (it->is_string()) {
            const std::string s = it->get<std::string>();
            char* end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            if (end == s.c_str() || !finiteD(v)) return false;   // "nan"/"inf" would poison every clamp
            out = v;
            return true;
        }
    } catch (...) {}
    return false;
}

bool paramsFromJson(const std::string& text, SimParams& p, double& shipX, std::string& err) {
    ordered_json j;
    try {
        j = ordered_json::parse(text);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    if (!j.is_object()) { err = "JSON nesnesi bekleniyordu"; return false; }
    double v;
    if (num(j, "sl_fdOrder", v)) p.fd_order = (int)v;
    if (num(j, "sl_dh", v)) p.dh = (float)v;
    if (num(j, "sl_dt", v)) p.dt = (float)v;
    if (num(j, "sl_npml", v)) p.npml = (int)v;
    if (num(j, "sl_freq", v)) p.freq = (float)v;
    if (num(j, "sl_amp", v)) p.amp = (float)v;
    if (num(j, "sl_cable", v)) p.cable_len = (float)v;
    if (num(j, "sl_groupSpace", v)) p.receiver_spacing = (float)v;
    if (num(j, "sl_cableHead", v)) p.cable_head_depth = (float)v;
    if (num(j, "sl_cableTail", v)) p.cable_tail_depth = (float)v;
    if (num(j, "sl_srcDepth", v)) p.source_depth = (float)v;
    if (num(j, "shipXSlider", v)) shipX = v;
    if (num(j, "sl_srcOff", v)) p.src_offset = (float)v;
    if (num(j, "sl_shipSrc", v)) p.ship_src_dist = (float)v;
    if (num(j, "sl_srcRec", v)) p.src_rec_dist = (float)v;
    if (num(j, "sl_shipSpeed", v)) p.ship_speed_knots = (float)v;
    if (num(j, "sl_shot", v)) p.shot_interval = (float)v;
    if (j.contains("sl_geoModel") && j["sl_geoModel"].is_string()) {
        std::string g = j["sl_geoModel"].get<std::string>();
        p.geology = g == "flat" ? GeologyModel::Flat : g == "fault" ? GeologyModel::Fault : GeologyModel::Salt;
    }
    if (num(j, "sl_uiWidth", v)) p.domain_W = (float)v;
    if (num(j, "sl_uiHeight", v)) p.domain_H = (float)v;
    if (num(j, "sl_obsSpace", v)) p.obs_spacing = (float)v;
    if (num(j, "sl_gain", v)) p.gain = (float)v;
    if (num(j, "sl_speed", v)) p.speed_ms = (float)v;
    if (num(j, "sismoTimeSlider", v)) p.sismo_ms = (int)v;
    if (num(j, "segyLenVal", v)) p.sismo_ms = (int)v;   // later key wins, as in the browser
    if (j.contains("sl_wavelet") && j["sl_wavelet"].is_string())
    {
        const std::string w = j["sl_wavelet"].get<std::string>();
        p.wavelet = w == "minphase" ? Wavelet::MinPhase : w == "gauss" ? Wavelet::Gauss
                  : w == "gaussderiv" ? Wavelet::GaussDeriv : Wavelet::Ricker;
    }
    if (j.contains("obs_enabled") && j["obs_enabled"].is_boolean()) p.obs_enabled = j["obs_enabled"].get<bool>();
    return true;
}

std::string localParamsPath() {
    // Wide API: getenv() returns the ANSI code page, which breaks (or mangles) profile
    // paths with characters outside it; the rest of the app passes UTF-8 around.
#ifdef _WIN32
    const wchar_t* appdata = _wgetenv(L"APPDATA");
#else
    const char* appdata = std::getenv("HOME");
#endif
    fs::path dir = appdata ? fs::path(appdata) / "SeismicFDTD" : fs::current_path();
    const std::u8string u8 = (dir / "params.json").u8string();
    return std::string(u8.begin(), u8.end());
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeTextFile(const std::string& path, const std::string& text) {
    fs::path p = fs::u8path(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return (bool)f;
}
