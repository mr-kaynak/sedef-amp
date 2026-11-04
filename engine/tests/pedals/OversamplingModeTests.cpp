// SPDX-License-Identifier: AGPL-3.0-or-later
// Every oversampled stage sounds the same in both oversampling modes (docs/dsp/oversampling.md,
// "Tone"): the same response and the same harmonics, to a twentieth of a decibel.
#include "PedalTestUtils.h"
#include "amp/AmpTestUtils.h"
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Kelp.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/Remora.h"
#include "sedef/dsp/Sargasso.h"
#include "sedef/dsp/Urchin.h"
#include "sedef/dsp/amp/AmpSection.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <functional>
#include <string>
#include <vector>

using namespace sedef;

namespace
{
using Run = std::function<void (std::vector<float>&, double, OversamplingMode)>;

struct Stage
{
    std::string name;
    Run run;
    bool rectifies = false; ///< the note leaves an octave up: no small-signal response at f
};

template <typename Module>
Run pedal (typename Module::Settings s)
{
    return [s] (std::vector<float>& v, double fs, OversamplingMode mode) {
        Module m;
        m.prepare ({ fs, 256, 1, mode });
        m.setSettings (s);
        for (size_t i = 0; i < v.size(); i += 256)
        {
            float* p = v.data() + i;
            m.process (AudioView (&p, 1, static_cast<int> (std::min<size_t> (256, v.size() - i))));
        }
    };
}

Run amp (int model, float gain)
{
    return [model, gain] (std::vector<float>& v, double fs, OversamplingMode mode) {
        dsp::AmpSection a;
        dsp::AmpSection::Settings s;
        s.model = model;
        s.lagoon.volume = gain;
        s.reef.gain = gain;
        s.abyss.gain = gain;
        test::prepareAmp (a, s, fs, 256, mode);
        test::runAmp (a, v, 256);
    };
}

/// Each stage at its main knob's minimum, middle and maximum.
std::vector<Stage> stages()
{
    std::vector<Stage> out;
    for (const float k : { 0.0f, 0.5f, 1.0f })
    {
        const auto at = [k] (const char* n) {
            return std::string (n) + " " + std::to_string (k).substr (0, 3);
        };
        out.push_back ({ at ("Remora"),
                         pedal<dsp::Remora> ({ k, 0.5f, 0.5f, 1.0f, dsp::Remora::Germanium }) });
        out.push_back ({ at ("Urchin"), pedal<dsp::Urchin> ({ k, 1.0f, 0.5f, 0.5f }), true });
        out.push_back ({ at ("Sargasso"), pedal<dsp::Sargasso> ({ k, 0.5f, 0.5f }) });
        out.push_back ({ at ("Pearl"), pedal<dsp::Pearl> ({ k, 0.5f, 0.5f, 0.5f }) });
        out.push_back ({ at ("Kelp"), pedal<dsp::Kelp> ({ k, 0.5f, 0.5f }) });
        out.push_back ({ at ("Coral"), pedal<dsp::Coral> ({ k, 0.5f, 0.5f }) });
        out.push_back ({ at ("Breaker"), pedal<dsp::Breaker> ({ k, dsp::Breaker::Treble }) });
        for (int model : { 0, 1, 2 })
            out.push_back (
                { at (std::array { "Lagoon", "Reef", "Abyss" }[static_cast<size_t> (model)]),
                  amp (model, k) });
    }
    return out;
}

double levelAt (const Run& run, double hz, double fs, OversamplingMode mode, float amp)
{
    std::vector<float> v (32768);
    test::fillSine (v, hz, fs, amp);
    run (v, fs, mode);
    return test::toneMagnitude (v.data() + 16384, 16384, hz, fs);
}
} // namespace

TEST_CASE ("Low latency and linear phase have the same small-signal response", "[pedals][amp]")
{
    // Where every stage is nearly linear (-40 dBFS): the oversampler's phase in the top octave
    // must not change the response, which it would where a clean path is summed with an
    // oversampled one (Pearl's and Kelp's, AlignedDryPath).
    const double fs = GENERATE (44100.0, 48000.0);
    for (const auto& st : stages())
        if (!st.rectifies)
            for (const double hz : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 })
            {
                const double low = levelAt (st.run, hz, fs, OversamplingMode::lowLatency, 0.01f);
                const double lin = levelAt (st.run, hz, fs, OversamplingMode::linearPhase, 0.01f);
                CAPTURE (st.name, fs, hz);
                CHECK (std::abs (20.0 * std::log10 (low / lin)) <= 0.05);
            }
}

TEST_CASE ("Low latency and linear phase distort alike", "[pedals][amp]")
{
    // Driven like a guitar (-20 dBFS): the first ten harmonics within 60 dB of the strongest
    // agree to 0.05 dB, so THD and the tone of the distortion are the same.
    for (const auto& st : stages())
        for (const double f0 : { 110.0, 440.0, 1320.0 })
        {
            std::array<std::array<double, 10>, 2> h {};
            for (const auto mode : { OversamplingMode::lowLatency, OversamplingMode::linearPhase })
            {
                std::vector<float> v (65536);
                test::fillSine (v, f0, 48000.0, 0.1f);
                st.run (v, 48000.0, mode);
                for (int k = 1; k <= 10; ++k)
                    h[static_cast<size_t> (mode)][static_cast<size_t> (k - 1)] =
                        k * f0 < 20000.0
                            ? test::toneMagnitude (v.data() + 32768, 32768, k * f0, 48000.0)
                            : 0.0;
            }
            const double top = *std::max_element (h[1].begin(), h[1].end());
            for (size_t k = 0; k < 10; ++k)
                if (h[1][k] > top * 1.0e-3)
                {
                    CAPTURE (st.name, f0, k + 1);
                    CHECK (std::abs (20.0 * std::log10 (h[0][k] / h[1][k])) <= 0.05);
                }
        }
}
