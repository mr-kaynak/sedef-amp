# Doubler

`engine/include/sedef/dsp/Doubler.h`, `engine/src/dsp/Doubler.cpp`

## What it models

Double tracking: the same part played twice and panned hard left and right.
Two takes never line up, and the way they differ is what makes the sound:

- **Timing changes from note to note, and from phrase to phrase.** A player's
  onsets sit "perhaps 10–20 ms" off the grid, and the deviations are not
  independent: a professional drummer playing to a click averaged −16 ms, and
  over one stretch of about 80 beats ran at −29 ms [Hennig 2011, Hennig 2012].
  Between two players without a click the spread is larger: Rasch measured
  between-player asynchrony with a standard deviation of 49 ms in a string trio
  and 32 ms in a wind trio, 24–73 ms across his recordings [Rasch 1979, as
  reported by Wing 2014]. An overdub played against the first take is tighter
  than that. The offset between two independent takes has √2 times the spread
  of one take, so tight takes a few ms off the grid each end up 5–15 ms apart,
  looser ones up to 30 ms.
- **Pitch differs by a few cents**, and it differs per note, not as a wobble:
  fretting deforms the string, so how hard and where each note is fretted moves
  its pitch [Varieschi 2010]. Pitch variation between takes is the part a
  fixed-modulation doubler gets wrong: modelling it from natural takes is
  perceived as closer to real double tracking than signal-processing ADT
  [Tamaru 2019].
- **Tone and dynamics change a little per note**: pick strength, angle and
  position are never the same twice.

Artificial double tracking (ADT) goes back to 1966, when Ken Townsend at EMI's
London studios fed a vocal from the multitrack's record head into a second tape
machine whose speed an oscillator varied, so the copy was delayed and its pitch
moved [ADT]. Any ADT built from a delay can only make the copy late [Anderton].
Sedef keeps that constraint (the played side is never delayed) but changes how
the copy moves: not a continuous wobble, which reads as chorus [DAFX, Dattorro
1997], but a new take per note.

## Signal flow

```
L in ───────────────────────────────────────────────► original (untouched)
                                                                │
R in ─► delay line ─► read A ┐                                  ▼
                   └► read B ┴► crossfade ─► tone tilt ─► level ─► soften ─► double
                        ▲                                       + 1 dB
onset source ─► detector ┘ (new offset, detune, level, tone per note)

wet L = cos θ · original + sin θ · double        θ = (1 − width) · π/4
wet R = sin θ · original + cos θ · double
out   = in + mix · (wet − in)
```

The left side is the input itself: at full width the Doubler passes it through
bit for bit, so it adds no latency to what you play (in the whole engine too,
see the tests). The right side becomes the second take. With a mono source
(L == R) that is double tracking; a stereo cab or room keeps its image, since
each side starts from its own channel. A mono stream (one channel) passes
untouched.

## The second take, note by note

At every onset the double draws its next note. Within a note nothing jumps.

| Per note | Draw                                                                          | Variation 0.5 (default)                                            | Variation 1            |
| -------- | ----------------------------------------------------------------------------- | ------------------------------------------------------------------ | ---------------------- |
| Offset   | spread · 2^(variation · z), z ≈ unit normal clamped to ±2.5, within 2.5–40 ms | half the notes within ×0.79–1.26 of spread, 80 % within ×0.64–1.56 | 80 % within ×0.41–2.43 |
| Detune   | uniform ±6 · variation cents, held for the note                               | ±3 cents                                                           | ±6 cents               |
| Level    | uniform ±1.5 · variation dB                                                   | ±0.75 dB                                                           | ±1.5 dB                |
| Tone     | band above 2 kHz (one-pole split), uniform ±3 · variation dB                  | ±1.5 dB                                                            | ±3 dB                  |

`z` mixes a phrase component with note jitter, `z = 0.8 p + 0.6 e`, where `e`
is independent per note and `p` is a first-order autoregressive process across
notes, `p ← 0.9 p + 0.44 g`. A take that runs late tends to stay late for a few
notes (lag-1 correlation 0.58), as in Hennig's measurements, where listeners
preferred correlated timing deviations to independent ones. Their long-range
correlated (1/f) generator is patented (US 7,777,123); this short-memory
process is a different, older idea and imitates only the phrase-level drift.

