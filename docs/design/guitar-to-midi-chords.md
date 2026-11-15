# Guitar to MIDI: chords from one pickup channel (feasibility study)

Status: study finished 2026-11-15. **Verdict: no-go for note-exact chords** from one ordinary
pickup channel with a deterministic method, against the bar fixed before the work began. What
could ship instead, with its numbers, is under [What would be shippable](#what-would-be-shippable).
Nothing in the product changed: Sedef MIDI stays monophonic
([guitar-to-midi.md](guitar-to-midi.md)); the code of this study lives in `tools/poly/` and is
not shipped.

## Why

The musician who uses Sedef MIDI wants to strum the guitar and play pads on a synthesiser. Chords
were a stated non-goal of the first design. Before anyone promises them, this study asks, with
mathematics and with measurement, whether a deterministic method can tell which notes of a chord
sound, from the one mixed channel an ordinary pickup delivers, well enough to ship.

**The bar, fixed before the work:** on sustained and strummed chords, note F ≥ 0.90, at most
1 ghost note per 20 chords, pick to note-on ≤ 80 ms at the median, and a real-time cost under
15 % of one core at 48 kHz.

A trained neural network is rejected for this project: it cannot be derived, and its training
data brings licence questions.

## The signal

Up to six strings sound at once. String *m*, struck at time *t_m*, is a sum of slightly
inharmonic partials

    f_mk = k · f0_m · sqrt (1 + B_m k²),    k = 1, 2, …

with amplitudes *a_mk* that decay slowly (seconds for the low partials, faster above 2 kHz), a
pick transient of a few milliseconds, and noise. The stiffness *B* is 3·10⁻⁵ to 1.5·10⁻⁴ on an
open string and grows as 2^(fret/6) as the string is shortened. The amplitudes are not free of
structure but not known either: a pluck at fraction *p* of the string gives sin (kπp)/k, and a
magnetic pickup at fraction *d* multiplies by sin (kπd), so whole families of partials (every
4th for a pickup a quarter of the way along) can be missing. One channel carries the sum:

    x(t) = Σ_m Σ_k a_mk(t − t_m) · cos (2π f_mk t + φ_mk) + transients + noise.

What is unknown: which *m* sound (the question), and for each its exact pitch (tuning, fretting:
± 12 cents), its *B*, and every amplitude and phase.

## Identifiability and time

### What can be seen of one note next to the others

Fix an observation window of *T* seconds. Let the partials of the other sounding strings span
the subspace *S* (each partial a cosine and a sine over the window: their amplitudes and phases
are free). For a note *m* with partial vector *s_m*, the others can explain the part of *s_m*
that lies in *S*; what is left to tell that *m* is there is

    η_m(T) = ‖P⊥_S s_m‖² / ‖s_m‖²,

the share of the note's energy outside the span of everything else. Two things follow.

**Against noise** (Cramér–Rao, known frequencies, unknown amplitudes): the least-squares
amplitude of a partial next to one other partial Δf away has variance σ²·2/(N·(1 − ρ²)), with
ρ = sin (πΔfT)/(πΔfT) their correlation over the window, so 1 − ρ² plays the part of η: the
energy that counts is η_m·E_m. A note at −30 dBFS over a −80 dBFS floor, seen for 1000 samples,
has E_m/σ² near 80 dB: even η = 0.001 would be detectable. **Noise is not the limit.**

**Against model error:** the other strings are not exactly what their templates say (the
stiffness and the tuning are only bounded, the amplitudes move during the window, close partials
beat). A single string's own template leaves 5 to 10 % of its strength unexplained (measured:
`sedef-poly-bench probe`, one E2 read as E2 plus 6–9 % of E3). A note is therefore seen when

    sqrt (η_m) · A_m  >  ε · A_others,    ε ≈ 0.05–0.10,

which for notes of equal strength in a six-note chord asks for **η ≥ about 0.1**. That threshold
is what the times below are read at.

### The time it takes

`tools/poly/identifiability.py` computes η from the model (partials under 4 kHz, amplitudes 1/k,
strings a few cents apart and with different *B*), and gives the window at which it first
reaches 0.1.

**Neighbours a semitone apart.** The fundamentals of E2 and F2 are 4.9 Hz apart, but partial *k*
is *k* times further, so the note is told from its neighbour by its upper partials long before
the fundamentals separate:

