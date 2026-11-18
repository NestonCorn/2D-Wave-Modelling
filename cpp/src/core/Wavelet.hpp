#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "Params.hpp"

// Source wavelets.
//
// targetPulse(): the waveform the wave field should carry, unit peak, sampled from the
// shot time at dt = 1 / (kPulseRate * f).
//   Ricker      zero-phase, symmetric, centred at 1.5/f.
//   MinPhase    impulse response of a causal band-pass: 2nd-order critically damped high-pass
//               at 0.66 f, 4th-order Butterworth low-pass at 1.32 f and two real poles at 2.8 f.
//               A causal filter with all poles in the left half-plane is minimum phase by
//               construction: sharp onset, main peak at 0.73/f (Ricker: 1.5/f), then a deeper
//               trough and a short decay. Its spectrum peaks at f and is down 31 dB at 3 f.
//               The high-pass is 2nd order on purpose: like the Ricker, the spectrum falls as
//               f^2 towards DC. With a 1st-order high-pass the 2D wake behind the wavefront
//               (a slowly decaying low-frequency tail of the 2D Green's function) was ~10x
//               stronger and showed as a dark disc / second ring inside the wavefront when the
//               free surface (whose ghost cancels it) was off.
//               The HTML's 6.993 tau(1-tau) e^(-1.3255 tau) peaked at ~0.53 f, had a non-zero
//               area (static pressure left at the source) and energy up to ~4 f (grid dispersion).
//
// SourcePulse: what has to be injected to get that waveform (see below).

constexpr int kPulseRate = 200;   // samples per dominant period

namespace wavelet_detail {

struct Biquad {
    double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double operator()(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// Bilinear-transform Butterworth sections; frequencies in units of f, sampled at kPulseRate per period
inline std::vector<double> minPhasePulse(int len) {
    constexpr double fl = 0.660722, fh = 1.321445, fp = 2.803208;   // place the spectral peak at f
    const double kl = std::tan(M_PI * fl / kPulseRate), kh = std::tan(M_PI * fh / kPulseRate);
    const double kp = std::tan(M_PI * fp / kPulseRate);
    std::vector<Biquad> chain;
    {   // 2nd-order high-pass, Q = 0.5 (double real pole: no overshoot)
        const double q = 0.5, nm = 1 / (1 + kl / q + kl * kl);
        chain.push_back({nm, -2 * nm, nm, 2 * (kl * kl - 1) * nm, (1 - kl / q + kl * kl) * nm});
    }
    for (double q : {0.5411961, 1.3065630}) {                                       // 4th-order low-pass
        const double nm = 1 / (1 + kh / q + kh * kh), b0 = kh * kh * nm;
        chain.push_back({b0, 2 * b0, b0, 2 * (kh * kh - 1) * nm, (1 - kh / q + kh * kh) * nm});
    }
    for (int k = 0; k < 2; k++) chain.push_back({kp / (1 + kp), kp / (1 + kp), 0, (kp - 1) / (kp + 1), 0});   // real poles
    std::vector<double> w(len);
    double peak = 0;
    for (int i = 0; i < len; i++) {
        double v = i == 0 ? 1.0 : 0.0;
        for (Biquad& s : chain) v = s(v);
        w[i] = v;
        if (std::fabs(v) > std::fabs(peak)) peak = v;
    }
    for (double& x : w) x /= peak;   // unit peak, main lobe positive
    return w;
}

} // namespace wavelet_detail

inline std::vector<double> targetPulse(Wavelet type, int len) {
    if (type == Wavelet::MinPhase) return wavelet_detail::minPhasePulse(len);
    std::vector<double> w(len);
    for (int i = 0; i < len; i++) {
        const double tc = (double)i / kPulseRate - 1.5;   // in periods
        const double a = M_PI * M_PI * tc * tc;
        w[i] = (1.0 - 2.0 * a) * std::exp(-a);
    }
    return w;
}

// Injected source time function. Adding s(t) to P every step is a pressure-rate source, and
// in 2D such a source radiates the half-derivative of s(t): a plain Ricker came out
// asymmetric (deeper trailing trough, ~45 deg phase shift). Injecting the half-integral
// (Riemann-Liouville, Gruenwald-Letnikov weights) of the target pulse makes the propagating
// wave carry the target pulse itself. Tabulated on a fine grid, normalised to unit peak.
class SourcePulse {
public:
    bool matches(Wavelet type, double f) const { return type == type_ && f == f_; }

    void build(Wavelet type, double f) {
        type_ = type; f_ = f;
        dtau_ = 1.0 / (kPulseRate * f);
        // Both targets are < 1e-4 of their peak after 3 periods; the half-integral tail of a
        // zero-area pulse decays like t^-3/2: keep 8 periods and taper the last 2.
        const int n = 8 * kPulseRate;
        std::vector<double> x = targetPulse(type, n);
        for (int i = 3 * kPulseRate; i < n; i++) x[i] = 0.0;
        std::vector<double> g(n);
        g[0] = 1.0;
        for (int k = 1; k < n; k++) g[k] = g[k - 1] * (k - 0.5) / k;
        v_.assign(n, 0.0);
        double peak = 0;
        for (int i = 0; i < n; i++) {
            double s = 0;
            for (int k = 0; k <= i; k++) s += g[k] * x[i - k];
            v_[i] = s;
            peak = std::max(peak, std::fabs(s));
        }
        const int taper = 2 * kPulseRate;
        for (int i = n - taper; i < n; i++) v_[i] *= 0.5 + 0.5 * std::cos(M_PI * (i - (n - taper)) / taper);
        if (peak > 0) for (double& s : v_) s /= peak;
    }

    // Linear interpolation; zero before the shot and after the table
    double operator()(double t) const {
        if (t < 0 || v_.empty()) return 0.0;
        const double u = t / dtau_;
        const size_t i = (size_t)u;
        if (i + 1 >= v_.size()) return 0.0;
        const double w = u - (double)i;
        return v_[i] * (1.0 - w) + v_[i + 1] * w;
    }

private:
    Wavelet type_ = Wavelet::Ricker;
    double f_ = -1, dtau_ = 1;
    std::vector<double> v_;
};
