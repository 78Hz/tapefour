#pragma once

#include "Common.h"
#include <mutex>
#include "InstrumentPresets.h"

/*
    TF PLATE: a plate reverb. A thin sheet (steel, or the EMT 240's gold foil) is held under tension in a
    frame; a driver near its middle shakes it, pickups elsewhere hear it, and a porous damper set some
    distance from it takes the energy out: how far sets how long it rings (EMT's figure: the decay at
    500 Hz).

      SIMULATED   the plate as its modes: a thin plate under tension, simply supported at its edges, its
                  every resonance from its size, thickness, the metal's stiffness and density and the
                  tension (omega^2 = kappa^2 k^4 + c^2 k^2), driven at the driver and heard at each pickup
                  as the mode shapes there say. The bending waves are dispersive (highs travel faster), so
                  a click arrives bright first and fills in; the modes are as dense in every octave. The
                  modes are computed up to a few kHz (a budget of them); above that, where no single
                  resonance can be heard, a dense network matched to the same decay carries on.
      LIGHT       that network alone, through a chain of allpasses for the dispersion: a fraction of the work.

    Losses (the damper, the air, the metal): the decay at 500 Hz is what DAMPING sets; highs go faster.
    CONDITION: the tension (a slack plate's lows are looser) and how fresh it is (an aged one is duller
    and doesn't ring as long). The pickups feed a recovery amplifier: a high-pass where EMT's turns over,
    and BASS and TREBLE. Which values are published and which chosen: docs/plate-reverb.md.
*/
namespace tf::plate
{
struct PlateInfo
{
    const char* label;       // for the chips
    const char* name;        // for the window's heading
    const char* size;        // "2 x 1 M, 0.5 MM"
    double lx, ly, h;        // metres
    double youngs, density, poisson;
    double minDecay, maxDecay;   // seconds at 500 Hz, the damper closest and furthest
    double driverX, driverY;     // as fractions of the plate
};
inline constexpr int numPlates = 3;
const PlateInfo& plateInfo (int plate);

/** The plate's physics: kappa = sqrt (D / rho h) (m^2/s), and the modes per hertz. */
double kappa (const PlateInfo&);
double modesPerHz (const PlateInfo&);
/** The decay (s, to -60 dB) at a frequency, for a decay at 500 Hz and a condition. */
double decayAt (double hz, double t500, double condition);

/** The modes as built for a plate, a condition and a rate. */
struct Modes
{
    int plate = -1;
    double condition = -1.0, sampleRate = 0.0, topHz = 0.0;
    int count = 0, padded = 0;
    std::vector<float> cosw, hz, gin;          // each mode's frequency (cos w), Hz, and how the driver moves it
    std::vector<float> kx, ky;                 // its shape: sin (kx x) sin (ky y), x and y as fractions
    std::vector<float> a1, a2, goutL, goutR, y1, y2;   // the resonators (set for a decay and the pickups)
    float tailGain = 0.0f;                     // the network above topHz, matched to the modes' level
    // The modes run at a fraction of the rate (they're all below topHz): in through a low-pass, every
    // decim-th sample; out zero-stuffed and low-passed again.
    int decim = 1, phase = 0;
    Biquad inLp1, inLp2, outLp1L, outLp2L, outLp1R, outLp2R;
    void build (int plate, double condition, double sampleRate, int budget);
    void setDecay (double t500, double conditionNow);
    void setPickups (float lx, float ly, float rx, float ry);
};

/** A dense network of delays: the plate's tail (above the modes), or all of it (LIGHT). */
class Network
{
public:
    void build (const PlateInfo&, double sampleRate);
    void reset();
    void setDecay (double t500, double condition, double sampleRate);
    void process (float in, float& outL, float& outR) noexcept;
    static constexpr int lines = 8, dispersers = 10;

private:
    std::vector<float> buf[lines];
    int len[lines] {}, pos[lines] {};
    float g0[lines] {}, pole[lines] {}, lp[lines] {};
    float apState[dispersers] {}, apCoef = 0.55f;
    std::vector<float> diff[4];
    int diffPos[4] {};
};

class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the window ----
    juce::RangedAudioParameter* param (const juce::String& id) const;
    float get (const juce::String& id) const;
    void set (const juce::String& id, float value);
    int plateIndex() const;
    bool simulated() const;
    /** The decay at 500 Hz the damper gives now, and how far it is from the plate (mm). */
    double decay500() const;
    double damperMm() const;
    int modesInUse() const { return modesShown.load(); }
    /** How many times new modes have taken over from the builder (for the tests). */
    int handovers() const { return handoverCount.load(); }
    double modesTopHz() const { return topShown.load(); }
    /** The response to a click (mono, at the first pickup), for the window's picture or a test. */
    static std::vector<float> click (int plate, bool simulated, double decay, double condition, double sampleRate, double seconds,
                                    float px = 0.18f, float py = 0.28f, std::function<bool()> stop = {});
    /** The tail's level now, a value per 1/50 s, for the window. */
    float levelNow() const { return level.load(); }

