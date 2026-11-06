// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginEditor.h"

#include "ParameterLayout.h"
#include "UiResources.h"

namespace sedef::plugin
{
namespace
{
using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

/// Development: point the editor at a running Vite dev server instead of the embedded build.
juce::String devServerUrl()
{
    return juce::SystemStats::getEnvironmentVariable ("SEDEF_UI_DEV_URL", {});
}

/// CI: when set to a file path, the editor checks that the web UI loaded, writes the result there
/// as JSON and (in the standalone app) quits. See tools/ci/ui-smoke.sh.
juce::String smokeTestPath()
{
    return juce::SystemStats::getEnvironmentVariable ("SEDEF_UI_SMOKE_TEST", {});
}

constexpr int kSmokeTimeoutTicks = 60 * 60; // one minute at the 60 Hz editor timer
constexpr int kSmokeSettleTicks = 60;       // let React render for a second after the handshake

std::optional<ParamId> paramFromVar (const juce::var& v)
{
    const auto s = v.toString().toStdString();
    return findParam (s);
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

float toDb (float linear)
{
    return juce::Decibels::gainToDecibels (linear, -100.0f);
}

const char* const kNoteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
} // namespace

// -------------------------------------------------------------------------------------------------

class SedefEditor::Browser final : public juce::WebBrowserComponent
{
public:
    using WebBrowserComponent::WebBrowserComponent;

    /// Only our own UI may load inside the editor. External links go through sedef_openUrl.
    bool pageAboutToLoad (const juce::String& url) override
    {
        const auto dev = devServerUrl();
        return url.startsWith (getResourceProviderRoot()) ||
               (dev.isNotEmpty() && url.startsWith (dev));
    }
};

SedefEditor::SedefEditor (SedefProcessor& p) : AudioProcessorEditor (&p), processor_ (p)
{
    for (std::size_t i = 0; i < kNumParams; ++i)
        lastSent_[i] = processor_.paramValue (static_cast<ParamId> (i));

    updates_ = std::make_unique<update::Bridge> (
        [this] (const juce::var& state) {
            if (pageReady_)
                browser_->emitEventIfBrowserIsVisible ("sedef:update", state);
        },
        processor_.isStandalone());
    browser_ = std::make_unique<Browser> (makeOptions());
    addAndMakeVisible (*browser_);

    const auto dev = devServerUrl();
    browser_->goToURL (dev.isNotEmpty() ? dev
                                        : juce::WebBrowserComponent::getResourceProviderRoot());

    const double scale = processor_.uiScale(); // read before resize limits trigger resized()
    setResizable (true, true);
    const double ratio =
        static_cast<double> (SedefProcessor::kBaseWidth) / SedefProcessor::kBaseHeight;
    setResizeLimits (SedefProcessor::kBaseWidth * 3 / 4, SedefProcessor::kBaseHeight * 3 / 4,
                     SedefProcessor::kBaseWidth * 2, SedefProcessor::kBaseHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (ratio);
    setSize (juce::roundToInt (SedefProcessor::kBaseWidth * scale),
             juce::roundToInt (SedefProcessor::kBaseHeight * scale));

    if (const auto path = smokeTestPath(); path.isNotEmpty())
        smokeResult_ = juce::File (path);

    startTimerHz (60);
}

SedefEditor::~SedefEditor()
{
    stopTimer();
    processor_.analysis().setActive (false);
}

void SedefEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b1117)); // matches the UI background while the page loads
}

void SedefEditor::resized()
{
    browser_->setBounds (getLocalBounds());
    processor_.setUiScale (getWidth() / static_cast<double> (SedefProcessor::kBaseWidth));
}

// -------------------------------------------------------------------------------------------------

