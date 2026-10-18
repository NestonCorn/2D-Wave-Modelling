#pragma once
// GLSL 4.30 compute port of WGSL_FDTD (gpu_engine.js).
// FD_ORDER is injected as a #define so each program is specialised at compile time.

namespace shaders {

inline const char* kHeader = R"GLSL(
layout(std430, binding = 0) buffer B0 { float P[]; };
layout(std430, binding = 1) buffer B1 { float Vx[]; };
layout(std430, binding = 2) buffer B2 { float Vz[]; };
layout(std430, binding = 3) readonly buffer B3 { float Vsq[]; };
layout(std430, binding = 4) buffer B4 { float psiVx[]; };
layout(std430, binding = 5) buffer B5 { float psiVz[]; };
layout(std430, binding = 6) buffer B6 { float psiPx[]; };
layout(std430, binding = 7) buffer B7 { float psiPz[]; };
layout(std430, binding = 8) readonly buffer B8 { float cpml[]; };   // bx[W] | ax[W] | bz[H] | az[H]
layout(std430, binding = 9) buffer B9 { float sismo[]; };
struct StepData {
    float si0, sj0, val0, act0;
    float si1, sj1, val1, act1;
    float sxShip, gy_pix, sismo_off, rec_valid;
    float ry_head, ry_tail, cable_head_x, pad;
};
layout(std430, binding = 10) readonly buffer B10 { StepData steps[]; };
layout(std430, binding = 11) readonly buffer B11 { vec2 obsPos[]; };  // OBS x, depth (m)

layout(std140, binding = 0) uniform Params {
    int W; int H; int boxX0; int boxY0;
    int boxX1; int boxY1; int sjSurf; int nRec;
    int nObs; int freeSurf; float dt; float scale;
    float cable; float gain; int GH; int _pad;
};
uniform int uStep;

#if FD_ORDER == 2
const int H2 = 1; const float C[1] = float[1](1.0);
#elif FD_ORDER == 4
const int H2 = 2; const float C[2] = float[2](9.0/8.0, -1.0/24.0);
#elif FD_ORDER == 6
const int H2 = 3; const float C[3] = float[3](75.0/64.0, -25.0/384.0, 3.0/640.0);
#elif FD_ORDER == 8
const int H2 = 4; const float C[4] = float[4](1225.0/1024.0, -245.0/3072.0, 49.0/5120.0, -5.0/7168.0);
#else
const int H2 = 5; const float C[5] = float[5](19845.0/16384.0, -735.0/8192.0, 567.0/40960.0, -405.0/229376.0, 35.0/294912.0);
#endif
)GLSL";

// Phase 1: velocity update + CPML psi (same index limits as the CPU kernel)
inline const char* kVelocity = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i < H2 - 1 || i > min(W - H2 - 1, boxX1)) return;
    if (j < H2 - 1 || j >= min(boxY1, H - H2)) return;
    int idx = j * W + i;
    float dPx = P[idx + 1] - P[idx];
    float dPz = P[idx + W] - P[idx];
    float px = cpml[i] * psiVx[idx] + cpml[W + i] * dPx;
    float pz = cpml[2 * W + j] * psiVz[idx] + cpml[2 * W + H + j] * dPz;
    psiVx[idx] = px; psiVz[idx] = pz;
    float sDx = C[0] * dPx, sDz = C[0] * dPz;
    for (int k = 1; k < H2; k++) {
        sDx += C[k] * (P[idx + k + 1] - P[idx - k]);
        sDz += C[k] * (P[idx + (k + 1) * W] - P[idx - k * W]);
    }
    Vx[idx] -= dt * (sDx + px);
    Vz[idx] -= dt * (sDz + pz);
}
)GLSL";

