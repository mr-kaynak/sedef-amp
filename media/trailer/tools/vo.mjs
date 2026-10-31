// Generates the trailer's English voiceover with ElevenLabs and saves the audio plus word
// timings, so scenes can land on their words.
// Run: ELEVENLABS_ENV=/path/to/.env.local npm run vo   (the SDK reads ELEVENLABS_API_KEY)
import { ElevenLabsClient } from "@elevenlabs/elevenlabs-js";
import { mkdirSync, writeFileSync } from "node:fs";

const VOICE = process.env.VO_VOICE || "EXAVITQu4vr4xnSDxMaL"; // Sarah: soft, calm
export const LINES = [
  "This is Sedef. It means mother-of-pearl, the shimmer inlaid in fine instruments.",
  "Sedef is a complete guitar rig. Free, and open source.",
  "Four pedals, and three amps, from glassy clean to a singing lead.",
  "Put two microphones on the speaker, exactly where you'd place them in a studio.",
  "Tape echo that drifts and breathes. Reverb as deep as you like.",
  "And one tool most rigs leave out: intonation.",
  "Tune the open string, play the twelfth fret, and Sedef tells you which way to move the saddle, and how far.",
  "VST3, AU, CLAP, or standalone. On Mac, Windows and Linux.",
  "Sedef. Sound beautiful. Play in tune.",
];

const client = new ElevenLabsClient();
const text = LINES.join("\n\n");
const res = await client.textToSpeech.convertWithTimestamps(VOICE, {
  text,
  modelId: "eleven_v4",
  outputFormat: "mp3_44100_128",
  voiceSettings: { stability: 0.6, similarityBoost: 0.75 },
});

mkdirSync("assets", { recursive: true });
writeFileSync("assets/vo.mp3", Buffer.from(res.audioBase64, "base64"));

// Characters -> words, then each line's start and end.
const a = res.alignment;
const words = [];
let cur = null;
for (let i = 0; i < a.characters.length; i++) {
  const ch = a.characters[i];
  if (/\s/.test(ch)) {
    if (cur) words.push(cur);
    cur = null;
    continue;
  }
  if (!cur) cur = { w: "", start: a.characterStartTimesSeconds[i], end: 0 };
  cur.w += ch;
  cur.end = a.characterEndTimesSeconds[i];
}
if (cur) words.push(cur);

const lines = [];
let wi = 0;
for (const line of LINES) {
  const n = line.split(/\s+/).length;
  const ws = words.slice(wi, wi + n);
  lines.push({ text: line, start: ws[0].start, end: ws[ws.length - 1].end, words: ws });
  wi += n;
}
writeFileSync("assets/vo-align.json", JSON.stringify({ voice: VOICE, lines }, null, 1));
for (const l of lines) console.log(l.start.toFixed(2), l.end.toFixed(2), l.text);
