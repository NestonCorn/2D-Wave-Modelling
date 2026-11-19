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
    float ry_head, ry_tail, cable_head_x, rec_w;
};
layout(std430, binding = 10) readonly buffer B10 { StepData steps[]; };
layout(std430, binding = 11) readonly buffer B11 { vec2 obsPos[]; };  // OBS x, depth (m)
layout(std430, binding = 12) readonly buffer B12 { float recvK[]; };  // recvKernelTable()

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
layout(local_size_x = TILE_X, local_size_y = TILE_Y) in;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i < H2 - 1 || i > min(W - H2 - 1, boxX1)) return;
    if (j < H2 - 1 || j >= min(boxY1, H - H2)) return;
    int idx = j * W + i;
    float dPx = P[idx + 1] - P[idx];
    float dPz = P[idx + W] - P[idx];
    // CPML memory only inside the absorbing layers (a = 0 elsewhere, where psi stays 0):
    // skipping the 4 psi loads/stores per cell halves the interior memory traffic
    float px = 0.0, pz = 0.0;
    float ax = cpml[W + i], az = cpml[2 * W + H + j];
    if (ax != 0.0) { px = cpml[i] * psiVx[idx] + ax * dPx; psiVx[idx] = px; }
    if (az != 0.0) { pz = cpml[2 * W + j] * psiVz[idx] + az * dPz; psiVz[idx] = pz; }
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
layout(local_size_x = TILE_X, local_size_y = TILE_Y) in;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i < H2 || i > min(W - H2 - 1, boxX1)) return;
    if (j < H2 || j >= min(boxY1, H - H2)) return;
    int idx = j * W + i;
    // Dirichlet free surface folded in (was a separate dispatch + barrier every step): rows at
    // and above the sea surface are held at 0. Their CPML memory only ever fed those rows.
    if (freeSurf != 0 && j <= sjSurf) { P[idx] = 0.0; return; }
    float dVx1 = Vx[idx] - Vx[idx - 1];
    float dVz1 = Vz[idx] - Vz[idx - W];
    float px = 0.0, pz = 0.0;
    float ax = cpml[W + i], az = cpml[2 * W + H + j];
    if (ax != 0.0) { px = cpml[i] * psiPx[idx] + ax * dVx1; psiPx[idx] = px; }
    if (az != 0.0) { pz = cpml[2 * W + j] * psiPz[idx] + az * dVz1; psiPz[idx] = pz; }
    float dVx = C[0] * dVx1, dVz = C[0] * dVz1;
    for (int k = 1; k < H2; k++) {
        dVx += C[k] * (Vx[idx + k] - Vx[idx - k - 1]);
        dVz += C[k] * (Vz[idx + k * W] - Vz[idx - (k + 1) * W]);
    }
    P[idx] -= dt * Vsq[idx] * (dVx + px + dVz + pz);
}
)GLSL";

// Phase 3b: point-source injection (see CpuSolver::serialPhase). A single invocation
// handles both shots so coinciding sources never race.
inline const char* kSource = R"GLSL(
layout(local_size_x = 1) in;
void inject(float sif, float sjf, float val) {
    int si = int(sif), sj = int(sjf);
    if (si > 1 && si < W - 1 && sj > 1 && sj < H - 1 && (freeSurf == 0 || sj > sjSurf))
        P[sj * W + si] += val * 6.21336;   // kSourceGain
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
float node(int i, int j) { return (i > 0 && i < W && j > 0 && j < H) ? P[j * W + i] : 0.0; }
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
    // Kaiser-windowed sinc along x (recvKernel in SolverTypes.hpp, tabulated), linear along z
    // (see CpuSolver::serialRows)
    float fx = rx / scale, fy = ry / scale;
    float x0 = floor(fx), y0 = floor(fy);
    int i0 = int(x0), j0 = int(y0);
    float t = fx - x0, wy = fy - y0;
    float top = 0.0, bot = 0.0, sum = 0.0;
    for (int k = 0; k < 2 * RECV_HALF; k++) {
        float x = (t - float(k - RECV_HALF + 1) + float(RECV_HALF)) * float(RECV_RES);
        int q = min(int(x), 2 * RECV_HALF * RECV_RES - 1);
        float v = recvK[q] + (x - float(q)) * (recvK[q + 1] - recvK[q]);
        sum += v;
        top += v * node(i0 - RECV_HALF + 1 + k, j0);
        bot += v * node(i0 - RECV_HALF + 1 + k, j0 + 1);
    }
    top /= sum; bot /= sum;
    float amp = top + wy * (bot - top);
    int idx = base + gy;
    sismo[idx] += amp * (s.rec_w > 0.0 ? s.rec_w : 1.0);   // mean over the row's steps
}
)GLSL";

// Wavefield colouring straight into the display texture (no CPU readback)
inline const char* kPixels = R"GLSL(
layout(local_size_x = TILE_X, local_size_y = TILE_Y) in;
layout(rgba8, binding = 0) writeonly uniform image2D waveImg;
void main() {
    int i = int(gl_GlobalInvocationID.x) + max(0, boxX0);
    int j = int(gl_GlobalInvocationID.y) + max(0, boxY0);
    if (i >= W || j >= H || i > boxX1 || j > boxY1) return;
    if (freeSurf != 0 && j < sjSurf) { imageStore(waveImg, ivec2(i, j), vec4(0.0)); return; }   // air above a free surface
    float v = P[j * W + i] * gain;
    float inten = abs(v);
    if (inten < 0.001) { imageStore(waveImg, ivec2(i, j), vec4(0.0)); return; }
    float a = floor(clamp(inten * 700.0, 0.0, 255.0)) / 255.0;
    float c = v > 0.0 ? 1.0 : 0.0;
    imageStore(waveImg, ivec2(i, j), vec4(c, c, c, a));
}
)GLSL";

} // namespace shaders
