// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MidiPortRouter.h"

#include <map>

namespace sedef::midiplugin
{

// An open destination. A virtual source is shared by every router in the process that asks for
// the same name (several plug-in instances in one host), so other apps see one port; sends to
// it are serialised.
class MidiPortRouter::Output
{
public:
    struct Port
    {
        std::mutex lock;
        std::unique_ptr<juce::MidiOutput> out;
    };

    static std::unique_ptr<Output> openVirtual (const juce::String& name)
    {
        static std::mutex registryLock;
        static std::map<juce::String, std::weak_ptr<Port>> registry;
        const std::scoped_lock lock (registryLock);
        auto port = registry[name].lock();
        if (port == nullptr)
        {
            auto out = juce::MidiOutput::createNewDevice (name);
            if (out == nullptr)
                return nullptr;
            port = std::make_shared<Port>();
            port->out = std::move (out);
            registry[name] = port;
        }
        return std::unique_ptr<Output> (new Output (std::move (port), name, true));
    }

    static std::unique_ptr<Output> openDevice (const juce::String& identifier)
    {
        auto out = juce::MidiOutput::openDevice (identifier);
        if (out == nullptr)
            return nullptr;
        const auto name = out->getName();
        auto port = std::make_shared<Port>();
        port->out = std::move (out);
        return std::unique_ptr<Output> (new Output (std::move (port), name, false));
    }

    void send (const juce::MidiMessage& m)
    {
        const std::scoped_lock lock (port_->lock);
        port_->out->sendMessageNow (m);
    }

    const juce::String& name() const noexcept { return name_; }
    /// Others may be sending through the same port: no channel-wide messages of our own.
    bool shared() const noexcept { return shared_; }

private:
    Output (std::shared_ptr<Port> port, juce::String name, bool shared)
        : port_ (std::move (port)),
          name_ (std::move (name)),
          shared_ (shared)
    {
    }

