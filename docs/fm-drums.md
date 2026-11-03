# TF FM DRUMS: what it's modelled on

A six-track FM drum machine in the manner of the Elektron Model:Cycles: six "machines", each a small FM
algorithm behind PITCH, DECAY and four macros, a sequencer with per-step parameter locks, a delay and a
reverb. It isn't a copy of the Cycles (whose algorithms aren't published): the machines are TAPE·FOUR's
own, built so the whole of each knob's travel stays musical. `Source/FmDrums.{h,cpp}` is the engine,
`Source/FmDrumsPanel.cpp` its window, `Tests/FmDrumsTest.cpp` checks it.

## Sources

- **Elektron, Model:Cycles** (as described in Sound On Sound's review): six tracks, the six machine types,
  PITCH, DECAY and the four macros COLOR, SHAPE, SWEEP and CONTOUR on every machine, sends to a delay and a
  reverb, a VOLUME + DIST, an LFO a track, parameter locks (the machine too), the CHORD machine. The layout
  is taken from it; nothing inside is.
- **John Chowning, "The Synthesis of Complex Audio Spectra by Means of Frequency Modulation"** (JAES,
  1973), and Chowning & Bristow, *FM Theory and Applications* (1986): drums, wood-blocks and bells from two
  operators: an inharmonic ratio and a modulation index that decays faster than the sound gives a drum;
  integer ratios give harmonic spectra. PERC, the snare's body and TONE follow this.
- **Werner, Abel & Smith, "The TR-808 Cymbal: a Physically-Informed, Circuit-Bendable, Digital Model"**
  (ICMC-SMC 2014): the 808's cymbal and hi-hats come from six square-wave oscillators at 205.3, 369.6,
  304.4 and 522.7 Hz, and two trimmed in the factory, at 359.4-1149.9 and 254.3-627.2 Hz. METAL's six
  operators are at these frequencies' ratios to the lowest, with 800 and 540 Hz for the two trimmed ones
  (our choice, within their ranges), FM'd in pairs instead of mixed.

## The machines (all ranges set by ear)

| Machine | PITCH | DECAY | COLOR | SHAPE | SWEEP | CONTOUR |
| --- | --- | --- | --- | --- | --- | --- |
| KICK | 30-120 Hz | 60 ms-2 s | BITE: a 1:1 modulator, index 0-3, falling in 30 ms | CLICK: a 2.5 ms noise and 1.8 kHz burst | PITCH DROP: 0-4 octaves from above, falling in 8-48 ms | BOOM: held 0-150 ms before decaying (tight to an 808's) |
| SNARE | 120-340 Hz | 50 ms-1 s | NOISE: snares against body | RATTLE: the body's index (1.6:1 modulator), 0-4 | PITCH DROP: up to 2.5 times, falling in 20 ms | SNAP: the snares' decay, 0.4-2 times the body's |
| METAL | 200-800 Hz | 20 ms-2.5 s | BRIGHTNESS: index 1-8, and some noise | SPREAD: the ratios 0.5-2 times as far apart | TONE: a 12 dB high-pass at 2-10 kHz | SHIMMER: a second tail, 3 times longer, up to half |
| PERC | 60-960 Hz | 30 ms-1.5 s | RATIO: 0.5, 1, 1.41, 1.5, 2, 2.5, 3, 3.5, 4, 5.33 : 1 | DEPTH: index 0-5 | PITCH DROP: 0-2 octaves, falling in 30 ms | RING: how long the index lasts, 0.1-1 of the decay |
| TONE | a note, C1-C7 | 50 ms-4 s | RATIO: 0.5, 1-8 : 1 | DEPTH: index 0-4 | BRIGHT DECAY: an extra index 0-6, decaying | ATTACK: 0-200 ms |
| CHORD | a root, C2-C6 | 80 ms-5 s | CHORD: MAJ, MIN, 7, MAJ7, MIN7, SUS2, SUS4, 6, MIN6, ADD9, DIM, AUG | VOICING: eight (close, inversions, open, wide, drop 2, spread, high) | BRIGHTNESS: each voice's 1:1 index, 0-2.5 | STRUM: up to 40 ms between voices |

Each track: VOLUME (squared), DRIVE (a tanh soft clip, up to ten times in, level kept), PAN (equal power),
DELAY and REVERB sends, an LFO (0.05-20 Hz; sine, triangle, ramp, square, random; to PITCH, DECAY, the
macros, VOLUME or PAN; restarting with each hit). A track plays one sound at a time; a new hit takes over.

## The sequencer

16 patterns, each 1-64 steps (sixteenths), with its own steps; the kit (the six tracks' sounds) is shared.
A step: on or off, velocity, chance (1-100 %), retrig (up to three more hits in the step, each 12 softer),
and locks: any of a track's settings, the machine too, for that step only. Swing (50-75 %) makes the
off-beat sixteenths late by up to half a step. A new pattern chosen while playing starts when the one
playing ends.

It keeps its own tempo (40-240 BPM) and its own PLAY, as a drum machine plugged into a recorder does: it
doesn't follow the tape or other instruments. MIDI notes 36-41 (C1-F1) play the six tracks, with their
velocity (no locks).

## The effects (set by ear)

| What | Here |
| --- | --- |
| Delay | Ping-pong, 1/16 to 1/2 of the tempo (3/16 and 3/8 too), feedback up to 90 %, a low-pass in the loop (800 Hz-16 kHz) |
| Reverb | Eight delay lines mixed by a Householder matrix, their lengths fitted to SIZE, decay 0.3-8 s, a low-pass in each (1.5-16 kHz) |