The detune drifts the read position slowly (3 cents ≈ 1.7 ms per second). On a
long note the drift turns round before it moves the offset by more than
clamp(0.3 · offset, 0.5 ms, 3 ms), so the pitch then swings slowly between +c
and −c. A shared micro wander (smoothed random walk, 1.3 Hz, ±0.2 ms) adds
about a cent of life to held notes, 30 % of it even at variation 0. Nothing is
periodic: there is no LFO.

## Re-timing without artefacts

Moving a delay's read position while a note plays bends its pitch; jumping it
clicks or repeats audio. So the double has two read positions. At an onset the
idle one takes the new note's offset and the two crossfade (smoothstep). The
fade has to end before the new note reaches either read position, so both
still play audio from before the note and the note itself comes out once, at
its new offset:

```
fade length = min(6 ms, min(old offset, new offset) + lead − rise − 0.5 ms)
```

`rise` is how long ago the detector's fast envelope left its slow one, the
onset's start. A note whose fade would be shorter than 1.5 ms keeps the
previous offset. One that would move by less than 0.25 ms keeps it too, but
still gets its own level, tone and detune. Level and tone ramp to the new draw
over the same fade.

A re-timing also replays (when the offset grows) or skips (when it shrinks)
the stretch of audio between the two read positions, up to 37 ms of it. That
stretch has to be the tail of the previous note, not an attack, so every
attack is timed, including the ones the detector lets pass within its 70 ms
(a grace note, fast tremolo picking). A note keeps the previous offset unless
the attack before it has passed both read positions by 5 ms. An onset seen
further ahead than 6.5 ms (a long `lead`) waits until 6.5 ms before its note
arrives, so the stretch lies just before that note however early it was seen.
On dense notes 80–100 ms apart with leads of 0, 31 and 46 ms, 79 of 82 notes
re-time and none is heard twice or dropped; with grace notes, the note after
each grace note keeps its offset.

**Switching on.** The engine resets the Doubler when it comes out of bypass, so
its delay line restarts from silence under whatever is already sounding. Until
the line has filled past the take's offset, the take is the undelayed input;
then it fades over in 10 ms. The double's filters start settled on the first
sample. Without that, the right side would drop out for the length of the
offset and the double would start with a step. A re-timing never moves the
take past what the line holds yet (the first note after a reset draws 1.65 ×
spread, and a pick soon after switching on would otherwise read the silence
from before the reset, then step). For its first 40 ms the onset detector only
listens: its slow envelope catches up with what is already sounding instead of
calling the switch-on an onset.

**The onset detector** compares the power above 2.5 kHz (2nd-order high-pass)
through a 1 ms and a 40 ms one-pole: an onset is a rise of more than 4 dB
(ratio 2.5), then nothing for 70 ms. It is an energy-rise detector of the kind
surveyed by Bello et al. [Bello 2005; Klapuri 1999].

The rise must also stand 1.5 times over the input's own background, not over a
fixed level. Up to 0.5.0 that level was −70 dB of high-frequency power, which a
player peaking at −30 dBFS with soft attacks (fingerstyle, a neck pickup) barely
clears: on a synthetic fingerstyle phrase 5 of 28 onsets landed where they do at
full level, and 36 dB down none were found. The background is the peak the
input's hiss, hum and buzz reach between notes: the fast envelope's peaks, held
over 100 ms, longer than a mains period, so a buzz's spikes count at their
height, not their mean. As in the Trench's level gate, it falls at once, is
first set as the detector warms up after a reset, and rises only while the
input holds still: the held peaks within 1.5 dB of their 0.3 s mean for 0.2 s.
Hiss, hum and buzz hold still; ringing and decaying notes do not. It then rises
by 60 dB a second, so a buzz that starts between notes is learnt within a
second (until then its spikes pass for attacks). It never goes above the old
fixed level, so the detector is never less sensitive than before, and onsets at
normal levels are unchanged: every factory preset renders bit for bit the same,
with the Doubler off or on.

