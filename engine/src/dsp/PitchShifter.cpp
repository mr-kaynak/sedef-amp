// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/PitchShifter.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <pffft/pffft.h>
#include <vector>

// docs/dsp/transpose.md is the design: §4 the method, §4.8 why each constant has its value.
namespace sedef::dsp
{
namespace
{
using i64 = std::int64_t;
using Cplx = std::complex<double>;

// Partial model (§4.4)
constexpr double kWindowSeconds = 0.085; // N: the power of two nearest 85 ms
constexpr int kHopsPerWindow = 16;       // H = N / 16
constexpr int kMaxTracks = 128;          // partials (sinusoids) U and S synthesise
constexpr int kMaxPeakTracks = 96;       // spectral peaks followed from frame to frame
constexpr int kHistoryHops = 16;         // parameter history S can read back (power of two)
constexpr int kQuarantineHops = 16;      // a freed slot is not reused while S may still read it
constexpr double kPeakFloorDb = -70.0;
constexpr double kAbsoluteFloor = 1.0e-5; // amplitude, -100 dBFS
constexpr double kMinPartialHz = 40.0;
constexpr double kMaxPartialHz = 8000.0;
constexpr int kFrequencySpanHops = 8;
// Two partials closer than the window resolves share one peak and beat. Over the frames, the
// value at the peak's bin is then a sum of two rotating, decaying phasors: an order-2 linear
// prediction separates them (§4.4).
constexpr int kLpFrames = 12;          // frames fitted (64 ms at 48 kHz)
constexpr int kLpMinFrames = 6;        // fewer: one partial, from the phase advance
constexpr double kOrderGain = 0.25;    // order 2 must leave under a quarter of order 1's error
constexpr double kOrderFloor = 1.0e-4; // ... and order 1 must leave at least -40 dB
constexpr double kMinSecondShare = 0.03;
constexpr double kMaxComponentBins = 1.5;
constexpr double kRebinBins = 0.75;
constexpr double kClaimBins = 1.5; // a peak this close to a track's bin belongs to it
constexpr int kCoastFrames = 8;    // frames an established track lives without a peak
constexpr int kLeakBins = 6;       // neighbours whose leakage is taken out (Hann: -60 dB beyond)
constexpr double kShortestDecaySeconds = 0.005;
constexpr double kMatchRelative = 0.01;
constexpr double kPhaseLock = 0.25;     // share of the predicted phase error U corrects per hop
constexpr double kErrorSmoothing = 0.3; // per hop, for a track's prediction error
constexpr double kNewTrackError = 1.0;  // a new track starts at half weight
constexpr double kErrorWeight = 1.0;
// Hann side-lobe envelope ~0.415 / k^3 (k in bins, -31.5 dB at 2.5 bins), +6 dB of margin.
constexpr double kSidelobeMask = 0.83;
constexpr double kMaskMinBins = 1.5;

// Grains (§4.2, §4.6, §4.7)
constexpr double kLatencyDownMs = 3.0;
constexpr double kLatencyUpMs = 10.0;
constexpr double kOnsetFadeMs = 1.0;
constexpr double kDetectMs = 1.5;
constexpr double kSearchMs = 16.0;
constexpr double kGrowMarginMs = 5.0;
constexpr double kSpliceFadeMs = 10.0;
constexpr double kCorrelationMs = 10.0;
constexpr double kLookAheadMs = 2.0;
constexpr double kProtectMs = 5.0;
constexpr double kAttackHoldMs = 50.0; // past the flam window (40 ms after the attack peak)
constexpr double kLevelSeconds = 0.02; // residual and input level
constexpr double kLooseShare = 0.1;    // residual over -10 dB of the input: the model is not enough
constexpr double kLooseMarginMs = 2.0; // ... then a grain lives only 2 ms of growth past its span
constexpr double kPreferMargin = 0.1;  // splice offsets within 10 % of the best correlation
constexpr double kChangeFadeMs = 20.0;
constexpr double kHistorySeconds = 0.25; // residual history the grains read from
constexpr int kMaxGrains = 4;

// Onset detector (§4.6)
constexpr double kOnsetFastMs = 0.2;
constexpr double kOnsetFloorMs = 40.0;
constexpr double kOnsetRiseDb = 12.0;
constexpr double kOnsetInputRiseDb = 3.0;
constexpr double kOnsetRefractoryMs = 30.0;
constexpr double kOnsetBackMs = 3.0;
constexpr double kOnsetAbsFloor = 1.0e-12;

// Interpolator (§4.2): Kaiser-windowed sinc.
constexpr int kTapsHalfDown = 8; // r <= 1: 16 taps, interpolating (passes integer positions)
constexpr double kBetaDown = 8.0;
constexpr int kTapsHalfUp = 12; // r > 1: 24 r taps, cut-off scaled by 1 / r
constexpr double kBetaUp = 7.0;
constexpr double kUpCutoff = 0.80;   // of the output's Nyquist, r > 1
constexpr double kUpModelBand = 0.6; // of the output's Nyquist: S stays in the pass band
constexpr int kKernelResolution = 512;
constexpr int kPhases = 256; // interpolator table rows per sample (linear between rows)

inline size_t sz (i64 v) noexcept
{
    return static_cast<size_t> (v);
}

inline double wrapPi (double a) noexcept
{
    return a - kTwoPiD * std::floor ((a + kPiD) / kTwoPiD);
}

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1.0e-12 * sum)
            break;
    }
    return sum;
}

/// One side of a Kaiser-windowed sinc with cut-off `gamma` (1 = Nyquist), t in [0, half].
std::vector<float> kernelTable (int half, double gamma, double beta)
{
    std::vector<float> t (static_cast<size_t> (half * kKernelResolution + 2), 0.0f);
    const double norm = besselI0 (beta);
    for (size_t i = 0; i + 1 < t.size(); ++i)
    {
        const double x = static_cast<double> (i) / kKernelResolution;
        const double u = x / half;
        const double w = u < 1.0 ? besselI0 (beta * std::sqrt (1.0 - u * u)) / norm : 0.0;
        const double s = i == 0 ? 1.0 : std::sin (kPiD * gamma * x) / (kPiD * gamma * x);
        t[i] = static_cast<float> (gamma * s * w);
    }
    return t;
}

inline float kernelAt (const std::vector<float>& table, double x) noexcept
{
    const double ax = std::abs (x) * kKernelResolution;
    const auto i = static_cast<size_t> (ax);
    if (i + 1 >= table.size())
        return 0.0f;
    const auto f = static_cast<float> (ax - static_cast<double> (i));
    return table[i] + f * (table[i + 1] - table[i]);
}

/// Circular buffer written twice, so any window of up to `size` samples is contiguous.
struct MirrorRing
{
    std::vector<float> data;
    i64 size = 1;
    i64 mask = 0;

    void prepare (i64 minSize)
    {
        size = 1;
        while (size < minSize)
            size <<= 1;
        mask = size - 1;
        data.assign (sz (2 * size), 0.0f);
    }
    void clear() noexcept { std::fill (data.begin(), data.end(), 0.0f); }
    void write (i64 index, float x) noexcept
    {
        const i64 i = index & mask;
        data[sz (i)] = x;
        data[sz (i + size)] = x;
    }
    /// Pointer to sample `index`; the next `size` samples follow contiguously.
    const float* at (i64 index) const noexcept { return data.data() + (index & mask); }
};

/// A resampling interpolator as a table: kPhases + 1 rows of `taps` weights, row q for the
/// fraction q / kPhases; taps run from -(taps / 2 - 1) to taps / 2 around the integer position.
struct Polyphase
{
    std::vector<float> w;
    int taps = 0;

    /// The windowed-sinc `kernel` with its argument divided by `r` (so its cut-off scales by 1/r),
    /// reaching `reach` samples each side; taps are padded to a multiple of 8 with zeros.
    void build (const std::vector<float>& kernel, double reach, double r)
    {
        const int half = (static_cast<int> (std::ceil (reach)) + 3) / 4 * 4;
        taps = 2 * half;
        w.assign (sz ((kPhases + 1) * taps), 0.0f);
        for (int q = 0; q <= kPhases; ++q)
            for (int j = 0; j < taps; ++j)
            {
                const double x =
                    static_cast<double> (j - (half - 1)) - static_cast<double> (q) / kPhases;
                w[sz (q * taps + j)] = static_cast<float> (kernelAt (kernel, x / r) / r);
            }
    }
};

/// Sum of a[i] b[i] over n samples, in eight partial sums.
inline float dot (const float* __restrict a, const float* __restrict b, int n) noexcept
{
    constexpr size_t kLanes = 8;
    std::array<float, kLanes> acc {};
    const size_t m = sz (n) / kLanes * kLanes;
    for (size_t i = 0; i < m; i += kLanes)
        for (size_t k = 0; k < kLanes; ++k)
            acc[k] += a[i + k] * b[i + k];
    float sum = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
    for (size_t i = m; i < sz (n); ++i)
        sum += a[i] * b[i];
    return sum;
}

