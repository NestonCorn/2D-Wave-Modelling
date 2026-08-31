let W_sim = 600;
let H_sim = 240;
let SCALE = 2.0;
let OCEAN_Y = 80;
let source_depth = 8.0;
let sxShip = 200;
let sxSrc = 100;
let ship_src_dist = 50;

let activeShots = [{type: 'main'}];
let V_sq = new Float32Array(W_sim * H_sim);
V_sq.fill(1.5*1.5); // water velocity squared (km/s) -> 1.5

let T_eikonal = new Float32Array(W_sim * H_sim);
T_eikonal.fill(1e9);
let h = SCALE / 1000.0; // SCALE is in meters, V_sq is (km/s)^2, so h MUST BE IN KM!
for (let shot of activeShots) {
    let ship_src_dist_val = ship_src_dist || 0;
    let src_pos = ((shot.type === 'main') ? sxShip : sxSrc) - ship_src_dist_val;
    let si = Math.floor(src_pos / SCALE);
    let sj = Math.floor((OCEAN_Y + (source_depth || 8.0)) / SCALE);
    console.log('Source at si='+si+' sj='+sj);
    if (si > 0 && si < W_sim - 1 && sj > 0 && sj < H_sim - 1) {
        T_eikonal[sj * W_sim + si] = 0;
    }
}

let sweeps = 4;
for (let s = 0; s < sweeps; s++) {
    let i_start = (s % 2 === 0) ? 1 : W_sim - 2;
    let i_end = (s % 2 === 0) ? W_sim - 1 : 0;
    let i_step = (s % 2 === 0) ? 1 : -1;
    let j_start = (Math.floor(s / 2) % 2 === 0) ? 1 : H_sim - 2;
    let j_end = (Math.floor(s / 2) % 2 === 0) ? H_sim - 1 : 0;
    let j_step = (Math.floor(s / 2) % 2 === 0) ? 1 : -1;
    for (let j = j_start; j !== j_end; j += j_step) {
        for (let i = i_start; i !== i_end; i += i_step) {
            let idx = j * W_sim + i;
            if (T_eikonal[idx] === 0) continue;
            let tx = Math.min(T_eikonal[idx - 1], T_eikonal[idx + 1]);
            let ty = Math.min(T_eikonal[(j - 1) * W_sim + i], T_eikonal[(j + 1) * W_sim + i]);
            let slowness = 1.0 / Math.sqrt(V_sq[idx]);
            let rhs = slowness * h;
            let t_new;
            if (Math.abs(tx - ty) >= rhs) {
                t_new = Math.min(tx, ty) + rhs;
            } else {
                t_new = 0.5 * (tx + ty + Math.sqrt(2.0 * rhs * rhs - (tx - ty) * (tx - ty)));
            }
            if (t_new < T_eikonal[idx]) T_eikonal[idx] = t_new;
        }
    }
}

let limit = 0.1; // 0.1 sec
let min_i = W_sim, max_i = 0, min_j = H_sim, max_j = 0;
for (let j = 0; j < H_sim; j++) {
    for (let i = 0; i < W_sim; i++) {
        if (T_eikonal[j * W_sim + i] <= limit) {
            if (i < min_i) min_i = i;
            if (i > max_i) max_i = i;
            if (j < min_j) min_j = j;
            if (j > max_j) max_j = j;
        }
    }
}
console.log('min_i='+min_i+' max_i='+max_i+' min_j='+min_j+' max_j='+max_j);
