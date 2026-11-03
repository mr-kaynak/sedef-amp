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
  other has its marker). Since 0.3.0 the marker itself is `cab-mic-a`, the real marker shot alone
  on a transparent ground. At the end of the drag the real capture 5 takes over.
- **End card on the fretboard.** The camera settles at 2× on the 12th-fret space, now 1176 px
  wide. The star mark (`assets/logo.svg`) is routed and set into it where the double dot was, and
  the wordmark (`assets/wordmark.svg`) wipes in beside it in the brand lockup's proportions (star
  150 px, gap 47, text 0.917 of the star, 28% of the frame width). "Free & open source" and the URL
  are 60 px Archivo, a 41 px cap height, and the URL holds 2.1 s.
- **No word is ever cut by the frame edge on a held framing.** Every word and line of every UI
  state was measured in the built UI (Playwright, the same states as `pnpm captures`). Each held
  camera framing was then chosen so that every line is either wholly in frame or wholly out of it,
  as is each grouped block (the Gate panel, the pedals, the mic strips, the status bar, the
  intonation step row, the tip box, the string list).
- Not built: B "The sour octave" (needs the viewer to hear beating, lost on a phone speaker) and
  C "Type on the groove" (beat cuts leave no time for the mics and the saddle advice).

## Changes from SPEC.md

1. The full window at 10.7 s is shown at 1.38× (z 0.86), not 1.6–2×: at 1.6× the 760-px window is
   1216 px tall and would be cropped. Every other product framing is 1.6×–4.8×, and the knob caps
   at the world changes reach 6.4×.
2. "cut to amp page" is a whip to the next tab; the same whip goes cab → FX. Amp → cab is a click
   on the Cab tab (not in the SPEC). The amp page holds one full-width framing for Lagoon, the
   Abyss click and the Cab tab: a tighter Lagoon framing would have cut the amp cards' text.
3. The push-in is on the UI's real "1.42 mm", not "1.4 mm".
4. The closing line is two lines ("Free & open source" over the URL) inside the 12th-fret space,
   not one line with "·", so it stays clear of the fret wires. The 12th-fret double dot is gone in
   S9; the star takes its place. S9's strings sit closer together (board y 400 and 900), so both
   stay in frame at 2×.
8. S1's push at 4.5 s frames the name and the 12th-fret double dot together, rather than the 12th
   fret alone, which would have cut the name at the frame edge.
9. The Intonation click (33.47 s) is a hard cut to the whole window, not a pull-back: pulling back
   through the half-open workbench showed cut body text. The intonation scene starts on that
   framing and moves in on "Tune" (34.45 s). The open-string and 12th-fret steps share one framing,
   so the workbench changes in place.
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
| "inlaid" | 4.50 | camera in on the name and the double dot, 4.5–5.1 s |
| "pedals" (L3) | 11.29 | Pearl clicked 11.06 s, LED on 11.12 s |
| "amps" | 12.33 | whip midpoint 12.35 s |
| "singing" | 14.49 | Abyss clicked 14.43 s, Abyss on 14.49 s |
| "speaker" (L4) | 16.85 | punch-in on the cone lands 16.6 s |
| "exactly" | 17.57 | Mic A drag 17.08–18.48 s |
| "Tape" (L5) | 20.05 | whip lands on Drift 20.0 s |
| "Reverb" | 22.77 | move to Fathom 22.65–23.25 s |
| "VST3" / "AU" / "CLAP" / "standalone" (L8) | 25.05 / 26.33 / 27.05 / 27.69 | each format lands 0.03 s before its word |
| "intonation" (L6) | 33.49 | click 33.41 s, workbench opens 33.47 s |
| "Tune" (L7) | 34.75 | camera in on string 6, 34.45–35.05 s |
| "play" | 36.15 | 12th-fret capture 36.15 s, same framing |
| "and Sedef tells you" | 37.47 | result and saddle advice 37.47 s |
| "how far" | 40.07–40.43 | push-in on "1.42 mm" 40.05–40.6 s |
| "Sedef" (L9) | 44.15 | star set 44.12–44.67 s |

## What was measured, what was only looked at

- `npx hyperframes check --json` (after the polish pass): ok. Lint, runtime, layout, motion,
  contrast and craft report 0 errors and 0 warnings. Four layout infos remain (`text_occluded` on the inlay letters, whose
  padded boxes overlap on purpose so the italic overhang gets its nacre).
- `snapshot --review` sheets (beats, eight seams, phone width) and full-resolution stills at every
  SPEC QA time (0.0, 1.2, 10.8, 11.3, 14.6, 17.5, 27.5, 33.6, 38.5, 44.6, 47.5) plus 7.3, 7.7,
  9.0, 12.4, 13.5, 19.8, 21.8, 23.3, 24.1, 24.75, 29.8, 31.1, 36.5, 40.5, 41.9, 42.5, 43.7. After
  the polish pass, every changed shot again (5.3, 8.6, 11.6, 13.5, 17.5, 21.8, 23.5, 24.3, 24.6,
  31.3, 33.6, 34.3, 35.3, 36.8, 38.5, 40.0, 40.8, 42.6, 44.6, 46.5, 47.5) and a new review set.
  Decoded frames from the delivery render match the snapshots.
