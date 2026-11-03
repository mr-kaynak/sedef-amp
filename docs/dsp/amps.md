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
| **Reef** (crunch) | British-style crunch: cascaded stages, a cold-biased second stage, a cathode follower into the tone stack, more power-amp sag. Edge of breakup at the bottom of the gain knob, a hot crunch bordering on lead at the top. Cleans up with picking dynamics; three voicings. |
| **Abyss** (lead) | Modern high gain that stays tight and articulate: four stages with coupling high-passes up to 160 Hz keeping low end out of the clippers, low-pass poles between stages against fizz, a movable mid scoop, presence and resonance in the power amp to restore weight and bite after the distortion. At full gain light picking and single notes are fully saturated and sustain for seconds. |

Signals are in normalised units: a triode stage starts to bend audibly around |x| = 1. A guitar
DI at -18 dBFS RMS (strum peaks around -4 dBFS) is the design input level; section 4 relates it
to volts and to real interfaces.

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
[OS]   V1a triode ─ LP 18k ─ HPF 25 Hz ─ bright shelf (VOICE, GAIN) ─ V1b cold triode* (drive = GAIN)
       ─ LP 6k ─ top shelf (VOICE) ─ HPF 40 Hz ─ PRESENCE shelf ─ V2a triode* (x2.6) ─ LP 6k
       ─ cathode follower ─ tone stack (British values; BASS MID TREBLE) ─ weight shelf (VOICE)
       ─ HPF 20 Hz ─ power amp (drive = MASTER, more sag) ─►
[base] transformer HPF 40 Hz ─ LP 15k ─ level (GAIN/MASTER comp, OUTPUT)

Abyss
[base] in ─ HPF 25 Hz ─ V1 cathode shelf -5 dB < 150 Hz ─►
[OS]   V1 triode ─ LP 14k ─ HPF 70 Hz ─ BRIGHT shelf (GAIN-dependent) ─ V2 cold clipper* (drive = GAIN)
       ─ LP 6.5k ─ HPF 160 Hz ─ cathode shelf -4 dB < 300 Hz ─ V3* (x7) ─ LP 5k ─ HPF 50 Hz
       ─ V4* (x3.5) ─ LP 3.5k ─ cathode follower ─ tone stack (BASS MID TREBLE; SHIFT moves the scoop) ─ HPF 20 Hz
       ─ resonance +4.5 dB < 100 Hz ─ PRESENCE shelf ─ power amp (drive = MASTER) ─►
[base] transformer HPF 45 Hz ─ LP 14k ─ level (GAIN/MASTER comp, OUTPUT)

* anti-aliased triode stage (ADAA, see below)
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
- **Anti-aliased triode stage** (`AdaaTriodeStage`) for the hard-driven positions: Reef's V1b and
  V2a, Abyss's V2-V4. Same operating point, bias shift and positive side as the triode stage; the
  cutoff side uses the same algebraic curve with its own limit, f(u) = u / r, r = sqrt(1 +
  (u/c)^2), c = soft above zero and hard below. Its antiderivative F(u) = u^2 / (1 + r) allows
  first-order antiderivative anti-aliasing (Parker et al. 2016; Bilbao et al. 2017): the stage
  outputs (F(u1) - F(u0)) / (u1 - u0), the mean of the curve between successive inputs. For two
  inputs on the same side of zero that mean is exactly (u0 + u1) / (r0 + r1), so it costs the same
  as sampling the curve and never divides by the step. A 4.3 kHz sine driven 30 dB into the cold
  clipper inside the 4x oversampler aliases 19 dB less (-64 instead of -44 dB re. the
  fundamental). Price: half a sample of delay at the oversampled rate and a two-tap average
  (-0.5 dB at 20 kHz at 192 kHz), both well above what the low-passes after each stage let
  through.
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
| Reef `gain` | V1b drive, exponential 0.35 → 60 (45 dB of travel). Bright shelf scales by `(1 - gain)^1.3`. |
| Reef `presence` | Preamp high shelf ahead of V2a: `-12 dB * (1 - presence)^1.3`, transition ~1.5-6 kHz. Clockwise = more presence (flat at 1). |
| Abyss `bright` | Bright cap: high shelf (~1.2-3.5 kHz transition), 9 dB at gain 0 falling to 1.5 dB at gain 1 (`sqrt(gain)` blend). |
| Abyss `gain` | V2 drive, exponential 0.3 → 220 (57 dB of travel). |
| Abyss `shift` | Tone stack mid scoop ~480 Hz → ~800 Hz. |
| Abyss `presence` | Power-amp high shelf, 0 → +10 dB (transition ~1.6-5 kHz at full). |
| `bass` / `mid` / `treble` (Reef, Abyss) | Tone stack pots; bass log taper, mid and treble linear. |
| `master` (all) | Power-amp drive, exponential 0.3 → 3.5 (Reef and Abyss also apply their tone-stack make-up here). |
| `output` (all) | -24 dB at 0, 0 dB at 0.5, +12 dB at 1, linear in dB on each half. Tone-neutral. |

