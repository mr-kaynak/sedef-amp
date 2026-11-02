# Transpose: polyphonic pitch shifter

Source: `engine/src/dsp/PitchShifter.cpp`. Interface: `engine/include/sedef/dsp/PitchShifter.h`.
Tests: `engine/tests/reverb/PitchShifterTests.cpp`.

## 1. What it models

Transpose shifts the guitar by −12 to +12 semitones before anything else in the chain. It has
two uses:

- **Alternate tunings.** Play "in E♭, D or C♯" on a guitar in standard tuning.
- **Octave effects.**

It is polyphonic: chords keep every note. It works in the time domain, so attacks stay sharp
and there is no phase-vocoder smear. At 0 semitones it is a true bypass: the audio is not
touched.

## Quality bars (v0.3.0)

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

- **Chord:** an open E major strum (six strings, 10 ms apart). The warble metrics use a
  just-intonation voicing, so no two partials beat against each other in the reference. The
  other metrics use equal temperament with string stiffness.
- **Palm-muted hits:** an E5 power chord (E2 B2 E3) decaying in about 40 ms. Isolated hits come
  400 ms apart, and a riff of sixteenth notes runs at 150 bpm.
- **Single notes:** an E3–E5 line of 300 ms notes, then a whole-step bend and release.

### Metrics

- **Latency:** at each attack, the time where the output envelope (1 ms RMS) first reaches half
  its attack peak, minus the same time in the reference. Reported as the mean, the range (max −
  min) and the difference from what the module reports to the host.
- **Purity:** on the sustained chord (0.3–2.3 s after the strum), the share of energy farther
  than ±4 Hz from every reference partial, from a Welch spectrum (0.68 s Hann windows). This is
  the shifter's "THD + N". It counts modulation sidebands, wrong frequencies, smear and
  aliasing.
- **Timbre:** the level error of each reference partial over the same span, as an
  energy-weighted RMS in dB over 60 Hz–6 kHz.
- **Partial warble:** each partial is demodulated at its target frequency (12 Hz, 4th-order
  low-pass) in both output and reference. The output-to-reference level ratio, in dB, is
  band-limited to 1–12 Hz. The RMS of that ratio is energy-weighted over the partials.
- **Envelope warble:** the same ratio for the broadband envelope (10 ms RMS frames),
  band-limited to 1–50 Hz.
- **Pre-echo:** on the isolated hits, the energy 2–20 ms before each attack crossing, relative
  to the attack peak.
- **Flam:** after the attack peak, the largest rise of the envelope (dB, within 40 ms) above its
  running minimum, minus the same in the reference. A doubled attack shows up as a rise.
- **Attack:** the attack peak level versus the reference, and the 10–90 % rise time versus the
  reference.
- **Pitch:** on the single-note line and bend, MPM pitch tracks of output and reference
  (`analysis::PitchDetector`, 2048-sample frames), compared outside the first 30 ms of each
  note.
- **Aliasing:** at +12 st, the output energy for 12 kHz and 15 kHz input sines, whose shifted
  frequencies do not exist at 48 kHz.
- **CPU:** mono, 48 kHz, 64-sample blocks, on the development Mac (Apple M-series).

### Bars

| # | Metric | Alternate tunings (−1…−5) | Octaves (±12) |
|---|---|---|---|
| B1 | Added latency, mean | ≤ 10 ms | ≤ 10 ms |
| B2 | Latency range over attacks | ≤ 0.5 ms (constant) | ≤ 0.5 ms |
| B3 | Measured − reported latency | ≤ 0.25 ms | ≤ 0.25 ms |
| B4 | Purity, sustained chord | ≤ −35 dB | ≤ −30 dB |
| B5 | Timbre, partial level error | ≤ 1.0 dB | ≤ 1.5 dB |
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

## 2. Algorithm

The shifter is a delay-line design with correlation-chosen splice points (WSOLA-style).

```
input ─► history (mirrored ring buffer, 160 ms)
            │
            ├─► read head(s): position moves at `rate = 2^(st/12)` input samples per output
            │   sample, Catmull-Rom interpolation ─► Hann crossfade ─► output
            │
            └─► mono sum ─► LP ─► decimate to ~12 kHz ─► coarse correlation search
```

**Read head.** A head reads the input history at `rate` times real time, which shifts the pitch
by exactly `rate`. Its delay behind the write point therefore drifts by `1 − rate` samples per
sample:

- **Downward shifts:** the head falls further behind.
- **Upward shifts:** the head catches up.

**Splice.** When the delay reaches its limit, a new head starts one "grain" earlier or later.
The output crosses from the old head to the new one with a Hann (raised-cosine) fade. Both
heads keep moving at `rate` during the fade.

**Choosing the splice offset.** Candidate positions for the new head span 16 ms, more than one
period of a low D string. For each candidate:

1. Compute the normalised cross-correlation between the old head's waveform and the
   candidate's, over a 12 ms segment that reaches 3 ms past the splice point.
2. Search coarsely on a decimated mono copy (about 12 kHz) for efficiency.
3. Refine at full rate, ±1 decimated step around the winner.
4. Place the peak with sub-sample accuracy by parabolic interpolation.

