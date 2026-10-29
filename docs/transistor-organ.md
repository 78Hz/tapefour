# TF TRANSISTOR: what it's modelled on

A transistor combo organ after the single-manual Vox Continental (1962). `Source/TransistorOrgan.{h,cpp}`
is the model, `Source/TransistorPanel.cpp` its editor, `Tests/TransistorOrganTest.cpp` checks it.

Some values come from the instrument's documentation, repair notes or schematics; others have no
published figure and were set by ear. The tables below say which.

## Sources

- **Busted Gear**, "Vox Continental: Tune & Bias" (repair notes, with oscilloscope traces): twelve
  generator cards, each an oscillator with a tuning coil feeding a chain of transistor flip-flops;
  each divider's square wave goes through an RC low-pass per key before the key switches; a global
  RC filter with two capacitors after the keyboard makes the flute (sine) tone.
- **Connie** (github.com/Ho-Ro/connie), a simulation written from the schematics: twelve LC
  oscillator boards with six-stage octave dividers (84 tones); four contacts a key, summed for the
  four stops; flute and reed made by summing the mix and a two-stage RC filtered copy; the vibrato a
  filtered rectangular wave that frequency-modulates the oscillator boards.
- **Combo Organ Heaven**, "Vox Single Manual": the drawbars 16', 8', 4', IV, ~ (foundation, flute)
  and M (reed); IV is 2⅔', 2', 1⅗' and 1'; with both tone drawbars in, no sound; fixed single-speed
  vibrato, switched on and off; no percussion or sustain.
- **Wikipedia**, "Vox Continental": 49 keys with reversed colours; four contacts a key to a bus bar;
  single speed and depth of vibrato, adjustable only inside; germanium transistors that drift.
- **Sound On Sound** (reviews of the Vox Continental reissue and Arturia's Continental V): the reed
  stop a filtered square wave, the flute a sine.
- **voxac30.org.uk**, "A Vox Continental from mid 1963": twelve generator-divider assemblies, voiced
  in three groups (C–D♯, E–G, A♭–B); the vibrato's speed and depth set by two presets inside.

## The model

| Part | What it does | Values |
| --- | --- | --- |
| Oscillators | Twelve, one per note name, at C7..B7; each counts its half-cycles | From the tuning system and A4. Drift: each wanders as a slow random walk (90 s to forget where it was) of the size DRIFT sets; set by ear. TUNE THEM UP returns them to the tuning |
| Dividers | Bit k of each oscillator's count is the k-th flip-flop's square wave: 84 tones, C1..B7, every octave exact | Six dividers (Connie). The edges are band-limited (polyBLEP) so the top octaves don't alias |
| Voicing | Each tone's square wave through its own RC low-pass, running all the time, before the keys: the reed tone | Corner at the tone's own frequency, set by ear: the repair notes show the shape (a rounded square) but not the values, and the three voicing groups' capacitors aren't published, so all twelve are voiced alike |
| Keys | 49 (C2..C6 at 8'); any MIDI note plays, its tones folded into the generators' range. Four contacts a key: 16', 8', 4', IV | A contact switches its tones straight on, mid-cycle: the click. BOUNCE: it chatters for that long (making or breaking every 0.15 ms, more often made as it settles); CONTACT SPREAD: the four close up to that far apart, in an order fixed for each key. Both set by ear. No velocity |
| Mixture IV | 2⅔', 2', 1⅗' and 1' on one contact; tones above B7 fold back an octave | The fold-back is inferred: 84 tones can't reach the top key's 1'. The four ranks are each 6 dB down, set by ear |
| Drawbars | Each footage's busbar, then the tones: reed is the mix as it is, flute the mix through two more RC filters | 3 dB a step, nothing at 0, set by ear (the taper isn't published). Flute filter corners at 250 Hz with 6 dB of make-up, set by ear so middle C's flute tone is near a sine (its 3rd harmonic about 31 dB down) |
| Vibrato | A square wave through an RC filter (time constant 0.12 of its period), frequency-modulating all twelve oscillators together | The shape is from the schematics (Connie). Speed 6.4 Hz and depth ±18 cents are defaults set by ear: no measured figure is published (6.4 Hz is Connie's) |
| Preamp | Two transistor stages: a gentle limit on big chords | Set by ear |
| Level | A four-note chord on the panel as it comes peaks at −15 dBFS: the track's mono sum adds 3 dB, so it reads 0 on the meters (Dolby level, −12 dBFS) | A choice, to match TAPE·FOUR's other sources |

## Tuning

A divide-down organ tunes twelve oscillators and gets every octave exact, so the piano's EDO and octave
stretch aren't offered, and a Scala file must have 12 notes and a 2/1 octave. The historical
temperaments, just intonation and per-note cents are the same as the piano's (docs/electric-piano.md).

## Presets

A preset is a `.tfto` file in `Documents/TapeFour/Presets/Transistor`: every control, the Scala
file, and the contacts' seed. The drift isn't kept: it is wherever the oscillators have wandered.
