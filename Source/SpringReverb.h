#pragma once

#include "Common.h"

/*
    TF SPRING REVERB: a spring reverb tank and the valve circuit around it, physically modelled
    (docs/spring-reverb.md has the sources, and which values are published, inferred or set by ear).

      springs     SIMULATED: each spring's vibration as about 850 modes, from the thin helical spring
                  model (Bilbao & Parker; van Walstijn's modal form, fitted to an Accutronics 9EB2C1B):
                  the chirps, the drip and the decay come from the spring's own physics.
                  WAVEGUIDE: the parametric model of Valimaki, Parker & Abel (delay lines and chains of
                  dispersive allpasses), set from the same springs: lighter on the processor
      tanks       Accutronics 4AB3C1B (Fender's long 2-spring), 9EB2C1B (17" 3-spring), 8EB2C1B (9" 3-spring)
      circuit     a 12AT7 driver into the tank's drive coil through a transformer (DWELL: how hard it's
                  driven, up to clipping), the springs, the pickup coil, a 12AX7 recovery stage and TONE

    The block's MIX blends it with the dry sound; this is the wet signal alone, mono, as a tank is.
*/
namespace tf::spring
{
enum class Tank { fender = 0, threeLong, threeShort };
inline constexpr int numTanks = 3;
enum class Model { simulated = 0, waveguide };

/** A spring, in van Walstijn's dimensionless terms (DAFx 2020, his Table 1): the 9EB2C1B's spring. */
struct Physics
{
    double kappa = 0.02018;   // bending stiffness (1/s)
    double q = 1994.0;        // length over coil radius
    double gamma = 1200.0;    // longitudinal wave speed over length (1/s)
    double phi = 2.0e-8;      // Kelvin-Voigt (internal) loss (s)
    double sigma = 3.0;       // frequency-independent loss (1/s)
    double w = 0.004;         // the transducers' reach along the spring (fraction of its length)

    /** The same wire and coil, s times as long (the delay scales with s; the transition doesn't). */
    Physics lengthened (double s) const;
    /** The lower (audible) branch of the dispersion relation: angular frequency at wavenumber beta. */
    double lowBranch (double beta) const;
    /** The transition frequency (Hz): the top of the lower branch, where the chirps turn. */
    double transitionHz() const;
    /** The time for low frequencies to travel the spring and back (s): the echoes' spacing. */
    double roundTripSeconds() const;
    /** The time a low-frequency mode takes to fall 60 dB (s). */
    double t60 (double hz) const;
};

struct TankInfo
{
    const char* code;          // Accutronics part number
    const char* label;         // for the chips
    const char* description;   // for the window
    int paths;                 // springs from input to output (each a counter-wound pair joined in the middle)
    double lengths[3];         // each path's length, relative to the fitted 9EB2C1B spring
    double sigma;              // its decay class (1/s)
    const char* decayClass;
    double inZ1k, inRdc;       // the drive coil: impedance at 1 kHz and DC resistance (ohms)
    double inches;             // the tank's length
};
const TankInfo& tankInfo (Tank);
Physics tankPhysics (Tank, int path);

/** One spring as its modes: a two-pole resonator each, driven at one end and heard at the other. */
class ModalPath
{
public:
    void build (const Physics&, double sampleRate, double maxHz);
    void reset();
    float process (float force) noexcept;
    int modes() const noexcept { return count; }
    /** The energy of its response to a unit click (the modes summed as if unrelated). */
    double clickEnergy() const;

private:
    int count = 0, padded = 0;
    std::vector<float> a1, a2, gin, gout, y1, y2;
};

/** One spring as Valimaki, Parker & Abel's parametric model: low and high chirp loops. */
class WaveguidePath
{
public:
    void build (const Physics&, double sampleRate);
    void reset();
    float process (float x) noexcept;

private:
    // 100 stretched allpasses, (a + Afd z^-K1) / (1 + a Afd z^-K1): their delay lines side by side
    // (all K1 long, all moving on together), and their fractional allpasses' states
    std::vector<float> lines, fdState;
    int linePos = 0;
    static constexpr int lowSections = 100, highSections = 200;
    std::vector<float> highState;
    std::vector<float> lowLoop, highLoop;
    int lowPos = 0, highPos = 0, lowDelay = 1, highDelay = 1, tap = 1, highTap = 1;
    float aLow = 0.62f, aFd = 0.0f, aHigh = -0.6f, gLow = -0.8f, gHigh = -0.77f;
    int k1 = 4;
    float dcX = 0.0f, dcY = 0.0f, dcA = 0.995f;
    Biquad lowOut1, lowOut2, highOut1, highOut2;
    float highMix = 0.3f;
};

/** The valve circuit around the tank. */
struct Circuit
{
    void prepare (double sampleRate, const TankInfo&);
    void reset();
    float drive (float x, float dwell) noexcept;     // 12AT7 driver and transformer: what drives the tank
    float recover (float x) noexcept;                // 12AX7 recovery and TONE
    void setTone (float tone0to10, double sampleRate);

    double sr = 48000.0;
    float xfA = 0.0f, xfIn = 0.0f, xf = 0.0f;        // the transformer's low end
    float toneA = 0.0f, toneY = 0.0f;
    float dcIn = 0.0f, dcOut = 0.0f;
};

//==============================================================================
class Processor : public juce::AudioProcessor
{
public:
    Processor();
    ~Processor() override;

    Tank tank() const;
    Model model() const;
    juce::RangedAudioParameter* param (const juce::String& id) const;
    /** The wet response to a click, for the window's picture (or a test): mono, at sampleRate. */
    static std::vector<float> click (Tank, Model, double sampleRate, double seconds, float dwell = 5.0f);
    int modesInUse() const;

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 5.0; }
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

    /** The tanks as built for a rate (each tank's paths, both models). */
    struct Tanks
    {
        void build (double sampleRate);
        std::array<std::vector<ModalPath>, numTanks> modal;
        std::array<std::vector<WaveguidePath>, numTanks> waveguide;
        std::array<float, numTanks> modalGain {}, waveguideGain {};
    };

private:
    juce::AudioParameterChoice *tankP {}, *modelP {};
    juce::AudioParameterFloat *dwellP {}, *toneP {};
    double sr = 48000.0;
    std::unique_ptr<Tanks> tanks;
    Circuit circuit;
    int lastTank = -1, lastModel = -1;
    float lastTone = -1.0f;
};
} // namespace tf::spring
