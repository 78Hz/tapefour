#pragma once

#include "TapeEngine.h"
#include "Instruments.h"

namespace tf
{
/**
    The deck's electronics: audio/MIDI I/O (ASIO on Windows), four channel
    strips feeding the tape and the stereo mix, and the instruments plugged
    into the tracks. There is no undo anywhere in here, on purpose.
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    struct TrackParams
    {
        std::atomic<int>   source { (int) Source::none };
        std::atomic<int>   inputChannel { 0 };
        std::atomic<bool>  armed { false };
        std::atomic<float> gain { 1.0f / 3.0f };       // input trim: 0..1 -> -20..+40 dB
        std::atomic<float> high { 0 }, mid { 0 }, low { 0 }, pan { 0 }; // -1..1
        std::atomic<float> fader { 0.75f };
        std::atomic<int>   bounceMask { 0 };
        std::atomic<float> meter { 0 };
        std::atomic<bool>  midiActivity { false };
    };

    explicit AudioEngine (juce::PropertiesFile& settings);
    ~AudioEngine() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    TapeEngine tapeEngine;

    TrackParams track[numTracks];
    std::atomic<float> masterFader { 0.75f }, phones { 0.6f }, meterL { 0 }, meterR { 0 };
    std::atomic<int>   mainOutPair { 0 }, phonesPair { -1 }, monitorMode { 0 };   // monitor: 0 auto (tape-style), 1 always input
    std::atomic<bool>  followArm { true }, midiFlash { false };
    std::atomic<int>   focusedTrack { 0 };
    std::atomic<bool>  mpeEnabled { true };       // MPE controllers such as the Osmose
    std::atomic<int>   mpeBendRange { 48 };       // per-note pitch-bend range, semitones

    // ---- tape (message thread) ----
    Tape* getTape() const noexcept { return tape.get(); }
    std::unique_ptr<Tape> swapTape (std::unique_ptr<Tape> newTape);

    // ---- sources (message thread) ----
    void setAudioSource (int t, int inputChannel);
    void setInstrument (int t, std::unique_ptr<Instrument> inst);
    void loadPlugin (int t, const juce::PluginDescription& desc, const juce::String& base64State,
                     std::function<void (const juce::String& error)> done);
    void toggleBounceSource (int t, int sourceTrack);
    Instrument* getInstrument (int t) const noexcept { return slots[t].inst.get(); }
    juce::String describeSource (int t) const;

    // ---- device info ----
    double getSampleRate() const noexcept { return sampleRate; }
    int getBufferSize() const noexcept { return bufferSize; }
    juce::String getDeviceSummary() const;
    juce::StringArray getActiveInputNames() const;
    juce::StringArray getOutputPairNames() const;

    void injectMidi (const juce::MidiMessage& m);

    // ---- MIDI inputs and MPE (message thread) ----
    juce::Array<juce::MidiDeviceInfo> getMidiInputs() const { return juce::MidiInput::getAvailableDevices(); }
    bool isMidiInputEnabled (const juce::String& id) const { return deviceManager.isMidiInputDeviceEnabled (id); }
    void setMidiInputEnabled (const juce::String& id, bool enabled);
    void setMpe (bool enabled, int bendRange);

    /** Set when the saved audio device couldn't be opened at start-up (the saved choice is kept). */
    juce::String getStartupProblem() const { return startupProblem; }

    // ---- settings ----
    void saveSettings();
    void restoreSettings();

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void saveDeviceState();
    static void addMpeConfiguration (juce::MidiBuffer& b, bool on, int bendRange);

    struct Slot
    {
        juce::SpinLock lock;
        std::unique_ptr<Instrument> inst;
    };

    void updateEq (int t);
    static float faderGain (float f) noexcept { return f <= 0.0f ? 0.0f : std::pow (f / 0.75f, 3.0f); }

    juce::PropertiesFile& props;
    juce::MidiMessageCollector midiCollector;

    juce::SpinLock tapeLock;
    std::unique_ptr<Tape> tape;
    Slot slots[numTracks];

    double sampleRate = 0.0;
    int bufferSize = 0, maxBlock = 8192, deviceLatency = 0;

    Biquad eq[numTracks][3];
    float eqState[numTracks][3] { { 99, 99, 99 }, { 99, 99, 99 }, { 99, 99, 99 }, { 99, 99, 99 } };
    float lastGainL[numTracks] {}, lastGainR[numTracks] {}, lastTrim[numTracks] {}, lastMaster = 0, lastPhones = 0;

    std::vector<float> tapeOut[numTracks], src[numTracks], chan[numTracks], mixL, mixR;
    float* tapeOutPtr[numTracks] {};
    juce::AudioBuffer<float> instBuffer;
    juce::MidiBuffer midiIn, trackMidi;
    bool wasReceiving[numTracks] {};
    std::atomic<bool> mpeConfigPending[numTracks] {};
    // A plug-in still loading (or missing): its saved settings are kept so they aren't lost.
    std::unique_ptr<juce::XmlElement> pendingInstrument[numTracks];
    bool pluginMissing[numTracks] {};

    bool restoringDevice = false;              // saved device failed to open: don't overwrite the saved choice
    std::unique_ptr<juce::XmlElement> stateAtStartup;
    juce::String startupProblem;
};
} // namespace tf
