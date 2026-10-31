// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/Engine.h"
#include "sedef/analysis/PitchTracker.h"

#include <juce_core/juce_core.h>

#include <mutex>

namespace sedef::plugin
{

/// Drains the engine's analysis ring on a background thread and keeps the latest pitch reading.
/// Runs only while the tuner or the intonation workbench is open.
class AnalysisWorker : private juce::Thread
{
public:
    explicit AnalysisWorker (Engine& engine);
    ~AnalysisWorker() override;

    /// Message thread. Safe to call repeatedly.
    void setActive (bool active);
    bool isActive() const noexcept { return active_; }
    /// Message thread; call after the engine was (re)prepared.
    void engineWasPrepared();
    void setReferenceHz (float hz) noexcept { referenceHz_.store (hz); }

    analysis::PitchTracker::Reading latest() const;

private:
    void run() override;
    void drainRing();

    Engine& engine_;
    analysis::PitchTracker tracker_;
    std::atomic<float> referenceHz_ { 440.0f };
    bool active_ = false;

    mutable std::mutex readingMutex_;
    analysis::PitchTracker::Reading reading_;
};

} // namespace sedef::plugin
