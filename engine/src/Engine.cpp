// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/Engine.h"

#include "sedef/core/BypassFader.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Denormals.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Doubler.h"
#include "sedef/dsp/Drift.h"
#include "sedef/dsp/Fathom.h"
#include "sedef/dsp/GraphicEq.h"
#include "sedef/dsp/Metronome.h"
#include "sedef/dsp/NoiseGate.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/PitchShifter.h"
#include "sedef/dsp/Ripple.h"
#include "sedef/dsp/Tide.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/cab/CabSim.h"

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
    int semitones = 0;
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
};

dsp::CabSim::MicSettings micSettings (const ParamSnapshot& p, int mic, int cab)
{
    // Mic A and B parameters are laid out identically; B follows A in the schema.
    const auto base = [mic] (P a, P b) { return mic == 0 ? a : b; };
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
    return m;
}

ChainSettings chainSettings (const ParamSnapshot& p)
{
    ChainSettings s;
    s.semitones = static_cast<int> (std::lround (p[P::transpose]));
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
    a.abyss = { p.flag (P::abyssBright), p[P::abyssGain],     p[P::abyssBass],
                p[P::abyssMid],          p[P::abyssTreble],   p.flag (P::abyssShift),
                p[P::abyssPresence],     p[P::abyssMaster],   p[P::abyssOutput] };

    // The cab follows the amp while linked; otherwise the user picks it.
    s.cab.cab = p.flag (P::ampCabLink) ? a.model
                                       : std::clamp (p.choice (P::cabModel), 0, dsp::CabSim::kNumCabs - 1);
    for (int m = 0; m < dsp::CabSim::kNumMics; ++m)
        s.cab.mics[static_cast<size_t> (m)] = micSettings (p, m, s.cab.cab);
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
        if (snapOnNextBlock)
        {
            fader.snap (enabled);
            snapOnNextBlock = false;
        }
        const bool wasBypassed = fader.isFullyBypassed();
        fader.setEnabled (enabled);
        module.setSettings (settings);

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
            return false;
        }
        if (wasBypassed)
            module.reset(); // stale state is masked by the fade-in

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
        if (snapOnNextBlock)
        {
            fader.snap (enabled);
            snapOnNextBlock = false;
        }
        const bool wasBypassed = fader.isFullyBypassed();
        fader.setEnabled (enabled);
        module.setSettings (settings);
        if (fader.isFullyBypassed())
            return;
        if (wasBypassed)
            module.reset();

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

/// One instrument chain: transpose -> input gain -> gate -> pedals -> amp -> cab. Mono in,
/// stereo out.
struct Chain
{
    dsp::PitchShifter transpose;
    LinearSmoothed inputGain;
    Slot<dsp::NoiseGate> gate;
    Slot<dsp::Tide> tide;
    Slot<dsp::Pearl> pearl;
    Slot<dsp::Coral> coral;
    Slot<dsp::Ripple> ripple;
    dsp::AmpSection amp;
    dsp::CabSim cab;

    std::vector<float> buffer;
    std::vector<float> sidechain;
    float inputPeak = 0.0f;
    bool gateRan = false, tideRan = false;
    bool snapGain = true;

    void prepare (double sampleRate, int maxBlock)
    {
        const ProcessSpec mono { sampleRate, maxBlock, 1 };
        transpose.prepare (mono);
        inputGain.reset (sampleRate, 0.02);
        gate.prepare (mono);
        tide.prepare (mono);
        pearl.prepare (mono);
        coral.prepare (mono);
        ripple.prepare (mono);
        amp.prepare (mono);
        cab.prepare ({ sampleRate, maxBlock, 2 });
        buffer.assign (static_cast<size_t> (maxBlock), 0.0f);
        sidechain.assign (static_cast<size_t> (maxBlock), 0.0f);
    }

    int latency() const noexcept
    {
        return pearl.latency + coral.latency + amp.latencySamples() + cab.latencySamples();
    }

    void reset() noexcept
    {
        snapGain = true;
        transpose.reset();
        gate.reset();
        tide.reset();
        pearl.reset();
        coral.reset();
        ripple.reset();
        amp.reset();
        cab.reset();
    }

