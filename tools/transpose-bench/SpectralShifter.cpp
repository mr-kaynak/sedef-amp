// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SpectralShifter.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <pffft/pffft.h>
#include <vector>

namespace sedef::bench
{
namespace
{
using Complex = std::complex<float>;

/// Wraps a phase to (-pi, pi].
inline float princarg (float phase) noexcept
{
    return phase - kTwoPi * std::round (phase * (1.0f / kTwoPi));
}

struct AlignedFloats
{
    float* data = nullptr;
    AlignedFloats() = default;
    explicit AlignedFloats (int n)
    {
        data =
            static_cast<float*> (pffft_aligned_malloc (sizeof (float) * static_cast<size_t> (n)));
        std::fill (data, data + n, 0.0f);
    }
    ~AlignedFloats() { pffft_aligned_free (data); }
    AlignedFloats (const AlignedFloats&) = delete;
    AlignedFloats& operator= (const AlignedFloats&) = delete;
    AlignedFloats (AlignedFloats&& o) noexcept : data (o.data) { o.data = nullptr; }
    AlignedFloats& operator= (AlignedFloats&& o) noexcept
    {
        std::swap (data, o.data);
        return *this;
    }
};
} // namespace

struct SpectralShifter::Impl
{
    Config cfg;
    double fs = 48000.0;
    int N = 4096; // FFT length = analysis window
    int M = 384;  // synthesis window
    int H = 96;   // hop
    int n0 = 0;   // frame index of the phase origin (centre of the synthesis window)
    int latency = 0;
    int bins = 0; // N / 2

    PFFFT_Setup* setup = nullptr;
    AlignedFloats frame, frameT, spec, specT, work, out;

    std::vector<float> wa, wt; // analysis window, and (n - n0) * wa for the energy centre
    std::vector<float> ws;     // synthesis window over the last M samples of the frame
    float scale = 1.0f;
    float onsetCentre = 0.0f; // energy centre (samples after the origin) that marks an attack
    float onsetRise = 1.5f;   // and the energy growth since the last frame

    std::vector<float> input; // ring
    std::uint32_t inMask = 0;
    std::vector<float> ola; // output accumulator ring
    std::uint32_t olaMask = 0;
    std::uint32_t pos = 0; // samples written
    int hopCount = 0;

    // Per-bin state.
    std::vector<Complex> prevX;
    std::vector<float> mag2, prevMag2, rotation; // rotation of the region that held the bin
    AlignedFloats Ybuf;                          // shifted spectrum, pffft order
    Complex* Y = nullptr;
    // Per-frame peak lists.
    std::vector<int> peaks, lo, hi, shift;
    std::vector<float> rot;

    double logRatio = 0.0, logTarget = 0.0, logStep = 0.0;
    int glideFrames = 1, glideLeft = 0;

    ~Impl()
    {
        if (setup != nullptr)
            pffft_destroy_setup (setup);
    }