**Level compensation.** Turning a drive knob up raises the level only by part of the drive change
(`drive^(1 - c)` while the stages are still linear: c = 0.5 for Lagoon volume and master, 0.11 / 0.35
for Reef gain / master, 0.04 / 0.35 for Abyss gain / master); the rest becomes saturation. Each
knob feels like turning up an amp without jumping tens of decibels. Reef and Abyss rise by 12 and
5 dB over the lower half of the gain knob (as the preamp goes from clean or edge-of-breakup into
saturation) and stay within 1 dB over the upper half; no 0.1 step moves the level by more than
3.7 dB.

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

## 4. Gain targets and input level

v0.3.0 re-staged Reef and Abyss after a player reported that even Abyss had no real gain with the
knob all the way up. The numbers below come from the hidden report `"[.gain]"`
(`AmpGainReport.cpp`), and the targets are locked in by `AmpGainTests.cpp`.

### Input level: what dBFS means here

The models are voiced for a calibration in which **1.0 (0 dBFS peak) stands for about 1 V peak at
the guitar jack**, so a full-scale sine is -0.6 dBu; `AmpSection::kFullScaleDbu` holds that number
for any code that needs to line its levels up with Sedef's amps. It comes from the design DI: the
synthetic strum at -18 dBFS RMS (crest factor 14 dB) then is 128 mV RMS, exactly the hardest strum
in Elliott's measurements (open E chord on a bridge humbucker, 128 mV RMS / 850 mV peak). His
single-coil chords measured 72-76 mV RMS (about -23 dBFS here) and single notes 12-120 mV RMS
(-39 to -19 dBFS). As a cross-check, a 12AX7 input stage at the classic operating point (100k
anode load, -1.5 V bias, 1.1 mA; Aiken) draws grid current from about +1.5 V, a few dB above full
scale, and the models' input stages likewise only start to bend on the hottest peaks.

Real interfaces differ a lot. Current two- to eight-channel USB interfaces specify a maximum
instrument-input level of +12 to +21 dBu at minimum gain (manufacturer spec sheets), 13-22 dB
above this calibration, so with the preamp gain at its minimum a hard humbucker strum lands at
-31 to -40 dBFS RMS, and single coils 4-5 dB lower. That is why every measurement below is taken
at DI levels of -30, -24, -18 and -12 dBFS RMS.

**Calibrating.** Sedef needs no new control for this: the global Input Gain (±24 dB) is the trim.
Either strum hard and set Input Gain so the input meter peaks around -4 to -2 dBFS, or, if you
know your interface's maximum input level X dBu at the gain you use, set Input Gain to
X + 0.6 dB (for example +13 dB for an interface that clips at +12.5 dBu). Presets are voiced at
this level. In Summed stereo mode a guitar on one input of a stereo track reaches the amp at full
level: the input fold (`engine/include/sedef/core/InputFold.h`) passes the live side at unity when
the other is 30 dB or more quieter, and averages otherwise.

### Targets from real cascaded-triode preamps

Per-stage gains follow from the 12AX7's published constants (mu = 100, ra = 62.5k, gm = 1.6 mA/V;
GE data sheet) and the common-cathode gain formula A = mu Ra / (Ra + ra + (mu + 1) Rk) (Blencowe
ch. 1, eq. 1.4-1.5; Aiken):

| Stage (100k anode) | Gain | Source |
|---|---|---|
| Cathode fully bypassed, into 1M | x59, 35.5 dB | Aiken (x61.5 unloaded) |
| Same, 300 V supply, load-line method | x60 | Blencowe Table 1.1 |
| Same, data-sheet test (240k next grid, 900 ohm) | x50 | GE data sheet |
| 1.5k cathode, unbypassed | x32, 30 dB | Aiken |
| Cold clipper, 10k unbypassed | x8.5, 18.6 dB | formula |
| Cold clipper, 39k unbypassed | x2.4, 7.7 dB | formula |
| 470k / 470k interstage divider | -6 dB | |

