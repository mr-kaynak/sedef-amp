# Amp section: Lagoon, Reef, Abyss

Code: `engine/include/sedef/dsp/amp/` (`AmpSection.h` public; `AmpModels.h`, `AmpStages.h`,
`ToneStack.h` internal), `engine/src/dsp/amp/`. Tests: `engine/tests/amp/`.

## 1. What it models

Three tube-amp voicings built from the same small set of parts: triode gain stages, coupling and
cathode filters, a passive three-knob tone stack, a cathode follower and a push-pull power amp
with supply sag. They are not circuit-exact simulations of any product; each is a design in the
style of a well-known amp family, tuned for feel and for sitting well in front of the cab.

| Model | Intent |
|---|---|
| **Lagoon** (clean) | Glassy, chimey, wide headroom. Clean at any guitar level with volume and master at noon; breaks up only when both are high. Touch-sensitive second-harmonic warmth, scooped mids, sparkle that fades as volume goes up (bright cap). |
| **Reef** (crunch) | British-style crunch: cascaded stages, a cold-biased second stage, a cathode follower into the tone stack, more power-amp sag. Cleans up with picking dynamics; three voicings. |
| **Abyss** (lead) | High gain that stays tight and articulate: four stages with coupling high-passes up to 160 Hz keeping low end out of the clippers, low-pass poles between stages against fizz, a movable mid scoop, presence and resonance in the power amp to restore weight and bite after the distortion. |

Signals are in normalised units: a triode stage starts to bend audibly around |x| = 1. A guitar
DI at -18 dBFS RMS (strum peaks around -6 dBFS) is the design input level.

## 2. Signal flow

`[base]` runs at the host rate, `[OS]` inside `core::Oversampler` (4x at <= 48 kHz, 2x at
<= 96 kHz, 1x above, so the oversampled rate is always >= 176.4 kHz). Linear filters before the
first and after the last nonlinearity run at the base rate; everything in between runs
oversampled, so only one up/down round trip is paid.

```
Lagoon
[base] in ─ HPF 10 Hz ─►
[OS]   V1 triode ─ Miller LP 20k ─ tone stack (tweed values; treble .68, mid .32 fixed; BASS)
       ─ bright shelf (VOLUME-dependent) ─ HPF 18 Hz ─ cathode shelf -3 dB < 90 Hz
       ─ V2 triode (drive = VOLUME) ─ LP 16k ─ HPF 15 Hz ─ power amp (drive = MASTER, gentle sag) ─►
[base] transformer HPF 35 Hz (2nd order) ─ LP 16k ─ CUT low-pass 12k..3k ─ level (VOLUME/MASTER comp, OUTPUT)

Reef
[base] in ─ HPF 15 Hz ─ V1a cathode shelf (VOICE) ─►
[OS]   V1a triode ─ LP 18k ─ HPF 25 Hz ─ bright shelf (VOICE, GAIN) ─ V1b cold triode (drive = GAIN)
       ─ LP 14k ─ top shelf (VOICE) ─ HPF 40 Hz ─ PRESENCE shelf ─ V2a triode ─ LP 12k
       ─ cathode follower ─ tone stack (British values; BASS MID TREBLE) ─ weight shelf (VOICE)
       ─ HPF 20 Hz ─ power amp (drive = MASTER, more sag) ─►
[base] transformer HPF 40 Hz ─ LP 15k ─ level (GAIN/MASTER comp, OUTPUT)

Abyss
[base] in ─ HPF 25 Hz ─ V1 cathode shelf -5 dB < 150 Hz ─►
[OS]   V1 triode ─ LP 14k ─ HPF 70 Hz ─ BRIGHT shelf (GAIN-dependent) ─ V2 cold clipper (drive = GAIN)
       ─ LP 9k ─ HPF 160 Hz ─ cathode shelf -4 dB < 300 Hz ─ V3 ─ LP 7k ─ HPF 50 Hz ─ V4 ─ LP 7k
       ─ cathode follower ─ tone stack (BASS MID TREBLE; SHIFT moves the scoop) ─ HPF 20 Hz
       ─ resonance +4.5 dB < 100 Hz ─ PRESENCE shelf ─ power amp (drive = MASTER) ─►
[base] transformer HPF 45 Hz ─ LP 14k ─ level (GAIN/MASTER comp, OUTPUT)
```

### Building blocks (`AmpStages.h`)

