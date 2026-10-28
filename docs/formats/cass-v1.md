# The cassette format (.cass), version 1

A `.cass` file is a cassette: the tape itself and what is recorded on it. It is not audio.
What you hear depends on the deck it is played in, described by a [.deck](deck-v1.md) file,
just as a real cassette sounds different in a hi-fi deck, a portable player or a car. A
cassette never holds a session (the mixer, plug-ins and so on around it: see
[session-v1.md](session-v1.md)).

The reference reader and writer is `Formats/Cassette.{h,cpp}` (plain C++17, no dependencies).

## The rule

What the **recording** deck did is in the cassette: record EQ, saturation, bias, noise
reduction encoding, and the wow and flutter of the machine that recorded it. What the
**playing** deck does is not: playback EQ, NR decoding, head response, its own wow and
flutter, and the tape's hiss, which is generated at playback from the formulation and
condition. A cassette's recording is therefore never "finished audio".

## Container

A cassette is a set of files. **Unpacked**, they sit in a folder (how TAPE·FOUR keeps the tapes
on its shelf). **Packed**, they are the entries of a `.cass` file: a standard zip archive whose
entries are *stored*, not compressed. The files are identical either way, byte for byte.

- Entry names are UTF-8, relative, with `/` separators; no `..`, no absolute paths, no `\`.
- Version 1 has no zip64: each entry and the archive are under 4 GiB (a C-90 is about 690 MB).
- Readers must refuse compressed or encrypted entries, verify CRC-32s, and ignore entries
  they don't know.
- Writers should start each entry's data on a 64-byte boundary in the archive, padding the
  local header's extra field (id `0xd935`, as Android's `zipalign`). A player can then map the
  tracks straight out of the file and play them in place, without unpacking. Readers must not
  rely on it: an entry that isn't aligned is read the ordinary way. (A player that plays in place
  checks CRC-32s when it has the time to, not before the first note.)

| File | Required | What it is |
| --- | --- | --- |
| `cassette.json` | yes | What the cassette is (below). Written once, when the cassette is made; changes only when its label or inlay is written on, or a new deck records on it. |
| `state.json` | no | What has happened to it since: write-protect tabs, where it is wound to, when it was last used. Missing: a fresh cassette. |
| `wear.bin` | no | Passes over the heads, per second of tape. Missing: never played. |
| `trackN.pcm` | no | Track N's recording (N from 1). Missing or short: the rest is blank. |
| `trackN.map` | no | Track N's map: what was recorded where. Missing: nothing recorded. |
| `art/look.json` + images | no | How the cassette looks, so any player can draw it ([The look](#the-look)). Readers that don't draw cassettes ignore it. |
| other `art/…` | no | Reserved for cover art. Readers that don't use it ignore it. |

## cassette.json

```json
{
  "format": "cass",
  "version": 1,
  "id": "6f1c5b0e-4d6a-4f6e-9f7d-3a2b1c0d9e8f",
  "created": "2026-10-27T09:30:00Z",
  "madeBy": "TAPE·FOUR 0.2.140",
  "label": "demo songs – oct",
  "formulation": {
    "name": "SD", "line": "STUDIO DYNAMIC", "position": "TYPE I · NORMAL",
    "iecType": 1, "eqMicroseconds": 120,
    "mol100": 3.8, "mol400": 6.2, "sol10k": -8.2,
    "snrDbA": 60.3, "modNoiseDb": -44.3, "sensitivityDb": -0.3,
    "basedOn": "TDK AD", "accent": "#e8702a"
  },
  "length": { "minutesPerSide": 30, "cellRate": 32000, "referenceSpeedIps": 1.875,
              "cellsPerTrack": 57600000, "blockCells": 3200 },
  "layout": "4-track-one-way",
  "tracks": [ { "pcm": "track1.pcm", "map": "track1.map", "encoding": "pcm-s16le", "fluxScale": 0.5 }, … ],
  "recorders": [ "tapefour.tf-4" ],
  "condition": { "grade": "new", "hissDb": 0, "dropoutsPerMinute": 0.02, "trebleLossDb": 0,
                 "modNoiseDb": 0, "shellFlutterPct": 0 },
  "inlay": { "layout": "tf4-inlay-1", "lines": [ "DEMO SONGS", … ] }
}
```

- **id**: a UUID, unique to this cassette. Copies of a cassette share it.
- **created**: ISO 8601, UTC. Calendar ageing counts from here.
- **formulation**: the tape's measured figures, so it plays the same in any deck, now or
  later, whether or not the deck knows the tape by name. All levels are dB re Dolby level
  (400 Hz) at 1⅞ ips. `mol100`/`mol400`: level for 3% third harmonic; `sol10k`: saturation
  output at 10 kHz; `snrDbA`: A-weighted bias noise below Dolby level without NR;
  `modNoiseDb`: modulation noise re the signal; `sensitivityDb`: output re the IEC reference
  tape; `iecType`: 1, 2 or 4; `eqMicroseconds`: 120 or 70.
- **length**: `cellRate` cells per second of tape at `referenceSpeedIps`. A track is
  `cellsPerTrack` cells long. Recording at another speed lays the same sound over more or
  fewer cells; the size of the file depends only on the length of tape.
- **layout**: how the tracks lie across the tape.
  - `4-track-one-way`: a multitracker's tape, four tracks played in one direction.
  - `stereo-two-sides`: a normal cassette. Tracks 1 and 2 are side A left and right;
    tracks 3 and 4 are side B left and right, and run the other way: cell 0 of track 3 is
    at the *end* of side B. Files store the tape as it physically is.

  A 4-track tape played in a stereo deck plays tracks 1 and 2 as side A, and tracks 3 and 4
  backwards when turned over, as on a real cassette.
- **tracks**: one per physical track, across the tape. `fluxScale`: the cell value that means
  full flux, as a fraction of full scale (0.5 leaves headroom for hot metal tape).
- **recorders**: ids of the decks that have recorded on it ([deck-v1.md](deck-v1.md)). The
  maps refer to them by number, so a playing deck can tell whether its head alignment
  matches the one that recorded.
- **condition**: what the cassette was like when it was made. Wear and age are added at
  playback from `wear.bin` and `created`, never written into the recording.
  `grade`: `new`, `used`, `worn` or `chewed`; the numbers are what count.
- **inlay**: the J-card. `layout` says how to read `lines`; `tf4-inlay-1` is TAPE·FOUR's
  (title, eight songs with counter numbers, four track notes, NR, speed, date).

Readers must keep members they don't know when they rewrite the file.

## state.json

```json
{ "writeProtect": { "A": false, "B": false }, "position": 1234567.5, "lastUsed": "2026-10-27T10:02:11Z",
  "summary": { "usedSeconds": 754.2, "tracksUsed": 3 } }
