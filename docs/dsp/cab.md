# Cab: cabinet and microphone simulation

Source: `engine/include/sedef/dsp/cab/`, `engine/src/dsp/cab/`. Tests: `engine/tests/cab/` (`[cab]`).

## 1. What it models

The cabinet is half of a guitar amp's sound: the speaker's narrow, peaky band (about
80 Hz–5 kHz), the box's low-end alignment, and the colour of the microphones an engineer puts in
front of it. `CabSim` offers three procedural cabinets, each picked up by two close microphones
(A and B) with their own type, cone position, distance, level, pan and polarity, plus an optional
small-room microphone per channel. Any (mic, cab) slot can instead play a user impulse response.

| Cab | Character | How it is built |
|---|---|---|
| 0 · 1x12 open back | loose, airy, light low end | 2nd-order high-pass at 70 Hz, lightly damped resonance at ~96 Hz, lean 400 Hz, broad 2 kHz presence, break-up peak 3.6 kHz and dip 4.4 kHz, gentle 24 dB/oct top from ~7 kHz. Open-back cancellation: the rear wave is a delayed (0.85 ms), low-passed (220 Hz), inverted copy, so below ~100 Hz the baffle behaves like a dipole and the deep lows thin out. |
| 1 · 2x12 open back | fuller, still open | resonance ~86 Hz, a little 220 Hz body from the second cone, 650 Hz dip, presence 2.4 kHz, break-up peak 3.2 kHz and notch 4 kHz, top from ~6.3 kHz. Rear wave: 1.2 ms, 240 Hz, smaller (a wider baffle cancels less). |
| 2 · 4x12 closed back | tight, thumpy, aggressive mids | closed-box high-pass (Qtc ~1) at 70 Hz plus a tight resonance at 84 Hz, 150 Hz thump, 500 Hz scoop, pronounced cone break-up (peak 2.5 kHz, deep notch 3.3 kHz, peak 4.1 kHz), and a 36 dB/oct roll-off from ~5.2 kHz. A small positive internal reflection (1.6 ms, 900 Hz) adds boxiness. |

The ten microphone voicings are generic families, not models of specific products:

| # | Name | Voicing | Proximity at the grille |
|---|---|---|---|
| 0 | Dyn Classic | cardioid dynamic: bass roll-off (2nd-order HP 115 Hz), presence peak +4.5 dB at 5.3 kHz, top rolled off from ~14 kHz | +8 dB shelf at 160 Hz |
| 1 | Dyn Classic Off-Axis | the same capsule at 45°: presence +1.5 dB, −5 dB shelf from 3.5 kHz | +7 dB |
| 2 | Dyn Bright | supercardioid dynamic: lean 500 Hz, wide +5.5 dB presence at 6.5 kHz, extended top | +7.5 dB |
| 3 | Dyn Broadcast | large-diaphragm dynamic built to limit proximity: flat, extended lows | +3.5 dB |
| 4 | Cond Small | small-diaphragm cardioid condenser: flat with a +2.5 dB lift from 8 kHz | +4.5 dB |
| 5 | Cond Large | large-diaphragm cardioid: +1.5 dB warmth shelf at 220 Hz, broad +3.5 dB presence at 6 kHz | +5.5 dB |
| 6 | Cond Precision | near-omni measurement-style capsule: flat | +1 dB |
| 7 | Cond Tube | valve large-diaphragm: +2 dB at 180 Hz, smooth highs (−2.5 dB shelf from 7 kHz) | +5.5 dB |
| 8 | Ribbon Smooth | figure-of-eight ribbon: rolled off from ~9 kHz, soft top | +9 dB |
| 9 | Ribbon Dark | heavier ribbon voicing: low-pass ~6 kHz, −4 dB from 2.5 kHz | +10 dB |
| 10 | Custom IR | the user IR loaded for this (mic, cab) slot; **Dyn Classic if none is loaded** (or if the loaded kernel was built for another sample rate) | (none) |

## 2. Signal flow

