// SPDX-License-Identifier: AGPL-3.0-or-later
// The capture stage: level, calibration, trims, latency, crossfades, the cab toggle, threading.

#include "CaptureTestUtils.h"
#include "amp/AmpTestUtils.h"

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <optional>
#include <thread>

using namespace sedef;
namespace nt = sedef::test::nam;

namespace
{
float largestStep (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float m = 0.0f;
    for (std::size_t i = std::max<std::size_t> (from, 1); i < std::min (to, v.size()); ++i)
        m = std::max (m, std::abs (v[i] - v[i - 1]));
    return m;
}

double levelDb (const std::vector<float>& v, double fs)
{
    return test::dbfs (test::rms (v, static_cast<std::size_t> (0.5 * fs)));
}
} // namespace

TEST_CASE ("A capture plays at the built-in amps' level at every host rate", "[capture]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    const auto f = nt::parseOrFail (nt::standardWaveNet (11));
    nt::StageRig rig (fs, 256);
    rig.load (f);
    rig.settings.useCapture = true;
    auto v = test::guitarStrum (fs, 6.0);
    rig.run (v);
    CAPTURE (fs, levelDb (v, fs), f->normalisationDb());
    // -15.0 at the model's own rate; the rate conversion only removes what is above 20 kHz.
    CHECK (levelDb (v, fs) == Catch::Approx (-15.0).margin (0.25));

    // Next to the built-in Reef at its defaults through the same stage.
    nt::StageRig builtIn (fs, 256);
    auto w = test::guitarStrum (fs, 6.0);
    builtIn.run (w);
    CHECK (std::abs (levelDb (v, fs) - levelDb (w, fs)) < 1.0);
}

TEST_CASE ("Trims and the interface level scale a capture's input and output", "[capture]")
{
    // A linear capture, so every input gain shows up one to one at the output.
    const auto calibrated =
        nt::parseOrFail (nt::linear ({ 1.0f }, 48000.0, { { "input_level_dbu", 19.0 } }));
    const auto plain = nt::parseOrFail (nt::linear ({ 1.0f }));
    const auto render = [] (const std::shared_ptr<const dsp::CaptureFile>& f, float inDb,
                            float outDb, float ifaceDbu) {
        nt::StageRig rig (48000.0, 128);
        rig.load (f);
        rig.settings = { true, inDb, outDb, ifaceDbu, true };
        auto v = test::guitarStrum (48000.0, 6.0); // as measured at load
        rig.run (v);
        return levelDb (v, 48000.0);
    };
    CHECK (render (plain, 0, 0, 12) == Catch::Approx (-15.0).margin (0.02));
    CHECK (render (plain, 6, 0, 12) == Catch::Approx (-9.0).margin (0.02));
    CHECK (render (plain, 0, -6, 12) == Catch::Approx (-21.0).margin (0.02));
    CHECK (render (plain, 0, 0, 24) ==
           Catch::Approx (-15.0).margin (0.02)); // no input level: no calibration
    CHECK (render (calibrated, 0, 0, 12) == Catch::Approx (-15.0).margin (0.02));
    CHECK (render (calibrated, 0, 0, 18) == Catch::Approx (-9.0).margin (0.02)); // hotter interface
    CHECK (render (calibrated, -6, 0, 18) == Catch::Approx (-15.0).margin (0.02));
}

TEST_CASE ("With no capture loaded the stage plays the built-in amp, bit for bit", "[capture]")
{
    nt::StageRig withSwitch (48000.0, 64), reference (48000.0, 64);
    withSwitch.settings.useCapture = true; // nothing loaded: falls back
    auto a = test::guitarStrum (48000.0, 1.0);
    auto b = a;
    withSwitch.run (a);
    reference.run (b);
    CHECK (a == b);

    // And the stage adds nothing at 44.1/48 kHz: the amp alone has the same output.
    dsp::AmpSection amp;
    amp.prepare ({ 48000.0, 64, 1 });
    auto c = test::guitarStrum (48000.0, 1.0);
    for (std::size_t s = 0; s < c.size(); s += 64)
    {
        amp.setSettings ({});
        amp.process (AudioView (c.data() + s, 64));
    }
    CHECK (c == b);
}