juce::WebBrowserComponent::Options SedefEditor::makeOptions()
{
    using Options = juce::WebBrowserComponent::Options;
    auto& proc = processor_;

    auto options =
        Options {}
            .withBackend (Options::Backend::webview2)
            .withWinWebView2Options (
                Options::WinWebView2 {}
                    .withUserDataFolder (PresetManager::userFolder().getSiblingFile ("WebView2"))
                    .withBackgroundColour (juce::Colour (0xff0b1117))
                    .withStatusBarDisabled())
            .withNativeIntegrationEnabled()
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withResourceProvider (
                [] (const juce::String& url) { return getUiResource (url); },
                devServerUrl().isNotEmpty()
                    ? std::optional<juce::String> (juce::URL (devServerUrl()).getOrigin())
                    : std::nullopt);

    const auto fn = [&options] (const char* name, juce::WebBrowserComponent::NativeFunction f) {
        options = options.withNativeFunction (name, std::move (f));
    };

    fn ("sedef_hello", [this] (const juce::Array<juce::var>&, Completion done) {
        pageReady_ = true;
        done (hello());
    });

    fn ("sedef_setParam", [this, &proc] (const juce::Array<juce::var>& a, Completion done) {
        if (a.size() >= 2)
            if (const auto id = paramFromVar (a[0]))
            {
                proc.setParamValue (*id, spec (*id).sanitise (static_cast<float> (a[1])));
                // Don't echo the UI's own change back. Read the stored value: the normalised round
                // trip through the host can move a skewed parameter by an ULP.
                lastSent_[index (*id)] = proc.paramValue (*id);
            }
        done ({});
    });

    const auto gesture = [&proc] (bool begin) {
        return [&proc, begin] (const juce::Array<juce::var>& a, Completion done) {
            if (!a.isEmpty())
                if (auto* p = proc.state().getParameter (a[0].toString()))
                    begin ? p->beginChangeGesture() : p->endChangeGesture();
            done ({});
        };
    };
    fn ("sedef_beginGesture", gesture (true));
    fn ("sedef_endGesture", gesture (false));

    fn ("sedef_resetParam", [this, &proc] (const juce::Array<juce::var>& a, Completion done) {
        if (!a.isEmpty())
            if (const auto id = paramFromVar (a[0]))
                if (auto* p = proc.state().getParameter (a[0].toString()))
                {
                    // A gesture, so hosts in touch/latch/write automation modes record the reset.
                    p->beginChangeGesture();
                    proc.setParamValue (*id, spec (*id).defaultValue);
                    p->endChangeGesture();
                    lastSent_[index (*id)] = proc.paramValue (*id);
                }
        done ({});
    });

    // Presets -------------------------------------------------------------------------------------
    const auto presetCall = [this, &proc] (auto action) {
        return [this, &proc, action] (const juce::Array<juce::var>& a, Completion done) {
            action (proc.presets(), a);
            proc.presetsChanged(); // also what the host saves, at once
            lastPresetRevision_ = proc.presetRevision.load();
            lastDirty_ = proc.presets().isDirty();
            done (proc.presets().stateAsVar());
        };
    };
    fn ("sedef_presets_list", presetCall ([] (PresetManager& pm, const auto&) { pm.scan(); }));
    fn ("sedef_presets_load", presetCall ([] (PresetManager& pm, const auto& a) {
            if (!a.isEmpty())
                pm.load (a[0].toString(), a.size() > 1 && static_cast<bool> (a[1]));
        }));
    fn ("sedef_presets_markRecent", presetCall ([] (PresetManager& pm, const auto& a) {
            if (!a.isEmpty())
                pm.markRecent (a[0].toString());
        }));
    fn ("sedef_presets_save", presetCall ([] (PresetManager& pm, const auto& a) {
            pm.save (a.size() > 0 ? a[0].toString() : juce::String(),
                     a.size() > 1 ? a[1].toString() : juce::String(),
                     a.size() > 2 ? a[2] : juce::var());
        }));
    fn ("sedef_presets_delete", presetCall ([] (PresetManager& pm, const auto& a) {
            if (!a.isEmpty())
                pm.remove (a[0].toString());
        }));
    fn ("sedef_presets_undoDelete",
        presetCall ([] (PresetManager& pm, const auto&) { pm.undoRemove(); }));
    fn ("sedef_presets_rename", presetCall ([] (PresetManager& pm, const auto& a) {
            if (a.size() >= 2)
                pm.rename (a[0].toString(), a[1].toString());
        }));
    fn ("sedef_presets_setFavorite", presetCall ([] (PresetManager& pm, const auto& a) {
            if (a.size() >= 2)
                pm.setFavorite (a[0].toString(), static_cast<bool> (a[1]));
        }));
    fn ("sedef_presets_step", presetCall ([] (PresetManager& pm, const auto& a) {
            pm.step (a.isEmpty() ? 1 : static_cast<int> (a[0]));
        }));
    fn ("sedef_presets_init", presetCall ([] (PresetManager& pm, const auto&) { pm.loadInit(); }));
    fn ("sedef_presets_snapshot", presetCall ([] (PresetManager& pm, const auto& a) {
            if (const auto slot =
                    PresetManager::slotFromString (a.isEmpty() ? juce::String() : a[0].toString()))
                pm.snapshot (*slot);
        }));
    fn ("sedef_presets_restore", presetCall ([] (PresetManager& pm, const auto& a) {
            if (const auto slot =
                    PresetManager::slotFromString (a.isEmpty() ? juce::String() : a[0].toString()))
                pm.restore (*slot);
        }));
    fn ("sedef_presets_compare", presetCall ([] (PresetManager& pm, const auto& a) {
            const auto action = a.isEmpty() ? juce::String() : a[0].toString();
            if (action == "switch")
                pm.compareSwitch();
            else if (action == "copy")
                pm.compareCopy();
        }));
    fn ("sedef_presets_revealFolder", [] (const juce::Array<juce::var>&, Completion done) {
        auto folder = PresetManager::userFolder();
        folder.createDirectory();
        folder.revealToUser();
        done ({});
    });

    // IRs ---------------------------------------------------------------------------------------
    fn ("sedef_ir_load", [this] (const juce::Array<juce::var>& a, Completion done) {
        loadIr (a.size() > 0 ? static_cast<int> (a[0]) : 0,
                a.size() > 1 ? static_cast<int> (a[1]) : 0, std::move (done));
    });
    fn ("sedef_ir_clear", [&proc] (const juce::Array<juce::var>& a, Completion done) {
        if (a.size() >= 2)
            proc.irs().clear (static_cast<int> (a[0]), static_cast<int> (a[1]));
        ++proc.irRevision;
        done (proc.irs().stateAsVar());
    });

    // Captures (docs/dsp/nam.md) ---------------------------------------------------------------
    // Loads answer once the capture is built (on the library's worker), with the new state.
    const auto captureDone = [this] (Completion completion, bool play) {
        return [safe = juce::Component::SafePointer<SedefEditor> (this),
                done = std::move (completion), play] (const juce::String& error) {
            if (safe == nullptr)
                return;
            if (error.isEmpty() && play)
                safe->processor_.captureLoaded();
            safe->lastCaptureRevision_ = safe->processor_.captures().revision.load();
            done (safe->processor_.captures().stateAsVar());
        };
    };
    fn ("sedef_capture_load", [this] (const juce::Array<juce::var>&, Completion done) {
        loadCapture (std::move (done));
    });
    fn ("sedef_capture_loadData",
        [&proc, captureDone] (const juce::Array<juce::var>& a, Completion done) {
            if (a.size() < 2 || !a[1].isString())
            {
                done (proc.captures().stateAsVar());
                return;
            }
            proc.captures().loadDroppedAsync (a[0].toString(), a[1].toString().toStdString(),
                                              captureDone (std::move (done), true));
        });
    fn ("sedef_capture_loadRecent",
        [&proc, captureDone] (const juce::Array<juce::var>& a, Completion done) {
            // Only a path the recent list offered: the page can't make the plug-in read others.
            const auto path = a.isEmpty() ? juce::String() : a[0].toString();
            bool listed = false;
            const auto state = proc.captures().stateAsVar();
            if (const auto* recent = state["recent"].getArray())
                for (const auto& r : *recent)
                    listed = listed || r["path"].toString() == path;
            if (!listed)
            {
                done (proc.captures().stateAsVar());
                return;
            }
            proc.captures().loadFileAsync (juce::File (path), captureDone (std::move (done), true));
        });
    fn ("sedef_capture_clear", [this, &proc] (const juce::Array<juce::var>&, Completion done) {
        proc.captures().clear();
        proc.changeParamValue (ParamId::ampSource, 0.0f);
        lastCaptureRevision_ = proc.captures().revision.load();
        done (proc.captures().stateAsVar());
    });

    // Tuner, metronome, UI ----------------------------------------------------------------------
    fn ("sedef_tuner_setActive", [&proc] (const juce::Array<juce::var>& a, Completion done) {
        proc.analysis().setActive (!a.isEmpty() && static_cast<bool> (a[0]));
        done ({});
    });
    fn ("sedef_metronome_setPattern", [&proc] (const juce::Array<juce::var>& a, Completion done) {
        std::array<std::uint8_t, 13> steps {};
        if (const auto* list = a.isEmpty() ? nullptr : a[0].getArray())
            for (int i = 0; i < std::min (13, list->size()); ++i)
                steps[static_cast<size_t> (i)] =
                    static_cast<std::uint8_t> (juce::jlimit (0, 3, static_cast<int> ((*list)[i])));
        proc.setMetronomePattern (steps);
        done ({});
    });
    fn ("sedef_metronome_tap",
        [&proc] (const juce::Array<juce::var>&, Completion done) { done (proc.tapTempo()); });
    fn ("sedef_ui_setState", [&proc] (const juce::Array<juce::var>& a, Completion done) {
        if (!a.isEmpty())
            proc.setUiState (a[0]);
        done ({});
    });
    fn ("sedef_ui_setScale", [this] (const juce::Array<juce::var>& a, Completion done) {
        const double scale =
            juce::jlimit (0.75, 2.0, a.isEmpty() ? 1.0 : static_cast<double> (a[0]));
        setSize (juce::roundToInt (SedefProcessor::kBaseWidth * scale),
                 juce::roundToInt (SedefProcessor::kBaseHeight * scale));
        done ({});
    });
    fn ("sedef_openUrl", [] (const juce::Array<juce::var>& a, Completion done) {
        if (!a.isEmpty())
        {
            const juce::URL url (a[0].toString());
            if (url.getScheme() == "https")
                url.launchInDefaultBrowser();
        }
        done ({});
    });
    options = updates_->withNativeFunctions (std::move (options)); // sedef_update_*

    return options;
}

