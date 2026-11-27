#include "Geology.hpp"
#include <algorithm>
#include <cmath>
#include "Material.hpp"

void layerDepths(GeologyModel g, double x, double W, double H, double& sf, double& dp, double& sc) {
    if (g == GeologyModel::Flat) {
        sf = H * 0.52;
        dp = H * 0.93;
        sc = H * 10;                       // no salt
    } else if (g == GeologyModel::Fault) {
        // A fault down-throwing the right side, plus a regional dip
        double faultPos = W / 2.0;
        double throwAmt = H * 0.31;
        double sf_base = H * 0.45;
        double dp_base = H * 0.79;
        sf = x < faultPos ? sf_base : sf_base + throwAmt;
        dp = x < faultPos ? dp_base : dp_base + throwAmt;
        sf += (x / W) * (H * 0.1);
        dp += (x / W) * (H * 0.1);
        sc = H * 10;
    } else {
        // Salt dome
        double domScale = W * W * 0.00833;
        sf = H * 0.45 + (x / W) * (H * 0.25);
        dp = H * 0.875 - (x / W) * (H * 0.10);
        sc = H * 1.08 - (H * 0.625) * std::exp(-std::pow(x - W / 2, 2) / domScale);
        if (sc < sf + 10) sc = sf + 10;
    }
}

GeoLayout buildLayout(GeologyModel g, double W, double H) {
    GeoLayout L;
    for (double x = -100; x <= W + 200; x += (W / 200)) {
        double sf, dp, sc;
        layerDepths(g, x, W, H, sf, dp, sc);
        L.seafloor.push_back({(float)x, (float)sf});
        L.deep.push_back({(float)x, (float)dp});
        if (g == GeologyModel::Salt) L.salt.push_back({(float)x, (float)sc});
    }
    return L;
}

void fillVsq(std::vector<float>& vsq, int W_sim, int H_sim, double SCALE, GeologyModel g, double W, double H) {
    vsq.assign((size_t)W_sim * H_sim, 0.0f);
    std::vector<float> sf_arr(W_sim), dp_arr(W_sim), sc_arr(W_sim);
    for (int i = 0; i < W_sim; i++) {
        double sf, dp, sc;
        layerDepths(g, i * SCALE, W, H, sf, dp, sc);
        sf_arr[i] = (float)sf; dp_arr[i] = (float)dp; sc_arr[i] = (float)sc;   // Float32Array parity
    }
    for (int j = 0; j < H_sim; j++) {
        double y = j * SCALE;
        float* row = vsq.data() + (size_t)j * W_sim;
        for (int i = 0; i < W_sim; i++) {
            double v = 0.25;
            if (y >= sc_arr[i]) v = 0.68;
            else if (y >= dp_arr[i]) v = 0.48;
            else if (y >= sf_arr[i]) v = 0.35;
            row[i] = (float)(v * v);
        }
    }
}

void fillPresetModel(std::vector<float>& vsq, std::vector<float>& rho, int W_sim, int H_sim, double SCALE, GeologyModel g,
                     double W, double H, bool average) {
    if (!average) {
        fillVsq(vsq, W_sim, H_sim, SCALE, g, W, H);
        densityFromVsq(vsq, rho);
        return;
    }
    constexpr int NS = 8;   // sub-columns per cell
    const size_t N = (size_t)W_sim * H_sim;
    vsq.assign(N, 0.0f);
    rho.assign(N, 0.0f);
    struct Col { float sf, dp, sc; };
    std::vector<Col> cols((size_t)W_sim * NS);
    for (int i = 0; i < W_sim; i++)
        for (int k = 0; k < NS; k++) {
            double sf, dp, sc;
            layerDepths(g, i * SCALE + SCALE * ((k + 0.5) / NS - 0.5), W, H, sf, dp, sc);
            cols[(size_t)i * NS + k] = {(float)sf, (float)dp, (float)sc};
        }
    // the layers of fillVsq: 0 water, 1 sediment, 2 basement, 3 salt
    static const double vn[4] = {0.25, 0.35, 0.48, 0.68};
    double rhoL[4], KL[4];
    for (int l = 0; l < 4; l++) {
        rhoL[l] = gardnerDensity(vn[l] * kVelocityNorm);
        KL[l] = rhoL[l] * vn[l] * vn[l];
    }
    auto layerAt = [](const Col& c, double y) { return y >= c.sc ? 3 : y >= c.dp ? 2 : y >= c.sf ? 1 : 0; };
    for (int j = 0; j < H_sim; j++) {
        const double y0 = (j - 0.5) * SCALE, y1 = (j + 0.5) * SCALE;
        for (int i = 0; i < W_sim; i++) {
            double w[4] = {0, 0, 0, 0};
            for (int k = 0; k < NS; k++) {
                const Col& c = cols[(size_t)i * NS + k];
                double b[5] = {y0, 0, 0, 0, y1};
                int nb = 1;
                for (float d : {c.sf, c.dp, c.sc})
                    if (d > y0 && d < y1) b[nb++] = d;
                b[nb] = y1;
                std::sort(b + 1, b + nb);
                for (int s = 0; s < nb; s++) w[layerAt(c, 0.5 * (b[s] + b[s + 1]))] += b[s + 1] - b[s];
            }
            double sw = 0, sr = 0, sK = 0;
            for (int l = 0; l < 4; l++) {
                sw += w[l];
                sr += w[l] * rhoL[l];
                sK += w[l] * KL[l];
            }
            const size_t idx = (size_t)j * W_sim + i;
            rho[idx] = (float)(sr / sw);
            vsq[idx] = (float)(sK / sr);   // mean K / mean rho
        }
    }
}

double obsSeafloorDepth(GeologyModel g, double rx, double W, double H) {
    if (g == GeologyModel::Flat) return H * 0.52;
    if (g == GeologyModel::Fault) {
        double faultPos = W / 2.0, throwAmt = H * 0.31;
        double ry = rx < faultPos ? H * 0.45 : H * 0.45 + throwAmt;
        return ry + (rx / W) * (H * 0.1);
    }
    return H * 0.45 + (rx / W) * (H * 0.25);
}
