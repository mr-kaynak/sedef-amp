// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sedef-host-smoke: loads the built VST3 the way a DAW does, opens its editor in a window and
// waits for the editor's UI smoke test (SEDEF_UI_SMOKE_TEST) to report. No audio device is opened,
// so it also runs on machines and CI runners without working audio.
//
//   sedef-host-smoke "<path>/Sedef Amp.vst3"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
class HostSmokeApp final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "sedef-host-smoke"; }
    const juce::String getApplicationVersion() override { return "1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        const auto pluginPath = commandLine.unquoted().trim();
        result_ = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getNonexistentChildFile ("sedef-host-smoke", ".json");
        // The plugin reads the variable from this process's environment.
        setEnv ("SEDEF_UI_SMOKE_TEST", result_.getFullPathName());

        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile (types, pluginPath);
        if (types.isEmpty())
            return fail ("No VST3 plugin found at " + pluginPath);

        juce::String error;
        plugin_ = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
        if (plugin_ == nullptr)
            return fail ("Could not load the plugin: " + error);

        // State round trip: random parameters -> save -> new instance -> load. The second save is
        // taken after the message loop has run for a while (timers, async restores), and both
        // saves must be identical byte for byte.
        auto source = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
        restored_ = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
        if (source == nullptr || restored_ == nullptr)
            return fail ("Could not create instances for the state check: " + error);
        juce::Random random (1234);
        for (auto* param : source->getParameters())
            param->setValueNotifyingHost (random.nextFloat());
        runOneBlock (*source);
        source->getStateInformation (savedState_);
        restored_->setStateInformation (savedState_.getData(),
                                        static_cast<int> (savedState_.getSize()));
        runOneBlock (*restored_);

        plugin_->setPlayConfigDetails (1, 2, 48000.0, 512);
        plugin_->prepareToPlay (48000.0, 512);
        auto* editor = plugin_->createEditorAndMakeActive();
        if (editor == nullptr)
            return fail ("The plugin has no editor");

        window_ = std::make_unique<juce::DocumentWindow> (
            "Sedef host smoke test", juce::Colours::black, juce::DocumentWindow::closeButton);
        window_->setUsingNativeTitleBar (true);
        window_->setContentNonOwned (editor, true);
        window_->centreWithSize (editor->getWidth(), editor->getHeight());
        window_->setVisible (true);

        startTimer (250);
    }

    void shutdown() override
    {
        stopTimer();
        if (window_ != nullptr)
            window_->clearContentComponent();
        if (plugin_ != nullptr)
        {
            delete plugin_->getActiveEditor();
            plugin_->releaseResources();
        }
        window_.reset();
        restored_.reset();
        plugin_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    static void setEnv (const char* name, const juce::String& value)
    {
#if JUCE_WINDOWS
        _putenv_s (name, value.toRawUTF8());
#else
        setenv (name, value.toRawUTF8(), 1);
#endif
    }

    static void runOneBlock (juce::AudioPluginInstance& p)
    {
        p.setPlayConfigDetails (1, 2, 48000.0, 512);
        p.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
    }

    /// Extracts the plugin's own XML from a hosted VST3 state blob, for a readable diff.
    static juce::StringArray stateLines (const juce::MemoryBlock& blob)
    {
        juce::StringArray lines;
        if (const auto outer = juce::parseXML (blob.toString()))
            if (const auto* component = outer->getChildByName ("IComponent"))
            {
                juce::MemoryBlock inner;
                inner.fromBase64Encoding (component->getAllSubText());
                const auto text = inner.toString();
                lines.addLines (text.substring (text.indexOf ("<?xml")));
            }
        return lines;
    }

    bool checkState()
    {
        juce::MemoryBlock again;
        restored_->getStateInformation (again);
        restored_.reset();
        if (again == savedState_)
        {
            std::printf ("state round trip: identical (%d bytes)\n",
                         static_cast<int> (again.getSize()));
            return true;
        }
        const auto a = stateLines (savedState_), b = stateLines (again);
        int shown = 0;
        for (int i = 0; i < juce::jmax (a.size(), b.size()) && shown < 12; ++i)
            if (a[i] != b[i])
            {
                std::fprintf (stderr, "- %s\n+ %s\n", a[i].trim().toRawUTF8(),
                              b[i].trim().toRawUTF8());
                ++shown;
            }
        fail ("The state saved after a reload differs from the state that was loaded");
        return false;
    }

    void timerCallback() override
    {
        ++ticks_;
        if (restored_ != nullptr && ticks_ >= 4 && !checkState())
            return;
        if (result_.existsAsFile() && result_.getSize() > 0)
        {
            const auto text = result_.loadFileAsString();
            std::printf ("%s\n", text.toRawUTF8());
            const bool ok = static_cast<bool> (juce::JSON::parse (text).getProperty ("ok", false));
            result_.deleteFile();
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
        }
        else if (ticks_ > 4 * 90)
        {
            fail ("Timed out waiting for the editor's smoke test result");
        }
    }

    void fail (const juce::String& message)
    {
        std::fprintf (stderr, "sedef-host-smoke: %s\n", message.toRawUTF8());
        setApplicationReturnValue (1);
        quit();
    }

    juce::File result_;
    std::unique_ptr<juce::AudioPluginInstance> plugin_, restored_;
    juce::MemoryBlock savedState_;
    std::unique_ptr<juce::DocumentWindow> window_;
    int ticks_ = 0;
};
} // namespace

START_JUCE_APPLICATION (HostSmokeApp)
