// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/Engine.h"

#include "sedef/core/BypassFader.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Denormals.h"
#include "sedef/core/InputFold.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/Anemone.h"
#include "sedef/dsp/Beacon.h"
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/Conch.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Doubler.h"
#include "sedef/dsp/Drift.h"
#include "sedef/dsp/Fathom.h"
#include "sedef/dsp/GraphicEq.h"
#include "sedef/dsp/Gyre.h"
#include "sedef/dsp/Jetty.h"
#include "sedef/dsp/Kelp.h"
#include "sedef/dsp/Metronome.h"
#include "sedef/dsp/NoiseGate.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/Remora.h"
#include "sedef/dsp/Ripple.h"
#include "sedef/dsp/Sargasso.h"
#include "sedef/dsp/Tide.h"
#include "sedef/dsp/Trench.h"
#include "sedef/dsp/Urchin.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/capture/CaptureStage.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace sedef
{
namespace
{
using P = ParamId;

// ---------------------------------------------------------------------------------------------
// Snapshot -> module settings. Modules own their voicing; these are plain field copies.

struct ChainSettings
{
    float inputGain = 1.0f;
    bool gateOn = true;
    dsp::NoiseGate::Settings gate;
    bool tideOn = false, pearlOn = false, coralOn = false, rippleOn = false;
    dsp::Tide::Settings tide;
    dsp::Pearl::Settings pearl;
    dsp::Coral::Settings coral;
    dsp::Ripple::Settings ripple;
    dsp::AmpSection::Settings amp;
    dsp::CabSim::Settings cab;
    dsp::CaptureStage::Settings capture;
    // Pedals lane: Trench, Urchin and Kelp.
    bool trenchOn = false, urchinOn = false, kelpOn = false;
    dsp::Trench::Settings trench;
    dsp::Urchin::Settings urchin;
    dsp::Kelp::Settings kelp;
    // Drives lane: Remora, Sargasso and Breaker.
    bool remoraOn = false, sargassoOn = false, breakerOn = false;
    dsp::Remora::Settings remora;
    dsp::Sargasso::Settings sargasso;
    dsp::Breaker::Settings breaker;
    // mods lane: wah, envelope filter, phaser, flanger, vibe.
    bool conchOn = false, anemoneOn = false, gyreOn = false, jettyOn = false, beaconOn = false;
    dsp::Conch::Settings conch;
    dsp::Anemone::Settings anemone;
    dsp::Gyre::Settings gyre;
    dsp::Jetty::Settings jetty;
    dsp::Beacon::Settings beacon;
};

dsp::CabSim::MicSettings micSettings (const ParamSnapshot& p, int mic, int cab)
{
    // Mic A and B parameters are laid out identically; B follows A in the schema.
    const auto base = [mic] (P a, P b) {
        return mic == 0 ? a : b;
    };
    const P types[2][3] = { { P::micACab1Type, P::micACab2Type, P::micACab3Type },
                            { P::micBCab1Type, P::micBCab2Type, P::micBCab3Type } };

    dsp::CabSim::MicSettings m;
    m.enabled = p.flag (base (P::micAEnabled, P::micBEnabled));
    m.type = p.choice (types[mic][cab]);
    m.position = p[base (P::micAPosition, P::micBPosition)];
    m.distance = p[base (P::micADistance, P::micBDistance)];
    m.levelDb = p[base (P::micALevel, P::micBLevel)];
    m.pan = p[base (P::micAPan, P::micBPan)] / 100.0f;
    m.invertPolarity = p.flag (base (P::micAPhase, P::micBPhase));
    m.roomEnabled = p.flag (base (P::roomAEnabled, P::roomBEnabled));
    m.roomLevelDb = p[base (P::roomALevel, P::roomBLevel)];
    m.azimuthDeg = p[base (P::micAAzimuth, P::micBAzimuth)];
    m.offAxisDeg = p[base (P::micAOffAxis, P::micBOffAxis)];
    return m;
}

ChainSettings chainSettings (const ParamSnapshot& p)
{
    ChainSettings s;
    // P::transpose is kept for old sessions and ignored: transpose was removed in 0.3.0
    // (docs/dsp/transpose.md, "Decision").
    s.inputGain = dbToGain (p[P::inputGain]);
    s.gateOn = p.flag (P::gateEnabled);
    s.gate.thresholdDb = p[P::gateThreshold];

    s.tideOn = p.flag (P::tideEnabled);
    s.tide = { p[P::tideComp], p[P::tideTone], p[P::tideLevel], p[P::tideBlend] };
    s.pearlOn = p.flag (P::pearlEnabled);
    s.pearl = { p[P::pearlGain], p[P::pearlTone], p[P::pearlLevel], p[P::pearlMid] };
    s.coralOn = p.flag (P::coralEnabled);
    s.coral = { p[P::coralGain], p[P::coralCut], p[P::coralLevel] };
    s.rippleOn = p.flag (P::rippleEnabled);
    s.ripple = { p.choice (P::rippleMode), p[P::rippleSpeed], p[P::rippleDepth] };

    auto& a = s.amp;
    a.model = std::clamp (p.choice (P::ampModel), 0, 2);
    a.lagoon = { p[P::lagoonVolume], p[P::lagoonBass], p[P::lagoonCut], p[P::lagoonMaster],
                 p[P::lagoonOutput] };
    a.reef = { p.choice (P::reefVoice), p[P::reefGain],     p[P::reefBass],   p[P::reefMid],
               p[P::reefTreble],        p[P::reefPresence], p[P::reefMaster], p[P::reefOutput] };
    a.abyss = { p.flag (P::abyssBright), p[P::abyssGain],   p[P::abyssBass],
                p[P::abyssMid],          p[P::abyssTreble], p.flag (P::abyssShift),
                p[P::abyssPresence],     p[P::abyssMaster], p[P::abyssOutput] };

    // The cab follows the amp while linked; otherwise the user picks it.
    s.cab.cab = p.flag (P::ampCabLink)
                    ? a.model
                    : std::clamp (p.choice (P::cabModel), 0, dsp::CabSim::kNumCabs - 1);
    for (int m = 0; m < dsp::CabSim::kNumMics; ++m)
        s.cab.mics[static_cast<size_t> (m)] = micSettings (p, m, s.cab.cab);

    s.capture = { p.choice (P::ampSource) == 1, p[P::captureInput], p[P::captureOutput],
                  p[P::captureInterfaceLevel], p.flag (P::captureCab) };

    // Pedals lane: Trench, Urchin and Kelp.
    s.trenchOn = p.flag (P::trenchEnabled);
    s.trench = { p[P::trenchDirect], p[P::trenchSub1], p[P::trenchSub2], p[P::trenchTone],
                 p.choice (P::trenchMode) };
    s.urchinOn = p.flag (P::urchinEnabled);
    s.urchin = { p[P::urchinFuzz], p[P::urchinOctave], p[P::urchinTone], p[P::urchinLevel] };
    s.kelpOn = p.flag (P::kelpEnabled);
    s.kelp = { p[P::kelpDrive], p[P::kelpTone], p[P::kelpLevel] };
    // Drives lane: Remora, Sargasso and Breaker.
    s.remoraOn = p.flag (P::remoraEnabled);
    s.remora = { p[P::remoraFuzz], p[P::remoraVolume], p[P::remoraBias], p[P::remoraGuitarVolume],
                 p.choice (P::remoraTransistor) };
    s.sargassoOn = p.flag (P::sargassoEnabled);
    s.sargasso = { p[P::sargassoSustain], p[P::sargassoTone], p[P::sargassoVolume] };
    s.breakerOn = p.flag (P::breakerEnabled);
    s.breaker = { p[P::breakerBoost], p.choice (P::breakerRange) };
    // mods lane
    s.conchOn = p.flag (P::conchEnabled);
    s.conch = { p.choice (P::conchMode), p[P::conchPosition], p[P::conchResonance],
                p[P::conchSensitivity] };
    s.anemoneOn = p.flag (P::anemoneEnabled);
    s.anemone = { p[P::anemoneSensitivity], p[P::anemoneRange], p[P::anemoneResonance],
                  p.choice (P::anemoneMode), p.choice (P::anemoneDirection) };
    s.gyreOn = p.flag (P::gyreEnabled);
    s.gyre = { p[P::gyreSpeed], p[P::gyreDepth], p[P::gyreFeedback], p.choice (P::gyreStages) };
    s.jettyOn = p.flag (P::jettyEnabled);
    s.jetty = { p[P::jettySpeed], p[P::jettyDepth], p[P::jettyDelay],
                p[P::jettyFeedback] / 100.0f };
    s.beaconOn = p.flag (P::beaconEnabled);
    s.beacon = { p[P::beaconSpeed], p[P::beaconDepth], p.choice (P::beaconMode) };
    return s;
}

struct EqBank
{
    P enabled, lowCut, highCut;
    std::array<P, 9> bands;
};

constexpr std::array<EqBank, 3> kEqBanks { {
    { P::eqLagoonEnabled,
      P::eqLagoonLowCut,
      P::eqLagoonHighCut,
      { P::eqLagoonBand1, P::eqLagoonBand2, P::eqLagoonBand3, P::eqLagoonBand4, P::eqLagoonBand5,
        P::eqLagoonBand6, P::eqLagoonBand7, P::eqLagoonBand8, P::eqLagoonBand9 } },
    { P::eqReefEnabled,
      P::eqReefLowCut,
      P::eqReefHighCut,
      { P::eqReefBand1, P::eqReefBand2, P::eqReefBand3, P::eqReefBand4, P::eqReefBand5,
        P::eqReefBand6, P::eqReefBand7, P::eqReefBand8, P::eqReefBand9 } },
    { P::eqAbyssEnabled,
      P::eqAbyssLowCut,
      P::eqAbyssHighCut,
      { P::eqAbyssBand1, P::eqAbyssBand2, P::eqAbyssBand3, P::eqAbyssBand4, P::eqAbyssBand5,
        P::eqAbyssBand6, P::eqAbyssBand7, P::eqAbyssBand8, P::eqAbyssBand9 } },
} };

// ---------------------------------------------------------------------------------------------

/// Hands a slot's settings to its module. Coming back from bypass, the module restarts from a
/// clean state that lands directly on the current settings: reset() re-arms each module's
/// "first settings snap" and the settings that follow consume it, so automation on the next block
/// glides again. The settings also go in once before reset() for modules whose reset reads them
/// (the gate starts open or closed depending on its threshold). Stale state is masked by the
/// slot's fade-in.
template <typename Module, typename Settings>
void applySettings (Module& module, const Settings& settings, bool wasBypassed) noexcept
{
    if (wasBypassed)
    {
        module.setSettings (settings);
        module.reset();
    }
    module.setSettings (settings);
}

/// Wraps a module with a click-free bypass. While bypassed, the dry path is delayed by the
/// module's latency so the chain's total latency never changes when pedals are switched.
template <typename Module>
struct Slot
{
    Module module;
    BypassFader fader;
    DelayLine dryDelay;
    std::vector<float> dry;
    int latency = 0;
    bool snapOnNextBlock = true; // first block after prepare/reset starts in its final state

    void prepare (const ProcessSpec& spec)
    {
        module.prepare (spec);
        if constexpr (requires { module.latencySamples(); })
            latency = module.latencySamples();
        dryDelay.prepare (std::max (latency, 1));
        dry.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
        fader.prepare (spec.sampleRate);
    }

    void reset() noexcept
    {
        module.reset();
        dryDelay.reset();
        snapOnNextBlock = true;
    }

    /// Returns true when the module ran (so its telemetry is meaningful).
    template <typename Settings, typename Run>
    bool process (float* io, int n, bool enabled, const Settings& settings, Run&& run) noexcept
    {
        // The first block after prepare/reset counts as coming out of bypass, so modules whose
        // reset() depends on their settings (the gate) get settings, reset, settings.
        const bool wasBypassed = snapOnNextBlock || fader.isFullyBypassed();
        if (snapOnNextBlock)
        {
            fader.snap (enabled);
            snapOnNextBlock = false;
        }
        fader.setEnabled (enabled);

        // The dry path is only needed while bypassed or crossfading, but a latency-bearing module
        // keeps its delay line warm so a later bypass lines up from the first sample.
        const bool needDry = !fader.isFullyActive();
        if (latency > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                dryDelay.push (io[i]);
                if (needDry)
                    dry[static_cast<size_t> (i)] = dryDelay.read (latency);
            }
        }
        else if (needDry)
        {
            std::copy_n (io, n, dry.data());
        }

        if (fader.isFullyBypassed())
        {
            if (latency > 0)
                std::copy_n (dry.data(), n, io);
            return false; // settings wait until the module runs again
        }
        applySettings (module, settings, wasBypassed);

        run (module, AudioView (io, n));
        if (!fader.isFullyActive())
            fader.mix (AudioView (dry.data(), n), AudioView (io, n));
        return true;
    }
};

/// Same idea for stereo post effects without latency.
template <typename Module>
struct StereoSlot
{
    Module module;
    BypassFader fader;
    std::array<std::vector<float>, 2> dry;
    bool snapOnNextBlock = true;

    void prepare (const ProcessSpec& spec)
    {
        module.prepare (spec);
        for (auto& d : dry)
            d.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
        fader.prepare (spec.sampleRate);
    }

    void reset() noexcept
    {
        module.reset();
        snapOnNextBlock = true;
    }

    template <typename Settings>
    void process (const AudioView& io, bool enabled, const Settings& settings) noexcept
    {
        // The first block after prepare/reset counts as coming out of bypass, so modules whose
        // reset() depends on their settings (the gate) get settings, reset, settings.
        const bool wasBypassed = snapOnNextBlock || fader.isFullyBypassed();
        if (snapOnNextBlock)
        {
            fader.snap (enabled);
            snapOnNextBlock = false;
        }
        fader.setEnabled (enabled);
        if (fader.isFullyBypassed())
            return; // settings wait until the module runs again
        applySettings (module, settings, wasBypassed);

        const int n = io.numFrames();
        std::array<float*, 2> dryPtrs { dry[0].data(), dry[1].data() };
        const AudioView dryView (dryPtrs.data(), 2, n);
        const bool fading = !fader.isFullyActive();
        if (fading)
            dryView.copyFrom (io);
        module.process (io);
        if (fading)
            fader.mix (dryView, io);
    }
};

/// One instrument chain: input gain -> gate -> pedals -> amp -> cab. Mono in,
/// stereo out.
struct Chain
{
    LinearSmoothed inputGain;
    Slot<dsp::NoiseGate> gate;
    Slot<dsp::Tide> tide;
    Slot<dsp::Pearl> pearl;
    Slot<dsp::Coral> coral;
    Slot<dsp::Ripple> ripple;
    // mods lane. Latency-free, linear modules; order in process().
    Slot<dsp::Anemone> anemone;
    Slot<dsp::Conch> conch;
    Slot<dsp::Beacon> beacon;
    Slot<dsp::Gyre> gyre;
    Slot<dsp::Jetty> jetty;
    dsp::AmpSection amp;
    dsp::CabSim cab;
    dsp::CaptureStage capture; // the amp or a .nam capture, and the cab or not
    // Pedals lane: Trench, Urchin and Kelp, each a self-contained module in a latency-aligned
    // bypass slot.
    Slot<dsp::Trench> trench;
    Slot<dsp::Urchin> urchin;
    Slot<dsp::Kelp> kelp;
    // Drives lane: Remora, Sargasso and Breaker (their places in the chain: process()).
    Slot<dsp::Remora> remora;
    Slot<dsp::Sargasso> sargasso;
    Slot<dsp::Breaker> breaker;

    std::vector<float> buffer;
    std::vector<float> sidechain;
    float inputPeak = 0.0f;
    bool gateRan = false, tideRan = false;
    bool snapGain = true;

    void prepare (double sampleRate, int maxBlock, OversamplingMode oversampling)
    {
        const ProcessSpec mono { sampleRate, maxBlock, 1, oversampling };
        inputGain.reset (sampleRate, 0.02);
        gate.prepare (mono);
        tide.prepare (mono);
        pearl.prepare (mono);
        coral.prepare (mono);
        ripple.prepare (mono);
        trench.prepare (mono);
        urchin.prepare (mono);
        kelp.prepare (mono);
        remora.prepare (mono);
        sargasso.prepare (mono);
        breaker.prepare (mono);
        anemone.prepare (mono); // mods lane
        conch.prepare (mono);
        beacon.prepare (mono);
        gyre.prepare (mono);
        jetty.prepare (mono);
        amp.prepare (mono);
        capture.prepare (mono, amp.latencySamples());
        cab.prepare ({ sampleRate, maxBlock, 2 });
        buffer.assign (static_cast<size_t> (maxBlock), 0.0f);
        sidechain.assign (static_cast<size_t> (maxBlock), 0.0f);
    }

    int latency() const noexcept
    {
        return pearl.latency + coral.latency + capture.latencySamples() + cab.latencySamples() +
               trench.latency + urchin.latency + kelp.latency + remora.latency + sargasso.latency +
               breaker.latency + anemone.latency + conch.latency + beacon.latency + gyre.latency +
               jetty.latency;
    }
    void reset() noexcept
    {
        snapGain = true;
        gate.reset();
        tide.reset();
        pearl.reset();
        coral.reset();
        ripple.reset();
        trench.reset();
        urchin.reset();
        kelp.reset();
        remora.reset();
        sargasso.reset();
        breaker.reset();
        anemone.reset(); // mods lane
        conch.reset();
        beacon.reset();
        gyre.reset();
        jetty.reset();
        amp.reset();
        capture.reset();
        cab.reset();
    }

    void process (const float* in, const AudioView& stereoOut, const ChainSettings& s) noexcept
    {
        const int n = stereoOut.numFrames();
        float* x = buffer.data();
        std::copy_n (in, n, x);

        // The gate listens to the instrument before input gain, so it follows the DI level.
        std::copy_n (x, n, sidechain.data());

        if (snapGain)
        {
            inputGain.setCurrentAndTarget (s.inputGain);
            snapGain = false;
        }
        inputGain.setTarget (s.inputGain);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            x[i] *= inputGain.next();
            peak = std::max (peak, std::abs (x[i]));
        }
        inputPeak = peak;

        gateRan =
            gate.process (x, n, s.gateOn, s.gate, [this] (dsp::NoiseGate& g, const AudioView& v) {
                g.process (v, sidechain.data());
            });
        const auto run = [] (auto& m, const AudioView& v) {
            m.process (v);
        };
        // Pedals lane: Trench, Urchin and Kelp go where they sound best (the "Where it goes in the
        // chain" section of docs/dsp/trench.md, urchin.md and kelp.md). The order stays fixed
        // until the pedalboard makes it the player's.
        // Drives lane: Remora, Sargasso and Breaker ("Chain position" in docs/dsp/remora.md,
        // sargasso.md and breaker.md). Remora behind the octave divider, which needs the clean
        // guitar more (and the mods lane's two filters, below); Sargasso after the octave-up fuzz,
        // whose rectifier wants an unclipped note; Breaker last, straight into the amp.
        trench.process (x, n, s.trenchOn, s.trench, run); // pedals lane
        // mods lane: the envelope filter and the wah go after the octave divider, before Remora
        // and the other drives: the filter reads picking dynamics a fuzz would flatten, a wah into
        // a fuzz is the classic order, and nothing here buffers Remora's clean-up away (its Guitar
        // volume models that). The vibe, phaser and flanger go after the drives (they move the
        // drives' harmonics), before Ripple. docs/dsp/conch.md et al.
        anemone.process (x, n, s.anemoneOn, s.anemone, run);
        conch.process (x, n, s.conchOn, s.conch, run);
        remora.process (x, n, s.remoraOn, s.remora, run); // drives lane
        tideRan = tide.process (x, n, s.tideOn, s.tide, run);
        urchin.process (x, n, s.urchinOn, s.urchin, run);       // pedals lane
        sargasso.process (x, n, s.sargassoOn, s.sargasso, run); // drives lane
        pearl.process (x, n, s.pearlOn, s.pearl, run);
        kelp.process (x, n, s.kelpOn, s.kelp, run); // pedals lane
        coral.process (x, n, s.coralOn, s.coral, run);
        beacon.process (x, n, s.beaconOn, s.beacon, run); // mods lane
        gyre.process (x, n, s.gyreOn, s.gyre, run);
        jetty.process (x, n, s.jettyOn, s.jetty, run);
        ripple.process (x, n, s.rippleOn, s.ripple, run);
        breaker.process (x, n, s.breakerOn, s.breaker, run); // drives lane

        capture.setSettings (s.capture);
        capture.process (amp, s.amp, AudioView (x, n));
        capture.processCab (cab, s.cab, x, stereoOut);
    }
};

