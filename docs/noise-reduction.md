# Noise reduction, tape noise and the tape's mechanics: what they're modelled on

This is where the numbers in `Source/NoiseReduction.cpp`, `Source/Geometry.h` and the machines'
`.deck` files come from, and how closely the models match their sources. `Tests/NoiseReductionTest.cpp`
and `Tests/EngineTest.cpp` check them.

## Dolby B (B-type)

**Circuit.** A main path and a side path. The side path is a high-pass filter with a FET across
part of it, an amplifier and overshoot diodes. Encoding adds the side path to the main path;
decoding takes it away in a feedback loop round the output, so the decoder sees exactly what the
encoder saw. The side path's output is rectified and integrated: attack about 100 ms, down to about
1 ms for a big step through a diode; decay 100 ms. The integrated voltage drives the FET. Above the
threshold, the FET's conductance moves the filter's corner up the band until the side path's output
is held roughly constant. Loud signals are left alone, and the noise reduction carries on above
them. Sources:
- *Audio*, October 1973, "Dolby B-Type Noise Reduction System, part 2".
- *Wireless World*, May–July 1975, "Dolby noise reducer". This gives the 1.5 kHz fixed filter
  (3.3 kΩ, 32.6 nF), 26 dB of side-path gain, the 100 ms and 1 ms timing, and the ~2 dB overshoot
  diodes.

**Response.** The model is fitted to Dolby Laboratories' own table: "Dolby encoder output for
constant level input", the average of many of Dolby's professional encoders, from the Signetics
NE645/646 data sheet. It covers 13 frequencies from 200 Hz to 20 kHz and 9 levels from Dolby level
to −40 dB.

| | |
| --- | --- |
| Fixed corner | 1588 Hz |
| Lift at low level | 10.5 dB |
| RMS error over the table | 0.33 dB (200 Hz–14 kHz) |
| Largest error | 0.98 dB |
| Philips TEA0665 test points (2 kHz at −25 dB, 5 kHz at −40 dB, 10 kHz at −30 dB) | all inside the data sheet's limits |

