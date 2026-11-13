// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Looper.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

namespace sedef::dsp
{
namespace
{
constexpr int kShift = LoopMemory::kChunkShift;
constexpr int kMask = LoopMemory::kChunkMask;
constexpr long long kRingMask = LoopMemory::kRingFrames - 1;
constexpr int kMaxFade = 2048; // the pre-roll's size: X at 192 kHz is 1920

int samples (double seconds, double fs) noexcept
{
    return static_cast<int> (std::lround (seconds * fs));
}
} // namespace

LoopMemory::LoopMemory()
    : pool (static_cast<std::size_t> (kPoolChunks) * 2 * static_cast<std::size_t> (kChunkFrames),
            0.0f)
{
    for (auto& r : ring)
        r.assign (static_cast<std::size_t> (kRingFrames), 0.0f);
}

// -------------------------------------------------------------------------------------------------

float Looper::Ramp::value() const noexcept
{
    if (k <= 0)
        return 0.0f;
    if (k >= len)
        return 1.0f;
    return 0.5f * (1.0f - std::cos (kPi * static_cast<float> (k) / static_cast<float> (len)));
}

Looper::Looper()
{
    live_.fill (-1);
    other_.fill (-1);
}

void Looper::prepare (const ProcessSpec& spec)
{
    const bool newRate = spec.sampleRate != sampleRate_;
    sampleRate_ = spec.sampleRate;
    const double fs = spec.sampleRate;
    fade_ = std::clamp (samples (kFadeSeconds, fs), 1, kMaxFade);
    // The ring must reach a grace and a crossfade back (docs/looper.md, "Bars").
    grace_ = std::min (samples (kGraceSeconds, fs), LoopMemory::kRingFrames - fade_ - 2);
    minLoop_ = std::max (samples (kMinLoopSeconds, fs), 4 * fade_);
    maxLatency_ = samples (kMaxLatencySeconds, fs);
    play_.len = dub_.len = swap_.len = fade_;
    level_.reset (fs, 0.02);
    feedback_.reset (fs, 0.02);
    snap_ = true;
    if (newRate)
        clearEverything();
    reset();
}

void Looper::attach (LoopMemory* memory) noexcept
{
    if (memory == nullptr || memory == mem_)
        return;
    mem_ = memory;
    live_.fill (-1);
    other_.fill (-1);
    refs_.fill (0);
    numFree_ = LoopMemory::kPoolChunks;
    // Popped from the end: chunks are handed out 0, 1, 2, ... (deterministic).
    for (int i = 0; i < LoopMemory::kPoolChunks; ++i)
        free_[static_cast<std::size_t> (i)] = LoopMemory::kPoolChunks - 1 - i;
    liveInfo_ = otherInfo_ = {};
    lastAction_ = Action::none;
    undone_ = false;
    length_ = 0;
    synced_ = false;
    state_ = State::empty;
}

void Looper::clearEverything() noexcept
{
    if (mem_ == nullptr)
        return;
    baseWriting_ = false;
    releaseTable (live_);
    releaseTable (other_);
    liveInfo_ = otherInfo_ = {};
    lastAction_ = Action::none;
    undone_ = false;
    length_ = 0;
    synced_ = false;
    state_ = State::empty;
}

void Looper::reset() noexcept
{
    numWraps_ = 0;
    queued_ = 0;
    if (mem_ == nullptr)
    {
        state_ = State::unavailable;
        return;
    }
    if (afterFade_ == AfterFade::clear)
        finishClear();
    afterFade_ = AfterFade::none;
    if (state_ == State::recording || state_ == State::closing)
        discardTake();
    else if (baseWriting_)
        completeBaseNow();
    play_.set (false);
    dub_.set (false);
    swap_.set (false);
    dubOn_ = false;
    dubPending_ = false;
    playPos_ = 0;
    closeAt_ = -1;
    stopAfterClose_ = false;
    armDownbeats_ = 0;
    state_ = length_ > 0 ? State::stopped : State::empty;
    now_ = 0;
    lastDownbeat_ = -1;
    for (auto& r : mem_->ring)
        std::fill (r.begin(), r.end(), 0.0f);
}

void Looper::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    settings_.levelDb = std::isfinite (s.levelDb) ? std::clamp (s.levelDb, -60.0f, 12.0f) : 0.0f;
    settings_.feedback = std::isfinite (s.feedback) ? std::clamp (s.feedback, 0.0f, 1.0f) : 1.0f;
    settings_.latencySamples = std::max (0, s.latencySamples);
    const float gain = dbToGain (settings_.levelDb);
    if (snap_)
    {
        level_.setCurrentAndTarget (gain);
        feedback_.setCurrentAndTarget (settings_.feedback);
        snap_ = false;
    }
    else
    {
        level_.setTarget (gain);
        feedback_.setTarget (settings_.feedback);
    }

