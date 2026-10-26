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

| File | Required | What it is |
| --- | --- | --- |
| `cassette.json` | yes | What the cassette is (below). Written once, when the cassette is made; changes only when its label or inlay is written on, or a new deck records on it. |
| `state.json` | no | What has happened to it since: write-protect tabs, where it is wound to, when it was last used. Missing: a fresh cassette. |
| `wear.bin` | no | Passes over the heads, per second of tape. Missing: never played. |
| `trackN.pcm` | no | Track N's recording (N from 1). Missing or short: the rest is blank. |
| `trackN.map` | no | Track N's map: what was recorded where. Missing: nothing recorded. |
| `art/…` | no | Reserved for cover art and shell design. Readers that don't use it ignore it. |

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
- **position**: where the tape is wound to, in cells along it.
- **summary**: how far it is recorded and on how many tracks, so a list of cassettes needn't
  read every map. It can always be worked out again from the maps.

## Track files

**trackN.pcm**: the magnetic flux along the track, one signed 16-bit little-endian value
per cell, from the start of the tape. The file may stop short; the rest is blank tape.
This is not playable audio: it is NR-encoded where the map says so, has no playback EQ and
no hiss, and is laid out by tape position, not time.

**trackN.map**: per block of `blockCells` cells, four bytes:

| Byte | Meaning |
| --- | --- |
| 0 | Flags. Bit 0: recorded. Bit 1: NR-encoded. Bits 2–3: tape switch position it was recorded at + 1 (1 normal, 2 chrome, 3 metal; 0 unknown). Bits 4–6: NR system + 1 (1 2:1 compander, 2 sliding band B, 3 sliding band C; 0 unknown). Bit 7: reserved, 0. |
| 1 | Recorder: index into `recorders`, from 1. 0: unknown. |
| 2–3 | Speed it was recorded at, relative to the reference speed × 1000, little-endian (1000 = 1⅞ ips, 2000 = 3¾ ips, varispeed included). |

## wear.bin

One unsigned 16-bit little-endian count per second of tape (at the reference speed): how
many times that stretch has passed the heads in play or record. Fast winding doesn't count.
The whole width of the tape passes together, so it is per position, not per track.

## Versions

`version` rises when a reader of an older version would misread the cassette. Additions
that older readers can safely ignore (new members, new files) don't change it.