TEST_CASE ("Capture and built-in amp line up in time", "[capture]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    // An identity capture against a clean Lagoon at low drive: both impulse responses must peak
    // at the stage's latency (the amp's voicing filters add up to 60 us, as in the amp tests).
    const auto f = nt::parseOrFail (nt::linear ({ 1.0f }));
    const auto peakAt = [fs, &f] (bool capture) {
        nt::StageRig rig (fs, 64);
        rig.load (f);
        rig.settings.useCapture = capture;
        rig.ampSettings.model = dsp::AmpSection::Lagoon;
        rig.ampSettings.lagoon.volume = 0.2f;
        rig.ampSettings.lagoon.master = 0.2f;
        std::vector<float> v (8192, 0.0f);
        v[2000] = 0.25f;
        rig.run (v);
        const auto it = std::max_element (v.begin() + 1000, v.end(), [] (float x, float y) {
            return std::abs (x) < std::abs (y);
        });
        return std::pair { static_cast<int> (it - v.begin()) - 2000, rig.stage.latencySamples() };
    };
    const auto [capturePeak, latency] = peakAt (true);
    const auto [ampPeak, latency2] = peakAt (false);
    CAPTURE (fs, capturePeak, ampPeak, latency);
    CHECK (latency == latency2);
    CHECK (capturePeak == latency);
    CHECK (ampPeak >= latency);
    CHECK (ampPeak <= latency + static_cast<int> (std::ceil (60.0e-6 * fs)));
}

TEST_CASE ("Switching between the amp and a capture is click-free", "[capture]")
{
    const auto f = nt::parseOrFail (nt::standardWaveNet (5));
    nt::StageRig rig (48000.0, 64);
    rig.load (f);
    // A low tone: any step from the switch stands out against its smooth waveform.
    std::vector<float> v (48000 * 2);
    test::fillSine (v, 110.0, 48000.0, 0.2f);
    std::vector<float> steady = v;
    rig.run (steady); // amp only, settled
    const float ampStep = largestStep (steady, 24000, steady.size());

    nt::StageRig capture (48000.0, 64);
    capture.load (f);
    capture.settings.useCapture = true;
    std::vector<float> c = v;
    capture.run (c);
    const float capStep = largestStep (c, 24000, c.size());

    nt::StageRig sw (48000.0, 64);
    sw.load (f);
    std::vector<float> s = v;
    for (std::size_t pos = 0; pos < s.size(); pos += 64)
    {
        sw.settings.useCapture = (pos / 9600) % 2 == 1; // every 200 ms
        std::vector<float> chunk (s.begin() + static_cast<long> (pos),
                                  s.begin() + static_cast<long> (pos + 64));
        sw.run (chunk, 64);
        std::copy (chunk.begin(), chunk.end(), s.begin() + static_cast<long> (pos));
    }
    const float switchStep = largestStep (s, 4800, s.size());
    CAPTURE (ampStep, capStep, switchStep);
    CHECK (test::allFinite (s));
    // A click would be a step of the size of the signal (~0.2); a 30 ms smoothstep between two
    // different waveforms adds at most 1.5 / 1440 of their difference per sample.
    CHECK (switchStep <= 1.5f * std::max (ampStep, capStep));
}

TEST_CASE ("A new capture crossfades in, and the old one is freed off the audio thread",
           "[capture]")
{
    const auto a = nt::parseOrFail (nt::linear ({ 1.0f }));
    const auto b = nt::parseOrFail (nt::linear ({ 0.0f, 0.0f, 0.0f, 1.0f })); // three samples later
    nt::StageRig rig (48000.0, 64);
    rig.settings.useCapture = true;
    rig.load (a);
    std::vector<float> v (48000);
    test::fillSine (v, 220.0, 48000.0, 0.25f);
    std::vector<float> first (v.begin(), v.begin() + 9600);
    rig.run (first);
    const float steady = largestStep (first, 4800, first.size());

    const int destroyedBefore = dsp::CaptureModel::destroyedOnThisThread();
    rig.load (b); // publishes; the old one is retired by the audio thread
    std::vector<float> second (v.begin() + 9600, v.end());
    rig.run (second);
    CHECK (dsp::CaptureModel::destroyedOnThisThread() ==
           destroyedBefore); // nothing freed while playing
    CHECK (largestStep (second, 0, second.size()) <= 1.1f * steady);
    // After 30 ms (+ a block) only the new capture plays: the input three samples late, levelled.
    const auto settled = static_cast<std::size_t> (0.04 * 48000.0);
    const float g = b->instantiate (rig.stage.modelSpec())->outputGain();
    const auto latency = static_cast<std::size_t> (rig.stage.latencySamples());
    for (std::size_t i = settled; i < settled + 200; ++i)
        CHECK (second[i] == Catch::Approx (g * v[9600 + i - 3 - latency]).margin (1.0e-5));

    const int live = dsp::CaptureModel::liveInstances();
    rig.stage.collectGarbage();
    CHECK (dsp::CaptureModel::liveInstances() == live - 1);
}

