# TAPE·FOUR

A four-track cassette multitracker for Windows. It records onto a virtual tape, uses ASIO audio interfaces, and plays VST3 instruments as if they were plugged into the deck.

It works like the real thing: four mono tracks, one direction, a C-46, C-60 or C-90 of tape, and **no undo**. Recording erases whatever was on the tape under the armed tracks.

## Getting it

Open the repository's **Actions** tab, pick the latest green **Build Windows** run, and download **TapeFour-windows-x64**. Unzip it and run `TapeFour.exe`. Nothing needs installing.

Windows may warn that the program is from an unknown publisher, because it isn't code-signed. Choose **More info → Run anyway**.

## First run

1. Click the amber display at the top right (**AUDIO SETUP ›**). Under **Audio & MIDI**:
   - choose **ASIO** and your interface's driver;
   - choose the sample rate and buffer size;
   - under **MIDI inputs**, tick only the controllers you want to play.

   If an ASIO driver is installed, TapeFour picks it on first start. Every change is saved straight away. If the saved driver can't be opened at start-up (for example, another program has it), the deck says so and keeps your choice for next time.
2. To use your own instruments, go to the **Instruments** tab and choose **Options… → Scan for new or updated VST3 plug-ins**. The program also has two built-in instruments, a poly synth and a tine piano, so you can start without any plug-ins.
3. Close the setup window. You're at the deck.

## Using the deck

| | |
|---|---|
| **Plug something into a track** | Right-click the channel strip, or click its amber display. Pick an audio input, an instrument, or tracks to bounce from. |
| **Record** | Press **REC ARM** on the track(s), then **REC**. To punch in, press **REC** while playing; press **PLAY** to punch out. |
| **Play an instrument** | Arm its track. Your MIDI keyboard plays the armed instrument track, just like a synth plugged into that channel. Without a keyboard, the computer keys A W S E D F T G Y H U J K play notes and Z / X change octave. |
| **Bounce** | Right-click the destination track, choose **Bounce from tape → Track n** for each source, balance the sources with their own EQ and faders, arm the destination and record. |
| **Counter** | **RESET** sets 000 where the tape is now. **RTZ** (or the Home key) rewinds to 000. |
| **Tapes** | **EJECT** (tape stopped) inserts a new blank C-46/C-60/C-90 or loads one from the tape shelf. |
| **Label and track sheet** | Double-click the cassette label to write on it. Type on the track sheet. Both are saved with the tape. |
| **Keys** | Space = play/stop, R = record. |

### MPE controllers (Osmose, Seaboard, LinnStrument…)

**MPE** is on by default. Every instrument gets told about the MPE zone: lower zone, 15 member channels, and your per-note pitch-bend range (default 48, as on the Osmose). Each note's bend, pressure and timbre (CC74) then reach the instrument separately.

- **Built-in instruments:** they respond directly. Pressure opens the filter and swells the level, and timbre moves the filter.
- **VST3 instruments:** the plug-in has to support MPE. Some need MPE switched on in their own settings too.

Set **MPE pitch-bend range** to match what your controller sends. With MPE off, ordinary keyboards work as before.

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

**Audio Setup → Tape** sets hiss, wow, flutter, saturation, HF loss, track crosstalk, dropouts and speed drift. You can also choose the noise reduction type (2:1 compander, sliding band B or C) and the tape formulation (Type I or II). The presets go from **Clean** to **Chewed**.

## Where things are kept

- **Tapes**: `Documents\TapeFour\Tape Shelf\`, one folder per tape. Tapes are saved automatically whenever the transport has been stopped for a second, and on eject and on quit.
- **Settings**: `%APPDATA%\TapeFour\TapeFour.settings`. This holds the audio device, track sources (including each plug-in's state), knob positions and tape character.

## Building it yourself

You need Visual Studio 2022 or later with the C++ workload, and CMake 3.22 or later.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

JUCE 8.0.11 is downloaded during configuration. It includes the ASIO headers (Steinberg made ASIO available under GPLv3) and the VST3 SDK.

Add `-DTAPEFOUR_TESTS=ON` to also build two headless test programs, `EngineTest` and `MixerTest`. They check the recording and playback paths: pitch and level, noise reduction, tape speed, punch-in, bounce, monitoring, the built-in synth via MIDI and MPE, settings surviving a restart, and saving and loading tapes.

## Licence

TAPE·FOUR is released under the **GNU GPL v3** (see `LICENSE`), because it uses ASIO under Steinberg's GPLv3 option and JUCE under its GPL option.

The embedded fonts (Barlow Condensed, Share Tech Mono, Caveat) are under the SIL Open Font License (`Resources/fonts/OFL.txt`).