    if (mem_ != nullptr && !settings_.clickRunning)
    {
        // The click stopped: nothing to wait for any more.
        if (state_ == State::armed)
            cancelArm();
        else if (state_ == State::closing && closeAt_ < 0)
            closeAt_ = now_;
    }
}

int Looper::length() const noexcept
{
    if (state_ == State::recording || state_ == State::closing)
        return static_cast<int> (std::max (0LL, now_ - takeStart_));
    return length_;
}

int Looper::bars() const noexcept
{
    if (state_ == State::recording || state_ == State::closing)
        return takeSync_ ? barsSoFar_ : 0;
    return length_ > 0 && synced_ ? liveInfo_.bars : 0;
}

bool Looper::canUndo() const noexcept
{
    if (mem_ == nullptr || undone_)
        return false;
    if (lastAction_ == Action::clear)
        return state_ == State::empty;
    return lastAction_ == Action::layer &&
           (state_ == State::playing || state_ == State::overdubbing || state_ == State::stopped);
}

bool Looper::canRedo() const noexcept
{
    if (mem_ == nullptr || !undone_)
        return false;
    if (lastAction_ == Action::clear)
        return state_ == State::stopped || state_ == State::playing;
    return lastAction_ == Action::layer &&
           (state_ == State::playing || state_ == State::overdubbing || state_ == State::stopped);
}

void Looper::setMaxFramesForTest (int frames) noexcept
{
    maxFrames_ = std::clamp (frames, minLoop_, LoopMemory::kMaxFrames);
}

std::vector<float> Looper::loopSamples (int channel, bool other) const
{
    const Table& table = other ? other_ : live_;
    const int n = other ? otherInfo_.length : length_;
    std::vector<float> out (static_cast<std::size_t> (std::max (0, n)));
    if (mem_ == nullptr)
        return out;
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] = readLoop (table, i, channel);
    return out;
}

// -------------------------------------------------------------------------------------------------
// Process

void Looper::process (const AudioView& io, const Event* events, int numEvents) noexcept
{
    numWraps_ = 0;
    const int n = io.numFrames();
    if (mem_ == nullptr || n <= 0 || io.numChannels() <= 0)
    {
        now_ += std::max (0, n);
        return;
    }
    const int numCh = std::min (io.numChannels(), 2);
    float* ch[2] = { io.channel (0), numCh > 1 ? io.channel (1) : io.channel (0) };

    for (int i = 0; i < n; ++i)
    {
        const long long t = now_ + i;
        if (queued_ > 0 && !blocked())
            runQueued (t);
        // Downbeats first, then commands: a press on the downbeat's own sample sees it.
        for (int e = 0; e < numEvents; ++e)
            if (events[e].offset == i && events[e].kind == Event::Kind::downbeat)
                onDownbeat (t);
        for (int e = 0; e < numEvents; ++e)
            if (events[e].offset == i && events[e].kind == Event::Kind::command)
                onCommand (events[e].command, t);
        processSample (ch, numCh, i, t, level_.next(), feedback_.next());
    }
    now_ += n;
}

