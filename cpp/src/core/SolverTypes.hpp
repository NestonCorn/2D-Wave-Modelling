#pragma once
#include <cstdint>
#include <vector>

// Per-step source / receiver data, precomputed on the CPU once per batch and
// consumed by both engines (64 bytes, std430-compatible; mirrors StepData in gpu_engine.js).
struct StepData {
    float si0, sj0, val0, act0;     // main shot cell, wavelet * dt * 0.4, active flag
    float si1, sj1, val1, act1;     // secondary shot (SLO src / CLO second ship)
    float sxShip;                   // ship x (m) after this step's movement
    float gy_pix;                   // sismogram row for this step
    float sismo_off;                // 0 or N_REC*GH (CLO second shot block)
    float rec_valid;                // 1 = record this step
    float ry_head, ry_tail;         // cable head / tail depth (absolute, m)
    float cable_head_x;             // x of the cable head (m)
    float _pad;
};
static_assert(sizeof(StepData) == 64, "StepData must be 64 bytes");

struct BatchJob {
    int steps = 0;
    float dt = 0.5f;
    int fd_order = 10;
    bool freeSurface = true;
    int sj_surf = 0;
    // Expanding box (inclusive bounds)
    int bxmin = 0, bxmax = 0, bymin = 0, bymax = 0;
    const StepData* stepData = nullptr;

    // Receivers
    int N_REC = 0, N_OBS = 0;
    float cable_len = 0, SCALE = 1;
    const float* obsX = nullptr;    // N_OBS receiver x (m)
    const float* obsY = nullptr;    // N_OBS receiver depth (m, already +4)

    // Wavefield colouring (P -> RGBA), done inside the batch
    bool renderPixels = false;
    float gainNorm = 1.0f;          // display_gain / 15
    int drawMinJ = 0, drawMaxJ = -1;      // rows to colour (inclusive)
    int clearMinJ = 0, clearMaxJ = -1;    // rows of the dirty rectangle (zeroed first, inclusive)
    int dirtyX0 = 0, dirtyX1 = -1;        // dirty rectangle columns (inclusive)
    const int* rowMin = nullptr;    // optional per-row column limits (xMinZ/xMaxZ)
    const int* rowMax = nullptr;
};

struct ThreadStat { double compute = 0, wait = 0; };
