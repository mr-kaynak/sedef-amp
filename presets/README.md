# Presets

`factory/` holds the presets that ship inside the plugin (they are compiled in as binary data).
Each file is JSON:

```json
{
  "format": "sedef-preset",
  "version": 1,
  "name": "Nacre",
  "folder": "Clean",
  "author": "Sedef",
  "tags": ["chorus", "compressor", "plate"],
  "description": "Glassy, lightly squeezed clean with slow chorus and a short plate.",
  "board": ["gate", "tide", "ripple"],
  "parameters": { "ampModel": 0, "lagoonVolume": 0.45 }
}
```

- `parameters` uses the ids from `engine/include/sedef/params/ParameterList.inc`, in real units.
  Anything left out takes its default, so a preset only lists what it changes.
- `board` is the pedalboard: pedal ids from `engine/include/sedef/board/PedalList.inc`, in signal
  order. A factory preset's board holds exactly the pedals it switches on, in default order; the
  `factory-preset-boards` test checks that and that each preset sounds the same as on the fixed
  chain. A file without a board (one saved before the pedalboard) gets the whole fixed chain.
- Global parameters (input and output gain, tuner, metronome, tempo, stereo mode and the like) are
  never stored in a preset; switching presets must not change how the plugin sits in a session.
- `folder` is the preset's category in the browser. `tags` are lower case, at most 8 of up to 24
  characters. `description` is optional: one line of at most 140 characters, shown under the
  name. Files without it (and older readers that don't know it) work as before, so the format
  version stays 1.
- User presets use the same format and live in the user folder (`Application Support/Sedef/Presets`
  on macOS, `%APPDATA%\Sedef\Presets` on Windows, `~/.config/Sedef/Presets` on Linux).

## Categories and tags

Factory presets use five categories: **Clean**, **Crunch** (edge of breakup to classic rock),
**High Gain**, **Lead** and **Ambient**. Tags say what the category doesn't, from one vocabulary
so the browser's chips stay useful (`ui/src/bridge/mock/factoryPresets.test.ts` checks both):

- effects: `compressor`, `overdrive`, `boost`, `distortion`, `fuzz`, `octave`, `chorus`,
  `tremolo`, `vibrato`, `delay`, `slapback`, `plate`, `hall`, `wah`, `envelope filter`, `phaser`,
  `flanger`, `vibe`
- feel and use: `edge of breakup`, `rhythm`, `solo`, `swell`, `drone`
- styles: `blues`, `rock`, `metal`, `funk`, `neo-soul`, `surf`, `rockabilly`, `garage`

Don't repeat the category as a tag. Descriptions are one line in a guitarist's words: what it
sounds like, then what it is for ("Snappy compressed clean for funk comping, dotted-eighth echo
underneath."). The browser shows the amp and the switched-on chain itself, so the description
doesn't need to list them.

## Levels

Factory presets are balanced to **-16 LUFS integrated, true peak at or below -1 dBTP**, rendered
from the synthetic DI in `sedef-render` (about -18 dBFS RMS, the amps' design input level: a hard
humbucker strum with Input Gain set as in [docs/dsp/amps.md](../docs/dsp/amps.md#4-gain-targets-and-input-level)).
The amp's Output knob carries the correction, so the sound itself never changes. After editing a
preset, rebalance and check:

```sh
cmake --preset engine && cmake --build --preset engine
python3 tools/presets/balance_levels.py build/engine/tools/sedef-render "$(mktemp -d)" -16 -1 \
    presets/factory/*.sedefpreset            # needs ffmpeg for the loudness measurement
build/engine/tools/sedef-render --check presets/factory/*.sedefpreset
```

`ctest` runs the same check (`factory-presets`) on every platform.
