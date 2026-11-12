// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Smoothed.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sedef::dsp
{

/// The looper's memory (docs/looper.md, "Memory"): a pool of chunks for the loop and its undo,
/// and the input ring. Reserved off the audio thread and handed to the looper once; the frame
/// budget doesn't depend on the sample rate, so the same memory serves every rate.
struct LoopMemory
{
    static constexpr int kChunkShift = 12;
    static constexpr int kChunkFrames = 1 << kChunkShift; ///< 4096
    static constexpr int kChunkMask = kChunkFrames - 1;
    /// The longest loop: 120 s at 48 kHz.
    static constexpr int kMaxFrames = 5'760'000;
    static constexpr int kMaxChunks = (kMaxFrames + kChunkFrames - 1) / kChunkFrames; ///< 1407
    /// Two tables (the loop and its undo) of at most kMaxChunks each: the pool never runs out.
    static constexpr int kPoolChunks = 2 * kMaxChunks;
    /// The input ring holds the grace and a crossfade (60 ms) at up to 192 kHz: 11 520 frames.
    static constexpr int kRingFrames = 1 << 14;

    /// Allocates and zeroes everything (about 92 MB): never on the audio thread.
    LoopMemory();

    float* chunk (int c, int channel) noexcept
    {
        return pool.data() +
               (static_cast<std::size_t> (c) * 2 + static_cast<std::size_t> (channel)) *
                   static_cast<std::size_t> (kChunkFrames);
    }
    static constexpr std::size_t bytes() noexcept
    {
        return (static_cast<std::size_t> (kPoolChunks) * kChunkFrames + kRingFrames) * 2 *
               sizeof (float);
    }

    std::vector<float> pool; ///< kPoolChunks chunks, each two planar channels of kChunkFrames
    std::array<std::vector<float>, 2> ring;
};

/// A looper for practice and writing: record, play, overdub with feedback, one level of undo,
/// clear with undo. Free length, or whole bars of the metronome with an optional count-in.
/// Every boundary is a 10 ms raised-cosine crossfade. Design, timing and math: docs/looper.md.
///
/// It records the stereo signal it is given and adds the loop's playback to it, in place.
/// Without memory (attach) it does nothing at all.
class Looper
{
public:
    enum class Command : std::uint8_t
    {
        main, ///< record, close and play, overdub, play: what it means depends on the state
        stop, ///< stop playing, or close a take and stay stopped, or cancel a wait
        undo, ///< the last overdub, or a clear
        redo,
        clear, ///< empty the looper (undoable), or discard a take
    };

    enum class State : std::uint8_t
    {
        unavailable, ///< no memory yet
        empty,
        armed,     ///< waiting for a downbeat to start a take or to play
        recording, ///< the first take
        closing,   ///< a take recording up to the sample it was rounded to
        playing,
        overdubbing,
        stopped,
    };

    struct Settings
    {
        float levelDb = 0.0f;
        float feedback = 1.0f; ///< 0..1: how much of the old take stays under an overdub
        bool countIn = false;
        /// The metronome is counting: takes snap to its bars and plays wait for its downbeat.
        bool clickRunning = false;
        /// What the click counts, to lock it to a loop recorded at the same tempo and meter.
        float clickBpm = 120.0f;
        int clickSignature = 2;
        double barSamples = 0.0; ///< the click's bar length (steps x interval)
        /// Input + output latency of the device and the rig's own (docs/looper.md, "Latency
        /// compensation"). Taken when a take or an overdub layer starts.
        int latencySamples = 0;
    };

    struct Event
    {
        enum class Kind : std::uint8_t
        {
            downbeat, ///< the metronome sounds step 0 on this sample
            command
        };
        int offset = 0; ///< sample in the block it acts on
        Kind kind = Kind::command;
        Command command = Command::main;
    };

    static constexpr double kFadeSeconds = 0.010;     ///< X: section 6
    static constexpr double kGraceSeconds = 0.050;    ///< G: a press this late still hits the "1"
    static constexpr double kMinLoopSeconds = 0.25;   ///< Lmin
    static constexpr double kMaxLatencySeconds = 0.1; ///< R is clamped to this
    static constexpr int kMaxWraps = 8;

    Looper();

    /// Not real time. A new sample rate clears the loop (it would play at the wrong pitch); the
    /// same rate keeps it, stopped.
    void prepare (const ProcessSpec& spec);
    /// Audio thread (or before audio starts). The first memory sets up the pool; it never
    /// changes afterwards. Null: the looper does nothing.
    void attach (LoopMemory* memory) noexcept;
    /// Stops any playing, recording or waiting; keeps a loop.
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// In place, 1 or 2 channels (1: the loop is mono). Events sorted by offset, all < frames;
    /// at the same offset downbeats act before commands.
    void process (const AudioView& io, const Event* events, int numEvents) noexcept;

    /// Samples in the last process() call where a loop recorded in bars started a pass while the
    /// click counts its tempo and meter: the click is re-anchored there (Metronome::alignDownbeat).
    int numWraps() const noexcept { return numWraps_; }
    int wrap (int i) const noexcept { return wraps_[static_cast<std::size_t> (i)]; }

    // State, read by the audio thread after process() (and by tests).
    State state() const noexcept { return state_; }
    /// The loop's length, or during a take the samples recorded so far.
    int length() const noexcept;
    /// Where the loop plays (0 when stopped).
    int position() const noexcept { return playPos_; }
    /// Whole bars of a loop recorded in bars (0 for a free loop), or bars counted so far.
    int bars() const noexcept;
    /// Downbeats still to pass before an armed start.
    int waitBars() const noexcept { return state_ == State::armed ? armDownbeats_ : 0; }
    bool armedForPlay() const noexcept { return state_ == State::armed && armForPlay_; }
    bool synced() const noexcept { return length_ > 0 && synced_; }
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    /// The undo on offer brings back a cleared loop.
    bool undoIsClear() const noexcept { return lastAction_ == Action::clear; }
    int maxFrames() const noexcept { return maxFrames_; }

    /// Tests: a shorter longest loop (at most LoopMemory::kMaxFrames).
    void setMaxFramesForTest (int frames) noexcept;
    /// Tests: the loop as it plays (live) or the other table (undo/redo/cleared), one channel.
    std::vector<float> loopSamples (int channel, bool other = false) const;

private:
    enum class Action : std::uint8_t
    {
        none,
        layer,
        clear
    };

    /// A raised-cosine ramp over `len` samples: k moves one step per sample toward 0 or len.
    struct Ramp
    {
        int k = 0;
        int len = 1;
        bool up = false;
        void set (bool on) noexcept
        {
            k = on ? len : 0;
            up = on;
        }
        bool full() const noexcept { return k >= len; }
        bool silent() const noexcept { return k <= 0; }
        bool moving() const noexcept { return up ? k < len : k > 0; }
        void step() noexcept { k += up ? (k < len ? 1 : 0) : (k > 0 ? -1 : 0); }
        float value() const noexcept;
    };

    struct LoopInfo
    {
        int length = 0;
        bool synced = false;
        float bpm = 0.0f;
        int signature = 0;
        int bars = 0;
    };

    using Table = std::array<std::int32_t, LoopMemory::kMaxChunks>;

    // Events
    void onDownbeat (long long t) noexcept;
    void onCommand (Command c, long long t) noexcept;
    void dispatch (Command c, long long t) noexcept;
    bool blocked() const noexcept;
    void runQueued (long long t) noexcept;

    // Transitions
    void startTake (long long start, long long t) noexcept;
    void closeTake (long long closeAt, long long length, long long playFrom,
                    bool thenStop) noexcept;
    void requestClose (long long t, bool thenStop) noexcept;
    void forcedClose (long long t) noexcept;
    void startPlay (int from, bool fadeIn) noexcept;
    void armOrPlay (long long t) noexcept;
    void cancelArm() noexcept;
    void beginDub() noexcept;
    void endDubNow() noexcept;
    void undoRedo (bool undo) noexcept;
    void undoRedoClear (bool undo) noexcept;
    void startClear() noexcept;
    void finishClear() noexcept;
    void discardTake() noexcept;
    void clearEverything() noexcept;
    bool lockable() const noexcept;

    // Per sample
    void processSample (float* const* ch, int numCh, int i, long long t, float gain,
                        float feedback) noexcept;
    float ring (long long t, int channel) const noexcept;
    void writeBase (long long idx, float l, float r) noexcept;
    void completeBaseNow() noexcept;
    void finishBase() noexcept;
    float readLoop (const Table& table, int pos, int channel) const noexcept;
    float* writable (int pos, int channel) noexcept;

    // Chunks
    int allocChunk() noexcept;
    void releaseChunk (std::int32_t c) noexcept;
    void releaseTable (Table& table) noexcept;
    void shareLiveIntoOther() noexcept;
    void swapTables() noexcept;

    LoopMemory* mem_ = nullptr;
    double sampleRate_ = 0.0;
    int fade_ = 480, grace_ = 2400, minLoop_ = 12000, maxLatency_ = 4800;
    int maxFrames_ = LoopMemory::kMaxFrames;
    Settings settings_ {};
    LinearSmoothed level_, feedback_;
    bool snap_ = true;

    // Chunk tables (docs/looper.md, "Undo")
    Table live_ {}, other_ {};
    LoopInfo liveInfo_ {}, otherInfo_ {};
    std::array<std::uint8_t, LoopMemory::kPoolChunks> refs_ {};
    std::array<std::int32_t, LoopMemory::kPoolChunks> free_ {};
    int numFree_ = 0;
    Action lastAction_ = Action::none;
    bool undone_ = false;

    State state_ = State::unavailable;
    long long now_ = 0; ///< samples since reset, at the start of the current call
    long long lastDownbeat_ = -1;

    // Armed
    bool armForPlay_ = false;
    int armDownbeats_ = 0;
    int armPlayFrom_ = 0;

    // The take
    long long takeStart_ = 0;
    bool takeSync_ = false;
    int barsSoFar_ = 0;
    int takeLatency_ = 0;
    long long closeAt_ = -1; ///< closing: the sample it closes on (-1: the next downbeat)
    bool stopAfterClose_ = false;
    bool baseWriting_ = false;
    long long baseWritten_ = 0;                         ///< take samples written so far
    std::array<std::array<float, 2048>, 2> preroll_ {}; ///< X samples before the take (<= 192 kHz)

    // The loop
    int length_ = 0;
    bool synced_ = false;
    int playPos_ = 0;
    Ramp play_;
    Ramp dub_;
    Ramp swap_;               ///< undo/redo crossfade: other_ (old) fades out, live_ fades in
    bool dubOn_ = false;      ///< the overdub the player asked for (the ramp follows it)
    bool dubPending_ = false; ///< asked for while the take's last samples are still written
    int dubLatency_ = 0;
    enum class AfterFade : std::uint8_t
    {
        none,
        stop,
        clear
    } afterFade_ = AfterFade::none;

    std::array<Command, 8> queue_ {};
    int queued_ = 0;

    std::array<int, kMaxWraps> wraps_ {};
    int numWraps_ = 0;
};

} // namespace sedef::dsp