| note | f0 | η ≥ 0.10 | η ≥ 0.25 | η ≥ 0.50 |
|---|---|---|---|---|
| E1 | 41 Hz | 35 ms | 65 ms | 125 ms |
| E2 | 82 Hz | 20 ms | 35 ms | 65 ms |
| A2 | 110 Hz | 15 ms | 25 ms | 50 ms |
| D3 | 147 Hz | 10 ms | 20 ms | 40 ms |
| E4 | 330 Hz | 5 ms | 10 ms | 20 ms |

That is T ≈ 1.5/f0 for η = 0.1 and about 5/f0 for η = 0.5: a resolution law in periods of the
lowest note, as for the monophonic tracker.

**Common chord shapes.** The time until each note of the chord has η ≥ 0.1 next to the other
five:

| chord | notes seen within 35 ms | notes that take longer |
|---|---|---|
| E (E2 B2 E3 G♯3 B3 E4) | E2 25, B2 30, G♯3 35 | E4 100 ms, B3 250 ms, E3 750 ms |
| A minor (A2 E3 A3 C4 E4) | A2 20, E3 25, C4 25 | E4 155 ms, A3 250 ms |
| D (D3 A3 D4 F♯4) | D3 15, A3 15, F♯4 20 | D4 190 ms |
| G (G2 B2 D3 G3 B3 G4) | G2 30, B2 30, D3 30 | B3 170 ms, G4 250 ms, G3 500 ms |
| C (C3 E3 G3 C4 E4) | C3 20, E3 25, G3 25 | E4 130 ms, C4 185 ms |
| F barre (F2 C3 F3 A3 C4 F4) | F2 25, C3 30, A3 35 | F4 100 ms, C4 200 ms, F3 500 ms |
| E5 (E2 B2 E3) | E2 15, B2 20 | E3 300 ms |

**The latency floor splits the chord in two.** The root, the fifth above it and the third are
identifiable in 15 to 35 ms. Every note that is an octave or a twelfth above a lower string is
not, within any delay a player would accept: in the six open shapes above that is 2 or 3 of the
5 or 6 notes.

### The hard cases

**Octaves** (and twelfths, double octaves). Every partial *k* of the upper note lies on partial
2*k* (3*k*, 4*k*) of the lower. Three things remain observable:
- *The stiffness beat.* The two partials are not exactly equal: they differ by
  k³·f0·(4B₁ − B₂) for an octave, which with B = 10⁻⁴ is 0.02 Hz at k = 1, 0.7 Hz at k = 3 and
  5.3 Hz at k = 6 for E2 + E3. Resolving it takes 1/beat: 190 ms at the sixth partial, 1.5 s at
  the third. Too long, and it needs *B* known per string.
- *The envelope.* The lower note's even partials stand higher than one string would give. That
  is a prior on the spectral envelope, not an observation, and the pluck and pickup positions
  break it (a pickup a quarter of the way along removes every fourth partial; a pluck at the
  middle every second). η of 0.03 to 0.1 for hundreds of milliseconds says the same.
- *The strike.* If the upper string is struck later, the shared partials change at that moment.
  Two partials of amplitudes 1 and r with a random phase between them add to less than 1 when
  cos φ < −r/2: for equal amplitudes, a third of the shared partials go *down*. And within a
  strum the strings are 8 to 30 ms apart, less than the 1.5 periods of the lower note (18 ms at
  E2) that a spectrum needs to exist. In a strum the strike does not help; for a note added
  later it does, statistically.

**Fifths.** A fifth above (B2 over E2) shares only every second partial with every third of the
lower: η stays above 0.6, and it is seen in 15 to 30 ms. A *twelfth* above is the octave case.
The major third is where the root's 5th partial lies, which is why a bare fifth reads as a major
chord unless the third's strength is judged against the root's.

**Unisons across strings** (B3 fretted on the G string and the open B). They are one MIDI note,
so nothing has to be told apart; their partials beat at the few cents between the strings, and
the note-off logic must ride that beat out.

**A new note under ringing ones.** The same as the strike above: seen by what the spectrum gains
at the onset, in the partials the note does not share, in 15 to 35 ms; in the ones it shares,
not reliably.

## The estimators, on paper

