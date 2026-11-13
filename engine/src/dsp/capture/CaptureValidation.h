// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Internal: validation of untrusted .nam files before NeuralAmpModelerCore sees them.

#include "sedef/dsp/capture/CaptureFile.h"

#include <dsp.h> // NeuralAmpModelerCore: nam::dspData, nlohmann::json
#include <string>
#include <string_view>

namespace sedef::dsp::capture
{

/// Resource limits. A standard WaveNet capture has 13,802 weights and as many multiply-adds per
/// sample (0.66 billion a second at 48 kHz, about 10 % of a core), a 4,093-sample receptive field
/// and about 3 MB of buffers; these are far above anything a real capture needs and far below
/// anything that could hurt the host.
struct Limits
{
    static constexpr std::size_t kMaxValues = 2'500'000; ///< numbers in the file
    static constexpr int kMaxDepth = 64;                 ///< JSON nesting
    static constexpr long kMaxWeights = 2'000'000;
    /// At the capture's own rate: a standard WaveNet runs up to 192 kHz (2.65 billion).
    static constexpr double kMaxMacsPerSecond = 3.0e9;
    /// What one instance runs before it plays (its receptive field once): a few tenths of a second.
    static constexpr double kMaxStartMacs = 2.0e9;
    /// A matrix product's inner and outer sizes: Eigen packs larger ones into heap buffers, on the
    /// audio thread (EIGEN_STACK_ALLOCATION_LIMIT is 128 KB; 64 x 512 floats fill it exactly).
    static constexpr int kMaxProductDepth = 64;
    static constexpr int kMaxProductRows = 512;
    static constexpr long kMaxReceptiveField = 1L << 17; ///< WaveNet / ConvNet
    static constexpr long kMaxLinearTaps = 1L << 18;     ///< Linear (an impulse response)
    static constexpr long kMaxBufferFloats = 8L << 20;   ///< 32 MB of convolution history
    static constexpr int kMaxChannels = 512;
    static constexpr int kMaxLayerArrays = 8;
    static constexpr int kMaxSubmodels = 16; ///< sizes in a SlimmableContainer
    static constexpr int kMaxLayers = 64;    ///< per array, and ConvNet blocks
    static constexpr int kMaxKernel = 64;
    static constexpr int kMaxDilation = 32768;
    static constexpr int kMaxLstmLayers = 16;
    static constexpr int kMaxHeadConvs = 16;
    static constexpr double kMaxAbsWeight = 1.0e6;
    static constexpr double kMinSampleRate = 16000.0;
    static constexpr double kMaxSampleRate = 192000.0;
    /// Model-rate samples per process() call (the core's maximum buffer size).
    static constexpr int kModelBlock = 512;
};

struct Validated
{
    nam::dspData data; ///< cleaned: what the core builds from
    CaptureInfo info;
};

/// Checks a .nam file completely. On success `out` holds a copy the core can build from without
/// ever reading outside its arrays: every key it reads exists with the right type, every count it
/// derives matches, numbers fit their types. On failure `error` says why, in a sentence.
bool validate (std::string_view text, Validated& out, std::string& error);

} // namespace sedef::dsp::capture