/// One sample of a bank of complex oscillators: returns sum a Re(z), then a += da, z *= w.
/// Eight independent partial sums in separate passes, so the compiler emits SIMD without
/// reordering any float addition.
inline float runOscillators (float* __restrict zr, float* __restrict zi, const float* __restrict wr,
                             const float* __restrict wi, float* __restrict a,
                             const float* __restrict da, int count) noexcept
{
    constexpr size_t kLanes = 8;
    std::array<float, kLanes> acc {};
    const size_t n = sz (count) / kLanes * kLanes;
    for (size_t k = 0; k < n; k += kLanes)
    {
        std::array<float, kLanes> re {}, im {};
        for (size_t j = 0; j < kLanes; ++j)
            acc[j] += a[k + j] * zr[k + j];
        for (size_t j = 0; j < kLanes; ++j)
            re[j] = zr[k + j] * wr[k + j] - zi[k + j] * wi[k + j];
        for (size_t j = 0; j < kLanes; ++j)
            im[j] = zr[k + j] * wi[k + j] + zi[k + j] * wr[k + j];
        for (size_t j = 0; j < kLanes; ++j)
            a[k + j] += da[k + j];
        for (size_t j = 0; j < kLanes; ++j)
            zr[k + j] = re[j];
        for (size_t j = 0; j < kLanes; ++j)
            zi[k + j] = im[j];
    }
    float sum = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
    for (size_t i = n; i < sz (count); ++i)
    {
        sum += a[i] * zr[i];
        a[i] += da[i];
        const float re = zr[i] * wr[i] - zi[i] * wi[i];
        zi[i] = zr[i] * wi[i] + zi[i] * wr[i];
        zr[i] = re;
    }
    return sum;
}

/// One hop of the model's parameters, as U synthesised them (S reads them back).
struct HopRecord
{
    i64 start = -1;
    float g0 = 1.0f, g1 = 1.0f; // confidence ramp
    int high = 0;               // slots in use are below this
    std::vector<std::uint32_t> gen;
    std::vector<std::uint8_t> on;
    std::vector<double> phase; // at the hop start
    std::vector<double> omega; // rad / sample over the hop
    std::vector<float> a0, a1; // raw amplitude ramp
};

struct Peak
{
    int bin = 0;
    double omega = 0.0;
    double logAmp = 0.0;
    double phase = 0.0;
    float amp = 0.0f;
    int track = -1;
};

struct Grain
{
    bool on = false;
    double anchorIn = 0.0; // input sample read at output sample anchorOut
    i64 anchorOut = 0;
    float base = 0.0f; // gain before the running fade

    double pos (i64 n, double r) const noexcept
    {
        return anchorIn + r * static_cast<double> (n - anchorOut);
    }
};

/// S: the model's partials, shifted, read where one grain reads (§4.5).
struct ShiftedModel
{
    std::vector<float> zr, zi, wr, wi, a, da, bias, biasStep;
    std::vector<double> theta;
    std::vector<std::uint32_t> gen;
    std::vector<std::uint8_t> live;
    float g = 1.0f, dg = 0.0f, gBias = 0.0f, gBiasStep = 0.0f;
    int segLeft = 0, biasLeft = 0, high = 0;
    int grain = 0;     // the grain whose read position S follows
    bool fresh = true; // starts every partial at once, in phase with that grain

    void prepare (size_t K)
    {
        for (auto* vec : { &zr, &zi, &wr, &wi, &a, &da, &bias, &biasStep })
            vec->assign (K, 0.0f);
        theta.assign (K, 0.0);
        gen.assign (K, 0);
        live.assign (K, 0);
    }

    void restart (int grainIndex) noexcept
    {
        std::fill (live.begin(), live.end(), 0);
        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (da.begin(), da.end(), 0.0f);
        std::fill (bias.begin(), bias.end(), 0.0f);
        std::fill (biasStep.begin(), biasStep.end(), 0.0f);
        g = 1.0f;
        dg = gBias = gBiasStep = 0.0f;
        segLeft = biasLeft = high = 0;
        grain = grainIndex;
        fresh = true;
    }
};

/// A shift amount: its grains and its shifted model S (§4.2, §4.5).
struct Voice
{
    bool on = false;
    int semitones = 0;
    double r = 1.0;
    const Polyphase* poly = nullptr; // the grains' interpolator
    int L = 0;
    double dMin = 0.0, dMax = 0.0;
    int spliceFade = 1;
    std::array<Grain, kMaxGrains> grains {};
    int newest = 0;
    bool fading = false;
    int fadeLen = 1, fadePos = 0;
    bool equalPower = true;
    float rho = 0.0f;
    i64 protect = std::numeric_limits<i64>::min();
    i64 holdUntil = 0;
    // S, in two banks: an attack re-anchors the grains and crossfades S from one bank (still on
    // the old grain) to the other (on the new grain), with the grains' own gains.
    std::array<ShiftedModel, 2> s;
    int sCur = 0;
    bool sCrossfade = false; // the running fade also crossfades the S banks
};

} // namespace

// -------------------------------------------------------------------------------------------------

struct PitchShifter::Impl
{
    // Configuration (prepare) ------------------------------------------------------------------
    double fs = 48000.0;
    int N = 4096, H = 256;
    int latencyDown = 144, latencyUp = 480;
    int onsetFade = 48, spliceFade = 480, changeFade = 960;
    int detect = 72, search = 768, growMargin = 240, corrLen = 480, lookAhead = 96;
    int protectLen = 240, refractory = 1440, onsetBack = 144, attackHold = 2400, looseMargin = 96;
    double minOmega = 0.0, maxOmegaBase = 0.0;
    double onsetFastCoef = 0.0, onsetFloorCoef = 0.0, onsetRise = 1.0, onsetInputRise = 1.0;
    double maxDecay = 0.0;               // per sample
    Polyphase polyDown;                  // every downward shift
    std::array<Polyphase, 12> polyUp {}; // +1..+12 semitones
    std::vector<float> hann;             // zero-phase order

    // Stream -------------------------------------------------------------------------------------
    i64 n = 0; // samples since reset
    int target = 0;
    bool snap = true;
    MirrorRing xh, eh;

    // Analysis -----------------------------------------------------------------------------------
    PFFFT_Setup* fft = nullptr;
    float* fftIn = nullptr;
    float* fftOut = nullptr;
    float* fftWork = nullptr;
    std::vector<Peak> peaks;
    bool modelOn = false;
    i64 modelStart = 0; // first sample U may sound

    // Tracks (structure of arrays, kMaxTracks)
    // ------------------------------------------------------ Peak tracks
    std::vector<std::uint8_t> pkOn, pkPredValid;
    std::vector<int> pkBin, pkAge, pkMissed, pkPeak, pkSlot; // pkSlot: [track * 2 + c] -> partial
    std::vector<double> pkOmega, pkErr, pkEnergy; // pkErr: smoothed relative prediction error
    std::vector<Cplx> pkZ, pkPred;                // [track * 16 + frame % 16]
    // The last fit, per track and component: bin value at the track's bin, its per-frame
    // rotation, and its offset from that bin (bins). Used to take each track's leakage out of
    // its neighbours' bins.
    std::vector<int> pkCount;
    std::vector<Cplx> pkBeta, pkRho;
    std::vector<double> pkOffset;
    std::vector<Cplx> pkZNow;
    // Partials
    std::vector<std::uint8_t> trOn, trDying;
    std::vector<std::uint32_t> trGen;
    std::vector<i64> trFreeAt;
    std::vector<double> trOmega;
    std::vector<double> uPhase0, uOmega; // current hop
    std::vector<float> uA0, uA1;
    std::uint32_t nextGen = 1;
    std::vector<float> uzr, uzi, uwr, uwi, ua, uda; // U's oscillators
    int uHigh = 0;
    float g0 = 1.0f, g1 = 1.0f, gNow = 1.0f, gStep = 0.0f;
    double sxu = 0.0, suu = 0.0, sxuPrev = 0.0, suuPrev = 0.0;
    std::vector<HopRecord> history;

    // How much of the input the model leaves in the residual (20 ms)
    double inputEnergy = 0.0, residualEnergy = 0.0, levelCoef = 0.0;

    // Onset detector ------------------------------------------------------------------------------
    float ePrev = 0.0f, xPrev = 0.0f;
    double ef = 0.0, efFloor = 0.0, xf = 0.0, xfFloor = 0.0;
    std::vector<float> efRing;
    i64 efMask = 0;
    i64 refractoryUntil = 0;

    // Voices --------------------------------------------------------------------------------------
    std::array<Voice, 2> voices;
    int cur = 0;          // the voice heading to the target
    int prev = -1;        // the voice fading out, or -1
    bool dryCur = true;   // the current state is the untouched input
    bool dryPrev = false; // the state fading out is the untouched input
    int changeLeft = 0;   // samples left in the setting-change fade
    std::int64_t onsetCount = 0, spliceCount = 0;
    // Splice search scratch: each candidate's score (kUnscored until computed) and the running
    // energy of the searched stretch.
    std::vector<double> spliceScore, spliceEnergy;

    ~Impl() { freeFft(); }

    void freeFft() noexcept
    {
        if (fft != nullptr)
            pffft_destroy_setup (fft);
        pffft_aligned_free (fftIn);
        pffft_aligned_free (fftOut);
        pffft_aligned_free (fftWork);
        fft = nullptr;
        fftIn = fftOut = fftWork = nullptr;
    }

    int ms (double v) const noexcept
    {
        return std::max (1, static_cast<int> (std::lround (v * 0.001 * fs)));
    }

