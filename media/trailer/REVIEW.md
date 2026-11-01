# Review: Sedef Amp launch trailer

48.0 s, 1920 × 1080, 30 fps. Storyline A, "The inlay" (`STORYLINES.md`), built to `SPEC.md`.
Delivery render: `renders/sedef-trailer.mp4` (gitignored); repo copy: `docs/media/sedef-trailer.mp4`.

## Creative calls

- **Nacre is the door between worlds.** The fretboard's 12th-fret dot fills the frame, and the
  same conic nacre shrinks onto Pearl's gain knob cap (7.0 s). Fathom's Decay cap grows into the pale
  formats world (24.8 s), which closes back into the header's Output knob cap (31.0 s). The
  storyline's one carrier, mother-of-pearl, carries every world change except the last.
- **Tabs whip, clicks cut.** Moving to the next section tab (pedals → amp, cab → FX) is a
  horizontal whip with both pages 2000 px apart, so the blur always shows a page. A real click
  causes every other change: Pearl's footswitch, the Abyss card, the Cab tab, Mic A, Intonation.
- **The mic drag uses only real pixels.** The five `cab-mic-*` captures swap underneath as Mic A
  passes the midpoint between states, so the Position knob reads 1.0 → 9.0. The marker moves
  continuously over a strip of the same cone cut from captures 1 and 5 (each is clean where the
  other has its marker), and the marker itself is cut from captures 1 and 3. At the end of the drag
  the real capture 5 takes over.
- **End card on the fretboard.** The star mark (`assets/logo.svg`) is routed and set into the
  12th-fret space where the double dot was, the wordmark (`assets/wordmark.svg`) wipes in beside
  it, using the brand lockup's proportions (star 92, gap 28.8, text 0.92 of the star), and the URL
  holds 2.1 s.
- Not built: B "The sour octave" (needs the viewer to hear beating, lost on a phone speaker) and
  C "Type on the groove" (beat cuts leave no time for the mics and the saddle advice).

## Changes from SPEC.md

1. The full window at 10.7 s is shown at 1.38× (z 0.86), not 1.6–2×: at 1.6× the 760-px window is
   1216 px tall and would be cropped. Every other product framing is 1.6×–4.8×, and the knob caps
   at the world changes reach 6.4×.
2. "cut to amp page" is a whip to the next tab; the same whip goes cab → FX. Amp → cab is a click
   on the Cab tab (not in the SPEC).
3. The push-in is on the UI's real "1.42 mm", not "1.4 mm".
4. The closing line is two lines ("Free & open source" over the URL) inside the 12th-fret space,
   not one line with "·", so it stays clear of the fret wires. The 12th-fret double dot is gone in
   S9; the star takes its place.
5. Voiceover clips start at the brief's table times. Each clip carries 50 ms before its first
   word, so a word lands at line start + 0.05 s + its offset (line 1 has no pre-roll, since it
   starts at 0 in the take). Every beat below was timed to those real word times.
6. The 4 px motion blur is a uniform blur on a screen-space wrapper (`#xx-lens`), peaking mid-move.
   It is not directional.
7. The formats leave at 30.8 s with a blur-out, so the pale ground is empty when it closes into the
   knob. "Linux" inks on its word (30.17 s) and is on screen, inked, for 0.7 s; the grey line
   arrives on "On" (28.73 s).

## Word timings against beats (film time = clip start + 0.05 + word offset)

| Word | Time | Beat |
|---|---|---|
| "Sedef" (L1) | 0.98 | letters routed and set 0.68–1.6 s, 3 frames apart |
| "mother-of-pearl" | 2.58 | shimmer through the letters from 2.55 s |
| "inlaid" | 4.50 | camera to the 12th fret, 4.5–5.1 s |
| "pedals" (L3) | 11.29 | Pearl clicked 11.06 s, LED on 11.12 s |
| "amps" | 12.33 | whip midpoint 12.35 s |
| "singing" | 14.49 | Abyss clicked 14.43 s, Abyss on 14.49 s |
| "speaker" (L4) | 16.85 | punch-in on the cone lands 16.6 s |
| "exactly" | 17.57 | Mic A drag 17.08–18.48 s |
| "Tape" (L5) | 20.05 | whip lands on Drift 20.0 s |
| "Reverb" | 22.77 | move to Fathom 22.65–23.25 s |
| "VST3" / "AU" / "CLAP" / "standalone" (L8) | 25.05 / 26.33 / 27.05 / 27.69 | each format lands 0.03 s before its word |
| "intonation" (L6) | 33.49 | click 33.41 s, workbench opens 33.47 s |
| "play" (L7) | 36.15 | 12th-fret capture 36.15 s |
| "and Sedef tells you" | 37.47 | result and saddle advice 37.47 s |
| "how far" | 40.07–40.43 | push-in on "1.42 mm" 40.05–40.6 s |
| "Sedef" (L9) | 44.15 | star set 44.12–44.67 s |

