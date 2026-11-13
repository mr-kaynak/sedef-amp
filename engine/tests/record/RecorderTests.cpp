// SPDX-License-Identifier: AGPL-3.0-or-later
// The recorder (docs/recorder.md): the files are the stream, aligned, through dropouts and
// latency changes, in the WAV layout the doc gives.
#include "RecordTestUtils.h"
#include "capture/CaptureTestUtils.h"
#include "sedef/dsp/capture/CaptureStage.h"
#include "sedef/record/WavFloatWriter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cstring>
#include <string>

using namespace sedef;
using namespace sedef::record;
using namespace sedef::test::rec;
namespace nt = sedef::test::nam;

namespace
{
using Phase = TakeRecorder::Phase;

/// Runs the rig over all of its buffers in blocks from `blocks`; the take starts at the first
/// block boundary at or after `startAt` and is stopped at the first one at or after `stopAt`.
/// Returns those boundaries. `everyBlock` runs after each block (with its start and size).
template <typename EveryBlock>
std::pair<int, int> runTake (DeckRig& rig, Writer& writer, Blocks blocks, const ParamSnapshot& p,
                             int startAt, int stopAt, EveryBlock&& everyBlock)
{
    int t0 = -1, t1 = -1;
    const int total = rig.in.frames();
    for (int at = 0; at < total;)
    {
        if (t0 < 0 && at >= startAt)
        {
            REQUIRE (writer.start (rig.takeConfig()));
            t0 = at;
        }
        if (t0 >= 0 && t1 < 0 && at >= stopAt)
        {
            rig.deck.recorder().requestStop();
            t1 = at;
        }
        const int n = std::min (blocks(), total - at);
        rig.process (at, n, p);
        everyBlock (at, n);
        at += n;
    }
    return { t0, t1 };
}

std::pair<int, int> runTake (DeckRig& rig, Writer& writer, Blocks blocks, const ParamSnapshot& p,
                             int startAt, int stopAt)
{
    return runTake (rig, writer, std::move (blocks), p, startAt, stopAt,
                    [&writer] (int, int) { writer.drain(); });
}

ParamSnapshot transposed()
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::transpose] = -2.0f; // Transpose's share on top of the oversampling filters
    return p;
}
} // namespace

TEST_CASE ("Recorder: the DI and output files are the processed stream, sample-exact", "[record]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const auto pattern = GENERATE (from_range (test::rec::blockPatterns()));
    const auto [ins, outs] = GENERATE (std::pair { 1, 2 }, std::pair { 2, 2 }, std::pair { 1, 1 });
    CAPTURE (fs, pattern, ins, outs);

    const int frames = static_cast<int> (fs * 0.5);
    DeckRig rig (fs, ins, outs, frames, OversamplingMode::linearPhase);
    for (int c = 0; c < ins; ++c)
        rig.in[c] = plucks (frames, fs, static_cast<std::uint32_t> (c + 3));
    const auto input = rig.in; // what the device delivered (nothing re-amps here)

    Writer writer (rig.deck.recorder());
    const auto [t0, t1] = runTake (rig, writer, Blocks { pattern }, transposed(),
                                   static_cast<int> (fs * 0.05), static_cast<int> (fs * 0.3));
    writer.finish();

    const int latency = rig.engine.latencySamples();
    REQUIRE (latency > 300); // linear phase plus Transpose: the shift is real
    const auto& recorder = rig.deck.recorder();
    CHECK (recorder.startLatency() == latency);
    CHECK_FALSE (recorder.latencyChanged());
    CHECK (recorder.droppedBlocks() == 0);
    CHECK (recorder.endReason() == TakeRecorder::EndReason::stopped);

    const int length = t1 - t0;
    REQUIRE (recorder.frames() == length);
    REQUIRE (writer.diFile.channels == ins);
    REQUIRE (writer.outFile.channels == outs);
    for (const auto* f : { &writer.diFile, &writer.outFile })
    {
        CHECK (f->formatTag == 3);
        CHECK (f->bitsPerSample == 32);
        CHECK (f->sampleRate == static_cast<std::uint32_t> (fs));
        CHECK (f->factFrames == static_cast<std::uint32_t> (length));
        REQUIRE (f->samples[0].size() == static_cast<std::size_t> (length));
    }

    // DI: the input from the take's first sample. Output: the output stream, `latency` later.
    for (int c = 0; c < ins; ++c)
        CHECK (std::memcmp (writer.diFile.samples[static_cast<std::size_t> (c)].data(),
                            input.data[static_cast<std::size_t> (c)].data() + t0,
                            static_cast<std::size_t> (length) * sizeof (float)) == 0);
    for (int c = 0; c < outs; ++c)
        CHECK (std::memcmp (writer.outFile.samples[static_cast<std::size_t> (c)].data(),
                            rig.out.data[static_cast<std::size_t> (c)].data() + t0 + latency,
                            static_cast<std::size_t> (length) * sizeof (float)) == 0);
}