    // ---------------------------------------------------------------------------------------------

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        // The power of two nearest (geometrically) to 85 ms: 4096 at 44.1-48 kHz, 8192 at 88-96.
        N = 1;
        while (static_cast<double> (N) * std::sqrt (2.0) < kWindowSeconds * fs)
            N <<= 1;
        H = N / kHopsPerWindow;
        latencyDown = PitchShifter::latencyFor (-1, fs);
        latencyUp = PitchShifter::latencyFor (1, fs);
        onsetFade = ms (kOnsetFadeMs);
        spliceFade = ms (kSpliceFadeMs);
        changeFade = ms (kChangeFadeMs);
        detect = ms (kDetectMs);
        search = ms (kSearchMs);
        growMargin = ms (kGrowMarginMs);
        corrLen = ms (kCorrelationMs);
        spliceScore.assign (sz (search + 2), 0.0);
        spliceEnergy.assign (sz (search + corrLen + 2), 0.0);
        lookAhead = ms (kLookAheadMs);
        protectLen = ms (kProtectMs);
        attackHold = ms (kAttackHoldMs);
        looseMargin = ms (kLooseMarginMs);
        refractory = ms (kOnsetRefractoryMs);
        onsetBack = ms (kOnsetBackMs);
        minOmega = kTwoPiD * kMinPartialHz / fs;
        maxOmegaBase = kTwoPiD * std::min (kMaxPartialHz, 0.45 * fs) / fs;
        onsetFastCoef = 1.0 - std::exp (-1.0 / (kOnsetFastMs * 0.001 * fs));
        onsetFloorCoef = 1.0 - std::exp (-1.0 / (kOnsetFloorMs * 0.001 * fs));
        onsetRise = std::pow (10.0, kOnsetRiseDb / 10.0);
        onsetInputRise = std::pow (10.0, kOnsetInputRiseDb / 10.0);
        maxDecay = 1.0 / (kShortestDecaySeconds * fs);
        levelCoef = 1.0 - std::exp (-1.0 / (kLevelSeconds * fs));

        polyDown.build (kernelTable (kTapsHalfDown, 1.0, kBetaDown), kTapsHalfDown, 1.0);
        const auto kernelUp = kernelTable (kTapsHalfUp, kUpCutoff, kBetaUp);
        for (int st = 1; st <= 12; ++st)
        {
            const double r = std::exp2 (st / 12.0);
            polyUp[sz (st - 1)].build (kernelUp, kTapsHalfUp * r, r);
        }

        hann.assign (sz (N), 0.0f);
        for (int m = -N / 2; m < N / 2; ++m)
            hann[sz ((m + N) & (N - 1))] =
                static_cast<float> (0.5 + 0.5 * std::cos (kTwoPiD * m / static_cast<double> (N)));

        xh.prepare (N + H);
        eh.prepare (static_cast<i64> (kHistorySeconds * fs));

        freeFft();
        fft = pffft_new_setup (N, PFFFT_REAL);
        const size_t bytes = sizeof (float) * sz (N);
        fftIn = static_cast<float*> (pffft_aligned_malloc (bytes));
        fftOut = static_cast<float*> (pffft_aligned_malloc (bytes));
        fftWork = static_cast<float*> (pffft_aligned_malloc (bytes));
        peaks.assign (sz (N / 2), Peak {});