    std::shared_ptr<Port> port_;
    juce::String name_;
    bool shared_ = false;
};

// -------------------------------------------------------------------------------------------------

MidiPortRouter::MidiPortRouter (juce::String virtualName)
    : juce::Thread ("Sedef MIDI port"),
      virtualName_ (std::move (virtualName))
{
    startThread (juce::Thread::Priority::high);
}

MidiPortRouter::~MidiPortRouter()
{
    // run() ends the notes still sounding before it returns.
    signalThreadShouldExit();
    notify();
    stopThread (4000);
    retire (std::move (output_));
    const std::scoped_lock lock (lock_);
    retire (std::move (incoming_));
}

void MidiPortRouter::retire (std::unique_ptr<Output> output)
{
    if (output == nullptr)
        return;
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
        return; // closes here
    juce::MessageManager::callAsync ([closing = std::shared_ptr<Output> (std::move (output))] {});
}

bool MidiPortRouter::virtualSupported() noexcept
{
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
    return true;
#else
    return false;
#endif
}

juce::Array<juce::MidiDeviceInfo> MidiPortRouter::availableOutputs()
{
    auto all = juce::MidiOutput::getAvailableDevices();
    all.removeIf ([] (const juce::MidiDeviceInfo& d) { return d.name.startsWith ("Sedef MIDI"); });
    return all;
}

void MidiPortRouter::setDestination (const juce::String& destination)
{
    const auto wanted = destination.isEmpty() ? juce::String (kOff) : destination;
    {
        const std::scoped_lock lock (lock_);
        requested_ = wanted;
    }
    if (juce::MessageManager::existsAndIsCurrentThread())
        open (wanted);
    else
        juce::MessageManager::callAsync (
            [weak = juce::WeakReference<MidiPortRouter> (this), wanted] {
                if (auto* router = weak.get())
                    router->open (wanted);
            });
}

void MidiPortRouter::open (const juce::String& wanted)
{
    {
        const std::scoped_lock lock (lock_);
        if (requested_ != wanted)
            return; // a newer choice is on its way
    }
    std::unique_ptr<Output> output;
    juce::String status;
    if (wanted == kVirtual)
    {
        if (!virtualSupported())
            status = "This system has no virtual MIDI ports: pick an output instead.";
        else if ((output = Output::openVirtual (virtualName_)) == nullptr)
            status = "The virtual port \"" + virtualName_ + "\" could not be created.";
    }
    else if (wanted.startsWith (kDevicePrefix))
    {
        const auto id = wanted.fromFirstOccurrenceOf (kDevicePrefix, false, false);
        if ((output = Output::openDevice (id)) == nullptr)
            status = "That MIDI output is not available. Is the device connected?";
    }
    {
        const std::scoped_lock lock (lock_);
        retire (std::move (incoming_)); // one the router's thread never took
        incoming_ = std::move (output);
        incomingStatus_ = status;
        swapPending_ = true;
    }
    notify();
}

juce::String MidiPortRouter::destination() const
{
    const std::scoped_lock lock (lock_);
    return requested_;
}

juce::String MidiPortRouter::status() const
{
    const std::scoped_lock lock (lock_);
    return status_;
}

juce::String MidiPortRouter::openName() const
{
    const std::scoped_lock lock (lock_);
    return openName_;
}

void MidiPortRouter::push (const midi::EventList& events, int baseOffset,
                           double sampleRate) noexcept
{
    if (events.empty() || !active_.load (std::memory_order_relaxed) || sampleRate <= 0.0)
        return;
    // The block's last sample came in just now; each event goes out one block after its own
    // sample, so the distance between events stays as it was.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double msPerSample = 1000.0 / sampleRate;
    for (const auto& e : events)
    {
        Packet p;
        p.dueMs = nowMs + static_cast<double> (baseOffset + e.offset) * msPerSample;
        p.size = e.size;
        std::copy (e.bytes, e.bytes + 3, p.bytes);
        queue_.push (&p, 1);
    }
    // No notify(): it takes a lock. The thread looks every millisecond.
}

void MidiPortRouter::run()
{
    std::array<Packet, 256> incoming {};
    while (!threadShouldExit())
    {
        swapIn();
        if (endAllRequested_.exchange (false, std::memory_order_relaxed))
        {
            flushPending();
            endAllNotes();
        }
        for (;;)
        {
            const auto n = queue_.pop (incoming.data(), incoming.size());
            if (n == 0)
                break;
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto& p = incoming[i];
                if (output_ == nullptr)
                    continue;
                if (pendingCount_ == kPending)
                {
                    send (pending_[0]);
                    std::move (pending_.begin() + 1, pending_.end(), pending_.begin());
                    --pendingCount_;
                }
                // Insert in time order; equal times keep their order (note-off before note-on).
                int k = pendingCount_;
                while (k > 0 && pending_[static_cast<std::size_t> (k - 1)].dueMs > p.dueMs)
                {
                    pending_[static_cast<std::size_t> (k)] =
                        pending_[static_cast<std::size_t> (k - 1)];
                    --k;
                }
                pending_[static_cast<std::size_t> (k)] = p;
                ++pendingCount_;
            }
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        int sent = 0;
        while (sent < pendingCount_ &&
               pending_[static_cast<std::size_t> (sent)].dueMs <= now + 0.25)
            send (pending_[static_cast<std::size_t> (sent++)]);
        if (sent > 0)
        {
            std::move (pending_.begin() + sent, pending_.begin() + pendingCount_, pending_.begin());
            pendingCount_ -= sent;
        }
        wait (1);
    }
    flushPending();
    endAllNotes();
}

void MidiPortRouter::flushPending()
{
    // What was decided is sent, so its note-offs pair with its note-ons.
    for (int i = 0; i < pendingCount_; ++i)
        send (pending_[static_cast<std::size_t> (i)]);
    pendingCount_ = 0;
}

void MidiPortRouter::swapIn()
{
    std::unique_ptr<Output> next;
    juce::String status;
    {
        const std::scoped_lock lock (lock_);
        if (!swapPending_)
            return;
        swapPending_ = false;
        next = std::move (incoming_);
        status = incomingStatus_;
    }
    if (output_ != nullptr)
    {
        flushPending();
        endAllNotes();
    }
    active_.store (false, std::memory_order_relaxed);
    retire (std::move (output_));
    output_ = std::move (next);
    // Events queued for the old destination are dropped.
    std::array<Packet, 256> drop {};
    while (queue_.pop (drop.data(), drop.size()) > 0)
    {
    }
    active_.store (output_ != nullptr, std::memory_order_relaxed);

    const std::scoped_lock lock (lock_);
    status_ = status;
    openName_ = output_ != nullptr ? output_->name() : juce::String();
}

void MidiPortRouter::send (const Packet& p)
{
    if (output_ == nullptr || p.size == 0)
        return;
    const midi::Event e = [&p] {
        midi::Event ev;
        std::copy (p.bytes, p.bytes + 3, ev.bytes);
        ev.size = p.size;
        return ev;
    }();
    const auto ch = static_cast<std::size_t> (e.channel() - 1);
    if (e.isNoteOn())
    {
        on_[ch].set (static_cast<std::size_t> (e.note()));
        channelsUsed_.set (ch);
    }
    else if (e.isNoteOff())
    {
        // A note-off for a note this router never started here (it began on another
        // destination) is not sent.
        if (!on_[ch].test (static_cast<std::size_t> (e.note())))
            return;
        on_[ch].reset (static_cast<std::size_t> (e.note()));
    }
    else if (e.isController (midi::kAllNotesOff) || e.isController (midi::kAllSoundOff))
    {
        on_[ch].reset();
    }
    output_->send (juce::MidiMessage (p.bytes, p.size));
}

void MidiPortRouter::endAllNotes()
{
    if (output_ == nullptr)
        return;
    for (std::size_t ch = 0; ch < 16; ++ch)
    {
        for (std::size_t n = 0; n < 128; ++n)
            if (on_[ch].test (n))
                output_->send (
                    juce::MidiMessage::noteOff (static_cast<int> (ch) + 1, static_cast<int> (n)));
        // On a port of its own, All Notes Off on every channel it used catches anything a
        // receiver still holds; a shared port may carry another instance's notes.
        if (!output_->shared() && channelsUsed_.test (ch))
            output_->send (juce::MidiMessage::allNotesOff (static_cast<int> (ch) + 1));
        on_[ch].reset();
    }
    channelsUsed_.reset();
}

} // namespace sedef::midiplugin