## What was measured, what was only looked at

- `npx hyperframes check --json`: ok. Lint, runtime, layout, motion, contrast and craft report 0
  errors and 0 warnings. Four layout infos remain (`text_occluded` on the inlay letters, whose
  padded boxes overlap on purpose so the italic overhang gets its nacre).
- `snapshot --review` sheets (beats, eight seams, phone width) and full-resolution stills at every
  SPEC QA time (0.0, 1.2, 10.8, 11.3, 14.6, 17.5, 27.5, 33.6, 38.5, 44.6, 47.5) plus 7.3, 7.7,
  9.0, 12.4, 13.5, 19.8, 21.8, 23.3, 24.1, 24.75, 29.8, 31.1, 36.5, 40.5, 41.9, 42.5, 43.7. Decoded
  frames from the draft render match the snapshots.
- Sound was measured, not heard. `ffmpeg ebur128` on the delivery render: −15.8 LUFS integrated,
  −1.4 dBTP true peak, LRA 4.2 LU. `hyperframes audio`: every click and whoosh onset is within one
  frame of its picture, no clipping. The last 0.73 s is silent: the music's own fade ends at about
  47.3 s and the last word ends at 47.5 s.
- The independent review by a second agent did not run (it hit a usage limit), so these notes are
  mine alone.

## Fixed in review

- P0 · t=7–44 s · vo-2…vo-9 were silent: the fade-out was timed on the source clock → recut with
  `-ss/-to` before `-i` (`tools/cut-vo.mjs`). The first draft measured −22.1 LUFS; now −15.8.
- P0 · t=12.33, 19.8 s · each whip showed a frame of empty ink at its fastest point → outgoing and
  incoming pages travel together 2000 px apart. Cab → FX is split across the seam as the two halves
  of one 0.4 s power3.inOut, so speed matches at the cut.
- P1 · t=2.7 s · the contrast audit read the inlay's dark routing layer as text → marked
  `data-layout-ignore` (it is texture; the nacre letters on top are the read).
- P1 · t=45.8–48 s · the URL held 1.9 s, under the 2 s floor for a closing line → the two lines now
  arrive at 45.8 and 45.9 s, so the URL holds 2.1 s.
- P1 · t=14.4, 15.3, 17, 33.4 s · the cursor tip covered "Abyss", "Cab", "A" and "Intonation" →
  each click point moved to the lower right of its control.
- P1 · t=17.5 s · Mic A had barely moved at the QA frame → the drag starts 0.12 s earlier; at
  17.5 s it reads Position 3.0.
- P2 · t=16.6–19.8 s · sliced cabinet-list labels at the left edge → speaker framing centred at
  x 600.
- P2 · t=24.0 s · the plate push-in cut the "Fathom" title → framed 20 px higher.
- P2 · nacre read as plain white → less white highlight, two colour cycles in the top conic layer.

## Weakest shots, and what I would change with more time

- **41.4–43.4 s, the summary.** The table's text is about 27 px at 1080p and unreadable at phone
  width; the headline "2 of 6 strings in tune." carries it. More time: a 1.8× framing on the top
  three rows, then the push into the diagram.
- **7.0–7.8 s, Pearl's knob at z 4.** The capture is upscaled about 2.1× and slightly soft while
  the nacre clears. A dedicated 6× capture of the pedals page would fix it (`captures.mjs` takes a
  scale).
- **The intonation section holds still images.** The tuner progress bar and the capture ring do
  not fill. Extra captures (`tuneProgress` and `captureProgress` at a few values, through the
  `?demo` hook) would let them move with no invented UI.
- **31.0–32.6 s, the header.** Below the header, half the frame is the FX page.
- **Phone width.** The UI shots are small at 360 px, as product UI always is; the voice carries
  them. The URL is about 9 px at phone width.

## Kept on purpose

- The pale formats world with Bodoni Moda italic resembles the generic "cream ground, italic serif"
  default. It is in the SPEC, the ground is the brand's nacre, and Bodoni Moda italic is the face
  the UI uses for every module name. It has none of that default's other tells (numbered labels,
  pills, mono caps).
- Clicks are one take, and so are whooshes: Hopla's `click.mp3` and `click-soft.mp3` are
  byte-identical, as are `whoosh.mp3` and `whoosh-short.mp3`. Volumes vary (0.3–0.6) instead.
- The whips have no whoosh. The storyline allows two whooshes, and both go to world changes.