**(a) Non-negative least squares on a dictionary of note templates.** The magnitude spectrum is
fitted as a non-negative sum of inharmonic note templates, with an l1 penalty (or a count limit)
so that the fewest notes explain it. It fits all notes jointly, so a partial is not counted
twice; it is convex, has no search and costs one small matrix solve per frame. It works on
magnitudes, so it cannot use the stiffness beat, and for coinciding partials the split between
two notes is decided by the template's envelope: a prior.

**(b) Iterative estimate-and-subtract with a harmonic sieve.** Find the strongest note, subtract
a smooth version of its partials, repeat. The same prior as (a) without the joint fit: an error
in the first note is carried into all the others, and a subtraction that takes the lower note's
even partials takes the octave with them.

**(c) Joint maximum likelihood over a small candidate set at each onset.** Fit the waveform with
the candidates' partials, frequencies and *B* free. It is the only one that could use the
stiffness beat and the phases, but that needs the 190 ms and more the beat takes, it is a
non-linear search with local minima in 12 dimensions for six strings, and for every window
under that it sees exactly what (a) sees.

**Chosen: (a), timed by onsets as (c) would be.** Within the delay a player accepts, the three
have the same information; (a) uses it jointly and cheaply. What was built
(`tools/poly/PolyEstimator.h`):

1. Decimate to 12 kHz (the partials that carry the decision lie under 4 kHz: above, amplitudes
   fall as 1/k and decay in tens of milliseconds, and the pick transient lives there).
2. A Hann window of *T* seconds every 5 ms; its peaks, placed by a parabola through the log
   magnitudes, are the **lines**. Each is drawn on a log-frequency axis (10 bins a semitone) as
   a bump whose width is the transform's resolution, 0.2/T. Fitting lines instead of the raw
   spectrum removes the window's skirts, which a non-negative fit otherwise explains with notes.
3. A template for each note from E2 to E6: bumps at k·f0·sqrt (1 + B k²), B = 7·10⁻⁵ (the
   middle of the range), each as wide as the line width, the tuning tolerance (± 12 cents) and
   what the unknown stiffness moves it (0.35·B·k² relative), weighted k^−0.5, on magnitudes
   raised to the power 0.5 (so the fit is not ruled by the two strongest partials).
4. Non-negative least squares with an l1 weight of 0.1 of the largest correlation (ε above), by
   coordinate descent on the normal equations, warm started from the last frame.
5. Onsets from the mean rise of the log magnitudes of a 10.7 ms transform against the largest
   each bin read in the last 40 ms (a beat's swell is not an onset; a new string shows in the
   bins it adds to however loud the others ring).

### Note logic

- **Onset-driven.** A strum is 2 to 6 onsets within 10 to 60 ms. It is decided once, *T* after
  its last onset (the window then holds every string it struck), at the latest *T* + 60 ms after
  its first.
- **A note starts** when its strength is at least 0.35 of the strongest note's in two frames
  running and 1.25 times what it was before the strum, once per strum, strongest first, six at
  most.
- **A note ends** when its strength stays under 0.10 of its own peak for 100 ms (longer than
  half a beat between two strings a few cents apart), when the level falls under the gate, or
  at a strum's decision if it is under 0.15 of the strongest then (the hand moved to another
  chord).
- **No pitch bend.** Velocity would come from the note's strength at its decision.

The latency parameter is *T*: the window, and the wait after an onset.

## The experiment

`sedef-poly-bench` (`tools/poly/sedef_poly_bench.cpp`, built with the tests, not shipped) runs
the estimator causally over a signal and scores its notes. Everything is deterministic: the same
input gives the same events.

### Data

- **Synthetic chords** from the repo's string models (`engine/tests/analysis/PitchTestSignals.h`,
  `engine/tests/midi/MidiTestSignals.h`): 16 open, barre and power-chord shapes on a guitar
  whose strings each have their own stiffness (3·10⁻⁵ to 1.5·10⁻⁴, growing with the fret) and
  are up to 4 cents off, picked or plucked with the fingers, on the bridge, neck and piezo
  models, at −80 dBFS noise. Three ways of playing, 20 chords each, three seeds: *sustained*
  (one strum, rings 1.2 to 1.8 s, damped), *strummed* (2 to 4 strums of each chord, down and
  up, 0.3 to 0.5 s apart, the up-strokes on 3 or 4 strings), *arpeggio* (one string at a time,
  up and back).
