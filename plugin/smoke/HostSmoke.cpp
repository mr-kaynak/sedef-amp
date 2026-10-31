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

    void timerCallback() override
    {
        if (result_.existsAsFile() && result_.getSize() > 0)
        {
            const auto text = result_.loadFileAsString();
            std::printf ("%s\n", text.toRawUTF8());
            const bool ok = static_cast<bool> (juce::JSON::parse (text).getProperty ("ok", false));
            result_.deleteFile();
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
        }
        else if (++ticks_ > 4 * 90)
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
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    std::unique_ptr<juce::DocumentWindow> window_;
    int ticks_ = 0;
};
} // namespace

START_JUCE_APPLICATION (HostSmokeApp)