void SedefEditor::loadIr (int mic, int cab, Completion completion)
{
    if (chooser_ !=
        nullptr) // a chooser is already open; answer instead of leaving a promise hanging
    {
        completion (processor_.irs().stateAsVar());
        return;
    }

    chooser_ = std::make_unique<juce::FileChooser> ("Load impulse response", juce::File(),
                                                    "*.wav;*.aif;*.aiff;*.flac");
    chooser_->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<SedefEditor> (this), mic, cab,
         done = std::move (completion)] (const juce::FileChooser& fc) {
            if (safe == nullptr)
                return;
            const auto file = fc.getResult();
            safe->chooser_.reset();
            if (!file.existsAsFile())
            {
                done (safe->processor_.irs().stateAsVar());
                return;
            }
            // Decoding, resampling and FFT partitioning run on the IR library's worker thread.
            auto& irs = safe->processor_.irs();
            irs.loadAsync (mic, cab, file, [safe, done] (const juce::String& error) {
                if (safe == nullptr)
                    return;
                if (error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            "Could not load IR", error);
                ++safe->processor_.irRevision;
                done (safe->processor_.irs().stateAsVar());
            });
        });
}

void SedefEditor::loadCapture (Completion completion)
{
    auto& captures = processor_.captures();
    if (chooser_ != nullptr)
    {
        completion (captures.stateAsVar());
        return;
    }
    chooser_ = std::make_unique<juce::FileChooser> ("Load a NAM capture", juce::File(), "*.nam");
    chooser_->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<SedefEditor> (this),
         done = std::move (completion)] (const juce::FileChooser& fc) {
            if (safe == nullptr)
                return;
            const auto file = fc.getResult();
            safe->chooser_.reset();
            auto& lib = safe->processor_.captures();
            if (!file.existsAsFile())
            {
                done (lib.stateAsVar());
                return;
            }
            lib.loadFileAsync (file, [safe, done] (const juce::String& error) {
                if (safe == nullptr)
                    return;
                if (error.isEmpty())
                    safe->processor_.captureLoaded();
                safe->lastCaptureRevision_ = safe->processor_.captures().revision.load();
                done (safe->processor_.captures().stateAsVar());
            });
        });
}