| Material (detector onsets)                          | up to 0.5.0               | now           |
| --------------------------------------------------- | ------------------------- | ------------- |
| fingerstyle, 0 dB vs −24 dB (peaks −30 dBFS)        | 5 of 28 at the same times | 28 of 28      |
| fingerstyle, 0 dB vs −36 dB (peaks −42 dBFS)        | none found                | 28 of 28      |
| picked sixteenths, 0 dB vs −36 dB                   | 30 of 56                  | 56 of 56      |
| fingerstyle at −30 dBFS peaks over −100 dBFS hiss   | 19 of 28 within 1 ms      | 27 of 28      |
| hiss, mains hum, 50/60 Hz buzz up to −60 dBFS alone | none                      | none          |
| a 50/60 Hz buzz at −90 to −60 dBFS that starts late | none                      | for 0.8–0.9 s |
| a sharp buzz at −50 dBFS or more                    | fires on it               | fires on it   |

Rows 1–3 compare onset times within two samples; the quiet versions are the
same playing with hiss 80 dB under its peaks scaled along, as an interface's
gain does. Row 4 compares with the full-level times within 1 ms, over hiss
that does not come down with the playing.

**The DI as onset source.** After a high-gain amp the signal is compressed to a
wall: on the synthetic palm-muted riff the detector's ratio after an onset had
a median of 1.6 (the threshold is 2.5), and only 9 of 64 notes re-timed. In the
engine the Doubler therefore listens to the instrument before the amp, where
pick attacks are clean: 59 of 64 notes re-time. It takes the signal after
transpose (what the noise gate listens to), since the pitch shifter delays it
by a varying 3–50 ms that nothing reports. From there the signal reaches the
Doubler one chain latency later (`lead`: the pedals' and amp's oversampling),
which gives the crossfade extra room. Without a source (module tests, other
hosts of the module) it listens to its own input with no lead.

## Control mapping

| Control                      | Mapping                                                                                                                                                                       |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| spread (3–20 ms)             | median offset of the second take (unchanged id and range). Glides through two one-poles, 120 ms each, so automation bends pitch smoothly. Non-finite values fall back to 6 ms |
| variation (0–1, default 0.5) | scales every per-note draw above; 0 = one fixed offset, no detune, no level or tone change                                                                                    |
| width (0–1, default 1)       | constant-power pan of the two takes: 1 = original hard left, double hard right; 0 = both centred (L == R)                                                                     |
| mix (0–1, default 1)         | dry/wet of the whole stereo pair; 0 = the input untouched                                                                                                                     |

Width and mix ramp over 30 ms. Variation acts on the next note's draw.

## Mono compatibility: the trade-off

Summing two copies of a signal offset by Δ is a comb filter, with notches at
(2k + 1) / (2Δ): at 6 ms the first is at 83 Hz, then 250 Hz and so on. This is
inherent to two-take double tracking (two real takes comb in mono too) and the
main cost of the effect. The strategy is the one v0.2.0 had, now stronger:

- **The notches move.** In v0.2.0 the offset drifted by about a millisecond;
  now every note has its own offset, so over a phrase no frequency stays in a
  notch. On a noise phrase the deepest notch of the phrase-averaged mono sum is
  2.9 dB at the default settings, against 8.9 dB for v0.2.0 and 12.3 dB for a
  fixed offset (variation 0).
- **The double is darker and 1 dB louder, and differs per note.** A one-pole
  low-pass at 7.5 kHz and +1 dB limit notch depth (unequal levels and spectra
  cannot cancel fully), and the per-note level and tone draws change the
  mismatch from note to note. The 1 dB lift also partly offsets the precedence
  effect, which pulls the image towards the earlier (left) side
  [Litovsky 1999, Haas 1951].
- **Larger spreads are kinder in mono.** Notches get denser and less tonal; at
  15–20 ms they read as space more than colour.
