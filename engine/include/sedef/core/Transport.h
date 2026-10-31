// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace sedef
{

/// Host transport information for one block.
struct Transport
{
    bool hasHostTempo = false;
    double bpm = 120.0;
    bool isPlaying = false;
    double ppqPosition = 0.0; ///< quarter notes since song start (valid when isPlaying)
};

} // namespace sedef