TEST_CASE ("Recorder: an impulse in the DI is at the same sample in the output file", "[record]")
{
    // The plain rig with an identity capture is a pure delay of the reported latency
    // (LatencyTests), so the output file must have the impulse exactly where the DI has it.
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const auto mode = GENERATE (OversamplingMode::lowLatency, OversamplingMode::linearPhase);
    CAPTURE (fs, static_cast<int> (mode));

    const int frames = static_cast<int> (fs * 0.4);
    DeckRig rig (fs, 1, 2, frames, mode, 256);
    const auto capture = nt::parseOrFail (nt::linear ({ 1.0f }, fs));
    for (int c = 0; c < rig.engine.numCabs(); ++c)
        rig.engine.capture (c).setModel (capture->instantiate (rig.engine.capture (c).modelSpec()));
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::eqLagoonEnabled] = 0.0f;
    p[ParamId::eqReefEnabled] = 0.0f;
    p[ParamId::eqAbyssEnabled] = 0.0f;
    p[ParamId::ampSource] = 1.0f;
    p[ParamId::captureCab] = 0.0f;

    const int startAt = static_cast<int> (fs * 0.2); // past the switch to the capture
    const int impulseIn = 1234;                      // into the take
    Writer writer (rig.deck.recorder());
    const int t0 = (startAt + 255) / 256 * 256;
    rig.in[0][static_cast<std::size_t> (t0 + impulseIn)] = 0.25f;
    runTake (rig, writer, Blocks { { 256 } }, p, startAt, static_cast<int> (fs * 0.35));
    writer.finish();

    const auto& di = writer.diFile.samples[0];
    const auto& out = writer.outFile.samples[0];
    REQUIRE (di.size() == out.size());
    const auto peakAt = [] (const std::vector<float>& v) {
        return std::max_element (v.begin(), v.end(),
                                 [] (float a, float b) { return std::abs (a) < std::abs (b); }) -
               v.begin();
    };
    CHECK (peakAt (di) == impulseIn);
    CHECK (peakAt (out) == impulseIn);
    CHECK (std::abs (out[static_cast<std::size_t> (impulseIn)]) > 0.01f);
}

TEST_CASE ("Recorder: a latency change during a take keeps the output aligned", "[record]")
{
    // Transpose switched on mid-take adds its latency: from then on each block's output goes to
    // the position its own latency gives, later writes replacing the moment they overlap.
    const double fs = 48000.0;
    const auto pattern = GENERATE (from_range (test::rec::blockPatterns()));
    CAPTURE (pattern);
    const int frames = 48000;
    DeckRig rig (fs, 1, 2, frames);
    rig.in[0] = plucks (frames, fs, 11);

    auto p = ParamSnapshot::defaults();
    Writer writer (rig.deck.recorder());
    std::vector<std::pair<int, int>> blockLatency; // (block start, latency after it), stream time
    const int switchAt = 20000;
    Blocks blocks { pattern };
    int t0 = -1, t1 = -1;
    for (int at = 0; at < frames;)
    {
        if (t0 < 0 && at >= 2000)
        {
            REQUIRE (writer.start (rig.takeConfig()));
            t0 = at;
        }
        if (t0 >= 0 && t1 < 0 && at >= 40000)
        {
            rig.deck.recorder().requestStop();
            t1 = at;
        }
        if (at >= switchAt)
            p[ParamId::transpose] = 3.0f;
        const int n = std::min (blocks(), frames - at);
        rig.process (at, n, p);
        for (int i = 0; i < n; ++i)
            blockLatency.emplace_back (at + i, rig.engine.latencySamples());
        writer.drain();
        at += n;
    }
    writer.finish();
    CHECK (rig.deck.recorder().latencyChanged());
    CHECK (rig.deck.recorder().startLatency() == 41);
    CHECK (rig.engine.latencySamples() > 41);
    CHECK (TakeFiles::comment (TakeRecorder::kOutput, rig.deck.recorder(), {})
               .find ("latency changed") != std::string::npos);

    // The rule, applied by hand: stream sample t goes to file position t - L(t) - t0, while it
    // is inside the take; later samples win.
    const int length = t1 - t0;
    std::vector<float> expected (static_cast<std::size_t> (length), 0.0f);
    for (const auto& [t, l] : blockLatency)
    {
        const int pos = t - l - t0;
        if (t >= t0 && pos >= 0 && pos < length)
            expected[static_cast<std::size_t> (pos)] = rig.out[0][static_cast<std::size_t> (t)];
    }
    REQUIRE (writer.outFile.samples[0].size() == expected.size());
    CHECK (writer.outFile.samples[0] == expected);
    CHECK (writer.diFile.samples[0].size() == expected.size());
}

