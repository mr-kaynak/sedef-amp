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
  "tags": ["clean", "chorus", "plate"],
  "parameters": { "ampModel": 0, "lagoonVolume": 0.45 }
}
```

- `parameters` uses the ids from `engine/include/sedef/params/ParameterList.inc`, in real units.
  Anything left out takes its default, so a preset only lists what it changes.
- Global parameters (input and output gain, tuner, metronome, tempo, stereo mode and the like) are
  never stored in a preset; switching presets must not change how the plugin sits in a session.
- User presets use the same format and live in the user folder (`Application Support/Sedef/Presets`
  on macOS, `%APPDATA%\Sedef\Presets` on Windows, `~/.config/Sedef/Presets` on Linux).

## Levels

Factory presets are balanced to **-16 LUFS integrated, true peak at or below -1 dBTP**, rendered
from the synthetic DI in `sedef-render` (about -18 dBFS RMS, a typical single-coil DI level). The
amp's Output knob carries the correction, so the sound itself never changes. After editing a
preset, rebalance and check:

```sh
cmake --preset engine && cmake --build --preset engine
python3 tools/presets/balance_levels.py build/engine/tools/sedef-render "$(mktemp -d)" -16 -1 \
    presets/factory/*.sedefpreset            # needs ffmpeg for the loudness measurement
build/engine/tools/sedef-render --check presets/factory/*.sedefpreset
```

`ctest` runs the same check (`factory-presets`) on every platform.