void SedefEditor::emitCapture()
{
    lastCaptureRevision_ = processor_.captures().revision.load();
    browser_->emitEventIfBrowserIsVisible ("sedef:capture", processor_.captures().stateAsVar());
}

// -------------------------------------------------------------------------------------------------

juce::var SedefEditor::schema() const
{
    juce::Array<juce::var> list;
    for (const auto& s : kParamSpecs)
    {
        juce::Array<juce::var> choices;
        for (int c = 0; c < s.numChoices(); ++c)
            choices.add (toJuce (s.choiceLabel (c)));
        static constexpr const char* kinds[] = { "float", "int", "bool", "choice" };
        list.add (object ({ { "id", toJuce (s.id) },
                            { "name", toJuce (s.name) },
                            { "group", toJuce (s.group) },
                            { "kind", kinds[static_cast<int> (s.kind)] },
                            { "min", s.min },
                            { "max", s.max },
                            { "default", s.defaultValue },
                            { "skew", s.skew },
                            { "unit", toJuce (s.unit) },
                            { "automatable", s.automatable },
                            { "choices", choices } }));
    }
    return list;
}

juce::var SedefEditor::allValues() const
{
    auto* o = new juce::DynamicObject();
    for (std::size_t i = 0; i < kNumParams; ++i)
        o->setProperty (toJuce (kParamSpecs[i].id),
                        processor_.paramValue (static_cast<ParamId> (i)));
    return juce::var (o);
}

