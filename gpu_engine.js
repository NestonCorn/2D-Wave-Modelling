// ===========================================================================
// gpu_engine.js - WebGPU 2D Acoustic FDTD + CPML Engine
// Komatitsch & Martin (2007) CPML - all stencil orders O2-O10 in WGSL
// AMD / NVIDIA compatible via DirectX12 / Vulkan backend (Chrome/Edge 113+)
// ===========================================================================
'use strict';

// --- WGSL Master Shader ------------------------------------------------------
// Uses `override FD_ORDER` pipeline constant so each compiled pipeline variant
// is fully dead-code-eliminated by the GPU shader compiler. Zero branch cost.
const WGSL_FDTD = /* wgsl */`
// --- Uniform / storage layouts ------------------------------------------------
struct Params {
    W_sim     : u32,   // 0
    H_sim     : u32,   // 4
    dt        : f32,   // 8
    sj_surf   : u32,   // 12
    SCALE     : f32,   // 16
    N_REC     : u32,   // 20
    GH        : u32,   // 24
    gain      : f32,   // 28
    sj_rec    : i32,   // 32
    CABLE_LEN : f32,   // 36
    N_OBS     : u32,   // 40
    obs_spacing : f32, // 44
    domain_W  : f32,   // 48  physical width in metres
    domain_H  : f32,   // 52  physical height in metres
    use_free_surface : f32, // 56
    geology_mode : u32,     // 60  0=salt, 1=flat, 2=fault
};

struct StepData {          // 64 bytes, padded to 256 in buffer (dynamic offset)
    si0       : f32,   // main shot x grid cell
    sj0       : f32,   // main shot z grid cell
    val0      : f32,   // main Ricker * dt * 0.4 (precomputed on CPU)
    act0      : f32,   // 1 = active, 0 = inactive
    si1       : f32,   // secondary shot x
    sj1       : f32,   // secondary shot z
    val1      : f32,
    act1      : f32,
    sxShip    : f32,   // ship pixel x for sismo receiver positions
    gy_pix    : f32,   // sismogram y pixel index for this step
    sismo_off : f32,   // element offset in sismo buffer (0 or N_REC*GH)
    rec_valid : f32,   // 1 = record this step, 0 = skip
    ry_head      : f32,
    ry_tail      : f32,
    cable_head_x : f32,
    box_xmin     : f32,
    box_ymin     : f32,
    _pad         : f32,
};

// Group 0: wavefield + CPML + output
@group(0) @binding(0)  var<storage, read_write> P        : array<f32>;
@group(0) @binding(1)  var<storage, read_write> Vx       : array<f32>;
@group(0) @binding(2)  var<storage, read_write> Vz       : array<f32>;
@group(0) @binding(3)  var<storage, read>       V_sq     : array<f32>;
@group(0) @binding(4)  var<storage, read_write> psi_VxX  : array<f32>;
@group(0) @binding(5)  var<storage, read_write> psi_VzZ  : array<f32>;
@group(0) @binding(6)  var<storage, read_write> psi_PxX  : array<f32>;
@group(0) @binding(7)  var<storage, read_write> psi_PzZ  : array<f32>;
@group(0) @binding(8)  var<storage, read>       b_x      : array<f32>;
@group(0) @binding(9)  var<storage, read>       a_x      : array<f32>;
@group(0) @binding(10) var<storage, read>        b_z      : array<f32>;
@group(0) @binding(11) var<storage, read>        a_z      : array<f32>;
@group(0) @binding(12) var<storage, read_write> sismo    : array<f32>;
@group(0) @binding(13) var<storage, read_write> pixels   : array<u32>;

// Group 1: fixed params uniform
@group(1) @binding(0) var<uniform> params : Params;

// Group 2: per-step data (dynamic offset into a large buffer)
@group(2) @binding(0) var<uniform> step : StepData;

// Pipeline-override constant: eliminated at compile time per variant
override FD_ORDER : i32 = 2;

// --- Helper: high-order x-derivative of P at cell (i,j) ---------------------
fn dPdx(idx: i32) -> f32 {
    let d1 = P[idx+1] - P[idx];
    if (FD_ORDER < 4)  { return d1; }
    let c1_4=9.0/8.0;      let c2_4=1.0/24.0;
    if (FD_ORDER == 4) { return c1_4*d1 - c2_4*(P[idx+2]-P[idx-1]); }
    let c1_6=75.0/64.0;    let c2_6=-25.0/384.0;  let c3_6=3.0/640.0;
    if (FD_ORDER == 6) { return c1_6*d1 + c2_6*(P[idx+2]-P[idx-1]) + c3_6*(P[idx+3]-P[idx-2]); }
    let c1_8=1225.0/1024.0; let c2_8=-245.0/3072.0; let c3_8=49.0/5120.0; let c4_8=-5.0/7168.0;
    if (FD_ORDER == 8) { return c1_8*d1 + c2_8*(P[idx+2]-P[idx-1]) + c3_8*(P[idx+3]-P[idx-2]) + c4_8*(P[idx+4]-P[idx-3]); }
    let c1=19845.0/16384.0; let c2=-735.0/8192.0; let c3=567.0/40960.0; let c4=-405.0/229376.0; let c5=35.0/294912.0;
    return c1*d1 + c2*(P[idx+2]-P[idx-1]) + c3*(P[idx+3]-P[idx-2]) + c4*(P[idx+4]-P[idx-3]) + c5*(P[idx+5]-P[idx-4]);
}

fn dPdz(idx: i32, W: i32) -> f32 {
    let d1 = P[idx+W] - P[idx];
    if (FD_ORDER < 4)  { return d1; }
    let c1_4=9.0/8.0;      let c2_4=1.0/24.0;
    if (FD_ORDER == 4) { return c1_4*d1 - c2_4*(P[idx+2*W]-P[idx-W]); }
    let c1_6=75.0/64.0;    let c2_6=-25.0/384.0;  let c3_6=3.0/640.0;
    if (FD_ORDER == 6) { return c1_6*d1 + c2_6*(P[idx+2*W]-P[idx-W]) + c3_6*(P[idx+3*W]-P[idx-2*W]); }
    let c1_8=1225.0/1024.0; let c2_8=-245.0/3072.0; let c3_8=49.0/5120.0; let c4_8=-5.0/7168.0;
    if (FD_ORDER == 8) { return c1_8*d1 + c2_8*(P[idx+2*W]-P[idx-W]) + c3_8*(P[idx+3*W]-P[idx-2*W]) + c4_8*(P[idx+4*W]-P[idx-3*W]); }
    let c1=19845.0/16384.0; let c2=-735.0/8192.0; let c3=567.0/40960.0; let c4=-405.0/229376.0; let c5=35.0/294912.0;
    return c1*d1 + c2*(P[idx+2*W]-P[idx-W]) + c3*(P[idx+3*W]-P[idx-2*W]) + c4*(P[idx+4*W]-P[idx-3*W]) + c5*(P[idx+5*W]-P[idx-4*W]);
}

fn dVxdx(idx: i32) -> f32 {
    let d1 = Vx[idx] - Vx[idx-1];
    if (FD_ORDER < 4)  { return d1; }
    let c1_4=9.0/8.0;      let c2_4=1.0/24.0;
    if (FD_ORDER == 4) { return c1_4*d1 - c2_4*(Vx[idx+1]-Vx[idx-2]); }
    let c1_6=75.0/64.0;    let c2_6=-25.0/384.0;  let c3_6=3.0/640.0;
    if (FD_ORDER == 6) { return c1_6*d1 + c2_6*(Vx[idx+1]-Vx[idx-2]) + c3_6*(Vx[idx+2]-Vx[idx-3]); }
    let c1_8=1225.0/1024.0; let c2_8=-245.0/3072.0; let c3_8=49.0/5120.0; let c4_8=-5.0/7168.0;
    if (FD_ORDER == 8) { return c1_8*d1 + c2_8*(Vx[idx+1]-Vx[idx-2]) + c3_8*(Vx[idx+2]-Vx[idx-3]) + c4_8*(Vx[idx+3]-Vx[idx-4]); }
    let c1=19845.0/16384.0; let c2=-735.0/8192.0; let c3=567.0/40960.0; let c4=-405.0/229376.0; let c5=35.0/294912.0;
    return c1*d1 + c2*(Vx[idx+1]-Vx[idx-2]) + c3*(Vx[idx+2]-Vx[idx-3]) + c4*(Vx[idx+3]-Vx[idx-4]) + c5*(Vx[idx+4]-Vx[idx-5]);
}

fn dVzdz(idx: i32, W: i32) -> f32 {
    let d1 = Vz[idx] - Vz[idx-W];
    if (FD_ORDER < 4)  { return d1; }
    let c1_4=9.0/8.0;      let c2_4=1.0/24.0;
    if (FD_ORDER == 4) { return c1_4*d1 - c2_4*(Vz[idx+W]-Vz[idx-2*W]); }
    let c1_6=75.0/64.0;    let c2_6=-25.0/384.0;  let c3_6=3.0/640.0;
    if (FD_ORDER == 6) { return c1_6*d1 + c2_6*(Vz[idx+W]-Vz[idx-2*W]) + c3_6*(Vz[idx+2*W]-Vz[idx-3*W]); }
    let c1_8=1225.0/1024.0; let c2_8=-245.0/3072.0; let c3_8=49.0/5120.0; let c4_8=-5.0/7168.0;
    if (FD_ORDER == 8) { return c1_8*d1 + c2_8*(Vz[idx+W]-Vz[idx-2*W]) + c3_8*(Vz[idx+2*W]-Vz[idx-3*W]) + c4_8*(Vz[idx+3*W]-Vz[idx-4*W]); }
    let c1=19845.0/16384.0; let c2=-735.0/8192.0; let c3=567.0/40960.0; let c4=-405.0/229376.0; let c5=35.0/294912.0;
    return c1*d1 + c2*(Vz[idx+W]-Vz[idx-2*W]) + c3*(Vz[idx+2*W]-Vz[idx-3*W]) + c4*(Vz[idx+3*W]-Vz[idx-4*W]) + c5*(Vz[idx+4*W]-Vz[idx-5*W]);
}

// --- Phase 1: Velocity Update + CPML psi_Vx, psi_Vz --------------------------
@compute @workgroup_size(16, 16)
fn velocity_update(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = i32(gid.x) + i32(step.box_xmin);
    let j = i32(gid.y) + i32(step.box_ymin);
    let W = i32(params.W_sim);
    let H = i32(params.H_sim);

    // Guard band: velocity stencil reads P[idx+FD_ORDER/2] → margin FD_ORDER/2-1
    let g = max(0, FD_ORDER / 2 - 1);
    if (i < g || i > W - g - 2 || j < g || j > H - g - 2) { return; }

    let idx = j * W + i;

    // 1st-order FD for CPML auxiliary variable update (Komatitsch & Martin 2007)
    let dPx1 = P[idx + 1] - P[idx];
    let dPz1 = P[idx + W]  - P[idx];

    psi_VxX[idx] = b_x[i] * psi_VxX[idx] + a_x[i] * dPx1;
    psi_VzZ[idx] = b_z[j] * psi_VzZ[idx] + a_z[j] * dPz1;

    // High-order FD for wavefield with CPML correction
    Vx[idx] -= params.dt * (dPdx(idx)      + psi_VxX[idx]);
    Vz[idx] -= params.dt * (dPdz(idx, W)   + psi_VzZ[idx]);
}

// --- Phase 2: Pressure Update + CPML psi_P ----------------------------------──
@compute @workgroup_size(16, 16)
fn pressure_update(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = i32(gid.x) + i32(step.box_xmin);
    let j = i32(gid.y) + i32(step.box_ymin);
    let W = i32(params.W_sim);
    let H = i32(params.H_sim);

    // Pressure guard band: stencil reads Vx[idx-FD_ORDER/2+1] → margin FD_ORDER/2
    let gp = FD_ORDER / 2;
    if (i < gp || i > W - gp - 1 || j < gp || j > H - gp - 1) { return; }

    let idx = j * W + i;

    let dVx1 = Vx[idx] - Vx[idx - 1];
    let dVz1 = Vz[idx] - Vz[idx - W];

    psi_PxX[idx] = b_x[i] * psi_PxX[idx] + a_x[i] * dVx1;
    psi_PzZ[idx] = b_z[j] * psi_PzZ[idx] + a_z[j] * dVz1;

    P[idx] -= params.dt * V_sq[idx] * (dVxdx(idx) + psi_PxX[idx] + dVzdz(idx, W) + psi_PzZ[idx]);
}

// --- Phase 3a: Free Surface Dirichlet BC (ocean top) ------------------------──
@compute @workgroup_size(16, 16)
fn free_surface(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x + u32(step.box_xmin);
    let j = gid.y + u32(step.box_ymin);
    let W = params.W_sim;
    if (params.use_free_surface > 0.5 && i < W && j <= params.sj_surf) { 
        P[j * W + i] = 0.0; 
    }
}

// --- Phase 3b: Source Injection (Ricker precomputed on CPU) -----------------
// 64-thread dispatch(1,1,1): threads 0-48 cover the 7x7 source footprint
@compute @workgroup_size(64)
fn source_inject(@builtin(global_invocation_id) gid: vec3<u32>) {
    let tid = i32(gid.x);
    if (tid >= 49) { return; }          // 7x7 = 49 active threads

    let di = tid % 7 - 3;               // di in [-3, +3]
    let dj = tid / 7 - 3;               // dj in [-3, +3]
    let d_sq = di * di + dj * dj;
    if (d_sq > 9) { return; }           // r_s = 3, r_s^2 = 9

    let gauss = exp(-f32(d_sq) / 2.0);
    let W = i32(params.W_sim);
    let H = i32(params.H_sim);

    // Main shot
    if (step.act0 > 0.5) {
        let si = i32(step.si0) + di;
        let sj = i32(step.sj0) + dj;
        if (si > 1 && si < W - 1 && sj > 1 && sj < H - 1) {
            P[sj * W + si] += step.val0 * gauss;
        }
    }
    // Secondary shot (SLO src / CLO second)
    if (step.act1 > 0.5) {
        let si = i32(step.si1) + di;
        let sj = i32(step.sj1) + dj;
        if (si > 1 && si < W - 1 && sj > 1 && sj < H - 1) {
            P[sj * W + si] += step.val1 * gauss;
        }
    }
}

// --- Phase 3c: Sismogram Recording ------------------------------------------──
@compute @workgroup_size(128)
fn sismo_record(@builtin(global_invocation_id) gid: vec3<u32>) {
    let rec_i = gid.x;
    let total_rec = params.N_REC + params.N_OBS;
    if (rec_i >= total_rec || step.rec_valid < 0.5) { return; }

    var rx: f32;
    var ry: f32;
    var sismo_idx_base: i32;

    if (rec_i < params.N_REC) {
        // Streamer (Towed)
        rx = step.cable_head_x - params.CABLE_LEN + f32(rec_i) * (params.CABLE_LEN / f32(params.N_REC - 1u));
        let norm = f32(rec_i) / f32(params.N_REC - 1u);
        ry = step.ry_tail + (step.ry_head - step.ry_tail) * norm;
        sismo_idx_base = i32(step.sismo_off) + i32(rec_i) * i32(params.GH);
    } else {
        // OBS (Stationary on Seafloor) - position depends on geology model
        let obs_idx = rec_i - params.N_REC;
        rx = f32(obs_idx + 1u) * params.obs_spacing;
        // Match CPU worker.js initSubsurface geometry per geology model
        if (params.geology_mode == 1u) {
            // flat
            ry = params.domain_H * 0.52;
        } else if (params.geology_mode == 2u) {
            // fault
            let faultPos = params.domain_W / 2.0;
            let throwAmt = params.domain_H * 0.31;
            if (rx < faultPos) {
                ry = params.domain_H * 0.45;
            } else {
                ry = params.domain_H * 0.45 + throwAmt;
            }
            ry += (rx / params.domain_W) * (params.domain_H * 0.1);
        } else {
            // salt dome (default)
            ry = params.domain_H * 0.45 + (rx / params.domain_W) * (params.domain_H * 0.25);
        }
        ry += 4.0;
        sismo_idx_base = i32(params.N_REC * 2u * params.GH) + i32(obs_idx) * i32(params.GH);
    }

    let si  = i32(rx / params.SCALE);
    let sj  = i32(ry / params.SCALE);
    let W   = i32(params.W_sim);
    let H   = i32(params.H_sim);
    
    var amp  = 0.0;
    if (si > 0 && si < W && sj > 0 && sj < H) { 
        amp = P[sj * W + si]; 
    }

    let gy  = i32(step.gy_pix);
    let GH  = i32(params.GH);
    let idx = sismo_idx_base + gy;
    
    if (gy >= 0 && gy < GH && abs(amp) > abs(sismo[idx])) {
        sismo[idx] = amp;
    }
}

// --- Pixel Conversion: P -> packed RGBA u32 (for canvas ImageData) ------------
// Black = negative phase, White = positive phase, alpha = intensity
@compute @workgroup_size(16, 16)
fn pixels_convert(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i   = gid.x + u32(step.box_xmin);
    let j   = gid.y + u32(step.box_ymin);
    let W   = u32(params.W_sim);
    let H   = u32(params.H_sim);
    if (i >= W || j >= H) { return; }
    if (j < params.sj_surf) { pixels[j * W + i] = 0u; return; }

    let idx       = j * W + i;
    let val       = P[idx] * params.gain;
    
    // Intensity exactly matches CPU colormap logic: abs(val)
    let intensity = abs(val);

    // Noise cutoff logic matched from CPU: if (intensity < 0.001) a = 0
    if (intensity < 0.001) { pixels[idx] = 0u; return; }

    let a = u32(clamp(intensity * 700.0, 0.0, 255.0));
    
    // Little-endian RGBA: byte0=R, byte1=G, byte2=B, byte3=A
    if (val > 0.0) {
        pixels[idx] = 255u | (255u << 8u) | (255u << 16u) | (a << 24u); // White
    } else {
        pixels[idx] = a << 24u;                                           // Black (RGB=0)
    }
}

`;

