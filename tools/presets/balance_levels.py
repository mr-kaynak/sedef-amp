#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Balances factory presets to a common integrated loudness via each preset's amp Output knob.
# usage: python3 tools/presets/balance_levels.py <render tool> <work dir> <target LUFS> <max true peak> presets...
import json, re, subprocess, sys, os

render, work, target, max_tp = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
presets = sys.argv[5:]
AMPS = ["lagoon", "reef", "abyss"]

def knob_to_db(k):
    return -24.0 + 48.0 * k if k <= 0.5 else 24.0 * (k - 0.5)

def db_to_knob(db):
    db = max(-24.0, min(12.0, db))
    return (db + 24.0) / 48.0 if db <= 0.0 else 0.5 + db / 24.0

def measure(preset, name):
    wav = os.path.join(work, name + ".wav")
    subprocess.run([render, "--preset", preset, "--out", wav, "--seconds", "12"], check=True,
                   stdout=subprocess.DEVNULL)
    out = subprocess.run(["ffmpeg", "-nostats", "-hide_banner", "-i", wav, "-af", "ebur128=peak=true",
                          "-f", "null", "-"], capture_output=True, text=True).stderr
    summary = out[out.rindex("Summary:"):]
    lufs = float(re.search(r"I:\s+(-?[\d.]+) LUFS", summary).group(1))
    tp = float(re.search(r"Peak:\s+(-?[\d.]+) dBFS", summary).group(1))
    return lufs, tp

for preset in presets:
    name = os.path.basename(preset).rsplit(".", 1)[0]
    with open(preset) as f:
        doc = json.load(f)
    params = doc["parameters"]
    key = AMPS[int(params.get("ampModel", 0))] + "Output"
    before = measure(preset, name)
    for _ in range(3):
        lufs, tp = measure(preset, name)
        delta = min(target - lufs, max_tp - tp)
        if abs(delta) < 0.1:
            break
        k = float(params.get(key, 0.5))
        params[key] = round(db_to_knob(knob_to_db(k) + delta), 4)
        # Keep the amp's knobs together: re-insert the output key right after its Master.
        master = key.replace("Output", "Master")
        items = [(k2, v) for k2, v in params.items() if k2 != key]
        idx = next((i + 1 for i, (k2, _) in enumerate(items) if k2 == master), len(items))
        items.insert(idx, (key, params[key]))
        doc["parameters"] = params = dict(items)
        with open(preset, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    after = measure(preset, name)
    print(f"{name:18s} {key:13s} {params.get(key, 0.5):.4f} ({knob_to_db(float(params.get(key, 0.5))):+6.1f} dB)  "
          f"before {before[0]:6.1f} LUFS {before[1]:5.1f} dBTP  after {after[0]:6.1f} LUFS {after[1]:5.1f} dBTP")
