# TAPE·FOUR

A four-track cassette multitracker for Windows. It records onto a virtual tape, uses ASIO audio interfaces, and plays VST3 instruments as if they were plugged into the deck.

It works like the real thing: four mono tracks, one direction, a C-46, C-60 or C-90 of tape, and **no undo**. Recording erases whatever was on the tape under the armed tracks.

## Getting it

Open the repository's **Actions** tab, pick the latest green **Build Windows** run, and download **TapeFour-windows-x64**. Unzip it and run `TapeFour.exe`. Nothing needs installing.

Windows may warn that the program is from an unknown publisher, because it isn't code-signed. Choose **More info → Run anyway**.

## First run

1. Click the amber display at the top right (**SETUP ›**). Under **Audio**:
   - choose **ASIO** and your interface's driver;
   - choose the sample rate and buffer size.

   Under **MIDI & Deck**:
   - under **Controllers in**, tick the controllers you want to use, and tick **MPE** for MPE controllers (with their bend range);
   - under **Hardware synths out**, tick the MIDI outputs your synths are on, and give each a name for the deck if you like.

   If an ASIO driver is installed, TapeFour picks it on first start. Every change is saved straight away. If the saved driver can't be opened at start-up (for example, another program has it), the deck says so and keeps your choice for next time.
2. To use your own instruments, go to the **Instruments** tab and choose **Options… → Scan for new or updated VST3 plug-ins**. The program also has two built-in instruments, a poly synth and a tine piano, so you can start without any plug-ins.
3. Close the setup window. You're at the deck.

## Using the deck

| | |
|---|---|
| **Plug something into a track** | Right-click the channel strip, or click its amber display, to open the track's patch card. Choose **Audio in**, **Instrument**, **Hardware synth** or **Bounce**, then what plays it and where its audio comes from. |
| **Record** | Press **REC ARM** on the track(s), then **REC**. To punch in, press **REC** while playing; press **PLAY** to punch out. |
| **Play an instrument** | On the patch card, **Play with** chooses the controller. **Any (when armed)** means any controller plays the track while it is armed; naming a controller ties it to that track, so different keyboards can play different tracks at once. Without a keyboard, the computer keys A W S E D F T G Y H U J K play notes and Z / X change octave. |
| **Hardware synth** | Choose **Hardware synth** on the patch card, the controller that plays it, the MIDI output (**Send to**) and channel, and the input its audio comes back on (**Listen to**). Tracks are mono, so a stereo synth can come back on a pair of inputs, mixed to mono. A track plays either a VST/built-in instrument or a hardware synth, never both. |
| **Bounce** | On the destination track's patch card choose **Bounce** and tick the tracks to mix from tape. Balance them with their own EQ and faders, arm the destination and record. |
| **Counter** | **RESET** sets 000 where the tape is now. **RTZ** (or the Home key) rewinds to 000. |
| **Tapes** | **EJECT** (tape stopped) inserts a new blank C-46/C-60/C-90 or loads one from the tape shelf. |
| **Label and inlay** | Double-click the cassette label to write on it. Click the inlay beside the cassette to take it out of the case and write the title, songs with their counter numbers, what's on each track, NR, speed and date. All of it is saved with the tape. |
| **Deck with the tape** | Every tape remembers the deck it was recorded on: sources, instruments and their settings, MIDI routing, EQ, pan, faders, master, speed and pitch. Tick **Recall the deck with each tape** (Setup → MIDI & Deck) to have loading a tape put the deck back that way. |
| **Keys** | Space = play/stop, R = record, Home = return to zero, Esc = close a card. |

### MPE controllers (Osmose, Seaboard, LinnStrument…)

MPE is set per controller, under **Setup → MIDI & Deck**. Controllers that are known to be MPE (Osmose, Seaboard, LinnStrument and others) start with it ticked. A track played by an MPE controller tells its instrument about the MPE zone: lower zone, 15 member channels, and that controller's per-note pitch-bend range (default 48, as on the Osmose). Each note's bend, pressure and timbre (CC74) then reach the instrument separately. On a hardware synth track, MPE passes through untouched on its own channels.

- **Built-in instruments:** they respond directly. Pressure opens the filter and swells the level, and timbre moves the filter.
- **VST3 instruments:** the plug-in has to support MPE. Some need MPE switched on in their own settings too.

Type the **bend range** to match what the controller sends. Ordinary keyboards don't need MPE ticked.

### Levels

Hosted VST3 instruments come in 12 dB lower than their own output. That puts a full-level synth around the deck's 0 VU, as if it were plugged into a line input. Use the track's **GAIN** knob to trim further.

### Monitoring

With the default **Auto** setting:

- An armed track lets you hear its input while stopped or recording, and the tape while playing back, as on a real deck.
- Unarmed tracks play the tape.

### Speed, pitch and noise reduction

- **1⅞ ips** is normal cassette speed. **3¾ ips** sounds better but halves the recording time.
- Everything on the tape remembers the speed it was recorded at, so playing it at the other speed shifts it an octave. A track's display shows the shift in semitones.
- **PITCH** moves the tape speed by up to ±12%.
- **NOISE REDUCTION** encodes while recording and decodes on playback. The **NR** lamp on each track shows how the tape under the head was recorded. It flashes red when the switch doesn't match.

### Tape character

**Setup → Tape** sets hiss, wow, flutter, saturation, HF loss, track crosstalk, dropouts and speed drift. You can also choose the noise reduction type (2:1 compander, sliding band B or C) and the tape formulation (Type I or II). The presets go from **Clean** to **Chewed**.

## Where things are kept

- **Tapes**: `Documents\TapeFour\Tape Shelf\`, one folder per tape. Tapes are saved automatically whenever the transport has been stopped for a second, and on eject and on quit.
- **Settings**: `%APPDATA%\TapeFour\TapeFour.settings`. This holds the audio device, MIDI controllers and synths, track sources (including each plug-in's state), knob positions and tape character.

## Building it yourself

You need Visual Studio 2022 or later with the C++ workload, and CMake 3.22 or later.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

JUCE 8.0.11 is downloaded during configuration. It includes the ASIO headers (Steinberg made ASIO available under GPLv3) and the VST3 SDK.

Add `-DTAPEFOUR_TESTS=ON` to also build two headless test programs, `EngineTest` and `MixerTest`. They check the recording and playback paths: pitch and level, noise reduction, tape speed, punch-in, bounce, monitoring, the built-in synth via MIDI and MPE, per-track controllers, hardware synth routing, deck recall, settings surviving a restart, and saving and loading tapes.

## Licence

TAPE·FOUR is released under the **GNU GPL v3** (see `LICENSE`), because it uses ASIO under Steinberg's GPLv3 option and JUCE under its GPL option.

The embedded fonts (Barlow Condensed, Share Tech Mono, Caveat) are under the SIL Open Font License (`Resources/fonts/OFL.txt`).
