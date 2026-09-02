// FDTD CPU Web Worker — CPML (Convolutional Perfectly Matched Layer) Edition
// Komatitsch & Martin (2007) GEOPHYSICS formulation.
// Memory variables are local to each worker (persistent across batches, reset on respawn).

let P, Vx, Vz, V_sq, sismo_data, syncState;
let id, numWorkers, startY, endY, W_sim, H_sim, SCALE, OCEAN_Y, CABLE_LEN, N_REC, GH, N_OBS, obs_spacing;
let domain_W = 1200, domain_H = 480, geology_model = 'salt';

// CPML coefficients (1D per axis, structured-cloned from main thread)
let b_x, a_x; // x-dir [W_sim]
let b_z, a_z; // z-dir [H_sim]

// CPML memory variables — local to this worker's row strip, persistent across batches
let psi_Vx_x, psi_Vz_z; // for velocity updates
let psi_P_x,  psi_P_z;  // for pressure updates

// ─────────────────────────────────────────────
// ATOMIC BARRIER (phase-locked, spurious-wake safe)
// ─────────────────────────────────────────────
function barrier() {
    let current_phase = Atomics.load(syncState, 0);
    let count = Atomics.add(syncState, 1, 1) + 1;
    if (count === numWorkers) {
        Atomics.store(syncState, 1, 0);
        Atomics.add(syncState, 0, 1);
        Atomics.notify(syncState, 0, numWorkers);
    } else {
        while (Atomics.load(syncState, 0) === current_phase) {
            Atomics.wait(syncState, 0, current_phase);
        }
    }
}