// ─── GPUEngine class ─────────────────────────────────────────────────────────
class GPUEngine {
    constructor() {
        this.adapter        = null;
        this.device         = null;
        this.ready          = false;
        this._pipelineCache = {};   // keyed by fd_order
        this._activeFDOrder = null;
        this._pipelines     = null;
        this._dt            = 0;
        this._gain          = 15;
        this.gpuLabel       = 'GPU';
    }

    // ── Public: check WebGPU availability ──────────────────────────────────
    static async isAvailable() {
        if (!navigator.gpu) return false;
        try {
            const a = await navigator.gpu.requestAdapter();
            return !!a;
        } catch { return false; }
    }

    // ── Public: full initialization ────────────────────────────────────────
    async init({ W_sim, H_sim, SCALE, dt, fd_order,
                 V_sq_cpu, b_x_cpu, a_x_cpu, b_z_cpu, a_z_cpu,
                 N_REC, GH, OCEAN_Y, CABLE_LEN, N_OBS, obs_spacing }) {

        this.W_sim = W_sim; this.H_sim = H_sim; this.SCALE = SCALE;
        this.N_REC = N_REC; this.GH    = GH;
        this.N_OBS = N_OBS || 20; this.obs_spacing = obs_spacing || 200;
        this.OCEAN_Y = OCEAN_Y; this.CABLE_LEN = CABLE_LEN;
        this.numCells = W_sim * H_sim;

        if (!navigator.gpu) throw new Error('WebGPU not supported in this browser');

        this.adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
        if (!this.adapter) throw new Error('No WebGPU adapter found');

        let infoDesc = 'WebGPU API';
        try {
            // Modern Chrome exposes synchronously, older Edge/Chrome use async
            const info = this.adapter.info || (this.adapter.requestAdapterInfo ? await this.adapter.requestAdapterInfo() : {});
            const parts = [];
            if (info.vendor) parts.push(info.vendor.toUpperCase());
            if (info.architecture) parts.push(info.architecture);
            if (info.device) parts.push(info.device);
            if (info.description) parts.push(info.description);
            // Deduplicate and filter empty
            const uniqueParts = [...new Set(parts.filter(p => p))];
            if (uniqueParts.length > 0) {
                // Return something like "NVIDIA AMPERE" or "INTEL GEN12" or specific description
                infoDesc = uniqueParts.join(' ');
                // Sometimes description alone is very long, but if so, it'll contain the others.
                if (info.description && info.description.length > 5) {
                    infoDesc = info.description; 
                }
            }
        } catch (e) {}
        this.gpuLabel = infoDesc;
        this.device = await this.adapter.requestDevice({
            requiredLimits: {
                // We use 14 storage buffer bindings (12 FDTD + sismo + pixels)
                // Adapter reports 16 max, so request 14 explicitly
                maxStorageBuffersPerShaderStage: 14,
                maxStorageBufferBindingSize: Math.min(
                    this.adapter.limits.maxStorageBufferBindingSize,
                    1 << 30   // 1 GB cap for safety
                ),
                maxBufferSize: Math.min(
                    this.adapter.limits.maxBufferSize,
                    1 << 30   // 1 GB cap for safety
                )
            }
        });

        this.device.lost.then(info => {
            console.error('[GPUEngine] Device lost:', info.message);
            this.ready = false;
            if (typeof this.onLost === 'function') this.onLost(info);
        });

        this._allocBuffers(V_sq_cpu, b_x_cpu, a_x_cpu, b_z_cpu, a_z_cpu);
        this._createLayouts();
        this._createBindGroups();
        this._writeParams(dt, 15.0);
        await this._ensurePipelines(fd_order);

        this.ready = true;
        console.log(`[GPUEngine] Ready on ${this.gpuLabel}`);
        return this;
    }

