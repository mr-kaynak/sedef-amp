// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/amp/AmpSection.h"

#include "sedef/core/Smoothed.h"
#include "sedef/dsp/amp/AmpModels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sedef::dsp
{
namespace
{
constexpr double kModelFadeSeconds = 0.03;

float knob (float v, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : fallback;
}

AmpSection::Settings sanitise (AmpSection::Settings s) noexcept
{
    s.model = std::clamp (s.model, 0, 2);
    auto& l = s.lagoon;
    l.volume = knob (l.volume, 0.5f);
    l.bass = knob (l.bass, 0.5f);
    l.cut = knob (l.cut, 0.5f);
    l.master = knob (l.master, 0.5f);
    l.output = knob (l.output, 0.5f);
    auto& r = s.reef;
    r.voice = std::clamp (r.voice, 0, 2);
    for (float* k : { &r.gain, &r.bass, &r.mid, &r.treble, &r.presence, &r.master, &r.output })
        *k = knob (*k, 0.5f);
    auto& a = s.abyss;
    for (float* k : { &a.gain, &a.bass, &a.mid, &a.treble, &a.presence, &a.master, &a.output })
        *k = knob (*k, 0.5f);
    return s;
}
} // namespace

struct AmpSection::Impl
{
    amp::LagoonModel lagoon;
    amp::ReefModel reef;
    amp::AbyssModel abyss;

    int current = Reef;
    int previous = -1;   ///< model fading out, -1 when no fade is running
    int pending = -1;    ///< requested while a fade was running; started when it ends
    LinearSmoothed fade; ///< weight of `current`, 0 -> 1
    std::array<std::vector<float>, kMaxChannels> scratch;
    bool prepared = false;

    void resetModel (int m) noexcept
    {
        switch (m)
        {
        case Lagoon:
            lagoon.reset();
            break;
        case Reef:
            reef.reset();
            break;
        default:
            abyss.reset();
            break;
        }
    }

    void processModel (int m, const AudioView& v) noexcept
    {
        switch (m)
        {
        case Lagoon:
            lagoon.process (v);
            break;
        case Reef:
            reef.process (v);
            break;
        default:
            abyss.process (v);
            break;
        }
    }

    void startSwitch (int to) noexcept
    {
        previous = current;
        current = to;
        resetModel (to); // no stale tails from the last time it played
        fade.setCurrentAndTarget (0.0f);
        fade.setTarget (1.0f);
        if (!fade.isSmoothing()) // not prepared yet: switch immediately
            previous = -1;
    }

    void requestModel (int m) noexcept
    {
        if (previous < 0)
        {
            if (m != current)
                startSwitch (m);
            return;
        }
        if (m == current)
            pending = -1;
        else if (m == previous)
        {
            // Reverse the running fade from where it is.
            std::swap (current, previous);
            fade.setCurrentAndTarget (1.0f - fade.current());
            fade.setTarget (1.0f);
            pending = -1;
        }
        else
            pending = m;
    }

    void processFade (const AudioView& io) noexcept
    {
        const int n = io.numFrames();
        std::array<float*, kMaxChannels> ptrs {};
        for (int c = 0; c < io.numChannels(); ++c)
        {
            ptrs[static_cast<size_t> (c)] = scratch[static_cast<size_t> (c)].data();
            std::memcpy (ptrs[static_cast<size_t> (c)], io.channel (c),
                         sizeof (float) * static_cast<size_t> (n));
        }
        const AudioView old (ptrs.data(), io.numChannels(), n);
        processModel (previous, old);
        processModel (current, io);

        for (int i = 0; i < n; ++i)
        {
            const float g = fade.next();
            const float w = g * g * (3.0f - 2.0f * g); // smoothstep: no slope jump at either end
            for (int c = 0; c < io.numChannels(); ++c)
            {
                const float o = old.channel (c)[i];
                float& y = io.channel (c)[i];
                y = o + w * (y - o);
            }
        }

        if (!fade.isSmoothing())
        {
            previous = -1;
            if (pending >= 0 && pending != current)
                startSwitch (pending);
            pending = -1;
        }
    }
};

AmpSection::AmpSection() : impl_ (std::make_unique<Impl>()) {}
AmpSection::~AmpSection() = default;

void AmpSection::prepare (const ProcessSpec& spec)
{
    auto& d = *impl_;
    d.lagoon.prepare (spec.sampleRate);
    d.reef.prepare (spec.sampleRate);
    d.abyss.prepare (spec.sampleRate);
    for (auto& s : d.scratch)
        s.assign (static_cast<size_t> (kControlBlock), 0.0f);
    d.fade.reset (spec.sampleRate, kModelFadeSeconds);
    d.prepared = true;
    reset();
}

void AmpSection::reset() noexcept
{
    auto& d = *impl_;
    d.lagoon.reset();
    d.reef.reset();
    d.abyss.reset();
    if (d.previous >= 0)
        d.current = d.pending >= 0 ? d.pending : d.current;
    d.previous = d.pending = -1;
    d.fade.setCurrentAndTarget (1.0f);
}

void AmpSection::setSettings (const Settings& raw) noexcept
{
    auto& d = *impl_;
    const Settings s = sanitise (raw);
    d.lagoon.setSettings (s.lagoon);
    d.reef.setSettings (s.reef);
    d.abyss.setSettings (s.abyss);
    d.requestModel (s.model);
}

void AmpSection::process (const AudioView& io) noexcept
{
    auto& d = *impl_;
    if (!d.prepared)
        return;
    for (int start = 0; start < io.numFrames(); start += kControlBlock)
    {
        const AudioView chunk =
            io.subView (start, std::min (kControlBlock, io.numFrames() - start));
        if (d.previous >= 0)
            d.processFade (chunk);
        else
            d.processModel (d.current, chunk);
    }
}

int AmpSection::latencySamples() const noexcept
{
    // All models share the same oversampling factor, hence the same latency.
    return impl_->lagoon.latencySamples();
}

} // namespace sedef::dsp
