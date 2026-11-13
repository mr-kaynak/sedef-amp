// SPDX-License-Identifier: AGPL-3.0-or-later
// Re-amp (docs/recorder.md, section 6): a file reaches the rig exactly as live input would,
// loops without a seam, and is converted when its rate differs.
#include "RecordTestUtils.h"
#include "sedef/core/InputFold.h"
#include "sedef/record/RateConverter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cstring>

using namespace sedef;
using namespace sedef::record;
using namespace sedef::test::rec;

namespace
{
std::vector<std::vector<float>> fileOf (int channels, int frames, double fs, std::uint32_t seed)
{
    std::vector<std::vector<float>> f;
    for (int c = 0; c < channels; ++c)
        f.push_back (plucks (frames, fs, seed + static_cast<std::uint32_t> (c)));
    return f;
}

/// The input a device would deliver if the file had been played into it from frame `start`:
/// repeated (`loop`) or once, then silence; mapped onto `channels` like the feeder maps.
test::Buffer asLive (const std::vector<std::vector<float>>& file, int channels, int frames,
                     int start, bool loop, double fs)
{
    const int n = static_cast<int> (file[0].size());
    const int fileChannels = static_cast<int> (file.size());
    std::vector<std::vector<float>> mapped (static_cast<std::size_t> (channels),
                                            std::vector<float> (static_cast<std::size_t> (frames)));
    InputFold fold;
    fold.prepare (fs);
    std::vector<float> l (1), r (1), m (1);
    for (int t = start; t < frames; ++t)
    {
        const int k = t - start;
        if (!loop && k >= n)
            break;
        const auto i = static_cast<std::size_t> (k % n);
        for (int c = 0; c < channels; ++c)
        {
            float v = 0.0f;
            if (channels == 1 && fileChannels == 2)
            {
                l[0] = file[0][i];
                r[0] = file[1][i];
                fold.process (l.data(), r.data(), m.data(), 1);
                v = m[0];
            }
            else
            {
                v = file[static_cast<std::size_t> (std::min (c, fileChannels - 1))][i];
            }
            mapped[static_cast<std::size_t> (c)][static_cast<std::size_t> (t)] = v;
        }
    }
    test::Buffer b (channels, frames);
    b.data = mapped;
    return b;
}

/// The player alone over an input buffer, pumping the feeder after every block (as the reader
/// thread would, but deterministically). Returns the frame where playing started.
struct PlayerRig
{
    PlayerRig (double sampleRate, int channels, int frames, int maxBlock = 4096,
               double fifoSeconds = ReampPlayer::kFifoSeconds)
        : fs (sampleRate),
          in (channels, frames)
    {
        player.prepare (fs, channels, maxBlock, fifoSeconds);
    }

    /// Runs [from, to) in blocks; `before` (block start) may act before each block.
    template <typename Before>
    void run (int from, int to, Blocks& blocks, Before&& before, bool pump = true)
    {
        for (int at = from; at < to;)
        {
            before (at);
            const int n = std::min (blocks(), to - at);
            std::array<float*, 2> p { in.ptrs[0] + at,
                                      in.channels() > 1 ? in.ptrs[1] + at : nullptr };
            player.process (p.data(), in.channels(), n);
            if (pump)
                REQUIRE (feeder.pump (player));
            if (start < 0 && player.consumed() > 0)
                start = at + n - static_cast<int> (player.consumed());
            at += n;
        }
    }

    void run (int from, int to, Blocks& blocks, bool pump = true)
    {
        run (from, to, blocks, [] (int) {}, pump);
    }

    double fs;
    test::Buffer in;
    ReampPlayer player;
    ReampFeeder feeder;
    int start = -1;
};
} // namespace