    void prepare (double sampleRate, const Config& c)
    {
        cfg = c;
        fs = sampleRate;
        // The power of two nearest to the requested length (in the log domain).
        N = 32;
        while (N * 1.4142135623730951 < c.analysisMs * 0.001 * fs)
            N <<= 1;
        bins = N / 2;
        if (c.asymmetric)
        {
            const int overlap = std::max (2, c.overlap);
            M = std::max (4 * overlap, static_cast<int> (std::lround (c.synthesisMs * 0.001 * fs)));
            M = std::min (M - M % (2 * overlap), N / 2);
            H = M / overlap;
            n0 = N - M / 2;
            latency = M - 1;
        }
        else
        {
            M = N;
            H = N / std::max (2, c.overlap);
            n0 = N / 2;
            latency = N - 1;
        }

        if (setup != nullptr)
            pffft_destroy_setup (setup);
        setup = pffft_new_setup (N, PFFFT_REAL);
        frame = AlignedFloats (N);
        frameT = AlignedFloats (N);
        spec = AlignedFloats (N);
        specT = AlignedFloats (N);
        work = AlignedFloats (N);
        out = AlignedFloats (N);

        wa.assign (static_cast<size_t> (N), 0.0f);
        wt.assign (static_cast<size_t> (N), 0.0f);
        ws.assign (static_cast<size_t> (M), 0.0f);
        const auto hann = [] (double m, double len) {
            return 0.5 - 0.5 * std::cos (kTwoPiD * (m + 0.5) / len);
        };
        if (c.asymmetric)
        {
            // Rising half of a long Hann, then the falling half of a Hann of length M. The product
            // with the synthesis window is a Hann of length M over the frame's last M samples.
            const int rise = N - M / 2;
            for (int n = 0; n < N; ++n)
                wa[static_cast<size_t> (n)] =
                    static_cast<float> (n < rise ? 0.5 - 0.5 * std::cos (kPiD * (n + 0.5) / rise)
                                                 : hann (n - (N - M), M));
            for (int m = 0; m < M; ++m)
                ws[static_cast<size_t> (m)] = static_cast<float> (
                    hann (m, M) /
                    std::max (1.0e-6, static_cast<double> (wa[static_cast<size_t> (N - M + m)])));
            scale = static_cast<float> (2.0 * H / M / N);
        }
        else
        {
            // Hann analysis and synthesis: the product (Hann squared) overlap-adds to 3N / (8H).
            for (int n = 0; n < N; ++n)
            {
                wa[static_cast<size_t> (n)] = static_cast<float> (hann (n, N));
                ws[static_cast<size_t> (n)] = wa[static_cast<size_t> (n)];
            }
            scale = static_cast<float> (8.0 * H / (3.0 * N) / N);
        }
        for (int n = 0; n < N; ++n)
            wt[static_cast<size_t> (n)] = static_cast<float> (n - n0) * wa[static_cast<size_t> (n)];
        onsetCentre = static_cast<float> (c.onsetCentre * (c.asymmetric ? M / 2 : N / 2));
        onsetRise = static_cast<float> (c.onsetRise);

        std::uint32_t size = 1;
        while (size < static_cast<std::uint32_t> (N + 8))
            size <<= 1;
        input.assign (size, 0.0f);
        inMask = size - 1;
        size = 1;
        while (size < static_cast<std::uint32_t> (M + latency + H + 8))
            size <<= 1;
        ola.assign (size, 0.0f);
        olaMask = size - 1;

        const auto b = static_cast<size_t> (bins + 1);
        prevX.assign (b, Complex {});
        mag2.assign (b, 0.0f);
        prevMag2.assign (b, 0.0f);
        rotation.assign (b, 0.0f);
        Ybuf = AlignedFloats (N);
        Y = reinterpret_cast<Complex*> (Ybuf.data);
        peaks.reserve (b);
        lo.reserve (b);
        hi.reserve (b);
        shift.reserve (b);
        rot.reserve (b);
        glideFrames = std::max (1, static_cast<int> (std::lround (c.glideMs * 0.001 * fs / H)));
        reset();
    }

    void reset() noexcept
    {
        std::fill (input.begin(), input.end(), 0.0f);
        std::fill (ola.begin(), ola.end(), 0.0f);
        std::fill (prevX.begin(), prevX.end(), Complex {});
        std::fill (prevMag2.begin(), prevMag2.end(), 0.0f);
        std::fill (rotation.begin(), rotation.end(), 0.0f);
        pos = 0;
        hopCount = 0;
        logRatio = logTarget;
        glideLeft = 0;
    }

    void setRatio (double ratio, bool glide) noexcept
    {
        const double target = std::log (std::clamp (ratio, 0.25, 4.0));
        if (target == logTarget && (glide || glideLeft == 0))
            return;
        logTarget = target;
        if (!glide)
        {
            logRatio = target;
            glideLeft = 0;
            return;
        }
        glideLeft = glideFrames;
        logStep = (logTarget - logRatio) / glideLeft;
    }

