# TF SPRING REVERB: what it's modelled on

A spring reverb tank and the valve circuit around it. `Source/SpringReverb.{h,cpp}` is the model,
`Source/SpringPanel.cpp` its window, `Tests/SpringReverbTest.cpp` checks it.

Some values are published (from the papers and Accutronics' data), some are inferred from them, and some
have no published figure and were set by ear. The tables say which.

## Sources

- **M. van Walstijn**, "Numerical calculation of modal spring reverb parameters", DAFx 2020: the thin helical
  spring as two coupled equations (transverse and longitudinal), its dispersion relation, its losses
  (Kelvin-Voigt and frequency-independent), the transducers as raised cosines at the ends, the output as the
  force on the end, the magnets' effect on the modes (a low-pass, a resonance above the transition and a
  slowing of the lowest frequencies), and the parameters fitted to one spring of an Accutronics 9EB2C1B (his
  Table 1).
- **S. Bilbao**, "Numerical simulation of spring reverberation", DAFx-13, and **S. Bilbao & J. Parker**, "A
  virtual model of spring reverberation", IEEE TASLP 2010: the helical spring model (after Wittrick) that van
  Walstijn's is a reduced form of.
- **V. Valimaki, J. Parker & J. Abel**, "Parametric spring reverberation effect", JAES 2010, and **H. Gamper,
  J. Parker & V. Valimaki**, "Automated calibration of a parametric spring reverb model", DAFx-11: the
  waveguide model's structure (dispersive cascades of stretched allpasses in a loop for the low chirps,
  first-order allpasses for the high ones) and the values fitted to a Leem spring (Gamper et al., Table 1).
- **Accutronics** part numbers and data, as compiled by Amplified Parts and Schematic Heaven: tank types,
  lengths, spring layouts, decay classes (short 1.2-2 s, medium 1.75-3 s, long 2.75-4 s), coil impedances; the
  4AB3C1B in Fender amplifiers.

## The springs: SIMULATED

Each spring is a set of modes, each a resonator, from van Walstijn's model:

| Part | What it does | Values |
| --- | --- | --- |
| Dispersion | Each mode's frequency from the lower branch of the dispersion relation, at wavenumber n&pi; | van Walstijn's κ = 0.02018, q = 1994, γ = 1200 (fitted to the 9EB2C1B). It gives a transition frequency of 4.30 kHz, where the chirps turn |
| Modes | sin(n&pi;x) along the spring, the transverse and longitudinal parts in the ratio the equations give; about 850 a spring below 10 kHz (van Walstijn keeps 1009 below 20 kHz) | The shapes are the pinned-end approximation: van Walstijn computes clamped-end shapes numerically, which this doesn't (inferred to differ little except at the very lowest modes) |
| Echoes | The low frequencies are carried mostly by waves about a coil turn long, either side of the point where the lower branch dips to zero; their group velocity spaces the echoes 35 ms apart on the fitted spring | From the model. The waves below the transition on the other side make the "secondary chirps" van Walstijn reports as too strong; they're kept as his model has them |
| Losses | Each mode decays at σ + φω²/2: σ for the air and the mounting, φ the wire's internal friction | φ = 2×10⁻⁸ s (van Walstijn). σ: 3 /s (his) for the medium-decay tanks, 2 /s for the long one (inferred from its decay class) |
| Transducers | A raised cosine over 0.004 of the spring's length at each end, pushing and feeling the transverse motion only | van Walstijn |
| Output | The force the spring exerts on the pickup end | van Walstijn |
| Magnets | Each mode weighted by a low-pass (100 Hz, slope 1.8) and a resonance (6.3 kHz, ×16, 300 Hz wide); the lowest frequencies slowed by up to 1.2 times below about 600 Hz | van Walstijn's own fit (his eqs. 70-74), phenomenological rather than physical. The resonance's formula is read from his paper as peaking at 6.3 kHz |

## The springs: WAVEGUIDE

Valimaki, Parker & Abel's parametric model, its delay and transition set from the same springs as the
simulation, so the two have the same echoes and decay:

| Part | Values |
| --- | --- |
| Low chirps | A loop: 100 stretched allpasses (coefficient 0.62, stretched to the transition frequency) and a delay of one round trip; its gain from the decay. Gamper et al.'s fitted values |
| High chirps | A loop: 200 first-order allpasses (coefficient −0.6), decaying 1.3 times as fast (Gamper et al.); its level against the low chirps set by ear |
| Split | Fourth-order filters at the transition frequency (set by ear) |

It is brighter than the simulation (it has none of the magnets' shaping), and it isn't lighter on the
processor here: the simulation's modes run side by side on the processor's vector unit, the waveguide's
allpasses one after another. SpringReverbTest prints both.

## The tanks

| Tank | Published | Inferred |
| --- | --- | --- |
| 4AB3C1B | Type 4, 17", four springs as two counter-wound pairs; long decay; in Fender amplifiers | Two paths, the fitted spring's length and 0.86 of it; σ = 2 /s |
| 9EB2C1B | Type 9, 17", six springs as three pairs; medium decay | Three paths: the fitted spring (van Walstijn's), 0.89 and 0.79 of its length |
| 8EB2C1B | Type 8, 9", three springs; medium decay | Three paths of 0.55, 0.49 and 0.44 of the fitted spring's length (the tank is 0.55 as long) |

A path is a spring from input to output; in the 17" tanks each is a counter-wound pair joined in the middle,
which the model treats as one spring (van Walstijn's measurement was of one such path). A shorter spring of
the same wire keeps the transition frequency and spaces its echoes in proportion to its length. Accutronics
doesn't publish the springs' dimensions or delays; the different lengths in a tank are there so its springs'
echoes don't coincide, as real tanks are built.

## The circuit

| Part | What it does | Values |
| --- | --- | --- |
| Driver | A 12AT7 into the tank through a transformer, as in Fender's circuits. DWELL is how hard it's driven: clean at low settings, clipping (more of one side than the other: a single-ended stage) towards 10 | The valve's curve is a 3/2 power law biased between cutoff and grid current: its shape, the bias and DWELL's range (−24 to +18 dB) set by ear |
| Transformer | Its low end rolls off below 30 Hz | Set by ear |
| Coils | Their response is in the magnets' low-pass above (fitted to a measured tank), so it isn't modelled twice | — |
| Recovery | A 12AX7 stage: gain, and a gentle limit when the tank's output is big | Set by ear |
| TONE | A treble cut, 1.5 to 12 kHz | Set by ear |
| Level | The wet sound about the level of the dry at DWELL 5 | A choice, measured by SpringReverbTest |

The output is mono, as a tank is; the block's MIX blends it with the dry sound.
