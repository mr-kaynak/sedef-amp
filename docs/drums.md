# Drums

Status: design for 0.10.0. Owners: core maintainers. Code: `engine/include/sedef/dsp/drums/`,
`engine/src/dsp/drums/`, `engine/src/dsp/Metronome.cpp`.

Drum grooves to practise and write with, in the standalone app. The metronome's panel gets a
second sound, **Drums**: pick a groove and play along. The drums keep the click's tempo, meter,
start and stop, and they lock to the looper's bars. Everything is synthesised by Sedef: there are
no samples.

## 1. What a player can do

- Open the metronome, switch **Click** to **Drums** and press play. The groove starts on the "1",
  at the tempo and in the meter the metronome shows.
- Pick a **groove**. Each one has a name and a one-line feel ("Eighths on the hat, backbeat on 2
  and 4"). The list shows the grooves written for the current meter; choosing another meter
  shows its grooves and picks the first.
- **Intensity** (1 to 5) plays fewer or more notes: 1 is the bare pattern, 5 is the busiest, with
  ghost notes, more kick and a ride or crash where the groove calls for it. Higher levels are also
  played a little harder (section 4.3).
- **Fills**: Off, or a fill on the last bar of every 2, 4 or 8 bars, with a crash on the "1"
  after it.
- **Level** sets how loud the drums are against the guitar; **Tone** makes the kit darker or
  brighter.
- Tempo, tap tempo and the meter are the metronome's. The play button is the metronome's: in
  Drums mode it starts and stops the drums.
- With the looper (docs/looper.md): while the drums run, a loop snaps to their bars exactly as it
  does to the click's, the count-in is a bar of the groove, and once a loop plays the drums stay
  locked to its passes.

## 2. Where it lives

**The standalone app only**, like the metronome and the looper: a DAW has its own drums,
tempo and transport, and a plug-in's drums would ignore them.

**After the looper, before the output gain**, at the metronome's place in the chain:

```
… ─► Fathom ─► Looper ─► Metronome: click or drums ─► Output gain ─► Out
```

- **Never into the amp or the cab**: the drums are added to the finished stereo signal.
- **Never into a loop**: the looper records before them, so a loop holds the guitar only and
  the drums keep playing live, locked to it. Recording them too would double them on every pass.
- **Output gain stays the master volume**, and the tuner's mute silences the drums too.

## 3. Timing

### 3.1 The grid is the metronome's

The drums are not a second clock. They are a second sound on the metronome's own scheduler
(`Metronome.cpp`): steps fall on `t_k = ceil(A + k · I)`, with `A` the anchor sample and `I` the
step length in samples (60/bpm s for x/4 meters, 30/bpm s for x/8 meters, which count eighths).
Everything the metronome does to that grid applies to the drums unchanged: starting on the
first sample of the next block, re-anchoring on a tempo change, a new bar when the meter gets
shorter, and the looper's lock. A groove's "1" therefore **is** the sample on which the click
would sound its step 0, which is the sample the looper calls a downbeat.

### 3.2 Inside a step

A groove is written on a grid of **slots**: `k` slots per step (4 for sixteenths over quarter
steps, 3 for triplets over quarter steps, 2 for sixteenths over eighth steps, 1 for 12/8's
eighths). When step `k` sounds at `t_k`, its slots are placed in that step's own length in
samples, `L = t_{k+1} − t_k` (the next step's time from the same formula), by integer
arithmetic:

```
slot j of the step (0 ≤ j < k):   t = t_k + floor((2·j·L + k) / (2·k))      (round half up)
```

So slot 0 is the step itself, to the sample, and every slot is within half a sample of its exact
position on the step. Integer division rounds the same on every platform, so the event times of a
groove depend on nothing but the step times.

### 3.3 Micro-timing

Some lanes are played a few milliseconds late on purpose (a relaxed backbeat). An offset `m` ms
moves an event by `round(m · fs / 1000)` samples. Two rules keep it inside its step:

- Offsets are **never negative on slot 0**: nothing sounds before its step, so the "1" is always
  the first sound of a bar (the looper starts a take there).
- An event never moves past `t_{k+1} − 1`. With offsets of at most 5 ms and steps of at least
  62.5 ms (an eighth at 240 bpm, the tempo knob's top), this clamp never binds; it is a guard.

Why at most 5 ms: about 20 ms between two onsets are needed to tell which came first (Hirsh,
1959). 3 to 5 ms shapes the feel without being heard as a late note.

### 3.4 Events per step

When a step sounds, the drums queue the events of that step (at most 64) with their sample times.
The scheduler's loop renders up to the earliest of the next step and the next queued event, fires
it on its sample, and goes on, exactly as it already does for steps. A step that sounds discards
anything left unplayed from the step before; that only happens when the grid jumps (a tempo
change made a step overdue, the looper snapping the bar), never in steady playing (above).

Events on the same sample fire in the order the groove lists them; the step sounds first.

### 3.5 Bars, changes and the transport

- **Start**: the metronome's rule. The first bar starts on the first sample of the next block.
- **Bar line**: when step 0 sounds, a new bar begins. The groove, intensity and fill setting
  chosen since the last bar line apply here, so a change never cuts a bar in half. Bars are
  counted from the start (the first bar is bar 0).
- **Switching Click to Drums while running**: the drums come in on the next step, in the middle
  of the current bar if need be. Drums to Click: the click comes in on the next step; the kit
  rings out.
- **Tempo** changes act at once (the metronome's rule); **meter** changes too. If the meter gets
  longer in the middle of a bar, its extra steps are silent until the next bar line, where the new
  meter's groove starts.
- **Stop**: no new events. Ringing drums and cymbals ring out, as the click's last click does.
- **A groove whose meter is not the metronome's** plays the first groove of the metronome's meter.
  The UI keeps them matched; the rule only makes every combination well defined.

### 3.6 The looper

The looper reads downbeats from `Metronome::downbeats()`, which runs the scheduler forward on a
copy of its state (docs/looper.md, section 5). The drums do not change when steps sound, so the
looper's downbeats and the drums' bar lines are the same samples by construction:

- **Bars**: a take starts on the "1" of a drum bar and closes on one; `L` is the grid distance.
- **Count-in**: the looper waits one more downbeat; the drums play that bar as a groove bar.
- **Lock**: each wrap re-anchors the grid on the wrap sample, so the bar line moves with it (by at
  most a sample per pass while the tempo is the loop's). When the drums were started in the middle
  of a loop, the first wrap starts a new bar there. Of the looper's rule that a step sounding less
  than half a step before the wrap counts as the downbeat (so the click never doubles), the drums
  keep only the case where that step was step 0; otherwise the drums' "1" plays on the wrap.

## 4. Grooves

### 4.1 Data

A groove is data in `engine/src/dsp/drums/Grooves.cpp`: a name, a one-line feel, a meter, slots
per step, a fill family, and lanes. A lane is one instrument, a pattern string with one character
per slot of the bar, the intensities it plays at (`lo`–`hi`, 1 to 5) and an optional offset in ms:

| Character | Velocity | Amplitude (section 5.3) |
|---|---|---|
| `.` | rest | |
| `g` | 40, ghost | −15.0 dB |
| `o` | 72, soft | −7.4 dB |
| `x` | 100, normal | −3.1 dB |
| `X` | 124, accent | −0.3 dB |

The four levels are what a drummer's notation distinguishes (ghost, soft, normal, accent); the
amplitudes follow from section 5.3.

Instruments: `K` kick, `S` snare, `H` closed hat, `O` open hat, `R` ride, `C` crash, `T1` high tom,
`T2` mid tom, `T3` floor tom.

### 4.2 Building a bar

At each bar line the drums build the bar's event list (at most 192 events, fixed storage):

1. Every lane whose range holds the intensity contributes its non-rest slots.
2. **Fill bar** (fills every `N` bars and `bar mod N = N − 1`): the groove's events in the last
   `F` slots are dropped and the fill's events take their place. `F` is the fill family's long
   length at intensity 3 and above, its short length at 1 and 2. Fills alternate between the
   family's two variants (A on the first fill, B on the second, and so on).
3. **After a fill** (`bar mod N = 0`, bar > 0): a crash and a kick at accent on slot 0; a hat or
   ride on slot 0 is dropped (the crash takes its place, as a drummer's hand does).
4. Events are ordered by slot, then by lane order.

### 4.3 Intensity

One control, five levels. Lanes carry their own ranges, so each groove decides what "more" means
(more kick, sixteenths, ghost notes, ride for hat). Higher levels are also played harder: every
velocity is scaled by `0.84, 0.88, 0.92, 0.96, 1.0` for levels 1 to 5 and rounded, which is 2.3 dB
and, through the contact time (section 5.3), a little brightness between 1 and 5. Default: 3.

### 4.4 The grooves

Original patterns written for this kit from common drumming idioms (backbeat, shuffle, the ride
cymbal's swing pattern, odd meters grouped in twos and threes). None is transcribed from a
record.

| # | Name | Meter, slots | Feel |
|---|---|---|---|
| 0 | Straight rock | 4/4, 16ths | Eighths on the hat, backbeat on 2 and 4 |
| 1 | Half-time | 4/4, 16ths | Snare on 3: slow and heavy over a steady pulse |
| 2 | Ballad | 4/4, 16ths | Soft and open, room for a slow song |
| 3 | Fast eighths | 4/4, 16ths | Kick and snare trade eighths, fast and loud |
| 4 | Shuffle | 4/4, triplets | Triplet bounce on the hat, backbeat on 2 and 4 |
| 5 | Swing | 4/4, triplets | Swing time on the ride, hat on 2 and 4 |
| 6 | Waltz | 3/4, 16ths | One-two-three, kick on one |
| 7 | Two-step | 2/4, 16ths | Bass on one, snare on two: a quick march |
| 8 | Five | 5/4, 16ths | Three plus two, kick on the first of each group |
| 9 | Six-eight | 6/8, 16ths | Two rolling groups of three, snare on four |
| 10 | Seven | 7/8, 16ths | Grouped two, two, three |
| 11 | Nine | 9/8, 16ths | Grouped two, two, two, three |
| 12 | Slow twelve-eight | 12/8, eighths | Slow triplets with a heavy backbeat |

Lanes (bar shown as slots; a space every step for reading only, not in the data):

```
0 Straight rock      H   X... x... X... x...   1        R   X.x. X.x. X.x. X.x.   5
                     H   X.x. X.x. X.x. X.x.   2-4      S   .... X... .... X...   1-5  +3 ms
                     S   .... ...g .g.. ....   4-5      K   X... .... X... ....   1-2
                     K   X... .... X.x. ....   3        K   X... ..x. X.x. ....   4-5
1 Half-time          H   X... x... X... x...   1        H   X.x. X.x. X.x. X.x.   2-5
                     O   .... .... .... ..o.   4-5      S   .... .... X... ....   1-5  +5 ms
                     S   .... .... ...g ..g.   4-5      K   X... .... .... ....   1
                     K   X... ..x. .... ....   2-3      K   X... ..x. ..x. ..x.   4-5
2 Ballad             H   o... o... o... o...   1-2      H   o.g. o.g. o.g. o.g.   3-4
                     R   o.g. o.g. o.g. o.g.   5        S   .... o... .... o...   1-2  +5 ms
                     S   .... x... .... x...   3-5  +5 ms
                     K   x... .... x... ....   1-3      K   x... ..g. x... ....   4-5
3 Fast eighths       H   x.x. x.x. x.x. x.x.   1-3      R   X.x. X.x. X.x. X.x.   4-5
                     C   X... .... .... ....   5        K   x... .... x... ....   1
                     S   .... x... .... x...   1        K   x... x... x... x...   2-5
                     S   ..x. ..x. ..x. ..x.   2-5
4 Shuffle            H   x.. x.. x.. x..       1        H   X.x x.x X.x x.x       2-4
                     R   X.x x.x X.x x.x       5        S   ... X.. ... X..       1-5  +3 ms
                     S   ... ... ..g ...       4-5      K   x.. ... x.. ...       1-3
                     K   x.. ... x.. ..x       4-5
5 Swing              R   x.. x.. x.. x..       1        R   X.. x.x X.. x.x       2-5
                     H   ... o.. ... o..       1-5      K   g.. g.. g.. g..       3-5
                     S   ... ... ..g ..g       4-5
6 Waltz              H   x... x... x...        1-2      H   x.x. x.x. x.x.        3-5
                     K   X... .... ....        1-5      S   .... x... x...        1-5  +3 ms
                     K   .... .... ..g.        4-5
7 Two-step           H   x... x...             1        H   x.x. x.x.             2-5
                     K   X... ....             1-3      K   X... ..x.             4-5
                     S   .... x...             1-5  +3 ms
8 Five               H   x... x... x... x... x...   1   H   x.x. x.x. x.x. x.x. x.x.   2-5
                     K   X... .... .... x... ....   1-5 S   .... .... x... .... x...   1-5
                     K   .... ..g. .... .... ....   4-5
9 Six-eight          H   x... .. x... ..       1        H   x.x. x. x.x. x.        2-5
                     K   X... .. .... ..       1-5      S   .... .. X... ..        1-5
                     K   .... .. .... x.       3-5
10 Seven             H   x... x... x... ..     1        H   x.x. x.x. x.x. x.      2-5
                     K   X... .... x... ..     1-5      S   .... x... .... x.      1-5
11 Nine              H   x... x... x... x... ..   1     H   x.x. x.x. x.x. x.x. x.   2-5
                     K   X... .... x... .... ..   1-5   S   .... x... .... x... ..   1-5
                     S   .... .... .... .... g.   4-5
12 Slow twelve-eight H   X.. X.. X.. X..       1        H   Xxx Xxx Xxx Xxx       2-5
                     K   X.. ... x.. ...       1-5      S   ... X.. ... X..       1-5  +5 ms
                     K   ... ... ... .x.       4-5
```

(In the x/8 meters the steps are eighths: two slots each, written in groups by the meter's
grouping.)

### 4.5 Fills

A fill family is written on a slot grid and fills the last `F` slots of a bar. Variants A and B
alternate. Long length at intensity 3–5, short at 1–2:

| Family | Grooves | Long | Short |
|---|---|---|---|
| Sixteenths | 0–3, 6, 7, 8 (16ths over quarter steps) | 8 slots (two beats) | 4 slots (one beat) |
| Triplets | 4, 5, 12 | 6 slots (two beats) | 3 slots (one beat) |
| Eighth meters | 9, 10, 11 (16ths over eighth steps) | 6 slots (three eighths) | 4 slots (two eighths) |

```
Sixteenths A   S   XxXx .... |  K   x... x...         (long; short = the last four slots)
               T1  .... xx.. |  T3  .... ..xX
Sixteenths B   S   x.xx .... |  K   x... x...
               T2  .... x.x. |  T3  .... .x.X
Triplets A     S   xxx ...   |  K   x.. x..
               T1  ... x..   |  T2  ... .x.   |  T3  ... ..X
Triplets B     S   x.x ...   |  K   x.. x..
               T1  ... xx.   |  T3  ... ..X
Eighth meters A  S  xx.. ..  |  K   x... x.
                 T1 ..xx ..  |  T3  .... xX
Eighth meters B  S  x.x. ..  |  K   x... x.
                 T2 ...x x.  |  T3  .... .X
```

## 5. The kit

### 5.1 Integer synthesis

Every drum is synthesised in **integer arithmetic**. Floating-point results differ in their last
bits between the platforms Sedef builds on: Windows compiles with `/fp:fast` (free to reorder
sums), Apple's arm64 compiler fuses multiply-adds, and the maths libraries round `exp` and `cos`
differently. The repo's MIDI tests already have to allow a 48-sample slack off Apple silicon for
that reason. A drum render in floats could only be pinned per platform. In integers, a groove
renders the **same bits on every platform**, so a hash of a few bars pins the whole kit and every
groove in CI on all three systems.

- Output: Q23 in `int32` (1.0 = 2²³); every instrument's output is bounded (5.6), so sums never
  overflow.
- Modes are kept in Q35 and envelopes in Q38, 12 and 15 bits under the output's LSB, with
  coefficients in Q26 and Q24, products in `int64`. The extra bits matter: with Q23 state, a mode
  that loses less than an LSB per sample (`(1 − r)·A < 1`) stops decaying and circles in a limit
  cycle. A closed hat built that way rang on at about −85 dBFS for seconds. At Q35 the same stall
  would sit far under the output's LSB, and the mode update rounds **toward zero** (magnitude
  truncation), so a decaying mode's norm strictly falls and reaches zero.
- Filters are first-order bilinear low- and high-passes with pre-warped cut-offs (Q30), also
  rounded toward zero, so with no input their state reaches zero too.
- Constants (frequencies, decays, levels) are computed in `double` when the kit is prepared and
  rounded to integers once. Those are the only floating-point values the kit uses; a last-bit
  difference in `exp` there would have to fall within 10⁻¹⁶ of a rounding boundary to change an
  integer, and the CI hashes check it on every platform.
- Noise: `sedef::Random` (xorshift32, integer), one generator per instrument, seeded with a
  constant at every transport start. A groove therefore sounds the same each time it starts.
  White noise spreads its power from 0 to `fs/2`, so its amplitude is scaled by `√(fs / 48 kHz)`:
  its level in the audio band is then the same at every rate.
- After the kit, Level and Tone run in `float` (5.7); the pinned renders are taken at 0 dB and a
  centred Tone, where both are exact (gain 1.0, filter bypassed).

### 5.2 One modal bank per instrument

A drum head and a cymbal are, to first order, linear: striking one twice gives the sum of two
strikes. So each instrument is **one bank of modes** that every strike adds to, not a voice per
hit:

- A mode is a decaying phasor `z = x + i·y`, advanced each sample by `z ← r·e^{iω}·z` (the
  coupled form: `x' = c·x − s·y`, `y' = s·x + c·y`, `c = r cos ω`, `s = r sin ω`). Its output is
  `x`.
- A strike adds its amplitude to `x` of every mode on the strike's sample, so the first sample of
  a hit is non-zero and a hit is heard on exactly its sample (cosine phase, as the click's partials).
- A re-strike adds to what still rings, in phase or against it, as on a real head. There is
  nothing to steal: an instrument costs one bank however dense the playing, and a crash or ride
  keeps its wash under the next stroke instead of being cut.
- `r = exp(−1/(τ·fs))`. Modes of one instrument have the same Q, `Q = π f τ` (losses proportional
  to frequency, the simplest damping law), so higher modes die faster.
- **Bound**: a strike may not raise a mode's amplitude above twice a single strike at the top
  velocity. `|z|` is computed as an integer square root and `z` scaled back if needed. It only
  binds in rolls on long-ringing modes; it is what keeps the output bounded (5.6).
- A bank whose modes and noises have all fallen below 2 LSB (−132 dBFS) stops and is zeroed:
  silence costs nothing.

### 5.3 Strikes

**Velocity to amplitude.** A pattern velocity `v` (1–127) becomes an impact strength
`u = (v/127)^1.5`. The head's response is linear in the impact speed, so amplitude is `∝ u`; the
exponent 1.5 maps the four written levels to −15, −7.4, −3.1 and −0.3 dB, so a ghost note sits
well under the backbeat.

**Contact time.** A stick or beater touches the head for `t_c` and pushes it with a pulse that is
close to a half sine. Its spectrum, relative to its level at 0 Hz, is

```
C(f) = |cos(π f t_c)| / |1 − (2 f t_c)²|          (π/4 at 2 f t_c = 1; first zero at 1.5 / t_c)
```

so a short contact excites high modes, a long one only the low ones. For elastic (Hertzian)
contact `t_c ∝ (impact speed)^(−1/5)`, so `t_c = t_ref · u^(−1/5)`: harder strokes are brighter, as
on a real drum. Each mode's strike amplitude is `u · w_m · C(f_m)`, scaled by the instrument's
level (5.5); the table for all 128 velocities is built when the kit is prepared.

**Where the head is struck.** For a membrane of radius `a`, mode (m, n) has the shape
`J_m(j_mn ρ / a) cos(mθ)` with `j_mn` the n-th zero of the Bessel function `J_m`. Its excitation
by a strike at radius `ρ₀` is that shape at the strike point, normalised to unit energy:
`w_mn = J_m(j_mn ρ₀/a) / ‖J_m(j_mn ρ/a)‖`. Modal ratios are the ideal membrane's
`j_mn / j_01`: 1, 1.593, 2.136, 2.295, 2.653, 2.917.

**Tension modulation (pitch glide).** A large displacement stretches the head and raises its
tension, so a drum starts sharp and falls to its pitch as it decays. The raised tension goes with
the square of the displacement, and all modes scale with `√T` together:

```
ω_m(t) = ω_m · (1 + g(t)),    g = min(g_max, κ · e₁(t)²)
```

with `e₁` the fundamental's amplitude, tracked exactly as the bank decays and re-computed at each
strike, and `κ` set so a single top-velocity strike starts at `g_max`. Because `e₁² ∝ r^(2n)`, the
glide falls twice as fast as the tone. The extra angle `δ = ω_m · g` per sample is applied as a
second rotation with `cos δ ≈ 1 − δ²/2`, `sin δ ≈ δ − δ³/6` (δ < 0.01 rad from 44.1 kHz up: the
next terms are under 10⁻⁹, the Q30 step).

### 5.4 The instruments

Membranes: polyester film 0.25 mm thick, `σ = 1390 kg/m³ × 0.25 mm = 0.3475 kg/m²`, and
`f₀₁ = j₀₁ √(T/σ) / (2π a)`.

| | Kick 22" | Snare 14" | Toms 10", 12", 16" |
|---|---|---|---|
| Tension `T` | 600 N/m, a low, slack tuning | 3000 N/m, a tight snare | 800 N/m, all three: equal tension tunes `f ∝ 1/a` (a fourth and a sixth up from the floor) |
| `f₀₁` | 56.9 Hz | 200.0 Hz | 144.6, 120.5, 90.4 Hz |
| Modes | (0,1), (1,1), (2,1) | (0,1), (1,1), (2,1), (0,2), (3,1), (1,2) | (0,1), (1,1), (2,1) |
| Struck at `ρ₀/a` | 0.15 (a beater near the centre) | 0.3 | 0.3 |
| `w` | 1.86, 0.97, 0.29 | 1.68, 1.70, 1.01, 1.24, 0.55, 2.68 | 1.68, 1.70, 1.01 |
| Q (τ of `f₀₁`) | 10 (56 ms): a pillow-damped kick, gone in about 0.4 s | 30 (48 ms): the wires damp the head | 80 (176, 211, 282 ms): open toms ring |
| `t_ref` | 2.0 ms, felt beater | 0.7 ms, wood stick | 0.8 ms |
| `g_max` | 0.45: a slack head glides most | 0 | 0.12 |
| Noise | beater click | wires; stick click | stick click |

**Snare wires.** The wires rattle while the head under them moves far enough to throw them off;
below that they rest on it and fall silent. So their noise is driven by the head's fundamental
above a threshold: `drive = max(0, e₁ − θ)`, `θ` 30 dB under a top-velocity strike. A soft stroke
therefore buzzes less than in proportion and its buzz ends sooner, as on a real snare: for 3.4 τ
after a top stroke (0.16 s) and 1.7 τ after a ghost note (0.08 s). The wire noise is band-limited
by a first-order high-pass at 1.8 kHz and low-pass at 9 kHz.

**Clicks.** The stick or beater's own impact: a burst of noise with a 1 ms decay (kick 1.5 ms),
amplitude `∝ u`, through a first-order low-pass at `0.5 / t_c` (brighter for harder strokes) and
a first-order high-pass at 1 kHz.

**Cymbals.** A cymbal's modes are many and inharmonic; above a few kHz they blur into a wash. Each
cymbal has a bank of partials plus a noise wash:

- Partials spaced geometrically from `f_lo` to `f_hi`, rounded to 10 Hz, each moved up in 10 Hz
  steps until it lies at least 2 % from every ratio `p/q` (p, q ≤ 6) to each lower partial, so no
  two read as a harmonic pair. The rule gives:
  - hats: 3200, 4080, 5210, 6650, 8480, 10820 Hz
  - ride: 420, 650, 930, 1430, 2190, 3100, 4750, 6900 Hz
  - crash: 330, 520, 890, 1370, 2130, 3270, 5180, 8810 Hz
- Partial weights rise as `√(f / f_lo)`: a thin plate radiates its higher bending modes better
  below its coincidence frequency, which for 1 mm bronze is about 18 kHz (Fahy).
- Stick contact on metal is short, `t_ref = 0.15 ms` (first zero of `C` at 10 kHz).
- The wash is noise whose envelope adds energy at each strike (`n ← √(n² + a²)`, independent
  bursts add in power) and decays on its own, through a high-pass and a low-pass.

| | Hats | Ride | Crash |
|---|---|---|---|
| Partials' Q | open 4700 (τ 0.47 s at 3.2 kHz), closed 310 (τ 31 ms) | 2500 (τ 1.9 s at 420 Hz, 0.12 s at 6.9 kHz) | 2000 (τ 1.9 s at 330 Hz) |
| Wash τ | open 0.28 s, closed 20 ms | 1.6 s | 1.2 s, rising over 12 ms |
| Wash band | 6–14 kHz (two high-pass poles) | 3–10 kHz | 2–12 kHz |

The crash's wash rises over 12 ms because a cymbal's energy moves from the struck low modes to
high ones over the first tens of milliseconds (its "bloom").

**Open and closed hats** are one instrument: the two cymbals either ring free or are clamped. A
closed-hat stroke sets the closed damping and excites the bank; an open-hat stroke sets the open
damping. A closed stroke after an open one therefore chokes it, as closing the pedal does: the
partials fall with τ 9–31 ms and the wash with 20 ms instead of ringing on for seconds (the tests
ask for at least 25 dB under the open ring 150 ms after the closed stroke).

### 5.5 Levels and stereo

Each instrument's strike amplitudes are scaled so a single top-velocity strike peaks at a set
level: its modes start in cosine phase, so the tonal peak is the sum of the mode amplitudes, and
the noise parts are bounded separately (5.6).

| | Kick | Snare | Toms | Hats | Ride | Crash |
|---|---|---|---|---|---|---|
| Tone peak | 0.40 | 0.20 | 0.32 | 0.05 | 0.07 | 0.08 |
| Noise peak | 0.06 | 0.26 (wires), 0.05 (click) | 0.04 | 0.12 | 0.05 | 0.14 |
| Pan (−1 left, +1 right) | 0 | 0 | −0.3, 0, +0.4 | −0.35 | +0.45 | −0.3 |

Kick and snare carry the groove and are the loudest; cymbals sit lower, as in a kit mixed for
practice. The pan is the drummer's view (hats left, ride and floor tom right), at most 0.45 so
the kit stays around the guitar, which is in the centre. Pan gains are constant power:
`gL = cos((p + 1)π/4)`, `gR = sin((p + 1)π/4)`.

All these levels are then raised together by the **kit level**, 1.9 (+5.6 dB), set once from a
measurement: at Level 0 dB a groove at intensity 3 reads −22.2 to −24.7 LUFS (median −23.3), about
7 dB under the factory presets' −16 LUFS, so the guitar stays on top. Its true peak is −1.9 to
−2.7 dBFS (16 bars with fills, 120 bpm). No groove reaches 0 dBFS at Level 0 dB even at intensity
5, 240 bpm and fills every 2 bars (a test). At intensity 1 the grooves read −23.8 to −26.6 LUFS,
at 5 −18.1 to −23.4 LUFS.

Measured single strokes at 48 kHz (peak per channel, velocity 124):

| | Kick | Snare | Closed hat | Open hat | Ride | Crash | Toms (high, mid, floor) |
|---|---|---|---|---|---|---|---|
| Left / right dBFS | −5.7 / −5.7 | −7.9 / −7.9 | −18.1 / −23.2 | −15.6 / −20.7 | −25.0 / −18.3 | −11.4 / −15.6 | −6.0 / −10.3, −7.6 / −7.6, −11.5 / −5.6 |
| Falls 60 dB in | 0.36 s | 0.23 s | 0.17 s | 2.4 s | over 4 s | over 4 s | 1.05, 1.26, 1.68 s |

### 5.6 Bounds

- A mode never exceeds `2·E_m` (its top-velocity strike, 5.2), so a bank's tone never exceeds
  twice its tone peak.
- Noise is uniform in ±1 times an envelope that is capped at twice its single-strike level. A
  filter raises a peak by at most the sum of the magnitudes of its impulse response (computed for
  each filter when the kit is prepared), so a noise part's output is at most twice its level times
  the product of those sums.
- The kit's output is bounded by the sum of those, panned. The tests check every render against it
  and that it never comes near the `int32` range.

### 5.7 Level and Tone

After the kit, in `float`:

- **Level** −30 … +6 dB, default 0 dB, smoothed over 20 ms.
- **Tone** 0 … 10 (dark to bright), default 5: a first-order tilt, a high shelf at 1 kHz of
  `24 · (tone − 0.5)` dB with half of that taken off as broadband gain, so 10 lifts the highs 6 dB
  and lowers the lows 6 dB, and 0 does the reverse. At 5 the filter is bypassed (exact).

## 6. Real time ([ADR-0005](adr/0005-real-time-safety.md))

- Banks, event queue and bar storage are fixed arrays in the metronome; nothing is allocated after
  `prepare()`. The velocity tables are built in `prepare()`.
- Settings come through parameters, read once per block.
- Cost: only sounding banks run. Every mode is four 64-bit multiplies a sample, eight with the
  glide; the whole kit ringing at once is 40 modes. The busiest groove (Fast eighths, intensity 5,
  fills every 2 bars, 200 bpm) takes 0.2–0.6 % of one core of an Apple M2 (measured with the
  desktop's usual load, `sedef_engine_tests "[drums-probe]"`).

## 7. Parameters ([ADR-0004](adr/0004-single-parameter-schema.md))

Appended to `ParameterList.inc`, not automatable and global like the metronome's: presets never
store or load them, and they stay with the session.

| Id | Values | Default |
|---|---|---|
| `metronomeMode` | Click, Drums | Click |
| `drumsGroove` | the groove names, in the order of 4.4 | Straight rock |
| `drumsIntensity` | 1 … 5 | 3 |
| `drumsFills` | Off, Every 2 bars, Every 4 bars, Every 8 bars | Every 4 bars |
| `drumsLevel` | −30 … +6 dB | 0 dB |
| `drumsTone` | 0 … 10 | 5 |

New grooves are appended to the end of the list, so a saved session keeps its groove. The groove
table (name, feel, meter) is exported by `sedef-params --grooves` to `ui/src/generated/grooves.json`
and checked by the build like the parameter schema.

## 8. UI

The metronome panel (`ui/src/features/metronome/`):

- Under the tempo row, a **Click / Drums** switch (a radio group).
- **Click** shows what it shows today: the beat grid, Sound and Volume.
- **Drums** shows the groove list for the current meter (each row: name and feel; a radio group
  with one tab stop, arrows, Home and End), **Intensity** 1–5 ("Fewer notes" to "More notes"),
  **Fills** (Off, 2, 4, 8 bars), and Level and Tone knobs. The live beat stays visible as a row of
  lamps.
- Signature, tempo, tap and the play button are shared. Rows and segments are at least 32 px high
  and as wide as the panel allows; every control has a name and a visible focus ring; under
  reduced motion nothing moves.

## 9. Tests

Engine (`engine/tests/drums/`):

- **Timing to the sample**: every event of every groove at every intensity, over several bars, at
  80, 120 and 200 bpm and in every meter, equals its formula (3.2, 3.3) from the metronome's own
  step times, and slot 0 equals the click's step.
- **Block-size invariance**: renders at blocks 1, 7, 64, 333, 4096 and random odd sizes are
  bit-identical. **Sample rates** 44.1, 48, 88.2 and 96 kHz: the same events within half a sample
  of the same times in seconds, and per-instrument peak and decay within set bounds.
- **Pinned renders**: an FNV-1a hash of four bars of every groove (with fills every 2 bars, so
  both variants and the crash play) at 120 bpm and 48 kHz, equal on every platform.
- **Bounds**: hammering every instrument at the densest pattern, peaks stay under 5.6's bound;
  banks stop once silent; the event queue and bar storage never overflow for any groove,
  intensity and fill setting.
- **Data**: every groove's lanes fit its bar, slot 0 offsets are never negative, every meter has a
  groove, names match the parameter's choices, partial sets keep the 2 % rule.
- **Physics**: the kick's pitch starts `g_max` sharp and falls at twice the tone's rate; a closed
  hat chokes an open one within 30 ms; a soft snare stroke's buzz ends before a hard one's.
- **Real time**: no allocation in `process()` with the drums running, at every setting.
- **Engine**: drums are not in a loop the looper records; the looper's Bars take starts on a drum
  bar line; the lock holds after many passes; the plug-in (metronome not allowed) is unchanged.
- **Factory presets** render byte-identical to main with the drums off (`sedef-render`, every
  preset at 44.1, 48 and 96 kHz).

UI (Vitest): the Click / Drums switch, the groove list per meter and its keys, intensity and
fills, the meter change picking the first groove, and the mock's drums.

## 10. Rejected

| Option | Why not |
|---|---|
| Samples or a sample pack | Not ours to ship; the lane's rule is synthesis only. |
| Floating-point synthesis | Renders differ in the last bits per platform (5.1): pinned hashes on one platform only. |
| A voice per hit with stealing (the click's design) | A ride in eighths at 240 bpm with τ 1.9 s overlaps about 30 strokes before they fall 20 dB; stealing cuts the wash, keeping them costs 30 voices. A bank per instrument superposes strokes as the drum does, at a fixed cost. |
| A drum clock of its own | Two clocks drift apart by a sample now and then; on the metronome's grid the drums, the click and the looper agree by construction. |
| Micro-timing in fractions of a beat | At 40 bpm a tempo-relative offset that is 5 ms at 120 bpm grows to 15 ms, past what reads as feel. |
| Early events (negative offsets on a step) | They need look-ahead past the step that has not sounded yet, and an early "1" would sound before the looper's downbeat. |
| A groove family adapting to every meter | A shuffle in 7/8 is not a groove anyone asked for; per-meter grooves stay musical. |

## 11. Known limits

- A synthesised kit: it sounds like a good drum machine, not a recorded kit.
- One groove plays at a time; no song arrangement (verse, chorus) and no user-written grooves yet.
- Fills come from two variants per family; they repeat.
- The drums do not follow a DAW's tempo (the plug-in has no drums).

## References

- N. H. Fletcher, T. D. Rossing, *The Physics of Musical Instruments*, 2nd ed., Springer, 1998:
  membrane modes and Bessel zeros (ch. 3, 18), cymbals (ch. 20).
- T. D. Rossing, *Science of Percussion Instruments*, World Scientific, 2000.
- F. Fahy, *Sound and Structural Vibration*, Academic Press: plate radiation below coincidence.
- K. L. Johnson, *Contact Mechanics*, Cambridge, 1985: Hertzian impact, contact time
  `∝ v^(−1/5)`.
- T. Tolonen, V. Välimäki, M. Karjalainen, "Modeling of tension modulation nonlinearity in
  plucked strings", IEEE Trans. Speech and Audio Processing 8(3), 2000: pitch rising with the
  square of the amplitude.
- I. J. Hirsh, "Auditory perception of temporal order", JASA 31(6), 1959.
- P. R. Cook, *Real Sound Synthesis for Interactive Applications*, A K Peters, 2002: modal
  synthesis and noise-driven percussion.