juce::var SedefEditor::hello()
{
    static constexpr const char* kWrappers[] = { "Browser",    "VST",         "VST3",
                                                 "AudioUnit",  "AudioUnitv3", "AAX",
                                                 "Standalone", "Unity",       "LV2" };
    const auto type = static_cast<int> (processor_.wrapperType);
    // The CLAP wrapper reports itself as "undefined" to JUCE.
    const juce::String wrapper = type > 0 && type < 9 ? kWrappers[type] : "CLAP";

    for (std::size_t i = 0; i < kNumParams; ++i)
        lastSent_[i] = processor_.paramValue (static_cast<ParamId> (i));
    lastPresetRevision_ = processor_.presetRevision.load();
    lastIrRevision_ = processor_.irRevision.load();
    lastCaptureRevision_ = processor_.captures().revision.load();
    lastDirty_ = processor_.presets().isDirty();
    lastSampleRate_ = processor_.engine().sampleRate();
    lastLatency_ = processor_.engine().latencySamples();

    return object ({ { "protocol", kProtocolVersion },
                     { "appVersion", SEDEF_VERSION },
                     { "wrapperType", wrapper },
                     { "isStandalone", processor_.isStandalone() },
                     { "sampleRate", processor_.engine().sampleRate() },
                     { "latencySamples", processor_.engine().latencySamples() },
                     { "layout", processor_.layoutDescription() },
                     { "schema", schema() },
                     { "values", allValues() },
                     { "uiState", processor_.uiState() },
                     { "presets", processor_.presets().stateAsVar() },
                     { "ir", processor_.irs().stateAsVar() },
                     { "capture", processor_.captures().stateAsVar() } });
}