TEST_CASE ("Re-amp: a file played through the rig equals the same file as live input, bit-exact",
           "[record][reamp]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const auto pattern = GENERATE (from_range (test::rec::blockPatterns()));
    const auto [fileChannels, ins] =
        GENERATE (std::pair { 1, 1 }, std::pair { 2, 2 }, std::pair { 1, 2 }, std::pair { 2, 1 });
    CAPTURE (fs, pattern, fileChannels, ins);

    const int frames = static_cast<int> (fs * 0.8);
    const auto file = fileOf (fileChannels, static_cast<int> (fs * 0.3) + 7, fs, 21);
    MemorySource source (file, fs);

    auto p = ParamSnapshot::defaults();
    p[ParamId::ampModel] = 2.0f;
    p[ParamId::driftEnabled] = 1.0f;

    // The deck re-amping the file, the live input silent.
    DeckRig rig (fs, ins, 2, frames);
    auto& player = rig.deck.reamp();
    ReampFeeder feeder;
    feeder.setSource (player, &source);
    feeder.setLoop (player, true);
    feeder.play (player);
    Blocks blocks { pattern };
    int start = -1;
    for (int at = 0; at < frames;)
    {
        const int n = std::min (blocks(), frames - at);
        rig.process (at, n, p);
        REQUIRE (feeder.pump (player));
        if (start < 0 && player.consumed() > 0)
            start = at + n - static_cast<int> (player.consumed());
        at += n;
    }
    REQUIRE (start >= 0);
    CHECK (player.underruns() == 0);

    // The same file fed as the device's input to a second, identical rig.
    auto live = asLive (file, ins, frames, start, true, fs);
    CHECK (rig.in.data == live.data); // what the rig heard
    Engine reference;
    reference.prepare (fs, 4096, ins, 2);
    test::Buffer out (2, frames);
    Blocks again { pattern };
    Transport transport;
    for (int at = 0; at < frames;)
    {
        const int n = std::min (again(), frames - at);
        std::array<const float*, 2> ip { live.ptrs[0] + at, ins > 1 ? live.ptrs[1] + at : nullptr };
        std::array<float*, 2> op { out.ptrs[0] + at, out.ptrs[1] + at };
        reference.process (ip.data(), op.data(), n, p, transport);
        at += n;
    }
    for (int c = 0; c < 2; ++c)
        CHECK (std::memcmp (rig.out.data[static_cast<std::size_t> (c)].data(),
                            out.data[static_cast<std::size_t> (c)].data(),
                            static_cast<std::size_t> (frames) * sizeof (float)) == 0);
}

TEST_CASE ("Re-amp: the loop seam adds and drops nothing, at any block size and rate",
           "[record][reamp]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const auto pattern = GENERATE (from_range (test::rec::blockPatterns()));
    // Shorter than a block (several seams in one), and longer than the reader's reads.
    const int length = GENERATE (1001, 9001);
    CAPTURE (fs, pattern, length);

    const int frames = 40000;
    const auto file = fileOf (2, length, fs, 31);
    MemorySource source (file, fs);
    PlayerRig rig (fs, 2, frames);
    rig.feeder.setSource (rig.player, &source);
    rig.feeder.play (rig.player);
    Blocks blocks { pattern };
    rig.run (0, frames, blocks);
    REQUIRE (rig.start >= 0);
    CHECK (rig.in.data == asLive (file, 2, frames, rig.start, true, fs).data);
    CHECK (rig.player.underruns() == 0);
    CHECK (rig.feeder.position (rig.player) == (frames - rig.start) % length);
}

TEST_CASE ("Re-amp: Loop off finishes the pass in play; Loop on before the end goes on",
           "[record][reamp]")
{
    const double fs = 48000.0;
    const int frames = 20000;
    Blocks blocks { { 64 } };

    SECTION ("Loop off from the start: one pass")
    {
        const auto file = fileOf (1, 5000, fs, 41);
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.setLoop (rig.player, false);
        rig.feeder.play (rig.player);
        rig.run (0, frames, blocks);
        CHECK (rig.in.data == asLive (file, 1, frames, rig.start, false, fs).data);
        CHECK (rig.player.endings() == 1);
        CHECK_FALSE (rig.player.playing());
        CHECK (rig.feeder.state() == ReampFeeder::State::idle);
    }

    SECTION ("Loop off during the second pass of a short file: that pass is the last")
    {
        // 1000 frames: the FIFO already holds many passes when Loop goes off.
        const auto file = fileOf (1, 1000, fs, 43);
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.play (rig.player);
        bool turnedOff = false;
        rig.run (0, frames, blocks, [&] (int) {
            if (!turnedOff && rig.player.consumed() >= 1500)
            {
                rig.feeder.setLoop (rig.player, false);
                turnedOff = true;
            }
        });
        REQUIRE (turnedOff);
        CHECK (rig.feeder.limit() == 2000);
        auto expected = asLive (file, 1, frames, rig.start, true, fs);
        std::fill (expected[0].begin() + rig.start + 2000, expected[0].end(), 0.0f);
        CHECK (rig.in.data == expected.data);
        CHECK (rig.player.endings() == 1);
    }

    SECTION ("Loop on before the end of a single pass: it carries on without a gap")
    {
        const auto file = fileOf (1, 5000, fs, 47);
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.setLoop (rig.player, false);
        rig.feeder.play (rig.player);
        bool turnedOn = false;
        rig.run (0, frames, blocks, [&] (int) {
            if (!turnedOn && rig.player.consumed() >= 3000)
            {
                rig.feeder.setLoop (rig.player, true);
                turnedOn = true;
            }
        });
        CHECK (rig.in.data == asLive (file, 1, frames, rig.start, true, fs).data);
        CHECK (rig.player.endings() == 0);
        CHECK (rig.player.playing());
    }
}

