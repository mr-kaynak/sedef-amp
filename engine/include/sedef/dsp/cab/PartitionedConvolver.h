// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/dsp/cab/ConvolutionKernel.h"

struct PFFFT_Setup;

namespace sedef::dsp::cab
{

/// Zero-latency convolution state for one voice (Gardner-style split):
///
///   y[n] = sum_{k<P} h[k] x[n-k]            direct form, sample by sample
///        + sum_{k>=P} h[k] x[n-k]           uniformly partitioned overlap-save FFT
///
/// The tail starts P taps in, so its block of P outputs depends only on input blocks that are
/// already complete when that block starts: no latency for any host block size. The FFT work runs
/// once per P input samples. Input spectra are kernel-independent; the kernel is passed per call.
class PartitionedConvolver
{
public:
    PartitionedConvolver() = default;
    PartitionedConvolver (const PartitionedConvolver&) = delete;
    PartitionedConvolver& operator= (const PartitionedConvolver&) = delete;

    /// Non-real-time. `setup` is a real pffft setup of size 2 * partitionSize, owned by the caller.
    void prepare (PFFFT_Setup* setup, int partitionSize, int maxTailPartitions);
    void reset() noexcept;

    /// True if `kernel` fits this convolver (same partition size, tail within capacity).
    bool accepts (const ConvolutionKernel& kernel) const noexcept
    {
        return !kernel.empty() && kernel.partitionSize() == partition_ &&
               kernel.numTailPartitions() <= capacity_;
    }

    /// Convolves n samples. `in` and `out` may alias. Requires accepts (kernel).
    void process (const ConvolutionKernel& kernel, const float* in, float* out, int n) noexcept;

private:
    void finishBlock (const ConvolutionKernel& kernel) noexcept;

    PFFFT_Setup* setup_ = nullptr;
    int partition_ = 0;
    int capacity_ = 0; // tail partitions the spectrum history can hold

    AlignedFloats input_;    // [previous block | current block], 2P
    AlignedFloats headAcc_;  // direct-form output accumulator, 2P
    AlignedFloats history_;  // ring of input spectra, capacity * 2P
    AlignedFloats spectrum_; // tail spectrum accumulator, 2P
    AlignedFloats time_;     // inverse FFT output, 2P
    AlignedFloats work_;     // pffft scratch, 2P
    AlignedFloats tailOut_;  // tail output for the current block, P

    int pos_ = 0;    // samples written into the current block
    int newest_ = 0; // history slot of the newest spectrum
    int valid_ = 0;  // spectra written since reset (lazily avoids clearing the history)
};

} // namespace sedef::dsp::cab
