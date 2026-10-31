// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AnalysisWorker.h"

#include <array>

namespace sedef::plugin
{

AnalysisWorker::AnalysisWorker (Engine& engine)
    : juce::Thread ("Sedef pitch analysis"),
      engine_ (engine)
{
}

AnalysisWorker::~AnalysisWorker()
{
    setActive (false);
}

void AnalysisWorker::setActive (bool active)
{
    if (active == active_)
        return;
    active_ = active;

    if (active)
    {
        drainRing(); // discard audio captured before the tuner opened
        tracker_.prepare (engine_.sampleRate());
        engine_.setAnalysisEnabled (true);
        startThread (juce::Thread::Priority::low);
    }
    else
    {
        engine_.setAnalysisEnabled (false);
        signalThreadShouldExit();
        notify(); // wake the idle wait so stopping doesn't stall the message thread
        stopThread (1000);
        const std::scoped_lock lock (readingMutex_);
        reading_ = {};
    }
}

void AnalysisWorker::engineWasPrepared()
{
    if (!active_)
        return;
    active_ = false; // the engine already stopped feeding the ring; restart at the new rate
    stopThread (1000);
    setActive (true);
}

analysis::PitchTracker::Reading AnalysisWorker::latest() const
{
    const std::scoped_lock lock (readingMutex_);
    return reading_;
}

void AnalysisWorker::drainRing()
{
    std::array<float, 4096> scratch {};
    while (engine_.analysisRing().pop (scratch.data(), scratch.size()) > 0)
    {
    }
}

void AnalysisWorker::run()
{
    std::array<float, 2048> block {};
    while (!threadShouldExit())
    {
        const auto n = engine_.analysisRing().pop (block.data(), block.size());
        if (n == 0)
        {
            wait (4);
            continue;
        }
        tracker_.setReferenceHz (referenceHz_.load());
        tracker_.push (block.data(), static_cast<int> (n));
        const auto r = tracker_.latest();
        const std::scoped_lock lock (readingMutex_);
        reading_ = r;
    }
}

} // namespace sedef::plugin
