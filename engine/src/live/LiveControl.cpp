// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/live/LiveControl.h"

#include <cmath>

namespace sedef::live
{
namespace
{
constexpr auto kSceneParam = index (ParamId::scene);

/// Serial-number order (RFC 1982): `a` is at or after `b`, also across the wrap of the counter.
constexpr bool atOrAfter (std::uint32_t a, std::uint32_t b) noexcept
{
    return static_cast<std::int32_t> (a - b) >= 0;
}
} // namespace

LiveControl::LiveControl() = default;

void LiveControl::setMap (const MidiMap& map) noexcept
{
    mapMailbox_.write (map);
}

void LiveControl::setScenes (const SceneBank& bank) noexcept
{
    sceneMailbox_.write (bank);
}

void LiveControl::acknowledge() noexcept
{
    if (overridden_.none())
        return;
    for (std::size_t i = 0; i < kNumParams; ++i)
        if (overridden_.test (i) &&
            atOrAfter (ackSeq_[i].load (std::memory_order_acquire), overrideSeq_[i]))
            overridden_.reset (i);
}

void LiveControl::setOverride (std::size_t i, float value) noexcept
{
    if (++seq_ == 0) // 0 is "nothing set yet"
        ++seq_;
    override_[i] = value;
    overrideSeq_[i] = seq_;
    overridden_.set (i);
    syncValue_[i].store (value, std::memory_order_relaxed);
    syncSeq_[i].store (seq_, std::memory_order_release);
}

void LiveControl::process (ParamSnapshot& snapshot, const MidiMessage* messages, int count,
                           int numFrames) noexcept
{
    // The parameters were read just before: whatever the message thread published before writing
    // the parameters this block saw is visible from here on (docs/live.md §7.1).
    std::atomic_thread_fence (std::memory_order_acquire);

    mapMailbox_.read (map_);

    if (sceneMailbox_.read (incoming_))
    {
        const bool onScreen = incoming_.switchCount != lastSwitchCount_;
        if (onScreen)
        {
            lastSwitchCount_ = incoming_.switchCount;
            consumedSwitch_.store (lastSwitchCount_);
        }
        // A bank that hasn't taken in every switch reported from here would undo one; the
        // message thread publishes again once it has (§7.2). An on-screen switch always counts.
        if (onScreen || incoming_.reportsSeen == sentReports_)
        {
            bank_ = incoming_;
            if (onScreen && incoming_.applySwitch)
                applyScene (bank_.active);
        }
    }

    // The Scene parameter, moved by the host (automation, a controller assignment).
    const float rawScene = snapshot.values[kSceneParam];
    if (rawScene != lastRawScene_)
    {
        lastRawScene_ = rawScene;
        const int to = static_cast<int> (std::lround (rawScene));
        if (to != bank_.active && !learning_.load (std::memory_order_relaxed))
            switchScene (snapshot, to);
    }

    const bool learning = learning_.load (std::memory_order_relaxed);
    const auto now = audioTime_.load (std::memory_order_relaxed);
    for (int k = 0; k < count; ++k)
    {
        const auto& m = messages[k];
        if (!m.isControlChange() && !m.isProgramChange())
            continue;
        lastMessage_.store (static_cast<std::uint32_t> (m.status) << 16 |
                                static_cast<std::uint32_t> (m.data1) << 8 | m.data2,
                            std::memory_order_relaxed);
        messageCount_.fetch_add (1, std::memory_order_relaxed);
        if (learning)
        {
            const LearnMessage l { m, now };
            learn_.push (&l, 1); // full: learn has what it needs from the first messages
            continue;
        }
        handle (snapshot, m);
    }

    for (std::size_t i = 0; i < kNumParams; ++i)
        if (overridden_.test (i))
            snapshot.values[i] = override_[i];

    audioTime_.store (now + numFrames, std::memory_order_release);
}

void LiveControl::handle (const ParamSnapshot& s, const MidiMessage& m) noexcept
{
    if (m.isControlChange())
    {
        for (int b = 0; b < map_.numBindings; ++b)
        {
            const auto& binding = map_.bindings[static_cast<std::size_t> (b)];
            if (binding.channel == m.channel() && binding.controller == m.data1)
                applyBinding (s, binding, m.data2);
        }
        return;
    }

    // A program change.
    if (map_.programChannel >= 0 && map_.programChannel != m.channel())
        return;
    const int scene = map_.programScene[m.data1 & 0x7f];
    if (scene >= 0)
    {
        switchScene (s, scene);
        return;
    }
    LiveEvent e;
    e.type = LiveEvent::Type::program;
    e.program = m.data1;
    events_.push (&e, 1); // full: the message thread is stalled; the request is dropped
}

void LiveControl::applyBinding (const ParamSnapshot& s, const Binding& b, int value) noexcept
{
    switch (b.action)
    {
    case Action::parameter:
    {
        const std::size_t i = b.param;
        if (i >= kNumParams)
            return;
        const auto& spec = kParamSpecs[i];
        if (i == kSceneParam)
        {
            switchScene (s, static_cast<int> (continuousValue (spec, b.low, b.high, value)));
            return;
        }
        if (isSwitch (spec))
        {
            if (const auto on = switchValue (b.mode, live (s, i) >= 0.5f, value))
                setOverride (i, *on ? 1.0f : 0.0f);
            return;
        }
        setOverride (i, continuousValue (spec, b.low, b.high, value));
        return;
    }
    case Action::scene:
        if (actionFires (b.mode, value))
            switchScene (s, b.scene);
        return;
    case Action::sceneNext:
    case Action::scenePrevious:
        if (actionFires (b.mode, value))
        {
            const int step = b.action == Action::sceneNext ? 1 : kNumScenes - 1;
            switchScene (s, (bank_.active + step) % kNumScenes);
        }
        return;
    case Action::presetNext:
    case Action::presetPrevious:
        if (actionFires (b.mode, value))
        {
            LiveEvent e;
            e.type = LiveEvent::Type::presetStep;
            e.step = b.action == Action::presetNext ? 1 : -1;
            events_.push (&e, 1);
        }
        return;
    }
}

bool LiveControl::switchScene (const ParamSnapshot& s, int to) noexcept
{
    if (to < 0 || to >= kNumScenes || to == bank_.active || bank_.numParams == 0)
        return false;
    LiveEvent e;
    e.type = LiveEvent::Type::sceneSwitched;
    auto& r = e.scene;
    r.from = bank_.active;
    r.to = static_cast<std::uint8_t> (to);
    r.numParams = bank_.numParams;
    r.basedOn = lastSwitchCount_;
    for (std::size_t i = 0; i < bank_.numParams; ++i)
    {
        r.params[i] = bank_.params[i];
        r.values[i] = live (s, bank_.params[i]);
    }
    // A switch the message thread can't hear about would leave the two banks disagreeing.
    if (events_.push (&e, 1) != 1)
        return false;
    ++sentReports_;
    auto& left = bank_.values[bank_.active];
    for (std::size_t i = 0; i < bank_.numParams; ++i)
        left[i] = r.values[i];
    // A scene not made yet starts as a copy of the one being left.
    if (!bank_.isMade (to))
        bank_.values[static_cast<std::size_t> (to)] = left;
    bank_.made = static_cast<std::uint8_t> (bank_.made | 1u << bank_.active | 1u << to);
    bank_.active = r.to;
    applyScene (to);
    return true;
}

void LiveControl::applyScene (int scene) noexcept
{
    const auto& values = bank_.values[static_cast<std::size_t> (scene)];
    for (std::size_t i = 0; i < bank_.numParams; ++i)
        setOverride (bank_.params[i], values[i]);
    setOverride (kSceneParam, static_cast<float> (scene));
}

} // namespace sedef::live