- **Real recordings:** the note-annotated set the "track" lane set up (360 excerpts of about
  30 s on an acoustic guitar with a pickup under each string, six players, openly licensed,
  CC BY 4.0; kept out of the repo). Its 180 *comping* excerpts are strummed and fingerpicked
  chords in five styles. **How it is mixed to one channel:** the set's own mono file, the six
  string pickups summed, is used as it is. That is what an under-saddle piezo delivers; it is
  not a magnetic pickup, whose spectrum differs (the synthetic bridge and neck models stand in
  for that). The annotation dates a note about 11 ms after its pick (the track lane's
  measurement), so the delays below are about 11 ms short on these recordings.
- **The musician's own chord recording** (local only, no annotations): 13 strummed chords.

### What is counted

- A played note is **answered** by the first unused sent note of its pitch whose note-on lies
  between 5 ms before its start and 250 ms after. Two strings at one pitch within 50 ms are one
  note. Precision, recall and F are over these.
- A sent note that answers nothing is a **ghost**; per 20 chords (a chord: two or more notes
  starting within 80 ms). *Again*: a ghost at the pitch of a played note that sounds (a second
  start). *Octave*: a ghost 12, 24 or 36 semitones from a played note that sounds.
- **Delay:** each answered note's start to its note-on (in a strum, the first string waits for
  the last).
- **What sounds** (snd): every 10 ms, the notes sounding in the output against the notes played
  *T* + 30 ms earlier; and the same folded to the twelve **pitch classes** (cls).
- **Cost:** processing time over audio time, one core of an Apple M2, the tool's own code (it
  allocates and is not tuned).

### Measured: synthetic chords (T = 80 ms)

`sedef-poly-bench synth --match 0.25`

| set | played | sent | P | R | F | ghosts per 20 chords | octave ghosts | delay median / p90 | what sounds F | pitch classes F |
|---|---|---|---|---|---|---|---|---|---|---|
| sustained, bridge | 278 | 264 | 0.913 | 0.867 | 0.889 | 7.7 | 13 | 95 / 135 ms | 0.876 | 0.971 |
| sustained, neck | 278 | 209 | 0.971 | 0.730 | 0.834 | 2.0 | 2 | 94 / 133 ms | 0.791 | 0.951 |
| sustained, piezo | 278 | 216 | 0.958 | 0.745 | 0.838 | 3.0 | 5 | 95 / 135 ms | 0.802 | 0.961 |
| **sustained** | 834 | 689 | 0.945 | 0.781 | **0.855** | **4.2** | 20 | 95 / 135 ms | 0.825 | 0.961 |
| strummed, bridge | 719 | 454 | 0.896 | 0.566 | 0.694 | 15.7 | 30 | 99 / 141 ms | 0.870 | 0.945 |
| strummed, neck | 719 | 342 | 0.950 | 0.452 | 0.613 | 5.7 | 1 | 102 / 142 ms | 0.797 | 0.935 |
| strummed, piezo | 719 | 352 | 0.938 | 0.459 | 0.616 | 7.3 | 5 | 105 / 144 ms | 0.803 | 0.924 |
| **strummed** | 2157 | 1148 | 0.925 | 0.492 | **0.643** | **9.6** | 36 | 102 / 142 ms | 0.825 | 0.935 |
| arpeggio | 1356 | 1553 | 0.645 | 0.739 | 0.689 | 61.2 | 185 | 82 / 85 ms | 0.858 | 0.954 |
| **sustained + strummed** | 2991 | 1837 | 0.932 | 0.573 | **0.710** | **6.9** | 56 | **99** / 140 ms | 0.825 | 0.950 |

- The notes that are missed are the ones the derivation says cannot be seen: the octaves and
  twelfths above a lower string (recall 0.73 to 0.87 on a single strum), and, in strummed
  changes, a string struck again while it still rings at the same level (recall 0.49: half of
  the played notes there are second and third strokes of a chord that is already sounding).
  "What sounds" does not count those strokes: 0.825.
- The pitch classes are right (0.95): the chord is recognised, its voicing is not.
- Arpeggios: every single-string onset opens a decision, and the ringing strings' octaves are
  started at it (299 second starts, 185 octave ghosts).

