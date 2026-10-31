# Fathom: plate and hall reverb

Source: `engine/src/dsp/reverb/` (`Fathom.cpp`, `PlateTank.*`, `HallFdn.*`, `ReverbBlocks.h`).
Interface: `engine/include/sedef/dsp/Fathom.h`. Tests: `engine/tests/reverb/FathomTests.cpp`.

## 1. What it models

Two reverbs, chosen by `mode`:

- **Plate.** A bright, dense, instantly diffuse plate. It uses the figure-eight tank from
  Dattorro's 1997 paper: delays, coefficients, modulation and output taps follow the published
  design, with every length scaled from 29761 Hz to the session rate.
- **Hall.** A large, smooth room. Sparse early reflections lead into an 8-line feedback delay
  network (FDN) with frequency-dependent decay, so the top end dies before the body.

Both run from 1 s to 60 s RT60 and can be switched off mid-tail with spillover, so the tail
rings out.

## 2. Signal flow

```
L,R ─► input fade (spillover, 20 ms) ─► pre-delay (1–200 ms) ─┬─► Plate tank ─┐
                                                              └─► Hall FDN ───┴─► mode crossfade
     ─► low cut (HP, 12 dB/oct) ─► high cut (LP, 12 dB/oct) ─► wet

out = dry · gDry + wet · gWet
```

**Plate (Dattorro, Fig. 1).** The two inputs are summed to mono, as in the paper.

```
in ─► bandwidth LP ─► AP 142 ─► AP 107 ─► AP 379 ─► AP 277 ─┬────────────────┐
                                                            ▼                ▼
        ┌─ × decay ◄─ D 3720 ◄─ AP 1800 ◄─ × decay ◄─ damp ◄─ D 4453 ◄─ AP~ 672 ◄─ (+) ◄─┐
        │                                                                                │
        └─► (+) ─► AP~ 908 ─► D 4217 ─► damp ─► × decay ─► AP 2656 ─► D 3163 ─► × decay ─┘
```

- Lengths are in samples at 29761 Hz.
- `AP~` marks the decay-diffusion-1 all-passes (coefficient −0.70). Their delays are modulated
  by ±10 reference samples (≈ 0.34 ms). The modulators are 0.9 Hz quadrature sines mixed 70/30
  with a slow random wander.
- Input diffusion is 0.75 / 0.75 / 0.625 / 0.625.
- Decay diffusion 2 is `clamp (decay + 0.15, 0.25, 0.5)`.
- Each output is the paper's seven signed taps (Table 2) times 0.6.

**Hall.**

```
in L ─► 4 all-passes (3.1–11.2 ms) ─┐         ┌─ early reflections: 10 taps per side, 4–81 ms ─┐
in R ─► 4 all-passes (3.4–12.3 ms) ─┤         │                                                │
                                    ▼         │                                                ▼
              inject (Hadamard rows 2 / 6) ─► 8 delay lines, 31.7–101.9 ms (primes), modulated
                                    ▲          │
                                    │   per-line shelf absorption g_i(ω)
                                    └── 8×8 Hadamard (orthonormal) ◄─┘
                                       taps out: Hadamard rows 3 (L) / 5 (R) ──► + early ─► wet
```

- Line lengths are spread roughly geometrically and rounded up to distinct primes at the
  session rate, so they are pairwise coprime.
- The feedback matrix is a fast Walsh-Hadamard transform scaled by 1/√8: 24 add/subtracts and
  8 multiplies, energy-preserving.
- Left and right enter and leave along different, mutually orthogonal Hadamard rows. A mono
  source therefore comes out decorrelated (measured |ρ| < 0.01).
- Each line's delay wanders by ±0.18 ms. The motion is a cosine-interpolated random walk at
  0.29–0.59 Hz, a different rate per line. This breaks up the metallic ringing of fixed modes
  without audible chorus. The lines are read with Catmull-Rom interpolation, which has
  |H| ≤ 1, so modulation never adds loop gain.

## 3. Control mapping

| Setting | Range | Mapping / smoothing |
|---|---|---|
| `enabled` | bool | Input fade 0↔1 over 20 ms. While disabled, the dry gain returns to 1 (see Spillover). |
| `mode` | Plate / Hall | 60 ms equal-power crossfade. An engine whose fade-out has finished is cleared, so its old tail never returns. |
| `mix` | 0..1 | Equal-power law: `gDry = cos(mix·π/2)`, `gWet = sin(mix·π/2)`. Both ramp linearly over 30 ms. Mix 0 is the exact dry signal; mix 1 is wet only. |
| `decaySeconds` | 1..60 s | RT60 at low and mid frequencies. Glides with a 60 ms time constant; loop gains are recomputed every 32 samples while gliding. |
| `lowCutHz` | 20..500 Hz | 2nd-order Butterworth high-pass on the wet signal. |
| `highCutHz` | 800..5000 Hz | 2nd-order Butterworth low-pass on the wet signal. It also voices the tanks: plate damping corner = 2.5 × highCut, plate input bandwidth = 4 × highCut, hall shelf corner = highCut. |
| `preDelayMs` | 1..200 ms | Rounded to whole samples at rest; glides with a 50 ms time constant (a short, smooth Doppler instead of a click). |

Out-of-range or non-finite settings are clamped, or fall back to the defaults.

## 4. Decay and stability

`decaySeconds` is the RT60, the time to fall 60 dB. A loop of length *L* seconds that must lose
60 dB in *T* seconds needs a gain of `10^(−3 L / T)` per pass.