```
           ┌────────────────────────── mic A voice (×2 while crossfading) ──────────────────────────┐
mono in ─┬─► [cab reflection] ► cab body (≤10 SOS) ─┬─► position (3) ► mic (≤4) ► distance (2) ► time of
         │                                          │                                            flight +
         │                                          │                                            floor bounce
         │                                          │                                               │
         │                                          └─ room send ◄─ room level/switch               ▼
         │                                                     │                 level·pan · switch(±1/0)
         ├─► mic B voice ... (same) ...                        ▼                                 │
         │                                       RoomAmbience (shared, stereo) ────────┐         │
         │                                                                             ▼         ▼
         └──────────────────────────────────────────────────────────────────────────►  L / R out
```

A custom IR voice replaces everything from the reflection to the floor bounce with a
zero-latency convolution; its output also feeds that mic's room send.

Processing runs in control blocks of 32 samples (`kControlBlock`). The input block is copied
first, so `in` may alias an output channel.

## 3. Control mapping

**Position** `p` (0 centre … 1 edge), per cabinet:

- dust-cap presence peak: `presenceDb · (1 − p)` (about +4 to +4.5 dB at 4.2–4.8 kHz);
- edge roll-off: a 2nd-order low-pass whose corner glides exponentially from
  `min(18 kHz, 0.45 fs)` at the centre to the cab's edge corner (3.0–3.4 kHz) at the edge;
- edge warmth: `+1.5…2 dB · p` around 300 Hz (the outer cone sounds fuller).

Each term is monotonic in `p`, so 4–8 kHz energy falls steadily from centre to edge.

**Distance** `d` (0 on the grille … 1 far): the capsule sits at `r = 2.5 cm + 32.5 cm · d`.

| Effect | Mapping |
|---|---|
| time of flight | `(r − 2.5 cm) / 343 m/s`: 0 … 0.95 ms. Fractional delay: linear below one sample (exact at 0), 4-point Hermite above |
| floor bounce | image source `2h` below (h = speaker centre height: 0.42 / 0.36 / 0.26 m); path `rb = √(r² + 4h²)`, arrival `(rb − 2.5 cm)/c`, gain `0.45 · r / rb` (−32 dB on the grille, about −10 dB far), low-passed at 1.8 kHz (the floor sees the speaker off-axis) |
| proximity | low shelf at the mic's corner, gain `proximityDb · (0.1 + 0.9 (1 − d)^1.6)`. A perceptual taper: a strict 1/r law would spend almost all of it in the first quarter of the knob |
| air / beaming | high shelf at 7 kHz, `−2.5 dB · d` |
| level | `−4.5 dB · d` |

Because the time of flight is real, blending A and B at different distances comb-filters in the
mono fold like two real microphones would; the polarity switch then becomes useful.

**Level, pan, enable, polarity.** Pan is constant-power, `L = cos θ`, `R = sin θ` with
`θ = (pan + 1) π/4`. Level and pan glide linearly over 20 ms. Enable and polarity drive a
30 ms S-curve (smoothstep) "switch" from −1/0/+1. Unlike a linear ramp it has no slope corners,
so flipping polarity mid-note does not spray treble (−106 dB in the click test, against −56 dB
with a linear ramp).

**Room.** `roomEnabled` drives the room switch, `roomLevelDb` its level. The send follows the
mic's pan at half width. Room level 0 dB carries about as much energy as the close mic.

## 4. Switching and smoothing

- **Voices.** Each mic has two voices. A change of cab, mic type or custom IR starts the idle voice
  on the new configuration and crossfades. The incoming voice first runs muted for a 5 ms
  pre-roll so its filters settle, then the 30 ms smoothstep crossfade runs. A change that arrives
  during a fade waits for it to finish. The amp/cab link in the engine switches cab and mic types
  together, which is a single crossfade.
- **Position / distance** glide with a 30 ms one-pole. The affected sections (presence, edge
  low-pass, warmth, proximity, air) are redesigned once per control block, and their coefficients
  are **interpolated per sample** across the block. A plain 32-sample coefficient staircase
  measured −65 dB of zipper noise above 2.5 kHz on a 110 Hz tone; interpolation brings it below
  −100 dB.