    // ── Buffer allocation ──────────────────────────────────────────────────
    _allocBuffers(V_sq_cpu, b_x_cpu, a_x_cpu, b_z_cpu, a_z_cpu) {
        const dev = this.device;
        const n   = this.numCells;
        const W   = this.W_sim;
        const H   = this.H_sim;

        const RW  = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC;
        const RO  = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
        const WO  = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC;
        const MR  = GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST;
        const UNI = GPUBufferUsage.UNIFORM  | GPUBufferUsage.COPY_DST;

        this.buf_P       = dev.createBuffer({ size: n*4, usage: RW,  label: 'P' });
        this.buf_Vx      = dev.createBuffer({ size: n*4, usage: RW,  label: 'Vx' });
        this.buf_Vz      = dev.createBuffer({ size: n*4, usage: RW,  label: 'Vz' });
        this.buf_V_sq    = dev.createBuffer({ size: n*4, usage: RO,  label: 'V_sq' });
        this.buf_psi_VxX = dev.createBuffer({ size: n*4, usage: RW,  label: 'psi_VxX' });
        this.buf_psi_VzZ = dev.createBuffer({ size: n*4, usage: RW,  label: 'psi_VzZ' });
        this.buf_psi_PxX = dev.createBuffer({ size: n*4, usage: RW,  label: 'psi_PxX' });
        this.buf_psi_PzZ = dev.createBuffer({ size: n*4, usage: RW,  label: 'psi_PzZ' });
        this.buf_bx      = dev.createBuffer({ size: W*4, usage: RO,  label: 'b_x' });
        this.buf_ax      = dev.createBuffer({ size: W*4, usage: RO,  label: 'a_x' });
        this.buf_bz      = dev.createBuffer({ size: H*4, usage: RO,  label: 'b_z' });
        this.buf_az      = dev.createBuffer({ size: H*4, usage: RO,  label: 'a_z' });

        const siLen       = (this.N_REC * 2 + this.N_OBS) * this.GH;
        this.buf_sismo    = dev.createBuffer({ size: siLen*4, usage: RW, label: 'sismo' });
        this.buf_simoRd   = dev.createBuffer({ size: siLen*4, usage: MR, label: 'sismo_read' });

        this.buf_pixels   = dev.createBuffer({ size: n*4, usage: RW | WO, label: 'pixels' });
        this.buf_pixRd    = dev.createBuffer({ size: n*4, usage: MR,       label: 'pixels_read' });

        this.buf_params   = dev.createBuffer({ size: 64,  usage: UNI, label: 'params' });

        // Per-step data: up to 2048 steps, each padded to 256-byte uniform alignment
        this.STEP_STRIDE  = 256;
        this.MAX_STEPS    = 2048;
        this.buf_stepData = dev.createBuffer({
            size:  this.MAX_STEPS * this.STEP_STRIDE,
            usage: UNI,
            label: 'step_data'
        });

        // Upload static data
        dev.queue.writeBuffer(this.buf_V_sq, 0, V_sq_cpu);
        dev.queue.writeBuffer(this.buf_bx,   0, b_x_cpu);
        dev.queue.writeBuffer(this.buf_ax,   0, a_x_cpu);
        dev.queue.writeBuffer(this.buf_bz,   0, b_z_cpu);
        dev.queue.writeBuffer(this.buf_az,   0, a_z_cpu);
    }

