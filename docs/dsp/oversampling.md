# Oversampling and latency

Source: `engine/include/sedef/core/Oversampler.h`, `engine/src/Oversampler.cpp`; the capture
stage's rate conversion in `engine/src/dsp/capture/RateBridge.cpp`; the setting in
`plugin/src/PluginProcessor.cpp` and `ui/src/features/settings/SettingsPanel.tsx`. Tests:
`engine/tests/core/OversamplerTests.cpp`, `engine/tests/engine/LatencyTests.cpp`,
`engine/tests/pedals/OversamplingModeTests.cpp`, `engine/tests/capture/RateBridgeTests.cpp`, and
the hidden report `engine/tests/engine/OversamplingReport.cpp`
(`sedef_engine_tests "[oversampling-report]"`), which prints every table on this page.

## 1. What it does

Every nonlinear stage runs inside its own `Oversampler`: the seven drive pedals (Remora, Urchin,
Sargasso, Pearl, Kelp, Coral, Breaker) and the amp. Each upsamples 4× at 44.1/48 kHz (2× at
88.2/96 kHz, 1× above 100 kHz), distorts at the high rate and comes back down, so the harmonics a
clipper makes above the base rate's Nyquist are filtered off before they can fold into the band
(ADR-0008).

The filters cost time. Up to 0.4.0 they were linear-phase FIRs, 37 samples per stage at
44.1/48 kHz. A switched-off pedal keeps its latency (its dry path is delayed by it), so the host's
delay compensation never moves when a pedal is switched, and the eight stages in series came to
298 samples, 6.2 ms at 48 kHz. Too much to play through live.

Now the default is **minimum-phase IIR half-bands**: 5 samples per stage, 41 for the
whole rig, 0.85 ms at 48 kHz, with stronger alias rejection than before and the same tone. The
FIRs remain as **Linear phase** in Settings > Latency.

## 2. The two modes

| | Low latency (default) | Linear phase |
|---|---|---|
| Filters | polyphase all-pass half-bands (elliptic) | Kaiser-windowed half-band FIRs (0.4.0) |
| Per stage, 44.1/48 kHz | 5 samples | 37 samples |
| Per stage, 88.2/96 kHz | 3 samples | 31 samples |
| Rig, 48 kHz | 41 samples, 0.85 ms | 298 samples, 6.2 ms |
| Phase | delay rises in the top octave (§4) | the same delay at every frequency |
| For | playing live | mixing, re-amping, bit-for-bit 0.4.0 sound |

`oversampling` is a global, non-automatable choice parameter (appended, ADR-0004), kept out of
presets like `stereoMode`. A change prepares the engine again at its rate and block size: the
plug-in holds the audio callback off (the host plays silence for those few milliseconds), rebuilds
the capture and the custom IRs, and reports the new latency with `setLatencySamples`, which hosts
accept as a settings change. The editor sends the new figure to the page (`sedef:session`), so the
status bar and Settings follow it. `Engine::setOversampling()` sets it for the next `prepare()`;
`sedef-render --set oversampling=1` renders in linear phase.

## 3. The minimum-phase half-band

### Structure

```
H(z) = 1/2 [ A0(z²) + z⁻¹ A1(z²) ],    A(z²) = Π_k (a_k + z⁻²) / (1 + a_k z⁻²)
```

Two branches of first-order all-pass sections, coefficients a₀, a₂, … in A0 and a₁, a₃, … in A1
(Valenzuela and Constantinides 1983; Regalia, Mitra and Vaidyanathan 1988; harris 2004, ch. 10).
Both branches run at the low rate:

- **Up:** the even outputs are A0 of the input, the odd outputs A1 (the ½ of H and the 2 of
  interpolation cancel).
- **Down:** y[m] = ½ (A0 u[2m] + A1 u[2m−1]).
- **Each section:** y[n] = a (x[n] − y[n−1]) + x[n−1]: one multiply per coefficient per low-rate
  sample, in each direction.

The round trip is H(z)² at the high rate, kept on the even phase, so its delay is a whole number
of high-rate samples apart from the filters' own.

### Design

The coefficients are those of an elliptic half-band, in closed form:

1. The transition band's half-width t gives the passband edge (¼ − t)·f_high and the modulus
   k = tan²((1 − 2t)π/4).
2. The nome q comes from the series of Abramowitz and Stegun 17.3.21, with
   e = ½ (1 − √k′)/(1 + √k′) and q = e + 2e⁵ + 15e⁹ + 150e¹³.
