#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Colour maps for the velocity model view and its colour bar. t in [0, 1] (slowest to fastest
// velocity); the result is 0xRRGGBB.
enum class ModelColormap { Ocean = 0, Turbo, Viridis, Magma, Gray, Count };

inline const char* colormapName(ModelColormap m) {
    switch (m) {
        case ModelColormap::Turbo: return "Turbo";
        case ModelColormap::Viridis: return "Viridis";
        case ModelColormap::Magma: return "Magma";
        case ModelColormap::Gray: return "Gri";
        default: return "Deniz";
    }
}

namespace colormap_detail {
inline uint32_t pack(double r, double g, double b) {
    auto u = [](double v) { return (uint32_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); };
    return u(r) << 16 | u(g) << 8 | u(b);
}
// Polynomial fits of matplotlib's viridis / magma and Google's Turbo (degree 6 / 5)
inline uint32_t poly6(const double c[7][3], double t) {
    double out[3];
    for (int k = 0; k < 3; k++) {
        double v = c[6][k];
        for (int i = 5; i >= 0; i--) v = c[i][k] + t * v;
        out[k] = v;
    }
    return pack(out[0], out[1], out[2]);
}
} // namespace colormap_detail

inline uint32_t colormapRGB(ModelColormap m, double t) {
    using namespace colormap_detail;
    t = std::clamp(t, 0.0, 1.0);
    switch (m) {
        case ModelColormap::Turbo: {
            t = 0.08 + 0.92 * t;   // the fit runs into near black over its first 8 %: start at its deep blue
            const double r = 0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943))));
            const double g = 0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604))));
            const double b = 0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973))));
            return pack(r, g, b);
        }
        case ModelColormap::Viridis: {
            static const double c[7][3] = {{0.2777273272234177, 0.005407344544966578, 0.3340998053353061},
                                           {0.1050930431085774, 1.404613529898575, 1.384590162594685},
                                           {-0.3308618287255563, 0.214847559468213, 0.09509516302823659},
                                           {-4.634230498983486, -5.799100973351585, -19.33244095627987},
                                           {6.228269936347081, 14.17993336680509, 56.69055260068105},
                                           {4.776384997670288, -13.74514537774601, -65.35303263337234},
                                           {-5.435455855934631, 4.645852612178535, 26.3124352495832}};
            return poly6(c, t);
        }
        case ModelColormap::Magma: {
            static const double c[7][3] = {{-0.002136485053939582, -0.000749655052795221, -0.005386127855323933},
                                           {0.2516605407371642, 0.6775232436837668, 2.494026599312351},
                                           {8.353717279216625, -3.577719514958484, 0.3144679030132573},
                                           {-27.66873308576866, 14.26473078096533, -13.64921318813922},
                                           {52.17613981234068, -27.94360607168351, 12.94416944238394},
                                           {-50.76852536473588, 29.04658282127291, 4.23415299384598},
                                           {18.65570506591883, -11.48977351997711, -5.601961508734096}};
            return poly6(c, t);
        }
        case ModelColormap::Gray: {
            const double v = 0.12 + 0.8 * t;
            return pack(v, v, v);
        }
        default: {   // Ocean: the app's dark blue to sand ramp
            struct Stop { double t, r, g, b; };
            static const Stop s[] = {{0.0, 8, 22, 44}, {0.25, 20, 52, 84}, {0.5, 52, 92, 108}, {0.75, 122, 118, 86}, {1.0, 196, 170, 120}};
            int k = 1;
            while (k < 4 && t > s[k].t) k++;
            const double u = (t - s[k - 1].t) / (s[k].t - s[k - 1].t);
            return pack((s[k - 1].r + u * (s[k].r - s[k - 1].r)) / 255.0, (s[k - 1].g + u * (s[k].g - s[k - 1].g)) / 255.0,
                        (s[k - 1].b + u * (s[k].b - s[k - 1].b)) / 255.0);
        }
    }
}
