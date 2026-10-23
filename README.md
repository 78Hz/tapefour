# TAPE·FOUR

A four-track cassette multitracker for Windows, macOS and Linux. It records onto a virtual tape, works with any audio interface the computer has (ASIO, WASAPI or DirectSound on Windows; CoreAudio on macOS; ALSA or JACK on Linux), and plays plug-in instruments and effects (CLAP, VST3 and LV2 everywhere, Audio Units on macOS, LADSPA on Linux) through a signal chain in front of each track, as if they were racked up in front of the deck.

It works like the real thing: four mono tracks, one direction, a C-46, C-60 or C-90 of tape, and **no undo**. Recording erases whatever was on the tape under the armed tracks.

## Manual

The owner's manual is [`Manual/TapeFour-Manual.pdf`](Manual/TapeFour-Manual.pdf): every control, the four cassettes, the tape switch and its mismatches, noise reduction, MIDI, and what to expect from each setting. Its source is `Manual/manual.html`; `python Manual/build_pdf.py` rebuilds it (needs Playwright with Chromium).

## Getting it

Open the repository's **Actions** tab, pick the latest green **Build** run, and download the one for your computer. Nothing needs installing. Each build has its own version number (0.2.*build*), shown in the window's title bar.

| Computer | Download | Then |
|---|---|---|
| Windows (64-bit) | **TapeFour-windows-x64** | Unzip and run `TapeFour.exe`. Windows may warn that it's from an unknown publisher, because it isn't code-signed: choose **More info → Run anyway**. |
| Mac (Apple silicon or Intel, macOS 11+) | **TapeFour-macos-universal** | Unzip and move `TapeFour.app` to Applications. It isn't notarised, so the first time, right-click it and choose **Open**, then **Open** again. Allow microphone access when asked: that's how macOS lets a program record from any audio input. |
| Linux (64-bit) | **TapeFour-linux-x64** | Extract the `.tar.gz` and run `TapeFour`. It needs ALSA (every distribution has it); JACK is used if a JACK server is running. |

## First run

1. Click the amber display at the top right (**SETUP ›**). Under **Audio**:
   - choose the audio system and your interface (see below);
   - choose the sample rate and buffer size.

   Under **MIDI & Deck**:
   - under **Controllers in**, tick the controllers you want to use, and tick **MPE** for MPE controllers (with their bend range);
   - under **Hardware synths out**, tick the MIDI outputs your synths are on, and give each a name for the deck if you like.

   On first start TapeFour opens the computer's usual audio device, or on Windows an ASIO driver if one is installed. Every change is saved straight away. If the saved driver can't be opened at start-up (for example, another program has it), the deck says so and keeps your choice for next time.
2. To use your own plug-ins, go to the **Plug-ins** tab, choose **Options…** and scan each format you use. A plug-in installed in several formats is offered once, in the best of them (CLAP, then VST3, AU, LV2, LADSPA). The program also has two built-in instruments (a poly synth and a tine piano) and three built-in effects (a plate, a spring reverb and a tape echo), so you can start without any plug-ins.
3. Close the setup window. You're at the deck.

### Audio systems

