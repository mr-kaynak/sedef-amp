// SPDX-License-Identifier: AGPL-3.0-or-later
// The rig's latency in both oversampling modes (docs/dsp/oversampling.md): what the host is told,
// that it never changes with what is switched on, and that it is the rig's real delay.
#include "TestUtils.h"
#include "capture/CaptureTestUtils.h"
#include "sedef/Engine.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/capture/CaptureStage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <vector>

using namespace sedef;
namespace nt = sedef::test::nam;

namespace
{
constexpr auto kLow = OversamplingMode::lowLatency;
constexpr auto kLinear = OversamplingMode::linearPhase;

/// Every pedal that oversamples, so carries latency.
constexpr std::array kOversampledPedals { ParamId::remoraEnabled,   ParamId::urchinEnabled,
                                          ParamId::sargassoEnabled, ParamId::pearlEnabled,
                                          ParamId::kelpEnabled,     ParamId::coralEnabled,
                                          ParamId::breakerEnabled };

int latencyOf (double fs, OversamplingMode mode, int block = 64)
{
    Engine e;
    e.setOversampling (mode);
    e.prepare (fs, block, 1, 2);
    return e.latencySamples();
}

/// A mono-in rig that renders `in` with fixed parameters.
struct LatencyRig
{
    LatencyRig (double sampleRate, OversamplingMode mode, int blockSize = 64)
        : fs (sampleRate),
          block (blockSize)
    {
        engine.setOversampling (mode);
        engine.prepare (fs, block, 1, 2);
    }

    /// An identity capture made at `modelRate` on every chain.
    void loadIdentityCapture (double modelRate)
    {
        const auto f = nt::parseOrFail (nt::linear ({ 1.0f }, modelRate));
        for (int c = 0; c < engine.numCabs(); ++c)
            engine.capture (c).setModel (f->instantiate (engine.capture (c).modelSpec()));
    }

    std::vector<float> render (const std::vector<float>& in, const ParamSnapshot& p)
    {
        std::vector<float> l (in.size()), r (in.size());
        Transport transport;
        for (size_t s = 0; s < in.size(); s += static_cast<size_t> (block))
        {
            const int n =
                static_cast<int> (std::min<size_t> (static_cast<size_t> (block), in.size() - s));
            std::array<const float*, 2> ip { in.data() + s, nullptr };
            std::array<float*, 2> op { l.data() + s, r.data() + s };
            engine.process (ip.data(), op.data(), n, p, transport);
        }
        return l;
    }

    Engine engine;
    double fs;
    int block;
};

/// Everything that colours the signal off: no gate, no EQ, no effects. The amp, the cab and the
/// pedals are what each test switches.
ParamSnapshot plainRig()
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::eqLagoonEnabled] = 0.0f;
    p[ParamId::eqReefEnabled] = 0.0f;
    p[ParamId::eqAbyssEnabled] = 0.0f;
    return p;
}

/// The rig's phase delay at `hz`, in samples: a steady sine through it.
double rigDelay (LatencyRig& rig, const ParamSnapshot& p, double hz, int expected)
{
    std::vector<float> in (65536);
    test::fillSine (in, hz, rig.fs, 0.02f);
    const auto out = rig.render (in, p);
    return test::toneDelaySamples (in.data() + 32768, out.data() + 32768, 32768, hz, rig.fs,
                                   expected);
}
} // namespace

TEST_CASE ("Rig latency per mode and sample rate", "[engine][latency]")
{
    // Low latency: 5 samples per oversampled stage (seven pedals and the amp) at 44.1/48 kHz,
    // 3 at 88.2/96 kHz, none above, and the capture stage's share where it needs more than the
    // amp (docs/dsp/oversampling.md, "Latency"). Linear phase: 0.4.0's figures.
    struct Row
    {
        double fs;
        int low, linear;
    };
    for (const auto& r :
         { Row { 44100.0, 40, 296 }, Row { 48000.0, 41, 298 }, Row { 88200.0, 31, 289 },
           Row { 96000.0, 32, 295 }, Row { 176400.0, 21, 143 }, Row { 192000.0, 23, 155 } })
    {
        CAPTURE (r.fs);
        CHECK (latencyOf (r.fs, kLow) == r.low);
        CHECK (latencyOf (r.fs, kLinear) == r.linear);
        // Under a millisecond at every rate; under 0.35 ms from 88.2 kHz up.
        CHECK (r.low * 1000.0 / r.fs < (r.fs < 80000.0 ? 0.91 : 0.37));
    }
    // The setting holds until changed, and the block size has nothing to do with it.
    Engine e;
    CHECK (e.oversampling() == kLow);
    e.setOversampling (kLinear);
    e.prepare (48000.0, 512, 2, 2);
    CHECK (e.latencySamples() == 298);
    e.setOversampling (kLow);
    e.prepare (48000.0, 1, 2, 2);
    CHECK (e.latencySamples() == 41);
    CHECK (latencyOf (48000.0, kLow, 4096) == 41);
}