float Looper::ring (long long t, int channel) const noexcept
{
    return mem_->ring[static_cast<std::size_t> (channel)][static_cast<std::size_t> (t & kRingMask)];
}

void Looper::processSample (float* const* ch, int numCh, int i, long long t, float gain,
                            float feedback) noexcept
{
    const float l = ch[0][i];
    const float r = numCh > 1 ? ch[1][i] : l;
    const auto ri = static_cast<std::size_t> (t & kRingMask);
    mem_->ring[0][ri] = l;
    mem_->ring[1][ri] = r;

    // A take that reaches its end on this sample closes here.
    if (state_ == State::recording || state_ == State::closing)
    {
        if (t - takeStart_ >= maxFrames_)
            forcedClose (t);
        else if (state_ == State::closing && closeAt_ >= 0 && t >= closeAt_)
            closeTake (closeAt_, closeAt_ - takeStart_, t - closeAt_, stopAfterClose_);
    }

    float outL = 0.0f, outR = 0.0f;
    if ((play_.k > 0 || play_.up) && length_ > 0)
    {
        if (playPos_ == 0 && lockable() && numWraps_ < kMaxWraps)
            wraps_[static_cast<std::size_t> (numWraps_++)] = i;
        play_.step();
        const float g = play_.value() * gain;
        float yl = readLoop (live_, playPos_, 0);
        float yr = numCh > 1 ? readLoop (live_, playPos_, 1) : yl;
        if (swap_.moving())
        {
            swap_.step();
            const float w = swap_.value();
            const float ol = readLoop (other_, playPos_, 0);
            const float orr = numCh > 1 ? readLoop (other_, playPos_, 1) : ol;
            yl = ol + w * (yl - ol);
            yr = orr + w * (yr - orr);
        }
        outL = g * yl;
        outR = g * yr;

        // Overdub: the write head trails the playhead by the latency (docs/looper.md).
        if (!dubPending_ && (dub_.up || dub_.k > 0))
        {
            dub_.step();
            const float d = dub_.value();
            if (d > 0.0f)
            {
                int w = playPos_ - dubLatency_;
                if (w < 0)
                    w += length_;
                const float keep = 1.0f - d * (1.0f - feedback);
                float* bl = writable (w, 0);
                *bl = *bl * keep + l * d;
                float* br = writable (w, 1);
                *br = *br * keep + r * d;
            }
        }
        if (++playPos_ >= length_)
            playPos_ = 0;
    }

    // The take: written the latency late, so its last samples arrive after it closed.
    if (baseWriting_)
    {
        const long long idx = t - takeStart_ - takeLatency_;
        if (idx >= baseWritten_)
        {
            writeBase (idx, l, r);
            baseWritten_ = idx + 1;
            if (length_ > 0 && baseWritten_ >= length_)
                finishBase();
        }
    }

    // Nothing playing leaves the signal untouched (bit for bit, signed zeros included).
    if (outL != 0.0f)
        ch[0][i] = l + outL;
    if (numCh > 1 && outR != 0.0f)
        ch[1][i] = r + outR;

    if (afterFade_ != AfterFade::none && play_.k == 0 && !play_.up)
    {
        const auto what = afterFade_;
        afterFade_ = AfterFade::none;
        dub_.set (false);
        if (what == AfterFade::clear)
        {
            finishClear();
        }
        else
        {
            state_ = State::stopped;
            playPos_ = 0;
        }
    }
}

bool Looper::lockable() const noexcept
{
    return synced_ && settings_.clickRunning && settings_.clickBpm == liveInfo_.bpm &&
           settings_.clickSignature == liveInfo_.signature &&
           (state_ == State::playing || state_ == State::overdubbing);
}

