#pragma once

#include "Common.h"
#include "InstrumentPresets.h"
#include "TapeFeed.h"

/*
    TF PHRASE SAMPLER: after the Yamaha SU10 sampling unit (1995)
    (docs/phrase-sampler.md has the sources, and which values are published and which are set by ear).

      memory      384 KB, keeping its samples with the power off, shared by 48 pads (12 pads, banks A-D).
                  Samples are stored as 4-bit ADPCM, Yamaha's, which is where its boxy, compressed sound
                  comes from
      grade       one sample clock for everything: HI, STD, LONG, EXLG trade quality for time (about 19,
                  27, 38 and 54 seconds in all); the global PITCH (-20 % to +10 %) moves the clock itself,
                  recording and playing, so a sample made at one grade plays at another pitch at another
      pads        start, loop and end points; five loop modes; NOTE, TOGGLE or TRIGGER; volume, alternate
                  group, MIDI note. HOLD latches, REV plays backwards
      voices      four mono voices (a stereo sample takes two), fewer at HI and with the ribbon's effects;
                  the oldest is cut when there aren't enough
      ribbon      PITCH, FILTER (low, band or high pass, flat, weak or strong resonance), SCALE (one
                  sample across the 12 pads in semitones), SCRATCH (a window of a sample, as a record),
                  CROSSFADER (the samples against the input), EXTERNAL SCRATCH and EXTERNAL FILTER (the
                  input, live)
      songs       four, recorded in real time from the pads, about 1000 events in all

    It takes the track's audio (its LINE IN), and can sample a track's playback from the tape, or a file.
*/
namespace tf::sampler
{
inline constexpr int banks = 4, padsPerBank = 12, numPads = banks * padsPerBank;
inline constexpr int memoryBytes = 384 * 1024;
inline constexpr int memoryNibbles = memoryBytes * 2;   // 4-bit ADPCM: a mono sample point is a nibble
inline constexpr int songEventLimit = 1000;               // 8 KB of song memory
inline constexpr int numSongs = 4;

enum class Grade { hi = 0, std, lng, exlg };
const char* gradeName (Grade);
/** The sample clock (Hz) at a grade and a system pitch (percent). HI at +10 % is a true 44.1 kHz (the
    owner's manual); the others follow from the published recording times (docs/phrase-sampler.md). */
double clockHz (Grade, double pitchPercent);
/** Seconds of mono sampling the whole memory holds at a grade (pitch 0). */
double memorySeconds (Grade);

enum class Loop { oneShot = 0, wholeLoop, partialLoop, reverseShot, reverseLoop };
enum class Sounding { note = 0, toggle, trigger };
enum class Effect { none = 0, scale, filter, crossfade, pitch, extScratch, extFilter };
enum class FilterType { lpf = 0, bpf, hpf, bypass };
enum class Resonance { flat = 0, weak, strong };
/** What's sampled: the track's own input (what reaches the sampler in its chain), another track as it leaves
    its channel strip (before or after its fader), the mix of the other tracks, or a file. */
enum class Source { input = 0, track, file, mix };

const char* loopName (Loop);
const char* soundingName (Sounding);
const char* effectName (Effect);

/** How many mono voices sound at once, at a grade, with an effect (0: the effect isn't available there). */
int polyphony (Grade, Effect, bool scratching);
/** Whether an effect can be used at a grade (HI: none of the ribbon's; STD: no FILTER or SCALE). */
bool effectAllowed (Grade, Effect, bool scratching);

/** Yamaha's 4-bit ADPCM (as documented for its sample chips): 16-bit samples in, nibbles out, and back. */
struct Adpcm
{
    int signal = 0, step = 127;
    int decode (int nibble) noexcept;
    int encode (int sample) noexcept;   // returns the nibble, and moves the state as decoding it would
};

/** A pad: where its sample lies in memory, and how it plays. */
struct Pad
{
    bool used = false;
    bool stereo = false;
    Grade grade = Grade::lng;       // the clock it was recorded at
    double pitchPercent = 0.0;      // and the pitch
    int offset = 0, frames = 0;     // in memory (nibbles; a stereo frame is two)
    int start = 0, loopPoint = 0, end = 0;   // frames
    Loop loop = Loop::oneShot;
    Sounding sounding = Sounding::note;
    int volume = 127;               // 0..127
    int group = 0;                  // alternate group: 0 off, 1..3
    int note = -1;                  // MIDI note, or -1 none
    juce::String name;