TEST_CASE ("Reported latency: an impulse through the plain rig arrives exactly then",
           "[engine][latency]")
{
    // Pedals off, an identity capture made at the session rate playing with the cab out: every
    // stage is a whole-sample delay, so the impulse arrives exactly at the reported latency.
    const auto mode = GENERATE (kLow, kLinear);
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    LatencyRig rig (fs, mode);
    rig.loadIdentityCapture (fs);
    auto p = plainRig();
    p[ParamId::ampSource] = 1.0f;
    p[ParamId::captureCab] = 0.0f;
    std::vector<float> in (16384, 0.0f);
    rig.render (in, p); // past the switch to the capture
    in[1000] = 0.1f;
    const auto out = rig.render (in, p);
    const auto peak = std::max_element (
        out.begin(), out.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
    const int latency = rig.engine.latencySamples();
    CAPTURE (fs, static_cast<int> (mode), latency);
    CHECK (peak - out.begin() == 1000 + latency);
    // And nothing before it: the whole-sample delays don't ring.
    for (int i = 0; i < 1000 + latency; ++i)
        CHECK (std::abs (out[static_cast<size_t> (i)]) < 1.0e-6f);
}

TEST_CASE ("Reported latency: the whole rig is the linear-phase rig, earlier by what it saves",
           "[engine][latency]")
{
    // Linear phase is exact by construction (pure delays, the impulse test above). At guitar
    // frequencies the low-latency rig lags its own reported latency by what the linear-phase rig
    // lags its own (the amp's and cab's voicing): within half a sample, with the default amp and
    // cab, with a treble booster into an overdrive in front of it, and with a capture made at
    // 44.1 kHz.
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    enum Setup
    {
        defaultRig,
        boosted,
        capture441
    };
    const auto setup = GENERATE (defaultRig, boosted, capture441);
    for (const double hz : { 110.0, 330.0, 1000.0 })
    {
        double relative[2] {};
        for (const auto mode : { kLow, kLinear })
        {
            LatencyRig rig (fs, mode);
            auto p = plainRig();
            if (setup == boosted)
            {
                p[ParamId::kelpEnabled] = 1.0f;
                p[ParamId::breakerEnabled] = 1.0f;
            }
            if (setup == capture441)
            {
                rig.loadIdentityCapture (44100.0);
                p[ParamId::ampSource] = 1.0f;
            }
            const int latency = rig.engine.latencySamples();
            relative[static_cast<int> (mode)] = rigDelay (rig, p, hz, latency) - latency;
        }
        CAPTURE (fs, static_cast<int> (setup), hz, relative[0], relative[1]);
        CHECK (std::abs (relative[0] - relative[1]) <= 0.5);
    }
}

TEST_CASE ("Switching a pedal moves the timing by its voicing alone, in both modes",
           "[engine][latency]")
{
    // The latency the host compensates never changes (a bypassed pedal's dry path is delayed by
    // its oversampler's latency), so what switching a pedal does to the timing at guitar
    // frequencies is its own voicing's phase. Low latency must not add to it: the shift from
    // off to on is the same as in linear phase to a tenth of a sample. (Not Urchin: its rectifier
    // moves the note an octave up, so the fundamental's phase at its output is no timing.)
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (const auto pedal : kOversampledPedals)
        for (const double hz : { 110.0, 330.0 })
        {
            if (pedal == ParamId::urchinEnabled)
                continue;
            double shift[2] {};
            for (const auto mode : { kLow, kLinear })
            {
                LatencyRig off (fs, mode), on (fs, mode);
                auto p = plainRig();
                const int latency = off.engine.latencySamples();
                const double dOff = rigDelay (off, p, hz, latency);
                p[pedal] = 1.0f;
                CHECK (on.engine.latencySamples() == latency);
                shift[static_cast<int> (mode)] = rigDelay (on, p, hz, latency) - dOff;
            }
            CAPTURE (fs, static_cast<int> (pedal), hz, shift[0], shift[1]);
            CHECK (std::abs (shift[0] - shift[1]) <= 0.1);
        }
}
