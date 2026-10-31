# SPEC

GOAL:    a guitarist finishes the film knowing Sedef is a complete, free rig that also fixes their
         guitar's intonation, and wants to try it.
OUTPUT:  HyperFrames, 1920 x 1080, 30 fps.
TIME:    48.0 s x 30 = 1440 frames. Music 84 bpm (21.4 frames per beat). Measured bed sections:
         sparse intro 0-10.7 s, groove 10.7-33.7 s, sparse 33.7-44.1 s, swell 44.1-48 s.
LOOK:    World 1 (fretboard): rosewood ground #2a1712 to #3d2219 with 6% grain, fret wire #b8b2a6
         with a 1 px highlight, nacre inlay = layered conic gradients (rose #f3d9df, aqua #cfe9e6,
         lilac #ddd5ee, gold #efe2bf) over #f4efe6, sheen angle driven by the timeline; one raking
         light (linear gradient, 12% white) sweeping left to right. No glow.
         World 2 (product): the real UI captures (assets/shots, 3600 x 2280) on the app's own ink
         #061119, shown at 1.6-2x of their 1200-wide design size; drop shadow 0 30 80 rgba(0,0,0,.55).
         World 3 (formats): pale nacre ground #f1ece3 with a 4% shell sheen; ink #0b1a24.
         Display: Bodoni Moda italic 500, 150-220 px, tracking -2%. Body: Archivo 500, 34-44 px.
         Accent: nacre only, on the inlay, the knob cap and the in-tune state.
OBJECTS: fretboard (rosewood, 4 frets visible, two strings, 12th-fret double-dot inlay), nacre inlay,
         cursor (macOS arrow, 64 px, white with 2 px ink stroke), the app window.
MOTION:  enter power3.out 0.5 s; camera moves power2.inOut 0.6 s with 4 px motion blur; exits
         power2.in 0.3 s; no idle drift on text. Staggers 3 frames.
SHOTS:   (VO line placements in index.html; L = voiceover line)
  S1 0.0-7.0   fretboard, light rakes across; "Sedef" inlay set into the wood on L1 "Sedef" (0.98 s);
               shimmer sweeps the inlay on "mother-of-pearl" (2.6 s); camera pushes into the
               12th-fret dot from 4.5 s.
  S2 7.0-10.7  nacre fills frame, match-cuts to the Pearl knob cap; pull back through the pedals page;
               the full window lands on the drum entry (10.7 s).
  S3 10.7-15.4 pedals: cursor clicks Pearl's footswitch on "four pedals" (11.0 s), LED lights; cut to amp
               page, Lagoon on "glassy clean" (13.2 s), Abyss on "singing lead" (14.4 s).
  S4 15.4-19.8 cab: punch-in on the speaker; cursor drags Mic A across the cone (5 real states).
  S5 19.8-24.8 FX: Drift on "tape echo" (20.0 s), Fathom on "reverb" (22.7 s).
  S6 24.8-31.0 formats world: VST3 / AU / CLAP / Standalone land on their words, then macOS · Windows · Linux.
  S7 31.0-34.4 back to the app header; cursor clicks Intonation on "intonation" (33.4 s); bed drops.
  S8 34.4-44.0 intonation: open string (34.7 s), 12th-fret capture (36.1 s), result + saddle advice
               (37.4 s), push-in on "1.4 mm" for "how far" (40.0 s), summary of all strings (41.4 s).
  S9 44.0-48.0 fretboard returns; the star mark is set into the 12th fret on "Sedef" (44.1 s), wordmark
               wipes in, then "Free & open source · github.com/mr-kaynak/sedef-amp" holds to the end.
BANNED:  dark void with floating cards; glow, bloom, particles; purple/blue gradients; every element
         fading and sliding up; titles that drift after landing; three cards in a row; the logo
         docked in a corner; a title over a dimmed screenshot; invented UI.
QA:      0.0 (thumbnail: fretboard, not black), 1.2 (inlay name readable), 10.8 (full window),
         11.3 (Pearl LED on), 14.6 (Abyss), 17.5 (mic mid-drag), 27.5 (formats legible at phone width),
         33.6 (Intonation clicked), 38.5 (saddle advice readable), 44.6 (mark), 47.5 (URL readable).
