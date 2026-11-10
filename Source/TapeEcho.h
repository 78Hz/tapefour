#pragma once

#include "Common.h"
#include "InstrumentPresets.h"

/*
    TF TAPE ECHO: a cassette loop delay. An endless cassette runs past an erase head, a record head and
    three playback heads that can sit anywhere on the loop. The tape is kept as a fixed length of tape
    (cells, like the deck's cassettes), so RATE (the motor) changes how fast it passes the heads: every
    head's delay at once, with the pitch bending as it does on a real machine.

      record      the input and the heads' feedback (INTENSITY), driven into the tape (DRIVE), which
                  saturates at the formulation's MOL and loses treble at the head as cassette tape does
                  at its speed (NORMAL, CHROME, METAL: the deck's SD, XR and MX)
      erase       just before the record head: with ERASE off nothing is wiped, so each turn plays what
                  was there under what's new (sound on sound); HOLD stops recording and erasing
      playback    each head reads the tape where it sits: wow and flutter, the tape's hiss, dropouts
                  (fixed places on the loop, more as it ages) and the splice, once a turn
      out         BASS and TREBLE on the repeats (and so on what goes round again); SPREAD pans heads
                  1-3 from left to right. The block's MIX blends it with the dry sound.
*/
namespace tf::echo
{
inline constexpr int numHeads = 3;
inline constexpr double cellRate = 48000.0;              // cells of tape per second at 1 7/8 ips
inline constexpr double mmPerNominalSecond = 47.625;      // 1 7/8 ips
inline constexpr double tooCloseMm = 5.0;                 // heads nearer than this couldn't be built
inline constexpr double eraseGapSeconds = 0.25;           // the erase head, before the record head (at 1 7/8)
inline constexpr double loopSeconds[] { 10.0, 20.0, 30.0, 60.0 };
inline constexpr int numLoops = 4;
inline constexpr int overviewBins = 400;

/** The head 1 note values the BPM is shown for, in quarter notes. */
struct NoteValue { const char* label; double quarters; };
inline constexpr NoteValue noteValues[] { { "1/16", 0.25 }, { "1/8T", 1.0 / 3.0 }, { "1/8", 0.5 }, { "1/8.", 0.75 },
                                          { "1/4T", 2.0 / 3.0 }, { "1/4", 1.0 }, { "1/4.", 1.5 } };
inline constexpr int numNoteValues = 7;

/** PLACE: heads 2 and 3 as multiples of head 1. */
struct Placement { const char* label; double h2, h3; };
inline constexpr Placement placements[] { { "STRAIGHT 1:2:3", 2.0, 3.0 }, { "DOTTED 1:1.5:2", 1.5, 2.0 }, { "TRIPLET 1:1.33:1.67", 4.0 / 3.0, 5.0 / 3.0 } };
inline constexpr int numPlacements = 3;

class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the window ----
    juce::RangedAudioParameter* param (const juce::String& id) const;
    float get (const juce::String& id) const;          // in its own units
    void set (const juce::String& id, float value);    // in its own units
    /** A head's place: seconds of tape after the record head at 1 7/8 ips (anywhere on the loop). */
    double headPos (int h) const;
    void setHeadPos (int h, double nominalSeconds);
    bool headOn (int h) const;
    void setHeadOn (int h, bool on);
    double loopNominal() const;                         // the loop's length at 1 7/8
    double rate() const;                                // the motor: 0.25..2 of 1 7/8 ips
    /** Seconds from the record head to a head (or along the loop) at the motor's speed now. */
    double toSeconds (double nominal) const { return nominal / rate(); }
    double toNominal (double seconds) const { return seconds * rate(); }
    /** The tempo head 1 makes, with its note value. */
    double bpm() const;
    /** Moves the motor so the tempo is this. */
    void setBpm (double bpm);
    int noteValue() const;
    void setNoteValue (int);
    void place (int placement);
    /** Whether two heads (or a head and the record head) are too close to have been built. */
    bool tooClose (int h) const;
    /** What's on the loop: a level for each stretch of it, and where the record head is (0..1). */
    float overview (int bin) const { return bins[(size_t) bin].load(); }
    double recordAt() const { return recFraction.load(); }
    /** The record level, dB re the tape's reference (0 VU), and resets the peak. */
    float takeRecordPeakDb();

    // juce::AudioProcessor
    const juce::String getName() const override { return "TF TAPE ECHO"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 8.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return true; }
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

private:
    void fromOldEcho (const juce::XmlElement&);
    juce::AudioParameterFloat* addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def, float skewCentre = -1.0f);
    juce::AudioParameterBool* addBool (const juce::String& id, const juce::String& name, bool def);
    juce::AudioParameterChoice* addChoice (const juce::String& id, const juce::String& name, const juce::StringArray&, int def);

    std::map<juce::String, juce::RangedAudioParameter*> params;
    juce::AudioParameterFloat *pRate, *pIntensity, *pBass, *pTreble, *pSpread, *pAge, *pWow, *pDrive;
    juce::AudioParameterFloat* pHead[numHeads];
    juce::AudioParameterBool* pOn[numHeads];
    juce::AudioParameterBool *pErase, *pSplice, *pHold;
    juce::AudioParameterChoice *pTape, *pLoop, *pNote;

    // the audio thread's
    std::vector<float> tape;              // the whole loop's cells (the longest loop)
    std::vector<float> dropout;           // gain along the tape, a value every dropoutCells
    static constexpr int dropoutCells = 480;
    float dropoutFor = -1.0f;
    double sr = 48000.0, writePos = 0.0, speed = 1.0, lastRecValue = 0.0;
    double ph[4] {};
    double readPrev[numHeads] {};
    float clickEnv[numHeads] {};
    Biquad recLp1, recLp2, fbBass, fbTreble, outBassL, outTrebleL, outBassR, outTrebleR;
    float lastLpHz = -1.0f, lastBass = 99.0f, lastTreble = 99.0f;
    juce::Random rng { 0x7ec0 };
    int lastBin = -1;
    std::array<std::atomic<float>, overviewBins> bins {};
    std::atomic<double> recFraction { 0.0 };
    std::atomic<float> recPeak { 0.0f };
};

const presets::Kind& presetKind();
} // namespace tf::echo