// -------------------------------------------------------------------------------------------------
// Events

bool Looper::blocked() const noexcept
{
    return afterFade_ != AfterFade::none || swap_.moving();
}

void Looper::runQueued (long long t) noexcept
{
    while (queued_ > 0 && !blocked())
    {
        const Command c = queue_[0];
        for (int q = 1; q < queued_; ++q)
            queue_[static_cast<std::size_t> (q - 1)] = queue_[static_cast<std::size_t> (q)];
        --queued_;
        dispatch (c, t);
    }
}

void Looper::onCommand (Command c, long long t) noexcept
{
    // A press during a fade (a second press within 10 ms) acts when the fade ends.
    if (blocked() || queued_ > 0)
    {
        if (queued_ < static_cast<int> (queue_.size()))
            queue_[static_cast<std::size_t> (queued_++)] = c;
        return;
    }
    dispatch (c, t);
}

void Looper::onDownbeat (long long t) noexcept
{
    lastDownbeat_ = t;
    switch (state_)
    {
    case State::armed:
        if (--armDownbeats_ <= 0)
        {
            if (armForPlay_)
                startPlay (0, true);
            else
                startTake (t, t);
        }
        break;
    case State::recording:
        if (takeSync_ && t > takeStart_)
            ++barsSoFar_;
        break;
    case State::closing:
        if (takeSync_ && closeAt_ < 0 && t > takeStart_)
        {
            ++barsSoFar_;
            closeTake (t, t - takeStart_, 0, stopAfterClose_);
        }
        break;
    default:
        break;
    }
}

void Looper::dispatch (Command c, long long t) noexcept
{
    switch (c)
    {
    case Command::main:
        switch (state_)
        {
        case State::empty:
            if (!settings_.clickRunning)
            {
                startTake (t, t);
                break;
            }
            {
                // Up to G after a "1" the press was meant for it (docs/looper.md).
                const bool late = lastDownbeat_ >= 0 && t - lastDownbeat_ <= grace_;
                if (!settings_.countIn && late)
                {
                    startTake (lastDownbeat_, t);
                }
                else
                {
                    state_ = State::armed;
                    armForPlay_ = false;
                    armDownbeats_ = (settings_.countIn ? 1 : 0) + (late ? 0 : 1);
                }
            }
            break;
        case State::armed:
            cancelArm();
            break;
        case State::recording:
            requestClose (t, false);
            break;
        case State::playing:
            state_ = State::overdubbing;
            dubOn_ = true;
            if (baseWriting_)
                dubPending_ = true;
            else
                beginDub();
            break;
        case State::overdubbing:
            state_ = State::playing;
            dubOn_ = false;
            dubPending_ = false;
            dub_.up = false;
            break;
        case State::stopped:
            armOrPlay (t);
            break;
        case State::closing:
        case State::unavailable:
            break;
        }
        break;

    case Command::stop:
        switch (state_)
        {
        case State::armed:
            cancelArm();
            break;
        case State::recording:
            requestClose (t, true);
            break;
        case State::closing:
            stopAfterClose_ = true;
            break;
        case State::playing:
        case State::overdubbing:
            dubOn_ = false;
            dubPending_ = false;
            dub_.up = false;
            play_.up = false;
            afterFade_ = AfterFade::stop;
            break;
        default:
            break;
        }
        break;

    case Command::undo:
    case Command::redo:
    {
        const bool undo = c == Command::undo;
        if (!(undo ? canUndo() : canRedo()))
            break;
        if (lastAction_ == Action::clear)
            undoRedoClear (undo);
        else
            undoRedo (undo);
        break;
    }

    case Command::clear:
        switch (state_)
        {
        case State::armed:
            if (armForPlay_)
            {
                state_ = State::stopped;
                startClear();
            }
            else
            {
                state_ = State::empty;
            }
            break;
        case State::recording:
        case State::closing:
            discardTake();
            break;
        case State::playing:
        case State::overdubbing:
        case State::stopped:
            startClear();
            break;
        default:
            break;
        }
        break;
    }
}

