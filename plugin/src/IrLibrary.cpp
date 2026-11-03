// SPDX-License-Identifier: AGPL-3.0-or-later
#include "IrLibrary.h"

#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include <filesystem>
#include <juce_events/juce_events.h>
#include <system_error>

namespace sedef::plugin
{
namespace
{
constexpr double kMaxSeconds = 1.0;
}

juce::String IrLibrary::checkFile (const juce::String& path)
{
    if (path.isEmpty() || !juce::File::isAbsolutePath (path))
        return "The IR file was not found";
#if JUCE_WINDOWS
    const std::filesystem::path p (path.toWideCharPointer());
#else
    const std::filesystem::path p (path.toRawUTF8());
#endif
    std::error_code ec;
    const auto status = std::filesystem::status (p, ec);
    if (ec || !std::filesystem::exists (status))
        return "The IR file was not found";
    if (!std::filesystem::is_regular_file (status))
        return "Unsupported audio file";
    const auto size = std::filesystem::file_size (p, ec);
    if (ec || size == 0)
        return "The file contains no audio";
    if (size > static_cast<std::uintmax_t> (kMaxFileBytes))
        return "The IR file is too large (over 64 MB)";
    return {};
}

IrLibrary::IrLibrary (Engine& engine, std::mutex& engineLock)
    : engine_ (engine),
      engineLock_ (engineLock)
{
    formats_.registerBasicFormats();
}

IrLibrary::~IrLibrary()
{
    worker_.removeAllJobs (true, 5000);
}

std::uint64_t IrLibrary::request (int mic, int cab, const juce::File& file)
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return 0;
    const std::scoped_lock lock (slotsLock_);
    requested_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] = file;
    return ++generation_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
}

void IrLibrary::loadAsync (int mic, int cab, const juce::File& file,
                           std::function<void (const juce::String&)> onDone)
{
    const auto generation = request (mic, cab, file);
    worker_.addJob ([this, mic, cab, file, generation, done = std::move (onDone)] {
        const auto error = loadIfCurrent (mic, cab, file, generation);
        juce::MessageManager::callAsync ([done, error] { done (error); });
    });
}

juce::String IrLibrary::load (int mic, int cab, const juce::File& file)
{
    return loadIfCurrent (mic, cab, file, request (mic, cab, file));
}

juce::String IrLibrary::fail (int mic, int cab, std::uint64_t generation, const juce::String& error)
{
    // A failed load leaves the slot empty, unless a newer request has replaced this one.
    const std::scoped_lock lock (slotsLock_);
    if (generation_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] == generation)
        requested_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] = juce::File();
    return error;
}

juce::String IrLibrary::loadIfCurrent (int mic, int cab, const juce::File& file,
                                       std::uint64_t generation)
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return "Invalid IR slot";
    if (const auto error = checkFile (file.getFullPathName()); error.isNotEmpty())
        return fail (mic, cab, generation, error);

    std::unique_ptr<juce::AudioFormatReader> reader (formats_.createReaderFor (file));
    if (reader == nullptr)
        return fail (mic, cab, generation, "Unsupported audio file");
    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return fail (mic, cab, generation, "The file contains no audio");

    const auto length = static_cast<int> (std::min<juce::int64> (
        reader->lengthInSamples, static_cast<juce::int64> (reader->sampleRate * kMaxSeconds)));
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), length);
    reader->read (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), 0, length);

    // Stereo IRs are folded to mono; Sedef pans each mic itself.
    Slot slot;
    slot.file = file;
    slot.sampleRate = reader->sampleRate;
    slot.samples.assign (static_cast<size_t> (length), 0.0f);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::addWithMultiply (
            slot.samples.data(), buffer.getReadPointer (ch),
            1.0f / static_cast<float> (buffer.getNumChannels()), length);

    const std::scoped_lock lock (slotsLock_);
    if (generation_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] != generation)
        return {}; // superseded by a newer load or clear while decoding
    slots_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] = std::move (slot);
    install (mic, cab);
    return {};
}

void IrLibrary::clear (int mic, int cab)
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return;
    const std::scoped_lock slotLock (slotsLock_);
    request (mic, cab, juce::File());
    slots_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] = {};
    const std::scoped_lock lock (engineLock_);
    for (int c = 0; c < engine_.numCabs(); ++c)
        engine_.cab (c).setCustomIr (mic, cab, nullptr);
}

void IrLibrary::install (int mic, int cab)
{
    const auto& slot = slots_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
    const std::scoped_lock lock (engineLock_);
    for (int c = 0; c < engine_.numCabs(); ++c)
    {
        auto kernel = slot.samples.empty()
                          ? nullptr
                          : dsp::CabSim::makeKernel (
                                slot.samples.data(), static_cast<int> (slot.samples.size()),
                                slot.sampleRate, engine_.sampleRate(), engine_.maxBlockSize());
        engine_.cab (c).setCustomIr (mic, cab, std::move (kernel));
    }
}

void IrLibrary::rebuildAll()
{
    const std::scoped_lock lock (slotsLock_);
    for (int m = 0; m < kMics; ++m)
        for (int c = 0; c < kCabs; ++c)
            if (!slots_[static_cast<size_t> (m)][static_cast<size_t> (c)].samples.empty())
                install (m, c);
}

void IrLibrary::collectGarbage()
{
    const std::scoped_lock lock (engineLock_);
    for (int c = 0; c < engine_.numCabs(); ++c)
        engine_.cab (c).collectGarbage();
}

juce::File IrLibrary::fileFor (int mic, int cab) const
{
    // What the slot should hold: saved state round-trips even while a load is still running.
    const std::scoped_lock lock (slotsLock_);
    return requested_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
}

juce::var IrLibrary::stateAsVar() const
{
    const std::scoped_lock lock (slotsLock_);
    juce::Array<juce::var> list;
    for (int m = 0; m < kMics; ++m)
        for (int c = 0; c < kCabs; ++c)
        {
            const auto& s = slots_[static_cast<size_t> (m)][static_cast<size_t> (c)];
            auto* o = new juce::DynamicObject();
            o->setProperty ("mic", m);
            o->setProperty ("cab", c);
            if (s.samples.empty())
            {
                o->setProperty ("name", juce::var());
                o->setProperty ("durationMs", juce::var());
            }
            else
            {
                o->setProperty ("name", s.file.getFileName());
                o->setProperty ("durationMs",
                                1000.0 * static_cast<double> (s.samples.size()) / s.sampleRate);
            }
            list.add (juce::var (o));
        }
    auto* root = new juce::DynamicObject();
    root->setProperty ("slots", list);
    return juce::var (root);
}

} // namespace sedef::plugin