    double recordedHz() const { return clockHz (grade, pitchPercent); }
};

struct SongEvent
{
    uint32_t ms = 0;     // from the start of the song
    uint8_t pad = 0;     // 0..47
    uint8_t kind = 0;    // 0 press, 1 release
    uint8_t velocity = 100;
    uint8_t flags = 0;   // 1 reverse, 2 hold
};

/** The front panel's state, for the editor's LCD (read on any thread). */
struct PanelState
{
    int bank = 0, lastPad = -1, voices = 0, maxVoices = 4;
    Effect effect = Effect::none;
    bool scratchArmed = false, shift = false, hold = false, reverse = false;
    int songSelected = 0;
    bool songPlaying = false, songRecording = false;
    double songSeconds = 0.0;
    bool recording = false, armed = false, clipped = false;
    double recordSeconds = 0.0;
    uint64_t padsSounding = 0;      // a bit for each pad with a voice
    int scalePad = -1, scratchPad = -1;
    double ribbonPitch = 0.0;       // PITCH's percent from the ribbon
    bool songRepeat = false;        // the song is playing round (the LCD's arrow)
    bool midiIn = false;            // a MIDI message came in lately (the LCD's MIDI)
};

/** A loop's tempo as the SU10 shows it (owner's manual p. 46): the looped part only, 80.00 to 159.99 BPM;
    to the hundredth, or the whole beat for reverse loops and very long forward ones; none under 0.375 s. */
struct Tempo
{
    bool shown = false;
    double bpm = 0.0;
    bool wholeBeats = false;
    juce::String text() const;      // "125.30", "97", or empty
};
inline constexpr double tempoShortestSeconds = 0.375;
inline constexpr double tempoWholeBeatsSeconds = 6.0;   // "very long": not published, our choice (16 beats at 160 BPM)
/** The tempo of a loop of this many seconds. */
Tempo tempoOf (double loopSeconds, bool reverse);

class Front;

//==============================================================================
class Processor : public juce::AudioProcessor,
                  public presets::Presettable,
                  public TapeFeedListener
{
public:
    Processor();
    ~Processor() override;

    // ---- the pads (message thread) ----
    Pad pad (int index) const;
    void setPad (int index, const Pad&);          // points, modes, volume, group, note, name
    int memoryUsedNibbles() const;
    double usedSeconds() const;                   // at the current grade
    double freeSeconds() const;
    /** A pad's audio as decoded (16-bit, interleaved if stereo), for the editor's waveform. */
    std::vector<int16_t> padAudio (int index) const;
    bool copyPad (int from, int to, juce::String& error);
    bool movePad (int from, int to, juce::String& error);
    bool splitPad (int index, int to, juce::String& error);   // the part after the end point to another pad
    bool compactPad (int index);                               // trims before the start and after the end
    void deletePad (int index);
    /** A file into a pad, as if recorded: at the grade and format, through the ADPCM. */
    bool importFile (const juce::File&, int toPad, juce::String& error);
    /** The loop's tempo (80..160 BPM), or 0. */
    double loopTempo (int index) const;
    /** The loop's tempo as the unit shows it (at the current grade and pitch). */
    Tempo tempo (int index) const;
    /** Whether the pad loops (and so has a tempo). */
    bool loops (int index) const;
    /** Moves the end point so the loop plays steps hundredths of a BPM faster (or slower), to the nearest
        frame, and always by at least one. False if the end can't move that way. */
    bool nudgeTempo (int index, int steps);
    /** The unit's INIT DATA (every sample and song goes) and INIT ALL (and the settings, as it came). */
    void deleteAllSamples();
    void resetEverything();

    // ---- recording ----
    struct RecordSettings
    {
        Source source = Source::input;
        int track = 0;                  // Source::track: which (0..3)
        bool postFader = true;          // a track or the mix: after the fader, or before it
        bool stereo = false;
        int gain = 8;
        int trigger = 0;
        int toPad = 0;
        Loop defaultLoop = Loop::oneShot;   // a new sample's loop mode (the unit's Defalt)
    };
    RecordSettings recordSettings() const;
    void setRecordSettings (const RecordSettings&);
    bool armRecord (juce::String& error);         // waits for the trigger level (or starts at once, manual)
    void stopRecord();                            // ends it; the sample goes into the pad

    // ---- the front panel (message thread; the audio thread acts on them) ----
    void pressPad (int padInBank, float velocity = 0.8f);
    void releasePad (int padInBank);
    /** Any of the 48 pads, whatever bank is chosen (the EDIT tab's audition). */
    void playPad (int index, bool down);
    void pressKey (const juce::String& key);      // "SHIFT", "SCRATCH", "HOLD", "REV", "BANK A".., "SCALE", "FILTER",
                                                  // "CROSSFADER", "PITCH", "EXT SCRATCH", "EXT FILTER", "EXIT",
                                                  // "REC", "TOP", "BWD", "STOP", "PLAY", "FWD", "SONG 1".., "+1", "-1"
    void releaseKey (const juce::String& key);
    void setRibbon (float position, bool touched);
    PanelState panel() const;
    /** The input's peak since last asked (the SAMPLE tab's meter), after the gain. */
    float takeInputPeak();
    juce::String lcdLine (int line) const;
    /** A message the unit is showing for a moment ("Pad Empty"), or empty. */
    juce::String flashMessage() const;
    /** The front panel's own keys and modes (PLAY, EDIT, UTILITY, recording): message thread. */
    Front& front();
    float ribbonPosition() const;

    // ---- songs ----
    /** REPEAT: PLAY plays the song round (as PLAY with HOLD does). */
    void setSongRepeat (bool);
    bool songRepeat() const;
    int songEvents (int song) const;
    int songEventsTotal() const;
    double songLength (int song) const;
    void deleteSong (int song);

    juce::RangedAudioParameter* param (const juce::String& id) const;
    Grade grade() const;
    double pitchPercent() const;

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 0.05; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return editorFactory != nullptr; }
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int editorTab = 0;
    int editorPad = 0;   // the pad the EDIT tab shows
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // presets::Presettable: the whole memory, like the original's bulk dump
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    /** The track the sampler is on (-1 not known yet): its own output can't be a source. */
    int ownTrack() const;

    // TapeFeedListener
    void tapeFeed (const TrackFeed&) noexcept override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    std::unique_ptr<Front> frontPanel;
};

/** The sampler's presets: one file each (.tfps) in Documents/TapeFour/Presets/Phrase Sampler. */
const presets::Kind& presetKind();
} // namespace tf::sampler