void SedefEditor::timerCallback()
{
    if (smokeResult_ != juce::File() && !smokeDone_)
        smokeTestTick();
    // While hidden (minimised, or a host that hides instead of destroying editors) the browser
    // drops events, so don't advance the last-sent bookkeeping either; the page catches up when
    // shown.
    if (!pageReady_ || !browser_->isShowing())
        return;
    ++tick_;

    emitParams();
    if (tick_ % 2 == 0)
        emitTelemetry();
    if (processor_.analysis().isActive())
        emitPitch();

    if (processor_.presetRevision.load() != lastPresetRevision_)
    {
        lastPresetRevision_ = processor_.presetRevision.load();
        emitPresets();
    }
    else if (tick_ % 15 == 0)
    {
        const bool dirty = processor_.presets().isDirty();
        if (dirty != lastDirty_)
            emitPresets();
    }
    if (processor_.irRevision.load() != lastIrRevision_)
    {
        lastIrRevision_ = processor_.irRevision.load();
        emitIrs();
    }
    if (processor_.captures().revision.load() != lastCaptureRevision_)
        emitCapture();
    emitSessionIfChanged();
}

void SedefEditor::emitSessionIfChanged()
{
    const double rate = processor_.engine().sampleRate();
    const int latency = processor_.engine().latencySamples();
    if (juce::exactlyEqual (rate, lastSampleRate_) && latency == lastLatency_)
        return;
    lastSampleRate_ = rate;
    lastLatency_ = latency;
    browser_->emitEventIfBrowserIsVisible (
        "sedef:session", object ({ { "sampleRate", rate },
                                   { "latencySamples", latency },
                                   { "layout", processor_.layoutDescription() } }));
}

void SedefEditor::emitParams()
{
    juce::DynamicObject::Ptr changed;
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        const float v = processor_.paramValue (static_cast<ParamId> (i));
        if (!juce::exactlyEqual (v, lastSent_[i]))
        {
            lastSent_[i] = v;
            if (changed == nullptr)
                changed = new juce::DynamicObject();
            changed->setProperty (toJuce (kParamSpecs[i].id), v);
        }
    }
    if (changed != nullptr)
        browser_->emitEventIfBrowserIsVisible (
            "sedef:params", object ({ { "values", juce::var (changed.get()) } }));
}

void SedefEditor::emitTelemetry()
{
    auto& t = const_cast<Engine::Telemetry&> (processor_.engine().telemetry());
    const int ins = processor_.engine().numInputs();
    const int outs = processor_.engine().numOutputs();
    juce::Array<juce::var> in, out;
    for (int c = 0; c < ins; ++c)
        in.add (toDb (t.inputPeak[static_cast<size_t> (c)].exchange (0.0f)));
    for (int c = 0; c < outs; ++c)
        out.add (toDb (t.outputPeak[static_cast<size_t> (c)].exchange (0.0f)));

    browser_->emitEventIfBrowserIsVisible (
        "sedef:telemetry", object ({ { "inputDb", in },
                                     { "outputDb", out },
                                     { "inputClip", t.inputClip.exchange (false) },
                                     { "outputClip", t.outputClip.exchange (false) },
                                     { "gateOpen", t.gateOpen.load() },
                                     { "gateReductionDb", t.gateReductionDb.load() },
                                     { "tideReductionDb", t.tideReductionDb.load() },
                                     { "metronomeStep", t.metronomeStep.load() },
                                     { "driftTimeMs", t.driftTimeMs.load() },
                                     { "cpu", processor_.cpuLoad() } }));
}

void SedefEditor::emitPitch()
{
    const auto r = processor_.analysis().latest();
    const int midi = juce::jlimit (0, 127, r.midiNote);
    browser_->emitEventIfBrowserIsVisible (
        "sedef:pitch", object ({ { "valid", r.valid },
                                 { "frequencyHz", r.frequencyHz },
                                 { "midiNote", midi },
                                 { "note", kNoteNames[midi % 12] },
                                 { "octave", midi / 12 - 1 },
                                 { "cents", r.cents },
                                 { "clarity", r.clarity },
                                 { "levelDb", r.levelDb },
                                 { "stable", r.stable },
                                 { "stableHz", r.stableHz },
                                 { "stableStdCents", r.stableStdCents },
                                 { "stableCount", r.stableCount },
                                 { "onsetId", static_cast<int> (r.onsetId) },
                                 { "displayValid", r.displayValid },
                                 { "displayHeld", r.displayHeld },
                                 { "displaySettled", r.displaySettled },
                                 { "displayHz", r.displayHz },
                                 { "displayNote", juce::jlimit (0, 127, r.displayNote) },
                                 { "displayCents", r.displayCents } }));
}

