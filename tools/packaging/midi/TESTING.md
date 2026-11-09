# Sedef MIDI test build (macOS)

Sedef MIDI turns your guitar's DI into MIDI notes, so you can play any instrument from the guitar.
This is a **test build**: unsigned, not an installer, and the plug-in codes may still change before
the first release (you may then have to put the plug-ins back on your tracks).

## What's in the zip

| File | What it is | Where it goes |
|---|---|---|
| `Sedef MIDI.app` | the standalone app: picks an audio input, sends notes through its own MIDI port "Sedef MIDI" | anywhere, for example `Applications` |
| `Sedef MIDI.component` | audio-effect AU, for the guitar track | `~/Library/Audio/Plug-Ins/Components` |
| `Sedef MIDI Instrument.component` | instrument AU, takes the guitar on its side chain (Logic) | `~/Library/Audio/Plug-Ins/Components` |
| `Sedef MIDI.vst3` | VST3 | `~/Library/Audio/Plug-Ins/VST3` |
| `Sedef MIDI.clap` | CLAP | `~/Library/Audio/Plug-Ins/CLAP` |

`~/Library` is the Library folder in your home folder. In Finder, choose **Go > Go to Folder…** and
paste the path.

## Install

1. Copy each file to the folder in the table.
2. The build is not signed, so macOS blocks it on first use. Open **Terminal** and run, for each
   file you copied (drag the file onto the Terminal window to type its path):

   ```sh
   xattr -dr com.apple.quarantine "<path>"
   ```

   Or open the app once, then choose **System Settings > Privacy & Security > Open Anyway**.
3. Make Logic see the new plug-ins: quit Logic, run

   ```sh
   killall -9 AudioComponentRegistrar
   ```

   and start Logic again. If a plug-in is missing, open **Logic Pro > Settings > Plug-in Manager**,
   select the Sedef plug-ins and choose **Reset & Rescan Selection**.
4. The first time the app opens, allow it to use the microphone: that is how macOS names any audio
   input, your interface included.

## Before you play

- **Level.** Play at your interface's usual setting; you don't need to change its gain. Sedef MIDI
  works the same with peaks anywhere from about −40 to −6 dBFS.
- **Fingers or a pick.** Play the way you always do: Sedef MIDI doesn't need a pick's click, and a
  finger touching the string just before it plucks it is not a note.
- **Gate.** Leave it on **Auto** (the default): it follows your input's own noise. Check that the
  **Signal** ring over the input meter fills when you play and empties when you're silent. If it
  stays lit while you're silent, switch the gate to **Manual** and drag its handle just above
  that. Notes only start above the gate.
- **Octave.** −1 by default (a bass sounds an octave under a guitar). For a bass part, playing
  higher on the neck with −2 often tracks faster and cleaner than playing on the low strings with −1.
- **Timing.** *Fastest* sends each note as soon as it is sure (the low E takes longest). *Steady*
  sends every note 38 ms after you pluck or pick it, so you can set the track delay to −38 ms and
  every note lands where you played it.
- **Panic** ends every note on all 16 channels if anything hangs.

## Checklist

Please go through these in order. For each step: what you should see, and what to send back if
you don't. A screenshot and a few words are always welcome.

**1. Standalone app into an instrument app**
Open `Sedef MIDI.app`, choose **Audio input…** and pick your interface's guitar input. Open any
instrument app that takes MIDI input and choose **Sedef MIDI** as its input.
*You should see:* the big note in Sedef MIDI follows what you play, and the instrument plays it.
*If not, send:* whether the meter moves, whether the note display moves, and whether "Sedef MIDI"
appears in the other app's MIDI inputs.

**2. Standalone app into Logic**
With the app running, create a software instrument track in Logic and record-arm it. Logic takes
MIDI from all inputs by default; to take only Sedef MIDI, choose it as the track's MIDI input.
*You should see:* the instrument plays as you play, and recording gives MIDI notes on the track.
*If not, send:* what Logic's control bar MIDI activity display shows while you play, and a
screenshot of the track's MIDI input setting.

**3. Effect AU on the guitar track**
On your guitar's audio track, insert **Audio Units > Sedef > Sedef MIDI**. Create a software
instrument track and set its input to **MIDI From** your guitar track (in the track header's input
menu or the inspector).
*You should see:* this may not work: Logic only offers MIDI From for instrument tracks whose plug-in
sends MIDI, and an effect on an audio track may not be offered. If it isn't, step 4 is the way in
Logic. Please tell us either way.
*If not, send:* a screenshot of the MIDI From menu.

**4. Instrument AU with the guitar on its side chain**
Create a software instrument track and load **AU Instruments > Sedef > Sedef MIDI (Instrument)**.
In its plug-in window, set **Side Chain** (top right) to your guitar track. Create a second
software instrument track with the instrument you want to hear, and set its input to **MIDI From**
the Sedef MIDI (Instrument) track.
*You should see:* the second track plays what you play, and records it.
*If not, send:* whether the Side Chain menu lists your guitar track, whether Sedef MIDI's meter
moves, and a screenshot of the MIDI From menu.

**5. Either AU into an IAC bus**
In **Audio MIDI Setup > Window > Show MIDI Studio**, open **IAC Driver** and tick **Device is
online**. In Sedef MIDI (effect or instrument), set **Also send to** to the IAC bus. On an
instrument track in Logic, choose the IAC bus as its MIDI input.
*You should see:* the instrument plays. This is the fallback when MIDI From doesn't work.
*If not, send:* the line under **Also send to** in Sedef MIDI.

**6. How it feels**
Play single notes on the low E and A strings in **Fastest**: does the delay bother you? Then switch
to **Steady**, set the instrument track's **Track Delay** to −38 ms (inspector, **Delay**), record
and look at where the notes land. Then play your own bass lines.
*Send back:* how the delay feels on low E and A in each mode, where recorded notes land against
the beat in Steady, and any wrong, missing or extra notes (which notes, which strings, how you
played them: plucked with a finger or picked, legato, muted, with open strings ringing). A short recording of the DI with
the MIDI it produced helps us most.

## Known limits

- Monophonic: one note at a time. A string left ringing under a clearly louder new note is
  usually fine; chords, or two strings ringing at a similar level, give short wrong notes between
  the right ones. Damp the strings you're not playing.
- In **Steady**, a note played softly over a string still ringing can land a little late. A note
  whose string you damp, or touch with the plucking finger while it still rings, just before you
  play it can land up to about 15 ms early.
- The very first note after Sedef MIDI starts comes about 60 ms after you play it: until then it
  can't tell a finger's touch from a soft pluck. Notes after a rest come as quickly as any other.
- Delay from the pluck or pick to the note is about 28 ms on the low E, 22 ms on the A, 17 ms on
  the D and 16 ms from the G string up, plus your audio buffer.