- **Idle mics** (close switch and room switch both off and settled) cost nothing; they restart
  clean, without a crossfade, when switched back on. The room processor keeps ringing after its
  sends close (spill-over) and stops itself `2 · RT60` later.

## 5. Level calibration

All voices are normalised to a common **loudness index**: the mean, in dB, of the power in
sixteen third-octave bands centred from 157 Hz to 5 kHz (`cab::loudnessIndexDb`; a unit impulse
reads 0 dB). In `prepare()` every procedural (cab, mic) voice is evaluated analytically at
position 0.5 and distance 0 and scaled to 0 dB. Moving position or distance away from that point
changes the level naturally, as on a real cab. `makeKernel` scales custom IRs to 0 dB on the
same scale (gain clamped to ±60 dB). So swapping a procedural mic for an IR, or one IR for
another, keeps the level within a dB or two, and an impulse IR plays at exactly unity.

## 6. Custom impulse responses

### Building a kernel (`CabSim::makeKernel`, loader thread)

1. **Resample** `irSampleRate → targetRate` with a Kaiser-windowed sinc (β = 9, 32 zero crossings
   per side, pass band to 95% of the lower Nyquist, ~90 dB stop band). The tabulated kernel is
   evaluated per output sample; amplitudes are scaled by `in/out` so the IR's frequency response
   keeps its gain. Equal rates copy the samples untouched.
2. **Truncate** to one second (`ConvolutionKernel::kMaxSeconds`) with a 10 ms half-cosine fade when
   the IR is longer. Trailing silence below −120 dB re peak is trimmed (CPU), and non-finite
   samples are zeroed.
3. **Normalise** to a loudness index of 0 dB (section 5). `normalisationGain()` reports the gain.
4. **Partition**: the first P taps become the direct-form head; taps from P onwards are cut into
   P-sample partitions, zero-padded to 2P and transformed (pffft, internal order, pre-scaled by
   1/2P).

`maxBlockSize` is not needed: the convolution has no latency at any block size (below).

### Zero-latency convolution (`cab::PartitionedConvolver`)

Gardner-style split of the IR at tap P:

```
y[n] = Σ_{k<P} h[k] x[n−k]            direct form, every sample
     + Σ_{k≥P} h[k] x[n−k]            uniformly partitioned overlap-save (FFT size 2P)
```

The tail starts P taps late, so each block of P tail outputs depends only on input blocks that
were complete when it starts. Once per P input samples the convolver transforms
`[previous block | current block]` into a ring of input spectra (a frequency-domain delay line),
multiply-accumulates it against the tail partitions (`pffft_zconvolve_accumulate`) and
inverse-transforms the next P tail samples. `latencySamples()` is 0 for every host block size,
including 1. The head is a scatter (`acc[q + k] += x · h[k]`), which vectorises without
reassociating a float reduction. Input spectra do not depend on the kernel, and a voice never
changes kernel, so a new voice clears only a few small buffers; spectra are counted as they are
written instead of zeroing the history.

P is chosen from the sample rate: 256 up to 50 kHz, 512 up to 100 kHz, 1024 above, near
`√(3 · taps)` for a one-second IR. Every voice's convolver is sized in `prepare()` for one second
at the session rate.

### Threading

| Call | Thread | What it does |
|---|---|---|
| `makeKernel` | any non-RT | allocates and builds the kernel |
| `setCustomIr (mic, cab, k)` | any non-RT | `RtHandoff::publish` into that slot (2 mics × 3 cabs). `nullptr` publishes an **empty** kernel, meaning "cleared", because `RtHandoff` cannot carry null |
| `process` | audio | once per block, `RtHandoff::acquire` per slot, then crossfades the voices that used the old kernel |
| `collectGarbage` | any non-RT, periodically | `RtHandoff::collect` per slot: frees retired kernels |

