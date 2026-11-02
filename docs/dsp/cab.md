# Cab: cabinet and microphone simulation

Source: `engine/include/sedef/dsp/cab/`, `engine/src/dsp/cab/`. Tests: `engine/tests/cab/` (`[cab]`).

## 1. What it models

The cabinet is half of a guitar amp's sound: the speaker's narrow, peaky band (about
80 Hz–5 kHz), the box's low-end alignment, and the colour of the microphones an engineer puts in
front of it. `CabSim` offers three procedural cabinets, each picked up by two close microphones
(A and B). Each mic has its own type, level, pan and polarity, and a free placement: anywhere over
the face of the miked speaker (radial position and azimuth), any distance from the grille, and
turned up to 60° off axis. On the 2x12 and 4x12 the mic also hears the other speakers, so where it
sits over the baffle matters. An optional small-room microphone per channel adds the room. Any
(mic, cab) slot can instead play a user impulse response.

| Cab | Character | How it is built |
|---|---|---|
| 0 · 1x12 open back | loose, airy, light low end | 2nd-order high-pass at 70 Hz, lightly damped resonance at ~96 Hz, lean 400 Hz, broad 2 kHz presence, break-up peak 3.6 kHz and dip 4.4 kHz, gentle 24 dB/oct top from ~7 kHz. Open-back cancellation: the rear wave is a delayed (0.85 ms), low-passed (220 Hz), inverted copy, so below ~100 Hz the baffle behaves like a dipole and the deep lows thin out. |
| 1 · 2x12 open back | fuller, still open | resonance ~86 Hz, a little 220 Hz body from the second cone, 650 Hz dip, presence 2.4 kHz, break-up peak 3.2 kHz and notch 4 kHz, top from ~6.3 kHz. Rear wave: 1.2 ms, 240 Hz, smaller (a wider baffle cancels less). |
| 2 · 4x12 closed back | tight, thumpy, aggressive mids | closed-box high-pass (Qtc ~1) at 70 Hz plus a tight resonance at 84 Hz, 150 Hz thump, 500 Hz scoop, pronounced cone break-up (peak 2.5 kHz, deep notch 3.3 kHz, peak 4.1 kHz), and a 36 dB/oct roll-off from ~5.2 kHz. A small positive internal reflection (1.6 ms, 900 Hz) adds boxiness. |