- Edge clipping was measured, not eyeballed: word boxes from the built UI were checked against
  each held framing's view rectangle. The checks are in this session's scratch scripts, not in
  the repo.
- Sound was measured, not heard. `ffmpeg ebur128` on the delivery render (20.8 MB, rendered in
  62 s): −15.8 LUFS integrated, −1.4 dBTP true peak, LRA 4.2 LU. The repo copy, re-encoded at
  CRF 20, is 12.2 MB, with SSIM 0.995 against the render and the same loudness. `hyperframes audio`: every click and whoosh onset is within one
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
- P2 · t=16.6–19.8 s · sliced cabinet-list labels at the left edge → speaker framing moved right
  (since refined again, below).
- P2 · t=24.0 s · the plate push-in cut the "Fathom" title → framed 20 px higher.
- P2 · nacre read as plain white → less white highlight, two colour cycles in the top conic layer.

## Polish pass (after bf50827)

- P0 · t=33.5–34.2 s · the pull-back after the Intonation click showed "second and the" cut at the
  left edge → on the click, a hard cut to the whole window, held until the intonation scene moves
  in on string 6.
- P0 · held framings across the film cut words at the frame edge → every framing re-chosen against
  measured word boxes (see Creative calls):
  - 5.1 s: "Sedef" at the left edge;
  - 8.5 s: "−72.0 dB", "0 st";
  - 11.6 s: "Gate", "Transpose", "kHz", "DSP";
  - 12.6–15.4 s: "Lagoon", "glassy", "voicings";
  - 16.6 s: "Level 0.0 dB";
  - 21.6 s: "Note 1/4";
  - 24.0 s: "Pre-delay";
  - 24.4 s: "gap,";
  - 35 s: "Instrument";
  - 36.6 s: "Instrument";
  - 37.9 s: "Tune";
  - 39.8 s: "the", "0.06", "screw,", "than";
  - 40.6 s: "saddle", "Tail";
  - 41.9 s: "Instrument", "Close".
  The changes: the pedals framing gains the tabs and the Gate/Transpose panels; the amp page is
  full width; the cab framing ends clear of Mic A's Level knob; and the Decay cap sits right of
  centre, with the nacre opening from its real position.
- P1 · t=45.8–48 s · the URL was 30 px (about 9 px at phone width) → 60 px with a 41 px cap height,
  on a 2× end card where the 12th-fret space is 1176 px wide. "Free & open source" matches it.
- P1 · t=41.4–43.4 s · the summary table read at about 27 px → framed at 2.065×, where its
  smallest text (11 px headers) is 36 px and the rows are 39 px. All six strings, the headline and
  the buttons stay in frame; the string list's bare cent values are left out.
- Skipped · t=7.0–7.8 s · Pearl's knob is still soft. `captures.mjs` has no scale flag (it is
  fixed at 3×), and zooming out at the knob would have cut the Transpose panel's text.

## 0.3.0 re-render

Same story, voice, music, clip placements and word timings; only the picture changed. The shots
were retaken from the 0.3.0 UI (`pnpm build && pnpm captures`), and every framing, click target
and nacre disc was measured again from the built UI's DOM, in the states the captures shoot. Each
held framing was checked against the boxes of every word, line and painted element (old → new,
as `{x, y, z}` in design px):

