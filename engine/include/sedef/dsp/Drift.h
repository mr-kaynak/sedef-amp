// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Lfo.h"
#include "sedef/core/Smoothed.h"
#include "sedef/core/Transport.h"

#include <array>
#include <atomic>

namespace sedef::dsp
{

/// "Drift" multi-head tape echo with wow/flutter, tape saturation, tone controls in the feedback
/// loop, ping-pong width and host/tap/free sync. Disabling stops new input but lets the repeats
/// ring out (spillover).
class Drift
{
public:
    enum Sync
    {
        Free = 0,
        Host = 1,
        Tap = 2
    };

    struct Settings
    {
        bool enabled = false;
        int heads = 0;          ///< 0..3 -> 1..4 playback heads
        float timeMs = 500.0f;  ///< 16..2500 ms (Free)
        int sync = Host;
        int note = 13;          ///< index into kNoteBeats (1/4 by default)
        float tapBpm = 120.0f;  ///< used when sync == Tap
        float feedback = 0.35f, tapeMod = 0.3f, tapeDrive = 0.0f;
        float bass = 0.5f, treble = 0.5f, width = 1.0f, mix = 0.35f;
    };

    /// Note length in quarter-note beats for each driftNote choice
    /// (1/64T, 1/64, 1/32T, 1/64D, 1/32, 1/16T, 1/32D, 1/16, 1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4,
    ///  1/2T, 1/4D, 1/2, 1/1T, 1/2D, 1/1, 1/1D).
    static float noteBeats (int noteIndex) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Stereo in place (2 channels).
    void process (const AudioView& io, const Transport& transport) noexcept;

    /// The delay time currently in effect (after sync), for the UI.
    float effectiveTimeMs() const noexcept;

private:
    /// Distinct playback-head positions (fractions of the delay time) used by the head patterns.
    static constexpr int kNumSlots = 5;

    float syncedTimeMs (const Transport& transport) const noexcept;
    void applyTargets (bool snap) noexcept;
    void updateTone() noexcept;
    void updateDrive() noexcept;
    void controlTick() noexcept;
    void processSamples (const AudioView& io, int start, int length) noexcept;
    void wake() noexcept;
    void goIdle() noexcept;

    double sampleRate_ = 48000.0;
    Settings settings_ {};
    bool snapNext_ = true;
    bool idle_ = true;
    long long silentSamples_ = 0;

    // Two tape loops: A is fed by the input, B only by A's feedback, and B feeds A. Repeats
    // therefore alternate between A and B; width decides how the two are panned.
    std::array<DelayLine, 2> tape_;
    // Channel index = loop (both loops share coefficients). Record path: emphasis, drive,
    // de-emphasis + LF loss, HF loss. Bass/treble sit in the playback amp: on the wet bus and on
    // the last head's feedback ahead of the loop's soft ceiling, so boosts cannot lift a runaway
    // echo past that ceiling.
    Biquad emphasis_, deEmphasis_, hfLoss_, playbackTone_, feedbackTone_;

    // Delay time follows its target like a critically damped spring whose velocity (the playback
    // speed offset) is limited: smooth pitch bends, no kinks, never backwards.
    double targetDelay_ = 0.0, delay_ = 0.0, delayVelocity_ = 0.0;
    double glideRate_ = 0.0;
    bool snapDelay_ = true;

    std::array<LinearSmoothed, kNumSlots> slotLevel_;
    LinearSmoothed loopGain_, inputGain_, dryGain_, wetGain_, sameSide_, otherSide_;
    LinearSmoothed bassDb_, trebleDb_, drive_, modAmount_;
    float driveK_ = 1.0f, driveGain_ = 1.0f, driveInvSlope_ = 1.0f;

    // Wow and flutter are evaluated per control tick and interpolated in between.
    Lfo wow_, flutter_;
    SmoothRandom wowDrift_, flutterNoise_;
    float mod_ = 0.0f, modStep_ = 0.0f;
    float wowSamples_ = 0.0f, flutterSamples_ = 0.0f;
    int tickCountdown_ = 0;

    std::atomic<float> effectiveTimeMs_ { 500.0f };
};

} // namespace sedef::dsp