TEST_CASE ("Recorder: a stalled writer drops whole stretches, counted, as silence in place",
           "[record]")
{
    // A FIFO for 0.1 s: five chunks of 2048 frames per file (four for the stall, one filling, one
    // being written: ceil(0.1 * 48000 / 2048) + 2 = 5). The writer is not called until `resume`,
    // so the free chunks run out at frame 5 * 2048 of each file and everything up to the resume
    // is dropped, never waited for.
    const double fs = 48000.0;
    const int frames = 40000;
    const int block = 256;
    const int resume = 20480; // a block boundary
    DeckRig rig (fs, 1, 2, frames, OversamplingMode::lowLatency, block);
    rig.in[0] = plucks (frames, fs, 5);
    const auto input = rig.in;

    auto config = rig.takeConfig();
    config.stallSeconds = 0.1;
    REQUIRE (TakeRecorder::chunksPerStream (fs, 0.1) == 5);
    Writer writer (rig.deck.recorder());
    REQUIRE (writer.files == nullptr);
    writer.di = MemorySink();
    writer.out = MemorySink();
    writer.files = std::make_unique<TakeFiles> (writer.di, writer.out, config);
    REQUIRE (writer.files->begin());
    REQUIRE (rig.deck.recorder().start (config));

    const auto p = ParamSnapshot::defaults();
    for (int at = 0; at < frames; at += block)
    {
        if (at == 32768)
            rig.deck.recorder().requestStop();
        rig.process (at, block, p);
        if (at + block >= resume) // the stalled disk comes back
            writer.drain();
    }
    REQUIRE (rig.deck.recorder().phase() == Phase::finished);
    writer.finish();

    const auto& recorder = rig.deck.recorder();
    const int latency = rig.engine.latencySamples();
    const int full = 5 * TakeRecorder::kChunkFrames; // 10240
    CHECK (recorder.droppedFrames (TakeRecorder::kDi) == resume - full);
    CHECK (recorder.droppedFrames (TakeRecorder::kOutput) == resume - latency - full);
    CHECK (recorder.droppedBlocks() == (resume - full) / block);

    // Both files keep the take's length; the dropped stretch is silence, the rest exact.
    const int length = 32768;
    REQUIRE (writer.diFile.samples[0].size() == static_cast<std::size_t> (length));
    REQUIRE (writer.outFile.samples[0].size() == static_cast<std::size_t> (length));
    for (int k = 0; k < length; ++k)
    {
        const bool diGone = k >= full && k < resume;
        const bool outGone = k >= full && k < resume - latency;
        const float di = diGone ? 0.0f : input.data[0][static_cast<std::size_t> (k)];
        const float out = outGone ? 0.0f : rig.out[0][static_cast<std::size_t> (k + latency)];
        if (writer.diFile.samples[0][static_cast<std::size_t> (k)] != di ||
            writer.outFile.samples[0][static_cast<std::size_t> (k)] != out)
        {
            FAIL ("frame " << k << " differs");
        }
    }

    const auto comment = writer.diFile.infoText ("ICMT");
    CHECK (comment.find ("Dropouts: " + std::to_string ((resume - full) / block) + " blocks, " +
                         std::to_string (resume - full) +
                         " samples of this file written as "
                         "silence.") != std::string::npos);
    CHECK (writer.outFile.infoText ("ICMT").find (std::to_string (resume - latency - full) +
                                                  " samples") != std::string::npos);
}