    void computeFrame() noexcept
    {
        if (glideLeft > 0)
        {
            logRatio = --glideLeft == 0 ? logTarget : logRatio + logStep;
        }
        const auto alpha = static_cast<float> (std::exp (logRatio));

        // Window the newest N samples and rotate so that index 0 is the phase origin n0.
        const std::uint32_t first = pos - static_cast<std::uint32_t> (N);
        for (int n = 0; n < N; ++n)
        {
            const float x = input[(first + static_cast<std::uint32_t> (n)) & inMask];
            const int b = (n - n0 + N) & (N - 1);
            frame.data[b] = x * wa[static_cast<size_t> (n)];
            frameT.data[b] = x * wt[static_cast<size_t> (n)];
        }
        pffft_transform_ordered (setup, frame.data, spec.data, work.data, PFFFT_FORWARD);
        if (cfg.transientReset)
            pffft_transform_ordered (setup, frameT.data, specT.data, work.data, PFFFT_FORWARD);

        const auto* X =
            reinterpret_cast<const Complex*> (spec.data); // X[k], k >= 1 (k = 0 packs DC, Nyquist)
        const auto* XT = reinterpret_cast<const Complex*> (specT.data);
        float maxMag2 = 0.0f;
        mag2[0] = 0.0f;
        for (int k = 1; k < bins; ++k)
        {
            mag2[static_cast<size_t> (k)] = std::norm (X[k]);
            maxMag2 = std::max (maxMag2, mag2[static_cast<size_t> (k)]);
        }
        mag2[static_cast<size_t> (bins)] = 0.0f;

        // Peaks: larger than two neighbours on each side, above the floor.
        peaks.clear();
        const float floor =
            std::max (maxMag2 * static_cast<float> (std::pow (10.0, cfg.peakFloorDb / 10.0)),
                      1.0e-24f * static_cast<float> (N) * static_cast<float> (N));
        const bool wide = cfg.peakNeighbours >= 2;
        for (int k = 2; k < bins - 2; ++k)
        {
            const float m = mag2[static_cast<size_t> (k)];
            if (m > floor && m > mag2[static_cast<size_t> (k - 1)] &&
                m >= mag2[static_cast<size_t> (k + 1)] &&
                (!wide ||
                 (m > mag2[static_cast<size_t> (k - 2)] && m >= mag2[static_cast<size_t> (k + 2)])))
                peaks.push_back (k);
        }

        std::fill (Ybuf.data, Ybuf.data + N, 0.0f);
        const int count = static_cast<int> (peaks.size());
        lo.resize (static_cast<size_t> (count));
        hi.resize (static_cast<size_t> (count));
        shift.resize (static_cast<size_t> (count));
        rot.resize (static_cast<size_t> (count));

        const float twoPiOverN = kTwoPi / static_cast<float> (N);
        const float hop = static_cast<float> (H);
        for (int p = 0; p < count; ++p)
        {
            const int k = peaks[static_cast<size_t> (p)];
            // Region of influence: from the lowest bin after the previous peak to the lowest
            // bin before the next one.
            int a = 1, b = bins - 1;
            if (p > 0)
            {
                a = peaks[static_cast<size_t> (p - 1)] + 1;
                for (int j = a; j < k; ++j)
                    if (mag2[static_cast<size_t> (j)] < mag2[static_cast<size_t> (a)])
                        a = j;
            }
            if (p + 1 < count)
            {
                const int next = peaks[static_cast<size_t> (p + 1)];
                b = k + 1;
                for (int j = k + 1; j < next; ++j)
                    if (mag2[static_cast<size_t> (j)] < mag2[static_cast<size_t> (b)])
                        b = j;
                b = b - 1; // the valley bin goes to the next region
                b = std::max (b, k);
            }
            lo[static_cast<size_t> (p)] = a;
            hi[static_cast<size_t> (p)] = b;

            // Instantaneous frequency from the phase advance since the last frame.
            const Complex d = X[k] * std::conj (prevX[static_cast<size_t> (k)]);
            float dev = 0.0f;
            if (prevMag2[static_cast<size_t> (k)] > 0.0f)
                dev = princarg (std::arg (d) - twoPiOverN * static_cast<float> (k) * hop);
            const float kHat =
                static_cast<float> (k) + std::clamp (dev / (twoPiOverN * hop), -1.0f, 1.0f);
            const float omega = twoPiOverN * kHat;
            shift[static_cast<size_t> (p)] = static_cast<int> (std::lround ((alpha - 1.0f) * kHat));

            float r = rotation[static_cast<size_t> (k)] + (alpha - 1.0f) * omega * hop;
            if (cfg.transientReset)
            {
                // Energy centre of the region relative to the phase origin. A region whose energy
                // sits well ahead of the origin and has grown is a new attack: rotate it so the
                // attack keeps its own phase structure (stays coherent at its own time) instead of
                // inheriting the rotation of whatever the bins held before. Accumulating from
                // there keeps it coherent in later frames.
                float e = 0.0f, et = 0.0f, ePrev = 0.0f;
                for (int j = a; j <= b; ++j)
                {
                    e += mag2[static_cast<size_t> (j)];
                    et += (XT[j] * std::conj (X[j])).real();
                    ePrev += prevMag2[static_cast<size_t> (j)];
                }
                const float centre = e > 0.0f ? et / e : 0.0f;
                if (centre > onsetCentre && e > onsetRise * ePrev)
                    r = -twoPiOverN * static_cast<float> (shift[static_cast<size_t> (p)]) * centre;
            }
            rot[static_cast<size_t> (p)] = princarg (r);
        }

        // Write the shifted regions. Downward shifts pack regions closer: optionally split the
        // overlap between neighbours.
        for (int p = 0; p < count; ++p)
        {
            int a = lo[static_cast<size_t> (p)] + shift[static_cast<size_t> (p)];
            int b = hi[static_cast<size_t> (p)] + shift[static_cast<size_t> (p)];
            if (cfg.clipRegions)
            {
                if (p > 0)
                {
                    const int prevTop =
                        hi[static_cast<size_t> (p - 1)] + shift[static_cast<size_t> (p - 1)];
                    if (a <= prevTop)
                        a = (a + prevTop) / 2 + 1;
                }
                if (p + 1 < count)
                {
                    const int nextBottom =
                        lo[static_cast<size_t> (p + 1)] + shift[static_cast<size_t> (p + 1)];
                    if (b >= nextBottom)
                        b = (b + nextBottom) / 2;
                }
            }
            a = std::max (a, 1);
            b = std::min (b, bins - 1);
            const Complex phasor = std::polar (1.0f, rot[static_cast<size_t> (p)]);
            const int s = shift[static_cast<size_t> (p)];
            for (int j = a; j <= b; ++j)
                Y[static_cast<size_t> (j)] += X[j - s] * phasor;
            for (int j = lo[static_cast<size_t> (p)]; j <= hi[static_cast<size_t> (p)]; ++j)
                rotation[static_cast<size_t> (j)] = rot[static_cast<size_t> (p)];
        }

        for (int k = 1; k < bins; ++k)
            prevX[static_cast<size_t> (k)] = X[k];
        std::copy (mag2.begin(), mag2.end(), prevMag2.begin());

        // Inverse FFT and overlap-add the last M samples of the frame.
        auto* Yf = Ybuf.data;
        Yf[0] = 0.0f; // DC
        Yf[1] = 0.0f; // Nyquist
        pffft_transform_ordered (setup, Yf, out.data, work.data, PFFFT_BACKWARD);
        const std::uint32_t outFirst = pos - static_cast<std::uint32_t> (M);
        for (int m = 0; m < M; ++m)
        {
            const int n = N - M + m;
            const int b = (n - n0 + N) & (N - 1);
            ola[(outFirst + static_cast<std::uint32_t> (m)) & olaMask] +=
                out.data[b] * ws[static_cast<size_t> (m)] * scale;
        }
    }