| System | Platform | Notes |
|---|---|---|
| **ASIO** | Windows | Lowest latency, with your interface's own driver. Best for overdubbing. |
| **Windows Audio** (WASAPI) | Windows | Works with any device, including built-in sound. *Exclusive Mode* and *Low Latency Mode* get the latency down. |
| **DirectSound** | Windows | The oldest; highest latency. For devices nothing else will open. |
| **CoreAudio** | macOS | Every Mac interface. Aggregate devices (made in Audio MIDI Setup) combine two interfaces. |
| **ALSA** | Linux | Talks to the hardware directly. Choose a `hw:` device for the lowest latency. |
| **JACK** | Linux | Appears when a JACK server (or PipeWire's JACK support) is running; TapeFour then shares the interface with other programs. |

However it's connected, recordings line up with what you heard: TapeFour corrects for the interface's reported latency (see **Overdub timing** in Setup if a driver misreports it).

## Using the deck

| | |
|---|---|
| **Layout** | Like a portastudio: the mixer runs down the left, the cassette and transport are in a column on the right. The window resizes and keeps the 16:9 shape. |
| **Keys** | Every key is the same hardware key with an LED: the LED lights, the key never changes colour. Red is record and arm, green is play and a matching tape, amber is a mode that's on or a mismatch, orange is mute, yellow is solo. EJECT, RESET and RTZ just act, so have none. The transport keys stay cream, with LEDs in REC and PLAY. |
| **Mixer** | Each strip: GAIN, HIGH shelf (full by 10 kHz), MID with a swept FREQ (250 Hz–5 kHz), LOW shelf (full by 100 Hz), PAN, meter and fader, and three LED keys: **MUTE**, **SOLO** and **ARM**. |
| **Mute and solo** | Mute silences a track everywhere (mix, bounces and live input), but a muted armed track still records. Solo only changes what you hear. Mute wins over solo; if every soloed track is muted the status display says so. |
| **Signal chain** | Click a track's display to drop down its chain: the inputs, up to eight blocks, then the tape. MIDI runs along the top, audio along the bottom. Instruments mix their sound in (two in a row layer), MIDI effects (an arpeggiator, PitchGrid) change the notes for every block after them, audio effects process with a MIX control, and an external instrument sends MIDI to a hardware synth and mixes its audio back in. Click **+** to add from a searchable list (ALL / INSTRUMENTS / MIDI FX / AUDIO FX), click a block's lamp to bypass it, drag to reorder, double-click to open its window. |
| **Inputs** | The first block: **Played by** (none, any controller, or a named one, so different keyboards can play different armed tracks), **Audio in** (an input, or a pair mixed to mono), or **Bounce from tape**. A track can take a controller and an audio input at once. |
| **Record** | Press **ARM** on the track(s), then **REC**. An armed track plays its live input through its chain; instruments sound only while armed. To punch in, press **REC** while playing; press **PLAY** to punch out. Without a keyboard, the computer keys A W S E D F T G Y H U J K play notes and Z / X change octave. |
| **Bounce** | Set the destination's inputs to **Bounce from tape** and tick the tracks to mix. Balance them with their own EQ and faders, arm the destination and record. The bounce goes through the destination's chain too. |
| **MIDI learn** | Press **LEARN**, click any knob, fader, key or transport button, and move a control on your keyboard; or right-click a control and choose Learn / Forget. Plug-in parameters are learned from the block in its chain. Knobs pick up (no jumps), endless encoders and 14-bit CCs are recognised, buttons toggle, and mapped messages don't also play the synth unless you ask. MMC transport buttons work without learning. Setup → MIDI & Deck lists every mapping. |
| **Counter** | **RESET** sets 000 where the tape is now. **RTZ** (or the Home key) rewinds to 000. |
| **Tapes** | **EJECT** (tape stopped) inserts a new blank tape (choose the cassette, then C-46, C-60 or C-90) or loads one from the tape shelf. |
| **Label and inlay** | Double-click the cassette label to write on it. Click the inlay beside the cassette to take it out of the case and write the title, songs with their counter numbers, what's on each track, NR, speed and date. All of it is saved with the tape. |
| **Deck with the tape** | Every tape remembers the deck it was recorded on: each track's inputs and signal chain with every plug-in's settings and MIDI mappings, MIDI routing, mute and solo, EQ, pan, faders, master, speed and pitch. Tick **Recall the deck with each tape** (Setup → MIDI & Deck) to have loading a tape put the deck back that way. |
| **Keys** | Space = play/stop, R = record, Home = return to zero, Esc = close the chain or inlay, or leave learn mode; Delete = remove the selected block. |

### MPE controllers (Osmose, Seaboard, LinnStrument…)

MPE is set per controller, under **Setup → MIDI & Deck**. Controllers that are known to be MPE (Osmose, Seaboard, LinnStrument and others) start with it ticked. A track played by an MPE controller tells its instrument about the MPE zone: lower zone, 15 member channels, and that controller's per-note pitch-bend range (default 48, as on the Osmose). Each note's bend, pressure and timbre (CC74) then reach the instrument separately. On a hardware synth track, MPE passes through untouched on its own channels.

- **Built-in instruments:** they respond directly. Pressure opens the filter and swells the level, and timbre moves the filter.
- **Plug-in instruments:** the plug-in has to support MPE. Some need MPE switched on in their own settings too.

Type the **bend range** to match what the controller sends. Ordinary keyboards don't need MPE ticked.

### Levels

Plug-in instruments come in 12 dB lower than their own output. That puts a full-level synth around the deck's 0 VU, as if it were plugged into a line input. Use the track's **GAIN** knob to trim further.

### Arming

- An armed track plays its live input, whatever the transport is doing; press RECORD to put it on tape.
- Unarmed tracks play the tape. To hear a take, disarm its track.

### Speed, pitch and noise reduction

- **1⅞ ips** is normal cassette speed. **3¾ ips** sounds better but halves the recording time.
- Everything on the tape remembers the speed it was recorded at, so playing it at the other speed shifts it an octave. A track's display shows the shift in semitones.
- **PITCH** moves the tape speed smoothly by up to ±12%, gliding to each new setting like a capstan motor so it bends rather than jumps.
- **NOISE REDUCTION** encodes while recording and decodes on playback. The **NR** lamp on each track shows how the tape under the head was recorded. It flashes red when the switch doesn't match.

### Cassettes

There are four cassettes. Each one behaves like a real tape, using published measurements (Roberson's comparison of retail cassettes, at 1⅞ ips without noise reduction, in dB relative to Dolby level, which is 0 VU on the meters).

| Cassette | Type | Measured on | MOL 400 Hz | SOL 10 kHz | S/N (A-wtd) | Mod. noise | Sensitivity |
|---|---|---|---|---|---|---|---|
| **BASIC** | I, normal | Sony HF | +2.4 | −10.3 | 54.3 | −36.1 | −1.0 |
| **SD** | I, normal | TDK AD | +6.2 | −8.2 | 60.3 | −44.3 | −0.3 |
| **XR** | II, chrome | TDK SA | +4.4 | −11.2 | 60.9 | −47.2 | +1.1 |
| **MX** | IV, metal | Maxell MX | +9.1 | −6.8 | 62.7 | −50.4 | +0.8 |

What these mean on the deck:

- **MOL** (maximum output level) is where a 400 Hz tone reaches 3% distortion. A higher MOL means you can record louder before the tape starts to compress and distort. Bass gets slightly less room than mid-range, as on the real tape.
- **SOL 10 kHz** is how loud treble can go before the tape saturates. Loud cymbals and bright synths compress and dull on tapes with a low SOL, especially at 1⅞ ips. At 3¾ ips the treble has more room.
- **S/N** sets the hiss on blank tape. Type II and IV tapes use 70 µs playback EQ, so their hiss is less bright.
- **Modulation noise** is a fine grain that rides on the signal itself. It is most noticeable on sustained notes on cheap tape.
- **Sensitivity** is how loud the tape plays back what you recorded. It changes slightly with every bounce, and noise reduction doubles the difference, just as a real deck that hasn't been calibrated to the tape does.

The automated tests record tones through each cassette and check that these figures come out. Tapes made before this version play as SD. The cassette shell, its print, and the inlay colours match the tape you choose.

### The tape switch

The **TAPE · BIAS · EQ** switch under the cassette (NORMAL / CHROME / METAL) sets the record bias, the record EQ and the playback EQ, as on a real deck. When a tape goes in, the switch moves to that tape's type. After that it stays where you put it, so you can try mismatches. One lamp shows where the switch is: green when that suits the loaded cassette, amber when it doesn't, and the status display says which way the bias is off.

- **Bias:** a Type II tape takes about 1.5× the bias of a Type I, and a Type IV about 2.5×.
  - **Under-biased** (a tape recorded at a lower position than its own): brighter, distorts and compresses sooner, grainier. For example, a chrome tape at NORMAL gains about +2.5 dB at 10 kHz and loses about 5 dB of MOL.
  - **Over-biased** (a tape recorded at a higher position than its own): duller and slightly quieter. For example, a normal tape at METAL loses about 16 dB at 10 kHz.

  The bias figures are estimates from how real decks behave; they are not from a published measurement.
- **Playback EQ:** NORMAL plays back at 120 µs, CHROME and METAL at 70 µs. Every stretch of tape remembers the position it was recorded at. Playing a 70 µs recording at NORMAL lifts the treble by up to 4.7 dB (bright, hissy), and the reverse cuts it. The **EQ** lamp on each track turns red when the playback EQ differs from the recording's. Blank tape's hiss follows the same rule.
- **Erase:** recording over a metal tape at NORMAL doesn't fully erase it. The old recording, mostly its bass, stays about 30 dB down under the new one.
- **With noise reduction:** the decoder enlarges any level or treble error, just as on a real deck. With 2:1 a 1 dB error becomes 2 dB, and with B and C a mismatch's treble error "breathes" as the music gets louder and quieter.

### Tape character

**Setup → Tape** shows the current cassette's figures. The sliders adjust things on top of those figures:

- **HISS** is relative to the cassette's measured level. Centre is as measured; double-click to return there.
- **TAPE DRIVE** sets where 0 VU lands on the tape, ±6 dB around Dolby level. More drive means earlier saturation.

The other sliders are wow, flutter, HF loss, track crosstalk, dropouts and speed drift. You also choose the noise reduction type here (2:1 compander, sliding band B or C). The condition presets go from **Clean** to **Chewed**; they leave the drive alone.

## Where things are kept

- **Tapes**: `Documents/TapeFour/Tape Shelf/`, one folder per tape (the audio, `tape.xml` for what's recorded where, and `deck.xml` for the deck that goes with it). Tapes are saved in the background: every five seconds while recording, so a crash costs seconds rather than a take; whenever the transport has been stopped for a second; and on eject and on quit.
- **Settings**: `TapeFour.settings` in `%APPDATA%\TapeFour\` (Windows), `~/Library/Application Support/TapeFour/` (macOS) or `~/.config/TapeFour/` (Linux). This holds the audio device, MIDI controllers and synths, each track's chain (including each plug-in's state and its MIDI mappings), deck MIDI mappings, knob positions and tape character. It's only written when something has changed.
- **Crash reports**: if TapeFour ever crashes it writes a report to `TapeFour/Crashes/` next to the settings (a `.txt` with the version and a stack trace, and on Windows a `.dmp`), and says so the next time it starts. Sending one along with a description of what you were doing makes the problem findable.

## Building it yourself

You need CMake 3.22 or later and a C++20 compiler.

| Platform | Also needs | Commands |
|---|---|---|
| Windows | Visual Studio 2022 with the C++ workload | `cmake -S . -B build -A x64` then `cmake --build build --config Release` |
| macOS | Xcode 15 or later | `cmake -S . -B build -G Xcode "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"` then `cmake --build build --config Release` |
| Linux | `ladspa-sdk libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev` (Debian/Ubuntu names) | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` then `cmake --build build` |

JUCE 8.0.11 and the CLAP headers (1.2.7, MIT licence) are downloaded during configuration. JUCE includes the ASIO headers (Steinberg made ASIO available under GPLv3), the VST3 SDK and LV2 support; CLAP plug-ins are hosted by TAPE·FOUR's own code (`Source/ClapFormat.cpp`), since JUCE doesn't host CLAP. JACK is loaded only if it's installed, so the Linux build runs without it.

Add `-DTAPEFOUR_TESTS=ON` to also build the headless test programs, which every build on GitHub runs on all three platforms:

- `EngineTest`: each cassette's MOL, SOL, hiss, modulation noise and sensitivity; pitch and level; noise reduction, tape speed, the tape switch, and clean punch-ins and punch-outs.
- `MixerTest`: the whole audio path: monitoring and arming crossfades, EQ, bounce, the built-in synth via MIDI and MPE, per-track controllers, signal chains (MIDI effects, layering, MIX and bypass), CLAP hosting (scanning, an effect with parameters and saved state, an instrument fed CLAP note events, using two test plug-ins built from `Tests/fixtures/TestClap.c`), when plug-in state is saved, mute and solo, MIDI learn (pickup, encoders, toggles, MMC, plug-in parameters), hardware synth routing, recordings lining up on an interface with latency, saving while recording, plug-in load races, MIDI device slots, deck recall, and settings surviving a restart.
- `ScanTest build/crashplugin`: scans a plug-in made to crash, to show the scan survives it.

To check real plug-ins on your machine, run `MixerTest` with `TAPEFOUR_SMOKE_DIR` set to a plug-in folder (for LV2, the folder holding the `.lv2` bundles): every format scans it, and each instrument found is loaded and played. `TAPEFOUR_CLAP_LOG=1` makes the app print what CLAP plug-ins ask of it.

`-DTAPEFOUR_SANITIZE=address` or `thread` (with Clang or GCC) builds them with the address/undefined-behaviour or thread sanitizer; GitHub runs both on every push.

## Licence

TAPE·FOUR is released under the **GNU GPL v3** (see `LICENSE`), because it uses ASIO under Steinberg's GPLv3 option and JUCE under its GPL option.

The embedded fonts (Barlow Condensed, Share Tech Mono, Caveat) are under the SIL Open Font License (`Resources/fonts/OFL.txt`).