- **Plate.** One trip around the figure eight passes all eight delays, including the all-passes:
  a Schroeder all-pass delays a flat spectrum by its length on average. The trip also passes
  four `decay` multipliers, so `decay = (10^(−3 L_loop / T))^(1/4)`. Here *L_loop* is 21589
  reference samples, about 0.725 s.
- **Hall.** Following Jot & Chaigne, line *i* gets a gain `g_i = 10^(−3 L_i / T)`. With an
  orthogonal feedback matrix, every mode of the lossless prototype then decays at the same
  rate.
  - The gain is a first-order shelf: `g_i` at DC, falling to `10^(−3 L_i / (0.5 T))` above the
    corner. High frequencies therefore decay twice as fast, with the corner at highCut.
  - The shelf is the bilinear first-order design, scaled by the DC gain. Its magnitude is
    monotonic between the DC and Nyquist gains, so |H| ≤ g_DC < 1.

**Never blowing up.** The longest decay is 60 s. At that setting the largest per-pass gain is
0.9964 (hall, 31.7 ms line) and the plate's `decay` is 0.979. Several things keep the loop
gain strictly below 1:

- Every in-loop filter is passive: the one-pole damping, the all-passes and the Catmull-Rom
  interpolation all have |H| ≤ 1.
- 1/√8 is rounded down to float, so the Hadamard matrix has norm ≤ 1.
- A modulated all-pass is a Möbius map of the unit disk, so it stays passive while its delay
  moves.

Values written into feedback paths pass through `flushToZero`. Filter states are flushed once
per 32-sample control block. Denormals cannot build up even without the engine's
`ScopedNoDenormals`.

## 5. Spillover

- `enabled = false` fades new input to zero over 20 ms. The tanks keep running and the dry gain
  returns to unity, so the tail rings out over the untouched dry signal.
- Fathom tracks the peak of the tank input (after pre-delay) and of the tank output. Once both
  stay below −100 dBFS for 250 ms (longer than the maximum pre-delay), it goes **idle**:
  `process` returns immediately and the buffer is a bit-exact dry passthrough.
- The frozen state is below −100 dBFS, so it is not cleared. That avoids a large memset on the
  audio thread.
- Re-enabling leaves idle, fades the input back in over 20 ms and ramps the dry gain back to
  the mix law.

## 6. Measurements (48 kHz, Apple M2)

RT60 is from a T30 Schroeder fit on the 350–1400 Hz band (the 500 Hz and 1 kHz octaves) of the
impulse response. Run `sedef_engine_tests "[reverb-bench]"` to reproduce.

| Mode | set 1 s | set 2 s | set 5 s | set 10 s |
|---|---|---|---|---|
| Plate | 1.10 s (+9.6 %) | 2.15 s (+7.6 %) | 5.21 s (+4.3 %) | 10.29 s (+2.9 %) |
| Hall | 0.94 s (−6.1 %) | 1.93 s (−3.7 %) | 4.83 s (−3.4 %) | 9.53 s (−4.7 %) |

- **Wet level.** For steady mono noise with a 2.5 s decay, the wet output (mix 1) sits about
  4 dB below the input in both modes. It rises with decay time, as in a real room.
- **Stereo correlation** of the wet output for a mono source: |ρ| < 0.01 in both modes.
- **CPU** (64-sample blocks, stereo): plate 35 ns/frame (0.17 % of a core); hall 79 ns/frame
  (0.38 %).

## 7. References

- J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", *J. Audio Eng. Soc.*
  45(9), 1997. Plate topology, lengths, coefficients, output taps.
- J.-M. Jot and A. Chaigne, "Digital Delay Networks for Designing Artificial Reverberators",
  90th AES Convention, preprint 3030, 1991. FDN with per-line absorption filters for
  frequency-dependent decay.
- M. R. Schroeder, "Natural Sounding Artificial Reverberation", *J. Audio Eng. Soc.* 10(3),
  1962. All-pass diffusers.
- M. R. Schroeder, "New Method of Measuring Reverberation Time", *J. Acoust. Soc. Am.* 37,
  1965. Backward integration, used by the RT60 test.
- R. Bristow-Johnson, "Audio EQ Cookbook". Cut filters, via `core/Biquad`.

## 8. Tests (`[reverb][fathom]`)

- Finite, bounded output for random settings (changed every 50 ms) with full-scale noise, at
  44.1 / 48 / 96 kHz and block sizes 1, 17, 64, 512.
- Measured RT60 within ±20 % of 2 s and 5 s, plate and hall.
- 60 s decay over 120 s of full-scale noise:
  - stays finite and bounded, with no growth in the second minute;
  - ten seconds after the input stops, the tail has lost only about 10 dB.
- Mix 0 is a bit-exact dry passthrough.
- Spillover:
  - the tail continues after disabling, then the output becomes exactly the input;
  - input arriving after the disable does not enter the tank.
- Wet left/right correlation < 0.5 for a mono source.
- High cut: with highCut at 5 kHz, the wet energy above 5 kHz falls to under half the input's
  share. Moving highCut from 5 kHz to 800 Hz removes a further 20+ dB (measured about 39 dB).
- Click detector (two cascaded 8 kHz high-passes) stays below 0.005 on a 220 Hz sine, while
  every parameter sweeps, the mode toggles every 0.5 s and enable toggles.
- Mono buffers and `reset()`.

## 9. Known limitations / future work

- The plate is mono-in, as in the paper. A stereo source's image is not carried into the plate.
- The hall decay has two bands (low/mid and high). A low-frequency ratio (bass decaying longer)
  and a size control would make it more flexible.
- The wet level grows with decay time, as in a physical space. Long decays with sustained
  playing can get loud, so lower the mix.