    void push (const float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            input[pos++ & inMask] = x[i];
        hopCount = 0;
    }

    void process (float* io, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            input[pos & inMask] = io[i];
            ++pos;
            if (++hopCount >= H)
            {
                hopCount = 0;
                computeFrame();
            }
            auto& slot = ola[(pos - 1u - static_cast<std::uint32_t> (latency)) & olaMask];
            io[i] = slot;
            slot = 0.0f;
        }
    }
};

SpectralShifter::SpectralShifter() : impl_ (std::make_unique<Impl>()) {}
SpectralShifter::~SpectralShifter() = default;
SpectralShifter::SpectralShifter (SpectralShifter&&) noexcept = default;
SpectralShifter& SpectralShifter::operator= (SpectralShifter&&) noexcept = default;

void SpectralShifter::prepare (double sampleRate, const Config& config)
{
    impl_->prepare (sampleRate, config);
}

void SpectralShifter::reset() noexcept
{
    impl_->reset();
}

void SpectralShifter::setRatio (double ratio, bool glide) noexcept
{
    impl_->setRatio (ratio, glide);
}

int SpectralShifter::latencySamples() const noexcept
{
    return impl_->latency;
}

void SpectralShifter::push (const float* in, int n) noexcept
{
    impl_->push (in, n);
}

void SpectralShifter::process (float* io, int n) noexcept
{
    impl_->process (io, n);
}

} // namespace sedef::bench
