# TF PHRASE SAMPLER: what it's modelled on

A phrase sampler after the Yamaha SU10 sampling unit (1995). `Source/PhraseSampler.{h,cpp}` is the model,
`Source/PhraseSamplerPanel.cpp` its window, `Tests/PhraseSamplerTest.cpp` checks it (and `Tests/MixerTest.cpp`
checks it in a track's chain).

Some values come from the unit's documentation and reviews; others are inferred from them, and some
have no published figure and were set by ear. The tables below say which.

## Sources

- **Yamaha, SU10 Owner's Manual** (Yamaha's manual library; also on ManualsLib): the four grades, the
  system pitch, the polyphony table and which ribbon effects each grade allows, the five loop modes,
  NOTE / TOGGLE / TRIGGER, alternate groups, HOLD, REV, the input gain 1–16 and the trigger levels, the
  filter's types and resonance, EXTERNAL FILTER's pads, the songs, the default MIDI notes.
- **Sound On Sound**, "Yamaha SU10" (Derek Johnson, March 1996): 384 KB of non-volatile memory and
  8 KB of song memory (about 1000 events); 17.8 to 67.4 seconds in all, halved in stereo; HI is
  44.1 kHz at +10 % pitch and EXTRA LONG 11.64 kHz at −20 %; the grade is global ("sample rates can't
  be mixed"); HI plays two mono samples (one stereo), STD four; HI has no scratch, scale, crossfade,
  filter or pitch, STD no scale or filter; four songs, recorded in real time, no overdub.
- **Polynominal**, "Yamaha SU10": the recording times 19, 27, 38 and 54 s; 16-bit sampling, a 16-bit
  44.1 kHz DAC; a 24 dB/octave filter, low, band or high pass, with resonance; gain 1–16.
- **Yamaha's 4-bit ADPCM**, as documented for its sample-playback chips (the YM2610's ADPCM-B, the
  YMZ280B): the step table and the step multipliers. Yamaha doesn't say which ADPCM the SU10 uses;
  384 KB holding 786,432 samples means four bits a sample, and this is Yamaha's own.

## The clock

One sample clock runs everything, recording and playing; the grade sets it, and the system PITCH
(−20 % to +10 %) moves it. So a sample plays at whatever the clock is now: changing the grade or the
pitch after recording changes how every sample sounds, as on the unit.

| Grade | Clock at 0 % | At +10 % | At −20 % | Whole memory, mono, at 0 % | Published |
| --- | --- | --- | --- | --- | --- |
| HI | 40,091 Hz | 44,100 Hz | 32,073 Hz | 19.6 s | 19 s; 44.1 kHz at +10 % |
| STD | 29,157 Hz | 32,073 Hz | 23,326 Hz | 27.0 s | 27 s |
| LONG | 20,045 Hz | 22,050 Hz | 16,036 Hz | 39.2 s | 38 s |
| EXLG | 14,579 Hz | 16,036 Hz | 11,663 Hz | 53.9 s | 54 s; 11.64 kHz at −20 % |

HI at +10 % is the published 44.1 kHz. The others are **inferred**: their clocks are in the ratios of
the published times (19 : 27 : 38 : 54 is close to 1 : 11/8 : 2 : 11/4), which puts LONG at +10 % on
22.05 kHz and STD at +10 % on 32 kHz-ish, two of the rates Polynominal lists; EXLG at −20 % comes out
at 11.66 kHz against the published 11.64, and the extremes at 17.8 and 67.4 s, as Sound On Sound has
them.

## The model