For a single note this picks a whole number of periods, and the splice is seamless: a shifted
sine stays 100 % on its target frequency. For chords it picks the best compromise, weighted by
energy.

**Correlation-aware fade gains.**
- The Hann gains `gOld + gNew = 1` are right when the two heads match (ρ = 1).
- When they don't match, both gains are divided by `√(gOld² + gNew² + 2ρ·gOld·gNew)`. This
  moves the fade smoothly toward equal power, so the loudness does not dip at poor splices.

**Grain length.** The jump, and so the time between splices, starts at 8 ms and grows by
10 ms per octave of shift, plus up to 16 ms chosen by the search. Splices happen every
`jump / |1 − rate|`:
- 1 semitone down: every 160–440 ms.
- 2 semitones down: every 90–240 ms.
- An octave up: every 18–34 ms.

The fade lasts up to 40 ms, or at most 80 % of the minimum splice interval.

**Stereo.** Both channels share one set of heads, and the splice decisions come from the mono
sum, so the image stays coherent.

**Semitone changes.** A change starts a new head at the new rate and crossfades to it over
30 ms (equal power, because the two pitches don't add coherently). Changing to 0 crossfades
into the untouched input. When that fade ends, processing stops and the output is bit-exact
again. A change that arrives during a splice fade waits for it to finish (≤ 40 ms).

## 3. Control mapping

| Setting | Range | Mapping |
|---|---|---|
| `semitones` | −12..+12 (clamped) | `rate = 2^(semitones/12)`; 0 = true bypass |

## 4. Latency

The head must stay far enough behind the write point to look ahead and to absorb a jump. That
makes the delay inherent and time-varying. `latencySamples()` is not part of this module's
interface, and the engine reports none for it. Mean delay of the active head, measured at
48 kHz (it scales with time, not samples):

| Shift | −12 | −7 | −5 | −3 | −2 | −1 | +1 | +2 | +3 | +5 | +7 | +12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mean delay (ms) | 30.2 | 26.2 | 24.6 | 22.0 | 20.6 | 19.1 | 11.3 | 12.1 | 14.3 | 16.0 | 17.5 | 21.3 |

The instantaneous delay sweeps across roughly 3–50 ms between splices. At 0 semitones the delay
is zero.

## 5. Measurements (48 kHz, Apple M2)

- **Pitch.** For a 220 Hz sine at every shift from −12 to +12, the spectral peak is within
  0.002 % of `220 · 2^(st/12)`, and 100 % of the energy is within ±3 % of it.
- **Chords.** For a G major triad of harmonic-rich notes (8 partials each), the energy away from
  the expected partials is −26 to −29 dB, depending on the shift (−1 to ±12 st).
- **CPU** (64-sample blocks, stereo): 11–17 ns/frame (≤ 0.08 % of a core). Bypass costs
  11 ns/frame to keep the history current.

## 6. References

- W. Verhelst and M. Roelands, "An Overlap-Add Technique Based on Waveform Similarity (WSOLA)
  for High Quality Time-Scale Modification of Speech", *Proc. IEEE ICASSP*, 1993. Splice
  positions chosen by waveform similarity.
- J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and Chorus", *J. Audio Eng. Soc.*
  45(10), 1997. Moving delay taps, interpolation.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011, chapter "Time-segment
  processing". Delay-line pitch shifting and SOLA-family methods.

## 7. Tests (`[reverb][transpose]`)

- Finite, bounded output for random semitone changes every 40 ms with full-scale stereo
  noise, at 44.1 / 48 / 96 kHz and block sizes 1, 17, 64, 512.
- 220 Hz sine:
  - −2 st reads 196.0 Hz, +12 st 440 Hz, −12 st 110 Hz; −1, +5 and −7 st are also checked.
  - All within ±0.5 %, with ≥ 90 % of the energy within ±3 % of the target.
- Chord (G–B–D) at −5, −2, +2 and +12 st:
  - all three shifted notes are present, within ±0.5 % and within 6 dB of each other;
  - the unshifted notes are at least 25 dB down.
- 0 st is bit-exact: from the start, mono and stereo, block sizes 1, 64 and 333, and after
  returning from a shift once the fades have finished.
- Semitone automation sweeping −12..+12 at 0.4 Hz is click-free: no sample step larger than
  twice the steepest legitimate slope.
- Stereo channels are shifted independently.

## 8. Known limitations / future work

- **Dense or inharmonic chords.** No single splice offset aligns every note, so each splice
  shifts the phase of the partials that don't fit. The result is a faint warble at the splice
  rate. It is least audible for small shifts, where splices are rare. A larger search span or
  longer jumps reduce it, at the cost of latency.
- **Transients across a splice** can briefly double (flam). The 3 ms look-ahead limits this,
  but it does not remove it.
- **Upward shifts do not band-limit the input**, so content above `fs / (2·rate)` folds back
  (above 12 kHz for +12 at 48 kHz). Guitar DI has little energy there. A rate-dependent
  windowed-sinc reader would fix it.
- **Latency is time-varying** (see §4) and not reported to the host.
