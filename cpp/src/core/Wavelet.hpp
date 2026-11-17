#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include "Params.hpp"

// Source wavelets.
//
// targetPulse(): the waveform the wave field should carry, unit peak, sampled from the
// shot time at dt = 1 / (kPulseRate * f).
//   Ricker      zero-phase, centred at 1.5/f.
//   MinPhase    the minimum-phase wavelet with the Ricker's amplitude spectrum (real-cepstrum
//               construction): same dominant frequency and bandwidth as the Ricker, so it is
//               just as clean on the grid, but causal and front-loaded. The HTML's
//               6.993 tau(1-tau) e^(-1.3255 tau) peaked at ~0.53 f, had a non-zero area
//               (static pressure left at the source) and energy up to ~4 f (grid dispersion).
//
// SourcePulse: what has to be injected to get that waveform (see below).

constexpr int kPulseRate = 200;   // samples per dominant period

namespace wavelet_detail {

inline void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / (double)len * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}

// Minimum-phase pulse with amplitude spectrum (F/f)^2 exp(-(F/f)^2) (the Ricker's), unit peak
inline std::vector<double> minPhaseRicker(int len) {
    const int n = 1 << 15;   // 163 periods: fine spectral sampling, negligible cepstral aliasing
    std::vector<std::complex<double>> c(n);
    for (int k = 0; k < n; k++) {
        const double fr = (double)std::min(k, n - k) / n * kPulseRate;   // in units of f
        const double a = fr * fr * std::exp(-fr * fr);
        c[k] = std::log(a + 1e-9);   // water level: keeps log finite at DC and far out of band
    }
    fft(c, true);   // real cepstrum
    for (int k = 1; k < n / 2; k++) c[k] *= 2.0;
    for (int k = n / 2 + 1; k < n; k++) c[k] = 0.0;
    fft(c, false);
    for (auto& x : c) x = std::exp(x);
    fft(c, true);
    std::vector<double> w(len);
    double peak = 0;
    for (int i = 0; i < len; i++) {
        w[i] = c[i].real();
        if (std::fabs(w[i]) > std::fabs(peak)) peak = w[i];
    }
    for (double& x : w) x /= peak;   // unit peak, dominant lobe positive
    return w;
}

} // namespace wavelet_detail

inline std::vector<double> targetPulse(Wavelet type, int len) {
    if (type == Wavelet::MinPhase) return wavelet_detail::minPhaseRicker(len);
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
