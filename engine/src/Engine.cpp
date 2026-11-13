// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/Engine.h"

#include "sedef/board/BoardSwitch.h"
#include "sedef/board/PedalRack.h"
#include "sedef/core/BypassFader.h"
#include "sedef/core/Denormals.h"
#include "sedef/core/InputFold.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/Doubler.h"
#include "sedef/dsp/Drift.h"
#include "sedef/dsp/Fathom.h"
#include "sedef/dsp/GraphicEq.h"
#include "sedef/dsp/Looper.h"
#include "sedef/dsp/Metronome.h"
#include "sedef/dsp/PitchShifter.h"
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
    int transpose = 0;    // semitones
    PedalSettings pedals; // every pedal's, on the board or not (board/PedalList.inc)
    dsp::AmpSection::Settings amp;
    dsp::CabSim::Settings cab;
    dsp::CaptureStage::Settings capture;
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

int transposeSemitones (const ParamSnapshot& p) noexcept
{
    return std::clamp (static_cast<int> (std::lround (p[P::transpose])), -12, 12);
}

ChainSettings chainSettings (const ParamSnapshot& p)
{
    ChainSettings s;
    s.inputGain = dbToGain (p[P::inputGain]);
    s.transpose = transposeSemitones (p);
    s.pedals = PedalSettings::from (p);

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

/// Hands a post effect's settings to its module. Coming back from bypass, the module restarts from
/// a clean state that lands directly on the current settings (PedalSlot does the same for pedals).
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

/// Wraps a stereo post effect without latency in a click-free bypass.
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

/// One instrument chain: input gain -> Transpose -> the pedalboard -> amp -> cab. Mono in,
/// stereo out.
struct Chain
{
    LinearSmoothed inputGain;
    dsp::PitchShifter transpose;
    PedalRack rack;
    dsp::AmpSection amp;
    dsp::CabSim cab;
    dsp::CaptureStage capture; // the amp or a .nam capture, and the cab or not

    std::vector<float> buffer;
    std::vector<float> sidechain;
    // The gate's sidechain, delayed by Transpose's latency (docs/dsp/transpose.md, section 7).
    std::vector<float> sidechainDelay;
    size_t sidechainMask = 0, sidechainWrite = 0;
    float inputPeak = 0.0f;
    bool snapGain = true;

    void prepare (double sampleRate, int maxBlock, OversamplingMode oversampling)
    {
        const ProcessSpec mono { sampleRate, maxBlock, 1, oversampling };
        inputGain.reset (sampleRate, 0.02);
        transpose.prepare (mono);
        size_t size = 1;
        while (size < static_cast<size_t> (dsp::PitchShifter::latencyFor (12, sampleRate)) + 1)
            size <<= 1;
        sidechainDelay.assign (size, 0.0f);
        sidechainMask = size - 1;
        rack.prepare (mono);
        amp.prepare (mono);
        capture.prepare (mono, amp.latencySamples());
        cab.prepare ({ sampleRate, maxBlock, 2 });
        buffer.assign (static_cast<size_t> (maxBlock), 0.0f);
        sidechain.assign (static_cast<size_t> (maxBlock), 0.0f);
    }

    /// From the pedalboard's input on, without Transpose (which the gate's sidechain shares).
    int latency() const noexcept
    {
        // Every pedal's latency, whatever is on the board (PedalRack keeps the delays).
        return rack.latencySamples() + capture.latencySamples() + cab.latencySamples();
    }

    void reset() noexcept
    {
        snapGain = true;
        transpose.reset();
        std::fill (sidechainDelay.begin(), sidechainDelay.end(), 0.0f);
        sidechainWrite = 0;
        rack.reset();
        amp.reset();
        capture.reset();
        cab.reset();
    }

    /// `duck` fades the board in and out while it is rewired.
    void process (const float* in, const AudioView& stereoOut, const ChainSettings& s,
                  const BoardSwitch::Gains& duck,
                  const std::array<bool, kNumSlots>& leaving) noexcept
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

        transpose.setSettings ({ s.transpose });
        transpose.process (x, n);
        delaySidechain (n, transpose.latencySamples());

        // Ducked on the way in and out while the board rewires (BoardSwitch).
        BoardSwitch::apply (x, duck.input, n);
        rack.process (x, sidechain.data(), n, s.pedals, leaving);
        BoardSwitch::apply (x, duck.output, n);

        capture.setSettings (s.capture);
        capture.process (amp, s.amp, AudioView (x, n));
        capture.processCab (cab, s.cab, x, stereoOut);
    }

    /// Delays the sidechain by `delay` samples (at most Transpose's largest latency), so the gate
    /// hears an attack when the shifted attack reaches it.
    void delaySidechain (int n, int delay) noexcept
    {
        float* sc = sidechain.data();
        const auto d = static_cast<size_t> (delay);
        for (int i = 0; i < n; ++i)
        {
            sidechainDelay[sidechainWrite] = sc[i];
            sc[i] = sidechainDelay[(sidechainWrite - d) & sidechainMask];
            sidechainWrite = (sidechainWrite + 1) & sidechainMask;
        }
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
    dsp::Looper looper;
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

    // The pedals after the amp and cab: one stereo rack, after the EQ (docs/board.md §4.1).
    PedalRack postRack;

    // Take the racks to the newest order of their side: both chains' racks in front of the amp,
    // the one after it (nothing there until a board comes).
    BoardSwitch preSwitch;
    BoardSwitch postSwitch { PedalOrder {} };

    std::array<PedalRack*, 2> preRacks() noexcept { return { &chains[0].rack, &chains[1].rack }; }
    std::array<PedalRack*, 1> postRacks() noexcept { return { &postRack }; }
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
    d.looper.prepare (stereo);
    d.metronome.prepare (stereo);
    d.postRack.prepare (stereo, Side::post);

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
    d.preSwitch.prepare (sampleRate, block, d.chains[0].rack.latencySamples());
    d.postSwitch.prepare (sampleRate, block, d.postRack.latencySamples());

    sampleRate_.store (sampleRate, std::memory_order_release);
    maxBlockSize_.store (block, std::memory_order_release);
    numInputs_.store (numInputs, std::memory_order_release);
    numOutputs_.store (numOutputs, std::memory_order_release);
    latency_.store (d.chains[0].latency(), std::memory_order_release);
    transposeLatency_.store (d.chains[0].transpose.latencySamples(), std::memory_order_release);
    reset();
}

void Engine::expectTranspose (float semitones) noexcept
{
    auto& d = *impl_;
    const int st = std::clamp (static_cast<int> (std::lround (semitones)), -12, 12);
    for (auto& chain : d.chains)
        chain.transpose.setSettings ({ st });
    transposeLatency_.store (d.chains[0].transpose.latencySamples(), std::memory_order_release);
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
    d.looper.reset();
    d.metronome.reset();
    d.postRack.reset();
    d.snapGains = true;
    // Audio is stopped: the newest board applies at once.
    Board target;
    target.pre = d.preSwitch.target();
    target.post = d.postSwitch.target();
    board_.read (target);
    d.preSwitch.reset (target.pre, d.preRacks());
    d.postSwitch.reset (target.post, d.postRacks());
}

void Engine::setBoard (const Board& board) noexcept
{
    board_.write (board);
}

void Engine::setMetronomePattern (const std::array<std::uint8_t, 13>& steps) noexcept
{
    std::uint32_t packed = 0;
    for (size_t i = 0; i < steps.size(); ++i)
        packed |= static_cast<std::uint32_t> (steps[i] & 3u) << (2 * i);
    metronomePattern_.store (packed, std::memory_order_relaxed);
}

bool Engine::prepareLooper()
{
    if (looperMemoryOwner_ == nullptr)
    {
        looperMemoryOwner_ = std::make_unique<dsp::LoopMemory>();
        looperMemory_.store (looperMemoryOwner_.get(), std::memory_order_release);
    }
    return true;
}

void Engine::looperCommand (LooperCommand command) noexcept
{
    if (!looperReady())
        return;
    const auto c = static_cast<std::uint8_t> (command);
    looperCommands_.push (&c, 1);
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
    // Drums (docs/drums.md): the metronome's second sound, on its grid.
    metro.mode = p.choice (P::metronomeMode);
    metro.drums.groove = p.choice (P::drumsGroove);
    metro.drums.intensity = p.choice (P::drumsIntensity) + 1;
    {
        constexpr std::array<int, 4> kFillEvery { 0, 2, 4, 8 };
        metro.drums.fillEvery =
            kFillEvery[static_cast<size_t> (std::clamp (p.choice (P::drumsFills), 0, 3))];
    }
    metro.drumsLevelDb = p[P::drumsLevel];
    metro.drumsTone = p[P::drumsTone];
    const auto pattern = metronomePattern_.load (std::memory_order_relaxed);
    for (size_t i = 0; i < metro.steps.size(); ++i)
        metro.steps[i] = static_cast<std::uint8_t> ((pattern >> (2 * i)) & 3u);

    // The looper: its memory arrives once (prepareLooper), its presses at a block start.
    auto* loopMemory = looperMemory_.load (std::memory_order_acquire);
    d.looper.attach (loopMemory);
    std::array<std::uint8_t, 16> presses {};
    int numPresses = 0;
    if (loopMemory != nullptr)
        numPresses = static_cast<int> (looperCommands_.pop (presses.data(), presses.size()));
    dsp::Looper::Settings loop;
    loop.levelDb = p[P::looperLevel];
    loop.feedback = p[P::looperFeedback] / 100.0f;
    loop.countIn = p.flag (P::looperCountIn);
    loop.clickBpm = metro.bpm;
    loop.clickSignature = metro.signature;
    // The device's and the rig's latency; Transpose's share follows its setting (below).
    const int looperLatency = looperDeviceLatency_.load (std::memory_order_relaxed) +
                              latency_.load (std::memory_order_relaxed);

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
    if (Board next; board_.read (next)) // a block boundary: the newest board, if one came
    {
        d.preSwitch.setTarget (next.pre);
        d.postSwitch.setTarget (next.post);
    }

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
        // Each side of the amp moves to its new order on its own; the chunk ends where either
        // side's fade to silence does.
        n = d.preSwitch.step (n, d.preRacks());
        n = d.postSwitch.step (n, d.postRacks());
        const auto duck = d.preSwitch.duckGains (n);
        const auto postDuck = d.postSwitch.duckGains (n);

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

        d.chains[0].process (chainIn, post, chain, duck, d.preSwitch.leaving());
        inPeaks[0] = std::max (inPeaks[0], d.chains[0].inputPeak);
        if (d.dual && in1 != nullptr)
        {
            std::array<float*, 2> rPtrs { d.chainR[0].data(), d.chainR[1].data() };
            const AudioView right (rPtrs.data(), 2, n);
            d.chains[1].process (in1, right, chain, duck, d.preSwitch.leaving());
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
        // The pedals after the amp, ducked on their own while that side rewires. (Nothing to do,
        // and no audio touched, while none stand there.)
        for (int c = 0; c < 2; ++c)
            BoardSwitch::apply (post.channel (c), postDuck.input, n);
        d.postRack.process (post, chain.pedals, d.postSwitch.leaving());
        for (int c = 0; c < 2; ++c)
            BoardSwitch::apply (post.channel (c), postDuck.output, n);
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
        if (loopMemory != nullptr)
        {
            // The looper records the rig without the click and adds the loop (docs/looper.md).
            // Downbeats come from the click's own scheduler, so both agree on every sample.
            std::array<dsp::Looper::Event, 24> events {};
            int numEvents = 0;
            std::array<int, 8> downbeats {};
            const int numDownbeats =
                d.metronome.downbeats (n, downbeats.data(), static_cast<int> (downbeats.size()));
            for (int k = 0; k < numDownbeats; ++k)
                events[static_cast<size_t> (numEvents++)] = { downbeats[static_cast<size_t> (k)],
                                                              dsp::Looper::Event::Kind::downbeat };
            if (start == 0)
                for (int k = 0; k < numPresses; ++k)
                    events[static_cast<size_t> (numEvents++)] = {
                        0, dsp::Looper::Event::Kind::command,
                        static_cast<dsp::Looper::Command> (presses[static_cast<size_t> (k)])
                    };
            loop.clickRunning = d.metronome.isRunning();
            loop.barSamples = d.metronome.barSamples();
            loop.latencySamples = looperLatency + d.chains[0].transpose.latencySamples();
            d.looper.setSettings (loop);
            d.looper.process (post, events.data(), numEvents);
            // A loop recorded in bars keeps the click locked to its passes.
            for (int k = 0; k < d.looper.numWraps(); ++k)
                d.metronome.alignDownbeat (d.looper.wrap (k));
        }
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
    // (A pedal off the board doesn't run either.)
    const auto& rack0 = d.chains[0].rack;
    const auto& pedals = rack0.slots();
    const bool gateRan = rack0.ran (PedalId::gate);
    const bool tideRan = rack0.ran (PedalId::tide);
    telemetry_.gateOpen.store (!gateRan || pedals.gate[0].module.isOpen(),
                               std::memory_order_relaxed);
    telemetry_.gateReductionDb.store (gateRan ? pedals.gate[0].module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    telemetry_.tideReductionDb.store (tideRan ? pedals.tide[0].module.gainReductionDb() : 0.0f,
                                      std::memory_order_relaxed);
    const bool gate2Ran = rack0.ran ({ PedalId::gate, 1 });
    const bool tide2Ran = rack0.ran ({ PedalId::tide, 1 });
    telemetry_.gate2Open.store (!gate2Ran || pedals.gate[1].module.isOpen(),
                                std::memory_order_relaxed);
    telemetry_.tide2ReductionDb.store (tide2Ran ? pedals.tide[1].module.gainReductionDb() : 0.0f,
                                       std::memory_order_relaxed);
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        telemetry_.pedalRan[slot].store (rack0.ran (slotRef (slot)) ||
                                             d.postRack.ran (slotRef (slot)),
                                         std::memory_order_relaxed);
    telemetry_.boardChanging.store (d.preSwitch.changing (rack0) ||
                                        d.postSwitch.changing (d.postRack),
                                    std::memory_order_relaxed);
    telemetry_.metronomeStep.store (d.metronome.currentStep(), std::memory_order_relaxed);
    {
        const auto& lp = d.looper;
        int flags = 0;
        flags |= lp.canUndo() ? kLooperCanUndo : 0;
        flags |= lp.canRedo() ? kLooperCanRedo : 0;
        flags |= lp.undoIsClear() ? kLooperUndoIsClear : 0;
        flags |= lp.synced() ? kLooperSynced : 0;
        flags |= lp.armedForPlay() ? kLooperArmedForPlay : 0;
        telemetry_.looperState.store (static_cast<int> (lp.state()), std::memory_order_relaxed);
        telemetry_.looperFlags.store (flags, std::memory_order_relaxed);
        telemetry_.looperLength.store (lp.length(), std::memory_order_relaxed);
        telemetry_.looperPosition.store (lp.position(), std::memory_order_relaxed);
        telemetry_.looperBars.store (lp.bars(), std::memory_order_relaxed);
        telemetry_.looperWait.store (lp.waitBars(), std::memory_order_relaxed);
    }
    telemetry_.driftTimeMs.store (d.drift.effectiveTimeMs(), std::memory_order_relaxed);
    transposeLatency_.store (d.chains[0].transpose.latencySamples(), std::memory_order_release);
}

} // namespace sedef