// ─────────────────────────────────────────────
// MESSAGE HANDLER
// ─────────────────────────────────────────────
self.onmessage = function(e) {
    let msg = e.data;

    if (msg.cmd === 'update_cpml') { b_z = msg.b_z; a_z = msg.a_z; }
    if (msg.cmd === 'reset_cpml') {
        // Zero all CPML memory variables — called whenever P/Vx/Vz are reset in main thread
        if (psi_Vx_x) psi_Vx_x.fill(0);
        if (psi_Vz_z) psi_Vz_z.fill(0);
        if (psi_P_x)  psi_P_x.fill(0);
        if (psi_P_z)  psi_P_z.fill(0);
        return; // No postMessage needed, fire-and-forget
    }

    if (msg.cmd === 'init') {

        P          = new Float32Array(msg.sab_P);
        Vx         = new Float32Array(msg.sab_Vx);
        Vz         = new Float32Array(msg.sab_Vz);
        V_sq       = new Float32Array(msg.sab_V_sq);
        sismo_data = new Float32Array(msg.sab_sismo);
        syncState  = new Int32Array(msg.sab_sync);

        id = msg.id; numWorkers = msg.numWorkers;
        startY = msg.startY; endY = msg.endY;
        W_sim = msg.W_sim; H_sim = msg.H_sim; SCALE = msg.SCALE;
        OCEAN_Y = msg.OCEAN_Y; CABLE_LEN = msg.CABLE_LEN;
        N_REC = msg.N_REC; GH = msg.GH;
        N_OBS = msg.N_OBS || 20; obs_spacing = msg.obs_spacing || 200;
        domain_W = msg.domain_W || 1200;
        domain_H = msg.domain_H || 480;
        geology_model = msg.geology_model || 'salt';

        // CPML coefficients (structured-clone, each worker has its own copy)
        b_x = msg.b_x; a_x = msg.a_x;
        b_z = msg.b_z; a_z = msg.a_z;

        // Allocate CPML memory variables for this worker's row strip
        let strip_size = (endY - startY) * W_sim;
        psi_Vx_x = new Float32Array(strip_size);
        psi_Vz_z = new Float32Array(strip_size);
        psi_P_x  = new Float32Array(strip_size);
        psi_P_z  = new Float32Array(strip_size);

        postMessage({cmd: 'ready'});
    }

    if (msg.cmd === 'batch') {
        let steps    = msg.steps;
        let dt       = msg.dt;
        let dt_phy   = msg.dt_phy;
        let cycle_start  = msg.cycle_t;
        let activeShots  = msg.activeShots;
        let source_freq  = msg.source_freq;
        let source_amp   = msg.source_amp;
        let mode     = msg.mode;
        let fd_order = msg.fd_order;
        
        // Sync dynamic geometry from main thread
        sxShip   = msg.sxShip;
        sxSrc    = msg.sxSrc;
        CABLE_LEN = msg.cable_len || CABLE_LEN;

        let cur_t_max     = Math.floor(64800.0 / (SCALE * dt));
        let T_sismo_ticks = Math.floor((msg.sismo_max_ms * 6.0) / (SCALE * dt));

        let W_max = msg.W_max;

        let bxmin = msg.box_xmin !== undefined ? msg.box_xmin : 0;
        let bxmax = msg.box_xmax !== undefined ? msg.box_xmax : W_sim - 1;
        let bymin = msg.box_ymin !== undefined ? msg.box_ymin : 0;
        let bymax = msg.box_ymax !== undefined ? msg.box_ymax : H_sim - 1;
        let local_startY = Math.max(startY, bymin);
        let local_endY = Math.min(endY, bymax);

        let compute_time = 0, wait_time = 0;

        for (let s = 0; s < steps; s++) {
            let t0 = performance.now();
            let cycle_t = cycle_start + s + 1;

            // ════════════════════════════════════════════════════════
            // PHASE 1: VELOCITY UPDATE WITH CPML
            // For each cell: update psi (CPML memory) using 1st-order FD,
            // then advance Vx/Vz using high-order stencil + psi correction.
            // ════════════════════════════════════════════════════════
            if (fd_order === 2) {
                let j_s = local_startY, j_e = Math.min(local_endY, H_sim - 1);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(0, bxmin); i <= Math.min(W_sim - 1 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dPx = P[idx + 1]      - P[idx];
                        let dPz = P[idx + W_sim]  - P[idx];
                        psi_Vx_x[pi] = b_x[i] * psi_Vx_x[pi] + a_x[i] * dPx;
                        psi_Vz_z[pi] =    bzj  * psi_Vz_z[pi] +    azj  * dPz;
                        Vx[idx] -= dt * (dPx + psi_Vx_x[pi]);
                        Vz[idx] -= dt * (dPz + psi_Vz_z[pi]);
                    }
                }
            } else if (fd_order === 4) {
                let c1 = 9.0/8.0, c2 = 1.0/24.0;
                let j_s = Math.max(1, local_startY), j_e = Math.min(local_endY, H_sim - 2);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(1, bxmin); i <= Math.min(W_sim - 2 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dPx = P[idx + 1]     - P[idx];
                        let dPz = P[idx + W_sim] - P[idx];
                        psi_Vx_x[pi] = b_x[i] * psi_Vx_x[pi] + a_x[i] * dPx;
                        psi_Vz_z[pi] =    bzj  * psi_Vz_z[pi] +    azj  * dPz;
                        let sDx = c1*dPx - c2*(P[idx+2] - P[idx-1]);
                        let sDz = c1*dPz - c2*(P[idx+2*W_sim] - P[idx-W_sim]);
                        Vx[idx] -= dt * (sDx + psi_Vx_x[pi]);
                        Vz[idx] -= dt * (sDz + psi_Vz_z[pi]);
                    }
                }
            } else if (fd_order === 6) {
                let c1 = 75/64, c2 = -25/384, c3 = 3/640;
                let j_s = Math.max(2, local_startY), j_e = Math.min(local_endY, H_sim - 3);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(2, bxmin); i <= Math.min(W_sim - 3 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dPx = P[idx+1]     - P[idx];
                        let dPz = P[idx+W_sim] - P[idx];
                        psi_Vx_x[pi] = b_x[i] * psi_Vx_x[pi] + a_x[i] * dPx;
                        psi_Vz_z[pi] =    bzj  * psi_Vz_z[pi] +    azj  * dPz;
                        let sDx = c1*dPx + c2*(P[idx+2]-P[idx-1]) + c3*(P[idx+3]-P[idx-2]);
                        let sDz = c1*dPz + c2*(P[idx+2*W_sim]-P[idx-W_sim]) + c3*(P[idx+3*W_sim]-P[idx-2*W_sim]);
                        Vx[idx] -= dt * (sDx + psi_Vx_x[pi]);
                        Vz[idx] -= dt * (sDz + psi_Vz_z[pi]);
                    }
                }
            } else if (fd_order === 8) {
                let c1 = 1225/1024, c2 = -245/3072, c3 = 49/5120, c4 = -5/7168;
                let j_s = Math.max(3, local_startY), j_e = Math.min(local_endY, H_sim - 4);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(3, bxmin); i <= Math.min(W_sim - 4 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dPx = P[idx+1]     - P[idx];
                        let dPz = P[idx+W_sim] - P[idx];
                        psi_Vx_x[pi] = b_x[i] * psi_Vx_x[pi] + a_x[i] * dPx;
                        psi_Vz_z[pi] =    bzj  * psi_Vz_z[pi] +    azj  * dPz;
                        let sDx = c1*dPx + c2*(P[idx+2]-P[idx-1]) + c3*(P[idx+3]-P[idx-2]) + c4*(P[idx+4]-P[idx-3]);
                        let sDz = c1*dPz + c2*(P[idx+2*W_sim]-P[idx-W_sim]) + c3*(P[idx+3*W_sim]-P[idx-2*W_sim]) + c4*(P[idx+4*W_sim]-P[idx-3*W_sim]);
                        Vx[idx] -= dt * (sDx + psi_Vx_x[pi]);
                        Vz[idx] -= dt * (sDz + psi_Vz_z[pi]);
                    }
                }
            } else if (fd_order === 10) {
                let c1 = 19845/16384, c2 = -735/8192, c3 = 567/40960, c4 = -405/229376, c5 = 35/294912;
                let j_s = Math.max(4, local_startY), j_e = Math.min(local_endY, H_sim - 5);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(4, bxmin); i <= Math.min(W_sim - 5 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dPx = P[idx+1]     - P[idx];
                        let dPz = P[idx+W_sim] - P[idx];
                        psi_Vx_x[pi] = b_x[i] * psi_Vx_x[pi] + a_x[i] * dPx;
                        psi_Vz_z[pi] =    bzj  * psi_Vz_z[pi] +    azj  * dPz;
                        let sDx = c1*dPx + c2*(P[idx+2]-P[idx-1]) + c3*(P[idx+3]-P[idx-2]) + c4*(P[idx+4]-P[idx-3]) + c5*(P[idx+5]-P[idx-4]);
                        let sDz = c1*dPz + c2*(P[idx+2*W_sim]-P[idx-W_sim]) + c3*(P[idx+3*W_sim]-P[idx-2*W_sim]) + c4*(P[idx+4*W_sim]-P[idx-3*W_sim]) + c5*(P[idx+5*W_sim]-P[idx-4*W_sim]);
                        Vx[idx] -= dt * (sDx + psi_Vx_x[pi]);
                        Vz[idx] -= dt * (sDz + psi_Vz_z[pi]);
                    }
                }
            }

            let t1 = performance.now();
            compute_time += (t1 - t0);
            barrier(); // Sync after velocity update
            let t2 = performance.now();
            wait_time += (t2 - t1);

            // ════════════════════════════════════════════════════════
            // PHASE 2: PRESSURE UPDATE WITH CPML
            // psi uses 1st-order FD; main update uses high-order stencil.
            // ════════════════════════════════════════════════════════
            if (fd_order === 2) {
                let j_s = Math.max(1, local_startY), j_e = Math.min(local_endY, H_sim - 1);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(1, bxmin); i <= Math.min(W_sim - 1 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dVx = Vx[idx] - Vx[idx - 1];
                        let dVz = Vz[idx] - Vz[idx - W_sim];
                        psi_P_x[pi] = b_x[i] * psi_P_x[pi] + a_x[i] * dVx;
                        psi_P_z[pi] =    bzj  * psi_P_z[pi] +    azj  * dVz;
                        P[idx] -= dt * V_sq[idx] * (dVx + psi_P_x[pi] + dVz + psi_P_z[pi]);
                    }
                }
            } else if (fd_order === 4) {
                let c1 = 9.0/8.0, c2 = 1.0/24.0;
                let j_s = Math.max(2, local_startY), j_e = Math.min(local_endY, H_sim - 2);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(2, bxmin); i <= Math.min(W_sim - 2 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dVx1 = Vx[idx] - Vx[idx-1];
                        let dVz1 = Vz[idx] - Vz[idx-W_sim];
                        psi_P_x[pi] = b_x[i] * psi_P_x[pi] + a_x[i] * dVx1;
                        psi_P_z[pi] =    bzj  * psi_P_z[pi] +    azj  * dVz1;
                        let dVx = c1*dVx1 - c2*(Vx[idx+1] - Vx[idx-2]);
                        let dVz = c1*dVz1 - c2*(Vz[idx+W_sim] - Vz[idx-2*W_sim]);
                        P[idx] -= dt * V_sq[idx] * (dVx + psi_P_x[pi] + dVz + psi_P_z[pi]);
                    }
                }
            } else if (fd_order === 6) {
                let c1 = 75/64, c2 = -25/384, c3 = 3/640;
                let j_s = Math.max(3, local_startY), j_e = Math.min(local_endY, H_sim - 3);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(3, bxmin); i <= Math.min(W_sim - 3 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dVx1 = Vx[idx] - Vx[idx-1];
                        let dVz1 = Vz[idx] - Vz[idx-W_sim];
                        psi_P_x[pi] = b_x[i] * psi_P_x[pi] + a_x[i] * dVx1;
                        psi_P_z[pi] =    bzj  * psi_P_z[pi] +    azj  * dVz1;
                        let dVx = c1*dVx1 + c2*(Vx[idx+1] - Vx[idx-2]) + c3*(Vx[idx+2] - Vx[idx-3]);
                        let dVz = c1*dVz1 + c2*(Vz[idx+W_sim] - Vz[idx-2*W_sim]) + c3*(Vz[idx+2*W_sim] - Vz[idx-3*W_sim]);
                        P[idx] -= dt * V_sq[idx] * (dVx + psi_P_x[pi] + dVz + psi_P_z[pi]);
                    }
                }
            } else if (fd_order === 8) {
                let c1 = 1225/1024, c2 = -245/3072, c3 = 49/5120, c4 = -5/7168;
                let j_s = Math.max(4, local_startY), j_e = Math.min(local_endY, H_sim - 4);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(4, bxmin); i <= Math.min(W_sim - 4 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dVx1 = Vx[idx] - Vx[idx-1];
                        let dVz1 = Vz[idx] - Vz[idx-W_sim];
                        psi_P_x[pi] = b_x[i] * psi_P_x[pi] + a_x[i] * dVx1;
                        psi_P_z[pi] =    bzj  * psi_P_z[pi] +    azj  * dVz1;
                        let dVx = c1*dVx1 + c2*(Vx[idx+1] - Vx[idx-2]) + c3*(Vx[idx+2] - Vx[idx-3]) + c4*(Vx[idx+3] - Vx[idx-4]);
                        let dVz = c1*dVz1 + c2*(Vz[idx+W_sim] - Vz[idx-2*W_sim]) + c3*(Vz[idx+2*W_sim] - Vz[idx-3*W_sim]) + c4*(Vz[idx+3*W_sim] - Vz[idx-4*W_sim]);
                        P[idx] -= dt * V_sq[idx] * (dVx + psi_P_x[pi] + dVz + psi_P_z[pi]);
                    }
                }
            } else if (fd_order === 10) {
                let c1 = 19845/16384, c2 = -735/8192, c3 = 567/40960, c4 = -405/229376, c5 = 35/294912;
                let j_s = Math.max(5, local_startY), j_e = Math.min(local_endY, H_sim - 5);
                for (let j = j_s; j < j_e; j++) {
                    let row = j * W_sim, pr = (j - startY) * W_sim;
                    let bzj = b_z[j], azj = a_z[j];
                    for (let i = Math.max(5, bxmin); i <= Math.min(W_sim - 5 - 1, bxmax); i++) {
                        let idx = row + i, pi = pr + i;
                        let dVx1 = Vx[idx] - Vx[idx-1];
                        let dVz1 = Vz[idx] - Vz[idx-W_sim];
                        psi_P_x[pi] = b_x[i] * psi_P_x[pi] + a_x[i] * dVx1;
                        psi_P_z[pi] =    bzj  * psi_P_z[pi] +    azj  * dVz1;
                        let dVx = c1*dVx1 + c2*(Vx[idx+1] - Vx[idx-2]) + c3*(Vx[idx+2] - Vx[idx-3]) + c4*(Vx[idx+3] - Vx[idx-4]) + c5*(Vx[idx+4] - Vx[idx-5]);
                        let dVz = c1*dVz1 + c2*(Vz[idx+W_sim] - Vz[idx-2*W_sim]) + c3*(Vz[idx+2*W_sim] - Vz[idx-3*W_sim]) + c4*(Vz[idx+3*W_sim] - Vz[idx-4*W_sim]) + c5*(Vz[idx+4*W_sim] - Vz[idx-5*W_sim]);
                        P[idx] -= dt * V_sq[idx] * (dVx + psi_P_x[pi] + dVz + psi_P_z[pi]);
                    }
                }
            }

            // ─── Phase 2 timing (all workers measure their own compute) ───────
            let t_p2 = performance.now();
            compute_time += (t_p2 - t2);

            // ════════════════════════════════════════════════════════
            // PHASE 3: Boundaries + source + sismogram
            // Free surface zeroing is DISTRIBUTED by row ownership.
            // Ship movement, source injection and sismogram remain in
            // worker 0 (they require the updated sxShip position).
            // ════════════════════════════════════════════════════════

            // 3a — Distributed: Dirichlet free surface (each worker owns its rows)
            {
                let sj_surf = Math.floor(OCEAN_Y / SCALE);
                let j_start = local_startY;
                let j_stop = msg.useFreeSurface ? Math.min(local_endY, sj_surf + 1) : 0;
                for (let j = j_start; j < j_stop; j++) {
                    let row = j * W_sim;
                    for (let i = Math.max(0, bxmin); i <= Math.min(W_sim - 1, bxmax); i++) P[row + i] = 0.0;
                }
            }

            // 3b — Serial: ship movement + source injection + sismogram (worker 0)
            if (id === 0) {
                // Ship movement (physical speed)
                let sk = msg.ship_speed_knots || 4.5;
                let mps = sk * 0.514444;
                let ship_speed_meters = mps * dt_phy;
                sxShip += ship_speed_meters;
                sxSrc  += ship_speed_meters;
                // Removed auto-reset to match main thread's persistent positioning

                // Source injection
                for (let shot of activeShots) {
                    let elapsed_sec = (cycle_t - shot.t0) * dt_phy;
                    let delay_sec = 1.5 / source_freq;
                    let max_sec   = 3.0 / source_freq;
                    if (elapsed_sec >= 0 && elapsed_sec < max_sec) {
                        let val = 0;
                        if (msg.wavelet_type === 'minphase') {
                            let omega = 2.0 * Math.PI * source_freq;
                            val = 1.634 * Math.exp(-0.35 * omega * elapsed_sec) * Math.sin(omega * elapsed_sec) * source_amp;
                        } else {
                            let t    = elapsed_sec - delay_sec;
                            let pi2  = Math.PI * Math.PI;
                            let fpt2 = pi2 * source_freq * source_freq * t * t;
                            val = (1.0 - 2.0 * fpt2) * Math.exp(-fpt2) * source_amp;
                        }
                        let ship_src_dist = msg.ship_src_dist || 0;
                        let src_pos = ((shot.type === 'main') ? sxShip : sxSrc) - ship_src_dist;
                        let si = Math.floor(src_pos / SCALE);
                        let sj = Math.floor((OCEAN_Y + (msg.source_depth || 8.0)) / SCALE);
                        let r_s = 3;
                        for (let dj = -r_s; dj <= r_s; dj++) {
                            for (let di = -r_s; di <= r_s; di++) {
                                let d_sq = di*di + dj*dj;
                                if (d_sq <= r_s*r_s) {
                                    let sj_c = sj+dj, si_c = si+di;
                                    if (si_c>1 && si_c<W_sim-1 && sj_c>1 && sj_c<H_sim-1) {
                                        P[sj_c*W_sim+si_c] += val * Math.exp(-d_sq/2.0) * dt * 0.4;
                                    }
                                }
                            }
                        }
                    }
                }

                // Sismogram recording
                if (mode !== 'none') {
                    let is_shot2 = (mode === 'clo' && cycle_t >= cur_t_max);
                    let t_local  = is_shot2 ? cycle_t - cur_t_max : cycle_t;
                    if (t_local <= T_sismo_ticks) {
                        let gy       = (t_local / T_sismo_ticks) * GH;
                        let gy_pixel = Math.floor(gy);
                        if (gy_pixel >= 0 && gy_pixel < GH) {
                            let offset = is_shot2 ? (N_REC * GH) : 0;
                            let cable_head_x = sxShip - (msg.ship_src_dist || 0) - (msg.src_rec_dist || 0);
                            for (let i = 0; i < N_REC; i++) {
                                let rx = cable_head_x - CABLE_LEN + i * (CABLE_LEN / (N_REC - 1));
                                let norm_i = i / (N_REC - 1);
                                let current_depth = msg.cable_tail_depth + (msg.cable_head_depth - msg.cable_tail_depth) * norm_i;
                                let ry = OCEAN_Y + current_depth;
                                let si = Math.floor(rx / SCALE);
                                let sj = Math.floor(ry / SCALE);
                                let amp = (si > 0 && si < W_sim && sj > 0 && sj < H_sim) ? P[sj * W_sim + si] : 0;
                                let idx = offset + i * GH + gy_pixel;
                                if (Math.abs(amp) > Math.abs(sismo_data[idx])) sismo_data[idx] = amp;
                            }

                            // OBS recording (stationary)
                            let obs_offset = N_REC * 2 * GH;
                            for (let i = 0; i < N_OBS; i++) {
                                let rx = (i + 1) * obs_spacing;
                                let ry;
                                if (geology_model === 'flat') {
                                    ry = domain_H * 0.52;
                                } else if (geology_model === 'fault') {
                                    let faultPos = domain_W / 2.0;
                                    let throwAmt = domain_H * 0.31;
                                    ry = rx < faultPos ? domain_H * 0.45 : (domain_H * 0.45) + throwAmt;
                                    ry += (rx / domain_W) * (domain_H * 0.1);
                                } else {
                                    // salt dome
                                    ry = domain_H * 0.45 + (rx / domain_W) * (domain_H * 0.25);
                                }
                                ry += 4.0; // slight offset below surface
                                let si = Math.floor(rx / SCALE);
                                let sj = Math.floor(ry / SCALE);
                                let amp = (si > 0 && si < W_sim && sj > 0 && sj < H_sim) ? P[sj * W_sim + si] : 0;
                                let idx = obs_offset + i * GH + gy_pixel;
                                if (Math.abs(amp) > Math.abs(sismo_data[idx])) sismo_data[idx] = amp;
                            }
                        }
                    }
                }
            }

            let t4 = performance.now();
            compute_time += (t4 - t_p2);
            barrier(); // Sync end of tick
            let t5 = performance.now();
            wait_time += (t5 - t4);
        }

        if (id === 0) {
            postMessage({cmd: 'batch_done', cycle_t: cycle_start + steps, sxShip, sxSrc, compute: compute_time, wait: wait_time, id});
        } else {
            postMessage({cmd: 'batch_done', compute: compute_time, wait: wait_time, id});
        }
    }
}