void SedefEditor::emitPresets()
{
    lastDirty_ = processor_.presets().isDirty();
    browser_->emitEventIfBrowserIsVisible ("sedef:presets", processor_.presets().stateAsVar());
}

void SedefEditor::emitIrs()
{
    browser_->emitEventIfBrowserIsVisible ("sedef:ir", processor_.irs().stateAsVar());
}

// -------------------------------------------------------------------------------------------------
// UI smoke test

namespace
{
/// Exercises the preset bridge the way the browser and A/B do, straight over JUCE's native
/// function protocol, and leaves the outcome in window.__sedefSmokePresets. Auditions don't record
/// Recent picks, so the test leaves the user's settings alone.
constexpr const char* kSmokePresetScript = R"js((() => {
  if (window.__sedefSmokePresetsStarted) return 'running';
  window.__sedefSmokePresetsStarted = true;
  const backend = window.__JUCE__.backend;
  let nextId = 1000000; // far above the ids the UI's own bridge hands out
  const call = (name, ...params) => new Promise((resolve, reject) => {
    const resultId = nextId++;
    const timer = setTimeout(() => reject(new Error(name + ' did not answer')), 10000);
    const handle = backend.addEventListener('__juce__complete', (e) => {
      if (!e || e.promiseId !== resultId) return;
      clearTimeout(timer);
      backend.removeEventListener(handle);
      resolve(e.result);
    });
    backend.emitEvent('__juce__invoke', { name, params, resultId });
  });
  const values = async () => (await call('sedef_hello', 2)).values;
  const differences = (a, b) => Object.keys(a).filter((k) => a[k] !== b[k]);
  (async () => {
    const hello = await call('sedef_hello', 2);
    const before = hello.values;
    const start = hello.presets;
    await call('sedef_presets_snapshot', 'undo'); // the session as the host has it

    // The browser: audition a preset, then Esc back.
    await call('sedef_presets_snapshot', 'browse');
    const auditioned = await call('sedef_presets_load', 'factory:mariana', true);
    const item = auditioned.items.find((p) => p.id === auditioned.currentId);
    const metadata = !!item && item.amp === 2 && Array.isArray(item.blocks) &&
      item.blocks.includes('gate') && typeof item.description === 'string';
    const auditionChanged = differences(before, await values()).length;
    const restored = await call('sedef_presets_restore', 'browse');
    const restoreMismatches = differences(before, await values());
    const restoredPreset = restored.currentId === start.currentId &&
      restored.currentName === start.currentName && restored.dirty === start.dirty;

    // Skewed ranges are where a normalised round trip can move a value by an ULP: restore from
    // many random settings of them (a fixed LCG keeps runs comparable).
    const skewed = hello.schema.filter((p) => p.kind === 'float' && p.skew !== 1);
    let seed = 20261101;
    const random = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
    const stressMismatches = [];
    for (let round = 0; round < 24; ++round) {
      for (const p of skewed) await call('sedef_setParam', p.id, p.min + (p.max - p.min) * random());
      const set = await values();
      await call('sedef_presets_snapshot', 'browse');
      await call('sedef_presets_load', round % 2 ? 'factory:mariana' : 'factory:undertow', true);
      await call('sedef_presets_restore', 'browse');
      stressMismatches.push(...differences(set, await values()));
    }
    await call('sedef_presets_restore', 'undo');
    const undoMismatches = differences(before, await values());

    // A/B: B starts as A, gets another preset, and switching back brings A back exactly.
    await call('sedef_presets_compare', 'switch');
    await call('sedef_presets_load', 'factory:riptide', true);
    const back = await call('sedef_presets_compare', 'switch');
    const compareMismatches = differences(before, await values());
    const compare = back.compare.active === 'A' && !!back.compare.other &&
      back.compare.other.name === 'Riptide' && back.currentName === start.currentName;
    const ok = metadata && auditionChanged > 0 && restoreMismatches.length === 0 &&
      restoredPreset && stressMismatches.length === 0 && undoMismatches.length === 0 &&
      compareMismatches.length === 0 && compare;
    window.__sedefSmokePresets = { ok, metadata, auditionChanged, restoreMismatches,
      restoredPreset, skewed: skewed.length, stressMismatches, undoMismatches, compareMismatches,
      compare };
  })().catch((error) => { window.__sedefSmokePresets = { ok: false, error: String(error) }; });
  return 'started';
})())js";
} // namespace

