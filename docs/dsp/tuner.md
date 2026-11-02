# Tuner and intonation measurement

Code: `engine/include/sedef/analysis/PitchDetector.h`, `PitchTracker.h`,
`TunerDisplay.h`, `engine/src/analysis/`. Tests: `engine/tests/analysis/` (tag `[analysis]`;
hidden `[.report]` prints the accuracy and display tables below, `[.bench]` the cost). The
gauge: `ui/src/features/tuner/` (`readout.ts`, `needle.ts`, `TunerGauge.tsx`).

## 1. What it measures

The precision tuner and the intonation workbench need the pitch of a single
plucked string to a few hundredths of a cent once it settles, and a usable
reading within ~100 ms of the pick. The workbench compares the open string with
its 12th-fret octave, so systematic bias matters more than noise.

The quantity reported is the frequency of the **fundamental partial**. Guitar
and bass strings are stiff: partial n sits at `n f0 sqrt (1 + B n^2)` (Fletcher
& Rossing), so the upper partials are stretched sharp. A periodicity estimate
(autocorrelation, NSDF) is a power-weighted compromise between the partials and
reads several cents sharp of the fundamental (measured +1..+6 cents with
`B = 1e-4` and a rich spectrum), and by different amounts for an open string
and its 12th fret (four times the stiffness), which would corrupt the octave
comparison. The fundamental is unambiguous and independent of pickup and tone
settings.

## 2. Signal flow

```
input (44.1-192 kHz)
  -> 8th-order Butterworth low-pass at 0.3 x analysis rate (4 RBJ sections, double)
  -> keep every D-th sample, D = round (fs / 12 kHz)  -> 11-12.6 kHz at common rates
  -> 15 Hz DC blocker
  -> history (8 periods of 25 Hz = 320 ms)
every 5 ms hop:
  level (input RMS, 50 ms)        -> gate: below -60 dBFS nothing is valid
  onset (gate opens, or high-passed energy > 4x the max of 10-45 ms ago)
                                  -> new segment, onsetId++
  reading detection   window = since onset, up to max (8 periods, 40 ms)
                      -> median of 5 -> frequencyHz, midiNote, cents
  settled detection   window = after max (150 ms, half the time since onset),
                      up to 320 ms  -> stability ring (last 0.4 s)
                      -> stable, stableHz (mean), stableStdCents, stableCount
  display detection   the settled window, but starting no earlier than the
                      last move of the pitch (reused while they coincide)
                      -> TunerDisplay with the reading -> display*
```

The intonation workbench uses `stable*`; the tuner needle uses `display*` (section 3).

### PitchDetector

1. **MPM** (McLeod & Wyvill 2005). NSDF `n'(t) = 2 r'(t) / m'(t)`; `r'` from the
   power spectrum of the zero-padded window (pffft, sizes 2^k and 3 x 2^k),
   `m'` by the incremental recursion. Key maxima are the highest point of each
   positive lobe after the zero-lag lobe, refined by parabolic interpolation.
   The period is the first key maximum within 0.9 of the highest.
2. **Window rules.** Lags run to 0.4 x window, so a period needs 2.5 of itself
   in the window. A period is also only accepted once the window could show a
   key maximum at twice it (unless that is below `minHz`). Together these stop
   a short window right after the pick from reporting the octave above.
3. **Harmonic support.** Two strings ringing together (the last one still
   sounding while the next is plucked) repeat at a common subharmonic: E2 + A2
   at 27.5 Hz, with clarity near 1. A period whose first and second harmonics
   both hold under 0.3% of the power (Hann demodulation over up to 20 periods)
   is skipped and the choice repeats among shorter lags.
4. **Clarity gate.** `max (0.5, 7 / sqrt (n))`. Noise key maxima scale as
   `1 / sqrt (n)` (low-passed noise peaks near 0.48 at n = 120, 0.1 at
   n = 3840). 0.5 rather than a higher value lets a new string read while the
   previous one rings, when its clarity is roughly its share of the power.
5. **Precision stage** (when the window holds >= 7.5 periods). The fundamental
   is demodulated in two Blackman-Harris sub-windows of `max (5.5 periods,
   n / 2)` at the start and end of the window. For a partial at `w1`,
   `c_B / c_A = exp (j w1 spacing)` regardless of the demodulation frequency,
   window shape or exponential decay, so `arg (c_B conj (c_A))` is its phase
   advance (the phase-vocoder principle). An intermediate sub-window 20 cycles
   in resolves the cycle count first, which tolerates a 40-cent error in the
   periodicity estimate. The 4-term Blackman-Harris main lobe is +-4 bins, so
   with >= 5.5 periods the second partial falls in sidelobes below -92 dB.
   Half-window sub-windows have about the white-noise precision of thirds but
   half the main-lobe width, which matters for mains hum near the fundamental.