// -------------------------------------------------------------------------------------------------
// Transitions

void Looper::cancelArm() noexcept
{
    state_ = armForPlay_ ? State::stopped : State::empty;
    armDownbeats_ = 0;
}

void Looper::startTake (long long start, long long t) noexcept
{
    // A new take ends what could be undone, a cleared loop included.
    releaseTable (other_);
    releaseTable (live_);
    liveInfo_ = otherInfo_ = {};
    lastAction_ = Action::none;
    undone_ = false;

    state_ = State::recording;
    takeStart_ = start;
    takeSync_ = settings_.clickRunning;
    takeLatency_ = std::clamp (settings_.latencySamples, 0, maxLatency_);
    barsSoFar_ = 0;
    closeAt_ = -1;
    stopAfterClose_ = false;
    length_ = 0;
    synced_ = false;
    playPos_ = 0;
    play_.set (false);
    baseWriting_ = true;
    baseWritten_ = 0;
    // Started on a downbeat already past (the grace): what was played since is in the ring.
    for (long long u = start + takeLatency_; u < t; ++u)
    {
        writeBase (u - start - takeLatency_, ring (u, 0), ring (u, 1));
        baseWritten_ = u - start - takeLatency_ + 1;
    }
}

void Looper::requestClose (long long t, bool thenStop) noexcept
{
    const long long elapsed = t - takeStart_;
    if (takeSync_ && settings_.clickRunning)
    {
        // Rounded to the nearest bar line: a late press closes on the "1" just past.
        const long long a = lastDownbeat_;
        const bool late = barsSoFar_ >= 1 && a > takeStart_ &&
                          static_cast<double> (t - a) < 0.5 * settings_.barSamples &&
                          a - takeStart_ >= minLoop_;
        if (late)
        {
            closeTake (a, a - takeStart_, t - a, thenStop);
            return;
        }
        state_ = State::closing;
        closeAt_ = -1; // the next downbeat
        stopAfterClose_ = thenStop;
        return;
    }
    if (elapsed >= minLoop_)
    {
        closeTake (t, elapsed, 0, thenStop);
        return;
    }
    state_ = State::closing;
    closeAt_ = takeStart_ + minLoop_;
    stopAfterClose_ = thenStop;
}

void Looper::forcedClose (long long t) noexcept
{
    if (takeSync_ && settings_.clickRunning && barsSoFar_ >= 1 && lastDownbeat_ > takeStart_)
        closeTake (lastDownbeat_, lastDownbeat_ - takeStart_, t - lastDownbeat_, stopAfterClose_);
    else
        closeTake (t, t - takeStart_, 0, stopAfterClose_);
}

void Looper::closeTake (long long closeAt, long long length, long long playFrom,
                        bool thenStop) noexcept
{
    length_ = static_cast<int> (std::clamp (length, 1LL, static_cast<long long> (maxFrames_)));
    synced_ = takeSync_ && settings_.clickRunning && closeAt == lastDownbeat_;
    liveInfo_ = { length_, synced_, settings_.clickBpm, settings_.clickSignature,
                  synced_ ? barsSoFar_ : 0 };
    closeAt_ = -1;
    if (thenStop)
    {
        state_ = State::stopped;
        playPos_ = 0;
        play_.set (false);
    }
    else
    {
        // The first pass starts from silence, so it fades in like any start; from the second
        // pass on, the wrap is the seam, which keeps the first attack whole.
        startPlay (static_cast<int> (playFrom % length_), true);
    }
    if (baseWritten_ >= length_)
        finishBase();
}

void Looper::startPlay (int from, bool fadeIn) noexcept
{
    state_ = State::playing;
    playPos_ = length_ > 0 ? from % length_ : 0;
    if (fadeIn)
    {
        play_.k = 0;
        play_.up = true;
    }
    else
    {
        play_.set (true);
    }
}