TEST_CASE ("Clearing a capture fades back to the built-in amp", "[capture]")
{
    const auto f = nt::parseOrFail (nt::standardWaveNet (6));
    nt::StageRig rig (48000.0, 64);
    rig.settings.useCapture = true;
    rig.load (f);
    std::vector<float> v (48000);
    test::fillSine (v, 110.0, 48000.0, 0.2f);
    std::vector<float> head (v.begin(), v.begin() + 12000);
    rig.run (head);
    const float before = largestStep (head, 6000, head.size());
    rig.stage.setModel (nullptr);
    std::vector<float> tail (v.begin() + 12000, v.end());
    rig.run (tail);
    // The amp alone, settled, for comparison.
    nt::StageRig amp (48000.0, 64);
    std::vector<float> ref = v;
    amp.run (ref);
    const float ampStep = largestStep (ref, 24000, ref.size());
    CHECK (largestStep (tail, 0, tail.size()) <= 1.5f * std::max (before, ampStep));
    // Settled on the amp: same as an amp that never stopped, give or take its reset.
    CHECK (levelDb (std::vector<float> (tail.end() - 9600, tail.end()), 0.0) ==
           Catch::Approx (levelDb (std::vector<float> (ref.end() - 9600, ref.end()), 0.0))
               .margin (0.1));
}

TEST_CASE ("A capture that went quiet doesn't replay old notes when it comes back", "[capture]")
{
    // A long decaying impulse response remembers 0.2 s of input.
    std::vector<float> taps (9600);
    for (std::size_t i = 0; i < taps.size(); ++i)
        taps[i] = 0.05f * std::exp (-static_cast<float> (i) / 2000.0f);
    const auto f = nt::parseOrFail (nt::linear (taps));
    nt::StageRig rig (48000.0, 64);
    rig.settings.useCapture = true;
    rig.load (f);
    std::vector<float> burst (9600);
    test::fillNoise (burst, 0.5f, 4);
    rig.run (burst); // fill its history
    rig.settings.useCapture = false;
    std::vector<float> silence (24000, 0.0f);
    rig.run (silence); // the amp plays; the capture settles on silence
    rig.settings.useCapture = true;
    std::vector<float> back (9600, 0.0f);
    rig.run (back);
    CHECK (test::dbfs (test::peak (back)) < -100.0);
}

TEST_CASE ("Full-rig captures can skip the cab, with a crossfade", "[capture]")
{
    const auto f = nt::parseOrFail (nt::standardWaveNet (7));
    const double fs = 48000.0;
    const int block = 64;
    nt::StageRig rig (fs, block);
    rig.load (f);
    rig.settings.useCapture = true;
    test::Buffer out (2, 48000);
    auto in = test::guitarStrum (fs, 1.0);
    const auto cabSettings = dsp::CabSim::Settings {};
    for (int s = 0; s < 48000; s += block)
    {
        rig.settings.throughCab = s < 24000; // switch the cab off halfway
        rig.stage.setSettings (rig.settings);
        rig.stage.process (rig.amp, rig.ampSettings, AudioView (in.data() + s, block));
        std::array<float*, 2> ptrs { out[0].data() + s, out[1].data() + s };
        rig.stage.processCab (rig.cab, cabSettings, in.data() + s,
                              AudioView (ptrs.data(), 2, block));
    }
    CHECK (test::allFinite (out[0]));
    // Without the cab both sides carry the capture itself at the direct level.
    const float g = dbToGain (dsp::CaptureStage::kDirectGainDb);
    for (int i = 30000; i < 30100; ++i)
    {
        CHECK (out[0][static_cast<std::size_t> (i)] ==
               Catch::Approx (g * in[static_cast<std::size_t> (i)]).margin (1e-6));
        CHECK (out[1][static_cast<std::size_t> (i)] == out[0][static_cast<std::size_t> (i)]);
    }
    // The built-in amps always keep the cab, whatever the toggle says.
    nt::StageRig amp (fs, block);
    amp.settings.throughCab = false;
    std::vector<float> x (static_cast<std::size_t> (block), 0.1f);
    test::Buffer o (2, block);
    amp.stage.setSettings (amp.settings);
    amp.stage.process (amp.amp, amp.ampSettings, AudioView (x.data(), block));
    amp.stage.processCab (amp.cab, cabSettings, x.data(), o.view());
    CHECK (o[0] != std::vector<float> (static_cast<std::size_t> (block), g * x[0]));
}