    void process (const float* in, const AudioView& stereoOut, const ChainSettings& s) noexcept
    {
        const int n = stereoOut.numFrames();
        float* x = buffer.data();
        std::copy_n (in, n, x);

        transpose.setSettings ({ s.semitones });
        transpose.process (AudioView (x, n));
        // The gate listens to the instrument before input gain (so it follows the DI level), but
        // after transpose, so its timing matches the shifted signal it gates.
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

        gateRan = gate.process (x, n, s.gateOn, s.gate, [this] (dsp::NoiseGate& g, const AudioView& v) {
            g.process (v, sidechain.data());
        });
        const auto run = [] (auto& m, const AudioView& v) { m.process (v); };
        tideRan = tide.process (x, n, s.tideOn, s.tide, run);
        pearl.process (x, n, s.pearlOn, s.pearl, run);
        coral.process (x, n, s.coralOn, s.coral, run);
        ripple.process (x, n, s.rippleOn, s.ripple, run);

        amp.setSettings (s.amp);
        amp.process (AudioView (x, n));

        cab.setSettings (s.cab);
        cab.process (x, stereoOut);
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
    bool snapGains = true;   // first block after prepare/reset: no ramps from silence

    std::array<std::vector<float>, 2> post;  // stereo post-chain buffers
    std::array<std::vector<float>, 2> chainR; // second chain's cab output in dual mode
    std::vector<float> monoIn;
};

Engine::Engine() : impl_ (std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::prepare (double sampleRate, int maxBlockSize, int numInputs, int numOutputs)
{
    assert (numInputs >= 1 && numInputs <= 2 && numOutputs >= 1 && numOutputs <= 2);
    const int block = std::max (1, maxBlockSize);

    auto& d = *impl_;
    d.numChains = numInputs;
    for (int c = 0; c < d.numChains; ++c)
        d.chains[static_cast<size_t> (c)].prepare (sampleRate, block);

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
    d.dual = false; // a re-prepare may drop the second input; the mode is re-evaluated per chunk

    for (auto* group : { &d.post, &d.chainR })
        for (auto& b : *group)
            b.assign (static_cast<size_t> (block), 0.0f);
    d.monoIn.assign (static_cast<size_t> (block), 0.0f);

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
    for (int c = 0; c < d.numChains; ++c)
        d.chains[static_cast<size_t> (c)].reset();
    d.eq.reset();
    d.doubler.reset();
    d.drift.reset();
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
    return impl_->numChains;
}

dsp::CabSim& Engine::cab (int index) noexcept
{
    return impl_->chains[static_cast<size_t> (std::clamp (index, 0, impl_->numChains - 1))].cab;
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

    const dsp::Doubler::Settings doublerSettings { p[P::doublerSpread] };
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
    metro.enabled = p.flag (P::metronomeEnabled) && metronomeAllowed_.load (std::memory_order_relaxed);
    metro.volumeDb = p[P::metronomeVolume];
    metro.signature = p.choice (P::metronomeSignature);
    metro.sound = p.choice (P::metronomeSound);
    metro.bpm = p[P::tempo];
    const auto pattern = metronomePattern_.load (std::memory_order_relaxed);
    for (size_t i = 0; i < metro.steps.size(); ++i)
        metro.steps[i] = static_cast<std::uint8_t> ((pattern >> (2 * i)) & 3u);

    const bool analysis = analysisEnabled_.load (std::memory_order_relaxed);
    const float outputTarget = dbToGain (p[P::outputGain]);
    const float muteTarget = analysis && p.flag (P::tunerMute) ? 0.0f : 1.0f;
    if (d.snapGains)
    {
        d.outputGain.setCurrentAndTarget (outputTarget);
        d.mute.setCurrentAndTarget (muteTarget);
        d.snapGains = false;
    }
    d.outputGain.setTarget (outputTarget);
    d.mute.setTarget (muteTarget);

    const int numInputs = numInputs_.load (std::memory_order_relaxed);
    const int numOutputs = numOutputs_.load (std::memory_order_relaxed);
    const int maxBlock = maxBlockSize_.load (std::memory_order_relaxed);

    float inPeaks[2] = { 0.0f, 0.0f };
    float outPeaks[2] = { 0.0f, 0.0f };

    for (int start = 0; start < numFrames; start += maxBlock)
    {
        const int n = std::min (maxBlock, numFrames - start);
        // Stereo mode changes fade out, switch, and fade back in (checked per chunk so a long host
        // block doesn't hold the output silent).
        const bool wantDual = numInputs == 2 && p.choice (P::stereoMode) == 1;
        if (wantDual != d.dual)
        {
            if (d.modeFade.target() > 0.5f)
            {
                d.modeFade.setTarget (0.0f);
            }
            else if (!d.modeFade.isSmoothing())
            {
                d.dual = wantDual;
                for (int c = 0; c < d.numChains; ++c)
                    d.chains[static_cast<size_t> (c)].reset();
                d.modeFade.setTarget (1.0f);
            }
        }
        else if (d.modeFade.target() < 0.5f)
        {
            d.modeFade.setTarget (1.0f); // switched back before the fade-out finished
        }

        const float* in0 = inputs[0] + start;
        const float* in1 = numInputs == 2 ? inputs[1] + start : nullptr;

        // Mono source for the single chain (summed at -6 dB in stereo-summed mode).
        const float* chainIn = in0;
        if (in1 != nullptr && !d.dual)
        {
            for (int i = 0; i < n; ++i)
                d.monoIn[static_cast<size_t> (i)] = 0.5f * (in0[i] + in1[i]);
            chainIn = d.monoIn.data();
        }

        if (analysis)
        {
            // The tuner hears the instrument: the summed input in Dual mode too.
            if (d.dual && in1 != nullptr)
            {
                for (int i = 0; i < n; ++i)
                    d.monoIn[static_cast<size_t> (i)] = 0.5f * (in0[i] + in1[i]);
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
                d.post[0][static_cast<size_t> (i)] =
                    0.5f * (d.post[0][static_cast<size_t> (i)] + d.post[1][static_cast<size_t> (i)]);
                d.post[1][static_cast<size_t> (i)] =
                    0.5f * (d.chainR[0][static_cast<size_t> (i)] + d.chainR[1][static_cast<size_t> (i)]);
            }
        }
        else
        {
            inPeaks[1] = inPeaks[0];
        }

        d.eq.process (post, p.flag (bank.enabled), eqSettings);
        d.doubler.process (post, p.flag (P::doublerEnabled) && !d.dual, doublerSettings);
        d.drift.setSettings (driftSettings);
        d.drift.process (post, transport);
        d.fathom.setSettings (fathomSettings);
        d.fathom.process (post);
        d.metronome.setSettings (metro);
        if (metro.enabled || d.metronome.currentStep() >= 0)
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
    telemetry_.gateOpen.store (!chain0.gateRan || chain0.gate.module.isOpen(), std::memory_order_relaxed);
    telemetry_.gateReductionDb.store (chain0.gateRan ? chain0.gate.module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    telemetry_.tideReductionDb.store (chain0.tideRan ? chain0.tide.module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    telemetry_.metronomeStep.store (d.metronome.currentStep(), std::memory_order_relaxed);
    telemetry_.driftTimeMs.store (d.drift.effectiveTimeMs(), std::memory_order_relaxed);
}

} // namespace sedef
