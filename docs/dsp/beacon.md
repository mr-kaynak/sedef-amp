# Beacon (photocell vibe)

`engine/include/sedef/dsp/Beacon.h`, `engine/src/dsp/Beacon.cpp`

## What it models

The photocell vibe: a four-stage phase shifter whose stages each have a different capacitor, so
their break frequencies are staggered over several decades, and whose sweep comes from a lamp
lighting four light-dependent resistors. The lamp heats faster than it cools and the cells react
to light faster than to darkness, so a sine LFO comes out as a lopsided throb: a quick brightening
and a long fade. **Chorus** mixes the stages with the dry signal (moving notches, the swirl);
**Vibrato** plays the stages alone (a pitch wobble).

Ripple's chorus and vibrato are delay-line effects; Beacon's are phase-shift ones, with a
different, rounder movement, so the two don't overlap.

## Chain position

**After the drives, first of the modulation pedals** (after Coral: Beacon, Gyre, Jetty, Ripple).
The vibe is classically played after a fuzz or overdrive, which the throb then moves; in front of a
drive, the drive's compression flattens it. It comes before the phaser and flanger because its
slow, wide swirl sits best under their faster, sharper notches.

On the board it can also stand after the amp and cab, where it moves the finished amp sound in stereo
(the effects-loop place; [board.md](../board.md)).

## Signal flow

```
LFO (sine) ─► drive = ½ (1 + depth · (1 + speed / 3 Hz) · sin) ─► power = drive²
   ─► lamp: heat 12 ms / cool 30 ms ─► cells: on 5 ms / off 45 ms ─► light L
   ─► R = R_lit · L^−0.8 (≤ 300 R_lit), one per stage

in ─┬─► stage 1 ─► stage 2 ─► stage 3 ─► stage 4 ─► wet
    │    (15 nF)   (220 nF)   (470 pF)   (4.7 nF)
    └─────────────────────────────────────────────► Chorus: ½ (dry + wet) · 1.58   Vibrato: wet · 0.95
```

- **Stages.** Each stage is a transistor phase splitter whose two legs drive an RC network with
  the stage's photocell. Its legs are not quite balanced: `H = (α + β) · LP − β`, α at DC and −β
  at high frequencies, with the per-stage α and β measured by Darabundit, Wedelich and Bischoff
  (DAFx-19). So each stage is a near all-pass with a small high shelf that moves with the sweep.
  The low-pass is a bilinear one-pole without prewarping (`g = 1 / (2 · fs · R · C)`): the phase
  below each break then matches the analog stage at any rate, which is where these stages' notches
  fall. Prewarping to the break would instead squeeze the 470 pF stage (14.5 kHz at idle, 44 kHz
  lit) into the top octave and move the upper notch with the sample rate.
- **Stagger.** With the capacitors above and the cells' lit resistances (12.7, 6.86, 7.69 and
  6.22 kΩ), the four breaks sit at 276 Hz, 35 Hz, 14.5 kHz and 1.8 kHz at idle: spread over about
  8.6 octaves, and all moving together because one lamp lights them. In Chorus the mix cuts two
  notches about six octaves apart (85 Hz and 5.2 kHz at idle), where four matched stages' are
  2.5 octaves apart.
- **Lamp and cells.** The LFO swings the lamp current about its idle point; the filament's heat
  follows the power (current squared) with a 12 ms heat and 30 ms cool time, and the cells follow
  the light with 5 ms on and 45 ms off. Resistance falls as a power of the light, `L^−0.8`, from
  the lit value up to a dark ceiling 300 times higher. The lag shrinks the swing as Speed rises, so
  the drive grows with Speed (`× (1 + speed / 3 Hz)`), clipping at the lamp's limits, as the
  circuit's does.
- **Mode.** Chorus mixes 50/50 with the dry signal; the notches take about 3 dB out of a guitar,
  which the 1.58 trim makes up. Vibrato plays the stages alone, trimmed 0.95 for the shelf's
  treble lift, so both modes play at the same level. The switch crossfades over 30 ms
  (smoothstep).