// Phase 2: pressure update + CPML psi
inline const char* kPressure = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i < H2 || i > min(W - H2 - 1, boxX1)) return;
    if (j < H2 || j >= min(boxY1, H - H2)) return;
    int idx = j * W + i;
    float dVx1 = Vx[idx] - Vx[idx - 1];
    float dVz1 = Vz[idx] - Vz[idx - W];
    float px = cpml[i] * psiPx[idx] + cpml[W + i] * dVx1;
    float pz = cpml[2 * W + j] * psiPz[idx] + cpml[2 * W + H + j] * dVz1;
    psiPx[idx] = px; psiPz[idx] = pz;
    float dVx = C[0] * dVx1, dVz = C[0] * dVz1;
    for (int k = 1; k < H2; k++) {
        dVx += C[k] * (Vx[idx + k] - Vx[idx - k - 1]);
        dVz += C[k] * (Vz[idx + k * W] - Vz[idx - (k + 1) * W]);
    }
    P[idx] -= dt * Vsq[idx] * (dVx + px + dVz + pz);
}
)GLSL";

// Phase 3a: Dirichlet free surface (rows from the box top through the sea surface)
inline const char* kFreeSurface = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (freeSurf == 0 || i > min(W - 1, boxX1)) return;
    if (j >= min(boxY1, sjSurf + 1)) return;
    P[j * W + i] = 0.0;
}
)GLSL";

// Phase 3b: source injection over the 7x7 Gaussian footprint (radius 3).
// A single invocation walks both shots so overlapping footprints never race
// (the WGSL version used 49 threads, which could collide when ships overlap).
inline const char* kSource = R"GLSL(
layout(local_size_x = 1) in;
void inject(float sif, float sjf, float val) {
    int si0 = int(sif), sj0 = int(sjf);
    for (int dj = -3; dj <= 3; dj++)
        for (int di = -3; di <= 3; di++) {
            int dsq = di * di + dj * dj;
            if (dsq > 9) continue;
            int si = si0 + di, sj = sj0 + dj;
            if (si > 1 && si < W - 1 && sj > 1 && sj < H - 1)
                P[sj * W + si] += val * exp(-float(dsq) / 2.0);
        }
}
void main() {
    StepData s = steps[uStep];
    if (s.act0 > 0.5) inject(s.si0, s.sj0, s.val0);
    if (s.act1 > 0.5) inject(s.si1, s.sj1, s.val1);
}
)GLSL";

// Phase 3c: sismogram recording (streamer receivers, then OBS)
inline const char* kSismo = R"GLSL(
layout(local_size_x = 128) in;
void main() {
    int r = int(gl_GlobalInvocationID.x);
    StepData s = steps[uStep];
    if (r >= nRec + nObs || s.rec_valid < 0.5) return;
    int gy = int(s.gy_pix);
    if (gy < 0 || gy >= GH) return;
    float rx, ry; int base;
    if (r < nRec) {
        float spacing = nRec > 1 ? cable / float(nRec - 1) : 0.0;
        float norm = nRec > 1 ? float(r) / float(nRec - 1) : 0.0;
        rx = s.cable_head_x - cable + float(r) * spacing;
        ry = s.ry_tail + (s.ry_head - s.ry_tail) * norm;
        base = int(s.sismo_off) + r * GH;
    } else {
        int o = r - nRec;
        rx = obsPos[o].x; ry = obsPos[o].y;
        base = nRec * 2 * GH + o * GH;
    }
    int si = int(floor(rx / scale)), sj = int(floor(ry / scale));
    float amp = (si > 0 && si < W && sj > 0 && sj < H) ? P[sj * W + si] : 0.0;
    int idx = base + gy;
    if (abs(amp) > abs(sismo[idx])) sismo[idx] = amp;
}
)GLSL";

// Wavefield colouring straight into the display texture (no CPU readback)
inline const char* kPixels = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) writeonly uniform image2D waveImg;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i >= W || j >= H || i > boxX1 || j > boxY1) return;
    if (j < sjSurf) { imageStore(waveImg, ivec2(i, j), vec4(0.0)); return; }
    float v = P[j * W + i] * gain;
    float inten = abs(v);
    if (inten < 0.001) { imageStore(waveImg, ivec2(i, j), vec4(0.0)); return; }
    float a = floor(clamp(inten * 700.0, 0.0, 255.0)) / 255.0;
    float c = v > 0.0 ? 1.0 : 0.0;
    imageStore(waveImg, ivec2(i, j), vec4(c, c, c, a));
}
)GLSL";

} // namespace shaders