```

- **writeProtect**: a tab broken out. A deck must not record on that side. A 4-track
  cassette is recorded through side A's tab.
- **position**: where the tape is wound to, in cells along it from the start of side A's
  recordable tape. A cassette has a clear leader at each end (see Leaders), so a fully rewound
  cassette is at `-leader₀` and one wound to the end at `cellsPerTrack + leader₁`.

## Leaders

Each end of the tape is a clear leader a few seconds long: no oxide, so nothing records on it
and it doesn't hiss. Their lengths aren't stored; every reader works them out from the
cassette's `id`, so a cassette has the same leaders wherever it is played:

```
h = 2166136261
for each byte b of the id (UTF-8):  h = (h xor b) * 16777619          (all mod 2^32)
h = (h xor ((end + 1) * 0x9e3779b9)) * 16777619;  h = h xor (h >> 15)
leader seconds = 3 + 3 * (h mod 10000) / 9999                          (end 0: before side A, 1: after it)
```

Cells of leader are seconds × `cellRate`.
- **summary**: how far it is recorded and on how many tracks, so a list of cassettes needn't
  read every map. It can always be worked out again from the maps.

## Track files

**trackN.pcm**: the magnetic flux along the track, one signed 16-bit little-endian value
per cell, from the start of the tape. The file may stop short; the rest is blank tape.
This is not playable audio: it is NR-encoded where the map says so, has no playback EQ and
no hiss, and is laid out by tape position, not time.

**trackN.map**: per block of `blockCells` cells, six bytes:

| Byte | Meaning |
| --- | --- |
| 0 | Flags. Bit 0: recorded. Bit 1: NR-encoded. Bits 2–3: tape switch position it was recorded at + 1 (1 normal, 2 chrome, 3 metal; 0 unknown). Bits 4–6: NR system + 1 (1 the 2:1 compander, as dbx type II; 2 B-type, as Dolby B; 3 C-type, as Dolby C; 0 unknown): what the audio was encoded with, to be decoded with the same (see below). Bit 7: reserved, 0. |
| 1 | Recorder: index into `recorders`, from 1. 0: unknown. |
| 2–3 | Speed it was recorded at, relative to the reference speed × 1000, little-endian (1000 = 1⅞ ips, 2000 = 3¾ ips, varispeed included). |
| 4–5 | The day it was recorded: days since 2000-01-01 (UTC), little-endian. 0: unknown. Print-through grows from here. |

**Noise reduction.** Encoded stretches hold what the encoder put on tape, at the level where a
sine at Dolby level (200 nWb/m) has a peak of 0.2512 × the 16-bit full scale × `fluxScale`.
The systems are defined as the published ones: B-type is Dolby B (its encode response is Dolby
Laboratories' average encoder table, the Signetics NE645/646 data sheet), C-type is Dolby C
(the Philips TEA0665's record-mode characteristics), and the 2:1 compander is dbx type II.
`Source/NoiseReduction.{h,cpp}` is the reference encoder and decoder; `docs/noise-reduction.md`
gives the sources and how closely it matches them.

## wear.bin

One unsigned 16-bit little-endian count per second of tape (at the reference speed): how
many times that stretch has passed the heads in play or record. Fast winding doesn't count.
The whole width of the tape passes together, so it is per position, not per track.

## The look

`art/look.json` and the images it names say how the cassette looks, so a player can draw it
exactly as the deck that made it does, with its reels turning and its tape packs growing and
shrinking. TAPE·FOUR writes one for every cassette it makes, and redraws it when the label is
written on.

A player draws five layers, back to front:

| # | Layer | Drawn from |
|---|---|---|
| 1 | The shell behind the works | `layers.back`, an image |
| 2 | The tape packs | drawn by the player, from `pack` and the position |
| 3 | The tape, round the guides | drawn by the player, from `tape` and the packs |
| 4 | The hubs | `layers.hub`, an image, at each of `reels`, turned as the tape moves |
| 5 | Everything in front of the works, the label's writing included | `layers.front.A` or `.B`, an image, for the side facing up |

```json
{
  "format": "cass-look", "version": 1,
  "design": "tapefour.sd/1",
  "labelText": "demo songs – oct",
  "shell": { "width": 1004, "height": 638, "unit": "0.1mm" },
  "scale": 2,
  "layers": { "back": "art/back.png", "front": { "A": "art/front-a.png", "B": "art/front-b.png" },
              "hub": { "image": "art/hub.png", "size": 224 } },
  "reels": [ { "x": 292, "y": 300 }, { "x": 712, "y": 300 } ],
  "pack": { "minDiameter": 208, "fullDiameter": 473, "inner": "#3a2618ff", "outer": "#160c06ff",
            "rings": { "colour": "#ffffff0b", "width": 2, "spacing": 26, "inset": 10, "stop": 104 } },
  "tape": { "colour": "#2c1a0eff", "width": 4,
            "guides": [ { "x": 118, "y": 586 }, { "x": 886, "y": 586 } ], "wrapRadius": 30 },
  "label": { "x": 68, "y": 398, "w": 868, "h": 80 }
}
```

- **Units**: every length is in tenths of a millimetre on the shell, from its top left corner,
  with the cassette side A up. A compact cassette's shell is 100.4 × 63.8 mm (1004 × 638).
- **scale**: image pixels per unit. The back and front images cover the whole shell
  (`width × scale` by `height × scale` pixels); the hub image is `size × scale` square and is
  centred on each reel. Images are PNG with transparency.
- **design**: who drew it, which design and which revision of the drawing (`tapefour.sd/1`).
  Informative; a writer uses it to know when to redraw its own looks. **labelText**: the label's
  writing as it appears on the front images.
- **pack**: the tape wound on each hub is a ring centred on its reel, from the hub's winding
  surface (`min`) out: nothing is drawn inside it, so the hub's openings show the shell behind. With a fraction `f` of the
  tape on a hub (`1 − f` on the other; at the start of a side it is all on the left, leader and
  all), its diameter is `√(min² + (full² − min²) · f)`, so the tape's area is kept. `min` is the
  hub's winding surface; `full` is every millimetre of this cassette's tape, leaders included,
  wound on it: for its length L (mm) and thickness t (mm), `√(min² + 4 L t / π)`. The TF-4 uses
  16 µm tape for a C-46 or C-60, 11 µm for a C-90. It is filled with a radial
  gradient from `inner` at the centre to `outer` at the edge. Rings of `rings.colour`,
  `rings.width` wide, are drawn every `rings.spacing` from `inset` inside the edge down to `stop`.
  Colours are `#rrggbbaa`.
