// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Reamp.h"

#include "Recorder.h"
#include "sedef/record/RateConverter.h"
#include "sedef/record/WavFloatWriter.h"

#include <cmath>

namespace sedef::plugin
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

bool isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;aif;aiff;flac");
}

/// A decoded file through one of JUCE's readers.
class ReaderSource final : public record::FrameSource
{
public:
    explicit ReaderSource (std::unique_ptr<juce::AudioFormatReader> reader)
        : reader_ (std::move (reader))
    {
    }
    int channels() const override { return static_cast<int> (reader_->numChannels); }
    std::int64_t length() const override { return reader_->lengthInSamples; }
    double sampleRate() const override { return reader_->sampleRate; }
    bool read (float* const* dest, std::int64_t start, int frames) override
    {
        return reader_->read (dest, channels(), start, frames);
    }

private:
    std::unique_ptr<juce::AudioFormatReader> reader_;
};
} // namespace

Reamp::Reamp (record::ReampPlayer& player) : juce::Thread ("Sedef re-amp"), player_ (player)
{
    formats_.registerBasicFormats();
    listFormats_.registerBasicFormats();
}

Reamp::~Reamp()
{
    stopThread (4000);
    unload();
    if (import_ != nullptr)
    {
        import_.reset();
        importFile_.deleteFile();
    }
}

juce::File Reamp::importFolder()
{
    return Recorder::folder().getChildFile ("Imported");
}

void Reamp::ensureThread()
{
#if !SEDEF_PLUGIN_TESTS // the tests drive the reader by hand (runOnce)
    if (!isThreadRunning())
        startThread();
    notify();
#endif
}

void Reamp::prepare (double sampleRate, int channels, int maxBlock)
{
    bool reload = false;
    {
        const std::scoped_lock feed (feedLock_);
        feeder_.setSource (player_, nullptr);
        player_.prepare (sampleRate, channels, maxBlock);
        const std::scoped_lock lock (stateLock_);
        player_.setMix (mix_);
        deviceRate_ = sampleRate;
        if (source_ != nullptr && std::llround (source_->sampleRate()) != std::llround (sampleRate))
        {
            // Converted for another rate: convert again.
            reload = true;
            wantLoad_ = file_;
            loadWanted_ = true;
            status_ = Status::loading;
        }
        else if (source_ != nullptr)
        {
            feeder_.setSource (player_, source_.get());
            feeder_.setLoop (player_, loop_);
        }
    }
    if (reload)
    {
        ++cancelLoad_;
        bump();
        ensureThread();
    }
}

void Reamp::load (const juce::File& file)
{
    {
        const std::scoped_lock lock (stateLock_);
        wantLoad_ = file;
        loadWanted_ = true;
        playWanted_ = false;
        status_ = Status::loading;
        error_ = {};
    }
    ++cancelLoad_;
    bump();
    ensureThread();
}

void Reamp::play()
{
    {
        const std::scoped_lock lock (stateLock_);
        playWanted_ = true;
        stopWanted_ = false;
    }
    ensureThread();
}

void Reamp::stop()
{
    {
        const std::scoped_lock lock (stateLock_);
        stopWanted_ = true;
        playWanted_ = false;
    }
    ensureThread();
}

void Reamp::setLoop (bool loop)
{
    {
        const std::scoped_lock lock (stateLock_);
        loop_ = loop;
        loopChanged_ = true;
    }
    bump();
    ensureThread();
}

void Reamp::setMix (bool mix)
{
    {
        const std::scoped_lock lock (stateLock_);
        mix_ = mix;
    }
    player_.setMix (mix);
    bump();
}

bool Reamp::mayLoad (const juce::File& file) const
{
    return isAudioFile (file) && file.isAChildOf (Recorder::folder()) && file.existsAsFile();
}

void Reamp::setError (const juce::String& message)
{
    {
        const std::scoped_lock lock (stateLock_);
        error_ = message;
        status_ = Status::error;
    }
    bump();
}

// Reader thread ----------------------------------------------------------------------------------

void Reamp::run()
{
    while (!threadShouldExit())
    {
        runOnce();
        wait (playing_.load() ? 10 : 40);
    }
}

void Reamp::runOnce()
{
    juce::File toLoad;
    bool load = false, play = false, stop = false, loopChanged = false, loop = true;
    {
        const std::scoped_lock lock (stateLock_);
        load = std::exchange (loadWanted_, false);
        toLoad = wantLoad_;
        play = std::exchange (playWanted_, false);
        stop = std::exchange (stopWanted_, false);
        loopChanged = std::exchange (loopChanged_, false);
        loop = loop_;
    }
    if (load)
        openFile (toLoad);

    bool ok = true;
    bool active = false;
    {
        const std::scoped_lock feed (feedLock_);
        if (loopChanged)
            feeder_.setLoop (player_, loop);
        if (stop)
            feeder_.stop (player_);
        if (play && source_ != nullptr)
            feeder_.play (player_);
        ok = feeder_.pump (player_);
        active = feeder_.state() != record::ReampFeeder::State::idle;
    }
    if (!ok)
        setError ("Could not read the file any more. Was it moved or deleted?");
    if (playing_.exchange (active) != active)
        bump();
}