TEST_CASE ("The direct path sits at the cab's level for the built-in amps", "[capture]")
{
    // kDirectGainDb is what the default cab (two mics, both centred) does to the level of the
    // three amps at their defaults, so a full-rig capture levelled like an amp lands with them.
    const double fs = 48000.0;
    double sum = 0.0;
    for (int model = 0; model < 3; ++model)
    {
        nt::StageRig rig (fs, 128);
        rig.ampSettings.model = model;
        auto in = test::guitarStrum (fs, 6.0);
        test::Buffer out (2, static_cast<int> (in.size()));
        dsp::CabSim::Settings cab;
        cab.cab = model;      // linked, as by default
        cab.mics[1].type = 8; // Ribbon Smooth on mic B, the schema default
        for (std::size_t s = 0; s < in.size(); s += 128)
        {
            rig.stage.setSettings (rig.settings);
            rig.stage.process (rig.amp, rig.ampSettings, AudioView (in.data() + s, 128));
            std::array<float*, 2> ptrs { out[0].data() + s, out[1].data() + s };
            rig.stage.processCab (rig.cab, cab, in.data() + s, AudioView (ptrs.data(), 2, 128));
        }
        auto ampOnly = test::guitarStrum (fs, 6.0);
        nt::StageRig ref (fs, 128);
        ref.ampSettings.model = model;
        ref.run (ampOnly);
        const double cabGain = levelDb (out[0], fs) - levelDb (ampOnly, fs);
        CAPTURE (model, cabGain);
        sum += cabGain;
    }
    CAPTURE (sum / 3.0);
    CHECK (std::abs (sum / 3.0 - dsp::CaptureStage::kDirectGainDb) < 1.0);
}

TEST_CASE ("Captures play at every block size and rate with finite output", "[capture]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const int block = GENERATE (1, 17, 64, 512, 4096);
    const auto f = nt::parseOrFail (nt::standardWaveNet (8));
    nt::StageRig rig (fs, block);
    rig.load (f);
    rig.settings.useCapture = true;
    std::vector<float> v (static_cast<std::size_t> (fs * 0.25));
    test::fillNoise (v, 1.0f, 3);
    rig.run (v, block);
    CAPTURE (fs, block);
    CHECK (test::allFinite (v));
    CHECK (test::peak (v) < 100.0f);
}

TEST_CASE ("Captures built for another rate are ignored until rebuilt", "[capture]")
{
    const auto f = nt::parseOrFail (nt::linear ({ 1.0f }));
    nt::StageRig rig (48000.0, 64);
    rig.settings.useCapture = true;
    rig.stage.setModel (f->instantiate ({ 44100.0, 64, 37 })); // stale spec
    nt::StageRig reference (48000.0, 64);
    auto a = test::guitarStrum (48000.0, 0.5);
    auto b = a;
    rig.run (a);
    reference.run (b);
    CHECK (a == b); // the built-in amp
}

TEST_CASE ("Publishing, clearing and collecting while audio runs is safe", "[capture]")
{
    const auto f = nt::parseOrFail (nt::standardWaveNet (9));
    const auto g = nt::parseOrFail (nt::linear ({ 0.5f, 0.5f }));
    const int liveBefore = dsp::CaptureModel::liveInstances();
    std::optional<nt::StageRig> holder;
    holder.emplace (48000.0, 64);
    auto& rig = *holder;
    rig.settings.useCapture = true;
    std::atomic<bool> stop { false };
    std::atomic<int> destroyedOnAudio { 0 };
    std::atomic<bool> finite { true };
    std::thread audio ([&] {
        const int before = dsp::CaptureModel::destroyedOnThisThread();
        std::vector<float> v (64);
        Random r (1);
        while (!stop.load())
        {
            for (auto& s : v)
                s = 0.3f * r.bipolar();
            rig.stage.setSettings (rig.settings);
            rig.stage.process (rig.amp, rig.ampSettings, AudioView (v.data(), 64));
            if (!test::allFinite (v))
                finite = false;
        }
        destroyedOnAudio = dsp::CaptureModel::destroyedOnThisThread() - before;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (600);
    int round = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        const auto spec = rig.stage.modelSpec();
        switch (round++ % 3)
        {
        case 0:
            rig.stage.setModel (f->instantiate (spec));
            break;
        case 1:
            rig.stage.setModel (g->instantiate (spec));
            break;
        default:
            rig.stage.setModel (nullptr);
            break;
        }
        rig.stage.collectGarbage();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    stop = true;
    audio.join();
    CHECK (finite.load());
    CHECK (destroyedOnAudio.load() == 0);
    CHECK (round > 10);
    rig.stage.setModel (nullptr);
    rig.stage.collectGarbage();
    // At most the playing model, one still fading out and the pending "cleared" marker are left,
    // and the stage frees those with itself.
    CHECK (dsp::CaptureModel::liveInstances() <= liveBefore + 3);
    holder.reset();
    CHECK (dsp::CaptureModel::liveInstances() == liveBefore);
}