    // juce::AudioProcessor
    const juce::String getName() const override { return "TF PLATE"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 6.0; }
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

    static constexpr int modeBudget = 4000;

private:
    class Builder;
    juce::AudioParameterFloat* addFloat (const juce::String& id, const juce::String& name, float lo, float hi, float def);
    juce::AudioParameterBool* addBool (const juce::String& id, const juce::String& name, bool def);
    juce::AudioParameterChoice* addChoice (const juce::String& id, const juce::String& name, const juce::StringArray&, int def);
    void processModes (Modes&, const float* in, float* l, float* r, int n, float gain) noexcept;
    void buildNetwork (int plate);
    double networkEnergy = 1.0;

    std::map<juce::String, juce::RangedAudioParameter*> params;
    juce::AudioParameterChoice *pEngine, *pPlate;
    juce::AudioParameterFloat *pDamping, *pCondition, *pPredelay, *pWidth, *pBass, *pTreble, *pLx, *pLy, *pRx, *pRy;
    juce::AudioParameterBool* pStereo;

    // the audio thread's
    double sr = 48000.0;
    std::unique_ptr<Modes> modes, fading;       // the modes in use (and the ones going, over a crossfade)
    std::atomic<Modes*> incoming { nullptr };   // built by the builder, taken at the next block
    std::atomic<Modes*> retired { nullptr };    // done with: the builder frees it
    int fadeLeft = 0;
    static constexpr int fadeLength = 2048;
    Network network;
    int networkPlate = -1;
    double lastDecay = -1.0, lastCondition = -1.0;
    float lastPick[4] { -1, -1, -1, -1 };
    std::vector<float> pre, inScratch, l2, r2;
    int prePos = 0;
    Biquad hpL, hpR, bassL, bassR, trebleL, trebleR, topL, topR, top2L, top2R;
    float lastBass = 99.0f, lastTreble = 99.0f, lastTop = -1.0f, levelHold = 0.0f;
    std::atomic<int> modesShown { 0 }, handoverCount { 0 };
    std::atomic<double> topShown { 0.0 };
    std::atomic<float> level { 0.0f };
    std::atomic<int> wantPlate { 0 };
    std::atomic<double> wantCondition { 0.7 }, wantRate { 48000.0 };
    // What the modes in hand (or on their way) were built for, whoever built them: prepareToPlay
    // or the builder. Under publishLock (never taken on the audio thread), with a count of prepares
    // so a build that was overtaken by one is thrown away rather than crossfaded in.
    std::mutex publishLock;
    int havePlate = -1, prepares = 0;
    double haveCondition = -1.0, haveRate = 0.0;
    std::unique_ptr<Builder> builder;
};

const presets::Kind& presetKind();
} // namespace tf::plate
