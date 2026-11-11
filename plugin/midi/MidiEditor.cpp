// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MidiEditor.h"

#include "../src/ParameterLayout.h"
#include "../src/ZipResources.h"
#include "SedefMidiUiData.h"

#if JucePlugin_Build_Standalone
// The standalone holder needs juce_audio_utils first; the comment keeps include sorting off.
#include <juce_audio_utils/juce_audio_utils.h>
//
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace sedef::midiplugin
{
namespace
{
using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;
using plugin::toJuce;

/// Development: point the editor at a running Vite dev server instead of the embedded build.
juce::String devServerUrl()
{
    return juce::SystemStats::getEnvironmentVariable ("SEDEF_MIDI_UI_DEV_URL", {});
}

/// CI: when set to a file path, the editor checks that the web UI loaded, writes the result there
/// as JSON and (in the standalone app) quits. See tools/ci/ui-smoke.sh.
juce::String smokeTestPath()
{
    return juce::SystemStats::getEnvironmentVariable ("SEDEF_UI_SMOKE_TEST", {});
}

constexpr int kTimerHz = 30;
constexpr int kSmokeTimeoutTicks = 60 * kTimerHz;
constexpr int kSmokeSettleTicks = kTimerHz;  // let React render for a second after the handshake
const juce::Colour kBackground (0xff0b1117); // the UI's background, shown while the page loads

juce::ZipFile& uiArchive()
{
    static int size = 0;
    static const char* data =
        SedefMidiUiData::getNamedResource (SedefMidiUiData::namedResourceList[0], size);
    static juce::MemoryInputStream stream (data, static_cast<size_t> (size), false);
    static juce::ZipFile zip (stream);
    return zip;
}

std::optional<midi::ParamId> paramFromVar (const juce::var& v)
{
    return midi::findParam (v.toString().toStdString());
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

juce::String wrapperName (juce::AudioProcessor::WrapperType type)
{
    static constexpr const char* kWrappers[] = { "Browser",    "VST",         "VST3",
                                                 "AudioUnit",  "AudioUnitv3", "AAX",
                                                 "Standalone", "Unity",       "LV2" };
    const auto t = static_cast<int> (type);
    // The CLAP wrapper reports itself as "undefined" to JUCE.
    return t > 0 && t < 9 ? kWrappers[t] : "CLAP";
}

/// Only the project's own pages open from the editor.
bool allowedUrl (const juce::String& url)
{
    return url.startsWith ("https://github.com/mr-kaynak/sedef-amp");
}
} // namespace

// -------------------------------------------------------------------------------------------------

class MidiEditor::Browser final : public juce::WebBrowserComponent
{
public:
    using WebBrowserComponent::WebBrowserComponent;

    bool pageAboutToLoad (const juce::String& url) override
    {
        const auto dev = devServerUrl();
        return url.startsWith (getResourceProviderRoot()) ||
               (dev.isNotEmpty() && url.startsWith (dev));
    }
};

MidiEditor::MidiEditor (MidiProcessor& p) : AudioProcessorEditor (&p), processor_ (p)
{
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
        lastSent_[i] = processor_.paramValue (static_cast<midi::ParamId> (i));

    browser_ = std::make_unique<Browser> (makeOptions());
    addAndMakeVisible (*browser_);
    const auto dev = devServerUrl();
    browser_->goToURL (dev.isNotEmpty() ? dev
                                        : juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (MidiProcessor::kBaseWidth, MidiProcessor::kBaseHeight,
                     MidiProcessor::kBaseWidth * 2, MidiProcessor::kBaseHeight * 2);
    setSize (MidiProcessor::kBaseWidth, MidiProcessor::kBaseHeight);

    if (const auto path = smokeTestPath(); path.isNotEmpty())
        smokeResult_ = juce::File (path);
    startTimerHz (kTimerHz);
}

MidiEditor::~MidiEditor()
{
    stopTimer();
}

void MidiEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
}

void MidiEditor::resized()
{
    browser_->setBounds (getLocalBounds());
}

// -------------------------------------------------------------------------------------------------

juce::WebBrowserComponent::Options MidiEditor::makeOptions()
{
    using Options = juce::WebBrowserComponent::Options;
    auto& proc = processor_;

    auto options =
        Options {}
            .withBackend (Options::Backend::webview2)
            .withWinWebView2Options (
                Options::WinWebView2 {}
                    .withUserDataFolder (
                        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Sedef")
                            .getChildFile ("Sedef MIDI")
                            .getChildFile ("WebView2"))
                    .withBackgroundColour (kBackground)
                    .withStatusBarDisabled())
            .withNativeIntegrationEnabled()
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withResourceProvider (
                [] (const juce::String& url) {
                    return plugin::resourceFromZip (uiArchive(), url, "midi.html");
                },
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
                proc.setParamValue (*id, static_cast<float> (a[1]));
                lastSent_[midi::index (*id)] = proc.paramValue (*id); // no echo
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
                    p->beginChangeGesture();
                    proc.setParamValue (*id, midi::spec (*id).defaultValue);
                    p->endChangeGesture();
                    lastSent_[midi::index (*id)] = proc.paramValue (*id);
                }
        done ({});
    });

    fn ("sedef_midi_panic", [&proc] (const juce::Array<juce::var>&, Completion done) {
        proc.panic();
        done ({});
    });

    fn ("sedef_midi_ports",
        [this] (const juce::Array<juce::var>&, Completion done) { done (ports()); });

    fn ("sedef_midi_setPort", [this, &proc] (const juce::Array<juce::var>& a, Completion done) {
        if (!a.isEmpty())
            proc.setDestination (a[0].toString());
        done (ports());
    });

    fn ("sedef_midi_forgetLowNote", [&proc] (const juce::Array<juce::var>&, Completion done) {
        proc.forgetLowNote();
        done ({});
    });

    fn ("sedef_midi_audioSettings", [] (const juce::Array<juce::var>&, Completion done) {
#if JucePlugin_Build_Standalone
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            holder->showAudioSettingsDialog();
#endif
        done ({});
    });

    fn ("sedef_openUrl", [] (const juce::Array<juce::var>& a, Completion done) {
        if (!a.isEmpty() && allowedUrl (a[0].toString()))
            juce::URL (a[0].toString()).launchInDefaultBrowser();
        done ({});
    });

    return options;
}