TEST_CASE ("Recorder: stops at the file size limit, before any file passes it", "[record]")
{
    DeckRig rig (48000.0, 1, 2, 30000, OversamplingMode::lowLatency, 300);
    rig.in[0] = plucks (30000, 48000.0, 2);
    Writer writer (rig.deck.recorder());
    auto config = rig.takeConfig();
    config.maxFrames = 10000;
    writer.di = MemorySink();
    writer.out = MemorySink();
    writer.files = std::make_unique<TakeFiles> (writer.di, writer.out, config);
    REQUIRE (writer.files->begin());
    REQUIRE (rig.deck.recorder().start (config));
    for (int at = 0; at < 30000; at += 300)
    {
        rig.process (at, 300, ParamSnapshot::defaults());
        writer.drain();
    }
    writer.finish();
    CHECK (rig.deck.recorder().endReason() == TakeRecorder::EndReason::sizeLimit);
    CHECK (rig.deck.recorder().frames() == 9900); // the last whole block that fits
    CHECK (writer.outFile.samples[0].size() == 9900);
    CHECK (writer.diFile.infoText ("ICMT").find ("WAV size limit") != std::string::npos);
    // The real limit: 4 GB of RIFF minus the header and the room kept for the metadata.
    CHECK (WavFloatWriter::maxFrames (1) == (0xffffffffLL - 58 - 4096) / 4);
    CHECK (WavFloatWriter::maxFrames (2) == (0xffffffffLL - 58 - 4096) / 8);
    CHECK (TakeFiles::maxFrames (rig.takeConfig()) == WavFloatWriter::maxFrames (2));
}

TEST_CASE ("Recorder: when the audio stops, the take ends where it is, files complete", "[record]")
{
    DeckRig rig (48000.0, 1, 2, 9600, OversamplingMode::lowLatency, 480);
    rig.in[0] = plucks (9600, 48000.0, 4);
    Writer writer (rig.deck.recorder());
    REQUIRE (writer.start (rig.takeConfig()));
    for (int at = 0; at < 4800; at += 480)
        rig.process (at, 480, ParamSnapshot::defaults());
    // The device stops: no more blocks. prepareToPlay() or releaseResources() ends the take.
    rig.deck.recorder().finishWithoutAudio();
    writer.finish();
    CHECK (rig.deck.recorder().endReason() == TakeRecorder::EndReason::audioStopped);
    CHECK (writer.diFile.samples[0].size() == 4800);
    // The output had not caught up by the latency: that tail is silence, same length.
    const int latency = rig.engine.latencySamples();
    REQUIRE (writer.outFile.samples[0].size() == 4800);
    CHECK (writer.outFile.samples[0][4800 - latency - 1] ==
           rig.out[0][static_cast<std::size_t> (4800 - 1)]);
    for (int k = 4800 - latency; k < 4800; ++k)
        CHECK (writer.outFile.samples[0][static_cast<std::size_t> (k)] == 0.0f);
    CHECK (writer.outFile.infoText ("ICMT").find ("audio device stopped") != std::string::npos);
}

TEST_CASE ("Recorder: stopped before it started, the files are empty and valid", "[record]")
{
    DeckRig rig (48000.0, 1, 2, 1024, OversamplingMode::lowLatency, 256);
    Writer writer (rig.deck.recorder());
    REQUIRE (writer.start (rig.takeConfig()));
    CHECK_FALSE (rig.deck.recorder().start (rig.takeConfig())); // one take at a time
    rig.deck.recorder().requestStop();
    rig.process (0, 256, ParamSnapshot::defaults());
    writer.finish();
    CHECK (writer.diFile.samples[0].empty());
    CHECK (writer.outFile.samples[0].empty());
    CHECK (writer.diFile.factFrames == 0);
    CHECK (rig.deck.recorder().phase() == Phase::idle);
    // And the next take works.
    REQUIRE (writer.start (rig.takeConfig()));
    rig.process (256, 256, ParamSnapshot::defaults());
    CHECK (rig.deck.recorder().frames() == 256);
}

