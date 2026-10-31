# Metronome

`engine/include/sedef/dsp/Metronome.h`, `engine/src/dsp/Metronome.cpp`

## What it models

A practice click with odd meters and per-step accents. Every sound is
synthesised; nothing is sampled. The metronome runs free (it is not synced to
the host) and is added on top of the stereo output at `volumeDb`, centred.

## Scheduling

- Steps per bar come from `stepsFor(signature)`: 2/4 → 2, 3/4 → 3, 4/4 → 4,
  5/4 → 5, 6/8 → 6, 7/8 → 7, 9/8 → 9, 12/8 → 12.
- The step interval is 60 / bpm s for x/4 meters and 30 / bpm s for x/8
  meters, which count eighths. bpm is clamped to 20–400.
- Scheduling is sample-accurate across any block partitioning. Steps are
  anchored to an absolute sample count: step k after the anchor sounds at
  `ceil(anchor + k · interval)`, so rounding never accumulates. The first
  sample of every click is non-zero (cosine-phase partials), so a click's onset
  is exactly its scheduled sample.
- Enabling starts the downbeat (step 0) on the first sample of the next block.
- Changing tempo or meter re-anchors at the last click, so the next one comes
  one *new* interval later. If a step is already overdue it sounds at once and
  the grid continues from there.
- When the meter gets shorter mid-bar, a new bar starts.
- `currentStep()` is the playhead: the last step reached, *including* silent
  (Off) steps, so a UI step display never stalls. It reads −1 while disabled.
  It is a relaxed atomic.
- Disabling stops scheduling. Ringing clicks finish naturally.

## Step states

| State | Gain | Pitch |
|---|---|---|
| Accent | 1.0 (0 dB) | × the sound's accent ratio (higher variant) |
| Normal | 0.55 (≈ −5 dB) | as designed |
| Soft | 0.22 (≈ −13 dB) | as designed |
| Off | silent | — |

Unknown step values are treated as Off.

## Sounds

Each sound is a small recipe:

- up to three decaying partials, each with an optional pitch flick, an optional
  second slower decay, and a sine or soft-square (tanh of a sine) shape;
- an optional band-pass "body" on the tonal part;
- an optional band-passed noise burst with its own attack and decay;
- a short overall attack ramp.

| Sound | Recipe | Accent |
|---|---|---|
| Blip | sine 1046.5 Hz (C6), τ 22 ms, starting 1.6× high and dropping in ~3 ms; faint octave | a fifth up |
| Wood | inharmonic block modes 1180 / 2710 / 4130 Hz (τ 26 / 12 / 6 ms) plus a 2.4 kHz band-passed noise knock | +2 semitones |
| Clave | one high-Q mode at 2.5 kHz (τ 38 ms), a faint 6.9 kHz partial, a tiny noise click | +2 semitones |
| Cowbell | two detuned soft squares at 540 / 800 Hz through a 1.1 kHz band-pass; fast drop (τ 10 ms) into a ringing tail (τ 110 ms, 35 %) | +1 semitone |
| Rim | noise crack (3.2 kHz band, τ 3 ms) plus a 1.7 kHz ring (τ 9 ms) and a little 480 Hz shell body | +1.3 semitones |
| Shaker | band-passed noise at 6.5 kHz with a 7 ms swish attack and τ 38 ms decay | brighter band |

Levels are trimmed so every sound reaches a similar short-term (50 ms)
loudness, with accent peaks between 0.34 and 0.6 at 0 dB volume. The 2.5 kHz
clave sits a little lower because the ear is most sensitive there.

A voice stops once its envelope falls below −80 dB, or after its recipe length
with a 5 ms fade. Ended voices output exact zeros.

There are four voices. At fast tempos a voice starts fading after three steps,
so a new click never has to cut a ringing one off. Reusing the oldest voice is
only a fallback after a sudden tempo jump.

## References

- P. R. Cook, *Real Sound Synthesis for Interactive Applications*, A K Peters,
  2002. Modal synthesis, noise-excited percussion, shaker models.
- J.-M. Adrien, "The missing link: modal synthesis", in *Representations of
  Musical Signals*, MIT Press, 1991.
- R. Bristow-Johnson, *Audio EQ Cookbook*. Band-pass filters.
- The cowbell is the classic analog-synthesis approach (two square oscillators
  into a band-pass, two-stage envelope), described generically in synthesis
  textbooks.

## Tests (`engine/tests/timefx/MetronomeTests.cpp`, tag `[timefx][metronome]`)

- `stepsFor` covers every signature and clamps out-of-range values.
- Click onsets equal `ceil(k · interval)` exactly at 44.1, 48 and 96 kHz, for
  x/4 and x/8 meters, with random odd block sizes from 1 to 700.
- Off steps produce no onsets.
- For every sound, Accent > 1.3 × Normal > 1.5 × Soft (peak), and accent peaks
  fall between 0.2 and 0.8.
- `currentStep` tracks the playhead across Off steps block by block, and reads
  −1 when disabled.
- Enabling mid-stream starts the downbeat on the first sample of that block.
- Disabled adds nothing (bit-exact), and +6 dB of volume doubles the peak.
- Output stays finite with random settings at all rates and block sizes.
- After a tempo change the next click is one new interval after the last.
- `[.bench][timefx-bench]` prints ns/frame (busiest case: 12/8 cowbell at
  240 bpm).

## Limitations / future work

- No host sync and no count-in.
- There is no swing setting, and no per-step choice of sound.
- The phase of the noise components is random, so noise-based sounds vary
  slightly from hit to hit, as acoustic ones do.
