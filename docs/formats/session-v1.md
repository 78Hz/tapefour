# The session (TAPE·FOUR), version 1

A session is how you have the TF-4 set up around one tape: the mixer, the signal chains and
their plug-ins, MIDI routing and mappings, and the operator's controls. It is TAPE·FOUR's own,
not part of the [cassette](cass-v1.md) or [deck](deck-v1.md) formats, and is never put in a
`.cass` file.

It is kept beside its cassette on the shelf, as `session.xml` in the cassette's folder, and
recalled when the tape goes in. (TAPE·FOUR versions before the cassette format called it
`deck.xml`, inside a folder with `tape.xml`; they are converted the first time they are opened.)

```xml
<DECK version="5">
  <TRACK index="0" feed="live" input="0" pair="0" bounce="0" mute="0" solo="0"
         gain="0" high="0" mid="0" midFreq="1200" low="0" pan="0" fader="0.8"
         midiIn="any">
    <CHAIN>
      <BLOCK kind="plugin" … />          <!-- each block's state and its MIDI mappings -->
    </CHAIN>
  </TRACK>
  …
  <MASTER fader="0.8" phones="0.7"/>
  <TAPE_DECK speed="1" pitchPct="0" nr="1" nrType="0" sat="0.5"/>
</DECK>
```

- **TRACK**: input, feed (live or bounce) and bounce sources, mute, solo, EQ, pan and fader;
  which MIDI controller plays it; its chain of blocks.
- **MASTER**: the master fader and the phones level.
- **TAPE_DECK**: the operator's controls on the TF-4: tape speed (1 or 2: 1⅞ or 3¾ ips), pitch,
  noise reduction on or off and its type, tape drive (record level calibration), and `sideB`
  when the cassette was left in the deck turned over. The machine itself (wow, flutter, heads,
  its counter) is in `TF-4.deck`, and the counter's reading in the settings; the cassette's
  condition is in the cassette. (Older sessions' `counterZero` is ignored: the counter belongs to
  the machine.)

Version 5 is the first with the cassette format: wow, flutter, drift, head response,
crosstalk, hiss and dropouts left the session. Earlier sessions still load; those values
become the machine's and the cassette's when the tape is converted.