void Reamp::unload()
{
    {
        const std::scoped_lock feed (feedLock_);
        feeder_.setSource (player_, nullptr);
        source_.reset();
    }
    if (converted_ != juce::File())
    {
        converted_.deleteFile();
        converted_ = juce::File();
    }
}

void Reamp::openFile (const juce::File& file)
{
    const int request = cancelLoad_.load();
    unload();
    {
        const std::scoped_lock lock (stateLock_);
        file_ = juce::File();
        fileFrames_ = playFrames_ = 0;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats_.createReaderFor (file));
    if (reader == nullptr)
    {
        setError ("Sedef can’t read " + file.getFileName() + ". Choose a WAV, AIFF or FLAC file.");
        return;
    }
    const int channels = static_cast<int> (reader->numChannels);
    const double fileRate = reader->sampleRate;
    const juce::int64 frames = reader->lengthInSamples;
    if (channels < 1 || channels > 2)
    {
        setError ("Re-amp plays mono and stereo files; " + file.getFileName() + " has " +
                  juce::String (channels) + " channels.");
        return;
    }
    if (frames <= 0 || fileRate <= 0.0)
    {
        setError (file.getFileName() + " has no audio in it.");
        return;
    }

    double deviceRate = 48000.0;
    {
        const std::scoped_lock lock (stateLock_);
        deviceRate = deviceRate_;
    }
    if (std::llround (fileRate) != std::llround (deviceRate))
    {
        {
            const std::scoped_lock lock (stateLock_);
            status_ = Status::converting;
            progress_ = 0.0;
            file_ = file;
            fileRate_ = fileRate;
            fileChannels_ = channels;
            fileFrames_ = frames;
        }
        bump();
        converted_ = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile ("Sedef Amp re-amp", ".wav", false);
        bool converted = false;
        {
            FileSink sink (converted_);
            record::WavFloatWriter writer (sink, channels, deviceRate);
            ReaderSource source (std::move (reader));
            std::vector<float> interleaved;
            std::int64_t at = 0;
            int lastStep = 0;
            converted = sink.openedOk() && writer.begin() &&
                        record::RateConverter::convert (
                            source, deviceRate,
                            [&] (const float* const* planar, int n) {
                                interleaved.resize (static_cast<std::size_t> (n * channels));
                                for (int i = 0; i < n; ++i)
                                    for (int c = 0; c < channels; ++c)
                                        interleaved[static_cast<std::size_t> (i * channels + c)] =
                                            planar[c][i];
                                const bool written = writer.write (at, interleaved.data(), n);
                                at += n;
                                return written;
                            },
                            [&] (double fraction) {
                                const int step = static_cast<int> (fraction * 20.0);
                                if (step != lastStep)
                                {
                                    lastStep = step;
                                    {
                                        const std::scoped_lock lock (stateLock_);
                                        progress_ = fraction;
                                    }
                                    bump();
                                }
                                return !threadShouldExit() && cancelLoad_.load() == request;
                            }) &&
                        writer.finish ({});
        }
        if (cancelLoad_.load() != request || threadShouldExit())
            return; // another file was asked for meanwhile
        if (!converted || (reader = std::unique_ptr<juce::AudioFormatReader> (
                               formats_.createReaderFor (converted_))) == nullptr)
        {
            setError ("Could not convert " + file.getFileName() + " to " +
                      juce::String (deviceRate / 1000.0, 1) + " kHz. Is the disk full?");
            return;
        }
    }

    const auto playFrames = reader->lengthInSamples;
    bool loop = true;
    {
        const std::scoped_lock lock (stateLock_);
        loop = loop_;
    }
    {
        const std::scoped_lock feed (feedLock_);
        source_ = std::make_unique<ReaderSource> (std::move (reader));
        feeder_.setSource (player_, source_.get());
        feeder_.setLoop (player_, loop);
    }
    {
        const std::scoped_lock lock (stateLock_);
        status_ = Status::ready;
        file_ = file;
        fileRate_ = fileRate;
        fileChannels_ = channels;
        fileFrames_ = frames;
        playFrames_ = playFrames;
        progress_ = 1.0;
        error_ = {};
    }
    bump();
}

// Drops ------------------------------------------------------------------------------------------

