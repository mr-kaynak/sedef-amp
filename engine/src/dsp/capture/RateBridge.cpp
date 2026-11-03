// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/capture/RateBridge.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace sedef::dsp::capture
{
namespace
{
double besselI0 (double x) noexcept
{
    // Power series; converges quickly for the arguments a Kaiser window needs (x <= ~20).
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double> (k) * static_cast<double> (k));
        sum += term;
        if (term < 1.0e-12 * sum)
            break;
    }
    return sum;
}

std::int64_t floorDiv (std::int64_t a, std::int64_t b) noexcept
{
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::int64_t ceilDiv (std::int64_t a, std::int64_t b) noexcept
{
    return -floorDiv (-a, b);
}

std::int64_t wholeHertz (double rate) noexcept
{
    return std::max<std::int64_t> (1, std::llround (rate));
}

std::int64_t nextPow2 (std::int64_t n) noexcept
{
    std::int64_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

struct Widths
{
    double cu, cd;
    std::int64_t wu, wd;
};

Widths widthsFor (std::int64_t fh, std::int64_t fm, int z, double passband) noexcept
{
    // Cut-off at `passband` of the lower Nyquist, expressed per direction relative to the input
    // rate (2 fc / f_in): host -> model filters at the host rate, model -> host at the model rate.
    const double ratio = static_cast<double> (fm) / static_cast<double> (fh);
    Widths w {};
    w.cu = passband * std::min (1.0, ratio);
    w.cd = passband * std::min (1.0, 1.0 / ratio);
    // Half-widths: Z zero crossings of sinc(c t), i.e. Z / c input samples, in time units of
    // 1 / (fh fm) seconds (a host sample is fm units, a model sample fh units).
    w.wu = static_cast<std::int64_t> (std::ceil (z / w.cu * static_cast<double> (fm)));
    w.wd = static_cast<std::int64_t> (std::ceil (z / w.cd * static_cast<double> (fh)));
    return w;
}
} // namespace

// -------------------------------------------------------------------------------------------------

void SincKernel::design (int zeroCrossings, double beta, int phases)
{
    z_ = std::max (1, zeroCrossings);
    phases_ = std::max (1, phases);
    last_ = static_cast<std::int64_t> (z_) * phases_;
    table_.assign (static_cast<std::size_t> (last_) + 2, 0.0f);
    const double i0Beta = besselI0 (beta);
    for (std::int64_t i = 0; i < last_; ++i)
    {
        const double u = static_cast<double> (i) / phases_;
        const double sinc = i == 0 ? 1.0 : std::sin (kPiD * u) / (kPiD * u);
        const double r = u / z_;
        const double window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        table_[static_cast<std::size_t> (i)] = static_cast<float> (sinc * window);
    }
}

// -------------------------------------------------------------------------------------------------

int RateBridge::minimumLatency (double hostRate, double modelRate, int zeroCrossings,
                                double passband) noexcept
{
    const auto fh = wholeHertz (hostRate);
    const auto fm = wholeHertz (modelRate);
    if (fh == fm)
        return 0;
    const auto w = widthsFor (fh, fm, zeroCrossings, passband);
    // The last output of a block reads model samples up to (wu + wd) time units past its own
    // position, and those exist once the block's input has arrived (see fromModel()).
    return static_cast<int> (ceilDiv (w.wu + w.wd, fm));
}

int RateBridge::zeroCrossingsFor (double hostRate, double modelRate, int latency,
                                  double passband) noexcept
{
    for (int z = kDefaultZeroCrossings; z >= 4; --z)
        if (minimumLatency (hostRate, modelRate, z, passband) <= latency)
            return z;
    return 0;
}

void RateBridge::prepare (double hostRate, double modelRate, int maxHostBlock, int latency,
                          int zeroCrossings, double passband)
{
    fh_ = wholeHertz (hostRate);
    fm_ = wholeHertz (modelRate);
    maxHostBlock = std::max (1, maxHostBlock);
    latency_ = std::max (latency, minimumLatency (hostRate, modelRate, zeroCrossings, passband));

    if (passthrough())
    {
        cu_ = cd_ = 1.0;
        wu_ = wd_ = 0;
        maxModelBlock_ = maxHostBlock;
        hostHist_.clear();
        const auto size = nextPow2 (maxHostBlock + latency_ + 2);
        modelHist_.assign (static_cast<std::size_t> (size), 0.0f);
        modelMask_ = size - 1;
        hostMask_ = 0;
        reset();
        return;
    }

    kernel_.design (zeroCrossings, kKaiserBeta);
    const auto w = widthsFor (fh_, fm_, zeroCrossings, passband);
    cu_ = w.cu;
    cd_ = w.cd;
    wu_ = w.wu;
    wd_ = w.wd;
    maxModelBlock_ =
        static_cast<int> (floorDiv (static_cast<std::int64_t> (maxHostBlock) * fm_, fh_)) + 2;

    // Host history: from the oldest tap of the block's first model sample to its newest input.
    const auto hostSpan = maxHostBlock + ceilDiv (fh_, fm_) + 2 * ceilDiv (wu_, fm_) + 8;
    const auto hostSize = nextPow2 (hostSpan);
    hostHist_.assign (static_cast<std::size_t> (hostSize), 0.0f);
    hostMask_ = hostSize - 1;

    // Model history: from the oldest tap of the block's first output (latency back) to the newest
    // model sample.
    const auto modelSpan =
        ceilDiv (static_cast<std::int64_t> (maxHostBlock + latency_ + 2) * fm_, fh_) +
        2 * ceilDiv (wd_ + wu_, fh_) + maxModelBlock_ + 16;
    const auto modelSize = nextPow2 (modelSpan);
    modelHist_.assign (static_cast<std::size_t> (modelSize), 0.0f);
    modelMask_ = modelSize - 1;
    reset();
}

void RateBridge::reset() noexcept
{
    std::fill (hostHist_.begin(), hostHist_.end(), 0.0f);
    std::fill (modelHist_.begin(), modelHist_.end(), 0.0f);
    hostCount_ = modelCount_ = nextModel_ = nextHost_ = 0;
}

int RateBridge::toModel (const float* hostIn, int n, float* modelIn) noexcept
{
    if (passthrough())
    {
        std::copy_n (hostIn, n, modelIn);
        return n;
    }

    for (int s = 0; s < n; ++s)
        hostHist_[static_cast<std::size_t> ((hostCount_ + s) & hostMask_)] = hostIn[s];
    hostCount_ += n;

    const double phases = kernel_.phases();
    const double step = cu_ * phases;
    const std::int64_t limit = hostCount_ * fm_;
    int m = 0;
    // Model sample k sits at time k fh - wu; its taps reach host samples below k fh / fm, all of
    // which have arrived once k fh <= hostCount fm.
    while (nextModel_ * fh_ <= limit && m < maxModelBlock_)
    {
        const std::int64_t tau = nextModel_ * fh_ - wu_;
        const std::int64_t lo = floorDiv (tau - wu_, fm_) + 1;
        const std::int64_t hi = ceilDiv (tau + wu_, fm_) - 1;
        double x =
            cu_ * (static_cast<double> (tau - lo * fm_) / static_cast<double> (fm_)) * phases;
        float acc = 0.0f;
        for (std::int64_t i = lo; i <= hi; ++i, x -= step)
            acc += hostHist_[static_cast<std::size_t> (i & hostMask_)] * kernel_.atTableIndex (x);
        modelIn[m++] = acc * static_cast<float> (cu_);
        ++nextModel_;
    }
    return m;
}

void RateBridge::fromModel (const float* modelOut, int m, float* hostOut, int n) noexcept
{
    for (int s = 0; s < m; ++s)
        modelHist_[static_cast<std::size_t> ((modelCount_ + s) & modelMask_)] = modelOut[s];
    modelCount_ += m;

    if (passthrough())
    {
        for (int s = 0; s < n; ++s)
            hostOut[s] =
                modelHist_[static_cast<std::size_t> ((nextHost_ + s - latency_) & modelMask_)];
        nextHost_ += n;
        return;
    }

    const double phases = kernel_.phases();
    const double step = cd_ * phases;
    for (int s = 0; s < n; ++s)
    {
        // Output j carries the signal of time (j - latency) fm; model sample k that of k fh - wu.
        const std::int64_t sigma = (nextHost_ + s - latency_) * fm_;
        const std::int64_t lo = floorDiv (sigma + wu_ - wd_, fh_) + 1;
        const std::int64_t hi = ceilDiv (sigma + wu_ + wd_, fh_) - 1;
        assert (hi < modelCount_);
        double x = cd_ *
                   (static_cast<double> (sigma - (lo * fh_ - wu_)) / static_cast<double> (fh_)) *
                   phases;
        float acc = 0.0f;
        for (std::int64_t k = lo; k <= hi; ++k, x -= step)
            acc += modelHist_[static_cast<std::size_t> (k & modelMask_)] * kernel_.atTableIndex (x);
        hostOut[s] = acc * static_cast<float> (cd_);
    }
    nextHost_ += n;
}

} // namespace sedef::dsp::capture