The usual filter values land where Abyss's already are: 2.2 nF into a 1M grid leak is a 72 Hz
high-pass and 1 nF a 159 Hz one (Abyss: 70 and 160 Hz); a 1.5k cathode with 0.68 uF shelves below
156 Hz (Abyss V1: 150 Hz). An anode bypass cap works against the stage's output impedance, Ra
in parallel with ra + (mu + 1) Rk: 38.5k bypassed, 68k with an unbypassed 1.5k, 98k for a 39k cold
clipper, so 470 pF gives 8.8, 5.0 and 3.5 kHz, the range of Abyss's 6.5 / 5 / 3.5 kHz interstage
low-passes. Grid stoppers add more: a stage with a gain of 60 has about 150 pF of Miller input
capacitance, so a 470k stopper is a 2.2 kHz low-pass (Aiken, "What is Miller capacitance?";
the Valve Wizard on grid stoppers in later stages).

Totals, with the gain pot fully up and each stage's headroom taken as its bias (about 1.5 V):

- **Modern high gain, four stages.** A published circuit analysis of a four-stage cascaded
  high-gain preamp finds that 340 uV peak at the input overdrives its fourth stage at full gain
  (3.4 mV at half rotation): about 73 dB of small-signal gain to that grid. In this calibration
  that is -72.5 dBFS RMS. Composing the stages above (bypassed, cold clipper, divider, bypassed,
  divider) gives a similar 78 dB.
- **British crunch, three stages** (bypassed input stage, cold clipper, divider, bypassed third
  stage; no published total found): 35.5 + 18.6 - 6 = 48 dB to the third grid, so it clips from
  about 6 mV peak (4.2 mV RMS) at the input, -48 dBFS RMS here.

So the targets: at full gain, Abyss should start to compress near -72 dBFS and Reef near -48, with
10 % THD a few dB above. Musically, a lightly picked single note (12-20 mV RMS, -39 to -34 dBFS)
is then 25-35 dB into saturation on Abyss, so it compresses and sustains, and about 10 dB on Reef.
Measured: Abyss's 1 dB compression point at full gain is -73.6 dBFS (10 % THD at -67.8), Reef's
-50.1 (10 % THD at -46.8).

### Before and after

Input levels in dBFS RMS. THD10: a 220 Hz sine reaches 10 % THD. Knee 6 dB: the output falls 6 dB
behind the small-signal gain (sine; the G3 note in brackets, as the matching strum level).
Sustain: seconds a G3 note picked like one string of the strum stays within 6 dB of its peak (dry
note: 0.68 s). Crest: peak-to-RMS of the strum output (dry: 14.2 dB).

| Amp, drive | THD10 | Knee 6 dB | Sustain at DI -30 / -24 / -18 | Crest at DI -30 / -18 |
|---|---|---|---|---|
| Reef 0.5, v0.2.0 | -17.7 | -11.9 (-7.9) | 0.66 / 0.66 / 0.73 | 13.6 / 8.3 |
| Reef 0.5, v0.3.0 | -24.5 | -19.1 (-15.3) | 0.66 / 0.76 / 1.06 | 10.3 / 6.9 |
| Reef 1, v0.2.0 | -34.0 | -28.5 (-24.5) | 0.89 / 1.32 / 1.89 | 7.4 / 6.3 |
| Reef 1, v0.3.0 | **-46.8** | -41.5 (-37.6) | **2.00 / 2.74 / 3.51** | 6.2 / 5.9 |
| Abyss 0.5, v0.2.0 | -27.4 | -21.1 (-20.0) | 0.59 / 0.78 / 1.17 | 9.4 / 7.1 |
| Abyss 0.5, v0.3.0 | -39.2 | -35.8 (-34.4) | 1.43 / 2.08 / 2.78 | 7.0 / 6.5 |
| Abyss 1, v0.2.0 | -44.0 | -38.1 (-36.7) | 1.62 / 2.28 / 2.98 | 6.5 / 6.7 |
| Abyss 1, v0.3.0 | **-67.8** | -64.4 (-62.9) | **4.92 / 5.78 / 6.58** | 6.6 / 6.8 |