### Measured: delay against accuracy (synthetic, sustained + strummed)

`sedef-poly-bench sweep --match 0.25`

| T | P | R | F | ghosts per 20 | delay median / p90 | what sounds F | pitch classes F |
|---|---|---|---|---|---|---|---|
| 25 ms | 0.314 | 0.357 | 0.334 | 129.7 | 34 / 75 ms | 0.357 | 0.657 |
| 35 ms | 0.484 | 0.522 | 0.502 | 92.6 | 50 / 91 ms | 0.575 | 0.789 |
| 45 ms | 0.667 | 0.600 | 0.632 | 49.7 | 60 / 104 ms | 0.696 | 0.846 |
| 60 ms | 0.850 | 0.598 | 0.703 | 17.5 | 77 / 119 ms | 0.800 | 0.921 |
| 80 ms | 0.932 | 0.573 | 0.710 | 6.9 | 99 / 140 ms | 0.825 | 0.950 |
| 100 ms | 0.961 | 0.563 | 0.710 | 3.8 | 119 / 160 ms | 0.843 | 0.962 |
| 130 ms | 0.983 | 0.554 | 0.709 | 1.6 | 147 / 190 ms | 0.858 | 0.968 |
| 170 ms | 0.979 | 0.550 | 0.704 | 1.9 | 187 / 230 ms | 0.867 | 0.973 |

More delay buys precision (ghosts fall from 17.5 to 1.6 per 20 chords between 60 and 130 ms) and
no recall: the notes that are missed at 60 ms are missed at 170 ms, as the η table says (they
need 100 to 750 ms, and then a prior). No setting reaches F 0.90, and none reaches one ghost in
20 chords.

