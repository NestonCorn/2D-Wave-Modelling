const fs = require('fs');
let html = fs.readFileSync('SLO_CLO_Interactive.html', 'utf-8');

let startIndex = html.indexOf('function drawFDTDToScreen()');
let endIndex = html.indexOf('function drawCable(toPX)');

if(startIndex > -1 && endIndex > -1) {
    let oldFunc = html.substring(startIndex, endIndex);
    
    let newFunc = \unction drawFDTDToScreen() {
            if (sismoOnlyMode) return;

            if (gpuReady && gpuEngine) {
                fdtdCtx.putImageData(imgData, 0, 0);
                ctx.imageSmoothingEnabled = true;
                ctx.globalCompositeOperation = 'source-over';
                ctx.drawImage(fdtdCanvas, 0, 0, W, H_main);
                return;
            }

            let dG = display_gain / 15.0;
            let cfl_limit = 100000.0;
            let sj_surf = Math.floor(OCEAN_Y / SCALE);
            
            pixelData32.fill(0);
            
            let dt_phy_box = (SCALE / 6000.0) * dt;
            let eikBox = getEikonalBox(cycle_t * dt_phy_box);
            
            let draw_min_j = sj_surf;
            let draw_max_j = H_sim - 1;
            
            if (useExpandingBox && eikBox && activeShots.length > 0) {
                draw_min_j = useFreeSurface ? Math.max(sj_surf, eikBox.min_j) : Math.max(0, eikBox.min_j);
                draw_max_j = Math.min(H_sim - 1, eikBox.max_j);
            }
            
            for (let j = draw_min_j; j <= draw_max_j; j++) {
                let min_i = 0, max_i = W_sim - 1;
                if (useExpandingBox && eikBox && eikBox.xMinZ && eikBox.xMaxZ) {
                    min_i = Math.max(0, eikBox.xMinZ[j]);
                    max_i = Math.min(W_sim - 1, eikBox.xMaxZ[j]);
                    if (min_i > max_i) continue;
                }
                
                let row_idx = j * W_sim;
                for (let i = min_i; i <= max_i; i++) {
                    let idx = row_idx + i;
                    let val = P[idx];

                    if (Number.isNaN(val) || Math.abs(val) > cfl_limit) {
                        if (!instability) {
                            instability = true;
                            document.getElementById('cflWarning').style.display = 'block';
                        }
                        break;
                    }

                    let scaled_val = val * dG;
                    if (scaled_val < -1.0) scaled_val = -1.0;
                    else if (scaled_val > 1.0) scaled_val = 1.0;

                    let lut_idx = ((scaled_val + 1.0) * 1023.5) | 0;
                    pixelData32[idx] = colormap32[lut_idx];
                }
                if (instability) break;
            }

            fdtdCtx.putImageData(imgData, 0, 0);
            ctx.imageSmoothingEnabled = true;
            ctx.globalCompositeOperation = 'source-over';
            ctx.drawImage(fdtdCanvas, 0, 0, W, H_main);
        }
        
        \;

    html = html.replace(oldFunc, newFunc);
    fs.writeFileSync('SLO_CLO_Interactive.html', html, 'utf-8');
    console.log('Replaced successfully');
} else {
    console.log('Could not find function bounds');
}