The audio thread never frees. A crossfade still needs the *retired* kernel for 35 ms, which
`collect()` alone would free under it. Each slot therefore has a non-blocking ownership flag
(`busy`, an atomic exchanged with acquire/release ordering). The audio thread takes it to acquire,
and keeps it while a voice still plays the retired kernel; `collectGarbage` frees a slot only when
it can take the flag. Neither side ever waits: a failed exchange simply retries on the next block
or the next collection. Kernels built for a different sample rate (or partition size) are ignored,
and the slot falls back to Dyn Classic until the shell rebuilds them for the new rate.

## 7. Room ambience (`cab::RoomAmbience`)

A room-mic pair a couple of metres away in a small (~4 × 5 m) room, shared by both mic channels
(the model is linear, so one shared room equals two identical rooms at half the CPU):

- input band-limited (1st-order HP 90 Hz, 2nd-order LP 4.5 kHz): a distant mic hears the cab
  darker;
- six early reflections per side (4–23 ms, mixed polarity, alternating sources);
- an 8-line feedback delay network (7.9–21.3 ms lines, orthonormal Hadamard mix, per-line
  one-pole damping at 4.5 kHz), RT60 0.42 s at low frequencies and shorter in the treble, fed
  after a 9 ms pre-delay;
- orthogonal output sign patterns for decorrelated L/R;
- early and late energies normalised to −3 dB each at prepare time from a rendered impulse.

## 8. Performance

Apple M2, 48 kHz, 64-sample blocks (`sedef_engine_tests "[.bench]"`). Ranges span a quiet machine
and one shared with parallel builds:

| Configuration | ns/sample | % of one core |
|---|---|---|
| 2 procedural mics + rooms | 100–120 | 0.5–0.6 |
| same, automating position and distance | 110–135 | 0.55–0.65 |
| one 1 s custom IR | 120–150 | 0.6–0.7 |
| two 1 s custom IRs + rooms | 250–320 | 1.2–1.55 |
| custom IR + procedural + rooms, 1-sample host blocks | ~325 | ~1.6 |

Budgets: procedural < 2%, one 1 s IR < 3%. `cab::runStages` runs four biquad sections per pass,
so their latency-bound recursions overlap; that makes the procedural chain about 30% faster than
section-by-section loops (A/B under the same load: 165 → 115 ns/sample). `makeKernel` for a
1 s IR at 44.1 → 48 kHz takes about 8 ms off the audio thread; `prepare()` takes 4–13 ms (it
calibrates all 30 procedural voices).

## 9. References

- R. Bristow-Johnson, *Cookbook formulae for audio EQ biquad filter coefficients* (public note):
  all peaking, shelving, low- and high-pass sections, via `core/Biquad`.