The control law (the FET's conductance against its control voltage), the threshold and the control
amplifier's treble weighting are the fitted parameters. They aren't published as values.

## Dolby C (C-type)

**Structure.** Two of the B-type stage in series:
- **Two stages.** The high-level stage works at about B-type's levels, and the low-level stage
  about 20 dB below it (fitted: 20.2 dB).
- **Frequency range.** Both stages reach two octaves further down than B (corner 351 Hz).
- **Spectral skewing.** A selective treble cut above about 10 kHz before the encoder, restored
  after the decoder.
- **Anti-saturation.** A treble shelf in the high-level stage's main path, starting about 1.5 kHz,
  that only acts once that stage's side path has backed off.

Sources:
- Dolby Laboratories, "Dolby B, C and S Noise Reduction Systems: Making Cassettes Sound Better"
  (about 15 dB near 400 Hz, 20 dB from 2 to 10 kHz, starting about 100 Hz).
- The Philips TEA0665 data sheet, record mode.

**Response.** All six TEA0665 record-mode test points fall inside its limits:

| Test point | Model | TEA0665 limits |
| --- | --- | --- |
| 200 Hz at −40 dB | +6.8 dB | 6.6 to 9.6 |
| 1 kHz at −30 dB | +10.4 dB | 9.9 to 12.9 |
| 1 kHz at −20 dB | +4.9 dB | 3.9 to 7.9 |
| 5 kHz at Dolby level (anti-saturation) | −2.2 dB | −3.8 to −0.8 |
| 5 kHz at −20 dB | +3.7 dB | 0.9 to 4.9 |
| 5 kHz at −40 dB | +14.4 dB | 11.5 to 15.5 |

Low-level lift: 16.2 dB at 400 Hz, 20 dB from 2 to 10 kHz.

**Not published.** The depth and shape of the spectral skewing network. The model uses a shelf
10 dB down, centred on 15 kHz (Q 1). It barely touches 10 kHz, and the decoder restores it exactly.

## dbx type II (the 2:1 compander)

**What's documented.**
- The architecture: a 2:1 compressor on record and a 1:2 expander on playback (Wikipedia; dbx's
  literature).
- Fixed treble emphasis on the signal.
- An RMS detector that hears a band-limited, treble-weighted copy (*Electronics & Music Maker*,
  October 1981, on the dbx 222 and 224).
- Blackmer's RMS detector: a recovery of 60–200 dB/s is needed for unobtrusive compression
  (US patent 3,789,143).
- TASCAM's figures for dbx type II on the 424: 55 dB S/N without it, 90 dB with it (unweighted).

**Not published anywhere I could find.** The values of type II's emphasis and detector networks.
The model uses:
- **Signal emphasis:** a +12 dB shelf from 1.6 to 6.4 kHz.
- **Detector:** band-limited from 30 Hz to 10 kHz, with +20 dB of treble weighting from 200 Hz
  to 2 kHz.
- **Recovery:** 125 dB/s.

They give type II's behaviour: 2:1 below the emphasis, treble steering the gain, and rumble and
ultrasonics ignored. They are not a measured match to a dbx unit. A dbx service manual, or
measurements of a real 424, would pin them down.

Both detectors hear the encoded signal: the compressor listens to its output, the expander to its
input. So the expander's gain is always exactly the inverse of the compressor's.

## Tape noise

The noise is generated as flux noise on the tape, white along it, and played back through the
playback EQ:
- **The EQ.** 3180 µs and 120 or 70 µs, the IEC equalisation that makes a recording flat. It lifts
  the noise's treble at 6 dB per octave above 1326 Hz (120 µs) or 2274 Hz (70 µs), which is why
  chrome and metal positions hiss less.
- **The heads.** Their treble loss comes after, as it does for the signal.
- **The level.** Set so its A-weighted level, measured with the exact IEC 61672 curve, equals the
  tape's published signal-to-noise figure at its own EQ. The figures are Roberson's measurements,
  in `Common.h`.

It is added per track before the noise reduction decoder, so the decoder treats it as a real deck
would. The leaders carry none.

## The machine's electronics

Each mixer channel adds its own noise at its input, before EQ, fader and pan. The mix bus adds
noise of its own that no fader or pan moves.

The TF-4's figures are TASCAM's for the 424:
- Line in to line out S/N: 76 dB with one input open and 70 dB with four (20 Hz–20 kHz). So each
  channel is at −76 dB re 0 VU, and four uncorrelated channels give 6 dB more.
- The bus is set at −90 dB, well below that, as the 424's own figures imply.
- The fast wind is "120 sec. (approx.) with C-60".

The Pocket's playback amplifier (−85 dB) and the master deck's figures are estimates for their kind
of machine.

## The tape's mechanics

| | Value | Source |
| --- | --- | --- |
| Tape thickness | about 16 µm (C-46, C-60), 11 µm (C-90), 7.5 µm (C-120) | Wikipedia "Cassette tape" and "Audio tape specifications"; Electronics Notes. Their ranges: 15–18, 10–12, 6–9 µm |
| Hub winding diameter | 22 mm | No published source found; the one estimated figure here |
| Full packs | about 43 mm (C-46), 47 mm (C-60 and C-90) | Area conservation: √(hub² + 4·length·thickness/π) |
| Leaders | 3 to 6 seconds each end, from the cassette's id | Leader length varies by manufacturer (US patent 5,125,588); a few seconds is the common experience |
| Winding | Take-up spindle at a constant speed: tape speed = spindle speed × take-up pack radius, so about 9× play speed at the start of a C-60 and 20× at the end | Wind times: TASCAM 424 120 s, 414 115 s, Porta 05 100 s, for a C-60 |
| Cue and review | Head 18 dB down; 3 µm of spacing (spacing loss 54.6·d/λ dB) | — |
| Counter | Mechanical: 0.75 counts per turn of the take-up spindle, so it counts about twice as fast at the start of a side as at the end | — |
