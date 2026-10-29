#pragma once

#include "Common.h"
#include "Tuning.h"
#include "InstrumentPresets.h"

/*
    TF ELECTRIC PIANO: a reed electric piano after the Wurlitzer 200 and 200A
    (docs/electric-piano.md has the sources, and which values are measured and which are set by ear).

      reeds     each key a steel reed: a fundamental and two higher bending modes (inharmonic), each
                ringing down at its own rate; low reeds ring longest. Tuned to the tuning system.
      hammers   a hammer hits the reed from below and stays on it for about a cycle of its pitch, so
                it barely excites the higher modes. A hammer let off too early doesn't reach the reed.
      pickup    each reed sits between two fingers of a charged comb (about 150 V), its face level
                with theirs, resting a paper's thickness clear. Reed and fingers are a capacitor that
                grows with how much of the reed overlaps the fingers: a reed swinging into them gives
                a strong fundamental and second harmonic; one that fills them flattens (clips); one
                that swings right through folds over (the bark). Bass reeds rest below their fingers,
                the rest above.
      network   the comb's charging resistors and the input capacitor: below about 880 Hz the output
                follows how fast the capacitance changes, so the bass's big swing comes out level.
      dampers   felt dampers stop the reeds when keys come up, unless the sustain pedal holds them.
      amp       preamp (drive), vibrato (5.75 Hz, the preamp's gain switched), volume, and either the
                power amp and the two 4x8" oval speakers or the rear jack (the preamp's output); hum.

    One instrument, mono. It ages with playing time: reeds drift, felt hardens, reeds go short.
*/
namespace tf::ep
{
/** The controls, as numbers: everything that decides the sound, for the audio thread and the editor alike. */
struct Settings
{
    int model = 1;                 // 0: 200, 1: 200A
    bool ages = true;
    // reeds
    float tuningSpreadCents = 3.0f, ringTime = 1.0f;
    int shortRingers = 1;
    // hammers
    float felt = 0.5f, strikePoint = 0.0f, blowInches = 1.078f, letOffInches = 0.125f, velocityCurve = 0.0f;
    // pickup: reedHeight 0..1 sets where the reed rests against its fingers (restMm), gapMm the side gap
    float reedHeight = 0.5f, gapMm = 0.064f, volts = 165.0f;
    // dampers and noises (dB)
    float damping = 0.7f, damperNoiseDb = -56.0f, actionNoiseDb = -50.0f, pedalNoiseDb = -58.0f;
    // amp
    float volume = 7.0f, vibrato = 0.4f, drive = 0.3f, humDb = -78.0f;
    bool rearJack = true, mains60 = false;
    // playing
    bool mpe = false, bend = true;
    int pressureTarget = 0;        // 0 off, 1 volume, 2 vibrato depth, 3 preamp drive, 4 damper
    float pressureAmount = 0.6f;
    int slideTarget = 0;           // 0 off, 1 hammer felt, 2 reed height
};

/** A condition: the technician's controls for a piano in that state (amp settings are left alone). */
void applyCondition (Settings&, int condition);   // 0 NEW, 1 SERVICED, 2 WORN, 3 NEGLECTED
const char* conditionName (int condition);

/** What each key's reed is like, from the settings, the piano's own random seed and its playing time. */
struct Reed
{
    double hz = 0.0;               // tuned frequency, drift included (0: the keyboard map leaves it unmapped)
    double driftCents = 0.0;       // how far it has wandered from the tuning system
    double ringSeconds = 0.0;      // time constant of the fundamental's decay
    bool shortRinger = false;
    bool onKeyboard = false;       // one of the real piano's 64 keys (A1..C7)
};
inline constexpr int lowestKey = 33, highestKey = 96;   // A1 .. C7, the 64 keys
std::array<Reed, 128> reeds (const Settings&, const std::array<double, 128>& tuned, uint32_t seed, double hours);

/** How the piano has aged: what playing time adds to its spread, felt, short ringers and dampers. */
struct Ageing { double spreadCents = 0, felt = 0, damping = 0; int shortRingers = 0; };
Ageing ageing (double hours);

/** The pickup at one key, end on (mm): the reed's tongue, the fingers either side of it, where it rests. */
struct PickupGeometry
{
    double reedThickness = 0.5, fingerThickness = 0.6;
    double rest = 0.08;            // the reed's face clear of the fingers' face (negative: resting inside them)
    double sideGap = 0.064;        // each side, reed to finger
    double edge = 0.2;             // how gradually the overlap starts (the fingers' fringe, the reed bending along them)
    bool below = true;             // the reed rests below its fingers (bass) or above (the rest)
};
PickupGeometry pickupGeometry (const Settings&, int key);
double restMm (float reedHeight);  // the REED AT REST control in mm

/** How far a reed's tip swings struck as hard as the action allows (mm). */
double fullSwingMm (double reedHz);
/** How long the hammer stays on the reed (s): about one cycle of the reed's pitch. */
double contactSeconds (double reedHz, double hammerSpeed, double felt);
/** The pickup's source for a reed tip at displacement x mm (positive: up, the hammer's way), before the
    network. 0 at rest; about 1 when the reed fully overlaps a 0.5 mm finger at the standard gap. */
double pickup (const Settings&, double xMm, int key);
/** The comb network's corner: below it the output follows the capacitance's rate of change. */
inline constexpr double networkCornerHz = 880.0;

/** C4's ring time to -40 dB, seconds (for the REEDS readout). */
double ringToMinus40 (const Reed&);

//==============================================================================
class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    /** The parameters, in the editor's terms; read on any thread. */
    Settings settings() const;
    /** Sets parameters from a settings struct (message thread: conditions). */
    void setSettings (const Settings&);
    void chooseCondition (int condition);   // sets the controls and the playing time back to zero
    int lastCondition() const noexcept { return condition.load(); }

