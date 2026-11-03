// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Internal: the ConvNet architecture without allocations in process().

#include <conv1d.h>
#include <dsp.h> // NeuralAmpModelerCore
#include <memory>
#include <vector>

namespace sedef::dsp::capture
{

/// NeuralAmpModelerCore's ConvNet (convnet.cpp) builds Eigen matrices on every process() call,
/// which allocates on the audio thread. This is the same network with the same weight layout,
/// assembled from the core's own real-time safe Conv1D: blocks of a kernel-2 dilated
/// convolution, an optional batch norm folded to a per-channel scale and offset, and an
/// activation; then a linear head. Tests check it against the core's ConvNet sample for sample.
class RtConvNet final : public nam::DSP
{
public:
    /// `config` and `weights` must have passed capture::validate (one input, one output).
    RtConvNet (const nlohmann::json& config, std::vector<float> weights, double sampleRate);

    void process (NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override;
    int GetPrewarmSamples() override { return prewarmSamples_; }

protected:
    void SetMaxBufferSize (int maxBufferSize) override;

private:
    struct Block
    {
        nam::Conv1D conv;
        bool batchnorm = false;
        Eigen::VectorXf scale, offset;
        nam::activations::Activation::Ptr activation;
    };

    std::vector<Block> blocks_;
    Eigen::MatrixXf input_;
    Eigen::MatrixXf headWeight_; // 1 x channels
    float headBias_ = 0.0f;
    int prewarmSamples_ = 1;
};

} // namespace sedef::dsp::capture