int Reamp::importBegin (const juce::String& fileName, juce::int64 size)
{
    if (import_ != nullptr)
        importCancel (importToken_);
    const auto name = juce::File::createLegalFileName (fileName.trim());
    if (!isAudioFile (juce::File::getCurrentWorkingDirectory().getChildFile (name)))
    {
        setError ("Drop a WAV, AIFF or FLAC file to re-amp it.");
        return 0;
    }
    if (size <= 0 || size > kMaxImportBytes)
    {
        setError (size <= 0 ? "That file is empty."
                            : "That file is over 2 GB. Use Open file… for files this large.");
        return 0;
    }
    const auto dir = importFolder();
    if (!dir.createDirectory())
    {
        setError ("Could not create the folder " + dir.getFullPathName() + ".");
        return 0;
    }
    const auto dot = name.lastIndexOfChar ('.');
    importFile_ =
        dir.getNonexistentChildFile (name.substring (0, dot), name.substring (dot), false);
    import_ = std::make_unique<juce::FileOutputStream> (importFile_);
    if (!import_->openedOk())
    {
        import_.reset();
        setError ("Could not write to " + dir.getFullPathName() + ".");
        return 0;
    }
    importExpected_ = size;
    importToken_ = juce::Random::getSystemRandom().nextInt ({ 1, 1 << 30 });
    {
        const std::scoped_lock lock (stateLock_);
        status_ = Status::loading;
        error_ = {};
    }
    bump();
    return importToken_;
}

bool Reamp::importChunk (int token, const juce::String& base64)
{
    if (import_ == nullptr || token != importToken_)
        return false;
    juce::MemoryOutputStream bytes;
    if (!juce::Base64::convertFromBase64 (bytes, base64) ||
        import_->getPosition() + static_cast<juce::int64> (bytes.getDataSize()) > importExpected_ ||
        !import_->write (bytes.getData(), bytes.getDataSize()))
    {
        importCancel (token);
        setError ("The dropped file did not arrive whole. Try again, or use Open file….");
        return false;
    }
    return true;
}

bool Reamp::importEnd (int token)
{
    if (import_ == nullptr || token != importToken_)
        return false;
    import_->flush();
    const bool whole = import_->getPosition() == importExpected_ && import_->getStatus().wasOk();
    import_.reset();
    importToken_ = 0;
    if (!whole)
    {
        importFile_.deleteFile();
        setError ("The dropped file did not arrive whole. Try again, or use Open file….");
        return false;
    }
    load (importFile_);
    return true;
}

void Reamp::importCancel (int token)
{
    if (import_ == nullptr || token != importToken_)
        return;
    import_.reset();
    importFile_.deleteFile();
    importToken_ = 0;
}

// State ------------------------------------------------------------------------------------------

juce::var Reamp::stateAsVar() const
{
    const std::scoped_lock lock (stateLock_);
    static constexpr const char* kStatus[] = { "empty", "loading", "converting", "ready", "error" };
    juce::var file;
    if (file_ != juce::File() && (status_ == Status::ready || status_ == Status::converting))
        file =
            object ({ { "name", file_.getFileName() },
                      { "path", file_.getFullPathName() },
                      { "seconds", static_cast<double> (fileFrames_) / std::max (1.0, fileRate_) },
                      { "sampleRate", fileRate_ },
                      { "channels", fileChannels_ },
                      { "converted", std::llround (fileRate_) != std::llround (deviceRate_) } });
    return object ({ { "status", kStatus[static_cast<int> (status_)] },
                     { "file", file },
                     { "progress", progress_ },
                     { "loop", loop_ },
                     { "mix", mix_ },
                     { "playing", playing_.load() },
                     { "error", error_.isEmpty() ? juce::var() : juce::var (error_) } });
}

juce::var Reamp::live() const
{
    double length = 0.0, position = 0.0;
    {
        const std::scoped_lock lock (stateLock_);
        if (playFrames_ > 0)
        {
            length = static_cast<double> (playFrames_) / deviceRate_;
            position = static_cast<double> (player_.consumed() % playFrames_) / deviceRate_;
        }
    }
    return object ({ { "playing", playing_.load() && player_.playing() },
                     { "seconds", position },
                     { "length", length },
                     { "gaps", static_cast<juce::int64> (player_.underruns()) } });
}

juce::var Reamp::files()
{
    juce::Array<juce::File> found;
    for (const auto& f :
         Recorder::folder().findChildFiles (juce::File::findFiles, false, "* DI.wav"))
        found.add (f);
    for (const auto& f : importFolder().findChildFiles (juce::File::findFiles, false, "*"))
        if (isAudioFile (f))
            found.add (f);
    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });

    juce::Array<juce::var> list;
    for (const auto& f : found)
    {
        if (list.size() >= kMaxTakes)
            break;
        double seconds = 0.0;
        if (std::unique_ptr<juce::AudioFormatReader> r (listFormats_.createReaderFor (f));
            r != nullptr)
            seconds = static_cast<double> (r->lengthInSamples) / std::max (1.0, r->sampleRate);
        const bool take = !f.isAChildOf (importFolder());
        auto name = f.getFileNameWithoutExtension();
        if (take && name.endsWith (" DI"))
            name = name.dropLastCharacters (3);
        list.add (object ({ { "name", name },
                            { "path", f.getFullPathName() },
                            { "seconds", seconds },
                            { "kind", take ? "take" : "imported" } }));
    }
    return list;
}

} // namespace sedef::plugin
