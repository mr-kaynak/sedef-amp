// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Recorder.h"

#include "PresetManager.h"

#include <cmath>

#ifndef SEDEF_VERSION
#define SEDEF_VERSION "dev"
#endif

namespace sedef::plugin
{

using record::TakeRecorder;

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

double toDb (float linear)
{
    return 20.0 * std::log10 (std::max (static_cast<double> (linear), 1.0e-6));
}

const char* statusName (Recorder::Status s)
{
    switch (s)
    {
    case Recorder::Status::starting:
        return "starting";
    case Recorder::Status::recording:
        return "recording";
    case Recorder::Status::saving:
        return "saving";
    case Recorder::Status::idle:
        break;
    }
    return "idle";
}
} // namespace

// -------------------------------------------------------------------------------------------------

FileSink::FileSink (const juce::File& file)
    : stream_ (std::make_unique<juce::FileOutputStream> (file))
{
}

bool FileSink::writeAt (std::uint64_t offset, const void* data, std::size_t bytes)
{
    if (!openedOk())
        return false;
    if (static_cast<std::uint64_t> (stream_->getPosition()) != offset &&
        !stream_->setPosition (static_cast<juce::int64> (offset)))
        return false;
    return stream_->write (data, bytes) && stream_->getStatus().wasOk();
}

bool FileSink::flush()
{
    if (!openedOk())
        return false;
    stream_->flush();
    return stream_->getStatus().wasOk();
}

// -------------------------------------------------------------------------------------------------

Recorder::Recorder (TakeRecorder& core) : juce::Thread ("Sedef recorder"), core_ (core) {}

Recorder::~Recorder()
{
    finishNow();
}

juce::File Recorder::folder()
{
#if SEDEF_PLUGIN_TESTS
    if (const auto root = PresetManager::testRoot(); root != juce::File())
        return root.getChildFile ("Recordings");
#endif
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
        .getChildFile ("Sedef Amp")
        .getChildFile ("Recordings");
}

void Recorder::toggle (double sampleRate, int diChannels, int outChannels)
{
    {
        const std::scoped_lock lock (commandLock_);
        switch (status())
        {
        case Status::idle:
            if (startWanted_)
            {
                startWanted_ = false; // a second press before it started: never mind
                break;
            }
            startWanted_ = true;
            wanted_ = {};
            wanted_.sampleRate = sampleRate;
            wanted_.diChannels = diChannels;
            wanted_.outChannels = outChannels;
            break;
        case Status::starting:
        case Status::recording:
            stopWanted_ = true;
            status_.store (Status::saving);
            ++revision;
            break;
        case Status::saving:
            break;
        }
    }
#if !SEDEF_PLUGIN_TESTS // the tests drive the writer by hand (step)
    if (!isThreadRunning())
        startThread();
    notify();
#endif
}

void Recorder::audioStopped()
{
    core_.finishWithoutAudio();
    notify();
}

void Recorder::finishNow()
{
    stopThread (4000);
    // The audio callback no longer runs once its processor is going away or has been released.
    core_.finishWithoutAudio();
    if (files_ != nullptr)
        complete();
}

void Recorder::run()
{
    while (!threadShouldExit())
    {
        step();
        wait (20);
    }
}

void Recorder::setError (const juce::String& message)
{
    {
        const std::scoped_lock lock (stateLock_);
        error_ = message;
    }
    ++revision;
}

void Recorder::begin (const TakeRecorder::Config& wanted)
{
    const auto fail = [this] (const juce::String& message) {
        files_.reset();
        diSink_.reset();
        outSink_.reset();
        status_.store (Status::idle);
        setError (message);
    };

    const auto dir = folder();
    if (!dir.createDirectory())
    {
        fail ("Could not create the folder " + dir.getFullPathName() + ".");
        return;
    }
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    juce::String name;
    juce::File di, output;
    for (int i = 1;; ++i)
    {
        name = i == 1 ? stamp : stamp + " (" + juce::String (i) + ")";
        di = dir.getChildFile (name + " DI.wav");
        output = dir.getChildFile (name + " Output.wav");
        if (!di.exists() && !output.exists())
            break;
    }

    auto config = wanted;
    config.maxFrames = record::TakeFiles::maxFrames (config);
    diSink_ = std::make_unique<FileSink> (di);
    outSink_ = std::make_unique<FileSink> (output);
    files_ = std::make_unique<record::TakeFiles> (*diSink_, *outSink_, config);
    if (!diSink_->openedOk() || !outSink_->openedOk() || !files_->begin())
    {
        fail ("Could not write to " + dir.getFullPathName() + ".");
        di.deleteFile();
        output.deleteFile();
        return;
    }
    if (!core_.start (config))
    {
        fail ("The previous take is still being saved.");
        di.deleteFile();
        output.deleteFile();
        return;
    }
    writeFailed_ = false;
    sinceHeader_ = 0;
    rate_ = config.sampleRate;
    {
        const std::scoped_lock lock (stateLock_);
        take_ = { name, di, output, 0.0, 0, 0.0, {} };
        hasTake_ = true;
        error_ = {};
    }
    status_.store (Status::starting);
    ++revision;
}

