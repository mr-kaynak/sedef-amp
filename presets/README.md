# Presets

`factory/` holds the presets that ship inside the plugin (they are compiled in as binary data).
Each file is JSON:

```json
{
  "format": "sedef-preset",
  "version": 1,
  "id": "nacre",
  "name": "Chorus Clean",
  "folder": "Clean",
  "author": "Sedef",
  "tags": ["chorus", "compressor", "plate"],
  "description": "Glassy, lightly squeezed clean with slow chorus and a short plate.",
  "board": ["gate", "tide", "ripple"],
  "parameters": { "ampModel": 0, "lagoonVolume": 0.45 }
}
```

- `id` names a factory preset for good: the plug-in knows it as `factory:<id>`, and sessions,
  favourites and Recent refer to it that way. It is the file's name without the extension. The
  `name` is what the player reads and can change in a later release; the id never does (the 17
  presets of 0.5.0 keep the ids they had under their old names, `nacre`, `riptide` and so on).
  A file without an `id` gets one from its name, as before. User presets have none: theirs is
  the file's path.
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

Factory presets use five categories, listed in this order everywhere: **Clean**, **Crunch** (edge
of breakup to classic rock), **High Gain**, **Lead** and **Ambient**. Each has at least five
presets, every pedal is in at least two, and each amp is covered from clean to its highest gain.
Tags say what the category doesn't, from one vocabulary so search stays useful
(`ui/src/bridge/mock/factoryPresets.test.ts` checks all of this):

- effects: `compressor`, `overdrive`, `boost`, `distortion`, `fuzz`, `octave`, `chorus`,
  `tremolo`, `vibrato`, `delay`, `slapback`, `plate`, `hall`, `wah`, `envelope filter`, `phaser`,
  `flanger`, `vibe`, `doubler`
- feel and use: `edge of breakup`, `rhythm`, `solo`, `swell`, `drone`, `pad`
- styles: `blues`, `rock`, `metal`, `funk`, `neo-soul`, `surf`, `rockabilly`, `garage`, `jazz`,
  `country`, `doom`

Don't repeat the category as a tag.

## Names and descriptions

A name says the sound, so a player can pick from the list without reading further: a style or a
use plus the one thing that sets the preset apart ("Surf Tremolo", "Tight Metal Boost", "Jazz
Warm Clean"), at most 24 characters. No amp or pedal names (the row shows the chain), and nothing
that only means something to us.

Descriptions are one line in a guitarist's words: what it sounds like, then what it is for
("Snappy compressed clean for funk comping, dotted-eighth echo underneath."). The browser shows
the amp and the switched-on chain itself, so the description doesn't need to list them.

Values come from what each control does ([docs/dsp/](../docs/dsp/)), then from the render below.
Nothing is taken from another product's presets, and no product, company, artist or song is
named ([ADR-0006](../docs/adr/0006-clean-room-sources.md)).

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
build/engine/tools/sedef-render --check-boards presets/factory/*.sedefpreset
```

`--check` renders each preset and fails one that is not finite, peaks above -1 dBFS or leaves the
level window (-24 to -18 dBFS RMS, where -16 LUFS lands for sounds from bright cleans to dense
high gain).

**The gate on a quiet guitar.** The gate listens to the raw DI, so its threshold is in dBFS at
the interface, before Input Gain. `--check` therefore also plays each preset a DI that peaks at
-30 dBFS (an interface at minimum gain) and then dies away at 12 dB a second, with the gate and
without it, and reports how far under its peak the guitar is when the gate first takes more than
6 dB: "gate holds a quiet note 31 dB down". It must hold at least 24 dB. Clean presets leave
the threshold at its default, -72 dB (over 42 dB down); high-gain ones sit between -56 and
-62 dB (31 to 41 dB down). `sedef-render --di-gain -18 …` renders any preset from a quieter
guitar.

`ctest` runs both checks (`factory-presets`, `factory-preset-boards`) on every platform.
