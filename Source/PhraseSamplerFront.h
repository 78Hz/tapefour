#pragma once

#include "PhraseSampler.h"

namespace tf::sampler
{
/**
    The SU10's front panel as its owner's manual has it (pp. 7-12, 31-54): the keys and pads, and the modes
    they move between, on the message thread. In PLAY the pads play and the keys go to the processor; with
    SHIFT held the pads are SONG 1-4, EDIT, UTILITY, SCALE, FILTER, CROSSFADER, PITCH, EXT SCRATCH and
    EXT FILTER (in that order: the order the manual lists them; the panel's printing isn't in its text).

      EDIT     SHIFT+EDIT, then the pad and ENTER; < > through the 13 edit functions; +1/-1 change them;
               TOP, BWD and FWD jump to the start, loop and end points; COPY, MOVE, SPLIT take a pad and
               ENTER; COMPACT and DELETE ask "Sure?" and take ENTER twice. EXIT back to PLAY.
      REC      with no song chosen: the pad and ENTER, then < > through the recording settings, PLAY
               starts, STOP ends. With a song chosen: PLAY starts the song's recording at the first pad.
      UTILITY  SHIFT+UTILITY: the MIDI settings, deleting a song, INIT DATA and INIT ALL.
*/
class Front
{
public:
    explicit Front (Processor&);

    enum class Mode { play, edit, utility, recPad, recSettings, songRec };

    /** A key pressed (down) or let go. repeat counts a held key's repeats (0 the first press): the
        values move faster the longer +1 or -1 is held. Names: SHIFT SCRATCH HOLD REV BANK EXIT ENTER
        +1 -1 < > REC TOP BWD STOP PLAY FWD. */
    void key (const juce::String& name, bool down, int repeat = 0);
    void pad (int padInBank, bool down, float velocity = 0.8f);

    /** The LCD, as its compartments show it (manual p. 11-12). */
    struct Lcd
    {
        juce::String grade, mode, loop, format, input, bank, effect, song;
        bool songRepeat = false;
        juce::String top, bottom;           // the message area: the parameter (or prompt), and its value
        bool midi = false, time = false, tempo = false, rec = false;
        juce::String meterLabel, meter;     // the meter area
        float bar = -1.0f;                  // the ribbon's value (or the input level), 0..1; -1 none
    };
    Lcd lcd() const;

    Mode mode() const { return current; }
    bool shiftHeld() const { return shift; }
    /** The pad EDIT is on (0..47). */
    int editPad() const { return padEdited; }
    /** What the pads do with SHIFT held: the labels printed above them. */
    static const char* shiftLabel (int padInBank);
    static juce::String padName (int index);
    static juce::String inputName (const Processor::RecordSettings&);

    // the edit functions (EDIT mode)
    enum Function { lpMode, sdMode, padVol, altGrp, noteNum, stPnt, lpPnt, edPnt, copyTo, moveTo, spltTo, compct, del, numFunctions };
    int function() const { return fn; }

private:
    void keyNow (const juce::String& name, bool down, int repeat);
    void playKey (const juce::String& name, bool down);
    void shiftPad (int padInBank);
    void editKey (const juce::String& name, int repeat);
    void utilityKey (const juce::String& name, int repeat);
    void recKey (const juce::String& name, int repeat);
    void change (int delta, int repeat);
    void moveFunction (int dir);
    void flash (const juce::String& m) { note = m; noteUntil = juce::Time::getMillisecondCounterHiRes() + 1500.0; }
    int bank() const { return proc.panel().bank; }
    double clock() const { return clockHz (proc.grade(), proc.pitchPercent()); }

    Processor& proc;
    Mode current = Mode::play;
    bool shift = false;
    int padEdited = 0, fn = 0, util = 0, recFn = 0, destination = -1, utilSong = 0;
    bool choosingPad = false, sure = false;
    juce::String note;
    double noteUntil = 0.0;
};
} // namespace tf::sampler
