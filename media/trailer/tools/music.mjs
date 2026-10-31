// Generates the trailer's music bed with ElevenLabs Music from a chunked composition plan (no
// lyrics, so instrumental). The model treats chunk durations as guidance, not a grid: the bed that
// shipped has a sparse intro to ~10.5 s, the groove to ~34 s, a sparse section to ~44 s and a
// final swell. The edit (index.html) is fitted to those measured times; if you regenerate, measure
// again (see docs/trailer.md) and move the clips.
// Run: ELEVENLABS_ENV=/path/to/.env.local npm run music
import { ElevenLabsClient } from "@elevenlabs/elevenlabs-js";
import { createWriteStream, mkdirSync } from "node:fs";
import { Readable } from "node:stream";
import { finished } from "node:stream/promises";

// One chunk per section of the edit. Section names go in brackets; no lyrics keeps it instrumental.
const chunk = (name, durationMs, positiveStyles, negativeStyles = []) => ({
  text: `[${name}]`,
  durationMs,
  positiveStyles,
  negativeStyles: ["vocals", "singing", "risers", "EDM drops", "distorted guitar", ...negativeStyles],
});

const client = new ElevenLabsClient();
const audio = await client.music.compose({
  modelId: "music_v2_5",
  compositionPlan: {
    chunks: [
      chunk("Intro", 7000, [
        "instrumental neo-soul",
        "84 bpm",
        "warm and intimate",
        "shimmering clean electric guitar harmonics with gentle chorus",
        "soft pad",
        "spacious, no drums",
        "great production quality",
      ], ["drums", "bass"]),
      chunk("Groove", 18000, [
        "laid-back neo-soul drums with brushes and a soft snare",
        "round electric bass",
        "warm Rhodes chords",
        "clean guitar fills between phrases",
        "leaves room for a voiceover",
      ]),
      chunk("Breakdown", 9500, [
        "drums drop out",
        "single clean guitar arpeggio over a soft pad",
        "sparse and attentive",
      ], ["drums", "percussion"]),
      chunk("Return", 7000, ["full groove returns", "small lift", "brighter Rhodes voicings"]),
      chunk("Outro", 6500, [
        "resolve on a held major seventh chord",
        "guitar harmonic on top",
        "fade out",
      ], ["drums"]),
    ],
  },
});

mkdirSync("assets", { recursive: true });
const out = createWriteStream("assets/music.mp3");
const src = typeof audio.pipe === "function" ? audio : Readable.fromWeb(audio);
src.pipe(out);
await finished(out);
console.log("assets/music.mp3");
