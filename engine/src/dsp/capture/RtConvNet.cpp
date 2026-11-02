// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RtConvNet.h"

#include <cmath>

namespace sedef::dsp::capture
{

RtConvNet::RtConvNet (const nlohmann::json& config, std::vector<float> weights, double sampleRate)
    : nam::DSP (1, 1, sampleRate)
{
    const int channels = config["channels"].get<int>();
    const auto dilations = config["dilations"].get<std::vector<int>>();
    const bool batchnorm = config["batchnorm"].get<bool>();
    const auto activation = nam::activations::ActivationConfig::from_json (config["activation"]);
    const int groups = config.value ("groups", 1);

    // Weight order as in nam::convnet::ConvNet: per block the convolution (bias only without
    // batch norm), then the batch norm's running mean, running variance, weight, bias and eps;
    // then the head's weights and bias.
    auto it = weights.begin();
    blocks_.resize (dilations.size());
    for (std::size_t b = 0; b < dilations.size(); ++b)
    {
        auto& block = blocks_[b];
        block.conv.set_size_and_weights_ (b == 0 ? 1 : channels, channels, 2, dilations[b],
                                          !batchnorm, groups, it);
        block.batchnorm = batchnorm;
        if (batchnorm)
        {
            Eigen::VectorXf mean (channels), var (channels), w (channels), bias (channels);
            for (int i = 0; i < channels; ++i)
                mean (i) = *(it++);
            for (int i = 0; i < channels; ++i)
                var (i) = *(it++);
            for (int i = 0; i < channels; ++i)
                w (i) = *(it++);
            for (int i = 0; i < channels; ++i)
                bias (i) = *(it++);
            const float eps = *(it++);
            block.scale.resize (channels);
            block.offset.resize (channels);
            for (int i = 0; i < channels; ++i)
            {
                block.scale (i) = w (i) / std::sqrt (eps + var (i));
                block.offset (i) = bias (i) - block.scale (i) * mean (i);
            }
        }
        block.activation = nam::activations::Activation::get_activation (activation);
        prewarmSamples_ += dilations[b];
    }
    headWeight_.resize (1, channels);
    for (int i = 0; i < channels; ++i)
        headWeight_ (0, i) = *(it++);
    headBias_ = *(it++);
}

void RtConvNet::SetMaxBufferSize (int maxBufferSize)
{
    nam::DSP::SetMaxBufferSize (maxBufferSize);
    input_.resize (1, maxBufferSize);
    input_.setZero();
    for (auto& block : blocks_)
        block.conv.SetMaxBufferSize (maxBufferSize);
}

void RtConvNet::process (NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames)
{
    for (int i = 0; i < numFrames; ++i)
        input_ (0, i) = input[0][i];

    const Eigen::MatrixXf* x = &input_;
    for (auto& block : blocks_)
    {
        block.conv.Process (*x, numFrames);
        Eigen::MatrixXf& y = block.conv.GetOutput();
        if (block.batchnorm)
        {
            const auto rows = y.rows();
            for (int f = 0; f < numFrames; ++f)
                for (Eigen::Index c = 0; c < rows; ++c)
                    y (c, f) = y (c, f) * block.scale (c) + block.offset (c);
        }
        block.activation->apply (y.leftCols (numFrames));
        x = &y;
    }

    const auto rows = x->rows();
    for (int f = 0; f < numFrames; ++f)
    {
        float acc = headBias_;
        for (Eigen::Index c = 0; c < rows; ++c)
            acc += headWeight_ (0, c) * (*x) (c, f);
        output[0][f] = acc;
    }
}

} // namespace sedef::dsp::capture