6. **Weak fundamental** (< 0.3% of the power, e.g. a bridge pickup on a bass).
   Partials 2 and 3 are measured the same way. Since `(f_n / n)^2 = f0^2 (1 + B
   n^2)` is linear in `n^2`, `f1^2 = 1.6 (f2/2)^2 - 0.6 (f3/3)^2` with no need to
   know B. Without partial 3, `f2 / 2` is used (sharp by ~1.5 B). Without
   partial 2 either, the periodicity estimate is returned.

### PitchTracker

- **Decimation.** Butterworth in double precision. Phase response is
  irrelevant: an LTI filter keeps a periodic signal periodic. Content that would
  alias below 3 kHz is attenuated by >= 60 dB.
- **Onsets.** The note emerging from silence (gate opening, with a 3 dB
  hysteresis), or the first-difference energy of a hop exceeding 4x (6 dB) the
  maximum of the hops 10-45 ms before. The maximum covers a full period of the
  lowest note, so a note's own waveform cannot trigger it. 80 ms refractory.
  A pitch change of more than 50 cents in the median (legato, slide) also
  restarts the segment, and counts as an onset when the detection is clear
  (clarity >= 0.8).
- **Attack skip.** A plucked string reads sharp while the large-amplitude
  tension decays. Settled windows start 150 ms after the onset, or at half the
  time since the onset if later. The latter discards a slow glide's tail
  whatever its time constant.
- **Two detections per hop.** The reading uses a short window (8 periods,
  >= 40 ms) so the needle follows a turning peg. The settled detection uses up
  to 320 ms after the skip; its long sub-windows average noise and reject
  interference near the fundamental that the short window cannot (60 Hz hum
  28 dB under an E2 swings short-window readings by +-8 cents).
- **Stability.** Settled detections (clarity >= 0.8, within 30 cents of the
  reading) from the last 0.4 s. `stable` requires the span to be covered (>= 75%
  of hops), mean clarity >= 0.9 and std-dev < 0.5 cent. `stableHz` is the mean,
  so it follows the slow flat drift of a decaying string with a lag of about
  0.36 s (half the span plus half the settled window). In the tests, two
  strings ringing together never reach the clarity for `stable`, so nothing is
  captured from a mixture. A string wobbling by a cent drops `stable` now and
  then during its sustain; the intonation workbench needs it once per capture,
  not throughout ([../intonation.md](../intonation.md)).
- **Validity.** At least two detections in the median buffer, the latest
  within 50 cents of the median, and no gap over 3 hops. Readings near 50, 60,
  100 or 120 Hz below -40 dBFS are ignored as mains hum. None of these is
  within 20 cents of an equal-tempered note, so only a quiet, badly detuned
  string is affected.
- `onsetId` is monotonic across `reset()` so a UI comparing ids never misses a
  pluck.

## 3. Display

The tuner needle used to follow the short-window reading through a 62 ms exponential, and the
gauge changed state on every event. On a real guitar that jumps: the short window swings with
hum, is pulled by a string still ringing, follows the pick's sharp attack and falls back to the
centre when the note dies. The display is now decided in the engine (shared by every host,
tested on the real tracker) and finished in the gauge.

### Bars (set before the work, from the 0.2.0 numbers below)

- Sustained note after it settles: needle jitter under 0.1 cent RMS and 0.3 cent peak, hum 28 dB
  under the note included; never busier than a string that really wobbles.
- 20-cent step (a peg turned between plucks, or quickly): within 1 cent in under 250 ms, no
  overshoot (< 0.2 cent). A peg turned at up to 30 cents/s: no more lag than 0.2.0's 3.1 cents,
  at rest on the pitch within 250 ms of the turn ending.
- From pick to the end of the decay: zero note-name changes, in tune entered at most once and
  never left for a string in the zone; never shown in tune for a string out of it; no sharp flash
  from the attack.
- When the note dies: the needle stays where it was (< 0.1 cent) and the note stays.
- The numbers change at most four times a second; screen readers hear a state once.

### Engine: `TunerDisplay` (per 5 ms hop)

