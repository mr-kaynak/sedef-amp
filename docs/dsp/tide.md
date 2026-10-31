# Tide (compressor)

`engine/include/sedef/dsp/Tide.h`, `engine/src/dsp/Tide.cpp`

## What it models

A pedal-style compressor for clean playing: smooth and even at low settings, a squishy, sustaining
squeeze at high settings, and quick recovery between staccato chords. It has a fixed threshold and
ratio like a stompbox; the `comp` knob drives the signal into that threshold. A parallel `blend`
keeps the pick attack of the dry signal.

## Signal flow

```
          ┌──────────────────────────────── dry ─────────────────────────────────┐
in ───────┤                                                                      ├─ mix ─► out
          └─► × gain ─► tilt EQ (1 kHz pivot) ─► × makeup·level ── compressed ───┘ (blend)
                ▲
detector: HP 60 Hz ─► × drive (comp) ─► mean square (τ 2 ms up / 6 ms down) ─► dB
          ─► soft-knee 3:1 curve ─► fast stage (attack τ 3 ms, release τ 50 ms) ─┐
                                 └► slow stage (charge τ 800 ms, release τ 400 ms) ─┴─ max ─► gain
```

- **Detector**: a short mean-square window, so it reacts to energy rather than to individual
  waveform peaks, but falls fast enough that the release is governed by the gain-reduction stages
  rather than by the detector. Stereo is linked (the louder channel sets the gain). The 60 Hz
  side-chain high-pass keeps low notes from pumping a whole chord.
- **Static curve**: threshold −12 dB (mean-square level of the driven signal), ratio 3:1, 6 dB
  soft knee (quadratic, after Giannoulis et al.).
- **Program-dependent release**: the fast stage recovers from transients in ~50 ms; the slow stage
  only charges while the signal stays above threshold, so after a long, dense passage it holds the
  gain back and lets it come up gently. The applied reduction is the larger of the two. Measured:
  a 30 ms stab at ~11 dB reduction is back within 1 dB in < 250 ms; the same level held for two
  seconds takes more than three times as long.
- **Attack** (63 % of the final reduction on a step) measures 1–8 ms; about 5 ms in practice.
- No lookahead, so there is no latency and the parallel blend cannot comb-filter.

## Control mapping

| Control | Mapping |
|---|---|
| `comp` | Detector drive 6 dB + 34 dB × comp (6…40 dB). Equivalent to lowering the threshold from −18 to −52 dB (mean square). |
| `tone` | First-order tilt around 1 kHz: tilt = 12 dB × (tone − 0.5), i.e. −t dB at DC, 0 dB at 1 kHz, +t dB at the top (±6 dB at the extremes, exactly flat at 0.5). Acts on the compressed path only. Coefficients are updated every sample while the knob moves (cheap: the pivot is fixed). |
| `level` | Output = automatic makeup + 24 dB × (level − 0.5), i.e. ±12 dB. The automatic makeup cancels the reduction a −24 dB mean-square signal (a typical playing level, about −20 dBFS guitar peaks) would receive at the current `comp`, so turning `comp` up adds sustain rather than volume. |
| `blend` | Linear dry/compressed mix: 0 = dry passthrough (bit-exact), 1 = fully compressed. |

Measured with a guitar-like tone peaking at −20 dBFS, level 0.5, blend 1: output within 0.4 dB of
the input for every `comp` ≥ 0.25 while the reduction goes from 2 to 19 dB. From `comp` 0.5 up,
input 10 dB quieter comes out ~6 dB louder (sustain) and input 10 dB hotter ~7 dB quieter.

Telemetry: `gainReductionDb()` is the largest reduction during the last block (≤ 0 dB).

## References

- D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design — A Tutorial
  and Analysis", *J. Audio Eng. Soc.* 60(6), 2012 — soft-knee gain computer, log-domain smoothing.
- J. D. Reiss, A. P. McPherson, *Audio Effects: Theory, Implementation and Application*, CRC
  Press, 2014 — dynamics processors, program-dependent release.
- U. Zölzer, *Digital Audio Signal Processing*, 2nd ed., Wiley, 2008 — first-order shelving filters.
- The two-stage release constants, the drive/auto-makeup mapping and the tilt are our own design.

## Tests (`engine/tests/pedals/TideTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- Static curve: slope 1.00 ± 0.03 below the knee and 1/3 ± 0.03 above it; more comp, more reduction.
- Telemetry matches the gain change actually applied (± 0.5 dB) and is ~0 dB for quiet input.
- `blend` 0 is a bit-exact dry passthrough.
- `tone` 0.5 is flat (± 0.2 dB, 50 Hz–15 kHz); the extremes tilt by more than ±4.5 dB.
- Attack within 1–8 ms; program-dependent release as above.
- Automating each knob (and all at once) is click-free.

## Known limitations

- Linked stereo detection only.
- Fast transients overshoot for the ~3–5 ms attack (by design: no lookahead).
- The automatic makeup is calibrated for one reference level; much hotter or quieter sources end
  up correspondingly quieter or louder (which is what a compressor does), use `level` to trim.