void SedefEditor::smokeTestTick()
{
    if (++smokeTicks_ > kSmokeTimeoutTicks)
    {
        if (smokePage_.isVoid())
            finishSmokeTest (false, pageReady_ ? "The page called sedef_hello but never rendered"
                                               : "The page never called sedef_hello");
        else
            finishSmokeTest (false, smokePage_, "The preset check never finished");
        return;
    }
    if (!pageReady_)
        return;
    if (smokeReadyAt_ < 0)
        smokeReadyAt_ = smokeTicks_;
    if (smokeEvaluating_ || smokeTicks_ - smokeReadyAt_ < kSmokeSettleTicks)
        return;

    // One evaluation in flight at a time: the page check, then the preset check, then polls for
    // its result. A JavaScript error ends the test.
    smokeEvaluating_ = true;
    const auto evaluate = [this] (const juce::String& script, auto onResult) {
        browser_->evaluateJavascript (
            script, [safe = juce::Component::SafePointer<SedefEditor> (this), onResult] (auto r) {
                if (safe == nullptr)
                    return;
                safe->smokeEvaluating_ = false;
                if (const auto* error = r.getError())
                {
                    safe->finishSmokeTest (false, safe->smokePage_,
                                           "JavaScript error: " + error->message);
                    return;
                }
                const auto* value = r.getResult();
                onResult (*safe, value != nullptr ? value->toString() : juce::String());
            });
    };

    if (smokePage_.isVoid())
    {
        evaluate (
            "JSON.stringify({ controls: document.querySelectorAll('button, input, select, "
            "[role=slider]').length, alerts: Array.from(document.querySelectorAll('[role=alert]'))"
            ".map(e => e.textContent), title: document.title, "
            "text: (document.body.innerText || '').slice(0, 160) })",
            [] (SedefEditor& self, const juce::String& text) {
                const auto page = juce::JSON::parse (text);
                const int controls = page.getProperty ("controls", 0);
                const auto* alerts = page.getProperty ("alerts", juce::var()).getArray();
                // The editor shows dozens of knobs and switches; a handful means it failed to
                // render. Any alert means the page fell back to an error message (bridge or
                // render failure).
                self.smokePageOk_ = controls >= 20 && (alerts == nullptr || alerts->isEmpty());
                self.smokePage_ = page.isObject() ? page : juce::var (text);
            });
    }
    else if (!smokePresetsStarted_)
    {
        evaluate (kSmokePresetScript, [] (SedefEditor& self, const juce::String&) {
            self.smokePresetsStarted_ = true;
        });
    }
    else
    {
        evaluate ("JSON.stringify(window.__sedefSmokePresets ?? null)",
                  [] (SedefEditor& self, const juce::String& text) {
                      const auto presets = juce::JSON::parse (text);
                      if (presets.isObject())
                          self.finishSmokeTest (self.smokePageOk_ &&
                                                    static_cast<bool> (presets["ok"]),
                                                self.smokePage_, presets);
                  });
    }
}

void SedefEditor::finishSmokeTest (bool ok, const juce::var& page, const juce::var& presets)
{
    if (smokeDone_)
        return;
    smokeDone_ = true;
    auto* result = new juce::DynamicObject();
    result->setProperty ("ok", ok);
    result->setProperty ("protocol", kProtocolVersion);
    result->setProperty ("wrapper",
                         juce::AudioProcessor::getWrapperTypeDescription (processor_.wrapperType));
    result->setProperty ("page", page);
    result->setProperty ("presets", presets);
    smokeResult_.replaceWithText (juce::JSON::toString (juce::var (result)));

    if (processor_.isStandalone())
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
}

} // namespace sedef::plugin
