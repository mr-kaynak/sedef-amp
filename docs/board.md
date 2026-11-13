# The pedalboard, part two: after the amp, and two of a pedal

Status: design for 0.11.0, written before the code; the code and its tests follow it. Owners:
core maintainers. Builds on the board in [architecture.md](architecture.md#the-pedalboard)
(the registry, `Board`, `PedalRack`, `BoardSwitch` and the duck), which stays true for the pedals
in front of the amp. Related: [ADR-0004](adr/0004-single-parameter-schema.md) (parameter ids),
[ADR-0005](adr/0005-real-time-safety.md) (the audio thread), [live.md](live.md) (scenes).

## 1. What the player gets

- **Pedals after the amp.** A modulation pedal (Ripple, Beacon, Gyre, Jetty) can sit after the
  amp and the cab, where a rig's effects loop puts it: a phaser or a chorus that moves the
  finished amp sound instead of feeding a driven amp. On the board the amp is a block in the
  chain, and the pedals stand on either side of it; the cable shows the signal going from the
  guitar through the pedals in front, into the amp, out through the pedals after it and on to
  the effects (EQ, Doubler, Drift, Fathom).
- **Two of a pedal.** Every pedal can be on the board twice: two drives stacked, a gate in front
  of the drives and another after them, a compressor on either side of a fuzz, a phaser before
  the amp and another after it. The second one is *Kelp 2*, with its own knobs and switch, and
  it is *Kelp 2* in the host's automation list too.
- **Stereo modulation after the amp.** After the cab the signal is stereo. There the phaser and
  the flanger sweep the two sides a quarter of a cycle apart, and Ripple's chorus does what it
  already did with two channels; the mono sum stays within bounds derived below (§6).

What does not change: every board that 0.10 can make plays and saves exactly as it did, and the
factory presets render bit for bit as on main (§10).

## 2. The limits, and where each comes from

| Limit | Value | Reason |
|---|---|---|
| Copies of one pedal | 2 | Every case players ask for (two drives stacked, a boost before and after, a gate on either side of the drives, a modulation before and after the amp) takes two. Each further copy of every pedal costs 73 more automatable parameters in every host's list (§7) and another number to tell apart on the board. |
| Drives and fuzzes on the board | 7 in all, any mix | The latency budget (§4.2). The seven Drive and Fuzz pedals are exactly the ones that oversample, each with the same latency `L` (5 samples at 44.1/48 kHz in Low latency, 37 in Linear phase). The rig reports `7 L` whatever the board holds, so its latency never changes; a second Kelp takes the place of a drive that isn't on the board. |
| After the amp | Modulation only (Ripple, Beacon, Gyre, Jetty) | They have no latency at any rate and in either mode, so the stage after the amp adds none. Drives and fuzzes after a miked cab distort the cab's response and would add latency; the gate listens to the guitar's DI, which after the amp is a different instrument; the compressor, the filters and the octave read the picking, which the amp has already compressed. A later time pedal (delay, reverb) would also fit there: the rule is by category. |
| Pedals on the board | 25 | What the two rules above leave: two each of the nine pedals without latency (18) and seven drives and fuzzes. |

On screen a full board of 25 is three rows at the smallest scale, the third reached by scrolling
the board (`layout.test.ts` pins the worst case, §9). The two-row promise for sixteen different
pedals stays.

## 3. The model

### 3.1 A pedal on the board

```cpp
struct PedalRef { PedalId id; std::uint8_t instance; };   // instance 0 or 1
```

`instance` is the pedal's identity, not its place: *Kelp 2* is instance 1 wherever it stands,
and stays *Kelp 2* when the first Kelp leaves the board (its parameters, and the automation on
them, are its own). Adding a pedal takes the lowest free instance.

### 3.2 The board

`sedef::Board` becomes two ordered lists, the pedals in front of the amp and the pedals after
it (`board/Board.h`):

```cpp
class PedalOrder { std::array<PedalRef, 32> refs; std::uint8_t size; ... };  // one side
struct Board { PedalOrder pre, post; ... };
```

Both are plain, trivially copyable data (about 130 bytes), so the board still crosses to the
audio thread by value through the `RtMailbox`. A rack plays one `PedalOrder`; the two sides never
share a rack.

Edits (message thread):

- `add (id)`: the lowest free instance, in front of the amp at its default position (after the
  last pedal there ranked at or below it), as 0.10 did. Nothing when both copies are on the board
  or the drive budget is full.
- `addAfter (ref)`: a second copy right after the first, on the first one's side ("two drives
  stacked").
- `place (ref, side, position)`: inserts or moves a pedal to a side and a position (clamped).
  Refused when the side doesn't take it (§2).
- `remove (ref)`.
- `legacy()`, `everyPedal()`, `legacyFor (isOn)` and `switchedOn (isOn)` keep their meaning and
  their output for every board 0.10 could make: first copies, in front of the amp. `switchedOn`
  also adds a second copy whose switch is on, right after its first.

Validity, checked by every edit and by every parse: each `PedalRef` at most once, instance below
2, the after side only for modulation, at most seven drives and fuzzes. An edit that would break
one does nothing and returns false.

### 3.3 Saving

The board keeps its spelling, a list of ids in signal order, with two additions:

- **`amp`** marks where the amp is. It is written only when a pedal stands after it, so a board
  with nothing after the amp is spelt exactly as 0.10 spells it.
- **`kelp#2`** is the second copy. The first is plain `kelp`, as before.

`gate,kelp,kelp#2,coral,amp,gyre` is the Gate, two Kelps and Coral in front of the amp and Gyre
after it. Sessions (`board="…"` in the state's `Extra` element), presets (`"board": [ … ]`),
snapshots, A/B and the UI bridge all use it.

**Old files load exactly as before**: they have neither token, so the parser produces the same
board 0.10 did, every pedal a first copy in front of the amp, and every parameter they don't
hold (the second copies' parameters) takes its default.

**A new file opened in 0.10**: 0.10 skips ids it doesn't know, so it drops `amp` and every
`#2`. What remains plays like this:

- *Second copies are gone*, with their knobs. Two stacked Kelps play as one Kelp, the first one's
  settings: less gain, and the stack's extra level is missing. A gate after the drives (a second
  Gate) is gone, so the drives' noise between notes comes through.
- *Pedals after the amp move in front of it*, at the end of the pedals there, in the same order
  and with the same settings, and turn mono. Into a clean amp a modulation sounds much as it did,
  narrower. Into a driven amp the amp distorts the sweep: a tremolo is mostly flattened by the
  amp's compression, a phaser or flanger gets the harder, more pronounced edge it has in front of
  a driven amp.
- The scenes keep working; switches of second copies in them are skipped.

### 3.4 Parsing

`boardFromText` and `boardFromVar` read the list in order, the side switching at `amp`. Unknown
ids are skipped, as before; so are repeats, a second `amp`, and a pedal its side doesn't take or
that would exceed the drive budget. A modulation pedal is never lost to a missing `amp`: without
one everything is in front of the amp, which is 0.10's board.

## 4. The engine

### 4.1 Where the stage after the amp sits

```
… Pedals (pre) ─► Amp ─► Cab ─► EQ ─► Pedals (post) ─► Doubler ─► Drift ─► Fathom ─► Metronome ─► Out
```

After the EQ, because each amp has its own EQ bank on the EQ page: it is part of the amp's tone,
and the pedals after the amp move the finished amp sound. In front of the Doubler, Drift and
Fathom, so a modulation is doubled, echoed and reverberated rather than modulating the echoes:
the order the pedals' own pages recommend for modulation and time effects.

It is stereo in every layout. With one input and two outputs (Mono → Stereo) and in Summed mode
it takes the cab's two sides; in Dual mode the two chains' sum, left and right; with one output
it is folded to mono afterwards with everything else (§6.4).

### 4.2 Racks and instances

- Each chain's `PedalRack` (in front of the amp, mono) holds two slots per pedal, one per copy,
  all prepared up front: 32 modules per chain, two chains.
- One more `PedalRack` after the amp, stereo, prepares only the modulation pedals' slots (8
  modules, two channels each). Its other slots are never prepared and never reached: the side
  after the amp can't hold those pedals.
- `PedalSettings` carries both copies' switches and settings, built from the snapshot once per
  block: the second copy's through a view of the snapshot that reads each parameter's second-copy
  id (§7.2), so `PedalList.inc` keeps one settings expression per pedal.

**Latency** stays the sum of each Drive and Fuzz pedal's latency once, `7 L`, whatever the
board. The rule that keeps it there extends the one in architecture.md: a drive or fuzz with no
copy on the board leaves its delay where the fixed chain had it, *except* that each second copy
of a drive or fuzz on the board takes one such delay away, the last ones in default order
first. A board holds at most seven drives and fuzzes, so there are always enough delays to take:
with `k` kinds present and `s` second copies, `k + s ≤ 7`, and `7 − k ≥ s` delays remain. A
board without second copies has exactly the layout it had, which keeps the renders bit-identical.

The gate's key still reaches each gate lined up with its place: the rack adds up the latency in
front of each step, delays and second copies included.

### 4.3 Changing the board while it plays (ADR-0005)

Every module, both copies, both sides, is prepared in `prepare()`; a board change only rewires
which slots play, so nothing allocates on the audio thread.

Each side has its own `BoardSwitch` and its own duck, with the rules architecture.md gives for
the board:

- A pedal without latency joining or leaving a side, with nothing else on that side changing
  its order, fades through its own 10 ms bypass.
- Anything else on a side (pedals that stay change order, a pedal with latency joins or leaves)
  happens at the bottom of that side's duck. The side in front of the amp ducks as it did. The
  side after the amp ducks its own input and output; its latency is 0, so its output follows its
  input with no lag.

**Across the amp.** Moving a pedal from one side to the other is a leave on one side and a join
on the other. Modulation pedals have no latency, and the move changes no one else's order on
either side, so it is two bypass fades and no duck: the pedal fades out in front of the amp (it is
switched off there and leaves once its fade has ended, 10 ms) and fades in after it from a clean
state, as if switched on there. If the same change also reorders a side, that side ducks as it
would on its own; a change never ducks a side it doesn't touch. Each side's duck is short (two
6 ms fades plus that side's latency), and the two can run at the same time.

A duck on the side after the amp has nothing behind it to fill the gap: the rig's output dips to
silence for about 12 ms unless Drift or Fathom ring through it, where a reorder in front of the
amp dips only −9 to −28 dB because the amp and cab ring on. It only happens when pedals after the
amp swap places, a deliberate edit; joining, leaving and crossing the amp never duck.

The engine steps both switches at every block boundary and processes the shorter of the two
chunks they allow, so a side's fade to silence still ends exactly on a boundary.

## 5. The UI

### 5.1 The board

The board draws the chain left to right and row by row:
`Guitar ─► pedals in front ─► [Amp] ─► pedals after ─► Out`.

- **The amp is a block in the chain**, a list item between the two groups, drawn as the amp
  head it already was (its colours and name, a button to the Amp page). It has no handle and
  never moves; pedals move around it.
- **After the amp** is a tinted band behind the pedals there and the Out jack, captioned
  *After the amp*. Out is a small jack like Guitar, labelled *Out*, where the cable ends and the
  effects take over. An empty side after the amp is just the amp, a short cable and Out.
- **Second copies** are named with their number on the enclosure (*Kelp 2*); the first stays
  *Kelp*, as the host names their parameters.

### 5.2 Moving pedals

The UI keeps the board as one list of tokens with `amp` in it, so moving a pedal is moving it in
that list; `amp` itself never moves.

- **Drag**: the drop bar can go anywhere a pedal fits. A modulation pedal can land on either
  side of the amp. Any other pedal's bar stops at the amp: past it, the bar stays in the last
  gap in front of the amp, and the hint under the board reads *Only modulation goes after the
  amp*.
- **Keyboard**: on the handle, the arrow keys move a pedal one place, and one more step from the
  last place in front of the amp crosses it (modulation) or stays (anything else). Home and End
  go to the ends of the pedal's own side. Move left and Move right do the same, and switch off
  at the amp for pedals that can't cross it.
- **Announcements** name the side: "Gyre moved after the amp, 1 of 2." / "Kelp 2 moved to 5 of 9
  in front of the amp, after Kelp."

### 5.3 The Add pedal shelf

The shelf keeps its cards, with what each pedal can do on this board:

- Not on the board: **Add**, in front of the amp at its default place, as before.
- One copy on the board: **Add another** puts the second copy right after the first, on its
  side. The card reads *On the board*; pointing at it rings the copy on the board.
- Both copies on: *Both on the board*; picking the card shows them.
- The board holds seven drives and fuzzes: drive and fuzz cards read *7 drives and fuzzes at
  most* and add nothing (they stay focusable, `aria-disabled`, so the reason can be read).
- Modulation cards have a second button, **After the amp**, which puts the next copy at the end
  of the side after the amp.

The shelf's line for the eye says where the pedal would go, with the side ("Goes after the amp,
last." / "Goes in front of the amp, after Tide.").

## 6. Stereo modulation after the amp

### 6.1 What spreads

After the cab the pedals process two channels with one module (prepared with two channels, as
Ripple already could). A pedal's LFO is shared; where the two sides differ is a phase offset
`φ` (in cycles) for the second channel:

| Pedal, mode | φ | Why |
|---|---|---|
| Ripple chorus | 1/4 | Unchanged: Ripple already offsets its second channel by a quarter cycle. |
| Ripple tremolo | 0 | Unchanged. An offset tremolo is an auto-pan; a tremolo after the amp is the amp's own, and in phase it is untouched by a mono sum. |
| Ripple vibrato | 0 | Unchanged. Vibrato is all wet: an offset makes the two sides different delays of the same signal, and their sum a deep comb (§6.3). |
| Beacon | 0 | One lamp drives every stage, as in the circuit (docs/dsp/beacon.md). |
| Gyre | 1/4 | New. |
| Jetty | 1/4 | New. |

**Why a quarter cycle.** Take a modulation `m(θ)` of the LFO phase θ with fundamental
`cos 2πθ`. Mid (the mono sum) and side (the difference) carry it with amplitudes

```
mid:  |cos πφ|,    side: |sin πφ|
```

so `φ = 1/4` is the one offset where both are −3 dB: the most width (side) the sweep can have
without the mono sum losing more of the sweep than the width gains. At `φ = 1/2` the sweeps of the
two sides mirror each other and the mono sum barely moves; at 0 there is no width.

### 6.2 The mono sum of a phaser

Gyre without feedback is `H = (1 + A) / 2` (times a fixed makeup gain), with `A` an all-pass
chain, `|A| = 1`. Write `A = e^{jα}`, `|α| ≤ π`; then

```
H = cos(α/2) · e^{jα/2},   cos(α/2) ≥ 0,   arg H ∈ [−π/2, π/2].
```

The two sides' responses at any frequency, `H_L` and `H_R`, therefore never differ in phase by
more than π/2, so `Re (H_L · conj H_R) ≥ 0` and the mono sum `M = (H_L + H_R) / 2` obeys

```
|M|² = (|H_L|² + |H_R|² + 2 Re (H_L conj H_R)) / 4  ≥  (|H_L|² + |H_R|²) / 4,
|M|  ≤ (|H_L| + |H_R|) / 2  ≤  1.
```

At every frequency and every instant the mono sum is at most 3 dB below the mean power of the two
sides (they never cancel), and never louder than the phaser's own peaks. Its notches are where
both sides notch at once, so it is less notched than either side: it reads in mono as a gentler
phaser. Feedback adds the resonance `1 / (1 − g A)` in front, which turns the phase further near
the peaks; that case is measured (§6.5).

### 6.3 The mono sum of a flanger or chorus

Jetty without feedback, and Ripple's chorus, are `H = (1 + D) / 2` with `D = e^{−jωτ}`, a delay
`τ` that the LFO sweeps. With the two sides at delays `τ_L` and `τ_R`:

```
M = (1 + (e^{−jωτ_L} + e^{−jωτ_R}) / 2) / 2  =  (1 + c · e^{−jω τ̄}) / 2,
c = cos (ω Δτ / 2),   τ̄ = (τ_L + τ_R) / 2,   Δτ = τ_L − τ_R.
```

Because `|c| ≤ 1`, `(1 − |c|)/2 ≤ |M| ≤ (1 + |c|)/2`: the mono sum lies inside the mono
flanger's own range `[0, 1]`, with its notches filled in by `1 − |c|`. The spread adds no peak and
no notch the effect doesn't already have. This is also why vibrato stays in phase: without the
dry `1`, the sum is `c · e^{−jωτ̄}`, which notches to silence at `f = 1 / (2 Δτ)`, for Ripple's
vibrato swing of a few milliseconds a comb from about 100 Hz up.

How wide the comb in `c` is follows from the sweep. Jetty sweeps its delay in octaves,
`τ = τ_0 · 2^{d · tri (θ)}` with depth `d ≤ 1` octave either side (docs/dsp/jetty.md); a quarter
cycle apart the two triangles differ by at most half their swing, so
`|Δτ| ≤ τ_max − τ_max / 2^{d}`, at most half the longest delay. With Delay at its 10 ms maximum,
`c`'s first null is at `1 / (2 · 5 ms) = 100 Hz`, moving: the comb a flanger is.

### 6.4 One output

With one output the host hears the mono sum. §6.2 and §6.3 bound it: no new peaks, the sides
never cancel in the phaser, and the flanger's and chorus's notches are shallower than in mono.
So the spread stays on with one output (unlike the Doubler, which with one output would be
nothing but a comb, and stays off).

### 6.5 What the tests hold

On white noise from the tests' fixed generator, the same signal on both sides (the worst case:
the cab's two mics are never more alike than that), each pedal at full depth, with feedback off
and at its maximum, through the stage after the amp at 48 kHz:

- **No cancellation**: in every 1024-sample window, the mono sum's power is at least half the
  mean power of the two sides (−3.01 dB). Proved for the phaser without feedback, measured with.
- **Inside the effect's own range**: in every window, the mono sum's power is within the range
  the same pedal with no spread (φ = 0) covers over the whole run, widened by the measured
  margin (§6.6).
- **Mono-identical where it should be**: Ripple tremolo and vibrato and Beacon put out the same
  samples on both sides when both sides get the same input.

### 6.6 Measured

To be filled in from `[.board-measure]` once the code exists.

## 7. Parameters (ADR-0004)

### 7.1 The scheme

Every existing parameter keeps its id and means the first copy. The second copy of each pedal
gets its own parameters, appended once, at the end of `ParameterList.inc`:

- **Ids**: the pedal's id with `2` after it: `kelpDrive` → `kelp2Drive`, `gateEnabled` →
  `gate2Enabled`. Every pedal parameter starts with its pedal's id (a test checks), so the rule
  is mechanical and the ids can't collide with another pedal's.
- **Names**: the pedal's name with ` 2`: *Kelp 2 Drive*, and the switch *Kelp 2*.
- **Group**: *Kelp 2*.
- **Everything else** (kind, range, default, skew, unit, choices, automatable) as the first
  copy's.

73 parameters (every pedal's 2 to 6), in registry order, then each pedal's parameters in
schema order. A test pins the whole block against the first copies, so it can't drift. The
scheme is bounded: two copies, 73 ids, and a third copy would be a new decision (§11).

**Automation.** A host lists *Kelp 2 Drive* next to *Kelp Drive*. Automation on *Kelp 2 Drive*
moves the second Kelp, wherever it is on the board and on whichever side of the amp; it does
nothing while the second Kelp is off the board, as the first copy's do today. Removing the first
Kelp doesn't renumber the second.

### 7.2 In the engine

`kSecondCopy[ParamId]`, a constant table built at compile time from the schema (for each pedal
parameter, the id with the pedal id's `2`), maps a first-copy id to its second-copy id. The
second copy's `PedalSettings` are the same settings expressions read through a snapshot view that
looks every pedal parameter up in that table. `sedef-params` writes the table into the UI's
generated files, and the UI maps a panel's parameter ids the same way.

## 8. Scenes

A scene holds every pedal's switch already; the second copies' switches join the list
(`kSceneSwitches`: 16 + 6 → 32 + 6 = 38 switches). With the 16 values a player can pick that is
54 parameters, so `kMaxSceneParams` goes from 48 to 64 (live.md §7.3). Scenes are saved by
parameter id, so old scene lists load as before (second copies off in every scene, as their
default) and 0.10 skips the new ids.

**Placement is not a scene value.** Which pedals are on the board, in which order and on which
side of the amp, is the board's, saved with the preset; a scene switches pedals on and off where
they stand. So a scene can't move a phaser across the amp, but two phasers, one on each side, with
a scene switching between them, does the same without a duck. The second copies' knobs can be
picked as scene values like any other.

## 9. Tests

- **After the amp is the same DSP after the amp.** The rig with Gyre (then Jetty, Ripple chorus,
  Beacon) after the amp equals, sample for sample, the rig without it with the same module,
  prepared stereo, applied offline to the amp's output (the stage's input, taken from the rig
  with nothing after the amp: the stage is after the EQ and in front of everything else that is
  off).
- **Instance independence.** Two Kelps with different settings equal the two modules applied by
  hand in series; changing the second copy's knob doesn't change the first's output and vice
  versa; the second copy's parameters reach only it.
- **The latency never changes** with second copies (extends the existing test: boards with two of
  several drives, in both modes, at every rate).
- **Second copies of drives take delays away** as §4.2 says, and a board without second copies
  has the layout it had.
- **Duck across the amp.** Moving a modulation pedal across: neither side ducks, the move is done
  within two fades plus a block, and the rig's output has no step (excess below −50 dB). Swapping
  two pedals after the amp ducks that side only. A move across plus a reorder in front of the amp
  ducks the front only.
- **Invariance.** The rig with pedals on both sides and second copies, in blocks of 1 to 4096
  frames and odd sizes, gives the same output; at 44.1, 48, 88.2 and 96 kHz the latency is `7 L`
  for that rate and every pedal runs.
- **No allocation.** The allocation counter around `process` while the board changes across the
  amp, adds second copies and reorders both sides.
- **Saving.** Every 0.10 spelling parses to the board it did; new spellings round-trip; a new
  spelling read by the 0.10 rules (unknown ids skipped) gives the board §3.3 says; invalid
  tokens are dropped. Plug-in: a session and a user preset with pedals after the amp and second
  copies round-trip, an old session and an old preset load as before.
- **Mono sum** (§6.5).
- **UI.** Dragging a modulation pedal across the amp moves it after the amp; dragging a drive
  there stops at the amp; the keyboard crosses the amp; Add another adds the second copy after
  the first; the drive limit; second copies' knobs drive their own parameters. The layout fits
  sixteen different pedals in two rows and the largest board (25) in three.
- **Factory presets** render bit-identical to main (`sedef-render` on both builds, all 49).

## 10. Factory presets

They don't change, and render bit-identical to main: none has a pedal after the amp or a second
copy, so they play the layout they played. Checked by rendering all 49 with `sedef-render` built
from main and from this branch and comparing the files byte for byte.

## 11. Rejected

- **Generic slot parameters** ("Slot 1 Knob A") instead of per-copy ones: a host's automation
  would follow a place on the board, not a pedal, so moving a pedal would move its automation
  onto another pedal. A slot needs as many knobs as the biggest pedal (6), 25 slots × 6 = 150
  parameters, and their names say nothing.
- **Three copies of each pedal**: 146 extra parameters instead of 73, for stacks nobody asked
  for; the board's 25 already needs a scroll.
- **Counting every copy's latency** (the rig would report every second copy's latency whether
  used or not): 7 × 5 = 35 more samples at 48 kHz in Low latency (0.73 ms), 7 × 37 = 259 (5.4 ms)
  in Linear phase, for every player, always, and every factory render moves.
- **Latency that follows the board**: the host would re-align the track every time a second drive
  joined or left, and some hosts drop out while they do.
- **Two mono modules for the stereo stage**, the second started a quarter cycle on: simpler, but
  the offset would apply to every mode (Ripple's vibrato and tremolo too, §6.1), and every
  coefficient is computed twice.
- **One duck for both sides**: a reorder in front of the amp would also silence the stage after
  it, and the rig's dip would go from −9 to −28 dB to silence.
- **Moving a pedal across the amp under the duck**: a 12 ms dip in the whole rig for a change that
  two fades make without one.
- **Allowing every pedal after the amp**: the drives would add latency after the amp (or a third
  latency budget), and see §2 for how each sounds there.
