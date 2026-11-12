# Transpose: polyphonic pitch shifter

Source: `engine/src/dsp/PitchShifter.cpp`. Interface: `engine/include/sedef/dsp/PitchShifter.h`.
Tests: `engine/tests/reverb/PitchShifterTests.cpp`, `engine/tests/engine/EngineTests.cpp`.
Bench: `tools/transpose-bench`.

> **History.** 0.2.0 shipped a time-domain shifter. 0.3.0 removed it: a player found it far
> worse than other amp sims, and no design measured then cleared the quality bars at a playable
> latency (Appendix A and B keep that evaluation). 0.9.0 brings Transpose back with the design
> below, measured against the same bars.

## 1. What it does

Transpose plays the whole rig −12 to +12 semitones away from the strings: drop the guitar a half
step, a whole step or more without retuning, or play an octave up or down. It is polyphonic:
chords keep every note.

It sits at the very start of the chain, right after Input Gain and before the pedalboard, so the
pedals, the amp and the cab hear the shifted string as they would hear a retuned guitar. The
tuner still listens to the strings themselves (it is for tuning the guitar).

At 0 semitones it is off: the audio is not touched (bit-exact) and it adds no latency.

## 2. Requirements

The 0.3.0 quality bars (Appendix A) are the yardstick, unchanged: they were written before
anything was measured, for two jobs, alternate tunings (−1 to −5 st, played live) and octaves
(±12 st). The ones that drive the design:

- **Latency (B1–B3).** At most 10 ms added, the same at every attack, and exactly what the
  plug-in reports. The player hears it live: a 64-sample interface round trip is 4–6 ms, and
  10 ms more is about standing 5 m from the amp. Lower is better, so the design takes as little
  as each direction needs (§5).
- **Chords (B4–B7).** A sustained open chord keeps its partials clean (−35 dB off-partial
  energy for tunings, −30 dB for octaves) and steady (no level warble).
- **Attacks (B8–B10).** No pre-echo, no doubled attack (flam), attack level and rise like a
  retuned string.
- **Pitch (B11).** Median within 2 cents, 95th percentile within 10 cents, bends included.
- **CPU (B13)** within 2 % of one core; **0 st (B14)** bit-exact with no latency;
  **changes (B15)** click-free.

## 3. Why it failed before, and what is new

The 0.3.0 evaluation found two families, each failing on the other's strength:

- **Time-domain splicing** (resampling read heads, crossfaded when they drift too far) keeps
  attacks and pitch exact and can run at a few ms, but each splice can only keep the strongest
  partials in phase. On a chord the rest jump, so purity stopped at −28 to −33 dB.
- **Frequency-domain methods** (phase vocoders) keep chords clean, but only with 85–170 ms
  windows, which smear attacks and cost that much latency. With short windows they fell apart.

What would clear the bars, the page concluded, is a model that estimates each partial over a long
window and resynthesises it in the present. The new design builds that, but uses it only for what
splicing cannot do. The argument in one line: **splices only hurt what is spliced**. The design
subtracts a model of the sustained partials from the input, splices only the remainder, and
resynthesises the model shifted, continuously, without splices. Between splices the output is
exactly a resampled input (§4.3), so attacks and pitch keep everything time-domain shifting
gets right, while the chord's steady partials never pass through a splice.

## 4. Method

### 4.1 Signal flow

```
x ─┬─────────────► analysis (Hann N, every H) ─► tracks: f, A, φ, decay
   │                                               │
   │                       ┌───── U: predicted partials at the input's pitch, now
   │                       ▼                       │
   └──► (−) ─► residual e = x − U ─► history       │ parameter history (per hop)
                      │                │           ▼
                      ▼                ▼        S: the same partials × r, read where
               onset detector ─► grains: resample e at rate r      the newest grain reads
                                       │                           │
                                       └───────────── (+) ◄────────┘ ─► y
```

`r = 2^(st/12)` is the pitch ratio. Everything runs per sample; the analysis runs at fixed
sample indices, so the output does not depend on the host's block size.

### 4.2 Grains

A grain `g` reads the residual history at `p_g(τ) = a_g + r (τ − τ_g)`: at output sample `τ_g` it
reads input sample `a_g` (its anchor) and then moves at `r` input samples per output sample,
which scales every frequency by exactly `r`. Its delay `d_g(τ) = τ − p_g(τ)` changes by `1 − r`
per sample: it grows for downward shifts and shrinks for upward ones. When it leaves its range a
new grain takes over through a crossfade (a splice). The time-domain output is
`TD[e](τ) = Σ_g w_g(τ) (h_r ∗ e)(p_g(τ))` with fade gains `w_g`.

`h_r` is a windowed-sinc interpolator (Kaiser, 16 taps). Upward, reading faster folds content
above `fs / 2r` back into the band, so for `r > 1` the kernel is stretched by `r` (up to 32 taps)
and its cut-off scaled by `1/r` (band-limited resampling, Smith 2002). It passes every sample at
an integer position unchanged, which §5's latency test relies on.

### 4.3 The identity that makes it work

The output is

```
y = TD[x − U] + S
```

where `U` is the model of the sustained partials at the input's pitch and `S` is the same
partials shifted. TD is linear, so if `S = TD[U]`, then `y = TD[x]`: the output is exactly the
resampled input, whatever the model got wrong. `S` is built to equal `TD[U]` for every partial as
long as the newest grain runs (it reads the model's parameter history at that grain's position
`p(τ)`, with the phase it would have there). At a splice, `S` keeps its phase running instead of
jumping with the grains. So:

- Between splices, `y = TD[x]`. Attacks, bends, vibrato and single notes come out as a
  resampled string: exact pitch, no smearing.
- At a splice, only `e = x − U` is crossfaded. On a sustained chord the model explains most of
  the signal, so the splice touches only the small remainder, while `S` carries the chord's
  partials on through, continuously.
- A wrong model costs nothing between splices; at a splice it costs what a plain time-domain
  splice of the unexplained part would.

### 4.4 The partial model U

Every `H` input samples, a frame of the last `N` samples is windowed (Hann, zero-phase) and
transformed.

- **Peaks.** Local maxima of the magnitude above `−70 dB` of the frame's strongest peak and
  above an absolute floor. Parabolic interpolation of the log magnitude gives the frequency and
  amplitude; the bin's phase is the partial's phase at the window centre `c = t − N/2`.
- **Tracks.** Each existing track takes the nearest unclaimed peak within one bin (or 1 % of its
  frequency), strongest track first. Peaks left over start new tracks, tracks left over end.
  At most `K` tracks; the weakest give way.
