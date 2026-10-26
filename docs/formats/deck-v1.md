# The deck format (.deck), version 1

A `.deck` file describes a cassette machine: what it can do, and the state of its mechanism and
heads. It holds no cassette and no session. The same [.cass](cass-v1.md) sounds different in
different decks, as real tapes do.

The reference reader and writer is `Formats/Deck.{h,cpp}` (plain C++17, no dependencies).
TAPE·FOUR keeps its own machine as `TF-4.deck` beside its settings.

```json
{
  "format": "deck",
  "version": 1,
  "id": "tapefour.tf-4",
  "name": "TAPE·FOUR TF-4",
  "maker": "78Hz",
  "kind": "multitracker",
  "capabilities": {
    "layouts": [ "4-track-one-way" ],
    "records": true,
    "heads": 2,
    "autoReverse": false,
    "speedsIps": [ 1.875, 3.75 ],
    "nrSystems": [ "compander-2to1", "sliding-b", "sliding-c" ],
    "tapeSwitch": "manual"
  },
  "transport": { "wowPct": 0.15, "flutterPct": 0.075, "driftPct": 0.2, "speedErrorPct": 0 },
  "heads": { "cutoffHz": 15157, "crosstalkDb": -65, "azimuthErrorMinutes": 0 },
  "condition": "serviced",
  "honoursTapeCondition": true
}
```

## What it is

- **id**: stable and unique to the model of machine, reverse-domain style. Cassettes record it
  in their `recorders` list.
- **kind**: `multitracker`, `hifi`, `portable`, `boombox` or `car`.
- **capabilities.layouts**: the cassette layouts it plays. A deck that doesn't list a cassette's
  layout plays what its heads see (a stereo deck reads a 4-track tape's tracks in pairs).
- **records**: false for a player.
- **heads**: 2 (one head records and plays) or 3 (separate heads: it can play what it has just
  recorded).
- **speedsIps**, **nrSystems**, **tapeSwitch** (`manual`, or `auto` from the cassette's sensing
  holes).

## How well it does it

- **transport**: peak wow (slow, under 6 Hz) and flutter (fast) as a percentage of speed, slow
  drift, and a constant speed error (many portable players run fast).
- **heads**: `cutoffHz`, where worn or misaligned heads start losing treble (applies to
  recording and playback alike); `crosstalkDb`, how much of each neighbouring track a head
  picks up; `azimuthErrorMinutes`, the head's alignment in minutes of arc against a
  cassette recorded by a perfectly aligned deck.
- **condition**: `new`, `serviced`, `worn` or `neglected`. A name for the numbers above; the
  numbers are what count.
- **honoursTapeCondition**: false plays every cassette as if new: its formulation counts, its
  starting condition, wear and age don't.

Values outside what a machine can be are limited when read (wow and flutter to 5%, cutoff to
1–30 kHz, crosstalk to −120…−20 dB).

## Versions

As for cassettes: `version` rises only when an older reader would misread the deck. Readers
keep members they don't know when they rewrite the file.