- Linear: no oversampling, no latency.

## Control mapping

| Control | Mapping |
|---|---|
| `beaconSpeed` | LFO rate 0.3 → 8 Hz, skewed so noon is about 1.5 Hz. |
| `beaconDepth` | How far the lamp swings about its idle point, 0 → full. |
| `beaconMode` | Chorus (mixed with the dry signal) or Vibrato (the stages alone). |

## Measurements (48 kHz, Depth 0.7; `engine/tests/pedals/ModsReport.cpp`)

| Speed | Light swing | Stage 1 sweep | Light rising for |
|---|---|---|---|
| 0.3 Hz | 0.000 … 1.000 | 3 … 835 Hz | 34 % of the cycle |
| 1 Hz | 0.001 … 1.000 | 3 … 835 Hz | 36 % |
| 2 Hz | 0.018 … 1.000 | 34 … 835 Hz | 39 % |
| 4 Hz | 0.145 … 1.000 | 178 … 835 Hz | 42 % |
| 8 Hz | 0.444 … 0.985 | 436 … 825 Hz | 40 % |

A sine would rise for 50 % of the cycle. In Vibrato at 1 Hz and full Depth, the brightening bends
a 500 Hz tone up 14.8 Hz and the fade bends it down 4.0 Hz. Guitar level: within 0.2 dB of unity in both modes at
Depth 0, 0.7 and 1. Cost: 125 ns a sample, 0.6 % of one core (48 kHz mono; the four stages
recompute their gains every sample from a `pow`).

## References

- K. J. Darabundit, R. Wedelich, P. Bischoff, *Proc. DAFx-19*, Birmingham, 2019: a digital
  grey-box model of the classic photocell vibe, with its circuit, component values, the measured
  per-stage phase-splitter gains and the lamp and photocell behaviour.
- R. G. Keen, geofex.com (online): his article on the classic photocell vibe's circuit, the
  staggered capacitors and the lamp-and-photocell LFO.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011: phaser and vibrato.
- The lamp and cell time constants, the light-to-resistance law and the trims are our own fit to
  the behaviour these describe.

## Tests (`engine/tests/pedals/BeaconTests.cpp`)

- Finite for random settings and full-scale noise; silence in gives silence out.
- Staggered stages: the four breaks at idle within 1 % of 276 Hz, 35 Hz, 14.5 kHz and 1.8 kHz,
  keeping their ratios as the light moves. The Chorus mix's measured notches match the analog
  circuit's (85 Hz and 5.2 kHz at idle): the low one within 1 % and deeper than 25 dB at every rate,
  the high one within 7 % below 80 kHz and 2 % above (the bilinear map's warping) and deeper than
  10 dB; more than 5.5 octaves apart.
- The lamp's asymmetry, measured: the LFO runs at the Speed within 1 %; the light rises in 25 to
  43 % of the cycle; the light's second harmonic is within 26 dB of its fundamental (the driving
  sine has none); in Vibrato the bend up is more than 1.3 times the bend down.
- Depth: at 0 the light holds at idle; more Depth swings it further; at full Depth stage 1 sweeps
  from below 100 Hz to above 700 Hz.
- The modes: in Chorus a tone's level swings more than 12 dB as a notch passes it, in Vibrato less
  than 3 dB; both play the guitar at the same level, and Vibrato bends the pitch.
- Speed, Depth and Mode automation are click-free; no harmonics or aliases at a static setting
  (THD < 0.01 %, inharmonic < −75 dB); output identical for block sizes 1 to 4096.

## Known limitations

- The lamp and cells are first-order lags with separate rise and fall times, not a thermal or
  photochemical model; the phase splitters are linear (no transistor saturation).
- One lamp for both channels, as in the circuit: no stereo spread.