- **Frequency.** From the phase advance of the track over the last `q ≤ 8` frames, the wrap count
  taken from the parabolic estimate. Over `qH = 43 ms` this is about 8 times more precise than
  one hop, and the phase must be carried `N/2 + H` ahead (below).
- **Decay.** From the amplitude ratio over the same `q` frames, clamped to `[0, 1/(5 ms)]`:
  never a predicted growth (a growing peak is a note arriving, which the residual carries).
- **Prediction.** For the hop `[t, t + H)`, `U` synthesises each track as an oscillator whose phase
  reaches `φ + ω (t + H − c)` at the end of the hop (continuously, by choosing the hop's
  frequency) and whose amplitude ramps to `A e^(−λ (t + H − c))`. That is a prediction
  `N/2 + H` samples past the window centre; with the frequency error of `q` frames the phase
  error stays near 1°, so a steady partial cancels in `e` by about 35 dB.
- **Confidence.** A model that predicts a string the player has just damped, or a pitch that has
  moved, would make `e` larger than `x`. Each hop, `U` is scaled by its least-squares gain onto
  `x` over the previous hop, `g = clamp(Σ x U / Σ U², 0, 1)` (ramped). A damped chord drops `g`
  to 0 within two hops; an added note leaves it at 1.
- **Band.** Only partials whose shifted frequency lies inside the interpolator's pass band are
  modelled; the rest stay in `e`, where `h_r` treats them like any other content.

### 4.5 The shifted model S

`S` evaluates `U`'s parameter history (per hop: phase at the hop start, frequency, start and end
amplitude, the confidence ramp) at the newest grain's read position `p(τ)`, with every frequency
times `r`, plus a per-track phase offset `θ`. A track enters `S` with `θ = 0` when `p(τ)` reaches
the hop where it was born, so it starts exactly in phase with what the grains subtracted. When a
new grain takes over, `θ` absorbs the jump so every partial of `S` continues without a break,
and the amplitude difference between the two read positions is ramped out over the crossfade.
Within a stretch where `p(τ)` stays inside one hop, phase and amplitude are linear in `τ`, so `S`
runs as complex oscillators, one sine and cosine per track per stretch.

### 4.6 Attacks

An attack must come out exactly `L` after it went in, every time (B1–B3), and no grain may play
it twice (B9) or early (B8).

- **Detector.** On the residual, which has the sustained partials removed, so a pick over a
  ringing chord still stands out: the energy of the first difference (a +6 dB/octave tilt that
  favours the broadband pick over tonal changes), smoothed over 0.2 ms, against its own floor
  over 40 ms. An onset is a rise of 12 dB over the floor, while the input's own energy also
  rises (so a damped string, which makes the residual jump while the input falls, is not one).
  Re-armed after 30 ms.
- **Anchor.** The onset's first sample: going back from the detection, the last sample where the
  fast energy was still under twice the floor. For a click this is the click itself.
- **Grain.** A new grain is anchored so that it reads the anchor `a` at output sample `a + L`. It
  starts at once and fades in over `F_on = 1 ms` while every older grain fades out. It reads the
  residual before the attack during the fade, so nothing of the attack sounds early. For this to
  work, the fade must end before the old grain reaches `a`: a grain may never get closer than
  `d_min = δ + r F_on` to the input (`δ` the detection delay), and `L ≥ δ + F_on`.
- **Protection.** For 5 ms of input after an anchor, no later grain may start reading inside it,
  so an upward splice cannot replay the pick.

### 4.7 Sustain splices

When the newest grain's delay leaves `[d_min, d_max]`, a new grain starts at a delay in the search
span `[L, L + 16 ms]` (inside the protection rule), at the offset where the residual around the
two read positions correlates best (normalised, 10 ms segments; WSOLA, Verhelst and Roelands
1993). The crossfade is 10 ms, with the correlation-aware gains of 0.2.0 (equal gain for a
perfect match, moving to equal power as the match drops). On a well-modelled chord the residual
is small and these splices are inaudible; on a single note or a bend the residual is the string
itself and the search lands on a whole period, as plain WSOLA does.

### 4.8 Constants

