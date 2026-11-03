# Transpose: polyphonic pitch shifter

Source: `engine/src/dsp/PitchShifter.cpp`. Interface: `engine/include/sedef/dsp/PitchShifter.h`.
Tests: `engine/tests/reverb/PitchShifterTests.cpp`.

> **Removed from the signal chain in 0.3.0.** The `transpose` parameter is kept, so sessions
> still load, and it is ignored. This page keeps the evaluation behind that decision. §2–§8
> describe the 0.2.0 shifter, whose code stays for one release.

## 1. What it models

Transpose shifts the guitar by −12 to +12 semitones before anything else in the chain. It has
two uses:

- **Alternate tunings.** Play "in E♭, D or C♯" on a guitar in standard tuning.
- **Octave effects.**

It is polyphonic: chords keep every note. It works in the time domain. At 0 semitones it is a
true bypass: the audio is not touched. The v0.3.0 evaluation below measured it against an ideal
reference and against other designs. It falls well short of the bars on chords, attacks and
latency, and so did every alternative.

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

## Evaluation (v0.3.0)

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
rule, the 0.2.0 shifter is not replaced, and nothing has been removed on this branch. The data
supports removal.

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
- The evaluation harness (`TransposeEvalTests.cpp`): see "Evaluation (v0.3.0)" above.

## 8. Known limitations / future work

All four are measured in "Evaluation (v0.3.0)" above.

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
