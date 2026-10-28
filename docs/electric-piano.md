# TF ELECTRIC PIANO: what it's modelled on

A reed electric piano after the Wurlitzer 200 and 200A. `Source/ElectricPiano.{h,cpp}` is the
model, `Source/Tuning.{h,cpp}` the tuning systems, `Tests/ElectricPianoTest.cpp` checks both.

Some values come from the instrument's documentation or measurements; others have no published
figure and were set by ear. The tables below say which.

## Sources

- **Service manual.** Wurlitzer Electronic Piano Models 200 and 200A Service Manual. Its figures:
  - reed bar polarising voltage, 160–170 V DC (OpenWurli reads 147 V off the 200A schematic)
  - vibrato preset to 5.75 Hz, made by shorting the preamp's feedback at that rate
  - power amp of 40 W into two 4×8″ oval speakers (model 200)
  - hammer blow distance 1 5/64″ and let-off 1/8″
  - voicing: the reed's flat face in the same plane as the pickup's face, centred between the
    fingers; "when a reed at rest is positioned inside the pickup, a very strong undesirable
    harmonic will be generated"
  - 64 notes, A1 to C7
- **Patents.** Miessner, US 2,932,231: the hammer stays on the reed for ¾ to 1 cycle of its
  fundamental. Miessner, US 2,919,616: side clearance of about .0025″ each side of the reed; a reed in
  the plane of the pickup doubles its frequency, one offset from it gives essentially the fundamental.
  Andersen, US 2,974,555: a reed swinging through the plate gives two peaks a cycle.