| Part | What it does | Values |
| --- | --- | --- |
| Memory | 786,432 four-bit samples (384 KB), shared by all 48 pads, kept with the deck | Published. A stereo frame takes two |
| Recording | The source through an anti-alias filter (fourth-order, at 45 % of the clock), sampled at the clock, encoded as ADPCM into the memory after what's there | The filter's order and corner are a choice (not published). Sources: the track's input, another track as it leaves its channel strip (before or after its fader), the other tracks' mix, or a file, all through the same path |
| Gain | 1 to 16 | Published range. 3 dB a step with 8 at unity: set by ear |
| Trigger | MANUAL, or levels 1–7: recording starts when the input reaches the level | Published. The levels, −42 to −6 dBFS in 6 dB steps, set by ear |
| ADPCM | Yamaha's four-bit ADPCM: each sample a step up or down of 1/8 to 15/8 of the step size, which then grows or shrinks | The tables are Yamaha's (see Sources). Silence encodes as a faint ±15 (of 32768) idle tone, as ADPCM does |
| Playback | Each sample at the clock, linearly interpolated, through a reconstruction filter at 45 % of the clock | The interpolation and filter are a choice: they stand in for the DAC and its filtering, which aren't published |
| Monitoring | THIS TRACK IN heard through the sampler: OFF, WHILE RECORDING (from REC until STOP), ALWAYS; after the gain; a 10 ms fade in and out | Not on the unit (its manual only has the input heard in CROSSFADER and the EXT modes): TAPE·FOUR's, since the sampler replaces what reaches it in the chain |
| Pads | Start, loop and end points; ONE SHOT, WHOLE LOOP, PARTIAL LOOP, REVERSE SHOT, REVERSE LOOP; NOTE (while held), TOGGLE, TRIGGER (plays to its end, held or not, no looping); volume 0–127 (a playing pad follows a change at once, over 5 ms); alternate group 1–3; a MIDI note | Published |
| HOLD, REV | HOLD latches NOTE pads; REV plays forward modes backwards and reverse ones forwards | Published. HOLD on when a song starts repeats the song: this model's choice |
| Voices | The owner's manual's table. When a pad needs more voices than are free, the oldest unheld one is cut | The table is published; which voice goes isn't: oldest first is a choice |
| Ribbon: PITCH | The clock, −20 % to +10 % across the ribbon | Published range |
| Ribbon: FILTER | Two state-variable sections in a row (24 dB an octave): low, band or high pass, resonance flat, weak or strong; the cutoff 40 Hz to 16 kHz across the ribbon | Slope and types published. Q 0.6, 2 and 6, and the cutoff's range, set by ear |
| Ribbon: SCALE | One sample across the 12 pads: pad 12 at its own pitch, pad 1 eleven semitones down; one at a time | Published (which pad is the sample's own pitch is inferred) |
| SCRATCH | A pad's sample under the ribbon, as a record under your hand: a window of it, moved with BWD, FWD and TOP; held still, silent | The window's 0.6 s set by ear |
| CROSSFADER | The samples against the track's input, equal power | Published |
| EXT SCRATCH | The input plays through; touch the ribbon and it stops, and the ribbon moves through what just came in | The 1.5 s buffer set by ear |
| EXT FILTER | The input through the ribbon's filter; pads 1–4 choose its type, 7–9 its resonance | Published |
| Loop tempo | The looped part (from the loop point in PARTIAL LOOP) as 80.00 to 159.99 BPM, to the hundredth; to the whole beat for reverse loops and very long forward ones; none under 0.375 s; shown while a loop's points are edited | Published (owner's manual p. 46), except where "very long" starts: not published, set here at 6 s (16 beats at 160 BPM). The unit writes the decimal point as an underscore (124_34); TAPE·FOUR shows a point |
| Front panel | SHIFT with pads 1-12: SONG 1-4, EDIT, UTILITY, SCALE, FILTER, CROSSFADER, PITCH, EXT SCRATCH, EXT FILTER; one BANK key; EXIT, ENTER, +1/-1, < >; EDIT's 13 functions (LpMode .. Delete, COMPACT and DELETE asking "Sure?"); recording: REC, a pad, ENTER, the settings, PLAY, STOP; UTILITY's MIDI settings, song delete, Init Data and Init All | Published (manual pp. 7-12, 31-54). Which pad carries which SHIFT function isn't in the manual's text (its drawing is): the order the manual lists them is used, and agrees with EXT FILTER's pads. UTILITY's Bulk, MSDS and Lock aren't here |
| Songs | Four, in real time from the pads (here, the mouse or MIDI): presses and releases, their velocity, REV and HOLD, to the millisecond; 1000 events in all; no overdub | Published. The ribbon isn't recorded, as on the unit |
| MIDI | Pads on C1–B4 (MIDI 36–83) by default, each pad's note changeable; velocity sets the level; the ribbon from pitch bend or a controller | The defaults are published |
| Level | A sample recorded at unity gain comes back, at volume 127, at the level it went in | A choice |

## In TAPE·FOUR

The sampler is an instrument that takes the track's audio (PROCESS INPUT), as the unit has a line input.
With no effect on, the input isn't heard through it; CROSSFADER and the EXTERNAL effects play it. It can
sample the other tracks as they leave their channel strips (their plug-ins, live when armed or from the
tape when not, and EQ; before or after the fader), or their mix (as heard: mute and solo count). Those
arrive a block late (the tracks are worked out one after another). Its own track's output can't be a
source: it would feed back.

While its window is open, its track's chain runs and is heard on top of the tape even when the track
isn't armed (any instrument's does), so it can be played from the window; only an armed track records.
A pad or key pressed while nothing is running is dropped after a quarter of a second rather than played
late.

## Presets

A preset is a `.tfps` file in `Documents/TapeFour/Presets/Phrase Sampler`: the whole memory (the ADPCM,
two samples a byte, as the unit's bulk dump keeps it), the 48 pads, the four songs, the grade, pitch,
volume and the rest of the settings.