TEST_CASE ("Re-amp: Mix adds exactly; Replace and Stop fade over 10 ms", "[record][reamp]")
{
    const double fs = 48000.0;
    const int frames = 12000;
    const int fade = 480; // 10 ms
    Blocks blocks { { 100 } };

    SECTION ("Mix: the file plus the live input")
    {
        const auto file = fileOf (1, 3000, fs, 51);
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        test::fillNoise (rig.in[0], 0.2f, 52);
        const auto live = rig.in[0];
        rig.player.setMix (true);
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.play (rig.player);
        rig.run (0, frames, blocks);
        const auto f = asLive (file, 1, frames, rig.start, true, fs);
        for (int t = 0; t < frames; ++t)
            if (rig.in[0][static_cast<std::size_t> (t)] !=
                live[static_cast<std::size_t> (t)] + f.data[0][static_cast<std::size_t> (t)])
                FAIL ("frame " << t);
    }

    SECTION ("Replace: the live input fades out as the file starts, the file does not fade in")
    {
        std::vector<std::vector<float>> file { std::vector<float> (6000, 0.0f) };
        file[0][0] = 0.75f; // the file's first sample arrives whole
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        std::fill (rig.in[0].begin(), rig.in[0].end(), 0.5f);
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.setLoop (rig.player, false);
        rig.feeder.play (rig.player);
        rig.run (0, frames, blocks);
        const auto s = static_cast<std::size_t> (rig.start);
        CHECK (rig.in[0][s - 1] == 0.5f);
        CHECK (rig.in[0][s] == 0.75f + 0.5f * (1.0f - 1.0f / fade));
        for (int i = 1; i < fade; ++i)
            CHECK (rig.in[0][s + static_cast<std::size_t> (i)] ==
                   Catch::Approx (0.5 * (1.0 - (i + 1.0) / fade)).margin (1e-5));
        for (int i = fade; i < 6000; ++i)
            REQUIRE (rig.in[0][s + static_cast<std::size_t> (i)] == 0.0f);
        // After the file ends, the live input fades back in.
        CHECK (rig.in[0][s + 6000 + static_cast<std::size_t> (fade) + 10] == 0.5f);
    }

    SECTION ("Stop: the file fades out, then stops")
    {
        std::vector<std::vector<float>> file { std::vector<float> (2000, 0.25f) };
        MemorySource source (file, fs);
        PlayerRig rig (fs, 1, frames);
        rig.player.setMix (true); // the live input (silent) stays open
        rig.feeder.setSource (rig.player, &source);
        rig.feeder.play (rig.player);
        rig.run (0, 3000, blocks);
        CHECK (rig.player.playing());
        rig.feeder.stop (rig.player);
        rig.run (3000, frames, blocks);
        CHECK_FALSE (rig.player.playing());
        for (int i = 0; i < fade; ++i)
            CHECK (rig.in[0][3000 + static_cast<std::size_t> (i)] ==
                   Catch::Approx (0.25 * (1.0 - (i + 1.0) / fade)).margin (1e-5));
        for (int t = 3000 + fade; t < frames; ++t)
            REQUIRE (rig.in[0][static_cast<std::size_t> (t)] == 0.0f);
        CHECK (rig.player.endings() == 0);
    }
}

TEST_CASE ("Re-amp: a reader that falls behind pauses the file, it never skips", "[record][reamp]")
{
    // A FIFO of 4096 frames (0.05 s at 48 kHz, rounded up to a power of two). The reader stops
    // for 6000 frames: the input is silent once the FIFO runs dry, and the file resumes exactly
    // where it was.
    const double fs = 48000.0;
    const int frames = 20000;
    const auto file = fileOf (1, 15000, fs, 61);
    MemorySource source (file, fs);
    PlayerRig rig (fs, 1, frames, 64, 0.05);
    REQUIRE (rig.player.fifo().capacity() == 4096);
    rig.feeder.setSource (rig.player, &source);
    rig.feeder.setLoop (rig.player, false);
    rig.feeder.play (rig.player);
    Blocks blocks { { 64 } };
    rig.run (0, 128, blocks); // flushed, filled, started
    rig.run (128, 128 + 6000, blocks, false);
    // It started at 64 and was topped up after that block: 64 + 4096 frames, then nothing.
    CHECK (rig.start == 64);
    CHECK (rig.player.underruns() > 0);
    CHECK (rig.player.consumed() == 64 + 4096);
    rig.run (128 + 6000, frames, blocks);

    const int s = rig.start;
    const int gapStart = 128 + 4096;
    const int gapEnd = 128 + 6000 + 64; // the block after the reader came back
    const int resumed = 64 + 4096;      // the file frame it resumes with
    for (int t = 0; t < frames; ++t)
    {
        float expected = 0.0f;
        if (t >= s && t < gapStart)
            expected = file[0][static_cast<std::size_t> (t - s)];
        else if (t >= gapEnd && t - gapEnd + resumed < 15000)
            expected = file[0][static_cast<std::size_t> (t - gapEnd + resumed)];
        if (rig.in[0][static_cast<std::size_t> (t)] != expected)
            FAIL ("frame " << t);
    }
}