3. For order N = 2n + 1, each coefficient follows from the theta-function series of the Jacobi
   elliptic sn at the pole positions. For c = 1…n:

   ```
   w = q^¼ Σ_{i≥0} (−1)^i q^{i(i+1)} sin((2i+1)cπ/N)  /  (½ + Σ_{i≥1} (−1)^i q^{i²} cos(2icπ/N))
   x = √((1 − w²k)(1 − w²/k)) / (1 + w²)
   a = (1 − x)/(1 + x)
   ```

4. The stopband attenuation is −10 log₁₀(4q^{N/2} / (1 + 4q^{N/2})).

The same closed form is in Laurent de Soras' public-domain (WTFPL) HIIR library, used only as a
cross-check. The tests measure every design's response rather than trusting the formula.

### Group delay, made a whole number

A section (a + z⁻²)/(1 + a z⁻²) delays DC by 2(1 − a)/(1 + a) high-rate samples. The two branches
average, so one pass of H delays DC by

```
τ_H(0) = ½ + Σ_k (1 − a_k)/(1 + a_k)        (high-rate samples)
```

Up and down through a stage at 2^s times the base rate is 2τ_H / 2^s base-rate samples. At 4× the
round trip is τ_H1 + τ_H2 / 2.

The 4× stage is designed first. Then the 2× stage's transition is tuned (bisection) so the round
trip's delay at DC is exactly 5 base-rate samples, and the 2×-only design so it is exactly 3. So
`latencySamples()` is an integer and the true delay at guitar frequencies, not a rounding of it.
Every stage, the latency-aligned bypasses and the board's padding then add up exactly.

| Factor | Stage | Coefficients | Transition t | Stopband | Delay at DC |
|---|---|---|---|---|---|
| 4× | ×2 | 9 | 0.021664 | 93.9 dB | 3.510 high-rate samples |
| 4× | ×4 | 5 | 0.136621 | 100.3 dB | 2.980 high-rate samples |
| 4× | round trip | | | | **5.000** base-rate samples |
| 2× | ×2 | 5 | 0.141531 | 102.1 dB | **3.000** |

The 4× stage's transition, (1 − 20/44.1)/4, puts its stopband where the images and aliases of the
band below 20 kHz at 44.1 kHz fall. Coefficients:

- 4×, stage ×2: 0.044701194, 0.163979018, 0.322991099, 0.486226699, 0.630678362, 0.747759114,
  0.838887676, 0.910502497, 0.971011254;
- 4×, stage ×4: 0.043751629, 0.165966744, 0.346406253, 0.569034401, 0.837719989;
- 2×: 0.042670824, 0.162408393, 0.340698485, 0.563150940, 0.834766953.

### Why 5 and not 1 or 2

A minimum-phase filter's delay at DC is fixed by its magnitude (Bode's gain–phase relation):
τ(0) = (1/2π) ∫₀^π −ln|H(θ)| / sin²(θ/2) dθ. Rejecting 90 dB from 28 kHz up at the 96 kHz stage
costs at least about 3.4 high-rate samples per pass, about 1.7 base-rate samples. Up and down, plus
the 4× stage, make about 4.5. The elliptic designs sit close to that bound. 4 samples would mean
giving up 15 to 25 dB of rejection (n₁ = 7, n₂ = 4: 88 and 71 dB), below 0.4.0's FIR. 5 is the
smallest whole number that keeps both stages above 90 dB.

## 4. Delay against frequency

The round trip's phase delay, measured with sines (`OversamplerTests`):

| Base rate | 100 Hz | 1 kHz | 5 kHz | 10 kHz | 15 kHz | 20 kHz |
|---|---|---|---|---|---|---|
| 44.1 kHz | 5.00 | 5.00 | 5.08 | 5.35 | 5.96 | 5.48 |
| 48 kHz | 5.00 | 5.00 | 5.07 | 5.29 | 5.76 | 4.45 |
| 88.2 kHz (2×) | 3.00 | 3.00 | 3.01 | 3.05 | 3.12 | 3.23 |
| 96 kHz (2×) | 3.00 | 3.00 | 3.01 | 3.04 | 3.10 | 3.19 |

Flat to 0.1 sample up to 5 kHz. The rise above that is an elliptic filter's, confined to the top
octave, where a guitar DI carries little (−40 to −60 dB under the fundamental) and the cab after
the amp cuts 30 to 60 dB more. It is under a millisecond of group-delay change, below the
thresholds measured for group-delay distortion (Blauert and Laws 1978).

## 5. Latency

What `Engine::latencySamples()` reports, whatever is switched on:

| Rate | Low latency | Linear phase |
|---|---|---|
| 44.1 kHz | 40 samples, 0.91 ms | 296 samples, 6.71 ms |
| 48 kHz | 41 samples, 0.85 ms | 298 samples, 6.21 ms |
| 88.2 kHz | 31 samples, 0.35 ms | 289 samples, 3.28 ms |
| 96 kHz | 32 samples, 0.33 ms | 295 samples, 3.07 ms |
| 176.4 kHz | 21 samples, 0.12 ms | 143 samples, 0.81 ms |
| 192 kHz | 23 samples, 0.12 ms | 155 samples, 0.81 ms |

Low latency at 48 kHz is seven pedals × 5, plus the capture stage's 6: the amp's 5, and one more
because a capture made at 44.1 kHz needs 5.7 (§8). Above 100 kHz nothing oversamples, and only the
capture conversion remains.

**Reported against measured** (`LatencyTests`):

- **The plain rig.** With the pedals off and an identity capture at the session rate playing
  without the cab, every stage is a whole-sample delay. An impulse arrives exactly at the reported
  latency, in both modes, at 44.1, 48, 96 and 192 kHz.
- **The whole rig.** At 110, 330 and 1000 Hz, the low-latency rig lags its reported latency by what
  the linear-phase rig (exact by construction) lags its own, to within half a sample. That holds
  with the default amp and cab, with a booster into an overdrive, and with a capture made at
  44.1 kHz. What remains is the amp's, cab's and pedals' own voicing (their filters' phase, the
  mic's distance from the cone), which is the same in both modes.
- **Switching a pedal.** It shifts the timing at guitar frequencies by its own voicing alone: the
  same in both modes, to 0.1 sample.

**Why not a few samples for the whole rig.** Each oversampled stage owns its oversampler, and a
switched-off pedal must keep its latency so the host never re-compensates. Eight stages in series
cost at least eight round trips, and §3 puts each at about 4.5 samples at best for this rejection.
Running the whole board in one shared oversampled domain (one round trip for every pedal) would
get under 10 samples. But it would move every pedal's linear filters to the high rate, which
changes their voicing, and it would replace the per-pedal design that the pedalboard builds on.
That is left for later.

## 6. Aliasing