void Recorder::step()
{
    bool start = false, stop = false;
    TakeRecorder::Config config;
    {
        const std::scoped_lock lock (commandLock_);
        start = std::exchange (startWanted_, false);
        stop = std::exchange (stopWanted_, false);
        config = wanted_;
    }
    if (start && files_ == nullptr && core_.phase() == TakeRecorder::Phase::idle)
    {
        status_.store (Status::starting);
        begin (config);
    }
    if (stop)
        core_.requestStop();
    if (files_ == nullptr)
        return;

    core_.drain ([this] (const record::TakeChunk& chunk) {
        if (!writeFailed_ && !files_->write (chunk))
            writeFailed_ = true;
    });
    if (writeFailed_)
        core_.requestStop(); // keep the chunks flowing back; the files end where the disk did

    const auto phase = core_.phase();
    auto starting = Status::starting; // (unless Stop was pressed meanwhile)
    if (phase == TakeRecorder::Phase::recording &&
        status_.compare_exchange_strong (starting, Status::recording))
        ++revision;

    // The sizes in the headers, every second, so a crash leaves files that open.
    const auto frames = core_.frames();
    if (!writeFailed_ && static_cast<double> (frames - sinceHeader_) >= rate_)
    {
        sinceHeader_ = frames;
        if (!files_->updateHeaders())
            writeFailed_ = true;
    }

    if (phase == TakeRecorder::Phase::finished)
        complete();
}

void Recorder::complete()
{
    core_.drain ([this] (const record::TakeChunk& chunk) {
        if (!writeFailed_ && !files_->write (chunk))
            writeFailed_ = true;
    });

    Take take;
    {
        const std::scoped_lock lock (stateLock_);
        take = take_;
    }
    const auto frames = core_.frames();
    record::TakeInfo info;
    info.diName = take.di.getFileName().toStdString();
    info.outputName = take.output.getFileName().toStdString();
    info.date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d").toStdString();
    info.software = std::string ("Sedef Amp ") + SEDEF_VERSION;
    if (!writeFailed_ && !files_->finish (core_, info))
        writeFailed_ = true;
    files_.reset();
    diSink_.reset();
    outSink_.reset();

    switch (core_.endReason())
    {
    case TakeRecorder::EndReason::sizeLimit:
        take.ended = "sizeLimit";
        break;
    case TakeRecorder::EndReason::audioStopped:
        take.ended = "audioStopped";
        break;
    case TakeRecorder::EndReason::none:
    case TakeRecorder::EndReason::stopped:
        take.ended = "stopped";
        break;
    }
    if (writeFailed_)
        take.ended = "writeError";
    if (frames == 0)
    {
        // Stopped before a single block: no files worth keeping.
        take.di.deleteFile();
        take.output.deleteFile();
        take.ended = "empty";
    }
    take.seconds = static_cast<double> (frames) / rate_;
    take.dropouts = core_.droppedBlocks();
    take.droppedSeconds =
        static_cast<double> (std::max (core_.droppedFrames (TakeRecorder::kDi),
                                       core_.droppedFrames (TakeRecorder::kOutput))) /
        rate_;
    {
        const std::scoped_lock lock (stateLock_);
        take_ = take;
    }
    core_.release();
    status_.store (Status::idle);
    ++revision;
    if (writeFailed_)
        setError ("Could not write to " + folder().getFullPathName() +
                  ". The disk may be full or gone; the take is kept up to where it stopped.");
}

juce::var Recorder::stateAsVar() const
{
    const std::scoped_lock lock (stateLock_);
    juce::var take;
    if (hasTake_)
        take = object ({ { "name", take_.name },
                         { "diPath", take_.di.getFullPathName() },
                         { "outputPath", take_.output.getFullPathName() },
                         { "seconds", take_.seconds },
                         { "dropouts", static_cast<juce::int64> (take_.dropouts) },
                         { "droppedSeconds", take_.droppedSeconds },
                         { "ended", take_.ended } });
    return object ({ { "status", statusName (status()) },
                     { "folder", folder().getFullPathName() },
                     { "take", take },
                     { "error", error_.isEmpty() ? juce::var() : juce::var (error_) } });
}

juce::var Recorder::live()
{
    const auto s = status();
    const bool active = s != Status::idle;
    const float di = core_.takePeak (TakeRecorder::kDi);
    const float out = core_.takePeak (TakeRecorder::kOutput);
    return object ({ { "status", statusName (s) },
                     { "seconds", active ? static_cast<double> (core_.frames()) / rate_ : 0.0 },
                     { "diDb", toDb (di) },
                     { "outputDb", toDb (out) },
                     { "dropouts", active ? static_cast<juce::int64> (core_.droppedBlocks())
                                          : static_cast<juce::int64> (0) } });
}

} // namespace sedef::plugin