- **Width trades stereo for mono safety.** At width 0 both takes are centred:
  the comb is then in both speakers, by choice.

Within one note the comb is still there (13–16 dB deep per 85 ms frame,
unchanged from v0.2.0); what changed is that it no longer sits still. In 1-out
hosts the engine folds its output to (L + R) / 2, where the double could only
comb-filter the part, so there the Doubler stays off whatever its switch says.
If mono playback of a stereo mix matters more than the double-tracked sound,
keep spread at 12 ms or more, lower width, or leave the Doubler off.

## A double from the second mic

Panning Mic A hard left and Mic B hard right in the Cab tab gives each take its
own mic: the played side is Mic A, the double is Mic B. Nothing in the Doubler
changes for this; it follows from each side starting from its own channel.
(`riff-9-new-dual-mic-*` in the listening files.)

## Measurements

All from the lab in this lane's scratchpad (`lab.cpp`, the v0.2.0 Doubler
compiled next to the new one). Noise phrase: 94 white-noise notes (1 ms attack,
80 ms decay, 140–460 ms apart); the module detects their onsets itself. Riff
and strum: the engine (`sedef-render`), Modern Metal Rhythm preset with a synthetic
palm-muted 140 bpm riff, Glassy Chorus (chorus and reverb off) with a synthetic strum,
onsets from the DI.

