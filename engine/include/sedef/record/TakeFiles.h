// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/record/TakeRecorder.h"
#include "sedef/record/WavFloatWriter.h"

#include <string>
#include <vector>

namespace sedef::record
{

/// What the files say about themselves (LIST/INFO).
struct TakeInfo
{
    std::string diName; ///< file names, so each file can name its partner
    std::string outputName;
    std::string date;     ///< ICRD: "2026-11-13"
    std::string software; ///< ISFT: "Sedef Amp 0.10.0"
};

/// The writer's half of a take: the DI and output files behind a TakeRecorder
/// (docs/recorder.md, sections 3 and 4). Not real time.
class TakeFiles
{
public:
    TakeFiles (ByteSink& di, ByteSink& output, const TakeRecorder::Config& config);

    bool begin();
    /// Interleaves a chunk into its file at its position.
    bool write (const TakeChunk& chunk);
    bool updateHeaders();
    /// Once the recorder has finished and been drained: brings both files to the take's length
    /// (silence where samples were dropped or the output did not catch up), adds the metadata
    /// and completes the headers.
    bool finish (const TakeRecorder& recorder, const TakeInfo& info);

    /// The ICMT text for one of the files.
    static std::string comment (std::uint8_t stream, const TakeRecorder& recorder,
                                const TakeInfo& info);
    /// The largest take both files can hold.
    static std::int64_t maxFrames (const TakeRecorder::Config& config) noexcept;

    const WavFloatWriter& file (std::uint8_t stream) const noexcept
    {
        return stream == TakeRecorder::kDi ? di_ : output_;
    }

private:
    WavFloatWriter di_, output_;
    std::vector<float> interleaved_;
};

} // namespace sedef::record
