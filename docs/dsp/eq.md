# EQ (GraphicEq)

`engine/include/sedef/dsp/GraphicEq.h`, `engine/src/dsp/GraphicEq.cpp`

## What it models

A nine-band graphic EQ with low and high cuts, placed after the cab. The amp
section owns three banks (one per amp); the engine hands the active bank to one
`GraphicEq`, so switching amps glides from one bank's curve to the next.

The curve is plain textbook filter design with no analog-style quirks. That keeps
the UI curve exact: `ui/src/features/eq/response.ts` computes the same
magnitude in TypeScript.

## Signal flow

```
in ─► low cut (HPF, 12 dB/oct) ─► 9 × peaking band ─► high cut (LPF, 12 dB/oct) ─► out
```

Each channel has its own filter state. Mono and stereo are both supported.

## Design (keep in sync with the UI)

| Section | Design | Parameters |
|---|---|---|
| Band *i* | RBJ peaking (`design::peaking`) | f = 65, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz; Q = `kBandQ` = 1.41 (one octave); gain −12..+12 dB |
| Low cut | RBJ high-pass, Q = 1/√2 (2nd-order Butterworth) | 20..500 Hz; **bypassed at ≤ 20 Hz** |
| High cut | RBJ low-pass, Q = 1/√2 | 1k..20k Hz; **bypassed at ≥ 20 kHz** |

The UI mirror must apply the same rules:

1. **Clamp the inputs** to the parameter ranges above: gain to ±12 dB, low cut
   to 20..500 Hz, high cut to 1k..20 kHz. For valid parameter values this does
   nothing.
2. **Clamp every design frequency to `0.45 · fs`.** At 32 kHz the 16 kHz band
   sits on Nyquist, so its design centre becomes 14.4 kHz. Cuts are clamped the
   same way. The bilinear transform cramps responses near Nyquist anyway; the
   clamp keeps the designs well defined.
3. A band at exactly 0 dB contributes exactly 0 dB. A cut at its extreme
   contributes nothing.
4. `responseDb = Σ 20·log10 |H_section(e^{jω})|` over the active sections,
   with each section's magnitude floored at 1e-12.

`GraphicEq::responseDb` is a pure static function and safe on any thread.

## Smoothing and automation

- Band gains glide linearly in dB over 50 ms. Cut frequencies glide linearly
  in log2(Hz) over 50 ms, so sweeps move evenly through the octaves.
- While anything glides, coefficients are recomputed every `kControlBlock` (32)
  samples. Inside each control block they are **linearly interpolated per
  sample**. Stepping a direct-form biquad every 32 samples left measurable
  output steps: a second difference of 0.02–0.05 on a 110 Hz test tone, worst
  for a low-pass gliding near Nyquist. Interpolation brings that down to the
  tone's own curvature (about 1e-3). Linear interpolation between two stable
  coefficient sets stays stable because the biquad stability triangle is
  convex.
- **Cuts fade instead of switching.** Switching even a 20 Hz high-pass in or
  out at once shifts low-frequency phase by tens of degrees, and you hear a
  thump. Turning a cut on starts it at its bypass extreme, fades it in against
  the dry signal over 20 ms, then glides to the target. Turning it off glides to
  the extreme first, then fades it out. Once the fade completes the section
  leaves the signal path.
- A band that reaches 0 dB is the identity filter, and its state settles to
  exactly zero, so it leaves the path. Flat settings are bit-exact
  transparent.
- The first `setSettings` after `prepare`/`reset` snaps instead of gliding.

## Implementation notes

Sections are designed with `design::*`. They run on a small TDF-II kernel
inside the module that keeps state in registers for the whole block and runs
the two channels as independent chains. With all 11 sections active this
measured about 2.5× faster than calling `Biquad::processSample` per sample.

## References

- R. Bristow-Johnson, *Cookbook formulae for audio EQ biquad filter
  coefficients* (the "Audio EQ Cookbook"). Peaking, high-pass and low-pass
  designs.
- J. O. Smith III, *Introduction to Digital Filters with Audio Applications*,
  W3K, 2007. Transposed direct form II, bilinear-transform frequency warping,
  and the stability triangle of second-order sections.

## Tests (`engine/tests/timefx/GraphicEqTests.cpp`, tag `[timefx][eq]`)

- Output stays finite with random settings and full-scale noise at 44.1, 48 and
  96 kHz, in blocks of 1, 17, 64 and 512 samples.
- Gain measured at every band centre, with random settings, matches
  `responseDb` within 0.1 dB at all three rates.
- Flat settings: output is bit-exact equal to input, ±0.01 dB at test tones,
  and `responseDb` is exactly 0.
- Cuts are bypassed at 20 Hz / 20 kHz. Just inside the range they are −3 dB at
  the corner.
- `responseDb` equals the sum of the `design::magnitude` values of the
  individual sections.
- 32 kHz: the 16 kHz band and a high cut above Nyquist stay finite, and
  measured gain matches the clamped design.
- Mono and stereo give the same per-channel output.
- Worst-case automation (every band jumping ±12 dB, cuts flipping in and out
  every 100 ms) keeps the second difference of a 110 Hz tone under 2e-3.
- After a glide, the measured response equals the target.
- Silence in gives silence out.
- `[.bench][timefx-bench]` prints ns/frame.

## Limitations / future work

- Bands are minimum-phase. A linear-phase option is out of scope.
- Constant Q, so band interaction is plain summation in dB: a +12 dB boost on
  two adjacent bands overlaps to about +15 dB between them. The UI curve shows
  this because it uses the same math.