1. **Two estimates.** The short reading (median of five, 8 periods) and a *display detection*:
   the settled window again, but starting no earlier than the last move of the pitch. While the
   pitch holds it is the settled window and its detection is reused (no extra cost); after a move
   it restarts at the move's end and grows to 320 ms. It accepts the detector's own clarity gate
   (0.5) rather than the stability window's 0.8: with the previous string ringing the mixture's
   clarity stays near 0.65, and the long window is then the only estimate that is right (the
   short window's demodulator main lobe, +-4 bins of 5.5 periods, reaches the other string's
   partials 31 Hz away and reads +10 cents; the 160 ms sub-windows do not).
2. **Inverse-variance blend.** Each estimate's scatter is measured online as half the mean square
   hop-to-hop difference (the variance of white noise; time constants 0.1 and 0.05 s). A long
   window that just (re)started starts at four times the reading's variance and earns its weight
   as it proves steady; one that only missed a hop keeps its record (resetting it there let a
   biased reading take over again, measured: 2 cents of drift with a string ringing). Measured
   scatter with hum 28 dB under an E2: reading 2.5 cents, long window 1-3 cents at 100-200 ms,
   0.01 cent at 320 ms; so the blend follows the reading early and the long window soon after.
3. **Moves.** The reading minus the long window, low-passed over 20 ms, against their usual
   disagreement. That is learned over the first 100 ms of each segment (bias included: a ringing
   string makes it 8-10 cents, hum 4-5), then adapts down in 0.4 s and up only in 3 s. Adapting
   up quickly absorbed a slow peg turn (the threshold ratcheted from 1.5 to 8 cents and the
   needle lagged 5.9 cents). A move needs 3 times that spread and at least 1.5 cents (a real
   string's +-1 cent wobble stays under it). During a move the tracker restarts the display
   window every hop and the display follows the reading (time constant 25 ms or less) until the
   reading's slope (hop differences low-passed over 40 ms) stays under 6 cents/s, or under what its
   scatter explains, for 15 ms (and at least 60 ms after the start). Ending the move on the
   reading rather than when the long window catches up cut the peg-turn lag from 3.0 to 2.4
   cents; re-anchoring alone left the long window cycling between 75 and 220 ms.
4. **Smoothing.** First order, `tau = clamp (hop / 2 * var / 0.05^2, 80 ms, 0.5 s) /
   (1 + (error / 3 sigma)^2)`, at least 10 ms. The first factor averages the measurement's noise
   down to about 0.05 cent; the second follows an error well beyond the noise at once, like the
   One-Euro filter's speed term (Casiez et al.), but in units of the measured noise rather than
   fixed cents per second. Both poles of the chain (this and the gauge's spring) are real, so a
   step never overshoots.
5. **Attack.** A new note's first readings come from short windows during the pick transient
   and read sharp (and, with hum, off by up to 16 cents). They are followed out of sight for
   50 ms (time constant 35 ms) while the note shown so far stands, then shown; until the long
   window is ready the display follows the reading with that time constant. A re-pluck of the
   note shown keeps the needle still until the long window is ready (~150-250 ms), then moves it
   once: measured on a retuned E4, no sharp swing at all (0.2.0 swung to +2 cents, following a
   reading of +6) and on pitch 0.22 s after the pick.
6. **Note lock.** The note changes only beyond +-60 cents of the one shown, for 60 ms. A pitch
   change without a pluck (the tracker's legato restart, an octave slip) holds the needle for
   120 ms and is taken only if it persists: a hammer-on switches once, 0.17 s after it.
7. **Hold.** No valid reading for 80 ms: `displayHeld`, and the value, note and settled flag
   stand. With the long window unavailable at the end of the decay the value also stands unless
   the reading moves. `displaySettled`: from the long window, not moving, window >= 150 ms,
   scatter < 0.1 cent and within 0.3 cent of it.

### Gauge (`ui/src/features/tuner`)

- **Readout** (`readout.ts`), on every event with the clock (`performance.now()`, as the
  workbench times its holds): in tune needs a settled reading inside the zone for 0.15 s; it ends
  after 0.25 s beyond the zone plus a margin (`max (0.3, zone / 4)`: 1.3 cents for the tuner,
  6.25 for the workbench's 5), or 0.06 s when far out. Before a note's first settled reading only
  a reading more than 5 cents beyond the zone, for 0.15 s, is called sharp or flat; otherwise the
  gauge says nothing ("listening"). Sharp or flat give way to listening once the needle is
  inside the zone by the margin. A first reading that is already settled shows its state at
  once. Text: tenths only when settled (kept through a re-pluck for 0.5 s unless it moves), at
  most every 0.25 s, and only after the value moves 1.5 steps (tenths) or 0.75 (whole cents):
  0.12 s still let a wobbling string change its tenths 7 times a second.
- **Needle** (`needle.ts`): a critically damped spring, 60 rad/s, integrated exactly per frame
  (stable at any frame time; a 20-cent jump is within 1 cent in 80 ms and never overshoots). A
  held reading stays at full strength for 1.5 s, then fades to 45 % over 1 s; the strobe stops.
- **Words:** "In tune", "Tune up", "Tune down" under the numbers, so the state is not colour
  alone. Screen readers hear the state once it has held 0.6 s, at most every 2 s, and nothing
  while listening or held (`TunerGauge.test.tsx`: 10 s of a reading wandering at the zone's
  edge are announced once).

### Measured

Engine display alone (`[.report][tuner-report]`, 48 kHz, read at 60 Hz). Jitter is the RMS of
the needle around its own 0.3 s moving average (a wobble or sag is not jitter); error is the RMS
against the true pitch; both over the sustained part.

| Scenario | jitter 0.2.0 -> now | error | in tune entered/left (naive band) | other |
|---|---|---|---|---|
| E2, hum 28 dB under | 1.35 -> 0.006 c | 1.65 -> 0.01 c | 11/11 -> 1/0 | |
| A2, 50 Hz hum | 0.052 -> 0.004 | 0.15 -> 0.00 | 1/1 -> 1/1 (attack) | |
| G3, +-1 c wobble, sag, hum | 0.24 -> 0.17 (string: 0.36) | 0.44 -> 0.52 | 17/17 -> 1/1 | |
| A2 while E2 rings | 1.00 -> 0.003 | 2.79 -> 0.00 | 15/11 -> 1/0 | dropouts 7 -> 0 |
| E2, fundamental -20 dB | 0.070 -> 0.023 | 0.07 -> 0.03 | 1/1 -> 1/0 | |
| A2 peg turn, 20 c in 1 s | | | | lag 3.10 -> 2.40 c, settle 17 ms |
| G3 20-cent step | | | | within 1 c 217 -> 117 ms, overshoot 0.00 |
| D3 dies | | | | needle moves 7.03 -> 0.01 c after the end |

The whole path, engine display through the gauge's readout and needle at 60 frames per second
(`ui/src/features/tuner/replay.test.ts` asserts the bars on these streams; the plots are made
from the same replay): 0.2.0 -> now.

| Scenario | jitter RMS (peak) | in tune entered/left | other |
|---|---|---|---|
| E2, hum 28 dB under | 1.39 (3.29) -> 0.002 (0.01) c | 11/11 -> 1/0 | text changes 12.2 -> 0.6 per s |
| G3, wobble, sag, hum | 0.22 (0.51) -> 0.11 (0.25) | 17/17 -> 1/1 | text 12.2 -> 4.4 per s |
| B3 0.6 c sharp, +-0.3 c wobble | 0.049 (0.11) -> 0.034 (0.08) | 1/0 -> 1/0 | |
| A2 while E2 rings | 0.98 (2.19) -> 0.002 (0.00) | 15/11 -> 0/0 | error 2.84 -> 0.01 c |
| A2 peg turn | | 1/0 -> 1/0 | lag 3.10 -> 2.72 c, overshoot 0.01 |
| G3 20-cent step | | 1/0 -> 1/0 | within 1 c 223 -> 157 ms, overshoot 0.00 |
| E4 retuned, picked again | | 1/0 -> 1/0 | no sharp swing |
| D3 dies | | 0/0 -> 0/0 | needle moves 7.03 -> 0.00 c |
| G3 +-12 c vibrato | 4.0 -> 5.3 (vibrato: 9.7) | 6/6 -> 1/1 | follows the vibrato more |

The first needle comes 50 ms later than in 0.2.0 (100-140 ms after the pick); the note name
with it. Cost: the display detection runs only while its window differs from the settled one;
on plucks the tracker costs 0.52-0.74 % of one core as before (+0.01-0.03 points), and 0.86 %
(48 kHz) to 0.98 % (192 kHz) when the pitch never stops moving.

### Limits

- **Heavy hum on the attack.** With hum 28 dB under an E2 the first short windows read up to 16
  cents sharp; the needle comes in at +12 and is on pitch 0.25 s after the pick (the state says
  nothing meanwhile). It is then steady; 0.2.0 swung +-5 cents throughout.
- **Another string ringing.** The short window is pulled 10 cents for as long as both ring, so
  until the long window has proved steady (0.6-0.8 s) the needle shows the pull; then it is right.
- **Vibrato** is followed (a wide, fast vibrato reads as a move), not averaged.
- Moves smaller than three times the reading's usual disagreement are followed through the long
  window only, with up to 160 ms of lag (half a 320 ms window); under heavy hum that threshold is
  about 15 cents.

## 4. Controls

| Input | Effect |
|---|---|
| `setReferenceHz (a4)` | Reference for `midiNote` / `cents` (`toNote`); does not affect measurement |
| `prepare (fs)` | Any host rate; D and the filters follow |

`toNote` maps to the nearest note with `cents` in [-50, +50). Exactly +50
cents belongs to the note above.

## 5. References

- P. McLeod, G. Wyvill, "A Smarter Way to Find Pitch", ICMC 2005 (NSDF, key
  maxima, clarity).
- N. H. Fletcher, T. D. Rossing, *The Physics of Musical Instruments*, ch. 2
  (stiff-string partials, inharmonicity coefficient B).
- F. J. Harris, "On the Use of Windows for Harmonic Analysis with the Discrete
  Fourier Transform", Proc. IEEE 66(1), 1978 (4-term Blackman-Harris).
- J. L. Flanagan, R. M. Golden, "Phase Vocoder", Bell Syst. Tech. J. 45, 1966
  (frequency from phase advance between frames).
- R. Bristow-Johnson, "Audio EQ Cookbook" (filter sections, via `core/Biquad`).
- pffft (J. Pommier; marton78 fork, BSD-style licence) for the FFTs.
- G. Casiez, N. Roussel, D. Vogel, "1 € Filter: A Simple Speed-based Low-pass Filter for Noisy
  Input in Interactive Systems", CHI 2012 (a low-pass whose cutoff rises with the error).
- V. Katkovnik, L. Stanković, "Instantaneous frequency estimation using the Wigner distribution
  with varying and data-driven window length", IEEE Trans. Signal Processing 46(9), 1998 (the
  longest window over which the frequency holds; here the test learns the short estimate's bias).
- Inverse-variance (minimum-variance) weighting of independent estimates, as in any text on
  estimation, e.g. S. M. Kay, *Fundamentals of Statistical Signal Processing: Estimation Theory*,
  1993.
- Critically damped second-order response, as used for meter movements and UI springs: two real
  poles, the fastest approach without overshoot.

## 6. Tests

Synthetic plucked string (`PitchTestSignals.h`): additive partials at
`n f1 sqrt ((1 + B n^2) / (1 + B))`, so the fundamental is exactly the true
pitch. Amplitudes `sin (n pi p) / n`, faster decay for higher partials, an
attack glide of +9 cents decaying with tau = 50 ms, a pick-noise burst and a
-75 dBFS noise floor.

- Detector on steady harmonic tones, 30.87-1318.5 Hz at 11.025 and 12 kHz:
  every single shot < 0.5 cent, mean < 0.1 cent.
- Tracker on steady tones at 44.1/48/96 kHz: every reading < 0.5 cent after
  0.6 s, mean < 0.1 cent, `stableHz` < 0.1 cent.
- Tracker on plucked E2, A2, D3, G3, B3, E4 (B = 1e-4) and 12th-fret E5
  (B = 4e-4) at 44.1/48/96 kHz: every `stable` reading within 0.15 cent of the
  fundamental. No spurious onsets during the sustain.
- First valid reading < 100 ms (< 120 ms for E2). >= 99% of valid readings
  after the skip on the right note, three pluck positions each.
- Re-pluck increments `onsetId` once and restarts the settled segment.
- White noise at -30 dBFS and silence: never valid. A note at -70 dBFS: not
  valid. Noise and out-of-range tones at the detector level.
- Reference 432 Hz reads midi 69 / 0 cents; 440 Hz reads +31.77 cents.
  `toNote` at +-49.9 / +-50.1 / exactly 50 cents and the extreme notes.
- Slow flat drift (-1.5 cents/s): `stableHz` tracks the pitch 0.36 s earlier
  within 0.1 cent.
- Robustness: 50/60 Hz hum at -45 dBFS, fundamentals 20-30 dB down (including
  B0), a quiet note 34 dB under full scale over -66 dBFS noise. All stable
  within 0.15 cent. Idle hum is never valid. With the previous string ringing,
  the new string is read and nothing else; a legato change starts a new
  segment within 100 ms.
- Arbitrary push block sizes, `reset`, re-`prepare` at another rate.
- The display (`TunerDisplayTests.cpp`, tag `[display]`; scenarios in `TunerScenarios.h`, signals
  as WAV with `SEDEF_TUNER_DUMP=<dir>` and `[tuner-dump]`): steady within 0.05 cent RMS with hum,
  a weak fundamental, a bass E1 and another string ringing; settled within 0.8 s (1.2 s with
  another string); a peg turn followed with under 3 cents of lag and on pitch within 150 ms of
  its end; a 20-cent step within 1 cent in 180 ms; no overshoot; a re-pluck never swings sharp;
  a dying note held; one note change for a hammer-on; a reading pulled 8 cents by another string
  is not taken for a move. The UI replays the same streams (`fixtures/streams.json`, regenerate
  with `SEDEF_TUNER_FIXTURES=<file>` and `[tuner-fixtures]`).
- The intonation workbench end to end (`PitchIntonationTests.cpp`, tag `[intonation]`): open
  strings and 12th-fret octaves with glide, +-1 cent of random wobble (`Wobble` in
  `PitchTestSignals.h`), flat drift, decay, noise and hum, read at 60 Hz and captured with the
  workbench's rule: every note stable by 0.8 s, every octave within 0.3 cent (0.6 cent with a
  wobble as slow as 0.4 Hz). Hidden `[.report]` compares the rule with the previous one; see
  [../intonation.md](../intonation.md).

Measured (48 kHz; 44.1 and 96 kHz are within 0.01 cent):

| Note | first valid | first stable | max \|stableHz err\| | std at 2 s |
|---|---|---|---|---|
| B0 30.87 (bass) | 96 ms | 920 ms | 0.009 c | 0.017 c |
| E1 41.20 (bass) | 76 ms | 791 ms | 0.013 c | 0.013 c |
| E2 82.41 | 76 ms | 651 ms | 0.037 c | 0.005 c |
| A2 110.0 | 60 ms | 625 ms | 0.050 c | 0.002 c |
| D3 146.83 | 45 ms | 605 ms | 0.056 c | 0.002 c |
| G3 196.0 | 40 ms | 596 ms | 0.064 c | 0.003 c |
| B3 246.94 | 36 ms | 591 ms | 0.066 c | 0.001 c |
| E4 329.63 | 31 ms | 591 ms | 0.067 c | 0.002 c |
| E5 659.26 (12th fret) | 20 ms | 591 ms | 0.067 c | 0.001 c |
| E6 1318.5 (24th fret) | 20 ms | 591 ms | 0.067 c | 0.001 c |

The largest error is at the first stable reading (the last of the glide); it
converges to < 0.01 cent. Detector single shots on steady tones: <= 0.031 cent.

Cost (Apple M-series, one core, two or three detections per 5 ms hop): 0.5% at
44.1 kHz, 0.6% at 48 kHz, 0.65% at 96 kHz, 0.75% at 192 kHz; up to 1% while the
pitch keeps moving (section 3). `detect` takes 4-8 us for a 40 ms window and
40-50 us for the full 320 ms window.

## 7. Limitations and future work

- **Interference within about 20 Hz of the fundamental** (inside the main lobe
  of the 160 ms sub-windows) is not rejected. For example, 50 Hz hum at -45 dBFS next to a
  bass E1 (8.8 Hz away) prevents `stable`; readings stay correct to a few cents.
- **Two strings at similar levels.** The tracker reads the louder, more recent
  one (100% right in tests) but cannot settle until the other decays. A soft
  pluck under a still-ringing string may not raise `onsetId` (the reading and
  segment still follow the pitch change). Repeated picking faster than ~4 per
  second at constant level is not segmented. A phase-based onset detector
  (complex-domain deviation) would catch these.
- **Low SNR.** A B0 about 10 dB over a white noise floor does not settle.
- **Attack glides much longer than 150 ms** (+20 cents, tau = 80 ms) leave up
  to ~0.5 cent in the first stable mean; later readings converge.
- The stiff-string fallback assumes the ideal `sqrt (1 + B n^2)` law for
  partials 2-3. Real strings follow it closely for low partials; end effects
  are not modelled.
- The synthetic tests are additive models. Validation on recordings of real
  instruments (open string vs 12th fret, several pickups) is still to do.