    tuning::Settings tuningSettings() const;
    void setScale (const tuning::Scale&, const std::string& sclText);
    void setKeyboardMap (const tuning::KeyboardMap&, const std::string& kbmText);
    void clearKeyboardMap();
    juce::String scaleDescription() const;
    bool hasKeyboardMap() const;

    double hoursPlayed() const noexcept { return hours.load(); }
    uint32_t seed() const noexcept { return reedSeed; }
    std::array<Reed, 128> currentReeds() const;

    void applyMpe (bool zoneOn, int bendRange) noexcept;

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 6.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return editorFactory != nullptr; }

    /** Its editor, in a window of its own like a plug-in's (the app sets this: the engine has no user interface). */
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int editorTab = 0;   // the tab the editor was last on
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** The whole piano (controls, tuning files, seed, condition; its age if withAge), and back. */
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    /** The parameters by id, for the editor. */
    juce::RangedAudioParameter* param (const juce::String& id) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    std::atomic<double> hours { 0.0 };
    std::atomic<int> condition { 1 };
    uint32_t reedSeed = 0;
    std::atomic<bool> zone { false };
    std::atomic<int> zoneBend { 48 };
};
/** The piano's presets: one file each (.tfep) in Documents/TapeFour/Presets/Electric Piano, the whole piano in each. */
const presets::Kind& presetKind();
namespace presets
{
    using Entry = tf::presets::Entry;
    using Kind = tf::presets::Kind;
    inline juce::File folder()                                { return tf::presets::folder (presetKind()); }
    inline juce::File fileFor (const juce::String& name)      { return tf::presets::fileFor (presetKind(), name); }
    inline std::vector<Entry> list()                          { return tf::presets::list (presetKind()); }
    inline bool save (Processor& p, const juce::String& n, juce::String& e)    { return tf::presets::save (p, n, e); }
    inline bool load (Processor& p, const juce::File& f, juce::String& e)      { return tf::presets::load (p, f, e); }
    inline bool rename (Processor& p, const juce::String& n, juce::String& e)  { return tf::presets::rename (p, n, e); }
    inline bool remove (Processor& p, juce::String& e)                         { return tf::presets::remove (p, e); }
}
} // namespace tf::ep