- **tape**: a line `width` wide in `colour`. It leaves each pack on the outside, on the line
  that touches both the pack and the nearest guide (circles of radius `wrapRadius`), runs round
  the underside of each guide, and straight between their bottoms.
- **hubs** turn anticlockwise as the tape moves from left to right (playing), each by the
  length of tape that passed divided by its own pack's radius (at 1⅞ ips, 47.6 mm a second):
  the smaller pack turns faster. Winding, they turn faster than a screen can show, and a player
  should blur them rather than let them strobe.
- **Side B** up: draw the same layers with `front.B`. (A cassette turned over shows its other
  face; the shell's moulding is symmetrical, and its print and label are the ones on that face.)
- A reader keeps members it doesn't know when it rewrites the file. Image names must lie
  directly in `art/`.

`Tools/cass_look.py` is a reference drawing of a cassette from its look, in Python.

## Wear and age (how a deck uses them)

The cassette records what happened to it; each deck decides how that sounds. TAPE·FOUR's model
(`Source/Wear.h`), kept gentle on purpose: cassettes are tougher than their reputation.

- **Passes** (`wear.bin`), at the point being played: up to about 1 dB of treble at 10 kHz (a fifth
  of a dB after 100 passes), up to 2 dB more modulation noise, and dropouts becoming more
  frequent (twice as many after 400 passes). Dropouts are fixed places on a cassette, worked
  out from its id, so they repeat on every play.
- **Age** since the cassette was made (`created`): a little treble, at most 0.3 dB.
- **Print-through**, from the day each stretch was recorded (byte 4–5 of the map): the
  recording slowly copies itself onto the turns of tape either side of it as it sits wound,
  heard as a faint pre-echo and post-echo one turn away (1.5 to 3 seconds, depending on
  where on the tape). About −61 dB after a day, −54 dB after a year, −51 dB after ten.

A deck that doesn't honour a cassette's condition (`honoursTapeCondition` false) ignores all of
this, and the cassette's starting condition, and plays it as new.

## Versions

`version` rises when a reader of an older version would misread the cassette. Additions
that older readers can safely ignore (new members, new files) don't change it.