float peakOf (const float* d, int n) noexcept
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i)
        p = std::max (p, std::abs (d[i]));
    return p;
}

/// Max-accumulates a peak the UI reads with exchange(0); a CAS loop so no peak is lost in between.
void raisePeak (std::atomic<float>& a, float v) noexcept
{
    float current = a.load (std::memory_order_relaxed);
    while (v > current && !a.compare_exchange_weak (current, v, std::memory_order_relaxed))
    {
    }
}

} // namespace

// -------------------------------------------------------------------------------------------------

struct Engine::Impl
{
    std::array<Chain, 2> chains;
    int numChains = 1;
    bool dual = false;

    StereoSlot<dsp::GraphicEq> eq;
    StereoSlot<dsp::Doubler> doubler;
    dsp::Drift drift;
    dsp::Fathom fathom;
    dsp::Metronome metronome;

    LinearSmoothed outputGain;
    LinearSmoothed mute;     // tuner mute
    LinearSmoothed modeFade; // stereo-mode switch
    int modeFadeSamples = 1; // length of one modeFade ramp
    int modeFadeLeft = 0;    // samples until a pending switch's fade-out completes
    bool snapGains = true;   // first block after prepare/reset: no ramps from silence

    std::array<std::vector<float>, 2> post;   // stereo post-chain buffers
    std::array<std::vector<float>, 2> chainR; // second chain's cab output in dual mode
    std::vector<float> monoIn;
    InputFold fold;         // stereo input -> the single chain (Summed mode)
    InputFold analysisFold; // stereo input -> the tuner in Dual mode
};

