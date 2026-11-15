// SPDX-License-Identifier: AGPL-3.0-or-later
#include "IrLibrary.h"

#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include <cmath>
#include <filesystem>
#include <juce_events/juce_events.h>
#include <system_error>

namespace sedef::plugin
{
namespace
{
constexpr double kMaxSeconds = 1.0;

juce::String rateText (double rate)
{
    return std::isfinite (rate) && rate >= 0.0 && rate < 1.0e12
               ? juce::String (static_cast<juce::int64> (std::llround (rate))) + " Hz"
               : juce::String ("an impossible rate");
}
} // namespace

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
                           std::function<void (const juce::String&)> onDone, Origin origin)
{
    const auto previous = fileFor (mic, cab);
    const auto generation = request (mic, cab, file);
    worker_.addJob (
        [this, mic, cab, file, generation, origin, previous, done = std::move (onDone)] {
            const auto error = loadIfCurrent (mic, cab, file, generation, origin, previous);
            juce::MessageManager::callAsync ([done, error] { done (error); });
        });
}

juce::String IrLibrary::load (int mic, int cab, const juce::File& file, Origin origin)
{
    const auto previous = fileFor (mic, cab);
    return loadIfCurrent (mic, cab, file, request (mic, cab, file), origin, previous);
}

void IrLibrary::changed (int mic, int cab)
{
    ++version_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
}

juce::String IrLibrary::fail (int mic, int cab, std::uint64_t generation, Origin origin,
                              const juce::File& previous, const juce::String& error)
{
    const auto m = static_cast<size_t> (mic);
    const auto c = static_cast<size_t> (cab);
    const std::scoped_lock lock (slotsLock_);
    if (generation_[m][c] != generation)
        return error; // a newer load or clear has replaced this one
    if (origin == Origin::player)
    {
        // The slot stays as it was: the IR that plays keeps playing and stays in the session.
        requested_[m][c] = previous;
        return error;
    }
    // A session's IR that can't be read: the path stays (the next save writes it again) and
    // nothing plays, so the slot shows it as missing (docs/robustness.md, section 4).
    slots_[m][c] = {};
    missing_[m][c] = error;
    changed (mic, cab);
    const std::scoped_lock engineLock (engineLock_);
    for (int i = 0; i < engine_.numCabs(); ++i)
        engine_.cab (i).setCustomIr (mic, cab, nullptr);
    installedRate_[m][c] = 0.0;
    return error;
}

juce::String IrLibrary::loadIfCurrent (int mic, int cab, const juce::File& file,
                                       std::uint64_t generation, Origin origin,
                                       const juce::File& previous)
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return "Invalid IR slot";
    const auto failWith = [&] (const juce::String& error) {
        return fail (mic, cab, generation, origin, previous, error);
    };
    if (const auto error = checkFile (file.getFullPathName()); error.isNotEmpty())
        return failWith (error);

    std::unique_ptr<juce::AudioFormatReader> reader (formats_.createReaderFor (file));
    if (reader == nullptr)
        return failWith ("Unsupported audio file");
    // A header can claim any rate and any channel count: refused before anything is read or
    // resampled (docs/robustness.md, section 5).
    if (!dsp::ConvolutionKernel::isSupportedIrRate (reader->sampleRate))
        return failWith (file.getFileName() + " says it was recorded at " +
                         rateText (reader->sampleRate) +
                         ". Sedef reads IRs recorded at 8 to 384 kHz.");
    const int channels = static_cast<int> (reader->numChannels);
    if (channels < 1 || channels > kMaxChannels)
        return failWith (file.getFileName() + " has " + juce::String (channels) +
                         " channels. Sedef reads IRs with up to " + juce::String (kMaxChannels) +
                         ".");
    if (reader->lengthInSamples <= 0)
        return failWith ("The file contains no audio");

    // At most one second at a rate of at most 384 kHz.
    const auto length = static_cast<int> (std::min<juce::int64> (
        reader->lengthInSamples,
        static_cast<juce::int64> (std::ceil (reader->sampleRate * kMaxSeconds))));
    juce::AudioBuffer<float> buffer (channels, length);
    reader->read (buffer.getArrayOfWritePointers(), channels, 0, length);

    // Stereo IRs are folded to mono; Sedef pans each mic itself.
    auto samples = std::make_shared<std::vector<float>> (static_cast<size_t> (length), 0.0f);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::addWithMultiply (samples->data(), buffer.getReadPointer (ch),
                                                      1.0f / static_cast<float> (channels), length);
    {
        const std::scoped_lock lock (slotsLock_);
        if (generation_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] != generation)
            return {}; // superseded by a newer load or clear while decoding
        auto& slot = slots_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
        slot.file = file;
        slot.samples = std::move (samples);
        slot.sampleRate = reader->sampleRate;
        missing_[static_cast<size_t> (mic)][static_cast<size_t> (cab)] = {};
        changed (mic, cab);
    }
    install (mic, cab);
    return {};
}