| Constant | Value (48 kHz) | Serves | Why this value |
|---|---|---|---|
| Analysis window `N` | 4096 (85 ms), scaled to the rate | B4–B7 | Hann resolves partials 2 bins = 23 Hz apart, under the 41 Hz grid of an open E chord and the 82 Hz of a low E's harmonics. A new note is in the model within about `N/2` |
| Hop `H` | `N/16` = 256 (5.3 ms) | B4, B6 | The model follows a decaying chord in steps short against its decay; the prediction distance `N/2 + H` stays near `N/2` |
| Tracks `K` | 128 | B13 | Six strings with their strong partials; the rest stay in `e` |
| Peak floor | −70 dB under the strongest | B4 | What is left in `e` is at most −70 dB per partial |
| Frequency span `q` | up to 8 hops (43 ms) | B4 | Phase error ≈ 1° after `N/2 + H` |
| Decay clamp | `[0, 1/5 ms]` | B6, B7 | No growth predicted; a damped string decays in about 5 ms |
| Interpolator | Kaiser-windowed sinc, 16 taps, ×r for r > 1 | B5, B12 | Flat pass band; upward, alias rejection over 60 dB |
| Onset fade `F_on` | 1 ms | B8–B10 | Over before the attack arrives; spread under 2 kHz |
| Detection budget `δ` | ≤ 1.5 ms | B2 | A pick's broadband rise crosses +12 dB within 0.1–0.5 ms |
| Latency down `L` | 3 ms | B1 | `δ + F_on` plus the interpolator's half length, rounded up |
| Latency up `L` | 10 ms | B1, B9 | Reading `r` times faster, the attack grain lasts `(L − d_min)/(r − 1)`: 6.5 ms at +12, 19 ms at +5; 10 ms is the B1 ceiling |
| `d_min` | `δ + r F_on` (3.5 ms) | B8 | An old grain fades out before it reaches a new attack |
| Search span | 16 ms | B4, B11 | One period of 62.5 Hz (a seven-string's low B) |
| `d_max` (down) | `L + 16 + 5 ms` = 24 ms | B4 | Each sustain grain lasts at least `5 ms / (1 − r)`: 89 ms at −1, 20 ms at −5 |
| Splice fade | 10 ms | B4 | Long enough for a small residual, short against the grain |
| Correlation segment | 10 ms | B11 | About one period of a low E |
| Protection | 5 ms | B9 | A pick's high-frequency burst |
| Setting-change fade | 20 ms, equal power | B15 | The two pitches don't add coherently |

## 5. Latency and the host

Transpose adds `L` = 3 ms when it shifts down and 10 ms when it shifts up (rounded to whole
samples at the session rate), and nothing at 0. The plug-in reports it like the other latency it
has (Settings > Latency):

- Steps that keep the direction (−1 to −5, or +2 to +7) never change the reported latency.
- Turning Transpose on or off, or changing direction, does. The plug-in tells the host, which
  re-aligns its tracks; some hosts only do that when playback stops. Hosts dislike latency that
  changes, which is why it changes only on those steps.

"Latency" is exact in the time-domain sense: an attack anchored at input sample `a` is read at
output sample `a + L` (the tests check a click to the sample). Between attacks the grains' delay
moves within `[d_min, d_max]`, which only shifts when a held note's partials are read, never
when a note starts.

## 6. Changing the setting

A change starts a new shifter state (the grains and `S`'s phases; the model and the residual are
shared, they don't depend on `r`) and crossfades to it over 20 ms with equal-power gains. A
change that arrives during that fade waits for it to finish. Moving to 0 crossfades to the
untouched input; when the fade ends, processing stops and the output is bit-exact again. From 0,
the new state starts from an empty model, so the first `N/2` are plain time-domain shifting
while the model fills in. The input history is kept current while off, so switching on needs
no warm-up.

## 7. In the engine

- `Chain::process`: input gain → **Transpose** → the pedalboard → amp → cab.
- The gate's sidechain (the DI before input gain) is delayed by Transpose's latency, so the gate
  stays aligned with what it gates. The Doubler, which times its double from that sidechain, is
  told the sidechain's lead without Transpose's share.
- `Engine::latencySamples()` adds Transpose's latency for the active direction. The plug-in
  checks it on its message-thread timer and reports a change to the host, as it does for the
  oversampling setting.
- The parameter keeps its id `transpose` (ADR-0004: ids are the contract). Its display name goes
  back to "Transpose" and it is automatable again. Hosts save parameters by id (each plug-in format keys
  a parameter by a hash of its id string), never by name; the name only labels automation
  lanes, so sessions and automation saved under "Transpose (Unused)" load unchanged. Presets do
  not store it (`PresetManager.cpp`), so factory presets are unchanged.

## 8. Tests

`[transpose]` in `PitchShifterTests.cpp`, plus the engine's:

- **Pitch, every step.** For −12..+12, single plucked strings (E2 to E4) and the open E chord: every
  strong partial of the sustained output (long FFT, interpolated peak) against `r` times the input
  partial, within 2 cents.
- **Bars.** The Appendix A harness at −1, −2, −3, −5, −12 and +12 st: every bar each job sets.
- **Latency to the sample.** A click after silence comes out at exactly `a + L`, for every step,
  and `latencySamples()` says `L`.
- **0 st.** Bit-exact and zero latency from the start, and again once a shift has faded back to 0.
- **Invariance.** The same output, bit for bit, for block sizes 1, 7, 64, 333 and 4096; the pitch,
  latency (in ms) and purity bars at 44.1, 48, 88.2 and 96 kHz.
- **Changes.** Stepping through random settings every 40 ms while a chord rings: finite, and no
  sample step larger than twice the steepest legitimate one.
- **Real time.** `Engine::process` never allocates with Transpose on (the engine's counting
  allocator).
- **Engine.** The reported latency follows the direction; the gate stays aligned; factory presets
  render byte-identical to main at 0.

## 9. Results

(Filled in from the bench once the code exists.)

## 10. Known limitations

(Filled in from the measurements.)

## 11. References

For the evaluation:

- J. Laroche and M. Dolson, "New Phase-Vocoder Techniques for Pitch-Shifting, Harmonizing and
  Other Exotic Effects", *Proc. IEEE WASPAA*, 1999. Shifting peak regions in the frequency
  domain.
- J. Laroche and M. Dolson, "Improved Phase Vocoder Time-Scale Modification of Audio", *IEEE
  Trans. Speech and Audio Processing* 7(3), 1999. Identity phase locking.
- A. Röbel, "A New Approach to Transient Processing in the Phase Vocoder", *Proc. DAFx*, 2003.
  Energy centre of gravity per bin to find attacks.
- D. Mauler and R. Martin, "A Low Delay, Variable Resolution, Perfect Reconstruction Spectral
  Analysis-Synthesis System for Speech Enhancement", *Proc. EUSIPCO*, 2007. Asymmetric
  analysis and synthesis windows.
- A. Kohlrausch, R. Fassel and T. Dau, "The Influence of Carrier Level and Frequency on
  Modulation and Beat-Detection Thresholds for Sinusoidal Carriers", *J. Acoust. Soc. Am.*
  108(2), 2000. Amplitude-modulation thresholds behind B6 and B7.
- Signalsmith Stretch 1.4.0 (MIT), https://github.com/Signalsmith-Audio/signalsmith-stretch.
  Measured as a third-party candidate only.

For the 0.2.0 shifter:

- W. Verhelst and M. Roelands, "An Overlap-Add Technique Based on Waveform Similarity (WSOLA)
  for High Quality Time-Scale Modification of Speech", *Proc. IEEE ICASSP*, 1993. Splice
  positions chosen by waveform similarity.
- J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and Chorus", *J. Audio Eng. Soc.*
  45(10), 1997. Moving delay taps, interpolation.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011, chapter "Time-segment
  processing". Delay-line pitch shifting and SOLA-family methods.


## Appendix A. Quality bars (v0.3.0)

A player compared 0.2.0's transpose with commercial amp sims and found it far worse. These bars
say, in numbers, what "very good" means for its two jobs. They were written before any
candidate was measured, and a design ships only if it clears every bar for its job.

- **Alternate tunings:** −1 to −5 st on chords, single notes and palm-muted riffs, played live.
- **Octave effects:** ±12 st.

### Test material and reference

The test signals are synthetic guitar DI made of additive plucked-string notes (sharp 0.5 ms
attack, per-partial decay, pluck-position spectrum, optional bend). Because each note is a sum
of known partials, the ideal output is synthesised directly: every partial frequency times
`2^(st/12)`, with the same amplitudes and the same envelopes in time. Partials that would land
above 0.47 fs are left out of the reference, so aliasing counts as error.

- **Chord:** an open E major strum (six strings, 10 ms apart, equal temperament, stiff
  strings), for purity. An open D major (D3 A3 D4 F♯4) is used for warble, because its partials
  sit on a coarser grid and more of them can be measured one by one.
- **Single low E** sustained, for timbre.
- **Palm-muted hits:** an E5 power chord (E2 B2 E3, all strings at once) decaying in about
  40 ms. Isolated hits come 400 ms apart, and a riff of sixteenth notes runs at 150 bpm.
- **Single notes:** an E3–E5 line of 300 ms notes, then a whole-step bend and release.

### Metrics

- **Latency:** at each attack, the time where the output envelope (1 ms RMS) first rises
  through half its attack peak, minus the same time in the reference. Reported as the mean,
  the range (max − min) and the difference from what the module reports to the host.
- **Purity:** on the sustained E chord (0.35–2.35 s after the strum), the share of energy
  farther than ±4 Hz from every reference partial, 40 Hz–10 kHz, from a Welch spectrum
  (0.68 s Hann windows). This is the shifter's "THD + N". It counts modulation sidebands, wrong
  frequencies, smear and aliasing.
- **Timbre:** the third-octave band levels (63 Hz–5 kHz) of the sustained low E versus the
  reference, as an energy-weighted RMS in dB.
- **Partial warble:** on the D chord, each partial with no other partial within 3.5
  demodulation bandwidths is demodulated at its target frequency (zero-phase 4th-order
  low-pass, 12 Hz, or the partial grid / 3.5 when the shift packs the partials closer). The
  output-to-reference level ratio in dB, with its slow trend (a cubic over 2 s) removed, is
  the warble of that partial. The bar is on their RMS, weighted by energy.
- **Envelope warble:** the same for the total power of those partials, which is the
  loudness modulation all of them share.
- **Pre-echo:** on the isolated hits, the energy 3–20 ms before each attack crossing, relative
  to the attack peak.
- **Flam:** after the attack peak, the largest rise of the envelope above 2 kHz (dB, within
  40 ms, floored 30 dB below the peak) above its running minimum, minus the same in the
  reference. A second attack shows up as a rise. Above 2 kHz the palm-muted strings die within
  a few ms, so the beating of the chord's partials does not count.
- **Attack:** the attack peak level versus the reference, and the 10–90 % rise time versus the
  reference.
- **Pitch:** on the single-note line and bend, MPM pitch tracks of output and reference
  (`analysis::PitchDetector`, 2048-sample frames), compared outside the first 30 ms of each
  note.
- **Aliasing:** at +12 st, the output energy for 12 kHz and 15 kHz input sines, whose shifted
  frequencies do not exist at 48 kHz.
- **CPU:** mono, 48 kHz, 64-sample blocks, on the development Mac (Apple M-series).

**Changes made while validating the harness.** Checked against ideal outputs and outputs with
known faults (`TransposeEvalTests.cpp`), four of the first definitions measured the test signal
rather than the shifter. They were changed. The bars were not.

- **Warble chord.** The first warble chord was in just intonation, so it was exactly periodic
  (41.2 Hz). A periodic chord lets a time-domain shifter splice seamlessly, which a real guitar
  never allows. The stiff equal-tempered D chord replaced it.
- **Envelope warble.** It was first a broadband 10 ms RMS. Between two ideal outputs that
  differ only in the phases of their partials, that read 0.26–0.63 dB RMS: it measured where
  near-coincident partials of different strings were in their beat cycle. Any shifter is free
  to choose those phases. The total power of the isolated partials reads 0.000 dB for the same
  pair.
- **Timbre.** It was first the level of each partial of the chord. Partials of different
  strings 0.3–1 Hz apart beat slowly, so their joint level over 2 s depends on the beat phase.
  Two ideal outputs differed by up to 7 dB in one band. A single note has no such pairs.
- **Attacks.** The hit's three strings first started 2 ms apart, so the reference itself showed
  a "pre-echo" (the first string) and a "flam" (the later ones). They now start together, and
  flam is read above 2 kHz.

### Bars

| # | Metric | Alternate tunings (−1…−5) | Octaves (±12) |
|---|---|---|---|
| B1 | Added latency, mean | ≤ 10 ms | ≤ 10 ms |
| B2 | Latency range over attacks | ≤ 0.5 ms (constant) | ≤ 0.5 ms |
| B3 | Measured − reported latency | ≤ 0.25 ms | ≤ 0.25 ms |
| B4 | Purity, sustained chord | ≤ −35 dB | ≤ −30 dB |
| B5 | Timbre, band level error | ≤ 1.0 dB | ≤ 1.5 dB |
| B6 | Partial warble | ≤ 0.5 dB RMS | ≤ 0.75 dB RMS |
| B7 | Envelope warble | ≤ 0.25 dB RMS | ≤ 0.5 dB RMS |
| B8 | Pre-echo, isolated hits | ≤ −30 dB | ≤ −30 dB |
| B9 | Flam over the reference | ≤ 1 dB | ≤ 1 dB |
| B10 | Attack peak / rise time vs reference | ±1.5 dB / ≤ +1 ms | ±2 dB / ≤ +1 ms |
| B11 | Pitch error, median / 95th percentile | ≤ 2 / ≤ 10 cents | ≤ 2 / ≤ 10 cents |
| B12 | Aliasing at +12 st | — | ≤ −60 dB |
| B13 | CPU | ≤ 2 % of one core | ≤ 2 % of one core |
| B14 | 0 st | bit-exact, no latency | bit-exact, no latency |
| B15 | Semitone changes | click-free | click-free |

Why these numbers:

- **B1.** A typical interface round trip at 64-sample buffers is 4–6 ms. Adding 10 ms gives a
  total of about 15 ms, which is roughly standing 5 m from the amp. More than that starts to
  feel late under the pick.
- **B2.** A delay that wanders by milliseconds from note to note makes a tight riff sound
  sloppy, and a host can only compensate a constant latency.
- **B6 and B7.** Amplitude modulation of a tone at 4 Hz is detectable from a modulation index of
  roughly 0.02–0.05 (Kohlrausch et al. 2000). Index m gives about 6.1·m dB RMS, so 0.25 dB RMS is
  m ≈ 0.04 on the whole envelope, and 0.5 dB RMS is m ≈ 0.08 on one partial among many.
- **B4.** −35 dB is a modulation sideband pair of index ≈ 0.036 on every partial, the same
  threshold seen from the spectral side.

A second "Studio" mode with more latency is allowed only if no design clears the bars at
≤ 10 ms. It must keep its latency constant and report it to the host. It may relax B1 to
≤ 100 ms, but nothing else, and must reach B4 ≤ −40 dB.

## Appendix B. Evaluation of the 0.3.0 candidates

### Harness

- `engine/tests/reverb/TransposeEval.h` holds the signals, the ideal reference, every metric
  above and the bars as code (`barsFor`, `missedBars`).
- `engine/tests/reverb/TransposeEvalTests.cpp` checks the harness itself. The metrics read no
  error for the ideal output. They read the right size for faults of known size: a 6 Hz
  amplitude modulation of index 0.05 (0.31 dB RMS, sidebands at −29 dB), a constant 5 ms
  delay, an attack doubled 15 ms late, a −20 dB copy 10 ms early, and a line 10 cents sharp.
  The warble metrics also read under 0.02 dB between two ideal outputs whose partial phases
  differ.
- `tools/transpose-bench` builds `sedef-transpose-bench`. It runs the candidates through every
  metric and prints one row per shift, with the bars each row misses. `--listen DIR` writes DI
  files of musical material: the input, the ideal shifted version and each candidate, all
  aligned to the input.

```sh
cmake --preset engine -DSEDEF_TRANSPOSE_BENCH_STRETCH=ON   # the option fetches Signalsmith Stretch
cmake --build --preset engine --target sedef-transpose-bench
build/engine/tools/sedef-transpose-bench --candidates wsola,pv:sym=1:a=85:o=8:nb=1,stretch-120-30
```

### Candidates

Each family was tuned before it was compared. The table shows the best setting found for
each.

| Candidate | Design | Latency |
|---|---|---|
| `wsola` | 0.2.0 (§2 below), unchanged | 3–50 ms, varying |
| `wsola:fade=80:jump=20` | the same, retuned. It had the best purity in a sweep of jump 8–80 ms, fade 10–80 ms and search 16–30 ms | varying |
| `pv:a=85:s=8:nb=1` / `a=170` | Live phase vocoder: an asymmetric 85 or 170 ms analysis window, an 8 ms synthesis window | 8 ms, fixed |
| `pv:sym=1:a=85:o=8:nb=1` / `a=170` | Studio phase vocoder: symmetric Hann windows, 4096 or 8192 points, hop 1/8 | 85 or 170 ms, fixed |
| `stretch-120-30` / `60-15` | Signalsmith Stretch 1.4.0 (MIT), at its default 120 ms block and at 60 ms | 120 or 60 ms, fixed |

Both phase vocoders (`tools/transpose-bench/SpectralShifter.cpp`) shift each spectral peak
with its region of influence (Laroche & Dolson 1999). The shift is rounded to whole bins, and
the phase follows the measured frequency, so the pitch is exact. Every bin of a region gets the
peak's phase rotation (identity phase locking). A region whose energy centre lies ahead of the
phase origin and has grown is a new attack. It is rotated so that it stays coherent at its own
time instead of inheriting the bins' old phases (after Röbel 2003). Where a downward shift
packs regions closer, they are clipped so they don't overlap. The peak rule was tuned too:
a peak only has to exceed its nearest neighbours. Requiring the two nearest on each side merged
more close partials and cost 3–4 dB of purity.

### Results (48 kHz, 64-sample blocks)

The worst value over each job's shifts: −1, −2, −3 and −5 for alternate tunings, and −12 and
+12 for octaves. The CPU figures were taken while other builds ran on the machine, so treat
them as rough (the 0.2.0 shifter's 0.1 % matches §5).

| Candidate | Job | Latency mean (ms) | Latency range (ms) | Purity (dB) | Timbre (dB) | Partial warble (dB) | Pre-echo (dB) | Flam (dB) | Attack (dB) | Pitch p95 (cents) | CPU (%) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **Bar** | tunings / octaves | ≤ 10 | ≤ 0.5 | ≤ −35 / −30 | ≤ 1.0 / 1.5 | ≤ 0.5 / 0.75 | ≤ −30 | ≤ 1 | ≤ 1.5 / 2 | ≤ 10 | ≤ 2 |
| wsola | −1…−5 | 17–23 | 39.9 | −25.3 | 0.26 | 0.11 | −12 | 21.6 | 14.3 | 10.6 | 0.1 |
| wsola | ±12 | 15–31 | 39.6 | −16.6 | 0.10 | 0.53 | −13 | 28.3 | 17.5 | 19.1 | 0.1 |
| wsola:fade=80:jump=20 | −1…−5 | 28–37 | 56.0 | −27.9 | 0.20 | 0.38 | −12 | 30.7 | 13.2 | 18.9 | 0.1 |
| wsola:fade=80:jump=20 | ±12 | 20–35 | 73.7 | −6.8 | 0.17 | 0.18 | −14 | 47.1 | 18.5 | 13.0 | 0.1 |
| pv:a=85:s=8:nb=1 | −1…−5 | 9–10 | 16.5 | −10.9 | 1.16 | 1.77 | −14 | 22.9 | 9.2 | 9.0 | 8.6 |
| pv:a=85:s=8:nb=1 | ±12 | 9–10 | 9.8 | −7.4 | 1.10 | 4.50 | −44 | 31.3 | 12.2 | 288.2 | 4.3 |
| pv:a=170:s=8:nb=1 | −1…−5 | 10–12 | 12.3 | −11.4 | 1.13 | 1.54 | −42 | 23.5 | 9.5 | 19.0 | 12.7 |
| pv:a=170:s=8:nb=1 | ±12 | 11–12 | 22.2 | −9.3 | 0.94 | 5.35 | −44 | 31.9 | 12.0 | 197.7 | 9.3 |
| pv:sym=1:a=85:o=8:nb=1 | −1…−5 | 85 | 12.9 | −29.6 | 0.45 | 0.01 | −19 | 7.2 | 7.7 | 3.6 | 0.7 |
| pv:sym=1:a=85:o=8:nb=1 | ±12 | 84–85 | 14.5 | −22.0 | 0.65 | 0.12 | −14 | 8.4 | 11.5 | 16.1 | 0.5 |
| pv:sym=1:a=170:o=8:nb=1 | −1…−5 | 170–171 | 13.2 | −36.9 | 0.59 | 0.00 | −17 | 8.3 | 9.3 | 6.2 | 1.0 |
| pv:sym=1:a=170:o=8:nb=1 | ±12 | 169 | 22.0 | −23.4 | 0.14 | 0.00 | −12 | 10.3 | 13.3 | 16.9 | 1.3 |
| stretch-120-30 | −1…−5 | 119–120 | 8.1 | −9.8 | 2.05 | 0.01 | −22 | 8.7 | 5.2 | 22.5 | 2.6 |
| stretch-120-30 | ±12 | 119–121 | 11.9 | −21.0 | 1.45 | 0.06 | −17 | 6.9 | 7.9 | 9.9 | 4.0 |
| stretch-60-15 | −1…−5 | 59–60 | 4.7 | −5.7 | 38.27 | 2.63 | −24 | 7.4 | 4.2 | 21.0 | 2.6 |
| stretch-60-15 | ±12 | 58–61 | 20.7 | −6.3 | 1.38 | 3.52 | −11 | 7.6 | 9.7 | 15.7 | 3.5 |

<details>
<summary>Every shift</summary>

| Candidate | st | Latency mean / range (ms) | Purity (dB) | Timbre (dB) | Warble partial / env (dB) | Pre-echo (dB) | Flam (dB) | Attack (dB) / rise (ms) | Pitch med / p95 (cents) | Alias (dB) | CPU (%) | Misses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| wsola | −1 | 17.0 / 35.5 | −30.5 | 0.02 | 0.06 / 0.02 | −120 | 21.6 | 8.6 / 5.4 | 0.0 / 5.2 |  | 0.05 | B1 B2 B3 B4 B9 B10 |
| wsola | −2 | 19.0 / 39.9 | −28.1 | 0.04 | 0.07 / 0.01 | −23 | 13.6 | 14.3 / 2.8 | 0.1 / 10.6 |  | 0.06 | B1 B2 B3 B4 B8 B9 B10 B11 |
| wsola | −3 | 19.2 / 33.9 | −27.2 | 0.26 | 0.11 / 0.01 | −120 | 13.2 | 12.3 / 7.0 | 0.3 / 4.6 |  | 0.09 | B1 B2 B3 B4 B9 B10 |
| wsola | −5 | 23.4 / 36.2 | −25.3 | 0.05 | 0.01 / 0.00 | −12 | 7.8 | 10.4 / 8.8 | 0.6 / 8.0 |  | 0.07 | B1 B2 B3 B4 B8 B9 B10 |
| wsola | −12 | 31.2 / 39.6 | −24.2 | 0.09 | 0.45 / 0.45 | −13 | 16.5 | 17.5 / 9.8 | 0.9 / 19.1 |  | 0.09 | B1 B2 B3 B4 B8 B9 B10 B11 |
| wsola | +12 | 14.9 / 13.6 | −16.6 | 0.10 | 0.53 / 0.01 | −18 | 28.3 | 6.7 / 1.0 | 1.0 / 3.4 | 2 | 0.09 | B1 B2 B3 B4 B8 B9 B10 B12 |
| wsola:fade=80:jump=20 | −1 | 27.6 / 44.9 | −33.0 | 0.04 | 0.27 / 0.04 | −48 | 1.3 | 4.9 / 11.8 | 0.0 / 14.7 |  | 0.07 | B1 B2 B3 B4 B9 B10 B11 |
| wsola:fade=80:jump=20 | −2 | 28.7 / 46.0 | −29.9 | 0.05 | 0.34 / 0.03 | −120 | 21.9 | 11.3 / 4.9 | 0.1 / 5.8 |  | 0.06 | B1 B2 B3 B4 B9 B10 |
| wsola:fade=80:jump=20 | −3 | 31.8 / 56.0 | −29.3 | 0.20 | 0.38 / 0.03 | −12 | 30.7 | 6.7 / 4.7 | 0.3 / 8.1 |  | 0.07 | B1 B2 B3 B4 B8 B9 B10 |
| wsola:fade=80:jump=20 | −5 | 36.5 / 55.2 | −27.9 | 0.13 | 0.06 / 0.03 | −14 | 5.4 | 13.2 / 6.3 | 0.6 / 18.9 |  | 0.08 | B1 B2 B3 B4 B8 B9 B10 B11 |
| wsola:fade=80:jump=20 | −12 | 35.0 / 73.7 | −8.9 | 0.15 | 0.13 / 0.13 | −14 | 47.1 | 18.5 / 10.0 | 0.5 / 13.0 |  | 0.07 | B1 B2 B3 B4 B8 B9 B10 B11 |
| wsola:fade=80:jump=20 | +12 | 20.4 / 39.4 | −6.8 | 0.17 | 0.18 / 0.01 | −16 | 27.2 | 5.7 / 19.3 | 0.8 / 11.2 | −3 | 0.09 | B1 B2 B3 B4 B8 B9 B10 B11 B12 |
| pv:a=85:s=8:nb=1 | −1 | 9.2 / 9.3 | −15.7 | 0.81 | 1.77 / 1.02 | −41 | 18.5 | 6.7 / 6.8 | 0.3 / 2.2 |  | 5.17 | B2 B3 B4 B6 B7 B9 B10 B13 |
| pv:a=85:s=8:nb=1 | −2 | 9.5 / 16.5 | −11.3 | 0.95 | 1.46 / 0.83 | −14 | 20.4 | 9.2 / 5.8 | 0.4 / 3.7 |  | 8.60 | B2 B3 B4 B6 B7 B8 B9 B10 B13 |
| pv:a=85:s=8:nb=1 | −3 | 9.4 / 5.6 | −10.9 | 1.02 | 0.94 / 0.48 | −51 | 22.6 | 7.6 / 8.9 | 0.2 / 8.6 |  | 2.94 | B2 B3 B4 B5 B6 B7 B9 B10 B13 |
| pv:a=85:s=8:nb=1 | −5 | 9.1 / 11.5 | −10.9 | 1.16 | 0.39 / 0.09 | −52 | 22.9 | 9.2 / 9.9 | 0.3 / 9.0 |  | 4.13 | B2 B3 B4 B5 B9 B10 B13 |
| pv:a=85:s=8:nb=1 | −12 | 9.8 / 9.8 | −14.9 | 1.10 | 4.50 / 4.50 | −44 | 31.3 | 12.2 / 11.1 | 0.7 / 288.2 |  | 3.66 | B2 B3 B4 B6 B7 B9 B10 B11 B13 |
| pv:a=85:s=8:nb=1 | +12 | 8.9 / 6.7 | −7.4 | 1.06 | 0.27 / 0.10 | −50 | 8.3 | 6.8 / 6.5 | 0.1 / 15.0 | −7 | 4.31 | B2 B3 B4 B9 B10 B11 B12 B13 |
| pv:a=170:s=8:nb=1 | −1 | 10.3 / 8.2 | −17.5 | 0.79 | 1.54 / 1.00 | −42 | 18.2 | 4.7 / 8.7 | 0.7 / 7.9 |  | 8.13 | B2 B3 B4 B6 B7 B9 B10 B13 |
| pv:a=170:s=8:nb=1 | −2 | 10.7 / 8.5 | −12.0 | 0.96 | 1.31 / 0.70 | −50 | 17.8 | 6.2 / 11.7 | 0.7 / 8.5 |  | 12.66 | B2 B3 B4 B6 B7 B9 B10 B13 |
| pv:a=170:s=8:nb=1 | −3 | 10.9 / 11.2 | −11.4 | 0.98 | 0.87 / 0.48 | −50 | 21.2 | 7.4 / 11.1 | 0.8 / 10.6 |  | 10.41 | B2 B3 B4 B6 B7 B9 B10 B11 B13 |
| pv:a=170:s=8:nb=1 | −5 | 11.5 / 12.3 | −12.0 | 1.13 | 0.38 / 0.10 | −51 | 23.5 | 9.5 / 9.8 | 0.8 / 19.0 |  | 8.97 | B1 B2 B3 B4 B5 B9 B10 B11 B13 |
| pv:a=170:s=8:nb=1 | −12 | 12.5 / 22.2 | −18.1 | 0.94 | 5.35 / 5.35 | −44 | 31.9 | 9.3 / 9.4 | 0.7 / 197.7 |  | 9.28 | B1 B2 B3 B4 B6 B7 B9 B10 B11 B13 |
| pv:a=170:s=8:nb=1 | +12 | 11.1 / 14.1 | −9.3 | 0.92 | 0.21 / 0.05 | −51 | 11.5 | 12.0 / 14.8 | 0.3 / 16.9 | −5 | 7.01 | B1 B2 B3 B4 B9 B10 B11 B12 B13 |
| pv:sym=1:a=85:o=8:nb=1 | −1 | 85.5 / 10.4 | −34.3 | 0.40 | 0.00 / 0.00 | −20 | 6.3 | 4.1 / 5.5 | 0.0 / 0.6 |  | 0.69 | B1 B2 B4 B8 B9 B10 |
| pv:sym=1:a=85:o=8:nb=1 | −2 | 85.2 / 3.8 | −33.1 | 0.44 | 0.00 / 0.00 | −24 | 6.0 | 4.4 / 10.2 | 0.0 / 1.5 |  | 0.65 | B1 B2 B4 B8 B9 B10 |
| pv:sym=1:a=85:o=8:nb=1 | −3 | 85.4 / 12.9 | −31.9 | 0.27 | 0.00 / 0.00 | −22 | 7.2 | 7.7 / 12.8 | 0.0 / 2.4 |  | 0.58 | B1 B2 B4 B8 B9 B10 |
| pv:sym=1:a=85:o=8:nb=1 | −5 | 85.5 / 4.6 | −29.6 | 0.45 | 0.01 / 0.00 | −19 | 5.8 | 5.8 / 12.9 | 0.0 / 3.6 |  | 0.50 | B1 B2 B4 B8 B9 B10 |
| pv:sym=1:a=85:o=8:nb=1 | −12 | 84.6 / 14.5 | −26.5 | 0.65 | 0.12 / 0.12 | −16 | 8.4 | 11.5 / 8.1 | 0.2 / 16.1 |  | 0.48 | B1 B2 B3 B4 B8 B9 B10 B11 |
| pv:sym=1:a=85:o=8:nb=1 | +12 | 84.5 / 7.1 | −22.0 | 0.12 | 0.00 / 0.00 | −14 | 6.8 | 7.8 / 12.2 | 0.0 / 4.0 | −16 | 0.49 | B1 B2 B3 B4 B8 B9 B10 B12 |
| pv:sym=1:a=170:o=8:nb=1 | −1 | 171.1 / 12.1 | −40.2 | 0.40 | 0.00 / 0.00 | −22 | 7.9 | 6.4 / 16.5 | 0.1 / 4.6 |  | 0.65 | B1 B2 B8 B9 B10 |
| pv:sym=1:a=170:o=8:nb=1 | −2 | 170.4 / 13.0 | −39.3 | 0.54 | 0.00 / 0.00 | −22 | 6.2 | 8.4 / 10.9 | 0.1 / 3.6 |  | 0.99 | B1 B2 B8 B9 B10 |
| pv:sym=1:a=170:o=8:nb=1 | −3 | 170.2 / 11.2 | −39.2 | 0.46 | 0.00 / 0.00 | −20 | 6.9 | 8.6 / 13.1 | 0.1 / 6.2 |  | 0.79 | B1 B2 B3 B8 B9 B10 |
| pv:sym=1:a=170:o=8:nb=1 | −5 | 170.0 / 13.2 | −36.9 | 0.59 | 0.00 / 0.00 | −17 | 8.3 | 9.3 / 11.5 | 0.1 / 5.0 |  | 0.69 | B1 B2 B3 B8 B9 B10 |
| pv:sym=1:a=170:o=8:nb=1 | −12 | 168.9 / 22.0 | −28.8 | 0.14 | 0.00 / 0.00 | −12 | 10.3 | 13.3 / 11.3 | 0.2 / 8.6 |  | 0.89 | B1 B2 B3 B4 B8 B9 B10 |
| pv:sym=1:a=170:o=8:nb=1 | +12 | 168.5 / 13.6 | −23.4 | 0.13 | 0.00 / 0.00 | −13 | 10.1 | 12.0 / 12.0 | 0.1 / 16.9 | −23 | 1.26 | B1 B2 B3 B4 B8 B9 B10 B11 B12 |
| stretch-120-30 | −1 | 120.0 / 4.2 | −11.7 | 0.18 | 0.00 / 0.00 | −26 | 8.7 | 3.1 / 7.8 | 1.8 / 10.8 |  | 2.07 | B1 B2 B4 B8 B9 B10 B11 B13 |
| stretch-120-30 | −2 | 119.9 / 5.5 | −10.0 | 0.14 | 0.01 / 0.00 | −22 | 7.0 | 5.2 / 10.4 | 3.7 / 22.5 |  | 2.01 | B1 B2 B4 B8 B9 B10 B11 B13 |
| stretch-120-30 | −3 | 119.6 / 8.1 | −9.8 | 2.05 | 0.01 / 0.00 | −23 | 6.5 | 5.2 / 6.6 | 1.5 / 9.9 |  | 1.58 | B1 B2 B3 B4 B5 B8 B9 B10 |
| stretch-120-30 | −5 | 119.1 / 6.0 | −11.1 | 0.18 | 0.00 / 0.00 | −23 | 7.0 | 4.5 / 10.3 | 2.3 / 8.9 |  | 2.61 | B1 B2 B3 B4 B8 B9 B10 B11 B13 |
| stretch-120-30 | −12 | 118.5 / 11.9 | −26.2 | 1.45 | 0.06 / 0.06 | −17 | 6.9 | 7.9 / 8.4 | 1.7 / 7.1 |  | 2.03 | B1 B2 B3 B4 B8 B9 B10 B13 |
| stretch-120-30 | +12 | 120.9 / 7.7 | −21.0 | 0.29 | 0.04 / 0.02 | −20 | 6.5 | 4.1 / 10.7 | 2.5 / 9.9 | −7 | 3.95 | B1 B2 B3 B4 B8 B9 B10 B11 B12 B13 |
| stretch-60-15 | −1 | 59.8 / 3.4 | −9.2 | 0.82 | 0.19 / 0.05 | −34 | 2.0 | 3.4 / 7.7 | 2.6 / 11.7 |  | 2.58 | B1 B2 B4 B9 B10 B11 B13 |
| stretch-60-15 | −2 | 59.7 / 4.3 | −5.9 | 22.10 | 0.37 / 0.06 | −30 | 4.5 | 3.1 / 6.9 | 3.7 / 15.1 |  | 1.77 | B1 B2 B3 B4 B5 B8 B9 B10 B11 |
| stretch-60-15 | −3 | 59.6 / 3.7 | −5.7 | 38.27 | 0.44 / 0.09 | −26 | 7.4 | 3.8 / 5.2 | 3.9 / 21.0 |  | 1.91 | B1 B2 B3 B4 B5 B8 B9 B10 B11 |
| stretch-60-15 | −5 | 59.3 / 4.7 | −6.0 | 21.90 | 2.63 / 0.71 | −24 | 5.2 | 4.2 / 8.0 | 5.3 / 17.3 |  | 1.75 | B1 B2 B3 B4 B5 B6 B7 B8 B9 B10 B11 |
| stretch-60-15 | −12 | 58.5 / 20.7 | −10.3 | 1.38 | 3.52 / 3.52 | −11 | 7.6 | 9.7 / 9.2 | 6.1 / 12.8 |  | 3.45 | B1 B2 B3 B4 B6 B7 B8 B9 B10 B11 B13 |
| stretch-60-15 | +12 | 60.8 / 3.4 | −6.3 | 0.39 | 0.37 / 0.02 | −24 | 6.2 | 4.1 / 10.0 | 9.1 / 15.7 | −9 | 1.72 | B1 B2 B3 B4 B8 B9 B10 B11 B12 |

</details>

### Findings

- **The 0.2.0 shifter misses 6–8 bars at every shift.** From note to note,
  its attack latency wanders over 34–40 ms for downward shifts and 14 ms at +12 (B2). Every
  attack that lands in a 40 ms splice fade is doubled (flam 8–28 dB, attack level off by
  7–18 dB). Sustained chords reach only −17 to −31 dB purity. Timbre and per-partial level
  modulation pass. The median pitch is exact, but the 95th percentile misses at −2 and −12 st,
  because the bend lags behind the wandering delay. This matches what the player heard.
- **Time-domain splicing can't make chords clean.** Each splice keeps the strongest partials in
  phase and lets the rest jump. Shorter grains make the jumps more frequent. Longer grains turn
  the envelope into a sawtooth, because the head replays older, louder parts of a decaying chord.
  In a sweep at −2 and −5 st (jump 8–80 ms, fade 10–80 ms, search 16–30 ms; rerun it with
  `wsola:jump=…:fade=…:search=…`), purity stayed between −20 and −30 dB. The best setting reaches
  −33.0 dB at −1 st and −27.9 dB at −5 st, 2–7 dB short of B4, and falls apart at ±12 (−7 to
  −9 dB). Splicing at the attacks could fix flam and attack latency for downward shifts. It
  can't change chord purity. At +12 st it can't keep latency under 10 ms without a flam either:
  the head gains 1 ms on the input every ms, so a grain that holds a whole 20 ms attack must
  start about 20 ms behind the input. That variant was not built.
- **A phase vocoder can't resolve a low chord within 10 ms.** The low E's partials are 82 Hz
  apart, and an open chord's are often 20–40 Hz apart. An asymmetric window takes that
  resolution from the past while it resynthesises only its last 8 ms. But its short tail spreads
  every partial over ±30 bins (−24 dB at 5 bins, −44 dB at 20), and moving each peak's region
  on its own tears those skirts apart. Purity drops to −7 to −18 dB. Partial warble reaches
  1.8 dB for the tunings and 5.4 dB at −12, and CPU 3–13 %.
- **A symmetric phase vocoder needs 170 ms for chords, and it still smears attacks.** At 85 ms
  it reaches −29.6 dB purity at −5 st (B4 needs −35, the Studio mode −40). At 170 ms it reaches
  −36.9 dB at −5 st and −40.2 dB at −1 st. At both lengths, attacks get pre-echo of −12 to
  −24 dB (B8: −30), an HF ripple that reads as 6–10 dB of flam, a rise 6–16 ms slower, and
  attack latencies that spread over 4–22 ms. Re-phasing attack regions cut pre-echo by 7 dB and
  the attack-level error by 3–4 dB at 85 ms. That still leaves it 6–11 dB short of B8 at −2 and
  −5 st.
- **Signalsmith Stretch** (1.4.0, default settings) misses on pitch. Sustained sines come out
  +13 to +19 cents sharp at 82 Hz and +15 to +18 cents at 110 Hz (−2 and −5 st), and about
  +4 cents at 147–440 Hz. That is the parabolic peak of a 5.5 s spectrum; the same measurement
  reads within ±0.06 cents on the phase vocoder above. Stretch reaches −10 to −26 dB purity and
  a 95th-percentile pitch error of up to 23 cents. It smears attacks like the symmetric phase
  vocoder, with 120 ms of latency.

### Decision

No candidate clears the bars for either job, at 10 ms or at Studio latency. Under the decision
rule, the 0.2.0 shifter was not replaced, and the data supported removal, which 0.3.0 did.

**Removal proposal.**

1. **Keep the parameter.** Keep `transpose` in `ParameterList.inc` unchanged: same id, range
   and flags (ADR-0004). Sessions that saved it still load, and hosts keep their automation
   lanes.
2. **Engine.** `Chain::process` stops running the shifter, so the parameter is ignored and the
   signal passes bit-exact. Remove `dsp::PitchShifter`, its tests and §2–§8 of this page, or
   keep them for one release.
3. **UI.** Remove the Transpose block from `ui/src/features/pedals/PedalsSection.tsx`. The
   schema tests (`format`, `normalise`, `mockBackend`) and `presetScope.ts` still refer to the
   id and can stay.
4. **Docs.** Remove Transpose from the README pedal list, from the chain diagram and module
   table in `docs/architecture.md`, and from the Pedals row in `docs/ui.md`. The trailer's
   header shot may show the knob.
5. **CHANGELOG, under Removed.** "Transpose. It could not shift chords cleanly or keep a steady
   latency. Sessions that used it now play at the original pitch."

Presets are not affected, because they never stored `transpose` (`PresetManager.cpp`).

**What would clear the bars.** For a low strummed chord, the partials must be resolved
(≥ 85–170 ms of analysis) and also synthesised from the last few ms. These two ask for opposite
windows. The asymmetric-window result shows that region-based modification can't bridge them.
A sinusoidal model that estimates each partial's frequency, amplitude slope and phase over a
long window, then resynthesises it at the shifted frequency and extrapolates its envelope to the
present, might. That is research, not a release item.

