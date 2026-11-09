// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace sedef::midi
{

/// One MIDI 1.0 channel message, placed at a sample of the block it belongs to. Channels are 1-16
/// in every constructor, as players count them; the status byte carries 0-15.
struct Event
{
    int offset = 0; ///< sample within the block
    std::uint8_t bytes[3] {};
    std::uint8_t size = 0;

    static Event noteOn (int channel, int note, int velocity, int offset) noexcept
    {
        return make (0x90, channel, note, std::clamp (velocity, 1, 127), offset);
    }
    /// Note-off with a release velocity of 0 (what instruments expect when none is measured).
    static Event noteOff (int channel, int note, int offset) noexcept
    {
        return make (0x80, channel, note, 0, offset);
    }
    static Event controller (int channel, int number, int value, int offset) noexcept
    {
        return make (0xB0, channel, number, value, offset);
    }
    /// Pitch bend, 0..16383 with 8192 at the centre.
    static Event pitchBend (int channel, int value, int offset) noexcept
    {
        const int v = std::clamp (value, 0, 16383);
        return make (0xE0, channel, v & 0x7F, (v >> 7) & 0x7F, offset);
    }

    Event withOffset (int newOffset) const noexcept
    {
        Event e = *this;
        e.offset = newOffset;
        return e;
    }

    int channel() const noexcept { return (bytes[0] & 0x0F) + 1; }
    int type() const noexcept { return bytes[0] & 0xF0; }
    bool isNoteOn() const noexcept { return type() == 0x90 && bytes[2] > 0; }
    bool isNoteOff() const noexcept { return type() == 0x80 || (type() == 0x90 && bytes[2] == 0); }
    bool isController (int number) const noexcept { return type() == 0xB0 && bytes[1] == number; }
    bool isPitchBend() const noexcept { return type() == 0xE0; }
    int note() const noexcept { return bytes[1]; }
    int velocity() const noexcept { return bytes[2]; }
    int bendValue() const noexcept { return bytes[1] | (bytes[2] << 7); }

private:
    static Event make (int status, int channel, int d1, int d2, int offset) noexcept
    {
        Event e;
        e.offset = offset;
        e.bytes[0] = static_cast<std::uint8_t> (status | (std::clamp (channel, 1, 16) - 1));
        e.bytes[1] = static_cast<std::uint8_t> (std::clamp (d1, 0, 127));
        e.bytes[2] = static_cast<std::uint8_t> (std::clamp (d2, 0, 127));
        e.size = 3;
        return e;
    }
};

inline constexpr int kAllSoundOff = 120;
inline constexpr int kResetControllers = 121;
inline constexpr int kAllNotesOff = 123;
inline constexpr int kBendCentre = 8192;

/// A fixed-capacity list of the events of one block, in the order they happen. Real-time safe:
/// it never allocates. When it is nearly full, pitch bends are dropped first, so a note-off or an
/// All Notes Off always has room.
class EventList
{
public:
    static constexpr int kCapacity = 512;
    /// Room kept for note-offs and controllers once bends are being dropped.
    static constexpr int kReserve = 64;

    void clear() noexcept { size_ = 0; }
    bool push (const Event& e) noexcept
    {
        const int limit = e.isPitchBend() ? kCapacity - kReserve : kCapacity;
        if (size_ >= limit)
            return false;
        events_[static_cast<std::size_t> (size_++)] = e;
        return true;
    }
    int size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const Event& operator[] (int i) const noexcept { return events_[static_cast<std::size_t> (i)]; }
    const Event* begin() const noexcept { return events_.data(); }
    const Event* end() const noexcept { return events_.data() + size_; }

private:
    std::array<Event, kCapacity> events_ {};
    int size_ = 0;
};

} // namespace sedef::midi
