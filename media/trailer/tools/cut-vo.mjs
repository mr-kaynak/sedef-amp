// Cuts the single voiceover take (assets/vo.mp3) into one clip per line, assets/vo-N.mp3, using
// the word timings in assets/vo-align.json: from 0.05 s before a line's first word to 0.15 s
// after its last (clamped to the take), with 10 ms / 40 ms edge fades. index.html places them.
// Run: npm run cut-vo   (needs ffmpeg on PATH; no API key)
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

const { lines } = JSON.parse(readFileSync("assets/vo-align.json", "utf8"));
const take = Number(
  execFileSync("ffprobe", ["-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", "assets/vo.mp3"], { encoding: "utf8" }),
);

lines.forEach((line, i) => {
  const ss = Math.max(0, line.start - 0.05);
  const to = Math.min(line.end + 0.15, take);
  const dur = to - ss;
  // -ss/-to before -i: the clip's own clock starts at 0, so the fades land on the clip.
  execFileSync("ffmpeg", [
    "-v", "error", "-y",
    "-ss", ss.toFixed(3), "-to", to.toFixed(3), "-i", "assets/vo.mp3",
    "-af", `afade=t=in:d=0.01,afade=t=out:st=${(dur - 0.04).toFixed(3)}:d=0.04`,
    "-c:a", "libmp3lame", "-q:a", "2",
    `assets/vo-${i + 1}.mp3`,
  ]);
  console.log(`vo-${i + 1}.mp3  ${ss.toFixed(2)}-${to.toFixed(2)} s  ${line.text}`);
});