- **Triode stage.** Static curve with unity slope at zero: the positive (grid-conduction) side
  bends softly, `u / sqrt(1 + (u/soft)^2)`; the negative (cutoff) side limits earlier and firmer,
  `hard * tanh(u/hard)` (rational tanh). A per-stage `bias` places the operating point up the
  curve, which gives the small-signal second harmonic of a real stage; the static offset is
  subtracted so silence stays exactly silent. **Grid-conduction bias shift:** once the input swings
  past a conduction threshold, a fast-attack (1.5 ms) / slow-release (60-80 ms) state pushes the
  bias colder, so the stage compresses and clips more asymmetrically when hit hard (bloom). The
  shift saturates at a per-stage depth so a hard pick never chokes the decay (blocking).
- **Coupling, cathode and Miller filters** are first-order sections around each stage: coupling
  high-passes set how much low end reaches the next clipper (the main tightness control in a
  high-gain design), partly bypassed cathodes are low shelves, plate/Miller capacitance are
  low-passes that keep stacked distortion from turning into fizz.
- **Bright cap.** A capacitor across the top of a volume/gain pot bypasses highs around the pot,
  so the lift is large at low settings and disappears at full rotation. Modelled as a high shelf
  whose gain falls with the pot setting.
- **Parallel-form shelves** (`MixShelf`): every shelf that moves at run time and sits ahead of or
  between clipping stages (bright, presence, voicings) is `x + (g - 1) * onepole(x)` with a fixed
  pole. Only the gain `g` moves, ramped per sample. Re-designing a recursive filter on the fly
  leaves small state transients that 30+ dB of downstream gain turns into audible ticks; this form
  has none.
- **Cathode follower.** Unity gain with a soft one-sided limit once the grid swings above the
  cathode. Its DC goes nowhere: the tone stack after it has no DC path.
- **Power amp.** Symmetric clip `u / (1 + u^4)^(1/4)` (within 0.2 dB of linear up to half swing,
  then a quick rounded knee), with supply sag: an envelope of the output (attack 5-10 ms, release
  120-200 ms) lowers the clip level and, slightly, the gain. That is the compression and bloom of
  a tube rectifier. Presence and resonance, normally produced by frequency-dependent negative
  feedback, are shelves ahead of the clipper so they shape its clipping as well.
- **Output transformer** bandwidth: a 2nd-order high-pass (35-45 Hz) and a first-order low-pass
  (14-16 kHz) at the base rate. The cab simulator that follows does the speaker roll-off, so the
  amp output is not pre-darkened.

### Tone stack (`ToneStack.h`)

The three-knob passive "FMV" stack is solved in closed form following Yeh & Smith (DAFx-06):

```
Vin ─C1─ A ─(1-t)R1─ Vout ─t·R1─ B ─l·R2─ C ─(1-m)R3─ W ─m·R3─ gnd
Vin ─R4─ D ─C2─ B
         D ─C3─ W
```

H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3), where every coefficient is a
polynomial in the component values and the pot positions t (treble), m (mid), l (bass). The
coefficients were re-derived from the nodal equations with sympy
(`tools/tonestack/derive_fmv.py`) and agree exactly with the paper's eq. 3. The stack assumes an
ideal source (the cathode follower, or a low-impedance stage) and an unloaded output.

Discretisation: bilinear transform without pre-warp at the oversampled rate, in double precision
(the poles sit at a few hertz, very close to z = 1 at 192 kHz). Coefficients are recomputed every
`kControlBlock` samples while a knob is moving. Pots: treble and mid linear, bass log taper
(10 % of the resistance at mid rotation).

| Set | C1 | C2 | C3 | R1 (treble) | R2 (bass) | R3 (mid) | R4 (slope) | Used by |
|---|---|---|---|---|---|---|---|---|
| `kTweedStack` | 250 pF | 20 nF | 20 nF | 250k | 1M | 25k | 56k | Lagoon |
| `kBritishStack` | 470 pF | 22 nF | 22 nF | 220k | 1M | 25k | 33k | Reef |
| Abyss | 470 pF | 47 nF → 18 nF | 47 nF → 18 nF | 220k | 1M | 25k | 33k | Abyss (SHIFT) |

Abyss's SHIFT glides C2 and C3 (geometrically, over 80 ms) from 47 nF to 18 nF, which moves the
scoop from about 480 Hz to about 800 Hz at noon settings.

## 3. Control mapping

All knobs are 0..1 and are smoothed with 30 ms linear ramps; switches (voice, bright, shift) glide
over 80 ms. Gains ramp per sample; filter coefficients follow every 32 samples.