Engine::Engine() : impl_ (std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::prepare (double sampleRate, int maxBlockSize, int numInputs, int numOutputs)
{
    assert (numInputs >= 1 && numInputs <= 2 && numOutputs >= 1 && numOutputs <= 2);
    const int block = std::max (1, maxBlockSize);

    auto& d = *impl_;
    d.numChains = numInputs;
    // Both chains are prepared and reset even in mono: custom IRs are published to both, and an
    // idle chain must not keep a half-finished IR crossfade (and its kernel) alive.
    for (auto& chain : d.chains)
        chain.prepare (sampleRate, block, oversampling_.load());

    const ProcessSpec stereo { sampleRate, block, 2 };
    d.eq.prepare (stereo);
    d.doubler.prepare (stereo);
    d.drift.prepare (stereo);
    d.fathom.prepare (stereo);
    d.metronome.prepare (stereo);

    d.outputGain.reset (sampleRate, 0.02);
    d.mute.reset (sampleRate, 0.01);
    d.modeFade.reset (sampleRate, 0.01);
    d.modeFade.setCurrentAndTarget (1.0f);
    // Same rounding as LinearSmoothed, so a chunk can end exactly where the fade-out does.
    d.modeFadeSamples = std::max (1, static_cast<int> (std::floor (0.01 * sampleRate)));
    d.modeFadeLeft = 0;
    d.dual = false; // a re-prepare may drop the second input; the first block snaps the mode

    for (auto* group : { &d.post, &d.chainR })
        for (auto& b : *group)
            b.assign (static_cast<size_t> (block), 0.0f);
    d.monoIn.assign (static_cast<size_t> (block), 0.0f);
    d.fold.prepare (sampleRate);
    d.analysisFold.prepare (sampleRate);

    sampleRate_.store (sampleRate, std::memory_order_release);
    maxBlockSize_.store (block, std::memory_order_release);
    numInputs_.store (numInputs, std::memory_order_release);
    numOutputs_.store (numOutputs, std::memory_order_release);
    latency_.store (d.chains[0].latency(), std::memory_order_release);
    reset();
}

void Engine::reset()
{
    auto& d = *impl_;
    for (auto& chain : d.chains)
        chain.reset();
    d.eq.reset();
    d.doubler.reset();
    d.drift.reset();
    d.fold.reset();
    d.analysisFold.reset();
    d.fathom.reset();
    d.metronome.reset();
    d.snapGains = true;
}

void Engine::setMetronomePattern (const std::array<std::uint8_t, 13>& steps) noexcept
{
    std::uint32_t packed = 0;
    for (size_t i = 0; i < steps.size(); ++i)
        packed |= static_cast<std::uint32_t> (steps[i] & 3u) << (2 * i);
    metronomePattern_.store (packed, std::memory_order_relaxed);
}

int Engine::numCabs() const noexcept
{
    // Every chain the engine owns, prepared or not: an IR installed while running with one input
    // must already be on the second chain when a later prepare() adds it.
    return static_cast<int> (impl_->chains.size());
}

dsp::CabSim& Engine::cab (int index) noexcept
{
    const int last = static_cast<int> (impl_->chains.size()) - 1;
    return impl_->chains[static_cast<size_t> (std::clamp (index, 0, last))].cab;
}

dsp::CaptureStage& Engine::capture (int index) noexcept
{
    // Like the cabs: every chain, prepared or not, so a capture reaches a later second chain.
    const int last = static_cast<int> (impl_->chains.size()) - 1;
    return impl_->chains[static_cast<size_t> (std::clamp (index, 0, last))].capture;
}

void Engine::process (const float* const* inputs, float* const* outputs, int numFrames,
                      const ParamSnapshot& p, const Transport& transport) noexcept
{
    ScopedNoDenormals noDenormals;
    auto& d = *impl_;

    // Settings are derived once per host block.
    const ChainSettings chain = chainSettings (p);

    const int ampModel = std::clamp (p.choice (P::ampModel), 0, 2);
    const auto& bank = kEqBanks[static_cast<size_t> (ampModel)];
    dsp::GraphicEq::Settings eqSettings;
    for (size_t b = 0; b < 9; ++b)
        eqSettings.bandsDb[b] = p[bank.bands[b]];
    eqSettings.lowCutHz = p[bank.lowCut];
    eqSettings.highCutHz = p[bank.highCut];

    const dsp::Doubler::Settings doublerSettings { p[P::doublerSpread], p[P::doublerVariation],
                                                   p[P::doublerWidth], p[P::doublerMix] };
    dsp::Drift::Settings driftSettings;
    driftSettings.enabled = p.flag (P::driftEnabled);
    driftSettings.heads = p.choice (P::driftHeads);
    driftSettings.timeMs = p[P::driftTime];
    driftSettings.sync = p.choice (P::driftSync);
    driftSettings.note = p.choice (P::driftNote);
    driftSettings.tapBpm = p[P::tempo];
    driftSettings.feedback = p[P::driftFeedback];
    driftSettings.tapeMod = p[P::driftTapeMod];
    driftSettings.tapeDrive = p[P::driftTapeDrive];
    driftSettings.bass = p[P::driftBass];
    driftSettings.treble = p[P::driftTreble];
    driftSettings.width = p[P::driftWidth];
    driftSettings.mix = p[P::driftMix];

    dsp::Fathom::Settings fathomSettings;
    fathomSettings.enabled = p.flag (P::fathomEnabled);
    fathomSettings.mode = p.choice (P::fathomMode);
    fathomSettings.mix = p[P::fathomMix];
    fathomSettings.decaySeconds = p[P::fathomDecay];
    fathomSettings.lowCutHz = p[P::fathomLowCut];
    fathomSettings.highCutHz = p[P::fathomHighCut];
    fathomSettings.preDelayMs = p[P::fathomPreDelay];

    dsp::Metronome::Settings metro;
    metro.enabled =
        p.flag (P::metronomeEnabled) && metronomeAllowed_.load (std::memory_order_relaxed);
    metro.volumeDb = p[P::metronomeVolume];
    metro.signature = p.choice (P::metronomeSignature);
    metro.sound = p.choice (P::metronomeSound);
    metro.bpm = p[P::tempo];
    const auto pattern = metronomePattern_.load (std::memory_order_relaxed);
    for (size_t i = 0; i < metro.steps.size(); ++i)
        metro.steps[i] = static_cast<std::uint8_t> ((pattern >> (2 * i)) & 3u);

    const int numInputs = numInputs_.load (std::memory_order_relaxed);
    const int numOutputs = numOutputs_.load (std::memory_order_relaxed);
    const int maxBlock = maxBlockSize_.load (std::memory_order_relaxed);
    const bool wantDual = numInputs == 2 && p.choice (P::stereoMode) == 1;

    const bool analysis = analysisEnabled_.load (std::memory_order_relaxed);
    const float outputTarget = dbToGain (p[P::outputGain]);
    const float muteTarget = analysis && p.flag (P::tunerMute) ? 0.0f : 1.0f;
    if (d.snapGains)
    {
        d.outputGain.setCurrentAndTarget (outputTarget);
        d.mute.setCurrentAndTarget (muteTarget);
        // Start in the wanted stereo mode: there is nothing to fade from yet. (Every chain was
        // reset with the engine.)
        d.dual = wantDual;
        d.modeFade.setCurrentAndTarget (1.0f);
        d.modeFadeLeft = 0;
        d.snapGains = false;
    }
    d.outputGain.setTarget (outputTarget);
    d.mute.setTarget (muteTarget);

    float inPeaks[2] = { 0.0f, 0.0f };
    float outPeaks[2] = { 0.0f, 0.0f };

    for (int start = 0; start < numFrames;)
    {
        int n = std::min (maxBlock, numFrames - start);
        // Stereo mode changes fade out, switch, and fade back in.
        if (wantDual != d.dual)
        {
            if (d.modeFade.target() > 0.5f)
            {
                d.modeFade.setTarget (0.0f);
                d.modeFadeLeft = d.modeFadeSamples;
            }
            else if (!d.modeFade.isSmoothing())
            {
                d.dual = wantDual;
                for (auto& c : d.chains)
                    c.reset();
                // The Doubler only runs in Summed mode; its delay lines still hold audio from
                // before the switch, which its bypass fade would otherwise replay.
                d.doubler.reset();
                d.modeFade.setTarget (1.0f);
                d.modeFadeLeft = 0;
            }
            // End this chunk where the fade-out does, so the switch happens there and not after up
            // to a whole prepared block of silence.
            if (d.modeFadeLeft > 0)
                n = std::min (n, d.modeFadeLeft);
        }
        else if (d.modeFade.target() < 0.5f)
        {
            d.modeFade.setTarget (1.0f); // switched back before the fade-out finished
            d.modeFadeLeft = 0;
        }

        const float* in0 = inputs[0] + start;
        const float* in1 = numInputs == 2 ? inputs[1] + start : nullptr;

        // Mono source for the single chain. In Summed mode the fold averages the two inputs, or
        // takes the live one at unity when the other carries nothing (a guitar on one input of a
        // stereo track would otherwise reach the amp 6 dB down; core/InputFold.h).
        const float* chainIn = in0;
        if (in1 != nullptr && !d.dual)
        {
            d.fold.process (in0, in1, d.monoIn.data(), n);
            chainIn = d.monoIn.data();
        }

        if (analysis)
        {
            // The tuner hears the instrument: the summed input in Dual mode too.
            if (d.dual && in1 != nullptr)
            {
                d.analysisFold.process (in0, in1, d.monoIn.data(), n);
                analysisRing_.push (d.monoIn.data(), static_cast<size_t> (n));
            }
            else
            {
                analysisRing_.push (chainIn, static_cast<size_t> (n));
            }
        }

        std::array<float*, 2> postPtrs { d.post[0].data(), d.post[1].data() };
        const AudioView post (postPtrs.data(), 2, n);

        d.chains[0].process (chainIn, post, chain);
        inPeaks[0] = std::max (inPeaks[0], d.chains[0].inputPeak);
        if (d.dual && in1 != nullptr)
        {
            std::array<float*, 2> rPtrs { d.chainR[0].data(), d.chainR[1].data() };
            const AudioView right (rPtrs.data(), 2, n);
            d.chains[1].process (in1, right, chain);
            inPeaks[1] = std::max (inPeaks[1], d.chains[1].inputPeak);
            // Each chain feeds its own side.
            for (int i = 0; i < n; ++i)
            {
                d.post[0][static_cast<size_t> (i)] = 0.5f * (d.post[0][static_cast<size_t> (i)] +
                                                             d.post[1][static_cast<size_t> (i)]);
                d.post[1][static_cast<size_t> (i)] = 0.5f * (d.chainR[0][static_cast<size_t> (i)] +
                                                             d.chainR[1][static_cast<size_t> (i)]);
            }
        }
        else
        {
            inPeaks[1] = inPeaks[0];
        }

        d.eq.process (post, p.flag (bank.enabled), eqSettings);
        // The Doubler times its double from the pick attacks in the gate's sidechain, which
        // reach it the chain's latency later. With one output it stays off: in
        // the mono fold the double would only comb-filter the part.
        d.doubler.module.setOnsetSource (d.chains[0].sidechain.data(), d.chains[0].latency());
        d.doubler.process (post, p.flag (P::doublerEnabled) && !d.dual && numOutputs == 2,
                           doublerSettings);
        d.drift.setSettings (driftSettings);
        d.drift.process (post, transport);
        d.fathom.setSettings (fathomSettings);
        d.fathom.process (post);
        d.metronome.setSettings (metro);
        // Always: a click that is still sounding when the metronome is switched off rings out.
        // (Returns at once when nothing is sounding.)
        d.metronome.process (post);

        for (int i = 0; i < n; ++i)
        {
            const float g = d.outputGain.next() * d.mute.next() * d.modeFade.next();
            post.channel (0)[i] *= g;
            post.channel (1)[i] *= g;
        }

        if (numOutputs == 2)
        {
            std::copy_n (post.channel (0), n, outputs[0] + start);
            std::copy_n (post.channel (1), n, outputs[1] + start);
            outPeaks[0] = std::max (outPeaks[0], peakOf (outputs[0] + start, n));
            outPeaks[1] = std::max (outPeaks[1], peakOf (outputs[1] + start, n));
        }
        else
        {
            float* out = outputs[0] + start;
            for (int i = 0; i < n; ++i)
                out[i] = 0.5f * (post.channel (0)[i] + post.channel (1)[i]);
            outPeaks[0] = std::max (outPeaks[0], peakOf (out, n));
            outPeaks[1] = outPeaks[0];
        }

        d.modeFadeLeft = std::max (0, d.modeFadeLeft - n);
        start += n;
    }

    for (size_t c = 0; c < 2; ++c)
    {
        raisePeak (telemetry_.inputPeak[c], inPeaks[c]);
        raisePeak (telemetry_.outputPeak[c], outPeaks[c]);
    }
    if (std::max (inPeaks[0], inPeaks[1]) >= 1.0f)
        telemetry_.inputClip.store (true, std::memory_order_relaxed);
    if (std::max (outPeaks[0], outPeaks[1]) >= 1.0f)
        telemetry_.outputClip.store (true, std::memory_order_relaxed);

    // A bypassed module's last reading would freeze on the meters; report neutral values instead.
    const auto& chain0 = d.chains[0];
    telemetry_.gateOpen.store (!chain0.gateRan || chain0.gate.module.isOpen(),
                               std::memory_order_relaxed);
    telemetry_.gateReductionDb.store (chain0.gateRan ? chain0.gate.module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    telemetry_.tideReductionDb.store (chain0.tideRan ? chain0.tide.module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    telemetry_.metronomeStep.store (d.metronome.currentStep(), std::memory_order_relaxed);
    telemetry_.driftTimeMs.store (d.drift.effectiveTimeMs(), std::memory_order_relaxed);
}

} // namespace sedef