Per-note offset of the double, measured from the audio (correlation over each
note's first 60 ms), noise phrase:

|                                  | p05–p95 (ms) | sd (ms)  | mean change note to note (ms) | lag-1 corr. |
| -------------------------------- | ------------ | -------- | ----------------------------- | ----------- |
| v0.2.0, spread 6                 | 5.3–6.6      | 0.43     | 0.38                          | 0.36        |
| new, spread 6, variation 0       | 6.0–6.1      | 0.03     | 0.02                          | —           |
| **new, spread 6, variation 0.5** | **3.6–9.8**  | **1.95** | **1.51**                      | **0.48**    |
| new, spread 6, variation 1       | 2.5–15.9     | 4.50     | 3.32                          | 0.44        |
| v0.2.0, spread 12                | 11.2–12.7    | 0.45     | 0.38                          | 0.42        |
| new, spread 12, variation 0.5    | 7.2–19.5     | 3.88     | 3.01                          | 0.48        |
| new, spread 12, variation 1      | 4.3–31.8     | 8.87     | 6.64                          | 0.44        |

On the riff (63 notes) the defaults give 3.5–8.7 ms (v0.2.0: 5.5–6.9 ms);
spread 14, variation 1 gives 4.7–29.1 ms.

Pitch of the double against the original, from the slope of a 768-sample
delay track inside notes (|cents|):

|                            | mean     | p95      | max     | modulation                        |
| -------------------------- | -------- | -------- | ------- | --------------------------------- |
| v0.2.0 (both sides wobble) | 2.66     | 6.05     | 7.8     | 0.9 / 1.13 Hz sines + random walk |
| new, variation 0           | 0.10     | 0.35     | 1.1     | wander only                       |
| **new, variation 0.5**     | **1.43** | **3.15** | **3.8** | held per note                     |
| new, variation 1           | 2.84     | 6.04     | 7.5     | held per note                     |

Inter-channel correlation, and the deepest notch of the phrase-averaged mono
sum (1/24-octave bands, 100 Hz–8 kHz, median minus minimum):

|                               | corr. at lag 0 | best corr. over 0–45 ms | mono notch, noise | riff       | strum      |
| ----------------------------- | -------------- | ----------------------- | ----------------- | ---------- | ---------- |
| v0.2.0, spread 6              | 0.00           | 0.05                    | 8.9 dB            | 8.7 dB     | 9.3 dB     |
| new, variation 0              | 0.00           | 0.34                    | 12.3 dB           | 17.5 dB    | 17.9 dB    |
| **new, defaults**             | **0.00**       | **0.02**                | **2.9 dB**        | **6.2 dB** | **4.3 dB** |
| new, spread 10, variation 0.6 |                |                         |                   | 2.7 dB     | 2.4 dB     |
| new, spread 14, variation 1   |                |                         |                   | 3.2 dB     | 1.8 dB     |

The best correlation over all lags says how much the right side is one delayed
copy of the left over the phrase: 0.90 on the riff for a fixed offset, 0.17 for
v0.2.0, 0.09 for the defaults.

Clicks, from the tests: re-timing a held 220 Hz sine every 150–400 ms (no
attack to hide it) leaves the energy above 3 kHz at −90 dB (spread 3) and
−103 dB (spread 20) re the signal, with a largest second difference of
0.6–1.2 × 10⁻³ (the sine's own is 4.1 × 10⁻⁴; a step of 1 % would show as
5 × 10⁻³). With every control jumping end to end every 100 ms while notes
re-time, it stays at −87 to −93 dB. Switching the Doubler on under a held note
(the engine resets it and fades it in over 10 ms) stays at −93 to −105 dB; v0.2.0
left the right side silent there until its delay line had filled (−∞ dB 10–20 ms
after switching on at spread 20, against −0.1 dB now). The left side of the
engine's output is identical, sample for sample, with the Doubler on or off.

Cost: 10.7 ns per stereo frame (0.05 % of one core at 48 kHz) on an idle Apple
silicon machine, where Drift measured 49.5 ns.

## Tests (`engine/tests/timefx/DoublerTests.cpp`, tag `[timefx][doubler]`)

- Finite output with random, out-of-range and NaN settings and full-scale noise
  at 44.1, 48 and 96 kHz, in blocks of 1, 17, 64 and 512.
- Mono noise in gives |correlation(L, R)| < 0.1 at lag 0 for spreads of 3, 6
  and 20 ms; at variation 0 the correlation peak sits at the spread ± 0.5 ms.
- The played side is bit-identical to the input at full width, whatever the
  mix, at block sizes 1, 64 and 512; at mix 0 both sides are.
- Per-note offsets measured from a noise phrase: at variation 0 none re-time;
  at 0.5 ≥ 90 % re-time, median within 20 % of spread, sd > 1.5 ms, range
  2.4–24 ms, mean change > 1 ms, lag-1 correlation > 0.2; variation 1 spreads
  them 1.6 times wider, within 1.7–40.5 ms. The same in ms at 44.1, 96 and
  192 kHz.
- Each note arrives once: after silence the right side stays silent until the
  note's own arrival, and the note's attack shows up there once (spreads 3, 6
  and 15 ms, and with a 110-sample lead).
- Fast notes are each heard once, neither replayed nor dropped by a re-timing:
  notes 80–100 ms apart with leads of 0, 1500 and 2200 samples, and grace notes
  inside the detector's 70 ms with leads of 0 and 300, at spreads 14 and 20 ms
  and variation 1; most notes still re-time.
- Switched on under a held tone at any phase, with a pick soon after (up to
  1.65 × spread after the take has warmed up), there is no step: energy above
  3 kHz below −70 dB over the first 120 ms, second difference below 2 × 10⁻³.
- With no settings given it doubles from the defaults; an onset source set
  before a reset still applies to the next block.
- Turning Variation to 0 after a loose phrase brings every later note back to
  the plain double, +1 dB within 0.2 dB, including when the take already sits
  within 0.25 ms of the spread's offset.
- The onset source re-times a held tone that has no onsets of its own.
- The same playing (fingerstyle, picked sixteenths) 24 and 36 dB down gives the
  same onsets, within two samples; quiet fingerstyle over hiss that does not
  come down with it keeps all but two onsets within 1 ms.
- Hiss, mains hum and a 50 Hz sawtooth or 60 Hz click buzz up to −60 dBFS give
  no onsets; a buzz that starts after hiss alone is learnt within a second, and
  notes picked over it are heard.
- On an 8 s note the detune moves the offset, but by less than twice its drift
  limit plus 0.5 ms, and never below 1.5 ms.
- Levels: left exact, right +1 dB ± 0.3 at variation 0; per-note level within
  +1 dB ± 2.1 dB at variation 1 and does vary.
- Pitch between re-timings stays within 6 · variation + 2 cents and does move.
- Click-free: re-timing a held tone, every control jumping every 100 ms, and
  switching on under a held tone through the engine's bypass fade, keep the
  energy above 3 kHz below −80 dB and the second difference below 2 × 10⁻³, in
  blocks of 1, 64 and 512.
- Width 0 gives L == R; width 0.5 a partly correlated pair.
- Mono sum: at the fixed-offset notch frequencies, per-note offsets lose
  < 2 dB against the comb's peaks, a fixed offset > 5 dB.
- Silence in gives silence out. Mono passes untouched.
- In the engine, the left output with the Doubler on equals the output with it
  off; with one output the whole output does.
- `[.bench][timefx-bench]` prints ns/frame.

## Limitations / future work

- The double can only be late. Real takes swap who is ahead from note to note;
  doing that would delay the played side, so the image leans slightly to the
  left (the +1 dB lift offsets part of it).
- Re-timing needs an onset. Legato phrases and hammer-ons keep their note's
  offset until the next pick attack; notes closer than 70 ms share one, and a
  note right after a grace note keeps the offset before it.
- One double. Quad tracking (two doubles per side) is not modelled.
- Timing, pitch, level and tone share one Variation control.

## References

- [ADT] "Automatic double tracking", Wikipedia,
  <https://en.wikipedia.org/wiki/Automatic_double_tracking>.
- [Anderton] C. Anderton, "A truly authentic ADT (automatic double tracking)
  effect for studio mixing", craiganderton.org.
- [Bello 2005] J. P. Bello, L. Daudet, S. Abdallah, C. Duxbury, M. Davies,
  M. Sandler, "A tutorial on onset detection in music signals", _IEEE Trans.
  Speech and Audio Processing_ 13(5), 2005.
- [Blauert] J. Blauert, _Spatial Hearing_, rev. ed., MIT Press, 1997.
  Precedence effect and summing localisation.
- [DAFX] U. Zölzer (ed.), _DAFX: Digital Audio Effects_, 2nd ed., Wiley, 2011.
  Delay-based effects, comb filtering, modulated delay lines.
- [Dattorro 1997] J. Dattorro, "Effect design, part 2: delay-line modulation and
  chorus", _J. Audio Eng. Soc._ 45(10), 1997.
- [Haas 1951] H. Haas, "Über den Einfluss eines Einfachechos auf die
  Hörsamkeit von Sprache", _Acustica_ 1, 1951.
- [Hennig 2011] H. Hennig et al., "The nature and perception of fluctuations in
  human musical rhythms", _PLoS ONE_ 6(10): e26457, 2011.
- [Hennig 2012] H. Hennig, R. Fleischmann, T. Geisel, "Musical rhythms: the
  science of being slightly off", _Physics Today_ 65(7), 2012.
- [Klapuri 1999] A. Klapuri, "Sound onset detection by applying psychoacoustic
  knowledge", _Proc. IEEE ICASSP_, 1999.
- [Litovsky 1999] R. Y. Litovsky, H. S. Colburn, W. A. Yost, S. J. Guzman,
  "The precedence effect", _J. Acoust. Soc. Am._ 106(4), 1999.
- [Rasch 1979] R. A. Rasch, "Synchronization in performed ensemble music",
  _Acustica_ 43(2), 1979.
- [Tamaru 2019] H. Tamaru, Y. Saito, S. Takamichi, T. Koriyama, H. Saruwatari,
  "Generative moment matching network-based random modulation post-filter for
  DNN-based singing voice synthesis and neural double-tracking", _Proc. IEEE
  ICASSP_, 2019.
- [Varieschi 2010] G. U. Varieschi, C. M. Gower, "Intonation and compensation
  of fretted string instruments", _Am. J. Phys._ 78(1), 2010.
- [Wing 2014] A. M. Wing, S. Endo, T. Yates, A. Bradbury, "Perception of string
  quartet synchronization", _Frontiers in Psychology_ 5, 2014.