- R. H. Small, "Closed-Box Loudspeaker Systems, Part I: Analysis", *JAES* 20(10), 1972: the
  closed box as a second-order high-pass with total Q (the 4x12's low end).
- L. L. Beranek and T. J. Mellow, *Acoustics: Sound Fields and Transducers*, Academic Press, 2012:
  open-baffle dipole cancellation, cone break-up, pressure-gradient microphones and proximity effect.
- J. Eargle, *The Microphone Book*, 2nd ed., Focal Press, 2004: polar-pattern families,
  proximity effect, typical response shapes of dynamic, condenser and ribbon microphones.
- W. G. Gardner, "Efficient Convolution without Input-Output Delay", *JAES* 43(3), 1995: the
  zero-latency head/tail split.
- A. Torger and A. Farina, "Real-time partitioned convolution for Ambiophonics surround sound",
  IEEE WASPAA 2001; F. Wefers, *Partitioned Convolution Algorithms for Real-Time Auralization*,
  PhD thesis, RWTH Aachen, 2015: uniformly partitioned overlap-save with a frequency-domain delay
  line.
- J. O. Smith III, *Digital Audio Resampling* (online notes, CCRMA) and J. F. Kaiser,
  "Nonrecursive digital filter design using the I0-sinh window function", IEEE ISCAS 1974: the
  windowed-sinc resampler.
- J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators",
  AES 90th Convention, 1991: the FDN room.
- M. R. Schroeder, "New method of measuring reverberation time", *JASA* 37, 1965: backward
  integration used by the RT test.
- pffft (J. Pommier, FFTPACK-derived, BSD-style licence), already linked by the engine: FFTs
  and spectral multiply-accumulate.

All voicing values are our own targets, informed by the general behaviour in these texts. No
third-party impulse responses, measurements or product data are used (ADR-0006).

## 10. Tests (`[cab]`)

| Test | Asserts |
|---|---|
| finite output | random settings changed every few ms (incl. custom IR slots, one for the wrong rate), full-scale noise, 44.1/48/96 kHz × blocks 1, 17, 64, 512: finite and bounded |
| empty / mono / in-place | 0-frame blocks; one output channel gets the constant-power fold; `in` aliasing an output gives identical results |
| plausible responses (all three rates) | 4x12 ≥ 12 dB down at 10 kHz vs 1 kHz for every mic type at the brightest position (≥ 25 dB for Dyn Classic); resonance peak in 88–110 / 78–100 / 72–95 Hz and a real peak; open back has ≥ 6 dB less 40–70 Hz than closed; 4–8 kHz energy strictly falls from centre to edge (3 cabs × 3 mics); distance removes > 3 dB of < 150 Hz proximity for the dynamic, more for the ribbon, less for the omni, is monotonic, and drops the mid level by 1–9 dB; voicings order (off-axis and ribbons darker, Dyn Bright brighter) |
| time of flight | cross-correlation of distance 1 vs 0: 0.8–1.1 ms later |
| mono fold | A at distance 0 plus B at 0.6 shows a notch ≥ 10 dB below the coherent sum in 300 Hz–3 kHz; B inverted with identical settings cancels to < −120 dB |
| pan | hard left is silent on the right; centre is −3 dB per side |
| room | RT (T20) between 0.25 and 0.6 s; nothing in the first 2 ms; tail rings after switch-off, decays, then the output is exactly zero |
| click-free automation | 6 s of a 110 Hz tone while every control moves (position, distance, level, pan, room level sweeps; cab, both mic types incl. custom IR, polarity, enables, rooms switching). Energy above 2.5 kHz (8th-order high-pass) stays < −80 dB re output RMS (measured ≈ −100 dB) |
| convolution vs direct | random 3000-tap IR, 44.1/48/96 kHz, blocks {1}, {333}, mixed: max error < 1e-5 re peak; also end-to-end through `CabSim` with random block sizes incl. 1 and 333 |
| zero latency | impulse IR: output equals input at every block size; delayed impulses at P−1, P, P+1, 3P+7 delay exactly |
| resampling | 1 kHz tone burst 44.1→48, 48→44.1, 96→48 kHz: frequency within 1 Hz, response at 1 kHz within 0.05 dB, length scaled |
| normalisation | scaled impulses get exactly the inverse gain; arbitrary IRs at any level land on 0 ± 0.05 dB; every procedural voice measures 0 ± 0.3 dB |
| truncation | 2.5 s IR → ≤ 1 s with the fade-out, untouched before it; same through the resampler |
| fallback | wrong-rate kernel renders exactly like Dyn Classic; clearing an IR crossfades to Dyn Classic and then matches it |
| concurrency | an audio thread processes while another thread builds, publishes, clears and collects kernels for 0.6 s: no crash, finite output, `ConvolutionKernel::destroyedOnThisThread()` is 0 on the audio thread, and every kernel is freed at the end |

Hidden helpers: `[.cabdump]` prints measured response tables for voicing work; `[.bench]` prints
the numbers in section 8.

## 11. Known limitations / future work

- Position and distance do not affect custom IRs: the IR already contains its mic placement.
- Only the mic type is per cab (as in the parameter schema); position and distance are per mic.
- The procedural models are linear. Speaker cone compression and break-up distortion at high
  levels, and voice-coil heating, are not modelled.
- Off-axis angle is a separate voicing (type 1) rather than a continuous control.
- The room is a single fixed small room; size and decay are not exposed.
- Kernels must be rebuilt by the shell after a sample-rate change. Until then the slot plays Dyn
  Classic.
- The FFT work runs once per P samples rather than being spread over the block; at P = 256 the
  burst is a few microseconds.
