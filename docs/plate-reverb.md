# TF PLATE: what it's modelled on

A plate reverb. `Source/Plate.{h,cpp}` is the model, `Source/PlatePanel.cpp` its window,
`Tests/PlateTest.cpp` checks it. Some values are published, some follow from physics, and some have no
published figure and were chosen. The tables say which.

## Sources

- **S. Bilbao**, "A digital plate reverberation algorithm", JAES 55(3), 2007: the plate as a thin plate
  under tension (Kirchhoff, with a membrane term), simply supported, driven at a point and heard at points.
- **K. Arcas & A. Chaigne**, "On the quality of plate reverberation", Applied Acoustics 71, 2010: a plate's
  modal density, its frequency-dependent losses, and measurements of a real EMT 140.
- **V. Valimaki, J. Parker, L. Savioja, J. O. Smith & J. Abel**, "Fifty years of artificial reverberation",
  IEEE TASLP 20(5), 2012: plate reverberation, and feedback delay networks (Jot's absorption filters).
- **EMT 240** data sheet: the foil (an electrolytic gold alloy, 18 um, 270 x 290 mm), decay 0.7-5 s at 500 Hz
  set by a damping plate's distance, resonance density "above 3 per Hz", twice the EMT 140's, the pickup
  turning over at about 100 Hz.
- **EMT 140** descriptions (no EMT data sheet with the plate's size was found): a steel plate in a tubular frame,
  a porous damper moved by hand wheel or motor and never touching the plate (closest about 1/8 inch), decay
  about 1 s (damper closest) to about 5 s (free) at 500 Hz; one pickup, two on the 140 ST.

## The plates

| Plate | Size | Thickness | Material | Decay at 500 Hz | |
|---|---|---|---|---|---|
| EMT 140 | 2 x 1 m | 0.5 mm | steel (E 200 GPa, 7850 kg/m3, nu 0.3) | 1-5 s | size and thickness as commonly given, not from EMT |
| EMT 240 | 290 x 270 mm | 18 um | gold alloy: pure gold's figures (79 GPa, 19300 kg/m3, nu 0.42) | 0.7-5 s | EMT; the alloy's properties aren't published |
| Small steel | 1 x 0.5 m | 0.5 mm | steel | 0.8-3 s | chosen: no particular unit |

From these, the physics: kappa = sqrt (D / rho h), D = E h^3 / 12 (1 - nu^2), and the modes per hertz
S / (2 kappa): 1.31 for the 140, 3.38 for the 240 (EMT: "above 3", "twice the 140"), 0.33 for the small one.

## SIMULATED

- The modes of a simply supported rectangular plate, sin (m pi x / Lx) sin (n pi y / Ly), at
  omega^2 = kappa^2 k^4 + c^2 k^2 (bending stiffness and tension), each a two-pole resonator, driven at the
  driver and heard at each pickup by the mode's shape there. Dispersion (highs first) comes out of this.
- A budget of 4000 modes: the EMT 140 to about 3.1 kHz, the 240 to about 1.2 kHz, the small plate to 8 kHz.
  They run at a quarter or half the rate (they're all below the top mode), through low-passes in and out.
- Above the top mode, the network below, high-passed there, at the level a plate's even modal density gives
  (the energy per hertz the modes have).
- The tension: chosen, as where it takes over from the bending stiffness, 15 Hz (slack) to 60 Hz (tight).

## LIGHT, and the tail

Eight delay lines (1031-3083 samples at 48 kHz for the 140, shorter for smaller plates) mixed by a Hadamard
matrix, each with Jot's one-pole absorption for the decay at 50 Hz and 12 kHz; ten first-order allpasses
(0.55) in front for the dispersion, and four Schroeder allpasses for diffusion. Its level is measured at a
2 s decay so LIGHT and SIMULATED match.

## Losses and condition (chosen)

- The decay at f: T(f) = T500 (1 + 500 / fc) / (1 + f / fc), fc = 2-5 kHz with condition: highs go faster.
- An aged plate rings shorter: T500 x (0.75 + 0.25 condition).
- DAMPING: the decay between the plate's range, exponentially; the damper's distance shown as 3-100 mm.
- The recovery amplifier: a high-pass at 80 Hz, BASS (200 Hz shelf) and TREBLE (4 kHz shelf).