// -------------------------------------------------------------------------------------------------
// Rate conversion

TEST_CASE ("RateConverter: lengths, identity at equal rates, unity at DC", "[record][reamp]")
{
    CHECK (RateConverter::outputLength (44100, 44100.0, 48000.0) == 48000);
    CHECK (RateConverter::outputLength (1, 44100.0, 48000.0) == 2); // times 0 and 0.92
    CHECK (RateConverter::outputLength (48000, 96000.0, 48000.0) == 24000);
    CHECK (RateConverter::outputLength (48001, 96000.0, 48000.0) == 24001);

    const auto file = fileOf (2, 5000, 48000.0, 71);
    MemorySource same (file, 48000.0);
    std::vector<std::vector<float>> out (2);
    const auto collect = [&out] (const float* const* p, int n) {
        for (std::size_t c = 0; c < out.size(); ++c)
            out[c].insert (out[c].end(), p[c], p[c] + n);
        return true;
    };
    REQUIRE (RateConverter::convert (same, 48000.0, collect));
    CHECK (out == file);

    for (const auto& [from, to] :
         { std::pair { 44100.0, 48000.0 }, std::pair { 48000.0, 44100.0 },
           std::pair { 96000.0, 44100.0 }, std::pair { 22050.0, 48000.0 } })
    {
        CAPTURE (from, to);
        std::vector<double> w;
        for (const double frac : { 0.0, 0.25, 0.5, 0.9 })
        {
            RateConverter::weights (from, to, frac, w);
            double sum = 0.0;
            for (const double x : w)
                sum += x;
            CHECK (sum == Catch::Approx (1.0).margin (1e-12));
        }
    }
}

TEST_CASE ("RateConverter: a sine comes through clean, images and aliases are gone",
           "[record][reamp]")
{
    struct Case
    {
        double from, to, hz;
    };
    for (const auto& c : { Case { 44100.0, 48000.0, 1000.0 }, Case { 48000.0, 44100.0, 1000.0 },
                           Case { 44100.0, 96000.0, 5000.0 }, Case { 96000.0, 48000.0, 12000.0 },
                           Case { 88200.0, 44100.0, 440.0 }, Case { 44100.0, 48000.0, 17000.0 },
                           Case { 48000.0, 44100.0, 17000.0 } })
    {
        CAPTURE (c.from, c.to, c.hz);
        std::vector<float> x (static_cast<std::size_t> (c.from));
        test::fillSine (x, c.hz, c.from, 0.5f);
        MemorySource source ({ x }, c.from);
        std::vector<float> y;
        REQUIRE (RateConverter::convert (source, c.to, [&y] (const float* const* p, int n) {
            y.insert (y.end(), p[0], p[0] + n);
            return true;
        }));
        REQUIRE (static_cast<std::int64_t> (y.size()) ==
                 RateConverter::outputLength (static_cast<std::int64_t> (x.size()), c.from, c.to));
        // Away from the ends (where the file meets the silence around it), the exact sine.
        const int margin = static_cast<int> (c.to * 0.01);
        double worst = 0.0;
        for (int k = margin; k < static_cast<int> (y.size()) - margin; ++k)
        {
            const double ideal = 0.5 * std::sin (kTwoPiD * c.hz * k / c.to);
            worst = std::max (worst, std::abs (y[static_cast<std::size_t> (k)] - ideal));
        }
        CAPTURE (20.0 * std::log10 (worst / 0.5));
        CHECK (worst / 0.5 < 1.0e-5); // -100 dB
    }

    // Downsampling: a tone above the new Nyquist frequency must not fold back.
    std::vector<float> x (96000);
    test::fillSine (x, 30000.0, 96000.0, 0.5f);
    MemorySource high ({ x }, 96000.0);
    std::vector<float> y;
    REQUIRE (RateConverter::convert (high, 48000.0, [&y] (const float* const* p, int n) {
        y.insert (y.end(), p[0], p[0] + n);
        return true;
    }));
    const double alias = test::toneMagnitude (y.data() + 4800, 38400, 18000.0, 48000.0);
    CAPTURE (20.0 * std::log10 (alias / 0.5));
    CHECK (alias / 0.5 < 1.0e-5);
}