| Control | Mapping |
|---|---|
| Lagoon `volume` | V2 drive, exponential 0.12 → 7 (x 3.2 stack make-up). Bright shelf (~1.6-3.5 kHz transition): `7 dB * (1 - volume)^1.5`. |
| Lagoon `bass` | Tone stack bass pot, log taper. Treble and mid are fixed at 0.68 / 0.32. |
| Lagoon `cut` | First-order low-pass after the power amp, 12 kHz (0) → 3 kHz (1), exponential. |
| Reef `voice` | Normal / Thick / Thicker; continuous morph of four shelves: V1a cathode (-7 / -4 / -1.5 dB, transition ~150-350 Hz), bright cap max (8 / 4.5 / 1.5 dB), top after V1b (0 / -3 / -6 dB, ~2.5-5 kHz), weight after the stack (0 / +1.5 / +3 dB below ~110 Hz). |
| Reef `gain` | V1b drive, exponential 0.35 → 16. Bright shelf scales by `(1 - gain)^1.3`. |
| Reef `presence` | Preamp high shelf ahead of V2a: `-12 dB * (1 - presence)^1.3`, transition ~1.5-6 kHz. Clockwise = more presence (flat at 1). |
| Abyss `bright` | Bright cap: high shelf (~1.2-3.5 kHz transition), 9 dB at gain 0 falling to 1.5 dB at gain 1 (`sqrt(gain)` blend). |
| Abyss `gain` | V2 drive, exponential 0.6 → 30. |
| Abyss `shift` | Tone stack mid scoop ~480 Hz → ~800 Hz. |
| Abyss `presence` | Power-amp high shelf, 0 → +10 dB (transition ~1.6-5 kHz at full). |
| `bass` / `mid` / `treble` (Reef, Abyss) | Tone stack pots; bass log taper, mid and treble linear. |
| `master` (all) | Power-amp drive, exponential 0.3 → 3.5 (Reef and Abyss also apply their tone-stack make-up here). |
| `output` (all) | -24 dB at 0, 0 dB at 0.5, +12 dB at 1, linear in dB on each half. Tone-neutral. |

**Level compensation.** Turning a drive knob up raises the level only by part of the drive change
(`drive^(1 - c)` while the stages are still linear: c = 0.5 for Lagoon volume and master, 0.18 / 0.35
for Reef gain / master, 0.08 / 0.35 for Abyss gain / master); the rest becomes saturation. Each
knob feels like turning up an amp without jumping tens of decibels.

**Gain staging.** With every knob at its default (0.5; Reef Normal; Abyss bright on, shift off),
the synthetic strummed-chord DI at -18 dBFS RMS comes out at -15.0 dBFS RMS from all three
models (within 0.1 dB at 44.1, 48 and 96 kHz), so A/B comparisons are fair.

**Model switching.** A model change crossfades over 30 ms (smoothstep curve) between the old and
new model; the incoming model is reset first so it carries no stale tail. Only the active model
runs, plus the outgoing one during a fade. A change that arrives mid-fade reverses the fade if it
goes back to the previous model, otherwise it is queued and starts when the fade ends.

**Latency.** `latencySamples()` is the oversampler's round trip (37 samples at 44.1/48 kHz,
31 at 88.2/96 kHz, 0 above). The voicing filters are minimum phase and add about 40 µs of group
delay on top at high frequencies.

## 4. References

- D. T. Yeh and J. O. Smith, "Discretization of the '59 Fender Bassman Tone Stack", Proc. 9th Int.
  Conf. on Digital Audio Effects (DAFx-06), Montreal, 2006. Tone stack topology and coefficients.
- D. T. Yeh, "Digital Implementation of Musical Distortion Circuits by Analysis and Simulation",
  PhD thesis, CCRMA, Stanford University, 2009. Triode stage behaviour, grid conduction,
  cathode follower, oversampling of static nonlinearities.
- J. Pakarinen and D. T. Yeh, "A Review of Digital Techniques for Modeling Vacuum-Tube Guitar
  Amplifiers", Computer Music Journal 33(2), 2009. Block-based (filter + waveshaper) amp
  modelling and its trade-offs.
- M. Blencowe, *Designing Tube Preamps for Guitar and Bass*, 2nd ed., 2012. Coupling and cathode
  bypass corner frequencies, bright caps on volume pots, cold-biased clipper stages, blocking
  distortion.
- R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients" (via
  `core/Biquad`). Bilinear transform: any DSP textbook, e.g. J. O. Smith, *Introduction to
  Digital Filters*, CCRMA, 2007.

No code, data or measurements from other products were used (ADR-0006). Component values in the
tone stack tables are generic values from published schematics and from the paper above.

## 5. Tests (`engine/tests/amp/`, tag `[amp]`)