**The filters.** A tone written into the oversampled stream (what a waveshaper's harmonics are)
that would fold to 20 kHz or below on the way down. The worst case, from the oversampler alone:

| Rate | Linear phase (0.4.0) | Low latency |
|---|---|---|
| 44.1 kHz | −25.0 dB | −93.9 dB |
| 48 kHz | −69.2 dB | −93.9 dB |
| 88.2 kHz | −101.0 dB | −102.1 dB |
| 96 kHz | −103.1 dB | −102.1 dB |

The FIR's wide transition let content between 24 and 26 kHz fold into the top of the band at
44.1 kHz. The elliptic stages reject everything that lands below 20 kHz by 94 dB at both base
rates. Images of every input tone up to 20 kHz are at least 90 dB down (`OversamplerTests`).

**Per stage.** The worst inharmonic component, in dB re. the strongest harmonic, at full drive,
for notes E6 and E7 at 44.1 and 48 kHz:

| Stage | Linear phase | Low latency |
|---|---|---|
| Remora | −101.6 | −101.6 |
| Urchin | −60.0 | −60.0 |
| Sargasso | −74.4 | −74.4 |
| Pearl | −78.6 | −78.6 |
| Kelp | −78.1 | −78.1 |
| Coral | −68.0 | −68.0 |
| Breaker | −101.6 | −101.6 |
| Lagoon | −99.7 | −98.9 |
| Reef | −97.2 | −97.7 |
| Abyss | −73.2 | −73.2 |

These are unchanged: what remains is folded inside the oversampled domain by the nonlinearity
itself (harmonics beyond the 4× rate's Nyquist), which both filters pass alike. The filters'
own leakage is 20 dB or more below it. Every pedal's and amp's aliasing test still passes.

## 7. Tone

Every oversampled stage sounds the same in both modes (`OversamplingModeTests`):

- **Small-signal response.** At −40 dBFS, with each stage's main knob at 0, 0.5 and 1, the
  response at 100 Hz to 18 kHz agrees to 0.05 dB at 44.1 and 48 kHz (measured: to 0.01 dB).
- **Distortion.** At −20 dBFS, 110, 440 and 1320 Hz, every harmonic within 60 dB of the strongest
  agrees to 0.05 dB (measured: 0.001 dB). THD at A3 (220 Hz), full drive:

  | Stage | Linear phase | Low latency |
  |---|---|---|
  | Remora | 40.32 % | 40.32 % |
  | Sargasso | 21.71 % | 21.71 % |
  | Pearl | 28.84 % | 28.84 % |
  | Kelp | 31.02 % | 31.02 % |
  | Coral | 42.28 % | 42.28 % |
  | Breaker | 2.53 % | 2.53 % |
  | Lagoon | 5.46 % | 5.46 % |
  | Reef | 39.96 % | 39.96 % |
  | Abyss | 18.14 % | 18.14 % |

  Urchin's note leaves an octave up, so its fundamental's THD means nothing; its harmonics agree
  like the rest.
- **The amp's phase.** At 100 Hz to 3 kHz, each model's phase (its voicing's) is the same in both
  modes once each mode's latency is taken off, to 0.05 samples (`AmpSectionTests`).

**Parallel paths.** Pearl and Kelp sum a clean term kept at the base rate with the oversampled
clipper, as the op-amp circuits do. A delay lines the two up only when the filters are linear
phase. With the minimum-phase ones, the top octave combed: Pearl at low gain lost 1 dB at
10 kHz, with a notch at 15 kHz. `AlignedDryPath` gives the clean term the same up and down with
nothing in between (a delay in linear phase, so that mode stays bit for bit 0.4.0), and the
response matches to the table above.

## 8. Bypass crossfades

A latency-aligned bypass fades over 10 ms between the dry path, delayed by the whole samples the
pedal reports, and the pedal. In linear phase the oversampler is that delay at every frequency.
In minimum phase it isn't in the top octave (§4), so mid-fade the two partly cancel there.

With the bare oversampler (a pedal that changes nothing, the worst case), the lowest level during
a fade, below the steady level (`OversamplerTests`, bounded there):

| Base rate | ≤ 5 kHz | 10 kHz | 12 kHz | 15 kHz | 18 kHz |
|---|---|---|---|---|---|
| 44.1 kHz | < 0.004 dB | 0.27 dB | 0.95 dB | 5.7 dB | 4.7 dB |
| 48 kHz | < 0.003 dB | 0.16 dB | 0.53 dB | 2.7 dB | 24.8 dB |

The 15 and 18 kHz figures are computed from the measured phase; the test bounds up to 12 kHz.

The dip lasts a few milliseconds, only above 10 kHz, at a pedal's input, ahead of an amp and cab
that cut that band by 30 to 60 dB. At the rig's output a footswitch press differs from an ideal
crossfade by the same amount in both modes. That difference comes from the pedal restarting from
a clean state, not from the filters. Each oversampled pedal switched on under a strum through the
default rig at 48 kHz (dB re. the signal over the fade, best alignment):

| Pedal | Linear phase | Low latency |
|---|---|---|
| Remora | −19.6 | −19.4 |
| Urchin | −12.0 | −12.6 |
| Sargasso | −16.6 | −21.4 |
| Pearl | −21.2 | −20.8 |
| Kelp | −21.5 | −20.3 |
| Coral | −21.8 | −20.5 |
| Breaker | −14.2 | −19.8 |

So the dry path stays a whole-sample delay (and the pedalboard's padding with it). An all-pass
phase match on it would cost a second oversampler per bypassed pedal and change nothing
measurable at the output. The engine tests check the fuzzes' and booster's switches in both modes
against the ideal crossfade, with no edge beyond it.

## 9. Captures: the rate conversion

A capture made at another rate runs through `RateBridge` (docs/dsp/nam.md §6). In linear phase its
windowed sinc looks 16 zero crossings ahead: 39 samples at 48 kHz for a 44.1 kHz capture, more than
a pedal. In low latency it uses `MinimumPhaseKernel`, the minimum-phase version of the same
prototype, from its real cepstrum (Oppenheim and Schafer, homomorphic deconvolution). That gives
the same magnitude response (to 0.01 dB in the pass band, −78 dB beyond the cut-off) with nothing
ahead: the delay is the kernel's own group delay, 2.361 zero crossings, about 0.12 ms for the round
trip at every host rate. The kernel is built once per process, when the first capture made at another rate loads (under
0.2 s).

The stage's latency is the larger of the amp's and the rounded conversion delay for captures made
at 44.1, 48, 88.2 or 96 kHz:

| Host rate | Amp | Capture stage, low latency | Linear phase |
|---|---|---|---|
| 44.1 kHz | 5 | 5 | 37 |
| 48 kHz | 5 | 6 | 39 |
| 88.2 kHz | 3 | 10 | 72 |
| 96 kHz | 3 | 11 | 78 |
| 176.4 kHz | 0 | 21 | 143 |
| 192 kHz | 0 | 23 | 155 |

A capture lags the stage's latency to half a sample at guitar frequencies. One made below 44.1 kHz
(the validator accepts 16 kHz up) needs more than this budget. It plays with the full filter, a
little late: a 16 kHz capture at 48 kHz arrives 0.2 ms after the amp would.

## 10. CPU

ns per sample at 48 kHz, mono, 64-sample blocks, a guitar strum (Apple M2, `-O3`):

| Stage | Linear phase | Low latency |
|---|---|---|
| Remora | 214 | 189 |
| Urchin | 274 | 250 |
| Sargasso | 171 | 148 |
| Pearl | 148 | 182 |
| Kelp | 361 | 393 |
| Coral | 154 | 130 |
| Breaker | 190 | 167 |
| Lagoon | 257 | 248 |
| Reef | 359 | 349 |
| Abyss | 489 | 479 |
| Oversampler alone, 4× | 84 | 62 |
| Oversampler alone, 2× | 49 | 19 |

The all-pass sections take 38 multiplies per sample at 4× against the FIRs' 118, but each one
waits for the last, so the saving is a quarter, not two thirds. Pearl and Kelp pay a second round
trip for their clean term (§7). The capture conversion costs the same in both modes (the same
number of taps).

## 11. References

- R. A. Valenzuela and A. G. Constantinides, "Digital signal processing schemes for efficient
  interpolation and decimation", *IEE Proceedings G*, 130 (6), 1983.
- P. A. Regalia, S. K. Mitra and P. P. Vaidyanathan, "The digital all-pass filter: a versatile
  signal processing building block", *Proceedings of the IEEE*, 76 (1), 1988.
- f. harris, *Multirate Signal Processing for Communication Systems*, Prentice Hall, 2004, ch. 10
  (two-path recursive all-pass filters).
- M. Abramowitz and I. A. Stegun, *Handbook of Mathematical Functions*, 17.3.21 (the nome series).
- L. de Soras, HIIR, <http://ldesoras.free.fr/prod.html> (WTFPL): the closed-form design, used as
  a cross-check only.
- A. V. Oppenheim and R. W. Schafer, *Discrete-Time Signal Processing*, 3rd ed., ch. 13 (the
  cepstrum and minimum-phase reconstruction).
- H. W. Bode, *Network Analysis and Feedback Amplifier Design*, 1945 (the gain–phase relation).
- J. Blauert and P. Laws, "Group delay distortions in electroacoustical systems", *JASA*, 63 (5),
  1978.
- J. O. Smith, *Digital Audio Resampling Home Page*, <https://ccrma.stanford.edu/~jos/resample/>.

## 12. Tests

- **Oversampler** (`OversamplerTests.cpp`):
  - the latency per mode and factor;
  - linear phase: an impulse centred on the latency;
  - minimum phase: the group delay at DC is the reported whole number (analytically to 10⁻⁶,
    measured at 82 Hz–1 kHz to 0.01 sample), and the delay stays within 0.1 sample up to 5 kHz;
  - flat to 20 kHz (±0.01 dB) at 44.1–96 kHz;
  - images and fold-downs at least 90 dB down;
  - block size makes no difference (bit for bit);
  - channels are independent, and the tail decays to exactly zero;
  - the bypass crossfade bound;
  - `AlignedDryPath` equals the round trip.
- **Stages** (`OversamplingModeTests.cpp`, `AmpSectionTests.cpp`): the same response, harmonics
  and amp phase in both modes. Every pedal's latency test (`latencySamples() == os.latencySamples()`)
  and aliasing, response and THD tests pass unchanged in the default mode.
- **Rig** (`LatencyTests.cpp`):
  - the latency per mode at 44.1–192 kHz;
  - the impulse through the plain rig;
  - the whole rig against linear phase;
  - pedal switches.

  `EngineTests` checks the fuzz and booster switches in both modes.
- **Captures** (`RateBridgeTests.cpp`, `CaptureStageTests.cpp`, plug-in `CaptureLibraryTests.cpp`):
  - the minimum-phase kernel's magnitude and delay;
  - the conversion's latency, flatness, block-size independence and rejection;
  - the capture against the amp in time;
  - a capture rebuilt after a mode switch.
- **UI** (`SettingsPanel.test.tsx`): the setting, and the latency figures following it.
