#pragma once
// Every user-facing setting of the original SLO_CLO_Interactive.html,
// with the same defaults (ranges live next to the widgets in ui/App.cpp).

enum class Mode { None, CLO, SLO, Trad };
enum class GeologyModel { Flat = 0, Fault = 1, Salt = 2, Imported = 3 };   // Imported: loaded from files
enum class Wavelet { Ricker = 0, MinPhase = 1, Gauss = 2, GaussDeriv = 3 };

// Sismogram panel size in logical pixels (GW x GH in the HTML)
constexpr int GW = 598;
constexpr int GH = 550;
// Samples stored per sismogram trace (the stride of every trace in the sismo buffers). It
// used to be GH, one sample per panel pixel: a 4000 ms record then had 7.3 ms samples, and a
// 50 Hz wavelet (main lobe ~10 ms) was caught at a different phase on every trace - the
// direct arrival changed shape and amplitude from trace to trace. A record uses
// Simulation::sismoSamples() of them, each the mean of the same number of time steps.
constexpr int kSismoStride = 4096;
constexpr int MAX_STEPS = 2048;   // GPU step-data buffer size / batch cap

struct SimParams {
    // ── Fiziksel & Sinyal ──
    int     fd_order = 10;           // sl_fdOrder 2/4/6/8/10
    float   dh = 4.0f;               // sl_dh  0.5..8   (grid spacing, m)
    float   dt = 0.5f;               // sl_dt  0.1..1.5 (time coefficient)
    int     npml = 40;               // sl_npml 5..60
    Wavelet wavelet = Wavelet::Ricker;
    float   freq = 50.0f;            // sl_freq 0.5..250 Hz
    float   amp = 2.0f;              // sl_amp 0.2..20

    // ── Saha Geometrisi ──
    float cable_len = 350.0f;        // sl_cable 50..1500
    float receiver_spacing = 6.25f;  // sl_groupSpace
    float cable_head_depth = 10.0f;  // sl_cableHead 2..100
    float cable_tail_depth = 10.0f;  // sl_cableTail 2..250
    float source_depth = 8.0f;       // sl_srcDepth 2..50
    float src_offset = 350.0f;       // sl_srcOff 0..800
    float ship_src_dist = 50.0f;     // sl_shipSrc 0..300
    float src_rec_dist = 100.0f;     // sl_srcRec 0..500
    float ship_speed_knots = 4.5f;   // sl_shipSpeed 0..12
    float shot_interval = 25.0f;     // sl_shot 6.25..1000 (also the line simulation's shot spacing)

    // ── Jeoloji & OBS ──
    GeologyModel geology = GeologyModel::Salt;
    float domain_W = 1200.0f;        // sl_uiWidth 800..25000
    float domain_H = 480.0f;         // sl_uiHeight 400..12000
    float obs_spacing = 50.0f;       // sl_obsSpace 50..500
    bool  obs_enabled = true;

    // ── Görsel ──
    float gain = 15.0f;              // sl_gain 1..100
    float speed_ms = 16.666f;        // sl_speed 1..250 (simulated ms per frame)
    int   sismo_ms = 4000;           // sismoTimeSlider / segyLenVal (synced on load)

    // ── Toolbar toggles ──
    bool free_surface = true;
    bool expanding_box = true;
    int model_colormap = 0;          // ModelColormap of the model view (0 Ocean)
    bool show_colorbar = true;       // velocity colour bar on the model view
    bool skip_gap = true;            // live: once the record is complete, glide the ship to the next shot
    bool cell_average = true;        // each grid cell takes the mean of the model over its area (else the value at the node)
    bool smooth_boundaries = true;   // piecewise-constant loaded models (images, drawings): boundaries without pixel steps
    bool sismo_only = false;         // "Sadece Sismogram (Turbo)"
};

inline const char* modeName(Mode m) {
    switch (m) {
        case Mode::CLO: return "clo";
        case Mode::SLO: return "slo";
        case Mode::Trad: return "trad";
        default: return "none";
    }
}