| Test | Asserts |
|---|---|
| Tone stack vs. nodal analysis | The closed-form H(s) matches an independent complex nodal solve of the network (written in the test, Gaussian elimination) to < 0.01 dB from 20 Hz to 20 kHz, for 32 pot settings x 3 component sets (measured worst: 1e-6 dB). |
| Bilinear transform | Digital response equals the circuit at the bilinear-warped frequency to < 0.1 dB at 44.1, 48, 176.4, 192 kHz. At the oversampled rates the unwarped error is < 0.1 dB to 10 kHz (worst 0.091 dB) and < 0.5 dB to 20 kHz (worst 0.38 dB, all pots at zero). |
| Tone stack filter | The double-precision section produces its designed response (< 0.05 dB). |
| Finite output | Random settings (re-randomised every ~40 ms, including model, voice and switches) with full-scale stereo noise at 44.1 / 48 / 96 kHz and blocks of 1, 17, 64, 512: all finite and bounded. Non-finite and out-of-range settings are sanitised. |
| Silence | After a loud strum and 3 s of silence the output is below -100 dBFS (all models at full drive); digital silence from a cold start stays exactly zero. |
| Gain staging | All three models at defaults are within [-18, -12] dBFS RMS and within 3 dB of each other at 44.1 / 48 / 96 kHz (measured: -15.0 ±0.1 dBFS). |
| THD vs. gain | Reef and Abyss THD (220 Hz sine, -18 dBFS RMS) rises strictly at every 0.1 step of gain and spans at least 3x. |
| Lagoon clean | Volume and master at 0.25: THD < 1 % for a 220 Hz sine at -18 dBFS RMS; both at 1: > 10 %. |
| Model switching | All six model pairs: the largest sample-to-sample step during a switch is within 1.1x of steady state. Model changes every 10 ms (fade reversals and queued third models) also stay within 1.1x. |
| Automation | Each of the 22 knobs and switches swept 0 → 1 → 0 over one second with a new value every 64-sample block: output finite and the largest step within 1.1x of the largest step of any static setting along the sweep. |
| Latency | `latencySamples()` equals the oversampler's at 44.1 / 48 / 96 / 192 kHz; an impulse through a clean Lagoon peaks at that latency plus at most 60 µs (voicing-filter group delay), never before. |
| Channels and blocks | Stereo channels are independent (left equals a mono run bit for bit); block size does not change the output. |

Hidden reports: `"[.voicing]"` prints levels, THD and level curves per drive knob, octave-band
spectra (raw and through a rough cab stand-in), compression curves, the effect of each switch and
worst-case aliasing; `"[.bench]"` prints ns/sample.

Measured on an Apple M2 (RelWithDebInfo, -O3), 48 kHz, 4x oversampling, 64-sample blocks, mono:
Lagoon ~175-210 ns/sample (0.8-1.0 % of one core), Reef ~230-245 ns (1.1-1.2 %), Abyss
~265-285 ns (1.3-1.4 %). A crossfade between two models costs the sum of both. Stereo costs
twice mono. Worst-case aliasing at full gain with a 1.2-4.3 kHz sine: -68 dB or lower re. the
fundamental.

Not covered by an automated test here: allocation-freedom. `prepare` is the only function that
allocates (the oversampler buffers and one 32-frame scratch buffer per channel); this was checked
once with a counting global allocator over 2000 randomised blocks, model switches and resets. The
engine-level allocation test (ADR-0005) covers it permanently.

## 6. Known limitations and future work

- These are block models (filters around static curves with a little state), not circuit
  simulations: no plate-load or source-impedance interaction between stages beyond the bias
  shift, no output-transformer saturation, and negative feedback is approximated by fixed or
  knob-driven shelves rather than a loop around the power amp.
- The tone stack is driven by an ideal source and unloaded. Lagoon's stack in a real amp of this
  style is fed from a plate (higher source impedance), which softens its scoop slightly.
- Lagoon's `cut` sits after the power amp (cheaper, and it tames power-amp edge too). In the
  circuits it is named after it acts in the phase inverter, before the output tubes.
- The bilinear warp of the tone stack reaches 0.38 dB at 20 kHz in its most extreme setting. That
  is above the cab's roll-off, so there is no pre-warp.
- Voicing constants were set by measurement (spectra, THD and compression curves, level
  matching against a synthetic strum), not by a listening panel. A listening pass with real DI
  recordings through the cab should follow, and the constants at the top of each model file are
  the knobs for it.
- No noise or hum is modelled, on purpose: silence in gives silence out.
- Stereo processes the channels one after another. Interleaving them would roughly halve the
  stereo cost if the amp is ever run in stereo (the engine's channel strip is mono).