- **OpenWurli** (github.com/hal0zer0/openwurli, docs/research): a physically modelled 200A whose
  research notes grade every figure by its source. From it:
  - the comb network read off the 200A schematic (22 kΩ, 1 MΩ feed, 470 kΩ, 220 pF at the first
    transistor), giving one high-pass corner near 880 Hz with a reed-bar capacitance of 240 pF
  - the pickup as an overlap of the reed between two fingers, not a reed closing on a plate (from
    the manual, the patents, a finite-element study and a technician's teardown video); the reed
    resting a paper's thickness clear (0.07–0.10 mm); bass reeds below their fingers, the rest above
  - its provisional full-force swing from a model of the hammer's impact: A1 3.26 mm, C4 1.19 mm,
    C7 0.22 mm (no measurement exists)
  - the preamp's heavy feedback, so it stays nearly linear and clips almost symmetrically (1.21:1);
    the vibrato's gain swing, about 8 dB
  - the beam theory for a reed with a solder tip: tip mass raises the mode ratios above the bare
    beam's 6.27 and 17.55
- **The physical model.** F. Pfeifle, "Real-time physical model of a Wurlitzer and Rhodes electric
  piano", DAFx-17 (2017): the pickup's current is the polarising voltage times the rate of change of
  the capacitance.
- **Reeds.** Vintage Vibe, "Case Study: Wurlitzer Electric Piano Reeds": reed dimensions by section;
  bass reeds (1–20) .020″ thick, the 200A's middle and treble tongues thickened to about .026″.
- **The 200 and the 200A.** Chicago Electric Piano, "What is the difference between a Wurlitzer
  200 and 200A?": the 200A's quieter amplifier and extra pickup shield, and the move from Alnico to
  ceramic speakers.

## The model

| Part | What it does | Values |
| --- | --- | --- |
| Reed | A fundamental and two higher bending modes, each a decaying sinusoid | Fundamental from the tuning system, plus the reed's drift. Mode ratios 7× and 19× in the bass falling to 6.4× and 17.8× at the top: within what beam theory allows for a reed with a solder tip; the exact values aren't measured. Ring time scales with pitch as (261.6 Hz / f)^0.62, with middle C's time constant 1.25 s at RING ×1, set by ear (no reliable measured decay rates exist) |
| Hammer | A half-sine force pulse lasting ¾ cycle of the reed's pitch (played hard) to 1 cycle (softly), from Miessner's patent; harder felt shortens it by up to a quarter, softer lengthens it | So long a contact barely strikes the higher modes (about 40 dB down). Blow distance and let-off as in the service manual. A note softer than the let-off allows doesn't sound |
| Swing | How far the reed's tip swings: OpenWurli's provisional full-force table, scaled by the hammer's speed squared and by how much of the contact lands on the fundamental | The table is an estimate, not a measurement. Speed squared (how MIDI velocity maps to hammer speed) is set by ear, so middle C starts to fold through its fingers at about velocity 120 |
| Pickup | The reed overlaps the two fingers beside it; the source is the overlap's change from rest, scaled by voltage and side gap | Reeds .51 mm thick (bass, and the 200's middle and treble), .66 mm (the 200A's middle and treble), from Vintage Vibe. Fingers 1.25 mm (bass), 0.6 mm (middle, reeds 21–42), 1.3 mm (treble): guesses inside OpenWurli's bands. Rest 0.09 mm clear (a paper's thickness). Side gap .064 mm (Miessner). The overlap's edges are smoothed over 0.16 mm + 0.6 × the side gap, set by ear: the field's fringe and the reed bending along the fingers spread it, and nothing measures it |
| Network | One high-pass pole at 880 Hz on the summed reeds: below it the output follows the overlap's rate of change | From OpenWurli's reading of the schematic; the 240 pF behind it is a single forum measurement, and another reading of one part would put the corner near 1.4 kHz. It cuts A1 about 24 dB against the treble, which evens out the bass's big swing |
| Dampers | On key up, each mode decays in 20 ms (firm) to 370 ms (weak) | Set by ear. The sustain pedal holds them off |
| Preamp | A transistor stage, nearly symmetric; DRIVE pushes it | Clean at playing levels as in OpenWurli's analysis. DRIVE's range is set by ear |
| Vibrato | The preamp's gain switched at 5.75 Hz, between a sine and a square; up to 8 dB of swing | Rate from the service manual, depth from OpenWurli; shape set by ear |
| Speakers | Power amp, then the two small ovals: high-pass 110 Hz, a presence peak, a treble roll-off | 200: Alnico, a 1 dB peak at 2.2 kHz and a 5.2 kHz roll-off. 200A: ceramic, a 3 dB peak at 2.8 kHz and a 7 kHz roll-off. Set by ear from the sources' descriptions (Alnico smoother, ceramic brighter) |
| Rear jack | The preamp's output, before the power amp and speakers | — |
| Noise | Mains hum (50 or 60 Hz and harmonics, 6 dB more on the 200), hiss (−84 dB on the 200, −92 dB on the 200A), action, damper and pedal noise | Set by ear. The 200's noisier amplifier is from the sources |

What comes out, at the rear jack (C4, the 200A, serviced): at velocity 64 the second harmonic is
about 14 dB below the fundamental and the third about 70 dB down; at 110 the third rises to about
−18 dB; at 127 the reed folds through its fingers and the third passes −10 dB. C2 reaches its
fingers' far side sooner (the third above −6 dB from velocity 90): the bass growls first. C6 stays
nearly clean (second harmonic −17 dB at 127).

## Ageing

Playing time (only while notes sound, and only with AGES AS YOU PLAY on) changes the piano:
- **Reed drift.** It grows as 2 × √(hours / 100) cents, each reed its own way.
- **Hammer felt.** It hardens by up to 0.25 over about 2000 hours.
- **Damper felt.** It loses up to 0.2 of its damping over about 3000 hours.
- **Short ringers.** One more reed rings short every 400 hours, up to 12.

These rates are set by ear: there's no published data for how fast a Wurlitzer wears.

## Tuning systems

The historical temperaments are built from their circles of fifths, so the tables come from their
definitions:

| System | Construction |
| --- | --- |
| Pythagorean | Pure fifths from E♭ to G♯ of the key |
| Quarter-comma meantone | Every fifth narrowed by ¼ syntonic comma |
| Werckmeister III | C–G, G–D, D–A and B–F♯ narrowed by ¼ Pythagorean comma; the rest pure |
| Kirnberger III | C–G, G–D, D–A and A–E narrowed by ¼ syntonic comma; F♯–C♯ by a schisma; the rest pure |
| Vallotti | F–C, C–G, G–D, D–A, A–E and E–B narrowed by ⅙ Pythagorean comma; the rest pure |
| Just | 5-limit: 1/1 16/15 9/8 6/5 5/4 4/3 45/32 3/2 8/5 5/3 9/5 15/8 |

The tests check each against its published table, for example Werckmeister III's E at 390.225 cents
and Kirnberger III's at 386.314. Every system keeps A4 at the reference, except Scala keyboard maps,
which name their own reference.

EDO is N equal steps to the octave, one per key, with A4 on its key. Scala files follow the Scala
format: ratios or cents, the last line the period. A keyboard map (.kbm) gives the map size, first
and last notes, the middle note, the reference note and its frequency, the period's degree, and the
mapping, with `x` for an unmapped key. With no map, key 60 is 1/1 and key 69 sounds at A4.

## Presets

A preset is a `.tfep` file (XML: a `TFEP` element with the preset's name, holding the piano's
`EPIANO` state: every parameter, the reed seed, the condition, the playing time, and the Scala
and keyboard-map text) in `Documents/TapeFour/Presets/Electric Piano`. Loading one replaces the
whole piano, clearing any Scala file the preset doesn't have. The modified mark compares a hash of
the state without its playing time, so wear from playing doesn't count as a change.