void Looper::armOrPlay (long long t) noexcept
{
    if (!settings_.clickRunning)
    {
        startPlay (0, true);
        return;
    }
    if (lastDownbeat_ >= 0 && t - lastDownbeat_ <= grace_)
    {
        startPlay (static_cast<int> ((t - lastDownbeat_) % length_), true);
        return;
    }
    state_ = State::armed;
    armForPlay_ = true;
    armDownbeats_ = 1;
}

void Looper::beginDub() noexcept
{
    if (dub_.k == 0)
    {
        // A new layer: what it changes is kept for Undo (copy-on-write, docs/looper.md).
        releaseTable (other_);
        for (std::size_t k = 0; k < live_.size(); ++k)
        {
            other_[k] = live_[k];
            if (live_[k] >= 0)
                ++refs_[static_cast<std::size_t> (live_[k])];
        }
        otherInfo_ = liveInfo_;
        lastAction_ = Action::layer;
        undone_ = false;
        ++layerSerial_;
        dubLatency_ = std::clamp (settings_.latencySamples, 0, maxLatency_);
    }
    dub_.up = true;
}

void Looper::endDubNow() noexcept
{
    dubOn_ = false;
    dubPending_ = false;
    dub_.set (false);
}

void Looper::undoRedo (bool undo) noexcept
{
    // The layer's writes stop at once: its whole content changes tables.
    endDubNow();
    if (state_ == State::overdubbing)
        state_ = State::playing;
    std::swap (live_, other_);
    std::swap (liveInfo_, otherInfo_);
    undone_ = undo;
    length_ = liveInfo_.length;
    synced_ = liveInfo_.synced;
    if (play_.k > 0 || play_.up)
    {
        swap_.k = 0;
        swap_.up = true;
    }
}

void Looper::undoRedoClear (bool undo) noexcept
{
    if (undo)
    {
        std::swap (live_, other_);
        std::swap (liveInfo_, otherInfo_);
        length_ = liveInfo_.length;
        synced_ = liveInfo_.synced;
        playPos_ = 0;
        state_ = State::stopped;
        undone_ = true;
        return;
    }
    startClear(); // ends as a Clear that can be undone again
}

void Looper::startClear() noexcept
{
    if (baseWriting_)
        completeBaseNow();
    endDubNow();
    if (play_.k > 0 || play_.up)
    {
        play_.up = false;
        afterFade_ = AfterFade::clear;
        return;
    }
    finishClear();
}

void Looper::finishClear() noexcept
{
    releaseTable (other_);
    other_ = live_;
    otherInfo_ = liveInfo_;
    live_.fill (-1);
    liveInfo_ = {};
    lastAction_ = Action::clear;
    undone_ = false;
    length_ = 0;
    synced_ = false;
    playPos_ = 0;
    play_.set (false);
    swap_.set (false);
    afterFade_ = AfterFade::none;
    state_ = State::empty;
}

void Looper::discardTake() noexcept
{
    baseWriting_ = false;
    releaseTable (live_);
    liveInfo_ = {};
    length_ = 0;
    synced_ = false;
    playPos_ = 0;
    play_.set (false);
    closeAt_ = -1;
    state_ = State::empty;
}

// -------------------------------------------------------------------------------------------------
// The take's samples

void Looper::writeBase (long long idx, float l, float r) noexcept
{
    if (idx < 0 ||
        idx >= static_cast<long long> (LoopMemory::kMaxChunks) * LoopMemory::kChunkFrames)
        return;
    if (idx == 0)
    {
        // The X samples just before the take, for the seam (docs/looper.md, section 6).
        const long long first = takeStart_ + takeLatency_ - fade_;
        for (int j = 0; j < fade_; ++j)
            for (int c = 0; c < 2; ++c)
                preroll_[static_cast<std::size_t> (c)][static_cast<std::size_t> (j)] =
                    ring (first + j, c);
    }
    const auto pos = static_cast<int> (idx);
    auto& slot = live_[static_cast<std::size_t> (pos >> kShift)];
    if (slot < 0)
        slot = allocChunk();
    mem_->chunk (slot, 0)[pos & kMask] = l;
    mem_->chunk (slot, 1)[pos & kMask] = r;
}