The body filters are one speaker in its box. The miked speaker is the left one of the 2x12 and the
bottom-left one of the 4x12 (the UI's cab glyphs outline it); the others sit 0.34 m to its right
(2x12), or 0.33 m right, 0.32 m up and diagonally (4x12), and section 3 adds what the mic hears of
them. Speaker centres stand 0.42 / 0.36 / 0.26 m above the floor.

The ten microphone voicings are generic families, not models of specific products. Each also has
a polar pattern `A + (1 − A) cos θ` and an off-axis model (section 3): the on-axis presence peak
that fades and a treble shelf that grows, both given at 60°.

| # | Name | Voicing | Proximity at the grille | Pattern A | At 60° off axis |
|---|---|---|---|---|---|
| 0 | Dyn Classic | cardioid dynamic: bass roll-off (2nd-order HP 115 Hz), presence peak +4.5 dB at 5.3 kHz, top rolled off from ~14 kHz | +8 dB shelf at 160 Hz | 0.5 | presence gone, −7.5 dB shelf from 3.5 kHz |
| 1 | Dyn Classic Off-Axis | the same capsule at 45°: presence +1.5 dB, −5 dB shelf from 3.5 kHz | +7 dB | 0.5 | its 1.5 dB presence gone, −5 dB more from 3.5 kHz |
| 2 | Dyn Bright | supercardioid dynamic: lean 500 Hz, wide +5.5 dB presence at 6.5 kHz, extended top | +7.5 dB | 0.37 | presence gone, −7 dB from 4 kHz |
| 3 | Dyn Broadcast | large-diaphragm dynamic built to limit proximity: flat, extended lows | +3.5 dB | 0.5 | −8 dB from 2.5 kHz (a large diaphragm beams early) |
| 4 | Cond Small | small-diaphragm cardioid condenser: flat with a +2.5 dB lift from 8 kHz | +4.5 dB | 0.5 | −3 dB from 8 kHz (a small capsule keeps its pattern highest) |
| 5 | Cond Large | large-diaphragm cardioid: +1.5 dB warmth shelf at 220 Hz, broad +3.5 dB presence at 6 kHz | +5.5 dB | 0.5 | presence gone, −6 dB from 4.5 kHz |
| 6 | Cond Precision | near-omni measurement-style capsule: flat | +1 dB | 0.9 | −4 dB from 10 kHz |
| 7 | Cond Tube | valve large-diaphragm: +2 dB at 180 Hz, smooth highs (−2.5 dB shelf from 7 kHz) | +5.5 dB | 0.5 | −6 dB from 4.5 kHz |
| 8 | Ribbon Smooth | figure-of-eight ribbon: rolled off from ~9 kHz, soft top | +9 dB | 0 | −3 dB from 5 kHz |
| 9 | Ribbon Dark | heavier ribbon voicing: low-pass ~6 kHz, −4 dB from 2.5 kHz | +10 dB | 0 | −3 dB from 4 kHz |
| 10 | Custom IR | the user IR loaded for this (mic, cab) slot; **Dyn Classic if none is loaded** (or if the loaded kernel was built for another sample rate) | (none) | (Dyn Classic's) | (Dyn Classic's, after the IR) |

## 2. Signal flow

```
            ┌──────────────────────────── mic A voice (×2 while crossfading) ────────────────────────────┐
mono in ─┬─► [cab reflection] ► cab body (≤10 SOS) ─┬─► near: position (3), proximity, air ─► (+) ─► mic (≤4) ► off axis (2)
         │                                          │                                         ▲          │
         │                                          ├─► other speakers: delay ► Bessel LP ► gain ┘ (×1 / ×3)
         │                                          │                                                    ▼
         │                                          └─ room send ◄─ room level/switch      time of flight + floor bounce
         │                                                     │                                    │
         ├─► mic B voice ... (same) ...                        ▼                   level·polar·pan · switch(±1/0)
         │                                       RoomAmbience (shared, stereo) ────────┐           │
         │                                                                             ▼           ▼
         └──────────────────────────────────────────────────────────────────────────►  L / R out
```

A custom IR voice replaces everything from the reflection to the floor bounce with a
zero-latency convolution, followed by the off-axis stages and polar gain while the mic is turned;
the convolution output (before the off-axis stages) also feeds that mic's room send.

Processing runs in control blocks of 32 samples (`kControlBlock`). The input block is copied
first, so `in` may alias an output channel.

## 3. Control mapping

A mic's **pose** is where the capsule sits in front of the miked speaker and how it is aimed
(`cab::MicPose`): `x, y` over the cone in cone radii (front view, x to the right, y up), the
distance `d`, and the off-axis angle `α`. The parameters give it in polar form:

| Parameter | Range | Meaning |
|---|---|---|
| `micXPosition` | 0 … 1 | radius `p = hypot (x, y)`: 0 dust cap, 1 cone edge (unchanged from v0.2.0) |
| `micXAzimuth` (new) | −180 … 180° | where round the cone, clockwise from 12 o'clock as you face the cab: `x = p sin β`, `y = p cos β`. Defaults 60° (A, 2 o'clock) and 120° (B, 4 o'clock) put the markers where v0.2.0 drew them |
| `micXDistance` | 0 … 1 | capsule `z = 2.5 cm + 32.5 cm · d` in front of the cone |
| `micXOffAxis` (new) | 0 … 60° | capsule tilt; the capsule turns towards the dust cap, so the body leans outwards |

In metres the capsule sits at `(mx, my) = 0.12 m · (x, y)` from the speaker centre (0.12 m is the
effective piston radius `a` of a 12" cone, where position 1 sits).

**Position** `p`, per cabinet:

- dust-cap presence peak: `presenceDb · (1 − p)` (about +4 to +4.5 dB at 4.2–4.8 kHz);
- edge roll-off: a 2nd-order low-pass whose corner glides exponentially from
  `min(18 kHz, 0.45 fs)` at the centre to the cab's edge corner (3.0–3.4 kHz) at the edge;
- edge warmth: `+1.5…2 dB · p` around 300 Hz (the outer cone sounds fuller).

Each term is monotonic in `p`, so 4–8 kHz energy falls steadily from centre to edge. The cone is
round, so these depend on the radius only. They stay the same at every distance: on the grille
they stand for which part of the cone the capsule hears, further back for the cone's off-axis
beaming, and the two are about the same size over this range (at 35 cm, position 0.5 is 9.7° off
the speaker's axis, where a 0.12 m rigid piston is 2.5 dB down at 4 kHz; the position terms take
3.2 dB off there).

**Distance** `d`:

| Effect | Mapping |
|---|---|
| time of flight | `(z − 2.5 cm) / 343 m/s`: 0 … 0.95 ms. Fractional delay: linear below one sample (exact at 0), 4-point Hermite above |
| floor bounce | image source as far below the floor as the speaker (centre height h) is above it; the reflected path climbs `2h + my`, so `rb = √(z² + (2h + my)²)`, arrival `(rb − 2.5 cm)/c`, gain `0.45 · z / rb` (−32 dB on the grille, about −10 dB far), low-passed at 1.8 kHz (the floor sees the speaker off-axis). A mic high on the cone hears a later bounce than one low on it |
| proximity | low shelf at the mic's corner, gain `proximityDb · (0.1 + 0.9 (1 − d)^1.6)` on axis. A perceptual taper: a strict 1/r law would spend almost all of it in the first quarter of the knob |
| air / beaming | high shelf at 7 kHz, `−2.5 dB · d` |
| level | `−4.5 dB · d` |

Because the time of flight is real, blending A and B at different distances comb-filters in the
mono fold like two real microphones would; the polarity switch then becomes useful.

**The other speakers** (2x12: one, 4x12: three). All speakers play the same signal, so each
neighbour k reaches the capsule as a delayed, filtered copy of the body output, added to the
miked cone's sound before the mic's own filters. Each is a baffled piston of radius `a` seen from
the capsule at horizontal offset `ρk` and distance `Rk = √(ρk² + z²)`:

- **level**, at low frequencies (`ka ≪ 1`): a piston's far-field pressure is `k a² / (2R)`, while on
  the miked cone's axis the near-field pressure is `k (√(z² + a²) − z)`. The ratio
  `a² / (2 Rk (√(z² + a²) − z))` is −13 dB for the nearest neighbour on the grille and −2.5 dB at
  35 cm, where the cab sums like one source;
- **beaming**: the neighbour is seen `φk = asin (ρk / Rk)` off its own axis, and a piston's
  directivity `2 J1(x) / x` with `x = k a sin φ` is 3 dB down at `x = 1.62`, so a low-pass with
  that corner (740 Hz seen from the grille, 1–1.5 kHz from further back). `2 J1(x)/x` is real, so
  the filter is a second-order Bessel (Q = 1/√3, maximally flat group delay) whose delay,
  `0.217 / f−3dB`, is taken off the path delay;
- **arrival**: `(Rk − z) / c` after the miked cone's sound (minus the filter's delay, at least
  25 µs);
- **polar pattern**: the capsule weighs the arrival direction with `A + (1 − A) cos ψk`, relative
  to its weighting of the miked cone. On the grille the neighbours arrive from about 85°, so a
  figure-of-eight barely hears them and an omni hears them most. The capsule axis leans towards
  the dust cap by `α` (directly over the dust cap, inside 3 cm, the lean fades out, since "towards
  the dust cap" has no direction there).

| 4x12, Dyn Classic, position 0.5 at 2 o'clock | gains (right, upper, diagonal cone) | delays after the filter's | corners |
|---|---|---|---|
| on the grille | −16.9, −17.4, −20.2 dB | 0.45, 0.50, 0.81 ms | 740 Hz |
| distance 0.5 | −6.5, −6.9, −9.7 dB | 0.19, 0.22, 0.48 ms | 810–890 Hz |
| distance 1 | −2.9, −3.2, −5.0 dB | 0.10, 0.12, 0.31 ms | 980–1180 Hz |

So moving a mic over a 2x12 or 4x12 changes the other speakers' contribution and arrival times,
little on the grille and a lot further back. Their low end adds up in phase while their treble
beams away, and in between they comb-filter with the miked cone (section 3a). The bleed gets no
proximity boost of its own: it arrives from the side, where a directional capsule's pressure
gradient is small.

**Off-axis angle** `α`. The direct sound arrives `α` off the capsule's axis:

- **polar gain** `A + (1 − A) cos α` on the whole mic: cardioid −1.4 dB at 45°, −2.5 dB at 60°;
  figure-of-eight −6 dB at 60°; near-omni −0.4 dB;
- **proximity**: only the pressure-gradient part of a directional mic has proximity effect, and it
  follows `cos α` while the pressure part does not. With on-axis proximity gain G, the low end goes
  from G to `(A + cos α (G − A)) / (A + (1 − A) cos α)`: Dyn Classic on the grille loses 1 dB of its
  8 dB at 45° and 2 dB at 60°; a figure-of-eight keeps all of it;
- **treble**: per mic family (section 1), the presence peak fades by `presenceDb · s(α)` and a
  high shelf reaches its 60° value times `s(α)`, with `s(α) = sin²α / sin²60°`. The `sin²` growth
  is the first-order loss of a receiving piston (`2 J1(x)/x ≈ 1 − x²/8`, `x ∝ sin α`).

Dyn Classic at 45° reproduces the shape of the v0.2.0 Dyn Classic Off-Axis voicing, which was
defined as that capsule at 45°, within ±0.3 dB from 30 Hz to 10 kHz; it plays about 2.7 dB quieter,
as a real capsule turned away does (that voicing is calibrated to 0 dB). The Off-Axis voicing stays
for compatibility; turning it further fades the rest of its presence.

**Level, pan, enable, polarity.** Pan is constant-power, `L = cos θ`, `R = sin θ` with
`θ = (pan + 1) π/4`. Level and pan glide linearly over 20 ms. Enable and polarity drive a
30 ms S-curve (smoothstep) "switch" from −1/0/+1. Unlike a linear ramp it has no slope corners,
so flipping polarity mid-note does not spray treble (−106 dB in the click test, against −56 dB
with a linear ramp).

**Room.** `roomEnabled` drives the room switch, `roomLevelDb` its level. The send follows the
mic's pan at half width. Room level 0 dB carries about as much energy as the close mic. The room
mic hears the body, not the close capsule: placement and angle do not change it.

### 3a. What the pose does, measured

Third-octave bands 100 Hz–3 kHz, mic at the cone edge, Dyn Classic (`[cab][placement]` tests and
the analytic model):

| | towards the other cones (1:30) vs away (7:30) | 1x12, top vs bottom of the cone |
|---|---|---|
| on the grille | up to 2.9 dB (4x12), 1.1 dB (2x12) | < 0.2 dB |
| distance 1 | up to 16.6 dB (4x12, the comb moves), 6.2 dB (2x12) | 2.2 dB (the floor bounce moves) |

An omni backed off to distance 1 gains 10.2 dB of low end (40–150 Hz against 1.5–4 kHz) on the
4x12, 5.1 dB on the 2x12 and none on the 1x12. A cardioid's proximity loss is mostly made up by the
cab sum, which is the "bigger" sound of a backed-off 4x12, with the cones' comb at 1–2 kHz.

### 3b. Compatibility with v0.2.0

Old presets and sessions load with the default azimuths and no angle, so each mic sits where
v0.2.0 drew it. What changes is what v0.2.0 did not model: the other speakers, and the mic's
height for the floor bounce. Largest third-octave difference against the v0.2.0 engine,
100 Hz–8 kHz, position 0.5, default azimuth:

| | distance 0 | 0.25 | 0.5 | 1 |
|---|---|---|---|---|
| 1x12, Dyn Classic / Ribbon Smooth / Cond Precision | ≤ 0.03 dB | ≤ 0.15 dB | 0.23 dB | ≤ 0.53 dB |
| 2x12, Dyn Classic (mic A default) | 0.9 dB | 1.5 dB | 2.6 dB | 4.1 dB |
| 2x12, Ribbon Smooth (mic B default) | 0.2 dB | 0.9 dB | 2.0 dB | 3.9 dB |
| 4x12, Dyn Classic | 2.2 dB | 4.3 dB | 6.2 dB | 8.7 dB |
| 4x12, Ribbon Smooth | 0.3 dB | 1.7 dB | 3.9 dB | 7.5 dB |
| 4x12, Cond Precision | 4.2 dB | 7.0 dB | 8.4 dB | 9.7 dB |

On the grille the 4x12's Dyn Classic gains up to 1.5 dB below 250 Hz and loses 2.2 dB around
630 Hz (the three other cones, whose paths are 0.75–1.1 ms longer). The factory presets keep their level within
±0.07 dB RMS (`sedef-render`, all 13).

## 4. Switching and smoothing

- **Voices.** Each mic has two voices. A change of cab, mic type or custom IR starts the idle voice
  on the new configuration and crossfades. The incoming voice first runs muted for a 5 ms
  pre-roll so its filters settle, then the 30 ms smoothstep crossfade runs. A change that arrives
  during a fade waits for it to finish. The amp/cab link in the engine switches cab and mic types
  together, which is a single crossfade.
- **The pose** glides with a 30 ms one-pole, in Cartesian coordinates (x, y, distance, and the
  angle over its 60° range): a move across the cone goes straight across, and crossing the
  ±180° azimuth wrap moves the mic 2°, not round the cone. The affected sections (presence, edge
  low-pass, warmth, proximity, air, off axis, the other speakers' low-passes) are redesigned once
  per control block only when their inputs moved, and their coefficients are **interpolated per
  sample** across the block; delays and gains (time of flight, bounce, bleed taps, level and polar
  gain) are interpolated per sample too. A plain 32-sample coefficient staircase measured −65 dB
  of zipper noise above 2.5 kHz on a 110 Hz tone; interpolation brings it below −100 dB.
- **Off-axis stages** are identity on axis, so they only run while a mic is (or was just) turned.
  Back on axis, they stop once their state is below 10⁻⁷ of the block's peak; a custom IR at 0°
  is then exactly the convolution again.
- **First settings snap.** The first `setSettings()` after `prepare()`/`reset()` starts voices
  and smoothers in their final state, like every module; later calls glide. If audio was
  processed before that first call (on the defaults), it glides too rather than cutting.
- **Idle mics** (close switch and room switch both off and settled) cost nothing; they restart
  clean, without a crossfade, when switched back on, with their smoothers snapped to the current
  targets (they are not advanced while idle). The room processor keeps ringing after its
  sends close (spill-over) and stops itself `2 · RT60` later.

## 5. Level calibration

All voices are normalised to a common **loudness index**: the mean, in dB, of the power in
sixteen third-octave bands centred from 157 Hz to 5 kHz (`cab::loudnessIndexDb`; a unit impulse
reads 0 dB). In `prepare()` every procedural (cab, mic) voice is evaluated analytically at the
reference pose (`cab::referencePose`: position 0.5 at 2 o'clock, distance 0, on axis, the other
speakers included) and scaled to 0 dB. Moving or turning the mic away from that point changes the
level naturally, as on a real cab: turning Dyn Classic 45° off axis costs 2.7 dB. `makeKernel` scales custom IRs to 0 dB on the
same scale (gain clamped to ±60 dB). So swapping a procedural mic for an IR, or one IR for
another, keeps the level within a dB or two, and an impulse IR plays at exactly unity.

## 6. Custom impulse responses

### What placement does to an IR

An IR already holds its mic's type, position and distance, so position, azimuth and distance do
nothing to it (as in v0.2.0), and the UI disables those controls and leaves the marker where it
was. Turning the capsule is different: it changes what any capsule hears. While a custom-IR mic is
turned, the off-axis stages and polar gain of a cardioid dynamic (Dyn Classic's model, the
commonest capsule for cab IRs) run after the convolution. The room send stays before them. At 0°
nothing runs and the output is the convolution, bit for bit.

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

Apple M2, 48 kHz, 64-sample blocks (`sedef_engine_tests "[.bench]"`, 4x12 with Dyn Classic and
Ribbon Smooth at distance 0.4). Ranges span a quiet machine and one shared with parallel builds:

| Configuration | ns/sample | % of one core |
|---|---|---|
| 2 procedural mics + rooms | 103–110 | 0.5 |
| 2 procedural mics, no room | ~88 | 0.42 |
| same, automating the whole pose (around, across, back, turning) | 142–145 | 0.7 |
| one 1 s custom IR | 110–150 | 0.55–0.7 |
| two 1 s custom IRs + rooms | 235–320 | 1.1–1.55 |
| custom IR + procedural + rooms, 1-sample host blocks | 270–325 | 1.3–1.6 |

Budgets: procedural < 2%, one 1 s IR < 3%. The other speakers of the 4x12 cost about 18 ns/sample
for two mics: A/B against the v0.2.0 engine in the same session, 85 → 103 ns/sample static and
97 → 142 automating. Their three taps run in one interleaved pass so the filter recursions
overlap, and `cab::runStages` runs static biquad sections fused in groups of up to four (five
split 3 + 2), which makes the chains about 30% faster than section-by-section loops (A/B under
the same load: 165 → 115 ns/sample). `makeKernel` for a 1 s IR at 44.1 → 48 kHz takes 5–8 ms off
the audio thread; `prepare()` takes 4–13 ms (it calibrates all 30 procedural voices).

## 9. References

- R. Bristow-Johnson, *Cookbook formulae for audio EQ biquad filter coefficients* (public note):
  all peaking, shelving, low- and high-pass sections, via `core/Biquad`.
- R. H. Small, "Closed-Box Loudspeaker Systems, Part I: Analysis", *JAES* 20(10), 1972: the
  closed box as a second-order high-pass with total Q (the 4x12's low end).
- L. L. Beranek and T. J. Mellow, *Acoustics: Sound Fields and Transducers*, Academic Press, 2012:
  open-baffle dipole cancellation, cone break-up, pressure-gradient microphones and proximity effect.
- J. Eargle, *The Microphone Book*, 2nd ed., Focal Press, 2004: polar-pattern families
  (`A + (1 − A) cos θ`), proximity effect as a property of the pressure-gradient component, and
  off-axis response: diaphragm size against high-frequency directivity.
- L. E. Kinsler, A. R. Frey, A. B. Coppens and J. V. Sanders, *Fundamentals of Acoustics*, 4th ed.,
  Wiley, 2000, ch. 7: the baffled circular piston, its on-axis near field
  (`|p| ∝ sin (k (√(z² + a²) − z) / 2)`), far field `k a² / (2R)` and directivity `2 J1(x) / x`.
- H. F. Olson, *Acoustical Engineering*, Van Nostrand, 1957: piston directivity, multiple-source
  interference, and directional microphones.
- J. B. Allen and D. A. Berkley, "Image method for efficiently simulating small-room acoustics",
  *JASA* 65(4), 1979: the image source behind the floor bounce.
- W. E. Thomson, "Delay networks having maximally flat frequency characteristics", *Proc. IEE*
  96(3), 1949: the Bessel filter whose flat group delay stands in for the real directivity.
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

All voicing values, the speaker spacings and the off-axis amounts are our own targets, informed
by the general behaviour in these texts. No third-party impulse responses, measurements or
product data are used (ADR-0006).

## 10. Tests (`[cab]`)

| Test | Asserts |
|---|---|
| finite output | random settings changed every few ms (incl. custom IR slots, one for the wrong rate, azimuths far outside ±180° and NaN, angles outside 0–60°), full-scale noise, 44.1/48/96 kHz × blocks 1, 17, 64, 512: finite and bounded |
| empty / mono / in-place | 0-frame blocks; one output channel gets the constant-power fold; `in` aliasing an output gives identical results |
| plausible responses (all three rates) | 4x12 ≥ 12 dB down at 10 kHz vs 1 kHz for every mic type at the brightest position (≥ 25 dB for Dyn Classic); resonance peak in 88–110 / 78–100 / 72–95 Hz and a real peak; open back has ≥ 6 dB less 40–70 Hz than closed; 4–8 kHz energy strictly falls from centre to edge (3 cabs × 3 mics); on the 1x12, distance removes > 3 dB of < 150 Hz proximity for the dynamic, more for the ribbon, less for the omni, is monotonic, and drops the mid level by 1–9 dB; backing an omni off brings in more lows on the 2x12 than the 1x12 and more again on the 4x12 (≥ 2 dB each, monotonic), and the cab sum makes up ≥ 3 dB of a cardioid's proximity loss; voicings order (off-axis and ribbons darker, Dyn Bright brighter) |
| real time = model (`[placement]`, 44.1/48/96 kHz) | 12 random poses over all cabs (position, azimuth, distance, angle): the rendered impulse response matches `voiceResponse` × calibration within 0.1 dB from 60 Hz to 4 kHz |
| round speaker | 1x12 on the grille: azimuth changes the response by < 0.15 dB |
| placement over the baffle | 4x12 edge, towards vs away from the other cones: 1–5 dB apart on the grille, > 6 dB and > 2× that at distance 1; the 2x12 too, more at distance; each neighbour gets earlier and louder as the mic moves towards it, and louder with distance (> 0.5 at distance 1); a figure-of-eight hears < 20% of what an omni hears of them on the grille |
| floor bounce | top vs bottom of the 1x12 cone: the bounce arrives > 0.5 ms later from the top; > 1 dB apart at distance 1, < 0.2 dB on the grille |
| off axis | 4–8 kHz falls strictly with the angle (0–60°) for all ten types; Dyn Classic loses > 6 dB; Cond Large > Cond Small + 2 dB; near-omni least; mids drop by the polar gain (cardioid −2.5, figure-of-eight −6 dB at 60°); a cardioid loses > 1 dB of proximity at 60°, a figure-of-eight none; Dyn Classic at 45° matches the Off-Axis voicing's shape within 0.5 dB (measured 0.3) and plays > 1 dB quieter |
| custom IR placement | position, azimuth and distance leave an IR bit-identical; an impulse IR on axis plays the input exactly; turned 45° it follows the dynamic capsule's off-axis model within 0.02 dB; back on axis it is exactly the input again once the glide ends |
| azimuth wrap | 179° → −179° at the cone edge, 35 cm back: the output barely changes (< 5% of a move straight across the cone) |
| time of flight | cross-correlation of distance 1 vs 0: 0.8–1.1 ms later |
| mono fold | A at distance 0 plus B at 0.6 shows a notch ≥ 10 dB below the coherent sum in 300 Hz–3 kHz; B inverted with identical settings cancels to < −120 dB |
| pan | hard left is silent on the right; centre is −3 dB per side |
| first settings | engine order (setSettings + process per block) after prepare, and after reset following other audio, is bit-identical to settings applied before reset (poses with azimuth and angle included); a later level change glides (no jump, −20 dB reached after the ramp); settings arriving after audio on the defaults keep the old voice through the pre-roll |
| idle wake | azimuth, distance, angle, level, pan and room level changed while a mic is idle: output after waking is bit-identical to having had them all along; hard left is silent on the right from the first sample |
| room | RT (T20) between 0.25 and 0.6 s; nothing in the first 2 ms; tail rings after switch-off, decays, then the output is exactly zero |
| click-free automation | 6 s of a 110 Hz tone while every control moves (position, distance, azimuth round the cone across the ±180° wrap, off-axis angle, level, pan, room level sweeps; cab, both mic types incl. custom IR, polarity, enables, rooms switching). Energy above 2.5 kHz (8th-order high-pass) stays < −80 dB re output RMS (measured ≈ −98 dB) |
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

- Position, azimuth and distance do not affect custom IRs: the IR already contains its mic
  placement. The off-axis angle applies a generic dynamic capsule's model, which is a guess about
  what the IR was recorded with.
- Only the mic type is per cab (as in the parameter schema); the pose is per mic.
- The other speakers are ideal, identical, coherent pistons. Real cones differ a little and break
  up differently above ~1 kHz, so real combs are shallower than the model's deepest notches
  (−10 dB and more at distance 1 on the 4x12).
- The capsule always leans towards the dust cap; leaning away, or sideways, is not modelled.
- The position terms do not change with distance (section 3 explains why that is close), and the
  open backs' rear wave does not depend on where the mic is.
- The mic stays over the miked speaker (position ≤ 1); a mic between the speakers or in front of
  the whole cab is not available.
- The procedural models are linear. Speaker cone compression and break-up distortion at high
  levels, and voice-coil heating, are not modelled.
- The room is a single fixed small room; size and decay are not exposed.
- Kernels must be rebuilt by the shell after a sample-rate change. Until then the slot plays Dyn
  Classic.
- The FFT work runs once per P samples rather than being spread over the block; at P = 256 the
  burst is a few microseconds.
