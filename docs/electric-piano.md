# TF ELECTRIC PIANO: what it's modelled on

A reed electric piano after the Wurlitzer 200 and 200A. `Source/ElectricPiano.{h,cpp}` is the
model, `Source/Tuning.{h,cpp}` the tuning systems, `Tests/ElectricPianoTest.cpp` checks both.

Some values come from the instrument's documentation or measurements; others have no published
figure and were set by ear. The tables below say which.

## Sources

- **Service manual.** Wurlitzer Electronic Piano Models 200 and 200A Service Manual. Its figures:
  - reed bar polarising voltage, 160–170 V DC
  - vibrato preset to 5.75 Hz, made by shorting the preamp's feedback at that rate
  - power amp of 40 W into two 4×8″ oval speakers (model 200)
  - hammer blow distance 1 5/64″ and let-off 1/8″
  - 64 notes, A1 to C7
- **The physical model.** F. Pfeifle, "Real-time physical model of a Wurlitzer and Rhodes electric
  piano", DAFx-17 (2017): a reed, hammer and electrostatic pickup model, compared with
  measurements of the instruments.
- **Reeds.** Vintage Vibe, "Case Study: Wurlitzer Electric Piano Reeds": reed dimensions by model,
  showing the 200A's thicker middle and upper reeds.
- **The 200 and the 200A.** Chicago Electric Piano, "What is the difference between a Wurlitzer
  200 and 200A?": the 200A's quieter amplifier and extra pickup shield, and the move from Alnico to
  ceramic speakers.

## The model

| Part | What it does | Values |
| --- | --- | --- |
| Reed | A fundamental and two higher bending modes, each a decaying sinusoid | Fundamental from the tuning system, plus the reed's drift. Mode ratios about 7× and 19× (falling a little toward the treble), set by ear: no measured ratios are published. Ring time scales with pitch as (261.6 Hz / f)^0.62, with middle C's time constant 1.25 s at RING ×1, set by ear |
| Hammer | A half-sine force pulse; its spectrum sets how much of each mode is struck | Contact time 1.6 ms down to 0.4 ms as the felt hardens and the blow gets harder, set by ear. Blow distance and let-off as in the service manual (defaults 1 5/64″ and 1/8″). A note softer than the let-off allows doesn't sound |
| Pickup | Reed and charged comb form a capacitor; the output follows the capacitance, C(u) = 1 / (1 + (u/w)²) | Published as a numerical model (Pfeifle), not a formula: this curve is a simplification with the same behaviour, peaking where the reed is level with the comb. A reed resting above the comb gives a near-sine for a small swing and folds over (the bark) when it swings through. Voltage scales the output (160–170 V healthy) |
| Dampers | On key up, each mode decays in 20 ms (firm) to 370 ms (weak) | Set by ear. The sustain pedal holds them off |
| Preamp | A transistor stage, slightly asymmetric; DRIVE pushes it | The 200's is less clean than the 200A's (bias 0.2 against 0.12), set by ear |
| Vibrato | An amplitude wobble at 5.75 Hz, between a sine and a square (the feedback is switched) | Rate from the service manual; shape set by ear |
| Speakers | Power amp, then the two small ovals: high-pass 110 Hz, a presence peak, a treble roll-off | 200: Alnico, a 1 dB peak at 2.2 kHz and a 5.2 kHz roll-off. 200A: ceramic, a 3 dB peak at 2.8 kHz and a 7 kHz roll-off. Set by ear from the sources' descriptions (Alnico smoother, ceramic brighter) |
| Rear jack | The preamp's output, before the power amp and speakers | — |
| Noise | Mains hum (50 or 60 Hz and harmonics, 6 dB more on the 200), hiss (−84 dB on the 200, −92 dB on the 200A), action, damper and pedal noise | Set by ear. The 200's noisier amplifier is from the sources |

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