        const auto P = sz (kMaxPeakTracks);
        pkOn.assign (P, 0);
        pkMissed.assign (P, 0);
        pkPeak.assign (P, -1);
        pkPredValid.assign (P * 16, 0);
        pkBin.assign (P, 0);
        pkAge.assign (P, 0);
        pkSlot.assign (P * 2, -1);
        pkOmega.assign (P, 0.0);
        pkErr.assign (P, 0.0);
        pkEnergy.assign (P, 0.0);
        pkZ.assign (P * 16, Cplx (0.0, 0.0));
        pkCount.assign (P, 0);
        pkBeta.assign (P * 2, Cplx (0.0, 0.0));
        pkRho.assign (P * 2, Cplx (0.0, 0.0));
        pkOffset.assign (P * 2, 0.0);
        pkZNow.assign (P, Cplx (0.0, 0.0));
        pkPred.assign (P * 16, Cplx (0.0, 0.0));
        const auto K = sz (kMaxTracks);
        trOn.assign (K, 0);
        trDying.assign (K, 0);
        trGen.assign (K, 0);
        trFreeAt.assign (K, 0);
        trOmega.assign (K, 0.0);
        uPhase0.assign (K, 0.0);
        uOmega.assign (K, 0.0);
        for (auto* v : { &uA0, &uA1, &uzr, &uzi, &uwr, &uwi, &ua, &uda })
            v->assign (K, 0.0f);
        history.assign (sz (kHistoryHops), HopRecord {});
        for (auto& h : history)
        {
            h.gen.assign (K, 0);
            h.on.assign (K, 0);
            h.phase.assign (K, 0.0);
            h.omega.assign (K, 0.0);
            h.a0.assign (K, 0.0f);
            h.a1.assign (K, 0.0f);
        }
        i64 ringSize = 1;
        while (ringSize < onsetBack + 4)
            ringSize <<= 1;
        efRing.assign (sz (ringSize), 0.0f);
        efMask = ringSize - 1;
        for (auto& v : voices)
            for (auto& bank : v.s)
                bank.prepare (K);
    }

    void reset() noexcept
    {
        n = 0;
        snap = true;
        xh.clear();
        eh.clear();
        resetModel();
        ePrev = xPrev = 0.0f;
        ef = efFloor = xf = xfFloor = 0.0;
        inputEnergy = residualEnergy = 0.0;
        std::fill (efRing.begin(), efRing.end(), 0.0f);
        refractoryUntil = 0;
        for (auto& v : voices)
            v.on = false;
        dryCur = true;
        dryPrev = false;
        prev = -1;
        changeLeft = 0;
        onsetCount = spliceCount = 0;
    }

    void resetModel() noexcept
    {
        modelOn = false;
        std::fill (pkOn.begin(), pkOn.end(), 0);
        std::fill (pkSlot.begin(), pkSlot.end(), -1);
        std::fill (trOn.begin(), trOn.end(), 0);
        std::fill (trDying.begin(), trDying.end(), 0);
        std::fill (trFreeAt.begin(), trFreeAt.end(), 0);
        std::fill (ua.begin(), ua.end(), 0.0f);
        std::fill (uda.begin(), uda.end(), 0.0f);
        uHigh = 0;
        g0 = g1 = gNow = 1.0f;
        gStep = 0.0f;
        sxu = suu = sxuPrev = suuPrev = 0.0;
        for (auto& h : history)
        {
            h.start = -1;
            h.high = 0;
            std::fill (h.on.begin(), h.on.end(), 0);
        }
    }

    // Voices and setting changes (§6) -------------------------------------------------------------

    void startVoice (Voice& v, int st) noexcept
    {
        v.on = true;
        v.semitones = st;
        v.r = std::exp2 (st / 12.0);
        v.poly = st < 0 ? &polyDown : &polyUp[sz (st - 1)];
        v.L = st < 0 ? latencyDown : latencyUp;
        // A grain never gets closer to the input than the detection delay plus the onset fade it
        // needs to leave before a new attack, plus the interpolator's reach.
        v.dMin = detect + v.r * onsetFade + std::max (kTapsHalfUp, v.poly->taps / 2);
        v.dMax = v.L + search + growMargin;
        // Upward, a grain lives (L - d_min) / (r - 1) samples from the latency down to d_min, and
        // its splice fade must end before it gets there: at most half of that.
        v.spliceFade =
            v.r <= 1.0
                ? spliceFade
                : std::max (
                      onsetFade,
                      std::min (spliceFade, static_cast<int> (0.5 * (v.L - v.dMin) / (v.r - 1.0))));
        for (auto& gr : v.grains)
            gr.on = false;
        v.grains[0] = { true, static_cast<double> (n - v.L), n, 1.0f };
        v.newest = 0;
        v.fading = false;
        v.protect = std::numeric_limits<i64>::min();
        v.holdUntil = 0;
        v.sCur = 0;
        v.sCrossfade = false;
        v.s[0].restart (0);
        v.s[1].restart (0);
    }

    void startModel() noexcept
    {
        resetModel();
        modelOn = true;
        modelStart = n;
    }

    /// Settings arrive per block; a change starts at a block boundary and waits for a running
    /// change fade to finish.
    void applyTarget() noexcept
    {
        if (snap)
        {
            snap = false;
            dryCur = target == 0;
            if (!dryCur)
            {
                cur = 0;
                startModel();
                startVoice (voices[0], target);
            }
            return;
        }
        if (changeLeft > 0)
            return;
        const int now = dryCur ? 0 : voices[sz (cur)].semitones;
        if (now == target)
            return;
        dryPrev = dryCur;
        prev = dryCur ? -1 : cur;
        if (target == 0)
            dryCur = true;
        else
        {
            cur = dryCur ? 0 : 1 - cur;
            if (dryCur)
                startModel();
            dryCur = false;
            startVoice (voices[sz (cur)], target);
        }
        changeLeft = changeFade;
    }

    // The partial model U (§4.4) ------------------------------------------------------------------

    double maxModelOmega() const noexcept
    {
        double r = 1.0;
        for (const auto& v : voices)
            if (v.on)
                r = std::max (r, v.r);
        if (r <= 1.0)
            return maxOmegaBase;
        return std::min (maxOmegaBase, kPiD * kUpModelBand / r);
    }

    /// The Hann window's transform at d bins from a partial, relative to its peak (signed).
    static double hannResponse (double d) noexcept
    {
        const double a = std::abs (d);
        if (a < 1.0e-6)
            return 1.0;
        if (std::abs (a - 1.0) < 1.0e-6)
            return 0.5;
        return std::sin (kPiD * d) / (kPiD * d) / (1.0 - d * d);
    }

    struct Component
    {
        double omega = 0.0, amp = 0.0, phase = 0.0; // at the window centre
        Cplx rho { 1.0, 0.0 };                      // per-frame rotation and decay of the bin value
        Cplx beta { 0.0, 0.0 };                     // the bin value it contributes now
    };

    /// One or two partials explaining peak track kk's bin value over its last frames (§4.4).
    int fitPeak (size_t kk, i64 hop, int age, int bin, const Peak& p, Component* out) const noexcept
    {
        const double binOmega = kTwoPiD / N;
        const double omegaBin = bin * binOmega;
        const auto zAt = [&] (int back) {
            return pkZ[kk * 16 + sz ((hop - back) & 15)];
        };
        const auto make = [&] (Cplx rho, Cplx beta, Component& c) {
            // The per-frame rotation is ambiguous by 2 pi / H: take the frequency nearest the bin.
            const double a = std::arg (rho);
            c.omega = (a + kTwoPiD * std::round ((omegaBin * H - a) / kTwoPiD)) / H;
            const double d = (c.omega - omegaBin) / binOmega;
            if (std::abs (d) > kMaxComponentBins)
                return false;
            const double gain = hannResponse (d);
            const double mag = std::abs (rho);
            c.rho = mag > 1.0 ? rho * (1.0 / mag) : rho; // never a predicted growth
            c.beta = beta;
            c.amp = 4.0 * std::abs (beta) / (N * gain);
            c.phase = std::arg (beta);
            return true;
        };
        const int M = std::min (age, kLpFrames);
        if (M >= kLpMinFrames)
        {
            // Order 1 and order 2 linear prediction over the last M frames (j frames back).
            Cplx A11 (0.0, 0.0), A12 (0.0, 0.0), A22 (0.0, 0.0), b1 (0.0, 0.0), b2 (0.0, 0.0);
            double energy = 0.0;
            for (int j = 0; j + 2 < M; ++j)
            {
                const Cplx y = zAt (j), u1 = zAt (j + 1), u2 = zAt (j + 2);
                A11 += std::norm (u1);
                A12 += std::conj (u1) * u2;
                A22 += std::norm (u2);
                b1 += std::conj (u1) * y;
                b2 += std::conj (u2) * y;
                energy += std::norm (y);
            }
            // A11, A22 and the determinant are real: divide by reals, not complex numbers.
            const Cplx alpha = b1 * (1.0 / (A11.real() + 1.0e-30));
            const double ridge = 1.0e-9 * (A11.real() + A22.real()) + 1.0e-30;
            const double a11 = A11.real() + ridge, a22 = A22.real() + ridge;
            const double det = a11 * a22 - std::norm (A12);
            double r1 = 0.0, r2 = 0.0;
            Cplx a1 (0.0, 0.0), a2 (0.0, 0.0);
            if (std::abs (det) > 1.0e-30)
            {
                const double inv = 1.0 / det;
                a1 = (a22 * b1 - A12 * b2) * inv;
                a2 = (a11 * b2 - std::conj (A12) * b1) * inv;
                for (int j = 0; j + 2 < M; ++j)
                {
                    const Cplx y = zAt (j), u1 = zAt (j + 1), u2 = zAt (j + 2);
                    r1 += std::norm (y - alpha * u1);
                    r2 += std::norm (y - a1 * u1 - a2 * u2);
                }
            }
            if (r1 > kOrderFloor * energy && r2 < kOrderGain * r1)
            {
                const Cplx disc = std::sqrt (a1 * a1 + 4.0 * a2);
                const Cplx rho1 = 0.5 * (a1 + disc), rho2 = 0.5 * (a1 - disc);
                // Their values now, by least squares over the M frames: z_j = sum beta rho^-j.
                Cplx G11 (0.0, 0.0), G12 (0.0, 0.0), G22 (0.0, 0.0), h1 (0.0, 0.0), h2 (0.0, 0.0);
                Cplx v1 (1.0, 0.0), v2 (1.0, 0.0);
                const Cplx i1 = std::conj (rho1) * (1.0 / (std::norm (rho1) + 1.0e-300));
                const Cplx i2 = std::conj (rho2) * (1.0 / (std::norm (rho2) + 1.0e-300));
                for (int j = 0; j < M; ++j)
                {
                    const Cplx y = zAt (j);
                    G11 += std::norm (v1);
                    G12 += std::conj (v1) * v2;
                    G22 += std::norm (v2);
                    h1 += std::conj (v1) * y;
                    h2 += std::conj (v2) * y;
                    v1 *= i1;
                    v2 *= i2;
                }
                const double gdet = G11.real() * G22.real() - std::norm (G12);
                if (std::abs (gdet) > 1.0e-12 * std::norm (G11 + G22))
                {
                    const double inv = 1.0 / gdet;
                    const Cplx beta1 = (G22 * h1 - G12 * h2) * inv;
                    const Cplx beta2 = (G11 * h2 - std::conj (G12) * h1) * inv;
                    Component c1, c2;
                    // Two near-equal roots fit with huge opposite amplitudes: not two partials.
                    double zMax = 0.0;
                    for (int j = 0; j < M; ++j)
                        zMax = std::max (zMax, std::abs (zAt (j)));
                    if (std::abs (beta1) < 4.0 * zMax && std::abs (beta2) < 4.0 * zMax &&
                        make (rho1, beta1, c1) && make (rho2, beta2, c2) &&
                        std::min (c1.amp, c2.amp) > kMinSecondShare * std::max (c1.amp, c2.amp))
                    {
                        out[0] = c1.amp >= c2.amp ? c1 : c2;
                        out[1] = c1.amp >= c2.amp ? c2 : c1;
                        return 2;
                    }
                }
            }
            if (make (alpha, zAt (0), out[0]))
                return 1;
        }
        // A young track: its rotation from the phase advance at its bin over q frames.
        Cplx rho = std::polar (1.0, p.omega * H);
        const int q = std::min (age - 1, kFrequencySpanHops);
        if (q > 0)
        {
            const Cplx z0 = zAt (0), zq = zAt (q);
            const double dphi = std::arg (z0) - std::arg (zq);
            const double span = static_cast<double> (q) * H;
            double omega = (dphi + kTwoPiD * std::round ((p.omega * span - dphi) / kTwoPiD)) / span;
            if (std::abs (omega - p.omega) > 0.3 * binOmega)
                omega = p.omega;
            const double decay = std::clamp (
                std::log (std::abs (zq) / std::max (std::abs (z0), 1.0e-30)) / span, 0.0, maxDecay);
            rho = std::polar (std::exp (-decay * H), omega * H);
        }
        if (make (rho, zAt (0), out[0]))
            return 1;
        out[0].omega = p.omega;
        out[0].amp = std::exp (p.logAmp);
        out[0].phase = p.phase;
        out[0].rho = std::polar (1.0, p.omega * H);
        out[0].beta = zAt (0);
        return 1;
    }

    /// A new partial whose phase at this hop's start is phase0; -1 when every slot is taken.
    int startPartial (i64 hop, double phase0, double omega) noexcept
    {
        for (int k = 0; k < kMaxTracks; ++k)
        {
            const auto kk = sz (k);
            if (trOn[kk] || trFreeAt[kk] > hop)
                continue;
            trOn[kk] = 1;
            trDying[kk] = 0;
            trGen[kk] = nextGen++;
            trOmega[kk] = omega;
            uPhase0[kk] = wrapPi (phase0);
            uOmega[kk] = omega;
            uA0[kk] = uA1[kk] = 0.0f;
            uHigh = std::max (uHigh, k + 1);
            return k;
        }
        return -1;
    }

    /// The partial ramps out over this hop at its last frequency.
    void endPartial (int slot) noexcept
    {
        if (slot < 0 || !trOn[sz (slot)] || trDying[sz (slot)])
            return;
        trDying[sz (slot)] = 1;
        uA1[sz (slot)] = 0.0f;
    }

    void analyse (i64 t) noexcept
    {
        // Frame x[t - N, t), zero-phase: the centre c = t - N/2 lands at index 0.
        const float* src = xh.at (t - N);
        for (int i = 0; i < N; ++i)
        {
            const int idx = (i - N / 2 + N) & (N - 1);
            fftIn[idx] = src[i] * hann[sz (idx)];
        }
        pffft_transform_ordered (fft, fftIn, fftOut, fftWork, PFFFT_FORWARD);

        const double binOmega = kTwoPiD / N;
        const int kLo = std::max (2, static_cast<int> (minOmega / binOmega));
        const int kHi = std::min (N / 2 - 2, static_cast<int> (maxModelOmega() / binOmega));
        const auto mag2 = [this] (int k) {
            const double re = fftOut[2 * k], im = fftOut[2 * k + 1];
            return re * re + im * im;
        };
        int count = 0;
        double maxAmp = 0.0;
        double prevM = mag2 (kLo - 1), m0 = mag2 (kLo);
        for (int k = kLo; k <= kHi; ++k)
        {
            const double next = mag2 (k + 1);
            if (m0 > prevM && m0 >= next && m0 > 1.0e-24)
            {
                const double ym1 = 0.5 * std::log (prevM + 1.0e-30);
                const double y0 = 0.5 * std::log (m0 + 1.0e-30);
                const double yp1 = 0.5 * std::log (next + 1.0e-30);
                const double den = ym1 - 2.0 * y0 + yp1;
                const double d = den < 0.0 ? std::clamp (0.5 * (ym1 - yp1) / den, -0.5, 0.5) : 0.0;
                const double logPeak = y0 - 0.25 * (ym1 - yp1) * d;
                const double amp = 4.0 * std::exp (logPeak) / N;
                if (amp > kAbsoluteFloor)
                {
                    auto& p = peaks[sz (count++)];
                    p.omega = (k + d) * binOmega;
                    p.logAmp = logPeak; // made the amplitude's log, and the phase set, once kept
                    p.amp = static_cast<float> (amp);
                    p.bin = k;
                    p.track = -1;
                    maxAmp = std::max (maxAmp, amp);
                }
            }
            prevM = m0;
            m0 = next;
        }
        // Keep the strongest, down to the floor under the strongest.
        const auto floorAmp = static_cast<float> (maxAmp * std::pow (10.0, kPeakFloorDb / 20.0));
        int kept = 0;
        for (int i = 0; i < count; ++i)
            if (peaks[sz (i)].amp >= floorAmp)
                peaks[sz (kept++)] = peaks[sz (i)];
        std::sort (peaks.begin(), peaks.begin() + kept,
                   [] (const Peak& a, const Peak& b) { return a.amp > b.amp; });
        // A Hann window's side lobes are local maxima too (-31 dB at 2.5 bins, falling 18 dB per
        // octave): keep a peak only above the side lobes of every stronger one kept.
        {
            int accepted = 0;
            for (int i = 0; i < kept && accepted < 2 * kMaxTracks; ++i)
            {
                const auto& p = peaks[sz (i)];
                bool masked = false;
                for (int j = 0; j < accepted && !masked; ++j)
                {
                    const auto& q = peaks[sz (j)];
                    const double bins = std::abs (p.omega - q.omega) / binOmega;
                    masked =
                        bins < kMaskMinBins || p.amp < q.amp * kSidelobeMask / (bins * bins * bins);
                }
                if (!masked)
                    peaks[sz (accepted++)] = p;
            }
            kept = std::min (accepted, kMaxPeakTracks);
        }
        for (int i = 0; i < kept; ++i)
        {
            auto& p = peaks[sz (i)];
            p.logAmp += std::log (4.0 / N);
            p.phase = std::atan2 (static_cast<double> (fftOut[2 * p.bin + 1]),
                                  static_cast<double> (fftOut[2 * p.bin]));
        }

        const i64 hop = t / H;
        const int ahead = N / (2 * H) + 1; // hops from a window centre to the end of this hop
        const double ahead0 = N / 2.0;     // from the window centre to this hop's start
        const double ahead1 = ahead0 + H;  // ... to its end
        const auto binValue = [this] (int k) {
            return Cplx (fftOut[2 * k], fftOut[2 * k + 1]);
        };

        // Confidence: U's least-squares gain onto x over the last two hops (§4.4).
        const double num = sxu + sxuPrev, den = suu + suuPrev;
        const float gNext =
            den > 1.0e-12 ? static_cast<float> (std::clamp (num / den, 0.0, 1.0)) : 1.0f;
        sxuPrev = sxu;
        suuPrev = suu;
        sxu = suu = 0.0;
        g0 = g1;
        g1 = gNext;

        // Partials of the last hop: continue at their phase, or finish ramping out.
        for (int k = 0; k < uHigh; ++k)
        {
            const auto kk = sz (k);
            if (!trOn[kk])
                continue;
            uPhase0[kk] = wrapPi (uPhase0[kk] + uOmega[kk] * H);
            uA0[kk] = uA1[kk];
            if (trDying[kk])
            {
                trOn[kk] = 0;
                trDying[kk] = 0;
                trFreeAt[kk] = hop + kQuarantineHops;
                uA0[kk] = uA1[kk] = 0.0f;
            }
        }

        // Claim: each peak goes to the nearest live track around its bin (strongest peak first).
        std::fill (pkPeak.begin(), pkPeak.end(), -1);
        for (int i = 0; i < kept; ++i)
        {
            auto& p = peaks[sz (i)];
            const double tol = std::max (kClaimBins * binOmega, kMatchRelative * p.omega);
            int best = -1;
            double bestDist = tol;
            for (int k = 0; k < kMaxPeakTracks; ++k)
            {
                const auto kk = sz (k);
                if (!pkOn[kk] || pkPeak[kk] >= 0)
                    continue;
                const double dist = std::abs (pkOmega[kk] - p.omega);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    best = k;
                }
            }
            if (best >= 0)
            {
                p.track = best;
                pkPeak[sz (best)] = i;
            }
        }
        // A track without a peak coasts on its own bin for a while once its fit is established
        // (two beating partials have a null every beat); otherwise it ends.
        for (int k = 0; k < kMaxPeakTracks; ++k)
        {
            const auto kk = sz (k);
            if (!pkOn[kk])
                continue;
            if (pkPeak[kk] >= 0)
            {
                pkMissed[kk] = 0;
                continue;
            }
            const bool coast = pkAge[kk] >= kLpMinFrames && ++pkMissed[kk] <= kCoastFrames &&
                               4.0 * std::abs (binValue (pkBin[kk])) / N > kAbsoluteFloor;
            if (coast)
                continue;
            pkOn[kk] = 0;
            for (int c = 0; c < 2; ++c)
            {
                endPartial (pkSlot[kk * 2 + sz (c)]);
                pkSlot[kk * 2 + sz (c)] = -1;
            }
        }
        // Peaks left over start tracks, unless a track already measures that bin.
        for (int i = 0; i < kept; ++i)
        {
            auto& p = peaks[sz (i)];
            if (p.track >= 0)
                continue;
            const int bin = static_cast<int> (std::lround (p.omega / binOmega));
            bool taken = false;
            for (int k = 0; k < kMaxPeakTracks && !taken; ++k)
                taken = pkOn[sz (k)] && std::abs (pkBin[sz (k)] - bin) <= 1;
            if (taken)
                continue;
            int k = 0;
            while (k < kMaxPeakTracks && pkOn[sz (k)])
                ++k;
            if (k >= kMaxPeakTracks)
                break;
            const auto kk = sz (k);
            pkOn[kk] = 1;
            pkBin[kk] = bin;
            pkOmega[kk] = p.omega;
            pkAge[kk] = 0;
            pkMissed[kk] = 0;
            pkCount[kk] = 0;
            pkErr[kk] = kNewTrackError;
            pkSlot[kk * 2] = pkSlot[kk * 2 + 1] = -1;
            for (size_t j = 0; j < 16; ++j)
                pkPredValid[kk * 16 + j] = 0;
            pkPeak[kk] = i;
            p.track = k;
        }

        // Each track's bin value without its neighbours' leakage: their last fits, one frame on,
        // seen through the window at this bin.
        for (int tk = 0; tk < kMaxPeakTracks; ++tk)
        {
            const auto kk = sz (tk);
            if (!pkOn[kk])
                continue;
            // A track that just moved bins is re-measured below; this is only a first value.
            Cplx z = binValue (pkBin[kk]);
            for (int j = 0; j < kMaxPeakTracks; ++j)
            {
                const auto jj = sz (j);
                if (j == tk || !pkOn[jj] || std::abs (pkBin[jj] - pkBin[kk]) > kLeakBins)
                    continue;
                for (int c = 0; c < pkCount[jj]; ++c)
                {
                    const auto jc = jj * 2 + sz (c);
                    const double own = hannResponse (pkOffset[jc]);
                    if (own < 0.1)
                        continue;
                    const double there = pkOffset[jc] + (pkBin[jj] - pkBin[kk]);
                    z -= pkBeta[jc] * pkRho[jc] * (hannResponse (there) / own);
                }
            }
            pkZNow[kk] = z;
        }

        // Each track: measure at its own bin, fit one or two partials, predict, set U.
        for (int tk = 0; tk < kMaxPeakTracks; ++tk)
        {
            const auto kk = sz (tk);
            if (!pkOn[kk])
                continue;
            Peak p;
            if (pkPeak[kk] >= 0)
                p = peaks[sz (pkPeak[kk])];
            else
            {
                // Coasting: what the bin itself says.
                const Cplx zb = binValue (pkBin[kk]);
                p.omega = pkOmega[kk];
                p.logAmp = std::log (std::max (4.0 * std::abs (zb) / N, 1.0e-30));
                p.phase = std::arg (zb);
            }
            if (pkAge[kk] < kLpMinFrames)
                pkOmega[kk] = p.omega; // a young track follows its peak
            if (std::abs (pkOmega[kk] / binOmega - pkBin[kk]) > kRebinBins)
            {
                // The partial moved (a bend): measure at its new bin from scratch.
                pkBin[kk] = static_cast<int> (std::lround (pkOmega[kk] / binOmega));
                pkAge[kk] = 0;
                for (size_t j = 0; j < 16; ++j)
                    pkPredValid[kk * 16 + j] = 0;
            }
            const int bin = pkBin[kk];
            const Cplx z = pkAge[kk] == 0 ? binValue (bin) : pkZNow[kk];
            const int age = ++pkAge[kk];
            pkZ[kk * 16 + sz (hop & 15)] = z;
            pkEnergy[kk] = age == 1
                               ? std::norm (z)
                               : pkEnergy[kk] + kErrorSmoothing * (std::norm (z) - pkEnergy[kk]);
            // How well the frame `ahead` hops ago predicted this one: that is the distance U
            // predicts over. Error relative to the track's recent level (not this frame's, which
            // is near zero at a beat's null), smoothed.
            if (pkPredValid[kk * 16 + sz (hop & 15)])
            {
                const Cplx zp = pkPred[kk * 16 + sz (hop & 15)];
                const double rel =
                    std::min (4.0, std::norm (z - zp) / std::max (pkEnergy[kk], 1.0e-30));
                pkErr[kk] += kErrorSmoothing * (rel - pkErr[kk]);
            }
            pkPredValid[kk * 16 + sz (hop & 15)] = 0;

            Component comps[2];
            const int count2 = fitPeak (kk, hop, age, bin, p, comps);
            pkOmega[kk] = comps[0].omega;
            pkCount[kk] = count2;
            for (int c = 0; c < count2; ++c)
            {
                pkBeta[kk * 2 + sz (c)] = comps[c].beta;
                pkRho[kk * 2 + sz (c)] = comps[c].rho;
                pkOffset[kk * 2 + sz (c)] = (comps[c].omega - bin * binOmega) / binOmega;
            }

            // Predict this bin's value `ahead` frames on, to grade the fit later.
            Cplx zAhead (0.0, 0.0);
            for (int c = 0; c < count2; ++c)
                zAhead += comps[c].beta * std::pow (comps[c].rho, static_cast<double> (ahead));
            pkPred[kk * 16 + sz ((hop + ahead) & 15)] = zAhead;
            pkPredValid[kk * 16 + sz ((hop + ahead) & 15)] = 1;

            // Partials: keep each slot on the component nearest its frequency.
            int* slots = &pkSlot[kk * 2];
            bool used[2] = { false, false };
            int slotFor[2] = { -1, -1 };
            for (int c = 0; c < count2; ++c)
            {
                int best = -1;
                double bestDist = 1.0e9;
                for (int s2 = 0; s2 < 2; ++s2)
                    if (slots[s2] >= 0 && !used[s2])
                        if (const double dd = std::abs (trOmega[sz (slots[s2])] - comps[c].omega);
                            dd < bestDist)
                        {
                            bestDist = dd;
                            best = s2;
                        }
                if (best >= 0)
                {
                    used[best] = true;
                    slotFor[c] = slots[best];
                }
            }
            for (int s2 = 0; s2 < 2; ++s2)
                if (slots[s2] >= 0 && !used[s2])
                {
                    endPartial (slots[s2]);
                    slots[s2] = -1;
                }
            const double weight = 1.0 / (1.0 + kErrorWeight * pkErr[kk]);
            for (int c = 0; c < count2; ++c)
            {
                const auto& comp = comps[c];
                int slot = slotFor[c];
                const double amp = comp.amp * std::pow (std::abs (comp.rho), ahead1 / H);
                if (slot < 0)
                {
                    slot = startPartial (hop, comp.phase + comp.omega * ahead0, comp.omega);
                    if (slot < 0)
                        continue;
                    for (int s2 = 0; s2 < 2; ++s2)
                        if (slots[s2] < 0)
                        {
                            slots[s2] = slot;
                            break;
                        }
                }
                const auto ks = sz (slot);
                trOmega[ks] = comp.omega;
                const double goal = comp.phase + comp.omega * ahead1;
                uOmega[ks] =
                    comp.omega + kPhaseLock * wrapPi (goal - uPhase0[ks] - comp.omega * H) / H;
                uA1[ks] = static_cast<float> (weight * amp);
            }
        }

        // Publish the hop for S, and set U's oscillators.
        auto& rec = history[sz (hop & (kHistoryHops - 1))];
        rec.start = t;
        rec.g0 = g0;
        rec.g1 = g1;
        int high = 0;
        for (int k = 0; k < uHigh; ++k)
        {
            const auto kk = sz (k);
            const bool live = trOn[kk] != 0;
            rec.on[kk] = live ? 1 : 0;
            if (!live)
            {
                ua[kk] = uda[kk] = 0.0f;
                continue;
            }
            high = k + 1;
            rec.gen[kk] = trGen[kk];
            rec.phase[kk] = uPhase0[kk];
            rec.omega[kk] = uOmega[kk];
            rec.a0[kk] = uA0[kk];
            rec.a1[kk] = uA1[kk];
            uzr[kk] = static_cast<float> (std::cos (uPhase0[kk]));
            uzi[kk] = static_cast<float> (std::sin (uPhase0[kk]));
            uwr[kk] = static_cast<float> (std::cos (uOmega[kk]));
            uwi[kk] = static_cast<float> (std::sin (uOmega[kk]));
            ua[kk] = uA0[kk];
            uda[kk] = (uA1[kk] - uA0[kk]) / static_cast<float> (H);
        }
        for (int k = high; k < kMaxTracks; ++k)
            rec.on[sz (k)] = 0;
        rec.high = high;
        uHigh = high;
        gNow = g0;
        gStep = (g1 - g0) / static_cast<float> (H);
    }

    /// U(n) before the confidence gain; advances the oscillators.
    float synthU() noexcept
    {
        return runOscillators (uzr.data(), uzi.data(), uwr.data(), uwi.data(), ua.data(),
                               uda.data(), uHigh);
    }

    // Onsets (§4.6) -------------------------------------------------------------------------------

    /// Feeds one sample of residual and input; returns the onset's anchor, or -1.
    i64 detectOnset (float e, float x) noexcept
    {
        const double de = static_cast<double> (e) - ePrev;
        const double dx = static_cast<double> (x) - xPrev;
        ePrev = e;
        xPrev = x;
        ef += onsetFastCoef * (de * de - ef);
        efFloor += onsetFloorCoef * (ef - efFloor);
        xf += onsetFastCoef * (dx * dx - xf);
        xfFloor += onsetFloorCoef * (xf - xfFloor);
        efRing[sz (n & efMask)] = static_cast<float> (ef);
        if (n < refractoryUntil || ef <= onsetRise * efFloor + kOnsetAbsFloor ||
            xf <= onsetInputRise * xfFloor)
            return -1;
        refractoryUntil = n + refractory;
        // The anchor: the first sample of the rise.
        const double floorAt = 2.0 * efFloor + kOnsetAbsFloor;
        i64 m = n;
        while (n - m < onsetBack && efRing[sz ((m - 1) & efMask)] > floorAt)
            --m;
        return m;
    }

    // Grains (§4.2, §4.6, §4.7) -------------------------------------------------------------------

    /// The residual history at fractional position p, through the voice's interpolator.
    float readAt (double p, const Polyphase& t) const noexcept
    {
        const auto i0 = static_cast<i64> (std::floor (p));
        const double q = (p - static_cast<double> (i0)) * kPhases;
        const auto qi = static_cast<size_t> (q);
        const auto qf = static_cast<float> (q - static_cast<double> (qi));
        const auto taps = sz (t.taps);
        const float* __restrict w0 = t.w.data() + qi * taps;
        const float* __restrict w1 = w0 + taps;
        const float* __restrict x = eh.at (i0 - (t.taps / 2 - 1));
        constexpr size_t kLanes = 8;
        std::array<float, kLanes> acc {};
        for (size_t k = 0; k < taps; k += kLanes)
            for (size_t j = 0; j < kLanes; ++j)
                acc[j] += x[k + j] * (w0[k + j] + qf * (w1[k + j] - w0[k + j]));
        return ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
    }

    static void fadeGains (const Voice& v, float& gIn, float& gOut) noexcept
    {
        const double t = static_cast<double> (v.fadePos) / v.fadeLen;
        if (v.equalPower)
        {
            gIn = static_cast<float> (std::sin (0.5 * kPiD * t));
            gOut = static_cast<float> (std::cos (0.5 * kPiD * t));
            return;
        }
        // Equal gain for a perfect match, toward equal power as the match drops (0.2.0's rule).
        const double in = 0.5 - 0.5 * std::cos (kPiD * t);
        const double out = 1.0 - in;
        const double norm = 1.0 / std::sqrt (in * in + out * out + 2.0 * v.rho * in * out);
        gIn = static_cast<float> (in * norm);
        gOut = static_cast<float> (out * norm);
    }

    /// Starts a grain anchored at (anchorIn, anchorOut) and crossfades every other grain out.
    void startGrain (Voice& v, double anchorIn, i64 anchorOut, int fadeLen, bool equalPower,
                     float rho, bool attack) noexcept
    {
        if (v.fading && v.sCrossfade)
        {
            // An S crossfade cut short: the incoming bank takes over now.
            v.sCur = 1 - v.sCur;
            v.sCrossfade = false;
        }
        if (v.fading)
        {
            // Fold the running fade into the grains' base gains.
            float gIn, gOut;
            fadeGains (v, gIn, gOut);
            for (int i = 0; i < kMaxGrains; ++i)
            {
                auto& gr = v.grains[sz (i)];
                if (gr.on)
                    gr.base = i == v.newest ? gIn : gr.base * gOut;
            }
        }
        int slot = -1;
        float weakest = 2.0f;
        for (int i = 0; i < kMaxGrains; ++i)
        {
            const auto& gr = v.grains[sz (i)];
            if (!gr.on)
            {
                slot = i;
                break;
            }
            if (i != v.newest && gr.base < weakest)
            {
                weakest = gr.base;
                slot = i;
            }
        }
        v.grains[sz (slot)] = { true, anchorIn, anchorOut, 0.0f };
        v.newest = slot;
        v.fading = true;
        v.fadeLen = std::max (1, fadeLen);
        v.fadePos = 0;
        v.equalPower = equalPower;
        v.rho = rho;
        if (attack)
        {
            // At an attack the whole output crossfades to the new grain, S included: there the
            // output is exactly the resampled input (§4.3), and the attack plays once.
            v.s[sz (1 - v.sCur)].restart (slot);
            v.sCrossfade = true;
        }
        else
            switchS (v, v.s[sz (v.sCur)], slot, v.fadeLen);
    }

    void onOnset (Voice& v, i64 a) noexcept
    {
        const i64 L = std::max<i64> (v.L, (n - a) + onsetFade);
        // Every grain must still be short of the attack when the fade ends.
        for (const auto& gr : v.grains)
            if (gr.on && gr.pos (n + onsetFade, v.r) >= static_cast<double> (a - kTapsHalfDown))
                return;
        startGrain (v, static_cast<double> (a), a + L, onsetFade, true, 0.0f, true);
        v.protect = a + protectLen;
        v.holdUntil = a + L + attackHold;
        ++onsetCount;
    }

    void maybeSplice (Voice& v) noexcept
    {
        if (v.fading)
            return;
        const double p = v.grains[sz (v.newest)].pos (n, v.r);
        const double d = static_cast<double> (n) - p;
        const bool down = v.r < 1.0;
        // Upward, the grain fading out keeps closing in on the input during the fade. Downward,
        // a grain that has just played an attack keeps going while the attack decays, up to a
        // hard limit, so no splice lands in its first kAttackHoldMs.
        // Where the model does not explain the input (a bend, vibrato, a new note), the grains
        // carry it alone: keep their delay short, so a bend does not come out late.
        const bool loose = residualEnergy > kLooseShare * inputEnergy;
        const double span = loose ? 0.5 * search : search;
        const double dMax = v.L + span + (loose ? looseMargin : growMargin);
        if (down ? (d <= dMax || (n < v.holdUntil && d <= dMax + attackHold))
                 : d >= v.dMin + (v.r - 1.0) * v.spliceFade)
            return;
        double dLo = v.L, dHi = v.L + span;
        if (!down && v.protect > std::numeric_limits<i64>::min())
        {
            // An upward splice jumps back: never into the attack just played.
            dHi = std::min (dHi, static_cast<double> (n - v.protect));
            if (dHi < dLo)
                dLo = std::max (d + 1.0, dHi - search);
            if (dHi <= d + 1.0)
                return;
        }
        // Candidate jumps J (new delay = d + J), whole samples so both segments share p's fraction.
        const auto jLo = static_cast<i64> (std::ceil (dLo - d));
        const auto jHi = static_cast<i64> (std::floor (dHi - d));
        if (jHi < jLo)
            return;
        const auto ip = static_cast<i64> (std::floor (p));
        const i64 segStart = ip - (corrLen - lookAhead);
        const MirrorRing& src = eh; // the residual: what the splice crossfades
        const float* ref = src.at (segStart);
        const double refEnergy = static_cast<double> (dot (ref, ref, corrLen));
        // Candidate j compares against the stretch starting at segStart - j; spliceEnergy[k] is
        // the energy of the k samples starting at segStart - jHi.
        const float* low = src.at (segStart - jHi);
        const auto count = jHi - jLo;
        spliceEnergy[0] = 0.0;
        for (i64 k = 0; k < count + corrLen; ++k)
            spliceEnergy[sz (k + 1)] = spliceEnergy[sz (k)] + static_cast<double> (low[k]) * low[k];
        constexpr double kUnscored = -3.0; // scores are correlations, in [-1, 1]
        std::fill_n (spliceScore.begin(), count + 1, kUnscored);
        const auto score = [&] (i64 j) {
            double& cached = spliceScore[sz (j - jLo)];
            if (cached == kUnscored)
            {
                const auto k = sz (jHi - j);
                const double e = spliceEnergy[k + sz (corrLen)] - spliceEnergy[k];
                cached = static_cast<double> (dot (ref, src.at (segStart - j), corrLen)) /
                         std::sqrt (refEnergy * std::max (e, 0.0) + 1.0e-30);
            }
            return cached;
        };
        // A coarse pass on every second offset (from the end the preference below scans from),
        // then the neighbours of the best.
        i64 bestJ = down ? jLo : jHi;
        double bestRho = -2.0;
        for (i64 c = 0; c <= count; c += 2)
            if (const i64 j = down ? jLo + c : jHi - c; score (j) > bestRho)
            {
                bestRho = score (j);
                bestJ = j;
            }
        for (i64 j = std::max (jLo, bestJ - 1); j <= std::min (jHi, bestJ + 1); ++j)
            if (const double sc = score (j); sc > bestRho)
            {
                bestRho = sc;
                bestJ = j;
            }
        // Of the offsets nearly as good, the one that keeps the grain longest in range: the
        // shortest delay downward, the longest upward.
        const double good = bestRho - kPreferMargin * std::abs (bestRho);
        if (down)
        {
            for (i64 j = jLo; j < bestJ; j += 2)
                if (const double sc = score (j); sc >= good)
                {
                    bestJ = j;
                    bestRho = sc;
                    break;
                }
        }
        else
            for (i64 j = jHi; j > bestJ; j -= 2)
                if (const double sc = score (j); sc >= good)
                {
                    bestJ = j;
                    bestRho = sc;
                    break;
                }
        startGrain (v, p - static_cast<double> (bestJ), n, v.spliceFade, false,
                    static_cast<float> (std::clamp (bestRho, 0.0, 1.0)), false);
        ++spliceCount;
    }

    // S: the shifted model (§4.5) -----------------------------------------------------------------

    struct ModelPoint
    {
        std::uint32_t gen = 0;
        double phase = 0.0, omega = 0.0;
        float amp = 0.0f, slope = 0.0f;
    };

    /// Slot k of the model at input position p, if it was live there.
    bool modelAt (double p, size_t k, ModelPoint& m) const noexcept
    {
        if (p < 0.0)
            return false;
        const auto hop = static_cast<i64> (std::floor (p / H));
        const auto& rec = history[sz (hop & (kHistoryHops - 1))];
        if (rec.start != hop * H || !rec.on[k])
            return false;
        const double u = p - static_cast<double> (rec.start);
        m.gen = rec.gen[k];
        m.omega = rec.omega[k];
        m.phase = rec.phase[k] + m.omega * u;
        m.slope = (rec.a1[k] - rec.a0[k]) / static_cast<float> (H);
        m.amp = rec.a0[k] + m.slope * static_cast<float> (u);
        return true;
    }

    int highAt (double p) const noexcept
    {
        if (p < 0.0)
            return 0;
        const auto hop = static_cast<i64> (std::floor (p / H));
        const auto& rec = history[sz (hop & (kHistoryHops - 1))];
        return rec.start == hop * H ? rec.high : 0;
    }

    float confidenceAt (double p, float& slope) const noexcept
    {
        slope = 0.0f;
        if (p < 0.0)
            return 1.0f;
        const auto hop = static_cast<i64> (std::floor (p / H));
        const auto& rec = history[sz (hop & (kHistoryHops - 1))];
        if (rec.start != hop * H)
            return 1.0f;
        slope = (rec.g1 - rec.g0) / static_cast<float> (H);
        return rec.g0 + slope * static_cast<float> (p - static_cast<double> (rec.start));
    }

    /// A sustain splice: the bank moves to grain `slot`. Every partial continues its phase (offset
    /// theta), and the amplitude difference between the two read positions ramps out over `len`.
    void switchS (const Voice& v, ShiftedModel& b, int slot, int len) noexcept
    {
        b.grain = slot;
        if (b.fresh)
            return;
        const double pNew = v.grains[sz (slot)].pos (n, v.r);
        const int high = std::max (b.high, highAt (pNew));
        for (int k = 0; k < high; ++k)
        {
            const auto kk = sz (k);
            ModelPoint m;
            const bool live = modelAt (pNew, kk, m);
            if (b.live[kk] && live && b.gen[kk] == m.gen)
            {
                const double now =
                    std::atan2 (static_cast<double> (b.zi[kk]), static_cast<double> (b.zr[kk]));
                b.theta[kk] = wrapPi (now - m.phase);
                b.bias[kk] = b.a[kk] - m.amp;
            }
            else if (live)
            {
                // New to S (or a different partial in the slot): from silence, in phase with
                // what the grains subtract.
                b.live[kk] = 1;
                b.gen[kk] = m.gen;
                b.theta[kk] = 0.0;
                b.bias[kk] = -m.amp;
            }
            else
                b.bias[kk] = b.live[kk] ? b.a[kk] : 0.0f; // gone: ramps out at its last pitch
            b.biasStep[kk] = b.bias[kk] / static_cast<float> (len);
        }
        float slope = 0.0f;
        b.gBias = b.g - confidenceAt (pNew, slope);
        b.gBiasStep = b.gBias / static_cast<float> (len);
        b.biasLeft = len;
        b.segLeft = 0;
        b.high = high;
    }

    /// Sets S's oscillators for the stretch where the newest grain reads inside one hop.
    void setupSegment (const Voice& v, ShiftedModel& b) noexcept
    {
        const double p = v.grains[sz (b.grain)].pos (n, v.r);
        const double r = v.r;
        const bool biasing = b.biasLeft > 0;
        const int high = std::max (b.high, highAt (p));
        int newHigh = 0;
        for (int k = 0; k < high; ++k)
        {
            const auto kk = sz (k);
            ModelPoint m;
            const bool live = modelAt (p, kk, m);
            const float bias = biasing ? b.bias[kk] : 0.0f;
            const float bStep = biasing ? b.biasStep[kk] : 0.0f;
            if (live)
            {
                if (!b.live[kk] || b.gen[kk] != m.gen)
                {
                    b.live[kk] = 1;
                    b.gen[kk] = m.gen;
                    b.theta[kk] = 0.0;
                }
                const double ph = m.phase + b.theta[kk];
                b.zr[kk] = static_cast<float> (std::cos (ph));
                b.zi[kk] = static_cast<float> (std::sin (ph));
                b.wr[kk] = static_cast<float> (std::cos (r * m.omega));
                b.wi[kk] = static_cast<float> (std::sin (r * m.omega));
                b.a[kk] = m.amp + bias;
                b.da[kk] = static_cast<float> (r) * m.slope - bStep;
                newHigh = k + 1;
            }
            else if (b.live[kk] && bias != 0.0f)
            {
                b.a[kk] = bias; // ramping out with its last pitch
                b.da[kk] = -bStep;
                newHigh = k + 1;
            }
            else
            {
                b.live[kk] = 0;
                b.a[kk] = b.da[kk] = 0.0f;
            }
        }
        b.high = newHigh;
        float gSlope = 0.0f;
        const float g = confidenceAt (p, gSlope);
        b.g = g + (biasing ? b.gBias : 0.0f);
        b.dg = static_cast<float> (r) * gSlope - (biasing ? b.gBiasStep : 0.0f);
        // Until the read position crosses into the next hop, or the bias ramp ends.
        const double toHop = (std::floor (p / H) + 1.0) * H - p;
        int len = std::max (1, static_cast<int> (std::ceil (toHop / r)));
        if (biasing)
            len = std::min (len, b.biasLeft);
        b.segLeft = len;
        b.fresh = false;
    }

    static float synthS (ShiftedModel& b) noexcept
    {
        const int K = b.high;
        const float sum = runOscillators (b.zr.data(), b.zi.data(), b.wr.data(), b.wi.data(),
                                          b.a.data(), b.da.data(), K);
        const float out = sum * b.g;
        b.g += b.dg;
        if (b.biasLeft > 0)
        {
            if (--b.biasLeft == 0)
            {
                std::fill (b.bias.begin(), b.bias.end(), 0.0f);
                b.gBias = 0.0f;
            }
            else
            {
                float* bias = b.bias.data();
                const float* step = b.biasStep.data();
                for (int k = 0; k < K; ++k)
                    bias[k] -= step[k];
                b.gBias -= b.gBiasStep;
            }
        }
        --b.segLeft;
        return out;
    }

    /// One output sample of a voice: the grains on the residual plus S.
    float voiceSample (Voice& v, i64 onset) noexcept
    {
        if (onset >= 0)
            onOnset (v, onset);
        maybeSplice (v);
        auto& sCur = v.s[sz (v.sCur)];
        auto& sNext = v.s[sz (1 - v.sCur)];
        if (sCur.segLeft <= 0)
            setupSegment (v, sCur);
        if (v.sCrossfade && sNext.segLeft <= 0)
            setupSegment (v, sNext);

        float td = 0.0f;
        float sOut;
        if (v.fading)
        {
            float gIn, gOut;
            fadeGains (v, gIn, gOut);
            sOut = v.sCrossfade ? gOut * synthS (sCur) + gIn * synthS (sNext) : synthS (sCur);
            for (int i = 0; i < kMaxGrains; ++i)
            {
                const auto& gr = v.grains[sz (i)];
                if (!gr.on)
                    continue;
                const float g = i == v.newest ? gIn : gr.base * gOut;
                if (g != 0.0f)
                    td += g * readAt (gr.pos (n, v.r), *v.poly);
            }
            if (++v.fadePos >= v.fadeLen)
            {
                v.fading = false;
                if (v.sCrossfade)
                {
                    v.sCur = 1 - v.sCur;
                    v.sCrossfade = false;
                }
                for (int i = 0; i < kMaxGrains; ++i)
                    v.grains[sz (i)].on = i == v.newest;
                v.grains[sz (v.newest)].base = 1.0f;
            }
        }
        else
        {
            td = readAt (v.grains[sz (v.newest)].pos (n, v.r), *v.poly);
            sOut = synthS (sCur);
        }
        return td + sOut;
    }

    // The stream
    // ------------------------------------------------------------------------------------

    void process (float* io, int numFrames) noexcept
    {
        // A setting change starts at once, or where a running change fade ends: at the same
        // sample whatever the block size.
        int i = 0;
        while (i < numFrames)
        {
            applyTarget();
            if (dryCur && changeLeft == 0)
            {
                // Off: bit-exact. The input history stays current so a shift can start at once.
                for (; i < numFrames; ++i)
                {
                    xh.write (n, io[i]);
                    eh.write (n, io[i]);
                    ++n;
                }
                return;
            }
            i = run (io, i, numFrames);
        }
    }

    /// Shifts io[i, end) until a change fade ends; returns where it stopped.
    int run (float* io, int i, int end) noexcept
    {
        for (; i < end; ++i)
        {
            const float x = io[i];
            xh.write (n, x);
            float u = 0.0f;
            if (uHigh > 0)
            {
                const float raw = synthU();
                u = gNow * raw;
                gNow += gStep;
                sxu += static_cast<double> (x) * raw;
                suu += static_cast<double> (raw) * raw;
            }
            const float e = x - u;
            eh.write (n, e);
            inputEnergy += levelCoef * (static_cast<double> (x) * x - inputEnergy);
            residualEnergy += levelCoef * (static_cast<double> (e) * e - residualEnergy);
            const i64 onset = detectOnset (e, x);

            const float now = dryCur ? x : voiceSample (voices[sz (cur)], onset);
            bool fadeEnded = false;
            if (changeLeft > 0)
            {
                const float before = dryPrev ? x : voiceSample (voices[sz (prev)], onset);
                const double t = 1.0 - static_cast<double> (changeLeft) / changeFade;
                io[i] = static_cast<float> (std::sin (0.5 * kPiD * t)) * now +
                        static_cast<float> (std::cos (0.5 * kPiD * t)) * before;
                if (--changeLeft == 0)
                {
                    if (prev >= 0)
                        voices[sz (prev)].on = false;
                    prev = -1;
                    dryPrev = false;
                    fadeEnded = true;
                }
            }
            else
                io[i] = now;
            ++n;
            if (modelOn && n % H == 0 && n - modelStart >= 0)
                analyse (n);
            if (fadeEnded)
            {
                if (dryCur)
                    resetModel(); // faded back to 0: off from the next sample
                return i + 1;
            }
        }
        return end;
    }
};

