// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/drums/DrumKit.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sedef::dsp::drums
{
namespace
{
constexpr double kQ23 = 8388608.0;    // 2^23
constexpr double kQ24 = 16777216.0;   // 2^24
constexpr double kQ26 = 67108864.0;   // 2^26
constexpr double kQ30 = 1073741824.0; // 2^30
constexpr std::int64_t kOne26 = std::int64_t { 1 } << 26;
// Modes are Q35: 12 bits under the output's Q23, so a slow decay never stalls in a limit cycle
// above the output's LSB (docs/drums.md, section 5.2).
constexpr int kModeShift = 12;
// Envelopes are Q38: 15 bits under Q23.
constexpr int kEnvShift = 15;

/// Rounds toward zero (magnitude truncation): a decaying mode's norm then strictly falls, so it
/// reaches zero instead of circling in a limit cycle.
constexpr std::int64_t truncate26 (std::int64_t v) noexcept
{
    return v / kOne26;
}
constexpr double kPi = std::numbers::pi;

// Bessel-function zeros j_mn and the ideal membrane's modal ratios j_mn / j_01
// (docs/drums.md, section 5.3).
constexpr double kJ01 = 2.404826;
constexpr std::array<double, 6> kMembraneRatios { 1.0,
                                                  3.831706 / kJ01,   // (1,1)
                                                  5.135622 / kJ01,   // (2,1)
                                                  5.520078 / kJ01,   // (0,2)
                                                  6.380162 / kJ01,   // (3,1)
                                                  7.015587 / kJ01 }; // (1,2)

/// A membrane's (0,1) frequency for a tension T (N/m) and a diameter in inches: polyester film
/// 0.25 mm thick, 1390 kg/m^3 (docs/drums.md, section 5.4).
double membraneHz (double tension, double inches)
{
    const double sigma = 1390.0 * 0.25e-3;
    const double radius = 0.5 * inches * 0.0254;
    return kJ01 * std::sqrt (tension / sigma) / (2.0 * kPi * radius);
}

struct Recipe
{
    int numModes = 0;
    std::array<double, DrumKit::kMaxModes> hz {};
    std::array<double, DrumKit::kMaxModes> weight {};
    double q = 10.0;        ///< every mode's Q (closed hats: qClosed)
    double qClosed = 0.0;   ///< hats only
    double contactMs = 1.0; ///< t_ref, the contact time of a top-velocity stroke
    double glideMax = 0.0;  ///< tension modulation at a top-velocity stroke
    double tonePeak = 0.0;  ///< sum of the modes' strike amplitudes at velocity 127
    int noise = 0;          ///< 0 none, 1 wires, 2 wash
    double noisePeak = 0.0; ///< the wash envelope or the wires at velocity 127
    double washMs = 0.0;    ///< wash decay τ (closed hats: washClosedMs)
    double washClosedMs = 0.0;
    double washRiseMs = 0.0; ///< bloom
    double wireThresholdDb = -30.0;
    double highHz = 0.0; ///< noise high-pass
    int highPoles = 0;
    double lowHz = 0.0; ///< noise low-pass (0 = none)
    double clickPeak = 0.0;
    double clickMs = 1.0;
    double pan = 0.0;
    std::uint32_t seed = 1;
};

Recipe membrane (double f01, int numModes, std::array<double, 6> weights, double q,
                 double contactMs, double glideMax, double tonePeak, double pan)
{
    Recipe r;
    r.numModes = numModes;
    for (int m = 0; m < numModes; ++m)
    {
        r.hz[static_cast<size_t> (m)] = f01 * kMembraneRatios[static_cast<size_t> (m)];
        r.weight[static_cast<size_t> (m)] = weights[static_cast<size_t> (m)];
    }
    r.q = q;
    r.contactMs = contactMs;
    r.glideMax = glideMax;
    r.tonePeak = tonePeak;
    r.pan = pan;
    return r;
}

/// Cymbal partials (docs/drums.md, section 5.4): weights rise as sqrt(f / f_lo).
Recipe cymbal (std::array<double, DrumKit::kMaxModes> hz, int numModes, double q, double tonePeak,
               double pan)
{
    Recipe r;
    r.numModes = numModes;
    r.hz = hz;
    for (int m = 0; m < numModes; ++m)
        r.weight[static_cast<size_t> (m)] = std::sqrt (hz[static_cast<size_t> (m)] / hz[0]);
    r.q = q;
    r.contactMs = 0.15;
    r.tonePeak = tonePeak;
    r.pan = pan;
    return r;
}

constexpr double kKitLevel = 1.9; // +5.6 dB

/// The kit (docs/drums.md, sections 5.4 and 5.5), in bank order: kick, snare, hats, ride, crash,
/// high, mid and floor tom.
std::array<Recipe, DrumKit::kNumBanks> recipes()
{
    std::array<Recipe, DrumKit::kNumBanks> k;

    // Kick 22": slack batter (600 N/m), beater near the centre (0.15 a), pillow-damped (Q 10).
    k[0] =
        membrane (membraneHz (600.0, 22.0), 3, { 1.864, 0.968, 0.294 }, 10.0, 2.0, 0.45, 0.40, 0.0);
    k[0].clickPeak = 0.06;
    k[0].clickMs = 1.5;

    // Snare 14": tight batter (3000 N/m), struck at 0.3 a, damped by the wires (Q 30).
    k[1] = membrane (membraneHz (3000.0, 14.0), 6, { 1.684, 1.703, 1.008, 1.244, 0.548, 2.676 },
                     30.0, 0.7, 0.0, 0.20, 0.0);
    k[1].noise = 1;
    k[1].noisePeak = 0.26;
    k[1].wireThresholdDb = -30.0;
    k[1].highHz = 1800.0;
    k[1].highPoles = 1;
    k[1].lowHz = 9000.0;
    k[1].clickPeak = 0.05;
    k[1].clickMs = 1.0;

    // Hats 14": one bank, open or clamped.
    k[2] = cymbal ({ 3200, 4080, 5210, 6650, 8480, 10820 }, 6, 4700.0, 0.05, -0.35);
    k[2].qClosed = 310.0;
    k[2].noise = 2;
    k[2].noisePeak = 0.12;
    k[2].washMs = 280.0;
    k[2].washClosedMs = 20.0;
    k[2].highHz = 6000.0;
    k[2].highPoles = 2;
    k[2].lowHz = 14000.0;

    // Ride 20".
    k[3] = cymbal ({ 420, 650, 930, 1430, 2190, 3100, 4750, 6900 }, 8, 2500.0, 0.07, 0.45);
    k[3].noise = 2;
    k[3].noisePeak = 0.05;
    k[3].washMs = 1600.0;
    k[3].highHz = 3000.0;
    k[3].highPoles = 1;
    k[3].lowHz = 10000.0;

    // Crash 17": the wash blooms over 12 ms.
    k[4] = cymbal ({ 330, 520, 890, 1370, 2130, 3270, 5180, 8810 }, 8, 2000.0, 0.08, -0.3);
    k[4].noise = 2;
    k[4].noisePeak = 0.14;
    k[4].washMs = 1200.0;
    k[4].washRiseMs = 12.0;
    k[4].highHz = 2000.0;
    k[4].highPoles = 1;
    k[4].lowHz = 12000.0;

    // Toms 10", 12", 16" at one tension (800 N/m): f ∝ 1/a.
    const std::array<double, 6> tomWeights { 1.684, 1.703, 1.008 };
    const std::array<double, 3> tomInches { 10.0, 12.0, 16.0 };
    const std::array<double, 3> tomPans { -0.3, 0.0, 0.4 };
    for (size_t t = 0; t < 3; ++t)
    {
        k[5 + t] = membrane (membraneHz (800.0, tomInches[t]), 3, tomWeights, 80.0, 0.8, 0.12, 0.32,
                             tomPans[t]);
        k[5 + t].clickPeak = 0.04;
        k[5 + t].clickMs = 1.0;
    }

    for (size_t b = 0; b < k.size(); ++b)
    {
        k[b].seed = 0x5EDEF000u + static_cast<std::uint32_t> (b) * 0x9E3779B1u;
        // The kit's level, measured once (docs/drums.md, section 5.5): a groove at intensity 3
        // reads -22.5 LUFS at Level 0 dB, 6.5 dB under the factory presets' -16 LUFS.
        k[b].tonePeak *= kKitLevel;
        k[b].noisePeak *= kKitLevel;
        k[b].clickPeak *= kKitLevel;
    }
    return k;
}

std::int64_t toQ30 (double v)
{
    return static_cast<std::int64_t> (std::llround (v * kQ30));
}

std::int64_t toQ26 (double v)
{
    return static_cast<std::int64_t> (std::llround (v * kQ26));
}

std::int64_t toQ24 (double v)
{
    return static_cast<std::int64_t> (std::llround (v * kQ24));
}

std::int32_t toQ23 (double v)
{
    return static_cast<std::int32_t> (std::llround (v * kQ23));
}

/// Impact strength u for a velocity (docs/drums.md, section 5.3).
double strength (int velocity)
{
    return std::pow (std::clamp (velocity, 0, 127) / 127.0, 1.5);
}

/// The half-sine contact pulse's spectrum relative to 0 Hz.
double contactSpectrum (double hz, double contactSeconds)
{
    const double x = 2.0 * hz * contactSeconds;
    const double den = 1.0 - x * x;
    if (std::abs (den) < 1.0e-9)
        return kPi / 4.0;
    return std::abs (std::cos (kPi * hz * contactSeconds) / den);
}

double contactSeconds (const Recipe& r, double u)
{
    // Hertzian contact: duration ∝ impact speed^(-1/5).
    return 1.0e-3 * r.contactMs * std::pow (std::max (u, 1.0e-6), -0.2);
}

/// A bilinear first-order filter's coefficients for a cut-off in Hz, and the sum of its impulse
/// response's magnitudes: the most it can raise a peak.
struct FirstOrder
{
    std::int64_t b0 = 0, a1 = 0;
    double peakGain = 1.0;
};

FirstOrder firstOrder (double hz, double fs, bool high)
{
    const double k = std::tan (kPi * std::min (hz, 0.45 * fs) / fs);
    const double b0 = (high ? 1.0 : k) / (1.0 + k);
    const double a1 = (k - 1.0) / (k + 1.0);
    // y[n] = b0 (x[n] ± x[n-1]) - a1 y[n-1]: h[0] = b0, h[1] = ±b0 - a1 b0, h[n] = -a1 h[n-1].
    const double h0 = b0;
    const double h1 = (high ? -b0 : b0) - a1 * b0;
    const double tail = std::abs (a1) < 1.0 ? std::abs (h1) / (1.0 - std::abs (a1)) : 1.0e9;
    return { toQ30 (b0), toQ30 (a1), std::abs (h0) + tail };
}

void design (KitFilter& f, double hz, double fs, bool high, double* peakGain = nullptr)
{
    const auto c = firstOrder (hz, fs, high);
    f.b0 = c.b0;
    f.a1 = c.a1;
    f.high = high;
    if (peakGain != nullptr)
        *peakGain *= c.peakGain;
}

/// An envelope's per-sample factor (Q24) for a time constant.
std::int64_t decay (double seconds, double fs)
{
    return toQ24 (std::exp (-1.0 / std::max (seconds * fs, 1.0)));
}

std::int64_t isqrt (std::int64_t n) noexcept
{
    if (n <= 0)
        return 0;
    auto x = static_cast<std::uint64_t> (n);
    std::uint64_t r = 0;
    std::uint64_t bit = std::uint64_t { 1 } << 62;
    while (bit > x)
        bit >>= 2;
    while (bit != 0)
    {
        if (x >= r + bit)
        {
            x -= r + bit;
            r = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<std::int64_t> (r);
}

/// Adds a burst of amplitude `a` to an envelope `env` in power (independent noise bursts),
/// capped.
std::int64_t addPower (std::int64_t env, std::int64_t a, std::int64_t cap) noexcept
{
    return std::min (cap, isqrt (env * env + a * a));
}

// A bank whose every state is under this (Q23) is silent: 2 LSB, -132 dBFS.
constexpr std::int64_t kQuiet = 2;
// The stop test runs every this many samples of the kit's own clock.
constexpr std::uint32_t kStopTestEvery = 64;
} // namespace

int DrumKit::bankOf (Instrument instrument) noexcept
{
    switch (instrument)
    {
    case Instrument::kick:
        return 0;
    case Instrument::snare:
        return 1;
    case Instrument::hatClosed:
    case Instrument::hatOpen:
        return 2;
    case Instrument::ride:
        return 3;
    case Instrument::crash:
        return 4;
    case Instrument::tomHigh:
        return 5;
    case Instrument::tomMid:
        return 6;
    case Instrument::tomFloor:
        return 7;
    }
    return 0;
}

void DrumKit::prepare (double sampleRate)
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto kit = recipes();
    peakBound_ = 0;
    // White noise spreads its power over 0..fs/2: to keep its level in the audio band the same
    // at every rate, its amplitude goes with sqrt(fs) (docs/drums.md, section 5.1).
    const double noiseScale = std::sqrt (fs / 48000.0);
    std::int64_t boundL = 0, boundR = 0;

    for (size_t b = 0; b < kit.size(); ++b)
    {
        const Recipe& r = kit[b];
        Bank& bank = banks_[b];
        bank = Bank {};
        bank.seed = r.seed;
        bank.numModes = 0;

        // Modes: coupled-form coefficients for the open (and, for the hats, the closed) damping.
        double toneAtTop = 0.0;
        for (int m = 0; m < r.numModes; ++m)
        {
            const double hz = r.hz[static_cast<size_t> (m)];
            if (hz >= 0.45 * fs)
                break;
            Mode& mode = bank.modes[static_cast<size_t> (bank.numModes++)];
            const double w = 2.0 * kPi * hz / fs;
            const double rOpen = std::exp (-kPi * hz / (r.q * fs)); // τ = Q / (π f)
            const double rClosed =
                r.qClosed > 0.0 ? std::exp (-kPi * hz / (r.qClosed * fs)) : rOpen;
            mode.cOpen = toQ26 (rOpen * std::cos (w));
            mode.sOpen = toQ26 (rOpen * std::sin (w));
            mode.cClosed = toQ26 (rClosed * std::cos (w));
            mode.sClosed = toQ26 (rClosed * std::sin (w));
            mode.c = mode.cOpen;
            mode.s = mode.sOpen;
            mode.omega = toQ30 (w);
            mode.hz = hz;
            if (m == 0)
            {
                bank.r1Open = toQ24 (rOpen);
                bank.r1Closed = toQ24 (rClosed);
                bank.e1.decay = bank.r1Open;
            }
            toneAtTop +=
                r.weight[static_cast<size_t> (m)] * contactSpectrum (hz, contactSeconds (r, 1.0));
        }

        // Strike amplitudes per velocity: u · w_m · C(f_m, t_c(u)), scaled so a top-velocity
        // strike's modes sum to the tone peak.
        const double scale = toneAtTop > 0.0 ? r.tonePeak / toneAtTop : 0.0;
        for (int v = 0; v < 128; ++v)
        {
            const double u = strength (v);
            const double tc = contactSeconds (r, u);
            for (int m = 0; m < bank.numModes; ++m)
            {
                const auto mi = static_cast<size_t> (m);
                bank.strikeAmp[static_cast<size_t> (v)][mi] =
                    toQ23 (scale * u * r.weight[mi] * contactSpectrum (bank.modes[mi].hz, tc));
            }
        }
        std::int64_t toneBound = 0;
        for (int m = 0; m < bank.numModes; ++m)
        {
            auto& mode = bank.modes[static_cast<size_t> (m)];
            mode.capMag =
                2 * static_cast<std::int64_t> (bank.strikeAmp[127][static_cast<size_t> (m)]);
            toneBound += mode.capMag;
        }

        // Tension modulation: glide = glideMax at a single top-velocity strike's e1, as e1^2.
        if (r.glideMax > 0.0 && bank.numModes > 0 && bank.strikeAmp[127][0] > 0)
        {
            const double a = bank.strikeAmp[127][0];
            bank.glideMax = toQ30 (r.glideMax);
            bank.glideK = static_cast<std::int64_t> (
                std::llround (r.glideMax * kQ30 * 17179869184.0 / (a * a))); // · 2^34
        }

        // Noise: wires driven by the head, or a wash with its own envelope.
        std::int64_t noiseBound = 0;
        if (r.noise == 1)
        {
            bank.noise = NoiseKind::wires;
            const double a = bank.strikeAmp[127][0];
            const double theta = a * std::pow (10.0, r.wireThresholdDb / 20.0);
            bank.wireThreshold = static_cast<std::int64_t> (std::llround (theta));
            bank.wireGain = toQ30 (noiseScale * r.noisePeak * kQ23 / std::max (a - theta, 1.0));
            // The head is capped at 2a, so the wires' drive is at most (2a - θ) · gain.
            noiseBound = static_cast<std::int64_t> (std::ceil (
                (2.0 * a - theta) * noiseScale * r.noisePeak * kQ23 / std::max (a - theta, 1.0)));
        }
        else if (r.noise == 2)
        {
            bank.noise = NoiseKind::wash;
            bank.washOpen = decay (1.0e-3 * r.washMs, fs);
            bank.washClosed =
                r.washClosedMs > 0.0 ? decay (1.0e-3 * r.washClosedMs, fs) : bank.washOpen;
            bank.wash.decay = bank.washOpen;
            bank.washRise = r.washRiseMs > 0.0
                                ? toQ30 (1.0 - std::exp (-1.0 / (1.0e-3 * r.washRiseMs * fs)))
                                : 0;
            for (int v = 0; v < 128; ++v)
                bank.washAmp[static_cast<size_t> (v)] =
                    toQ23 (noiseScale * r.noisePeak * strength (v));
            bank.washCap = 2 * static_cast<std::int64_t> (bank.washAmp[127]);
            noiseBound = bank.washCap;
        }
        if (r.noise != 0)
        {
            // The filters raise a peak by at most the sum of their impulse responses' magnitudes.
            double gain = 1.0;
            bank.highPoles = r.highPoles;
            design (bank.noiseHp1, r.highHz, fs, true, &gain);
            if (r.highPoles > 1)
                design (bank.noiseHp2, r.highHz, fs, true, &gain);
            bank.lowPass = r.lowHz > 0.0;
            if (bank.lowPass)
                design (bank.noiseLp, r.lowHz, fs, false, &gain);
            noiseBound =
                static_cast<std::int64_t> (std::ceil (static_cast<double> (noiseBound) * gain));
        }

        // The stick's or beater's click.
        std::int64_t clickBound = 0;
        if (r.clickPeak > 0.0)
        {
            bank.click.decay = decay (1.0e-3 * r.clickMs, fs);
            double lowGain = 1.0;
            for (int v = 0; v < 128; ++v)
            {
                const double u = strength (v);
                bank.clickAmp[static_cast<size_t> (v)] = toQ23 (noiseScale * r.clickPeak * u);
                // Brighter for a harder stroke: low-pass at 0.5 / t_c.
                const auto lp = firstOrder (0.5 / contactSeconds (r, u), fs, false);
                bank.clickLpB0[static_cast<size_t> (v)] = static_cast<std::int32_t> (lp.b0);
                bank.clickLpA1[static_cast<size_t> (v)] = static_cast<std::int32_t> (lp.a1);
                lowGain = std::max (lowGain, lp.peakGain);
            }
            bank.clickCap = 2 * static_cast<std::int64_t> (bank.clickAmp[127]);
            double highGain = 1.0;
            design (bank.clickHp, 1000.0, fs, true, &highGain);
            clickBound = static_cast<std::int64_t> (
                std::ceil (static_cast<double> (bank.clickCap) * lowGain * highGain));
        }

        // Constant-power pan.
        const double angle = 0.25 * kPi * (r.pan + 1.0);
        bank.panL = toQ30 (std::cos (angle));
        bank.panR = toQ30 (std::sin (angle));
        bank.bound = toneBound + noiseBound + clickBound;
        boundL += ((bank.bound * bank.panL) >> 30) + 1;
        boundR += ((bank.bound * bank.panR) >> 30) + 1;
    }
    peakBound_ = std::max (boundL, boundR);
    reset();
}

void DrumKit::reset() noexcept
{
    for (auto& bank : banks_)
    {
        bank.silence();
        bank.setClosed (false);
        bank.rng.seed (bank.seed);
    }
    clock_ = 0;
}

void DrumKit::reseed() noexcept
{
    for (auto& bank : banks_)
        bank.rng.seed (bank.seed);
}

void DrumKit::Bank::silence() noexcept
{
    for (auto& m : modes)
        m.x = m.y = 0;
    e1.value = 0;
    wash.value = 0;
    washOut = 0;
    click.value = 0;
    for (auto* f : { &noiseHp1, &noiseHp2, &noiseLp, &clickLp, &clickHp })
        f->x1 = f->y1 = 0;
    active = false;
}

void DrumKit::Bank::setClosed (bool shut) noexcept
{
    closed = shut;
    for (int m = 0; m < numModes; ++m)
    {
        auto& mode = modes[static_cast<size_t> (m)];
        mode.c = shut ? mode.cClosed : mode.cOpen;
        mode.s = shut ? mode.sClosed : mode.sOpen;
    }
    e1.decay = shut ? r1Closed : r1Open;
    wash.decay = shut ? washClosed : washOpen;
}

bool DrumKit::Bank::quiet() const noexcept
{
    for (int m = 0; m < numModes; ++m)
    {
        const auto& mode = modes[static_cast<size_t> (m)];
        if (std::abs (mode.x) > (kQuiet << kModeShift) ||
            std::abs (mode.y) > (kQuiet << kModeShift))
            return false;
    }
    for (const auto* f : { &noiseHp1, &noiseHp2, &noiseLp, &clickLp, &clickHp })
        if (std::abs (f->x1) > kQuiet || std::abs (f->y1) > kQuiet)
            return false;
    return wash.q23() <= kQuiet && washOut <= kQuiet && click.q23() <= kQuiet &&
           (noise != NoiseKind::wires || e1.q23() <= wireThreshold);
}

void DrumKit::strike (Instrument instrument, int velocity) noexcept
{
    const int v = std::clamp (velocity, 1, 127);
    Bank& bank = banks_[static_cast<size_t> (bankOf (instrument))];
    if (instrument == Instrument::hatClosed || instrument == Instrument::hatOpen)
        bank.setClosed (instrument == Instrument::hatClosed);

    for (int m = 0; m < bank.numModes; ++m)
    {
        auto& mode = bank.modes[static_cast<size_t> (m)];
        mode.x += static_cast<std::int64_t> (
                      bank.strikeAmp[static_cast<size_t> (v)][static_cast<size_t> (m)])
                  << kModeShift;
        // The bound (docs/drums.md, 5.2): never more than twice a top-velocity strike.
        const std::int64_t x23 = mode.x >> kModeShift;
        const std::int64_t y23 = mode.y >> kModeShift;
        const std::int64_t mag = isqrt (x23 * x23 + y23 * y23);
        if (mag > mode.capMag && mag > 0)
        {
            const std::int64_t k = (mode.capMag << 20) / mag; // Q20
            mode.x = (mode.x * k) >> 20;
            mode.y = (mode.y * k) >> 20;
        }
    }
    if (bank.numModes > 0)
    {
        const auto& m0 = bank.modes[0];
        const std::int64_t x23 = m0.x >> kModeShift;
        const std::int64_t y23 = m0.y >> kModeShift;
        bank.e1.value = isqrt (x23 * x23 + y23 * y23) << kEnvShift;
    }
    if (bank.noise == NoiseKind::wash)
        bank.wash.value =
            addPower (bank.wash.q23(), bank.washAmp[static_cast<size_t> (v)], bank.washCap)
            << kEnvShift;
    if (bank.clickCap > 0)
    {
        bank.click.value =
            addPower (bank.click.q23(), bank.clickAmp[static_cast<size_t> (v)], bank.clickCap)
            << kEnvShift;
        bank.clickLp.b0 = bank.clickLpB0[static_cast<size_t> (v)];
        bank.clickLp.a1 = bank.clickLpA1[static_cast<size_t> (v)];
        bank.clickLp.high = false;
    }
    bank.active = true;
}

std::int64_t DrumKit::Bank::next() noexcept
{
    std::int64_t out = 0;

    // Tension modulation: every mode sharpened by g = min(max, K · e1²).
    std::int64_t g = 0;
    if (glideK > 0)
    {
        const std::int64_t e = e1.q23();
        g = std::min (glideMax, (((e * e) >> 14) * glideK) >> 20);
    }

    for (int m = 0; m < numModes; ++m)
    {
        auto& mode = modes[static_cast<size_t> (m)];
        const std::int64_t x = mode.x;
        const std::int64_t y = mode.y;
        out += x >> kModeShift;
        std::int64_t nx = truncate26 (mode.c * x - mode.s * y);
        std::int64_t ny = truncate26 (mode.s * x + mode.c * y);
        if (g > 0)
        {
            // A second, small rotation by δ = ω·g: cos δ ≈ 1 − δ²/2, sin δ ≈ δ − δ³/6 (Q26).
            const std::int64_t d = ((mode.omega * g) >> 30) >> 4;
            const std::int64_t d2 = (d * d) >> 26;
            const std::int64_t cd = kOne26 - (d2 >> 1);
            const std::int64_t sd = d - ((d2 * d) >> 26) / 6;
            const std::int64_t rx = truncate26 (cd * nx - sd * ny);
            const std::int64_t ry = truncate26 (sd * nx + cd * ny);
            nx = rx;
            ny = ry;
        }
        mode.x = nx;
        mode.y = ny;
    }

    if (noise == NoiseKind::wires)
    {
        const std::int64_t n = static_cast<std::int32_t> (rng.nextU32()) >> 8;
        const std::int64_t drive = std::max<std::int64_t> (0, e1.q23() - wireThreshold);
        const std::int64_t amp = (drive * wireGain) >> 30;
        std::int64_t w = noiseHp1.process ((n * amp) >> 23);
        if (lowPass)
            w = noiseLp.process (w);
        out += w;
    }
    else if (noise == NoiseKind::wash)
    {
        const std::int64_t n = static_cast<std::int32_t> (rng.nextU32()) >> 8;
        if (washRise > 0)
            washOut += ((wash.q23() - washOut) * washRise) >> 30;
        else
            washOut = wash.q23();
        std::int64_t w = noiseHp1.process ((n * washOut) >> 23);
        if (highPoles > 1)
            w = noiseHp2.process (w);
        if (lowPass)
            w = noiseLp.process (w);
        out += w;
        wash.step();
    }

    if (click.value > 0 || clickLp.x1 != 0 || clickLp.y1 != 0 || clickHp.x1 != 0 || clickHp.y1 != 0)
    {
        const std::int64_t n = static_cast<std::int32_t> (rng.nextU32()) >> 8;
        out += clickHp.process (clickLp.process ((n * click.q23()) >> 23));
        click.step();
    }

    e1.step();
    return out;
}

void DrumKit::render (std::int32_t* left, std::int32_t* right, int numFrames) noexcept
{
    for (int i = 0; i < numFrames; ++i)
    {
        std::int64_t l = 0;
        std::int64_t r = 0;
        bool any = false;
        for (auto& bank : banks_)
        {
            if (!bank.active)
                continue;
            any = true;
            const std::int64_t s = bank.next();
            l += (s * bank.panL) >> 30;
            r += (s * bank.panR) >> 30;
        }
        left[i] = static_cast<std::int32_t> (l);
        right[i] = static_cast<std::int32_t> (r);
        if (!any)
            continue;
        // The stop test, on the kit's own clock so it falls on the same samples at any block size.
        if (++clock_ % kStopTestEvery == 0)
            for (auto& bank : banks_)
                if (bank.active && bank.quiet())
                    bank.silence();
    }
}

bool DrumKit::sounding() const noexcept
{
    for (const auto& bank : banks_)
        if (bank.active)
            return true;
    return false;
}

DrumKit::BankInfo DrumKit::bankInfo (Instrument instrument) const noexcept
{
    const Bank& bank = banks_[static_cast<size_t> (bankOf (instrument))];
    BankInfo info;
    info.numModes = bank.numModes;
    for (int m = 0; m < bank.numModes; ++m)
        info.hz[static_cast<size_t> (m)] = bank.modes[static_cast<size_t> (m)].hz;
    if (bank.numModes > 0)
    {
        const auto& m0 = bank.modes[0];
        const double x = std::ldexp (static_cast<double> (m0.x), -kModeShift); // to Q23
        const double y = std::ldexp (static_cast<double> (m0.y), -kModeShift);
        info.fundamentalAmplitude = std::sqrt (x * x + y * y);
        info.fundamentalPhase = std::atan2 (y, x);
    }
    if (bank.glideK > 0)
    {
        const std::int64_t e = bank.e1.q23();
        info.glide =
            static_cast<double> (std::min (bank.glideMax, (((e * e) >> 14) * bank.glideK) >> 20)) /
            kQ30;
    }
    if (bank.noise == NoiseKind::wires)
        info.wireDrive =
            static_cast<double> (std::max<std::int64_t> (0, bank.e1.q23() - bank.wireThreshold));
    info.active = bank.active;
    return info;
}

} // namespace sedef::dsp::drums