TEST_CASE ("Recorder: idle, the deck is the engine, byte for byte", "[record]")
{
    // Nothing recording, nothing re-amped: the deck must not change a bit of the rig's output.
    const auto pattern = GENERATE (from_range (test::rec::blockPatterns()));
    const int frames = 24000;
    DeckRig rig (48000.0, 2, 2, frames);
    Engine direct;
    direct.prepare (48000.0, 4096, 2, 2);
    test::Buffer out (2, frames);
    rig.in[0] = plucks (frames, 48000.0, 8);
    rig.in[1] = plucks (frames, 48000.0, 9);
    const auto input = rig.in;

    auto p = ParamSnapshot::defaults();
    p[ParamId::ampModel] = 2.0f;
    p[ParamId::pearlEnabled] = 1.0f;
    p[ParamId::doublerEnabled] = 1.0f;
    p[ParamId::driftEnabled] = 1.0f;
    p[ParamId::fathomEnabled] = 1.0f;
    p[ParamId::transpose] = 5.0f;

    Blocks blocks { pattern };
    Transport transport;
    for (int at = 0; at < frames;)
    {
        const int n = std::min (blocks(), frames - at);
        rig.process (at, n, p);
        std::array<const float*, 2> ip { input.data[0].data() + at, input.data[1].data() + at };
        std::array<float*, 2> op { out.ptrs[0] + at, out.ptrs[1] + at };
        direct.process (ip.data(), op.data(), n, p, transport);
        at += n;
    }
    CHECK (rig.in.data == input.data); // the input untouched
    CHECK (std::memcmp (rig.out.data[0].data(), out.data[0].data(), frames * sizeof (float)) == 0);
    CHECK (std::memcmp (rig.out.data[1].data(), out.data[1].data(), frames * sizeof (float)) == 0);
}

// -------------------------------------------------------------------------------------------------
// The WAV writer

TEST_CASE ("WavFloatWriter: the header, positioned writes, silence in gaps, metadata", "[record]")
{
    MemorySink sink;
    WavFloatWriter w (sink, 2, 44100.0);
    REQUIRE (w.begin());
    REQUIRE (sink.bytes.size() == WavFloatWriter::kHeaderBytes);

    // The exact header of an empty stereo file.
    const unsigned char expected[] = {
        'R', 'I', 'F', 'F', 50,  0, 0,   0,    'W',  'A', 'V', 'E',  'f',  'm',  't', ' ', 18,
        0,   0,   0,   3,   0,   2, 0,   0x44, 0xac, 0,   0,   0x20, 0x62, 0x05, 0, // 44100, 352800
        8,   0,   32,  0,   0,   0, 'f', 'a',  'c',  't', 4,   0,    0,    0,    0,   0,   0,
        0,   'd', 'a', 't', 'a', 0, 0,   0,    0,
    };
    REQUIRE (sizeof (expected) == WavFloatWriter::kHeaderBytes);
    CHECK (std::memcmp (sink.bytes.data(), expected, sizeof (expected)) == 0);

    const float a[] = { 0.5f, -0.5f, 0.25f, -0.25f, 1.5f, -1.5f }; // three frames, over full scale
    REQUIRE (w.write (0, a, 3));
    const float b[] = { 0.125f, -0.125f };
    REQUIRE (w.write (5, b, 1)); // frames 3 and 4 become silence
    REQUIRE (w.write (1, b, 1)); // and frame 1 is replaced
    CHECK (w.frames() == 6);

    // While recording, the sizes are kept current: the file already opens.
    REQUIRE (w.updateHeader());
    WavFloatFile f;
    REQUIRE (WavFloatFile::parse (sink.bytes, f));
    CHECK (f.factFrames == 6);
    CHECK (f.samples[0] == std::vector<float> { 0.5f, 0.125f, 1.5f, 0.0f, 0.0f, 0.125f });
    CHECK (f.samples[1] == std::vector<float> { -0.5f, -0.125f, -1.5f, 0.0f, 0.0f, -0.125f });

    InfoField name { { 'I', 'N', 'A', 'M' }, "odd" };  // odd length with its zero: no pad
    InfoField note { { 'I', 'C', 'M', 'T' }, "even" }; // even text, odd size: padded
    REQUIRE (w.finish ({ name, note }));
    CHECK_FALSE (w.write (0, a, 1)); // finished
    REQUIRE (WavFloatFile::parse (sink.bytes, f));
    CHECK (f.infoText ("INAM") == "odd");
    CHECK (f.infoText ("ICMT") == "even");
    CHECK (f.riffSize == sink.bytes.size() - 8);
    CHECK (sink.bytes.size() % 2 == 0);
    CHECK (f.channels == 2);
    CHECK (f.sampleRate == 44100);
}

TEST_CASE ("WavFloatWriter: a full disk is an error, not a crash", "[record]")
{
    MemorySink sink;
    sink.limit = 200;
    WavFloatWriter w (sink, 1, 48000.0);
    REQUIRE (w.begin());
    std::vector<float> x (100, 0.1f);
    CHECK (w.write (0, x.data(), 30));        // 58 + 120 bytes fit
    CHECK_FALSE (w.write (30, x.data(), 30)); // no room
    CHECK_FALSE (w.write (-1, x.data(), 1));
    CHECK_FALSE (w.extendTo (WavFloatWriter::maxFrames (1) + 1));
}
