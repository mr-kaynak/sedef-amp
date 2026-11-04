// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace sedef::dsp::capture
{

/// Windowed-sinc prototype g(u) = sinc(u) * kaiser(u / Z) for |u| < Z, u in zero crossings,
/// tabulated with `phases` points per zero crossing and read with linear interpolation.
class SincKernel
{
public:
    void design (int zeroCrossings, double beta, int phases = 1024);
    int zeroCrossings() const noexcept { return z_; }

    /// g(|u|) with u already scaled to table units (u * phases). Zero at and beyond the edge.
    float atTableIndex (double x) const noexcept
    {
        if (x < 0.0)
            x = -x;
        const auto i = static_cast<std::int64_t> (x);
        if (i >= last_)
            return 0.0f;
        const float frac = static_cast<float> (x - static_cast<double> (i));
        const float a = table_[static_cast<std::size_t> (i)];
        const float b = table_[static_cast<std::size_t> (i) + 1];
        return a + frac * (b - a);
    }

    int phases() const noexcept { return phases_; }

private:
    std::vector<float> table_;
    std::int64_t last_ = 0;
    int z_ = 0;
    int phases_ = 1;
};

/// The minimum-phase version of SincKernel's standard prototype (16 zero crossings, Kaiser
/// beta 8, 1024 phases): the same magnitude response, all of it causal, from the real cepstrum
/// (Oppenheim and Schafer, "Discrete-Time Signal Processing", homomorphic deconvolution). Built
/// once per process, on first use (about 0.2 s), and shared.
class MinimumPhaseKernel
{
public:
    /// Zero crossings of the linear-phase prototype it is made from; its span is twice that.
    static constexpr int kZeroCrossings = 16;
    static constexpr int kPhases = 1024;
    /// Its group delay at DC, in zero crossings (the centroid of the table; a test checks it).
    static constexpr double kDelayZc = 2.3609;

    static const MinimumPhaseKernel& standard();

    /// g(x / phases) for x >= 0 in table units; zero beyond the span.
    float atTableIndex (double x) const noexcept
    {
        const auto i = static_cast<std::int64_t> (x);
        if (i >= last_)
            return 0.0f;
        const float frac = static_cast<float> (x - static_cast<double> (i));
        const float a = table_[static_cast<std::size_t> (i)];
        const float b = table_[static_cast<std::size_t> (i) + 1];
        return a + frac * (b - a);
    }

    int spanZc() const noexcept { return 2 * kZeroCrossings; }
    /// Measured from the table (zero crossings).
    double delayZc() const noexcept { return delayZc_; }
    const std::vector<float>& table() const noexcept { return table_; }

private:
    MinimumPhaseKernel();
    std::vector<float> table_;
    std::int64_t last_ = 0;
    double delayZc_ = 0.0;
};

/// Runs a model at its own sample rate inside a host stream: host block -> model-rate samples ->
/// (the model) -> host block, with one fixed, integer latency for the round trip.
///
/// Both conversions are band-limited interpolation (J. O. Smith, "Digital Audio Resampling") with
/// a Kaiser-windowed sinc whose cut-off sits at `passband` times the lower Nyquist frequency.
/// Positions are exact: rates are whole hertz and every sample time is an integer multiple of
/// 1 / (hostRate * modelRate), so the conversion never drifts. The latency can be any value from
/// minimumLatency() up; the host-side interpolation simply reads further back. Equal rates pass
/// the samples through and only delay them.
///
/// prepareMinimumPhase() uses MinimumPhaseKernel instead (OversamplingMode::lowLatency): the same
/// magnitude response with no look-ahead, so the round trip's delay is the filters' own group
/// delay, about 0.12 ms at low frequencies (minimumPhaseDelay()), rising towards the cut-off.
///
///   prepare*()               any non-real-time thread (allocates)
///   toModel(), fromModel()   audio thread, once each per host block, in that order
class RateBridge
{
public:
    static constexpr int kDefaultZeroCrossings = 16;
    static constexpr double kDefaultPassband = 0.9;
    static constexpr double kKaiserBeta = 8.0; // about 80 dB of stop-band rejection

    /// The smallest round-trip latency (host samples) for these rates and filter length.
    static int minimumLatency (double hostRate, double modelRate,
                               int zeroCrossings = kDefaultZeroCrossings,
                               double passband = kDefaultPassband) noexcept;

    /// The longest filter (most zero crossings, at least 4) that fits in `latency`; 0 if none.
    static int zeroCrossingsFor (double hostRate, double modelRate, int latency,
                                 double passband = kDefaultPassband) noexcept;

    void prepare (double hostRate, double modelRate, int maxHostBlock, int latency,
                  int zeroCrossings = kDefaultZeroCrossings, double passband = kDefaultPassband);
    void reset() noexcept;

    /// Minimum phase: the round trip's group delay at DC (host samples) with the default pass
    /// band; 0 at equal rates.
    static double minimumPhaseDelay (double hostRate, double modelRate) noexcept;
    /// The smallest latency a minimum-phase bridge honours: minimumPhaseDelay(), rounded.
    static int minimumPhaseLatency (double hostRate, double modelRate) noexcept;
    /// Minimum-phase bridge whose round trip lags `latency` host samples at low frequencies
    /// (to within half a sample), or minimumPhaseLatency() if that is longer.
    void prepareMinimumPhase (double hostRate, double modelRate, int maxHostBlock, int latency);

    bool passthrough() const noexcept { return fh_ == fm_; }
    bool minimumPhase() const noexcept { return minimumPhase_; }
    /// The round trip's delay at low frequencies, host samples (minimum phase: the rounded
    /// group delay plus the read delay).
    int latency() const noexcept { return latency_; }
    /// Host samples of input after which the output only depends on that input (the filters'
    /// span plus the latency).
    int settleSamples() const noexcept { return settle_; }
    /// The most model samples one toModel() call can produce.
    int maxModelBlock() const noexcept { return maxModelBlock_; }

    /// Audio thread. Takes n host samples and writes the model-rate samples now due into
    /// modelIn; returns how many (at most maxModelBlock()).
    int toModel (const float* hostIn, int n, float* modelIn) noexcept;
    /// Audio thread. Takes the m model outputs for the samples toModel() just produced and writes
    /// the block's n host samples (the same n), `latency()` samples late.
    void fromModel (const float* modelOut, int m, float* hostOut, int n) noexcept;

private:
    void allocate (std::int64_t hostSpan, std::int64_t modelSpan);

    SincKernel kernel_;
    const MinimumPhaseKernel* mpKernel_ = nullptr;
    bool minimumPhase_ = false;
    int readDelay_ = 0; // host samples the output position trails the newest input (minimum phase)
    int settle_ = 0;
    std::int64_t fh_ = 48000, fm_ = 48000; // whole hertz
    double cu_ = 1.0, cd_ = 1.0;           // cut-off, 2 fc / input rate, per direction
    std::int64_t wu_ = 0, wd_ = 0;         // kernel half-widths in time units (1 / (fh fm) s)
    int latency_ = 0;
    int maxModelBlock_ = 0;

    std::vector<float> hostHist_, modelHist_;
    std::int64_t hostMask_ = 0, modelMask_ = 0;
    std::int64_t hostCount_ = 0;  // host samples pushed
    std::int64_t modelCount_ = 0; // model outputs pushed
    std::int64_t nextModel_ = 0;  // next model sample toModel() produces
    std::int64_t nextHost_ = 0;   // next host sample fromModel() produces
};

} // namespace sedef::dsp::capture