The v0.2.0 numbers bear the report out: at full gain Abyss reached 10 % THD at 6 mV RMS, so a
light single note (12-20 mV) was only 6-10 dB into distortion, and Reef at 20 mV RMS, a mid-gain
crunch. From an interface at low gain (DI -30) a note on full-gain Abyss sustained 1.6 s. Now
Abyss reaches 10 % THD at 0.4 mV RMS and Reef at 4.6 mV RMS.

What changed:

- **Abyss:** the gain pot now spans 0.3 → 220 (was 0.6 → 30), V3 is driven x7 (was 3.5) and V4
  x3.5 (was 2.5), so a fully clipped stage drives the next one 12-15 dB and 8-10 dB past its knee.
  The anode bypass low-passes after V2, V3 and V4 moved down to 6.5, 5 and 3.5 kHz (were 9, 7,
  7 kHz) to hold the fizz and aliasing that the extra saturation creates. V2-V4 are anti-aliased.
- **Reef:** the gain pot spans 0.35 → 60 (was 16) and V2a is driven x2.6 (was 2.2); V1b and V2a
  are anti-aliased, and the low-passes after them moved from 14 and 12 kHz to 6 kHz. That keeps
  the default sound's top end where it was (4 / 8 kHz octaves -21.7 / -32.4 dB re. the total, v0.2.0
  -22.4 / -31.8) and the cathode follower from clipping a fizzy edge.
- **Lagoon** is unchanged: clean up to noon at every DI level up to -12 dBFS (THD10 -2.3 dBFS at
  volume 0.5), breaking up near the top (THD10 -19.8 at full volume).
- **Taper:** each quarter turn now moves Reef's THD10 by 10-11 dB and Abyss's by 14 dB (v0.2.0:
  7-8 dB), so the whole travel does something: Abyss at gain 0 is an edge-of-breakup crunch at the
  design level, at 0.25 a crunch, at 0.5 a high-gain rhythm sound, at 1 a wall of gain.
- **Tightness and fizz** at full gain, measured as octave bands re. the total: palm-muted chugs
  keep the 63 / 125 Hz bands where v0.2.0 had them (-13.1 / -4.3 dB vs. -13.5 / -4.5), the 63 Hz
  band 3 dB below a clean amp's; the strum's 8 kHz band through a rough cab stand-in is -30.2 dB (v0.2.0
  -29.4). Palm-muted chugs, which start dark, gain 3-4 dB at 4-8 kHz: the extra harmonics of
  the deeper saturation, still 39 dB below the total through the cab.
- **Aliasing** for a 1.2-4.3 kHz sine at -15 dBFS RMS and full drive, measured exactly at the
  images of the harmonics (`test::worstAliasDb`; v0.2.0's report scanned a 7 Hz grid, which falls
  between the images and read up to 12 dB too low):

  | Worst image re. fundamental | Lagoon | Reef | Abyss |
  |---|---|---|---|
  | full master, v0.2.0 | -61.6 dB | -62.3 dB | -55.7 dB |
  | full master, v0.3.0 | -61.6 dB | -63.2 dB | -60.0 dB |
  | default master, v0.2.0 | -105.8 dB | -93.7 dB | -56.6 dB |
  | default master, v0.3.0 | -105.8 dB | -81.8 dB | -64.6 dB |

  With 13-24 dB more gain, Abyss aliases 4-8 dB less than before and Reef stays below -80 dB at
  the default master; at full master the power amp clipping a saturated preamp dominates in all
  three models. The worst case is always the 4.3 kHz tone.

## 5. References

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
  distortion. For the gain budget: sections 1.13-1.14 (cut-off and grid-current clipping), 1.18
  (cathode bypass), 4.8 and 4.17 (interstage filters, anode bypass capacitor) and 14.3-14.8
  (medium-, high- and ultra-high-gain preamp design).
- R. Aiken, "Designing Common-Cathode Triode Amplifiers", Aiken Amplification technical pages,
  https://aikenamps.com/index.php/designing-common-cathode-triode-amplifiers. 12AX7 constants
  (mu 100, ra 62.5k, gm 1.6 mA/V), the gain formulas, 35.5 dB bypassed / 30 dB unbypassed for a
  100k stage, and the -1.5 V / 1.1 mA operating point.