    // ── Bind group layouts ─────────────────────────────────────────────────
    _createLayouts() {
        const dev = this.device;
        const S   = GPUShaderStage.COMPUTE;

        this.bgl_fields = dev.createBindGroupLayout({
            label: 'fields',
            entries: [
                { binding:  0, visibility: S, buffer: { type: 'storage' } },
                { binding:  1, visibility: S, buffer: { type: 'storage' } },
                { binding:  2, visibility: S, buffer: { type: 'storage' } },
                { binding:  3, visibility: S, buffer: { type: 'read-only-storage' } },
                { binding:  4, visibility: S, buffer: { type: 'storage' } },
                { binding:  5, visibility: S, buffer: { type: 'storage' } },
                { binding:  6, visibility: S, buffer: { type: 'storage' } },
                { binding:  7, visibility: S, buffer: { type: 'storage' } },
                { binding:  8, visibility: S, buffer: { type: 'read-only-storage' } },
                { binding:  9, visibility: S, buffer: { type: 'read-only-storage' } },
                { binding: 10, visibility: S, buffer: { type: 'read-only-storage' } },
                { binding: 11, visibility: S, buffer: { type: 'read-only-storage' } },
                { binding: 12, visibility: S, buffer: { type: 'storage' } },
                { binding: 13, visibility: S, buffer: { type: 'storage' } },
            ]
        });

        this.bgl_params = dev.createBindGroupLayout({
            label: 'params',
            entries: [{ binding: 0, visibility: S, buffer: { type: 'uniform', minBindingSize: 64 } }]
        });

        this.bgl_step = dev.createBindGroupLayout({
            label: 'step',
            entries: [{
                binding: 0, visibility: S,
                buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: 128 }
            }]
        });