- **What the new UI moved, or what the 0.2.0 render already cut:**
  - 7.6 s · Pearl's Gain cap is at (417, 308.2) and 26 px: K `{417, 310, 4}` → `{417, 308.2, 4}`,
    disc 170 → 166.4 px.
  - 8.5–10 s · Ripple was cut at the right edge and Gate's bottom edge showed at the top: P
    `{467, 486, 1.35}` → `{460, 483, 1.453}`, Tide, Pearl and Coral whole.
  - 11.7–12.2 s · the new Doubler switch was cut at the top and the Pedals underline at the left:
    R `{600, 387, 1.04}` → `{600, 399, 1.02}`, from just below the header.
  - 12.6–16 s · the amp head ran off the bottom: A `{600, 337.5, 1}` → `{600, 345.1, 0.978}`,
    shared with the cab's first framing. The Abyss click moves to the swatch of the four-card row:
    (834, 168) → (642, 172).
  - 16.6–19.6 s · the old speaker framing cut the speaker and both mic strips: S
    `{606, 400, 1.73}` → `{732.5, 422, 1.32}`, the whole speaker, the speakers' map, both mics and
    the caption.
  - 21.6–22.6 s · "Note 1/4" was cut at the left and the Doubler switch at the top: D2
    `{844, 250, 1.8}` → `{866.03, 278.7, 2.02}`, the readout and the right-hand knobs.
  - 23.3 s · the new Doubler panel and Drift's head buttons were cut: FA `{413, 509, 1.5}` →
    `{402, 541.8, 1.55}`, Fathom whole.
  - 24.3 s · Low cut was cut at the right and the plate graph at the left: DK `{417, 599, 2.97}` →
    `{476, 597.2, 3.75}`, Mix to Low cut whole; disc 126.4 → 156 px, now left of centre.
  - 31–33.5 s · "Input" showed as "ut": H `{925, 153.41, 2.2}` → `{903.95, 159.75, 2.113}`, Input
    knob to Settings; the Output cap is at (801.3, 25), 18.72 px (disc 68 → 63.3 px).
  - 35–37.4 s · 1–2 px of the workbench steps and Close showed at the top: I0 `{700, 422, 1.25}` →
    `{704, 444, 1.25}`.
  - 38–39.2 s · the string list sat 1.5 px from the left edge: I2 x 414 → 404.5, y 398 → 392.2.
  - 39.8 s · the saddle plate was cut at the bottom: I3 `{896, 342, 1.99}` → `{935.3, 385.6, 2.35}`,
    the whole diagram.
  - 40.6 s · "Tail →" sat 5 px from the right edge: I4 x 973 → 980.
  - 41.9–43.4 s · the summary rows are now two lines tall, so the old framing cut the buttons: I5
    `{494, 350, 2.065}` → `{503, 329, 2.15}`, the headline and the table, buttons out; the
    smallest text is 38 px.
- **The mic drag.** The mics now have an azimuth, and Mic A's default of 60° would make the drag a
  diagonal towards 2 o'clock; at 90° it would brush Mic B's ring at Position 5. The captures set
  `micAAzimuth:-90` (9 o'clock), so `markerPoint` puts the five positions on one horizontal line,
  from (475.2, 394.9) on the dust cap to (340.8, 394.9) by the edge: a 134.4 px sweep to the left.
  The strip is x 316–408 from capture 1 and 408–499 from capture 5, y 371.5–418.5. The ring is
  unfilled, so the old marker cut from captures 1 and 3 would have carried the dust cap across the
  cone; the marker is now `cab-mic-a`.
- **Kept on purpose:** the 6.4× close-up on Pearl's cap at 7.6 s still crosses the neighbouring
  knobs (a 0.25 s pass-through, centred on the cap), the 4.8× push-in on "1.42 mm" crosses the plate
  and the strings, the saddle diagram's strings run off the right edge at 38.5 s and 42 s (their
  labels are whole), and Drift's panel is wider than its framings. Drift's readout framing has 16 px
  to the right edge: no zoom fits it between the tab row and Fathom with more.
- **Checks.** `npm run check`: 0 errors, 0 warnings, the same four `text_occluded` infos.
  `snapshot --review` (beats, eight seams, phone width), stills at every SPEC QA time, through
  the drag every 0.1–0.2 s, and at every click. Decoded frames from the render match the
  snapshots (SSIM 0.988–0.999). Render 21.0 MB in 57.5 s; repo copy 12.2 MB, SSIM 0.995;
  −15.8 LUFS, −1.4 dBTP, LRA 4.2 LU. The audio stream is bit-identical to the 0.2.0 copy's.

## Weakest shots, and what I would change with more time

- **7.0–7.8 s, Pearl's knob at z 4.** The capture is upscaled about 2.1× and slightly soft while
  the nacre clears. A 6× capture of the pedals page would fix it; `captures.mjs` would need a
  scale option first.
- **The intonation section holds still images.** The tuner progress bar and the capture ring do
  not fill. Extra captures (`tuneProgress` and `captureProgress` at a few values, through the
  `?demo` hook) would let them move with no invented UI.
- **31.0–32.6 s, the header.** Below the header, half the frame is the FX page.
- **Phone width.** The UI shots are small at 360 px, as product UI always is; the voice carries
  them. The summary and the end card are now the exceptions.
- **Frames in motion.** Mid-move frames (camera moves, whips, zooms through a cap) still pass
  words through the frame edge, as any camera move does. Only held framings were held to the rule.

## Kept on purpose

- The pale formats world with Bodoni Moda italic resembles the generic "cream ground, italic serif"
  default. It is in the SPEC, the ground is the brand's nacre, and Bodoni Moda italic is the face
  the UI uses for every module name. It has none of that default's other tells (numbered labels,
  pills, mono caps).
- Clicks are one take, and so are whooshes: Hopla's `click.mp3` and `click-soft.mp3` are
  byte-identical, as are `whoosh.mp3` and `whoosh-short.mp3`. Volumes vary (0.3–0.6) instead.
- The whips have no whoosh. The storyline allows two whooshes, and both go to world changes.