juce::var MidiEditor::schema() const
{
    juce::Array<juce::var> list;
    for (const auto& s : midi::kParamSpecs)
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

juce::var MidiEditor::allValues() const
{
    auto* o = new juce::DynamicObject();
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
        o->setProperty (toJuce (midi::kParamSpecs[i].id),
                        processor_.paramValue (static_cast<midi::ParamId> (i)));
    return juce::var (o);
}

juce::var MidiEditor::ports() const
{
    juce::Array<juce::var> outputs;
    for (const auto& d : MidiPortRouter::availableOutputs())
        outputs.add (object (
            { { "id", MidiPortRouter::kDevicePrefix + d.identifier }, { "name", d.name } }));
    auto& router = processor_.router();
    return object ({ { "selected", router.destination() },
                     { "openName", router.openName() },
                     { "status", router.status() },
                     { "virtualSupported", MidiPortRouter::virtualSupported() },
                     { "virtualName", processor_.virtualPortName() },
                     { "outputs", outputs } });
}

juce::var MidiEditor::audio() const
{
    const auto t = processor_.telemetry();
    const double rate = processor_.sampleRate();
    double inputLatencyMs = -1.0; // unknown: a host does not tell a plug-in
#if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        if (auto* device = holder->deviceManager.getCurrentAudioDevice())
            if (device->getCurrentSampleRate() > 0.0)
                inputLatencyMs =
                    1000.0 * device->getInputLatencyInSamples() / device->getCurrentSampleRate();
#endif
    return object ({ { "sampleRate", rate },
                     { "blockSize", t.blockSize },
                     // What the buffer costs a note sent through the port: at most, on average.
                     { "bufferWorstMs", MidiPortRouter::worstDelayMs (t.blockSize, rate) },
                     { "bufferMeanMs", MidiPortRouter::meanDelayMs (t.blockSize, rate) },
                     { "inputLatencyMs", inputLatencyMs },
                     { "lowNote", t.lowNote } });
}

juce::var MidiEditor::hello()
{
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
        lastSent_[i] = processor_.paramValue (static_cast<midi::ParamId> (i));
    lastPorts_ = ports();
    lastAudio_ = audio();
    return object ({ { "protocol", kProtocolVersion },
                     { "appVersion", SEDEF_VERSION },
                     { "wrapperType", wrapperName (processor_.wrapperType) },
                     { "isStandalone", processor_.isStandalone() },
                     { "isInstrument", MidiProcessor::isInstrument() },
                     { "sampleRate", processor_.sampleRate() },
                     { "steadyLagMs", 1000.0 * midi::GuitarToMidi::kSteadyLagSeconds },
                     { "schema", schema() },
                     { "values", allValues() },
                     { "ports", lastPorts_ },
                     { "audio", lastAudio_ } });
}

void MidiEditor::timerCallback()
{
    if (smokeResult_ != juce::File() && !smokeDone_)
        smokeTestTick();
    if (!pageReady_ || !browser_->isShowing())
        return;
    ++tick_;
    emitParams();
    emitTelemetry();
    // The port list changes when devices come and go, and the router applies a new destination
    // on its own thread: look twice a second.
    if (tick_ % (kTimerHz / 2) == 0)
    {
        const auto now = ports();
        if (juce::JSON::toString (now) != juce::JSON::toString (lastPorts_))
        {
            lastPorts_ = now;
            browser_->emitEventIfBrowserIsVisible ("sedef:midi:ports", now);
        }
        MidiProcessor::standaloneHousekeeping();
        const auto sound = audio();
        if (juce::JSON::toString (sound) != juce::JSON::toString (lastAudio_))
        {
            lastAudio_ = sound;
            browser_->emitEventIfBrowserIsVisible ("sedef:midi:audio", sound);
        }
    }
}

void MidiEditor::emitParams()
{
    juce::DynamicObject::Ptr changed;
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
    {
        const float v = processor_.paramValue (static_cast<midi::ParamId> (i));
        if (!juce::exactlyEqual (v, lastSent_[i]))
        {
            lastSent_[i] = v;
            if (changed == nullptr)
                changed = new juce::DynamicObject();
            changed->setProperty (toJuce (midi::kParamSpecs[i].id), v);
        }
    }
    if (changed != nullptr)
        browser_->emitEventIfBrowserIsVisible (
            "sedef:params", object ({ { "values", juce::var (changed.get()) } }));
}

void MidiEditor::emitTelemetry()
{
    const auto t = processor_.telemetry();
    browser_->emitEventIfBrowserIsVisible (
        "sedef:midi:telemetry", object ({ { "levelDb", t.levelDb },
                                          { "gateDb", t.gateDb },
                                          { "note", t.note },
                                          { "pitch", t.pitch },
                                          { "velocity", t.velocity },
                                          { "sounding", t.sounding },
                                          { "notesSent", static_cast<int> (t.notesSent) },
                                          { "lateNotes", static_cast<int> (t.lateNotes) },
                                          { "sampleRate", processor_.sampleRate() } }));
}

// -------------------------------------------------------------------------------------------------
// UI smoke test: the page rendered its controls and shows no error.

void MidiEditor::smokeTestTick()
{
    if (++smokeTicks_ > kSmokeTimeoutTicks)
    {
        finishSmokeTest (false, {},
                         pageReady_ ? "The page called sedef_hello but never rendered"
                                    : "The page never called sedef_hello");
        return;
    }
    if (!pageReady_)
        return;
    if (smokeReadyAt_ < 0)
        smokeReadyAt_ = smokeTicks_;
    if (smokeEvaluating_ || smokeTicks_ - smokeReadyAt_ < kSmokeSettleTicks)
        return;
    smokeEvaluating_ = true;
    browser_->evaluateJavascript (
        "JSON.stringify({ controls: document.querySelectorAll('button, input, select, "
        "[role=slider], [role=radio]').length, alerts: Array.from(document.querySelectorAll("
        "'[role=alert]')).map(e => e.textContent), title: document.title, "
        "text: (document.body.innerText || '').slice(0, 160) })",
        [safe = juce::Component::SafePointer<MidiEditor> (this)] (auto r) {
            if (safe == nullptr)
                return;
            safe->smokeEvaluating_ = false;
            if (const auto* error = r.getError())
            {
                safe->finishSmokeTest (false, {}, "JavaScript error: " + error->message);
                return;
            }
            const auto* value = r.getResult();
            const auto page = juce::JSON::parse (value != nullptr ? value->toString() : "");
            const int controls = page.getProperty ("controls", 0);
            const auto* alerts = page.getProperty ("alerts", juce::var()).getArray();
            // The editor shows a dozen controls; a handful means it failed to render, and an
            // alert means it fell back to an error message.
            safe->finishSmokeTest (controls >= 12 && (alerts == nullptr || alerts->isEmpty()),
                                   page);
        });
}

void MidiEditor::finishSmokeTest (bool ok, const juce::var& page, const juce::String& error)
{
    if (smokeDone_)
        return;
    smokeDone_ = true;
    auto* result = new juce::DynamicObject();
    result->setProperty ("ok", ok);
    result->setProperty ("product", processor_.getName());
    result->setProperty ("protocol", kProtocolVersion);
    result->setProperty ("wrapper",
                         juce::AudioProcessor::getWrapperTypeDescription (processor_.wrapperType));
    result->setProperty ("page", page);
    if (error.isNotEmpty())
        result->setProperty ("error", error);
    smokeResult_.replaceWithText (juce::JSON::toString (juce::var (result)));
    if (processor_.isStandalone())
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
}

} // namespace sedef::midiplugin