void Looper::completeBaseNow() noexcept
{
    // Interrupted before its last samples arrived: they stay silent.
    for (long long idx = baseWritten_; idx < length_; ++idx)
        writeBase (idx, 0.0f, 0.0f);
    baseWritten_ = length_;
    finishBase();
}

void Looper::finishBase() noexcept
{
    baseWriting_ = false;
    // A late close wrote past the loop's end: those chunks go back.
    const auto used = static_cast<std::size_t> ((length_ + LoopMemory::kChunkFrames - 1) >> kShift);
    for (std::size_t k = used; k < live_.size(); ++k)
    {
        releaseChunk (live_[k]);
        live_[k] = -1;
    }
    // The seam: the loop's last X samples fade into the X before its first, which run into it.
    const int x = std::min (fade_, length_);
    for (int j = 0; j < x; ++j)
    {
        Ramp ramp;
        ramp.len = x;
        ramp.k = j + 1;
        const float w = ramp.value();
        const int pos = length_ - x + j;
        for (int c = 0; c < 2; ++c)
        {
            float* s = writable (pos, c);
            const float pre = preroll_[static_cast<std::size_t> (c)][static_cast<std::size_t> (j)];
            *s += w * (pre - *s);
        }
    }
    if (dubPending_ && dubOn_)
    {
        dubPending_ = false;
        beginDub();
    }
    dubPending_ = false;
}

// -------------------------------------------------------------------------------------------------
// Chunks (docs/looper.md, "Undo")

float Looper::readLoop (const Table& table, int pos, int channel) const noexcept
{
    const auto c = table[static_cast<std::size_t> (pos >> kShift)];
    return c < 0 ? 0.0f : mem_->chunk (c, channel)[pos & kMask];
}

float* Looper::writable (int pos, int channel) noexcept
{
    auto& slot = live_[static_cast<std::size_t> (pos >> kShift)];
    if (slot < 0)
    {
        slot = allocChunk();
        for (int c = 0; c < 2; ++c)
            std::fill_n (mem_->chunk (slot, c), LoopMemory::kChunkFrames, 0.0f);
    }
    else if (refs_[static_cast<std::size_t> (slot)] > 1)
    {
        // Shared with the undo table: the layer writes into its own copy.
        const int copy = allocChunk();
        for (int c = 0; c < 2; ++c)
            std::memcpy (mem_->chunk (copy, c), mem_->chunk (slot, c),
                         sizeof (float) * static_cast<std::size_t> (LoopMemory::kChunkFrames));
        releaseChunk (slot);
        slot = copy;
    }
    return mem_->chunk (slot, channel) + (pos & kMask);
}

int Looper::allocChunk() noexcept
{
    // Never empty: at most two tables of kMaxChunks are referenced (docs/looper.md).
    assert (numFree_ > 0);
    const int c = free_[static_cast<std::size_t> (--numFree_)];
    refs_[static_cast<std::size_t> (c)] = 1;
    return c;
}

void Looper::releaseChunk (std::int32_t c) noexcept
{
    if (c < 0)
        return;
    auto& r = refs_[static_cast<std::size_t> (c)];
    if (r > 0 && --r == 0)
        free_[static_cast<std::size_t> (numFree_++)] = c;
}

void Looper::releaseTable (Table& table) noexcept
{
    for (auto& c : table)
    {
        releaseChunk (c);
        c = -1;
    }
}

} // namespace sedef::dsp
