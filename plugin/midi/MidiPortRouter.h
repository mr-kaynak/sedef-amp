// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/SpscRing.h"
#include "sedef/midi/MidiEvent.h"

#include <array>
#include <atomic>
#include <bitset>
#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>
#include <mutex>

namespace sedef::midiplugin
{

/// Sends what the converter decides to a MIDI port, off the audio thread.
///
/// The audio thread hands each block's events, with their sample offsets, to a wait-free queue
/// (push()). A block's audio is all in by the time it is processed, so its events can only go out
/// from then on: an event at the block's last sample at once, an earlier one having waited for the
/// rest of its block already. A thread of its own sends them in order, spread over the block's
/// length when that is no more than kMaxSpreadMs (every event then goes out exactly one block
/// after its sample, and the spacing of notes survives to the router's millisecond), or over
/// kMaxSpreadMs when the block is longer (a note waits for the rest of its block and at most
/// that; holding it for the whole block would make a large buffer cost twice). The destination is a
/// virtual source other apps see (macOS and Linux) or an existing MIDI output (an IAC bus, a
/// hardware port, a loopback port on Windows). Ports are opened and closed on the message thread,
/// as the system's MIDI service expects. Every note-on sent is tracked until its note-off: changing
/// the destination, turning it off and quitting first end every sounding note on the old one, so
/// nothing is left hanging.
class MidiPortRouter final : private juce::Thread
{
public:
    /// A destination as the UI and the saved state name it: "off", "virtual", or "device:" plus a
    /// device identifier.
    static constexpr const char* kOff = "off";
    static constexpr const char* kVirtual = "virtual";
    static constexpr const char* kDevicePrefix = "device:";

    /// `virtualName` is the name other apps see for the virtual source.
    explicit MidiPortRouter (juce::String virtualName);
    ~MidiPortRouter() override;

    /// Whether this platform can create a virtual source (macOS and Linux).
    static bool virtualSupported() noexcept;
    /// Outputs the user can pick, without our own virtual sources. Message thread only.
    static juce::Array<juce::MidiDeviceInfo> availableOutputs();

    // Any thread (the port opens on the message thread) ----------------------------------------
    /// Takes effect once the port is open and the notes sounding on the old destination have
    /// ended.
    void setDestination (const juce::String& destination);
    /// The destination last asked for.
    juce::String destination() const;
    /// What happened when the destination was opened: empty when it is open (or off), else a
    /// sentence for the UI.
    juce::String status() const;
    /// The name of the open destination, for the UI ("Sedef MIDI", "IAC Driver Bus 1").
    juce::String openName() const;

    // Audio thread ------------------------------------------------------------------------------
    /// The longest a block's events are spread over, once the block is in (ms).
    static constexpr double kMaxSpreadMs = 3.0;

    /// Queues the events of a block (their offsets counted from `baseOffset` into the host's
    /// block of `blockFrames` samples; 0: no block, each event goes out its offset from now).
    /// Wait-free; drops events when the queue is full (never in practice: it holds seconds of a
    /// busy line).
    void push (const midi::EventList& events, int baseOffset, double sampleRate,
               int blockFrames = 0) noexcept;
    /// How long after its sample an event goes out, in ms, at most and on average, for a block
    /// of this many samples: what the audio buffer costs every note.
    static double worstDelayMs (int blockFrames, double sampleRate) noexcept;
    static double meanDelayMs (int blockFrames, double sampleRate) noexcept;
    /// Ends the notes sounding at the destination soon, on the router's thread (when the host
    /// stops processing, so no block will end them). Not for the audio thread.
    void endAllSoon() noexcept
    {
        endAllRequested_.store (true, std::memory_order_relaxed);
        notify();
    }
    /// Whether anything is listening; push() is cheap either way.
    bool active() const noexcept { return active_.load (std::memory_order_relaxed); }

private:
    struct Packet
    {
        double dueMs = 0.0;
        std::uint8_t bytes[3] {};
        std::uint8_t size = 0;
    };

    class Output;

    void open (const juce::String& destination);
    void run() override;
    void swapIn();
    void send (const Packet& p);
    void endAllNotes();
    void flushPending();
    /// Closes a port on the message thread.
    static void retire (std::unique_ptr<Output> output);

    const juce::String virtualName_;
    SpscRing<Packet> queue_ { 4096 };
    std::atomic<bool> active_ { false }, endAllRequested_ { false };

    // Between the message thread, the router's thread and the UI.
    mutable std::mutex lock_;
    juce::String requested_ { kOff }, status_, openName_;
    std::unique_ptr<Output> incoming_; // opened, waiting for the router's thread to take it
    juce::String incomingStatus_;
    bool swapPending_ = false;

    std::unique_ptr<Output> output_; // the router's thread's
    static constexpr int kPending = 1024;
    std::array<Packet, kPending> pending_ {}; // sorted by dueMs
    int pendingCount_ = 0;
    std::array<std::bitset<128>, 16> on_ {}; // notes sounding at the destination
    std::bitset<16> channelsUsed_;

    JUCE_DECLARE_WEAK_REFERENCEABLE (MidiPortRouter)
};

} // namespace sedef::midiplugin
