// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The live layer (docs/live.md §7): reads a block's MIDI on the audio thread and turns it into
// parameter values for that block, without touching the host's parameters there. The message
// thread then writes those values to the host's parameters and acknowledges them.

#include "sedef/core/RtMailbox.h"
#include "sedef/core/SpscRing.h"
#include "sedef/live/LiveMap.h"
#include "sedef/live/SceneBank.h"
#include "sedef/params/Parameters.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>

namespace sedef::live
{

/// A scene switch made on the audio thread (a controller, a program change, the Scene parameter),
/// with the values the scene it left was playing.
struct SceneReport
{
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint8_t numParams = 0;
    /// The on-screen switch count of the bank this switch was made on (§7.2).
    std::uint32_t basedOn = 0;
    std::array<std::uint16_t, kMaxSceneParams> params {};
    std::array<float, kMaxSceneParams> values {};
};

/// What the audio thread asks of the message thread.
struct LiveEvent
{
    enum class Type : std::uint8_t
    {
        sceneSwitched, ///< `scene`
        program,       ///< load the preset for `program` (no scene assigned to it)
        presetStep     ///< step the preset list by `step` (+1 or -1)
    };
    Type type = Type::program;
    std::int8_t step = 0;
    std::uint8_t program = 0;
    SceneReport scene;
};

/// A message seen while learning, with the audio clock (samples) at its block.
struct LearnMessage
{
    MidiMessage message;
    std::int64_t time = 0;
};

class LiveControl
{
public:
    static constexpr int kEventCapacity = 64;
    static constexpr int kLearnCapacity = 256;

    LiveControl();

    // Message thread (callers serialise these) ----------------------------------------------------

    /// The bindings and the program table. Newest wins, picked up at the next block.
    void setMap (const MidiMap& map) noexcept;
    /// The master scene bank (§7.2). Newest wins.
    void setScenes (const SceneBank& bank) noexcept;
    /// While learning, every control and program change goes to the learn ring and does nothing.
    void setLearning (bool learning) noexcept { learning_.store (learning); }
    bool learning() const noexcept { return learning_.load(); }

    bool popEvent (LiveEvent& out) noexcept { return events_.pop (&out, 1) == 1; }
    bool popLearn (LearnMessage& out) noexcept { return learn_.pop (&out, 1) == 1; }

    /// Writes every value the audio thread set since the last call (`write (ParamId, value)`),
    /// then acknowledges it, so the override ends. The message thread's 50 Hz tick.
    template <typename Write>
    void syncParameters (Write&& write)
    {
        for (std::size_t i = 0; i < kNumParams; ++i)
        {
            const auto seq = syncSeq_[i].load (std::memory_order_acquire);
            if (seq == ackSeq_[i].load (std::memory_order_relaxed))
                continue;
            write (static_cast<ParamId> (i), syncValue_[i].load (std::memory_order_relaxed));
            ackSeq_[i].store (seq, std::memory_order_release);
        }
    }

    /// Samples the audio thread has processed (its clock for learn).
    std::int64_t audioTime() const noexcept { return audioTime_.load (std::memory_order_acquire); }
    /// The on-screen switch count the audio thread has taken (applied or not).
    std::uint32_t consumedSwitchCount() const noexcept { return consumedSwitch_.load(); }
    /// The last control or program change that arrived (status << 16 | data1 << 8 | data2), and
    /// how many have arrived: the MIDI panel's activity light.
    std::uint32_t lastMessage() const noexcept
    {
        return lastMessage_.load (std::memory_order_relaxed);
    }
    std::uint32_t messageCount() const noexcept
    {
        return messageCount_.load (std::memory_order_relaxed);
    }

    // Audio thread
    // ---------------------------------------------------------------------------------

    /// Before the first block at a sample rate (prepareToPlay, audio stopped): the double-tap
    /// window (§4.5) in samples.
    void prepare (double sampleRate) noexcept;

    /// Step 1 of a block, before the parameters are read: ends the overrides the message thread
    /// has acknowledged.
    void acknowledge() noexcept;

    /// Steps 3-6: mailboxes, the Scene parameter, the block's messages in order, then the
    /// overrides written over `snapshot`. Advances the audio clock by `numFrames`.
    void process (ParamSnapshot& snapshot, const MidiMessage* messages, int count,
                  int numFrames) noexcept;

    /// The looper's footswitch presses of the last process() call, in order (§4.5); the
    /// processor hands them to Engine::looperFootswitch.
    int numLooperPresses() const noexcept { return numLooperPresses_; }
    LooperPress looperPress (int i) const noexcept
    {
        return looperPresses_[static_cast<std::size_t> (i)];
    }
    std::int64_t doubleTapWindow() const noexcept { return doubleTapSamples_; }

    /// The scene the audio thread is playing (tests).
    int activeScene() const noexcept { return bank_.active; }
    const SceneBank& bank() const noexcept { return bank_; }
    bool overridden (ParamId id) const noexcept { return overridden_.test (index (id)); }

private:
    float live (const ParamSnapshot& s, std::size_t i) const noexcept
    {
        return overridden_.test (i) ? override_[i] : s.values[i];
    }
    void setOverride (std::size_t i, float value) noexcept;
    void handle (const ParamSnapshot& s, const MidiMessage& m) noexcept;
    void applyBinding (const ParamSnapshot& s, int index, int value) noexcept;
    void pressLooper (LooperPress press) noexcept;
    /// Switches to `to`; false (nothing done) when it can't be reported.
    bool switchScene (const ParamSnapshot& s, int to) noexcept;
    void applyScene (int scene) noexcept;

    RtMailbox<MidiMap> mapMailbox_;
    RtMailbox<SceneBank> sceneMailbox_;
    SpscRing<LiveEvent> events_ { kEventCapacity };
    SpscRing<LearnMessage> learn_ { kLearnCapacity };
    std::atomic<bool> learning_ { false };

    std::array<std::atomic<float>, kNumParams> syncValue_ {};
    std::array<std::atomic<std::uint32_t>, kNumParams> syncSeq_ {};
    std::array<std::atomic<std::uint32_t>, kNumParams> ackSeq_ {};

    std::atomic<std::int64_t> audioTime_ { 0 };
    std::atomic<std::uint32_t> consumedSwitch_ { 0 };
    std::atomic<std::uint32_t> lastMessage_ { 0 };
    std::atomic<std::uint32_t> messageCount_ { 0 };

    // Audio thread only.
    MidiMap map_;
    SceneBank bank_;
    SceneBank incoming_; // the mailbox reads into it (no 2 KB copy on the stack)
    std::uint32_t lastSwitchCount_ = 0;
    std::uint32_t sentReports_ = 0;
    float lastRawScene_ = 0.0f;
    std::array<float, kNumParams> override_ {};
    std::array<std::uint32_t, kNumParams> overrideSeq_ {};
    std::bitset<kNumParams> overridden_;
    std::uint32_t seq_ = 0;

    // The looper's footswitches (§4.5).
    std::int64_t now_ = 0; ///< the audio clock at the block being processed
    std::int64_t doubleTapSamples_ = doubleTapSamples (48000.0);
    /// Per binding: the block a looper:main press waits for its second press in, or -1.
    std::array<std::int64_t, kMaxBindings> lastTap_ {};
    std::array<LooperPress, kMaxLooperPresses> looperPresses_ {};
    int numLooperPresses_ = 0;
};

} // namespace sedef::live
