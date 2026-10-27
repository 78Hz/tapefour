#pragma once

#include "../Source/TapeEngine.h"

namespace tf::pocket
{
/**
    The pocket player's mechanism: one stereo playback head, 1 7/8 ips, off or B-type noise
    reduction, and a portable's transport. It plays a cassette opened where it lies
    (Tape::openPacked) and never changes it.

    Threads: everything here on the message thread, except the audio callback.
*/
class PlayerEngine : public juce::AudioIODeviceCallback
{
public:
    PlayerEngine();
    ~PlayerEngine() override;

    juce::AudioDeviceManager deviceManager;
    /** Opens the default output. Returns what went wrong, if anything. */
    juce::String start();

    /** Puts a cassette in (or none), stopped, at its side and position. The old one comes back. */
    std::unique_ptr<Tape> load (std::unique_ptr<Tape> tape, int side, double position);
    const Tape* getTape() const noexcept { return tape.get(); }

    enum class Key { play, stop, ff, rew };
    void press (Key);
    Mode getMode() const noexcept { return engine.getMode(); }

    /** Turns the cassette over (stopping it first); the same place on the tape, seen from the other side. */
    void turnOver();
    int getSide() const noexcept { return side; }
    /** Where the tape is on the side facing up, in cells from its start. */
    double getPosition() const noexcept;
    /** How much of the side has played, 0..1. */
    double getFraction() const noexcept;

    void setNoiseReduction (bool on) noexcept { engine.params.nrOn = on; }
    bool getNoiseReduction() const noexcept   { return engine.params.nrOn.load(); }
    /** PLAY pressed while winding: cue (forward) or review. */
    bool isCueing() const noexcept            { return engine.isCueing(); }
    /** For the tests: no amplifier noise. */
    void setElectronicsNoiseForTests (bool on) noexcept { electronicsNoise = on ? tapeRefPeak / 1.41421356f * 5.6234e-5f : 0.0f; }
    /** The noise reduction the cassette was recorded with, which the player decodes with:
        0 the 2:1 compander, 1 B, 2 C (B if the cassette doesn't say). */
    int getNoiseReductionType() const noexcept { return engine.params.nrType.load(); }
    /** The system most of a tape's recordings were encoded with, or -1 if none was. */
    static int encodedWith (const Tape&);
    /** 0..10, the volume wheel's numbers. */
    void setVolume (int v) noexcept;
    int getVolume() const noexcept            { return volume.load(); }

    /** The portable's own transport and heads (pocket.deck). */
    static void setPocketMachine (TapeEngine::Machine&);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    TapeEngine engine;
    std::unique_ptr<Tape> tape;
    juce::SpinLock lock;
    int side = 0;
    std::atomic<double> position { 0.0 };   // the transport's place, as of the last audio block (or a load / turn)
    std::atomic<int> volume { 7 };
    std::atomic<float> gain { 0.5f };
    juce::SmoothedValue<float> smoothedGain;
    std::vector<float> outs[numTracks];
    float* outPtrs[numTracks] {};
    int maxBlock = 0;
    double sampleRate = 48000.0;
    // The playback amplifier's own noise: -85 dB re 0 VU (an estimate for a portable's preamp).
    std::atomic<float> electronicsNoise { tapeRefPeak / 1.41421356f * 5.6234e-5f };
    Noise noiseL { 0x85ebca6bu }, noiseR { 0xc2b2ae35u };

    JUCE_DECLARE_NON_COPYABLE (PlayerEngine)
};
} // namespace tf::pocket
