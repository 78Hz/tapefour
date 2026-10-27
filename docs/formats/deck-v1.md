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
  "transport": { "wowPct": 0.15, "flutterPct": 0.075, "driftPct": 0.2, "speedErrorPct": 0, "windSecondsC60": 120 },
  "heads": { "cutoffHz": 15157, "crosstalkDb": -65, "azimuthErrorMinutes": 0 },
  "cue": { "levelDb": -18, "spacingUm": 3 },
  "counter": { "kind": "mechanical", "countsPerTurn": 0.75 },
  "electronics": { "channelNoiseDb": -76, "busNoiseDb": -90 },
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
- **speedsIps**, **nrSystems** (`compander-2to1`: dbx type II; `sliding-b`: Dolby B;
  `sliding-c`: Dolby C), **tapeSwitch** (`manual`, or `auto` from the cassette's sensing
  holes).

## How well it does it

- **transport**: peak wow (slow, under 6 Hz) and flutter (fast) as a percentage of speed, slow
  drift, and a constant speed error (many portable players run fast).
- **heads**: `cutoffHz`, where worn or misaligned heads start losing treble on playback (gap
  loss is a playing effect); `crosstalkDb`, how much of each neighbouring track a head
  picks up; `azimuthErrorMinutes`, the head's alignment in minutes of arc against a
  cassette recorded by a perfectly aligned deck. (TAPE·FOUR's own machine is aligned, so it
  doesn't apply this yet.)
- **transport.windSecondsC60**: fast wind, end to end, of one side of a C-60. The take-up
  spindle is driven at about a constant speed, so the tape speeds up as its pack grows (about
  9 times playing speed at the start of a C-60, 20 times at the end).
- **cue**: cue and review (PLAY while winding): the head rides lightly on the tape, `levelDb`
  down, `spacingUm` off it (spacing loss, 54.6 d / λ dB).
- **counter**: the tape counter is the machine's. `mechanical`: three digits driven by the
  take-up spindle, `countsPerTurn` counts per turn, so not linear in time and different from
  one machine to another; `realtime`: minutes and seconds of playing time. It keeps counting
  when cassettes are changed or turned over.
- **electronics**: the machine's own noise, dB re its nominal level (0 VU), unweighted 20 Hz to
  20 kHz. `channelNoiseDb`: each mixer channel's, with its fader at the nominal mark (it follows
  the fader and the pan); `busNoiseDb`: the mix bus's and outputs', the same whatever the faders do.
  TASCAM's 424 measures 76 dB S/N line in to line out with one input open and 70 dB with four.
- **condition**: `new`, `serviced`, `worn` or `neglected`, or `custom` when set by hand. A name
  for the numbers above; the numbers are what count.
- **honoursTapeCondition**: false plays every cassette as if new: its formulation counts, its
  starting condition, wear and age don't.

Values outside what a machine can be are limited when read (wow and flutter to 5%, cutoff to
1–30 kHz, crosstalk to −120…−20 dB, wind 20–600 s, cue −60…0 dB, noise −140…−30 dB).
`cue`, `counter`, `electronics` and `windSecondsC60` were added within version 1: a deck file
without them gets the values above.

## Versions

As for cassettes: `version` rises only when an older reader would misread the deck. Readers
keep members they don't know when they rewrite the file.
