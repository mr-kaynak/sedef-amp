# Input level

A player who has just plugged in needs one answer before anything else: is my guitar coming in,
and at the right level? Sedef's presets and amps are voiced for one input level
([amps.md, "Input level: what dBFS means here"](../dsp/amps.md#input-level-what-dbfs-means-here)):
a hard strum peaking around −4 to −2 dBFS at the input meter. Before 0.9.0 the header's input
meter was a 92 px bar with no numbers and no target, and its gradient turned amber, the
"needs attention" colour, right at that level, so a player who set the level correctly was told
to back off.

This page specifies what replaced it: a target zone on the meter, a verdict in words under it,
and a panel that says what to do. It is UI only. It reads the telemetry the engine already
sends; no parameter or engine changes.

## What the UI gets

Telemetry (`sedef:telemetry`, about 30 per second while the editor is visible):

- `inputDb[]`: the peak since the previous event, per input channel (one or two), in dBFS,
  **after** the Input knob (`Engine.cpp`, the chain's `inputPeak`). The plug-in floors it at
  −100 dB (`toDb`), which is also what an input of digital silence reports.
- `inputClip`: some sample reached 1.0 (0 dBFS) after the Input knob since the previous event.

The Input knob's value `G` (`inputGain`, −24 to +24 dB) is in the parameter store. So for each
event the UI knows:

- `P = max(inputDb)`, the level reaching the rig, and
- `R = P − G`, the level the audio interface itself delivered. In _Sum to mono_ a guitar on one
  side passes at unity and two equal sides average to the same level (InputFold), so `R` is the
  interface's peak in every mode but one: two different signals summed, which only lowers it.

## The verdict

`features/header/inputLevel.ts` (`InputLevelJudge`) turns the events into one of six states.
It is a pure class driven by explicit time, so tests run it without a clock.

| State     | Condition                                                                                | Chip        | Lamp  |
| --------- | ---------------------------------------------------------------------------------------- | ----------- | ----- |
| `none`    | every event in the last 3 s at the floor (`P ≤ −99 dB`)                                  | No signal   | off   |
| `waiting` | nothing judged yet: no event since the start, or since the last `none`, reached −60 dBFS | Check level | off   |
| `clip`    | the loudest `R` of the last 3 s is within 0.5 dB of full scale                           | Too hot     | coral |
| `hot`     | the loudest `R` of the last 3 s, plus `G`, is 0 dBFS or more                             | Too hot     | coral |
| `quiet`   | the loudest `R` of the last 8 s, plus `G`, is under −12 dBFS                             | Too quiet   | amber |
| `good`    | otherwise                                                                                | Level good  | lit   |

The first matching row wins. Only events at −60 dBFS or more (`P`) enter the windows.

Why each number:

- **The windows hold raw peaks (`R`), and the current Input is added when judging.** Turning the
  knob changes the verdict at once, between strums too, so "turn Input until it says Level
  good" works. The interface's own gain has no such shortcut: after turning it down, the next
  strums replace the old peaks within the window.
- **8 s for quiet.** The verdict follows the loudest playing, not every note. A soft passage
  after a hard strum must not read as "too quiet": with an 8 s memory a player has to play
  softly for 8 s before the verdict drops, and an Abyss chug or a strummed chord comes round
  well within that. A louder peak counts at once.
- **3 s for hot and clipping.** A clip is a fact about the last few strums; once the player has
  turned something down, three seconds of playing clear it.
- **Held when the playing stops.** When a window has had no event at −60 dBFS or more for its
  length, it keeps the loudest value it last had. The verdict stays on screen between songs, and
  a turn of the Input knob still moves it. A `none` clears both windows: whatever was judged
  before the input went silent no longer describes it.
- **−12 dBFS, the bottom of the zone.** The design strum peaks at −4 dBFS on a humbucker;
  single coils run 4–5 dB lower (amps.md), and a player checking the level with ordinary
  rather than hardest strums lands a few dB lower again. −12 dBFS leaves 8 dB under the design
  point. Further down, the drive presets lose more than 8 dB of drive and sound clean and thin,
  and the gate and compressor thresholds sit high relative to the playing.
- **0 dBFS, the top of the zone.** It is where the engine's clip light latches (`inputClip`), so
  the meter's clip lamp and the verdict agree. Between −4 and 0 the playing is a little hot for
  the presets but nothing is lost.
- **Within 0.5 dB of full scale, the interface clips.** An interface cannot deliver more than
  0 dBFS: a flat-topped wave and a peak just under full scale cannot be told apart from peaks,
  so the last half decibel counts as clipping. Turning Input down does not undo it; the panel
  says to turn the interface's gain down instead.
- **Hysteresis: 1 dB at −12.** From `quiet`, `good` needs the loudest peak at −11 dBFS or more;
  from `good`, `quiet` needs under −12. The loudest peak of 8 s moves in steps (a new strum, an
  old one leaving the window), so 1 dB is enough to stop a player sitting on the line from
  seeing the chip change on every strum. No hysteresis at 0 dBFS, so the verdict and the clip
  lamp change at the same point.
- **−60 dBFS for "playing".** It is the bottom of the header meter (a bar that shows nothing
  shows nothing to judge) and the level under which the tuner reads no note. Hum and an
  interface's own noise sit far below it at sane gains.
- **3 s of the floor for `none`.** −100 dB is the floor the plug-in reports for digital silence:
  a muted input, a track that is not monitoring, a wrong input channel, or a very quiet
  interface with the guitar untouched. A rest between phrases is never digital silence on a
  real input, and 3 s keeps the chip from blinking while a host starts its audio.

## The header

The input meter keeps its place and size. Two changes:

- **Its own colours.** Under −12 dBFS the bar runs from slate to aqua, through the zone it runs
  aqua to lilac to rose (nacre: the level is alive), and over 0 dBFS it is coral. A thin mark
  under the bars spans the zone, −12 to 0 dBFS, so the target is visible without reading
  anything. The output meter keeps its gradient, where amber near 0 dBFS is the right warning.
- **A chip under it**, in the slot where the output meter has the Doubler switch and in the
  same style: a lamp and two words. It is a button that opens the Input level panel; its
  accessible name is its text and what it is about ("Too quiet, input level"). The chip's text
  carries the state; the lamp's colour only repeats it.

The clip lamps of both meters now say what they show: "Input clipped. Click to reset" while
latched, "Input clip light" otherwise.

## The panel

A popover under the chip (`popover: 'input'`, Escape and outside clicks close it; focus starts
on its Input knob):

1. **A large meter**, the same meter across the panel, with "Good" over a bracket that spans
   the zone above the bars and the scale −48, −24, −12, −6, 0 under them.
2. **The verdict** as a heading with its lamp, and one or two sentences that say what it means
   and what to turn. Heading and sentences are a polite live region, so a player turning a knob
   by ear hears when the level is right.
   - _No signal_: "Nothing is coming in." Then, in the standalone app, "Pick your interface's
     input in Audio settings, and switch off Mute audio input there."; in a plug-in, "Check
     that this track's input is the one your guitar is plugged into, and that the track is
     monitoring."
   - _Check your level_: "Play your hardest chords for a few seconds."
   - _Too quiet_: "The amps hear less guitar than the presets expect, so they sound cleaner
     and thinner. Turn up the gain on your audio interface, or Input here."
   - _Level good_: "Your guitar reaches the amps at the level the presets were made for."
   - _Too hot_ (`hot`): "Louder than the presets expect: drives and amps break up more. Turn
     Input down."
   - _Your interface is clipping_ (`clip`): "Turn the gain on your audio interface down until
     its clip light stays off, then bring the level back up with Input here."
3. **The loudest peak** of the last 8 s, in whole dB with a sign, once there is one.
4. **The Input knob**, the same parameter as the header's, at a size that is easy to turn, and
   how to set it in one line: "Strum hard. Set your interface's gain so the peaks land in
   Good, then fine-tune with Input."
5. **Audio settings…** (standalone app only): the app's audio device window.

## Elsewhere

- **Tuner**: while the verdict is _No signal_, the tuner says "Nothing is coming in" under its
  gauge, with a **Check input** button that opens the panel. "Play a string" alone would leave
  a player with a muted input waiting.
- **Settings** (standalone app only): an **Audio settings…** row first, which opens the app's
  audio device window (`sedef_audioSettings`, the same JUCE dialog as the title bar's Options
  button). Sedef MIDI's window has had the same button since 0.6.0.

## Mock

`?input=silent|quiet|clipping` sets what the browser mock's simulated interface delivers:
nothing, peaks around −30 dBFS (the level of the musician's own recordings in
[guitar-to-midi.md](../design/guitar-to-midi.md)), or a flat top at full scale. Without the flag
the simulated strums peak between −11 and −3 dBFS, inside the zone.

## Tests

`features/header/inputLevel.test.ts` pins the table above: each state from a synthetic event
stream, the 8 s and 3 s windows, the hold when playing stops, the Input knob moving a held
verdict, the hysteresis at −12 dBFS, `none` after exactly 3 s of floor and its reset, and two
channels judged by the louder. `InputLevelChip.test.tsx` checks the chip's name and the panel's
text and buttons per state, standalone and plug-in.