- M. Blencowe, "Chapter 1: Fundamentals of Amplification", free chapter of the book above,
  https://www.valvewizard.co.uk/Common_Gain_Stage.pdf: gain formulas, Table 1.1 (gain vs. anode
  load), cut-off and grid-current clipping. The Valve Wizard, "Grid-Stoppers and Miller
  Capacitance", https://www.valvewizard.co.uk/gridstopper.html.
- General Electric, 12AX7 data sheet ET-T509B (1953), https://w140.com/tekwiki/images/5/5e/GE_12AX7_data_sheet.pdf:
  mu, ra, gm and the resistance-coupled amplifier table.
- R. Aiken, "What is Miller Capacitance?", https://aikenamps.com/index.php/what-is-miller-capacitance.
- R. Kuehnel, published circuit analysis of a four-stage cascaded high-gain preamp (Amp Books,
  classic-circuits series), https://www.ampbooks.com/mobile/classic-circuits/soldano-slo/:
  input sensitivity of the fourth stage at full and half gain, interstage dividers. Used only for
  those two numbers; Abyss is a generic design, not a model of that circuit.
- R. Elliott, "Guitar & Bass Pickup Output Voltages", Elliott Sound Products, 2021,
  https://sound-au.com/articles/guitar-voltage.htm. Measured RMS and peak outputs of single-coil
  and humbucker pickups for single notes and chords; the anchor of the input calibration.
- J. D. Parker, V. Zavalishin and E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping
  Using Continuous-Time Convolution", Proc. 19th Int. Conf. on Digital Audio Effects (DAFx-16),
  Brno, 2016; S. Bilbao, F. Esqueda, J. D. Parker and V. Välimäki, "Antiderivative Antialiasing
  for Memoryless Nonlinearities", IEEE Signal Processing Letters 24(7), 2017. First-order ADAA.
- R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients" (via
  `core/Biquad`). Bilinear transform: any DSP textbook, e.g. J. O. Smith, *Introduction to
  Digital Filters*, CCRMA, 2007.

No code, data or measurements from other products were used (ADR-0006). Component values in the
tone stack tables and the gain budget are generic values from published schematics and from the
sources above; interface input levels are from manufacturers' public spec sheets.

## 6. Tests (`engine/tests/amp/`, tag `[amp]`)

| Test | Asserts |
|---|---|
| Tone stack vs. nodal analysis | The closed-form H(s) matches an independent complex nodal solve of the network (written in the test, Gaussian elimination) to < 0.01 dB from 20 Hz to 20 kHz, for 32 pot settings x 3 component sets (measured worst: 1e-6 dB). |
| Bilinear transform | Digital response equals the circuit at the bilinear-warped frequency to < 0.1 dB at 44.1, 48, 176.4, 192 kHz. At the oversampled rates the unwarped error is < 0.1 dB to 10 kHz (worst 0.091 dB) and < 0.5 dB to 20 kHz (worst 0.38 dB, all pots at zero). |
| Tone stack filter | The double-precision section produces its designed response (< 0.05 dB). |
| Finite output | Random settings (re-randomised every ~40 ms, including model, voice and switches) with full-scale stereo noise at 44.1 / 48 / 96 kHz and blocks of 1, 17, 64, 512: all finite and bounded. Non-finite and out-of-range settings are sanitised. |
| Silence | After a loud strum and 3 s of silence the output is below -100 dBFS (all models at full drive); digital silence from a cold start stays exactly zero. |
| Gain staging | All three models at defaults are within [-18, -12] dBFS RMS and within 3 dB of each other at 44.1 / 48 / 96 kHz (measured: -15.0 ±0.1 dBFS). |
| THD vs. gain | Reef and Abyss THD (220 Hz sine, -18 dBFS RMS) rises strictly at every 0.1 step of gain and spans at least 3x. |
| Full-gain targets | At 44.1 / 48 / 96 / 192 kHz, 10 % THD is reached at <= -64 dBFS RMS on Abyss (measured -67.8) and between -51 and -43 on Reef (-46.8), at least 15 dB apart. |
| Gain taper | Every quarter turn moves Reef's and Abyss's 10 % THD point by 8-16 dB (measured 10-11 and 14). Lagoon at volume 0.5 stays under 10 % THD up to -12 dBFS and breaks up below -15 dBFS at full volume. |
| Sustain | A G3 note at full gain stays within 6 dB of its peak for >= 4.2 s (DI -30) and >= 5 s (DI -24) on Abyss, >= 1.6 s and >= 2.2 s on Reef; the dry note: 0.68 s. |
| Density | Full-gain Abyss strum output has a crest factor below 7.5 dB at DI -30, -24, -18 and -12 dBFS. |
| Level vs. gain | Strum level never moves more than 4 dB per 0.1 of gain, and by less than 1.5 dB over the upper half (Reef, Abyss). |
| Tight, no fizz | Full-gain Abyss chugs: 63 Hz octave < -12.5 dB and 125 Hz < -3.5 dB re. the total, 2 dB below clean Lagoon's 63 Hz; full-gain strum through the rough cab: 8 kHz octave < -28.4 dB, 16 kHz < -48 dB (Reef, Abyss). |
| Aliasing | Worst image of a harmonic for 1.2 / 2.7 / 4.3 kHz sines at full drive: < -58 dB at full master, < -62 dB at the default master, all models (measured worst -60.0 and -64.6, Abyss). |
| ADAA stage | `AdaaTriodeStage` is exactly silent at rest, settles on its static curve to 1e-5, tracks the curve's mean for slow signals, and aliases at least 10 dB less than the sampled curve inside the 4x oversampler (measured 19 dB). |
| Lagoon clean | Volume and master at 0.25: THD < 1 % for a 220 Hz sine at -18 dBFS RMS; both at 1: > 10 %. |
| Model switching | All six model pairs: the largest sample-to-sample step during a switch is within 1.1x of steady state. Model changes every 10 ms (fade reversals and queued third models) also stay within 1.1x. |
| Automation | Each of the 22 knobs and switches swept 0 → 1 → 0 over one second with a new value every 64-sample block: output finite and the largest step within 1.1x of the largest step of any of 17 static settings along the sweep. |
| Latency | `latencySamples()` equals the oversampler's at 44.1 / 48 / 96 / 192 kHz; an impulse through a clean Lagoon peaks at that latency plus at most 60 µs (voicing-filter group delay), never before. |
| Channels and blocks | Stereo channels are independent (left equals a mono run bit for bit); block size does not change the output. |