        this.pipelineLayout = dev.createPipelineLayout({
            bindGroupLayouts: [this.bgl_fields, this.bgl_params, this.bgl_step]
        });
    }

    _createBindGroups() {
        const dev = this.device;
        this.bg_fields = dev.createBindGroup({
            layout: this.bgl_fields,
            entries: [
                { binding:  0, resource: { buffer: this.buf_P       } },
                { binding:  1, resource: { buffer: this.buf_Vx      } },
                { binding:  2, resource: { buffer: this.buf_Vz      } },
                { binding:  3, resource: { buffer: this.buf_V_sq    } },
                { binding:  4, resource: { buffer: this.buf_psi_VxX } },
                { binding:  5, resource: { buffer: this.buf_psi_VzZ } },
                { binding:  6, resource: { buffer: this.buf_psi_PxX } },
                { binding:  7, resource: { buffer: this.buf_psi_PzZ } },
                { binding:  8, resource: { buffer: this.buf_bx      } },
                { binding:  9, resource: { buffer: this.buf_ax      } },
                { binding: 10, resource: { buffer: this.buf_bz      } },
                { binding: 11, resource: { buffer: this.buf_az      } },
                { binding: 12, resource: { buffer: this.buf_sismo   } },
                { binding: 13, resource: { buffer: this.buf_pixels  } },
            ]
        });
        this.bg_params = dev.createBindGroup({
            layout: this.bgl_params,
            entries: [{ binding: 0, resource: { buffer: this.buf_params } }]
        });
        // Step bind group uses dynamic offsets at dispatch time
        this.bg_step = dev.createBindGroup({
            layout: this.bgl_step,
            entries: [{ binding: 0, resource: { buffer: this.buf_stepData, offset: 0, size: 128 } }]
        });
    }

    // ── Pipeline compilation (one set per fd_order) ────────────────────────
    async _ensurePipelines(fd_order) {
        if (this._pipelineCache[fd_order]) {
            this._pipelines     = this._pipelineCache[fd_order];
            this._activeFDOrder = fd_order;
            return;
        }

        const dev  = this.device;
        const mod  = dev.createShaderModule({ code: WGSL_FDTD, label: `fdtd_o${fd_order}` });
        const layout = this.pipelineLayout;
        const C    = { FD_ORDER: fd_order };

        const makeAsync = (entry) => dev.createComputePipelineAsync({
            layout, compute: { module: mod, entryPoint: entry, constants: C }
        });

        const [vel, pres, surf, src, sismo, pix] = await Promise.all([
            makeAsync('velocity_update'),
            makeAsync('pressure_update'),
            makeAsync('free_surface'),
            makeAsync('source_inject'),
            makeAsync('sismo_record'),
            makeAsync('pixels_convert'),
        ]);

        this._pipelineCache[fd_order] = { vel, pres, surf, src, sismo, pix };
        this._pipelines     = this._pipelineCache[fd_order];
        this._activeFDOrder = fd_order;
        console.log(`[GPUEngine] Pipelines compiled for O${fd_order}`);
    }

    // ── Write Params uniform ───────────────────────────────────────────────
    _writeParams(dt, gain, domain_W, domain_H, useFreeSurface, geologyMode) {
        const W       = this.W_sim;
        const H       = this.H_sim;
        const sj_surf = Math.floor(this.OCEAN_Y / this.SCALE);
        const sj_rec  = Math.floor((this.OCEAN_Y + 4) / this.SCALE);

        const ab = new ArrayBuffer(64);
        const dv = new DataView(ab);
        dv.setUint32(  0, W,                 true);
        dv.setUint32(  4, H,                 true);
        dv.setFloat32( 8, dt,                true);
        dv.setUint32( 12, sj_surf,           true);
        dv.setFloat32(16, this.SCALE,        true);
        dv.setUint32( 20, this.N_REC,        true);
        dv.setUint32( 24, this.GH,           true);
        dv.setFloat32(28, gain / 15.0,       true);
        dv.setInt32(  32, sj_rec,            true);
        dv.setFloat32(36, this.CABLE_LEN,    true);
        dv.setUint32( 40, this.N_OBS,        true);
        dv.setFloat32(44, this.obs_spacing,  true);
        dv.setFloat32(48, domain_W || 1200,  true);
        dv.setFloat32(52, domain_H || 480,   true);
        dv.setFloat32(56, useFreeSurface ? 1.0 : 0.0, true);
        dv.setUint32( 60, geologyMode !== undefined ? geologyMode : 0, true); // 0=salt,1=flat,2=fault
        this.device.queue.writeBuffer(this.buf_params, 0, ab);
        this._dt   = dt;
        this._gain = gain;
    }

    // ── Per-step data precompute & upload ──────────────────────────────────
    _prepareStepData(opts) {
        const { steps, cycle_start, dt, dt_phy, mode,
                sxShip, sxSrc, activeShots, source_freq, source_amp,
                cur_t_max, T_sismo_ticks } = opts;

        const STRIDE = this.STEP_STRIDE;
        const ab     = new ArrayBuffer(steps * STRIDE);
        const dv     = new DataView(ab);

        const sk = opts.ship_speed_knots || 4.5;
        const ship_speed_mps = sk * 0.514444;
        const ship_speed = (ship_speed_mps / 6000.0) * dt * this.SCALE;
        let   sxShip_s   = sxShip;
        let   sxSrc_s    = sxSrc;
        const W_max      = opts.W_max || 9999;

        for (let s = 0; s < steps; s++) {
            const cycle_t = cycle_start + s + 1;
            sxShip_s += ship_speed;
            sxSrc_s  += ship_speed;
            // Removed automatic reset to keep vessel position persistent until manual reset

            let si0=0, sj0=0, val0=0, act0=0;
            let si1=0, sj1=0, val1=0, act1=0;

            for (const sh of activeShots) {
                const elapsed_sec = (cycle_t - sh.t0) * dt_phy;
                const delay_sec   = 1.5 / source_freq;
                const max_sec     = 3.0 / source_freq;
                let   val         = 0;
                if (elapsed_sec >= 0 && elapsed_sec < max_sec) {
                    if (opts.wavelet_type === 'minphase') {
                        const omega = 2.0 * Math.PI * source_freq;
                        // Slower decay (0.35) ensures a realistic ~33% negative trough
                        val = 1.634 * Math.exp(-0.35 * omega * elapsed_sec) * Math.sin(omega * elapsed_sec) * source_amp * dt * 0.4;
                    } else {
                        const t    = elapsed_sec - delay_sec;
                        const pi2  = Math.PI * Math.PI;
                        const fpt2 = pi2 * source_freq * source_freq * t * t;
                        // Ricker wavelet, pre-multiplied by dt*0.4 (worker parity)
                        val = (1.0 - 2.0 * fpt2) * Math.exp(-fpt2) * source_amp * dt * 0.4;
                    }
                }
                const ship_src_dist = opts.ship_src_dist || 0;
                const src_px = ((sh.type === 'main') ? sxShip_s : sxSrc_s) - ship_src_dist;
                const si     = Math.floor(src_px / this.SCALE);
                const sj     = Math.floor((this.OCEAN_Y + (opts.source_depth || 8.0)) / this.SCALE);
                if (sh.type === 'main') { si0=si; sj0=sj; val0=val; act0 = (val !== 0) ? 1 : 0; }
                else                    { si1=si; sj1=sj; val1=val; act1 = (val !== 0) ? 1 : 0; }
            }

            const is_shot2  = (mode === 'clo' && cycle_t >= cur_t_max);
            const t_local   = is_shot2 ? cycle_t - cur_t_max : cycle_t;
            const rec_valid = (mode !== 'none' && t_local <= T_sismo_ticks) ? 1.0 : 0.0;
            const gy        = Math.min(Math.floor((t_local / T_sismo_ticks) * this.GH), this.GH - 1);
            const sismo_off = is_shot2 ? this.N_REC * this.GH : 0;

            const base = s * STRIDE;
            dv.setFloat32(base +  0, si0,       true);
            dv.setFloat32(base +  4, sj0,       true);
            dv.setFloat32(base +  8, val0,      true);
            dv.setFloat32(base + 12, act0,      true);
            dv.setFloat32(base + 16, si1,       true);
            dv.setFloat32(base + 20, sj1,       true);
            dv.setFloat32(base + 24, val1,      true);
            dv.setFloat32(base + 28, act1,      true);
            dv.setFloat32(base + 32, sxShip_s,  true);
            dv.setFloat32(base + 36, gy,        true);
            dv.setFloat32(base + 40, sismo_off, true);
            dv.setFloat32(base + 44, rec_valid, true);
            dv.setFloat32(base + 48, this.OCEAN_Y + (opts.cable_head_depth || 10), true);
            dv.setFloat32(base + 52, this.OCEAN_Y + (opts.cable_tail_depth || 10), true);
            dv.setFloat32(base + 56, sxShip_s - (opts.ship_src_dist || 0) - (opts.src_rec_dist || 0), true);
            dv.setFloat32(base + 60, opts.box_xmin !== undefined ? opts.box_xmin : 0, true);
            dv.setFloat32(base + 64, opts.box_ymin !== undefined ? opts.box_ymin : 0, true);
        }

        this.device.queue.writeBuffer(this.buf_stepData, 0, ab, 0, steps * STRIDE);
        return { sxShip: sxShip_s, sxSrc: sxSrc_s };
    }

    // ── Main batch submit ──────────────────────────────────────────────────
    async submitBatch(opts) {
        const { steps, fd_order, display_gain, dt } = opts;
        if (!this.ready) throw new Error('GPUEngine not ready');

        // Ensure correct pipeline variant is compiled
        if (fd_order !== this._activeFDOrder) {
            await this._ensurePipelines(fd_order);
        }
        // Update params if dt, gain, or cable_len changed
        const cable_len = opts.cable_len || this.CABLE_LEN;
        const domain_W = opts.domain_W || 1200;
        const domain_H = opts.domain_H || 480;
        if (dt !== this._dt || display_gain !== this._gain || cable_len !== this.CABLE_LEN || domain_W !== this._domain_W || domain_H !== this._domain_H) {
            this.CABLE_LEN = cable_len;
            this._domain_W = domain_W;
            this._domain_H = domain_H;
            this._writeParams(dt, display_gain, domain_W, domain_H, opts.useFreeSurface, opts.geologyMode);
        }

        // Pre-compute & upload all per-step source/sismo data (one writeBuffer call)
        const result = this._prepareStepData(opts);

        const W       = this.W_sim;
        const H       = this.H_sim;
        const bxmin = opts.box_xmin !== undefined ? opts.box_xmin : 0;
        const bxmax = opts.box_xmax !== undefined ? opts.box_xmax : W - 1;
        const bymin = opts.box_ymin !== undefined ? opts.box_ymin : 0;
        const bymax = opts.box_ymax !== undefined ? opts.box_ymax : H - 1;
        const box_w = bxmax - bxmin + 1;
        const box_h = bymax - bymin + 1;

        const pip     = this._pipelines;
        const wx      = Math.ceil(box_w / 16);
        const wy      = Math.ceil(box_h / 16);
        const sj_surf = Math.floor(this.OCEAN_Y / this.SCALE);
        const wx_surf = wx;
        const surf_max_y = Math.min(sj_surf, bymax);
        const surf_h = Math.max(0, surf_max_y - bymin + 1);
        const wy_surf = Math.ceil(surf_h / 16);
        const wx_si   = Math.ceil((this.N_REC + this.N_OBS) / 128);

        const enc = this.device.createCommandEncoder({ label: 'fdtd_batch' });

        for (let s = 0; s < steps; s++) {
            const dynOff = s * this.STEP_STRIDE;

            const p1 = enc.beginComputePass({ label: `vel_${s}` });
            p1.setPipeline(pip.vel);  p1.setBindGroup(0, this.bg_fields);
            p1.setBindGroup(1, this.bg_params); p1.setBindGroup(2, this.bg_step, [dynOff]);
            p1.dispatchWorkgroups(wx, wy); p1.end();

            const p2 = enc.beginComputePass({ label: `pres_${s}` });
            p2.setPipeline(pip.pres); p2.setBindGroup(0, this.bg_fields);
            p2.setBindGroup(1, this.bg_params); p2.setBindGroup(2, this.bg_step, [dynOff]);
            p2.dispatchWorkgroups(wx, wy); p2.end();

            const p3 = enc.beginComputePass({ label: `surf_${s}` });
            p3.setPipeline(pip.surf); p3.setBindGroup(0, this.bg_fields);
            p3.setBindGroup(1, this.bg_params); p3.setBindGroup(2, this.bg_step, [dynOff]);
            p3.dispatchWorkgroups(wx_surf, wy_surf); p3.end();

            const p4 = enc.beginComputePass({ label: `src_${s}` });
            p4.setPipeline(pip.src);  p4.setBindGroup(0, this.bg_fields);
            p4.setBindGroup(1, this.bg_params); p4.setBindGroup(2, this.bg_step, [dynOff]);
            p4.dispatchWorkgroups(1); p4.end();

            const p5 = enc.beginComputePass({ label: `sismo_${s}` });
            p5.setPipeline(pip.sismo); p5.setBindGroup(0, this.bg_fields);
            p5.setBindGroup(1, this.bg_params); p5.setBindGroup(2, this.bg_step, [dynOff]);
            p5.dispatchWorkgroups(wx_si); p5.end();
        }

        // Pixel colormap (once per batch)
        if (!opts.sismoOnly) {
            const pp = enc.beginComputePass({ label: 'pixels' });
            pp.setPipeline(pip.pix); pp.setBindGroup(0, this.bg_fields);
            pp.setBindGroup(1, this.bg_params); pp.setBindGroup(2, this.bg_step, [0]);
            pp.dispatchWorkgroups(wx, wy); pp.end();
        }

        // Append readback copy immediately to the same encoder
        if (!opts.sismoOnly) {
            enc.copyBufferToBuffer(this.buf_pixels, 0, this.buf_pixRd,  0, W * H * 4);
        }
        enc.copyBufferToBuffer(this.buf_sismo,  0, this.buf_simoRd, 0, (this.N_REC * 2 + this.N_OBS) * this.GH * 4);

        // Submit ALL physics + copy commands together
        this.device.queue.submit([enc.finish()]);

        return result;
    }

    // ── Async readbacks (called after submitBatch resolves) ────────────────
    async readPixels() {
        await this.buf_pixRd.mapAsync(GPUMapMode.READ);
        const copy = this.buf_pixRd.getMappedRange().slice(0);
        this.buf_pixRd.unmap();
        return new Uint8Array(copy);
    }

    async readSismo() {
        await this.buf_simoRd.mapAsync(GPUMapMode.READ);
        const copy = this.buf_simoRd.getMappedRange().slice(0);
        this.buf_simoRd.unmap();
        return new Float32Array(copy);
    }


    // ── Clear operations ───────────────────────────────────────────────────

    async clearWavefield() {
        const dev = this.device;
        const n   = this.numCells * 4;
        const enc = dev.createCommandEncoder();
        enc.clearBuffer(this.buf_P,       0, n);
        enc.clearBuffer(this.buf_Vx,      0, n);
        enc.clearBuffer(this.buf_Vz,      0, n);
        enc.clearBuffer(this.buf_psi_VxX, 0, n);
        enc.clearBuffer(this.buf_psi_VzZ, 0, n);
        enc.clearBuffer(this.buf_psi_PxX, 0, n);
        enc.clearBuffer(this.buf_psi_PzZ, 0, n);
        enc.clearBuffer(this.buf_pixels,  0, this.W_sim * this.H_sim * 4);
        dev.queue.submit([enc.finish()]);
    }

    async clearSismo() {
        const dev = this.device;
        const enc = dev.createCommandEncoder();
        enc.clearBuffer(this.buf_sismo, 0, (this.N_REC * 2 + this.N_OBS) * this.GH * 4);
        dev.queue.submit([enc.finish()]);
    }


    // ── Update V_sq (called when subsurface model changes) ────────────────
    updateVsq(V_sq_cpu) {
        this.device.queue.writeBuffer(this.buf_V_sq, 0, V_sq_cpu);
    }

    updateCPML(b_x, a_x, b_z, a_z) {
        this.device.queue.writeBuffer(this.buf_bx, 0, b_x);
        this.device.queue.writeBuffer(this.buf_ax, 0, a_x);
        this.device.queue.writeBuffer(this.buf_bz, 0, b_z);
        this.device.queue.writeBuffer(this.buf_az, 0, a_z);
    }

    // ── Tear down ──────────────────────────────────────────────────────────
    destroy() {
        this.ready = false;
        this.device?.destroy();
    }
}