**The estimator alone, with the strum times given** (`sedef-poly-bench oracle`: the frame *T*
after each strum's last strike, the notes over a share of the strongest; best share per row),
before any note logic, two seeds: 30 ms 0.46, 45 ms 0.71, 60 ms 0.84, 80 ms 0.88, 100 ms 0.90, 130 ms 0.90,
170 ms 0.905, 250 ms 0.895. On synthetic strings, with perfect onsets, the frame-level F
saturates at 0.90.

### Measured: real recordings (180 comping excerpts, 5484 s, 45 568 notes; T = 80 ms)

`sedef-poly-bench set <comp manifest> --match 0.25`

| style | played | sent | P | R | F | ghosts per 20 chords | delay median / p90 | what sounds F | pitch classes F |
|---|---|---|---|---|---|---|---|---|---|
| bossa nova | 6657 | 7430 | 0.487 | 0.543 | 0.514 | 41.4 | 70 / 89 ms | 0.610 | 0.676 |
| funk | 10175 | 14426 | 0.426 | 0.603 | 0.499 | 55.2 | 78 / 107 ms | 0.628 | 0.703 |
| jazz | 6505 | 7998 | 0.479 | 0.589 | 0.528 | 44.6 | 74 / 97 ms | 0.633 | 0.706 |
| rock | 12312 | 7544 | 0.642 | 0.393 | 0.488 | 16.0 | 80 / 130 ms | 0.659 | 0.807 |
| singer-songwriter | 9919 | 7613 | 0.581 | 0.446 | 0.505 | 24.9 | 73 / 102 ms | 0.639 | 0.775 |
| **all** | 45568 | 45011 | 0.508 | 0.502 | **0.505** | **35.0** | **75** / 103 ms | 0.636 | 0.741 |

Against the delay (every third recording): T = 45 ms F 0.512 (ghosts 47.4 per 20, median 43 ms);
60 ms 0.526 (38.3, 56 ms); 80 ms 0.512 (37.2, 74 ms); 120 ms 0.481 (33.6, 112 ms); 170 ms 0.434
(37.1, 164 ms). On real comping a longer window is *worse*: strokes come every 150 to 300 ms and
the window then spans two chords.

Where the ghosts are (a sixth of the set, 3024 ghosts): 16 % start while no annotated note
sounds; of the rest the commonest are a semitone under a sounding note (256), a second start of
a sounding note (210), a fourth or a fifth under one (168, 167), a semitone over (154), an
octave under (92). They are not one error with one cure.

**The estimator alone on settled chords** (`sedef-poly-bench stable`: every 50 ms where two or
more annotated notes sound and none has started or stopped for T + 30 ms; 24 375 frames, 22 % of
the playing time). This is the most a note logic could be handed:

| T | notes P / R / F (share 0.12) | notes at share 0.30 | pitch classes P / R / F |
|---|---|---|---|
| 45 ms | 0.716 / 0.691 / 0.704 | | 0.819 / 0.836 / 0.827 |
| 80 ms | 0.811 / 0.720 / **0.763** | 0.900 / 0.596 / 0.717 | 0.941 / 0.868 / **0.903** |
| 170 ms | 0.817 / 0.726 / 0.769 | | 0.943 / 0.874 / 0.907 |
| 250 ms | 0.796 / 0.752 / 0.773 | | 0.938 / 0.897 / 0.917 |

Even with no onset to time, no change to follow and a quarter of a second to look, the notes of
a held real chord are told at F 0.77. Precision can be bought (0.90 at a share of 0.30) at a
recall of 0.60: four notes of a six-string chord in ten are not sent.

### Measured: the musician's chord recording

13 strummed chords, one every two seconds, each a major chord a semitone above the last (D, E♭,
E … C♯, D), peaks at −27 dBFS (gate set to −75 dB for it). No annotations, so nothing can be
scored; what came out (T = 80 ms):
- 67 notes for 13 chords, 3 to 7 a chord.
- In 12 of the 13 chords the root, the third and the fifth are all among the notes sent. The
  first chord, from silence, has its root and fifth, no third, and four notes that are not in a
  D chord (G♯2, C♯5, F3, F2).
- Notes outside the chord in the others: an F2 of 10 ms under the C chord, a C♯5 of 30 ms over
  the C♯ chord. Six in all, in 13 chords: about 9 per 20.
- 15 notes that belong to their chord end after less than 0.5 s although the chord rings for
  1.5 s (mostly octave doublings: the note below explains them once the strum settles).
- Which strings he struck cannot be told from here, so how many of the 67 are right is unknown.

### Cost

1.15 % of one core at 48 kHz for T = 80 ms, 1.65 % for 170 ms (Apple M2, blocks of 64 or 512, the
tool's untuned code: one 4096-point transform and a 49-note fit every 5 ms at 12 kHz). 0.95 % of
real time over the 5484 s of recordings at 44.1 kHz.

## Verdict

| bar | synthetic (sustained + strummed) | real comping | |
|---|---|---|---|
| note F ≥ 0.90 | 0.710 (sustained 0.855, strummed 0.643) | 0.505 | **fails** |
| ≤ 1 ghost per 20 chords | 6.9 | 35.0 | **fails** |
| pick to note-on ≤ 80 ms, median | 99 ms | 75 ms (+ about 11 ms of annotation lag) | fails / at the edge |
| < 15 % of one core | 1.15 % | 0.95 % | passes |

**No-go.** Not by a margin a better threshold closes: the derivation says 2 or 3 notes of a
common six-string chord are not identifiable inside any acceptable delay, the estimator alone
with perfect timing stops at 0.90 on synthetic strings and at 0.77 on settled real chords, and
the delay sweep shows the misses do not come back with time. A deterministic method on one
ordinary pickup channel cannot give note-exact chords to this bar; what the channel does carry
within 80 ms is the chord's **pitch classes**.

## What would be shippable

Measured on the real recordings' settled frames (T = 80 ms), where the played pitch classes are
exactly a major triad, a minor triad or a bare fifth (14 710 frames):

| reading | right |
|---|---|
| the root | 0.952 |
| root and quality (major / minor / fifth) | 0.849 (0.882 at T = 170 ms, 0.896 at 250 ms) |
| major against minor, when a third is read | 7370 of 7385 |
| a triad read as a bare fifth | 1211 of 8596 (14 %) |
| a bare fifth read as a triad | 294 of 5410 (5 %) |

The reading: fold the note strengths to pitch classes, take the triad whose three classes hold
the most against the rest, and call it a bare fifth when its third stands under 0.06 of the mean
of its root and fifth.

- **A "chord pad" mode: root plus quality, sent as a fixed voicing** (root, fifth, and the third
  when there is one, in one octave the player chooses). It is what a pad needs, it does not
  depend on the notes that cannot be seen, and its errors are of one kind (a third left out),
  which sounds bare, not wrong. As measured it is at 0.85 to 0.90, not yet at a 0.90 bar, and
  **only on settled frames**: how soon after a change the reading is right, and how often it
  flickers through a change, was not measured and decides whether it is playable. It would need
  its own design and its own bar (and seventh chords: the set's jazz and bossa excerpts are full
  of them, and they were not in the three kinds read here).
- **Pitch classes as notes** ("chord tones" in one octave): 0.90 on settled frames, but 0.74
  through the note logic on real comping, with 35 ghosts per 20 chords. Not shippable as built.
- **"Held chords" with a longer delay: no.** The delay does not buy the missing notes (0.763 at
  80 ms, 0.773 at 250 ms on settled real chords).

The study stops here, as its brief says.

## Tried and rejected

Synthetic figures are the frame-level F with the strum times given (`oracle`), T = 60 ms unless
said.

| tried | result | why it lost |
|---|---|---|
| fitting the raw magnitude spectrum, three free frequency bands per note | 0.45–0.62 bridge, 0.53–0.71 neck and piezo | a free envelope lets any note be its own harmonics: one E2 on the bridge model read as E2, E3, B3, E4, G♯4, B4 at 40 to 100 % |
| the raw magnitude spectrum, one band | 0.55–0.68 | the window's skirts are explained with notes |
| lines, magnitudes as they are, flat template | 0.755 | ruled by the two strongest partials |
| lines, power 0.5, template k^−0.5, l1 0.1 | **0.842** (kept) | |
| template k^−1 | 0.69–0.78 | the lower octave takes everything |
| l1 0.02 instead of 0.1 | 0.829 | more octave ghosts (137 against 84) |
| an odd-partial sieve (a candidate whose partials 3, 5, 7 are missing is an octave under) | real F 0.507 → 0.502; octave ghosts 549 → 396; what sounds, recall 0.587 → 0.529 | it removes real low notes as often as ghosts |
| onsets from the spectral rise relative to the level | re-strums not seen (0.14 against a threshold of 0.3 that the first strum's echo needs) | replaced by the mean rise of the log magnitudes |
| ending notes only by their own decay | real, what sounds: precision 0.59 | notes of the last chord rang on; ending them at the next strum's decision gives 0.70 |
| a higher threshold for a note (0.5, 0.7 of the strongest) | real F 0.479, 0.428 (from 0.503) | recall falls faster than ghosts |
| a longer window on real comping (120, 170 ms) | F 0.481, 0.434 | the window spans two chords |
| iterative estimate-and-subtract; joint maximum likelihood | not built | on paper: the same information inside the delay, without the joint fit or at a far higher cost |
| a trained neural network | not considered | not derivable; training data brings licence questions |

## Not verified

- **A magnetic pickup on real recordings.** The real set is a piezo under each string, summed.
  The bridge and neck figures are from the string model only.
- **The musician's recording** has no annotations: what came out is described, not scored.
- **The constants were not held out.** The thresholds were set while looking at the synthetic
  sets and at a sixth of the real set; the bare-fifth threshold (0.06) was the best of four on a
  third of the frames it is reported on. The real figures are therefore, if anything, flattering.
- **Root and quality as a stream of events.** Only settled frames were read.
- **Two of the 180 excerpts have annotations that are late by 0.3 and 0.4 s** (a known error of
  the set); they were left in.
- **The cost is of offline code.** It allocates; a real-time version was not written (and is not
  needed for a no-go). The headroom to the 15 % bar is a factor of 13.

## How to repeat it

    cmake --preset engine && cmake --build build/engine --target sedef-poly-bench -j4
    python3 tools/poly/identifiability.py                 # the η tables (numpy; about 5 minutes)
    build/engine/tools/sedef-poly-bench synth --match 0.25
    build/engine/tools/sedef-poly-bench sweep --match 0.25
    build/engine/tools/sedef-poly-bench oracle brief --window 0.08
    build/engine/tools/sedef-poly-bench cpu
    build/engine/tools/sedef-poly-bench set <manifest> --match 0.25      # "<wav> <notes>" per line
    build/engine/tools/sedef-poly-bench stable <manifest>
    build/engine/tools/sedef-poly-bench wav <recording.wav> [--truth notes.txt]

A notes file has one note per line: onset and offset in seconds and a MIDI pitch.