Hidden reports: `"[.voicing]"` prints levels, THD and level curves per drive knob, octave-band
spectra (raw and through a rough cab stand-in), compression curves, the effect of each switch and
worst-case aliasing at full and default master; `"[.gain]"` prints the section 4 tables (THD and compression knees, sustain,
level and crest factor per drive setting and DI level, chug spectra); `"[.bench]"` prints
ns/sample. The synthetic DI signals (strum, single note, palm-muted chugs) are in
`AmpTestUtils.h`.

Measured on an Apple M2 (RelWithDebInfo, -O3), 48 kHz, 4x oversampling, 64-sample blocks, mono:
Lagoon ~175-210 ns/sample (0.8-1.0 % of one core), Reef ~230-245 ns (1.1-1.2 %), Abyss
~265-285 ns (1.3-1.4 %); the anti-aliased stages cost no more than the plain ones. A crossfade
between two models costs the sum of both. Stereo costs twice mono. Worst-case aliasing at full
gain with a 1.2-4.3 kHz sine: -60 dB or lower re. the fundamental at full master, -64 dB or
lower at the default master (section 4).

Not covered by an automated test here: allocation-freedom. `prepare` is the only function that
allocates (the oversampler buffers and one 32-frame scratch buffer per channel); this was checked
once with a counting global allocator over 2000 randomised blocks, model switches and resets. The
engine-level allocation test (ADR-0005) covers it permanently.

## 7. Known limitations and future work

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
  matching against a synthetic strum, and from v0.3.0 the gain budget of section 4), not by a
  listening panel. A listening pass with real DI
  recordings through the cab should follow, and the constants at the top of each model file are
  the knobs for it.
- No noise or hum is modelled, on purpose: silence in gives silence out. The interface's own
  noise and the guitar's hum are another matter: full-gain Abyss has about 59 dB of small-signal
  gain at 1 kHz (Reef 31 dB), so white noise at -90 dBFS RMS comes out at -37 dBFS, as hiss from a
  real four-stage preamp would. Keep the gate on.
- The input calibration (section 4) is a documented convention, not a control: Input Gain is the
  trim. A target mark on the input meter, or an "auto-set from a strum" helper, would make it
  easier to find.
- Stereo processes the channels one after another. Interleaving them would roughly halve the
  stereo cost if the amp is ever run in stereo (the engine's channel strip is mono).
