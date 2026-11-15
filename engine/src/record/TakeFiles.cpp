// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/TakeFiles.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace sedef::record
{

namespace
{
InfoField field (const char* id, std::string text)
{
    InfoField f;
    std::copy (id, id + 4, f.id.begin());
    f.text = std::move (text);
    return f;
}

std::string plural (std::int64_t n, const char* one, const char* many)
{
    return std::to_string (n) + " " + (n == 1 ? one : many);
}
} // namespace

TakeFiles::TakeFiles (ByteSink& di, ByteSink& output, const TakeRecorder::Config& config)
    : di_ (di, config.diChannels, config.sampleRate),
      output_ (output, config.outChannels, config.sampleRate),
      interleaved_ (static_cast<std::size_t> (TakeRecorder::kChunkFrames) *
                    TakeRecorder::kMaxChannels)
{
}

std::int64_t TakeFiles::maxFrames (const TakeRecorder::Config& config) noexcept
{
    return std::min (WavFloatWriter::maxFrames (config.diChannels),
                     WavFloatWriter::maxFrames (config.outChannels));
}

bool TakeFiles::begin()
{
    return di_.begin() && output_.begin();
}

bool TakeFiles::write (const TakeChunk& chunk)
{
    auto& file = chunk.stream == TakeRecorder::kDi ? di_ : output_;
    const int channels = file.channels();
    for (int i = 0; i < chunk.frames; ++i)
        for (int c = 0; c < channels; ++c)
            interleaved_[static_cast<std::size_t> (i * channels + c)] =
                c < chunk.channels
                    ? chunk.data[static_cast<std::size_t> (c) * TakeRecorder::kChunkFrames +
                                 static_cast<std::size_t> (i)]
                    : 0.0f;
    return file.write (chunk.position, interleaved_.data(), chunk.frames);
}

bool TakeFiles::updateHeaders()
{
    const bool a = di_.updateHeader();
    const bool b = output_.updateHeader();
    return a && b;
}

std::string TakeFiles::comment (std::uint8_t stream, const TakeRecorder& recorder,
                                const TakeInfo& info)
{
    const auto rate = std::to_string (std::llround (recorder.config().sampleRate)) + " Hz";
    std::string s;
    if (stream == TakeRecorder::kDi)
    {
        s = "Sedef Amp take, DI: the dry input before anything, " + rate + ".";
        if (!info.outputName.empty())
            s += " Its processed output is \"" + info.outputName + "\", sample for sample.";
    }
    else
    {
        s = "Sedef Amp take, output: the processed signal after everything, " + rate + ".";
        s += " Shifted " + plural (recorder.startLatency(), "sample", "samples") +
             " earlier (the rig's latency) so it lines up sample for sample with \"" + info.diName +
             "\".";
        if (recorder.latencyChanged())
            s += " The latency changed during the take (Transpose or the Latency setting);"
                 " each part is shifted by its own.";
    }

    const auto dropped = recorder.droppedFrames (stream);
    if (recorder.droppedBlocks() == 0)
        s += " Dropouts: none.";
    else if (dropped == 0)
        s += " Dropouts: none in this file (" +
             plural (recorder.droppedBlocks(), "block", "blocks") + " in its partner).";
    else
        s += " Dropouts: " + plural (recorder.droppedBlocks(), "block", "blocks") + ", " +
             plural (dropped, "sample", "samples") + " of this file written as silence.";

    switch (recorder.endReason())
    {
    case TakeRecorder::EndReason::sizeLimit:
        s += " The take stopped at the WAV size limit.";
        break;
    case TakeRecorder::EndReason::audioStopped:
        s += " The take ended when the audio device stopped.";
        break;
    default:
        break;
    }
    return s;
}

bool TakeFiles::finish (const TakeRecorder& recorder, const TakeInfo& info)
{
    const std::int64_t frames = recorder.frames();
    bool ok = di_.extendTo (frames);
    ok = output_.extendTo (frames) && ok;

    const auto infoFor = [&] (std::uint8_t stream) {
        return std::vector<InfoField> {
            field ("INAM", stream == TakeRecorder::kDi ? info.diName : info.outputName),
            field ("ISFT", info.software),
            field ("ICRD", info.date),
            field ("ICMT", comment (stream, recorder, info)),
        };
    };
    ok = di_.finish (infoFor (TakeRecorder::kDi)) && ok;
    ok = output_.finish (infoFor (TakeRecorder::kOutput)) && ok;
    return ok;
}

} // namespace sedef::record