void IrLibrary::clear (int mic, int cab)
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return;
    const auto m = static_cast<size_t> (mic);
    const auto c = static_cast<size_t> (cab);
    const std::scoped_lock slotLock (slotsLock_);
    request (mic, cab, juce::File());
    slots_[m][c] = {};
    missing_[m][c] = {};
    changed (mic, cab);
    const std::scoped_lock lock (engineLock_);
    for (int i = 0; i < engine_.numCabs(); ++i)
        engine_.cab (i).setCustomIr (mic, cab, nullptr);
    installedRate_[m][c] = 0.0;
}

void IrLibrary::install (int mic, int cab)
{
    const auto m = static_cast<size_t> (mic);
    const auto c = static_cast<size_t> (cab);
    // Kernels are built outside the engine lock, which prepareToPlay and the message thread's
    // timer wait on (docs/robustness.md, section 5). If the engine is prepared again meanwhile,
    // they are built again for its new rate; after a few rounds the rebuild that follows every
    // prepare takes over.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        std::shared_ptr<const std::vector<float>> samples;
        double irRate = 0.0;
        std::uint64_t version = 0;
        {
            const std::scoped_lock lock (slotsLock_);
            samples = slots_[m][c].samples;
            irRate = slots_[m][c].sampleRate;
            version = version_[m][c];
        }
        if (samples == nullptr)
            return;
        double rate = 0.0;
        int block = 0, cabs = 0;
        {
            const std::scoped_lock lock (engineLock_);
            rate = engine_.sampleRate();
            block = engine_.maxBlockSize();
            cabs = engine_.numCabs();
        }
        if (whileBuilding)
            whileBuilding();
        std::vector<std::unique_ptr<dsp::ConvolutionKernel>> kernels;
        for (int i = 0; i < cabs; ++i)
            kernels.push_back (dsp::CabSim::makeKernel (
                samples->data(), static_cast<int> (samples->size()), irRate, rate, block));

        const std::scoped_lock slotLock (slotsLock_);
        if (version_[m][c] != version)
            return; // a newer load or clear took the slot over
        const std::scoped_lock lock (engineLock_);
        if (!juce::exactlyEqual (engine_.sampleRate(), rate) || engine_.maxBlockSize() != block ||
            engine_.numCabs() != cabs)
            continue; // prepared again while building
        for (int i = 0; i < cabs; ++i)
            engine_.cab (i).setCustomIr (mic, cab, std::move (kernels[static_cast<size_t> (i)]));
        installedRate_[m][c] = rate;
        return;
    }
}

void IrLibrary::rebuildAll()
{
    for (int m = 0; m < kMics; ++m)
        for (int c = 0; c < kCabs; ++c)
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
    // What the slot should hold: saved state round-trips even while a load is still running, and
    // a missing file's path is saved again.
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return {};
    const std::scoped_lock lock (slotsLock_);
    return requested_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
}

juce::String IrLibrary::missingReason (int mic, int cab) const
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return {};
    const std::scoped_lock lock (slotsLock_);
    return missing_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
}

double IrLibrary::installedRate (int mic, int cab) const
{
    if (mic < 0 || mic >= kMics || cab < 0 || cab >= kCabs)
        return 0.0;
    const std::scoped_lock lock (slotsLock_);
    return installedRate_[static_cast<size_t> (mic)][static_cast<size_t> (cab)];
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
            if (s.samples == nullptr)
            {
                o->setProperty ("name", juce::var());
                o->setProperty ("durationMs", juce::var());
            }
            else
            {
                o->setProperty ("name", s.file.getFileName());
                o->setProperty ("durationMs",
                                1000.0 * static_cast<double> (s.samples->size()) / s.sampleRate);
            }
            // A session's IR that couldn't be read (docs/robustness.md, section 4).
            juce::var missing;
            if (const auto& reason = missing_[static_cast<size_t> (m)][static_cast<size_t> (c)];
                reason.isNotEmpty())
            {
                const auto& file = requested_[static_cast<size_t> (m)][static_cast<size_t> (c)];
                auto* info = new juce::DynamicObject();
                info->setProperty ("fileName", file.getFileName());
                info->setProperty ("path", file.getFullPathName());
                info->setProperty ("reason", reason);
                missing = juce::var (info);
            }
            o->setProperty ("missing", missing);
            list.add (juce::var (o));
        }
    auto* root = new juce::DynamicObject();
    root->setProperty ("slots", list);
    return juce::var (root);
}

} // namespace sedef::plugin