// -------------------------------------------------------------------------------------------------

PitchShifter::PitchShifter() = default;
PitchShifter::~PitchShifter() = default;

int PitchShifter::latencyFor (int semitones, double sampleRate) noexcept
{
    if (semitones == 0)
        return 0;
    const double ms = semitones < 0 ? kLatencyDownMs : kLatencyUpMs;
    return static_cast<int> (std::lround (ms * 0.001 * sampleRate));
}

void PitchShifter::prepare (const ProcessSpec& spec)
{
    if (impl_ == nullptr)
        impl_ = std::make_unique<Impl>();
    impl_->prepare (spec.sampleRate);
    impl_->target = std::clamp (settings_.semitones, -12, 12);
    impl_->reset();
}

void PitchShifter::reset() noexcept
{
    if (impl_ != nullptr)
        impl_->reset();
}

void PitchShifter::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    if (impl_ != nullptr)
        impl_->target = std::clamp (s.semitones, -12, 12);
}

void PitchShifter::process (const AudioView& io) noexcept
{
    if (io.numChannels() > 0)
        process (io.channel (0), io.numFrames());
}

void PitchShifter::process (float* x, int numFrames) noexcept
{
    if (impl_ != nullptr && numFrames > 0)
        impl_->process (x, numFrames);
}

int PitchShifter::latencySamples() const noexcept
{
    return impl_ == nullptr ? 0 : latencyFor (impl_->target, impl_->fs);
}

bool PitchShifter::isActive() const noexcept
{
    return impl_ != nullptr && (!impl_->dryCur || impl_->changeLeft > 0);
}

PitchShifter::Stats PitchShifter::stats() const noexcept
{
    Stats s;
    if (impl_ == nullptr)
        return s;
    s.onsets = impl_->onsetCount;
    s.splices = impl_->spliceCount;
    for (int k = 0; k < impl_->uHigh; ++k)
        s.tracks += impl_->trOn[sz (k)] ? 1 : 0;
    return s;
}

} // namespace sedef::dsp
