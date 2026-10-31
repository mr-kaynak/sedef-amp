# Tuner and intonation measurement

Code: `engine/include/sedef/analysis/PitchDetector.h`, `PitchTracker.h`,
`engine/src/analysis/`. Tests: `engine/tests/analysis/` (tag `[analysis]`; hidden
`[.report]` prints the accuracy table below, `[.bench]` the cost).

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
```

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
  captured from a mixture.
- **Validity.** At least two detections in the median buffer, the latest
  within 50 cents of the median, and no gap over 3 hops. Readings near 50, 60,
  100 or 120 Hz below -40 dBFS are ignored as mains hum. None of these is
  within 20 cents of an equal-tempered note, so only a quiet, badly detuned
  string is affected.
- `onsetId` is monotonic across `reset()` so a UI comparing ids never misses a
  pluck.

## 3. Controls

| Input | Effect |
|---|---|
| `setReferenceHz (a4)` | Reference for `midiNote` / `cents` (`toNote`); does not affect measurement |
| `prepare (fs)` | Any host rate; D and the filters follow |

`toNote` maps to the nearest note with `cents` in [-50, +50). Exactly +50
cents belongs to the note above.

## 4. References

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

## 5. Tests

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

Cost (Apple M-series, one core, two detections per 5 ms hop): 0.7% at
44.1 kHz, 0.8% at 48 kHz, 0.9% at 96 kHz, 1.0% at 192 kHz. `detect` takes
4-8 us for a 40 ms window and 55-69 us for the full 320 ms window.

## 6. Limitations and future work

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
