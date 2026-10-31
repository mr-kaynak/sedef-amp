// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/cab/PartitionedConvolver.h"

#include <algorithm>
#include <cstring>
#include <pffft/pffft.h>

namespace sedef::dsp::cab
{

void PartitionedConvolver::prepare (PFFFT_Setup* setup, int partitionSize, int maxTailPartitions)
{
    setup_ = setup;
    partition_ = partitionSize;
    capacity_ = std::max (1, maxTailPartitions);
    const auto p = static_cast<size_t> (partitionSize);
    input_.allocate (2 * p);
    headAcc_.allocate (2 * p);
    history_.allocate (static_cast<size_t> (capacity_) * 2 * p);
    spectrum_.allocate (2 * p);
    time_.allocate (2 * p);
    work_.allocate (2 * p);
    tailOut_.allocate (p);
    reset();
}

void PartitionedConvolver::reset() noexcept
{
    input_.clear();
    headAcc_.clear();
    tailOut_.clear();
    pos_ = 0;
    newest_ = 0;
    valid_ = 0;
}

void PartitionedConvolver::process (const ConvolutionKernel& kernel, const float* in, float* out,
                                    int n) noexcept
{
    const int p = partition_;
    const float* h = kernel.head();
    while (n > 0)
    {
        const int len = std::min (n, p - pos_);
        float* current = input_.data() + p;
        const float* tail = tailOut_.data();
        for (int i = 0; i < len; ++i)
        {
            const float x = in[i];
            const int q = pos_ + i;
            current[q] = x;
            // Scatter form (acc[q + k] += x h[k]): vectorises without reassociating a reduction.
            float* acc = headAcc_.data() + q;
            for (int k = 0; k < p; ++k)
                acc[k] += x * h[k];
            out[i] = acc[0] + tail[q];
        }
        pos_ += len;
        in += len;
        out += len;
        n -= len;
        if (pos_ == p)
            finishBlock (kernel);
    }
}

void PartitionedConvolver::finishBlock (const ConvolutionKernel& kernel) noexcept
{
    const int p = partition_;
    const auto twoP = static_cast<size_t> (2 * p);
    const int numTail = kernel.numTailPartitions();

    if (numTail > 0)
    {
        newest_ = newest_ + 1 < capacity_ ? newest_ + 1 : 0;
        float* newest = history_.data() + static_cast<size_t> (newest_) * twoP;
        pffft_transform (setup_, input_.data(), newest, work_.data(), PFFFT_FORWARD);
        valid_ = std::min (valid_ + 1, capacity_);

        // Tail output for the next block: sum_j X[b - j] * T[j].
        spectrum_.clear();
        const int count = std::min (numTail, valid_);
        int slot = newest_;
        for (int j = 0; j < count; ++j)
        {
            pffft_zconvolve_accumulate (setup_, history_.data() + static_cast<size_t> (slot) * twoP,
                                        kernel.tailSpectrum (j), spectrum_.data(), 1.0f);
            slot = slot > 0 ? slot - 1 : capacity_ - 1;
        }
        pffft_transform (setup_, spectrum_.data(), time_.data(), work_.data(), PFFFT_BACKWARD);
        std::memcpy (tailOut_.data(), time_.data() + p, sizeof (float) * static_cast<size_t> (p));
    }

    // Slide the input window and the direct-form accumulator by one block.
    std::memcpy (input_.data(), input_.data() + p, sizeof (float) * static_cast<size_t> (p));
    std::memcpy (headAcc_.data(), headAcc_.data() + p, sizeof (float) * static_cast<size_t> (p));
    std::memset (headAcc_.data() + p, 0, sizeof (float) * static_cast<size_t> (p));
    pos_ = 0;
}

} // namespace sedef::dsp::cab
