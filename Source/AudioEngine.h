#pragma once

#include "TapeEngine.h"
#include "Instruments.h"

namespace tf
{
/**
    The deck's electronics: audio/MIDI I/O (ASIO on Windows), four channel
    strips feeding the tape and the stereo mix, the instruments plugged into
    the tracks, and hardware synths driven over MIDI. No undo, on purpose.
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    static constexpr int maxMidiPorts = 16;
    static constexpr int midiAnyWhenArmed = -1;   // track listens to every controller, while armed
    static constexpr int midiNone = -2;
    static constexpr int computerKeyboardPort = 0;

    struct TrackParams
    {
        std::atomic<int>   source { (int) Source::none };
        std::atomic<int>   inputChannel { 0 };
        std::atomic<bool>  inputPair { false };          // mono sum of inputChannel and the next one
        std::atomic<int>   midiPort { midiAnyWhenArmed }; // which controller plays this track
        std::atomic<int>   midiOutPort { -1 };           // hardware synth: which MIDI output
        std::atomic<int>   midiOutChannel { 1 };         // 1..16, or 0 = as played
        std::atomic<bool>  armed { false };
        std::atomic<float> gain { 1.0f / 3.0f };       // input trim: 0..1 -> -20..+40 dB
        std::atomic<float> high { 0 }, mid { 0 }, low { 0 }, pan { 0 }; // -1..1
        std::atomic<float> fader { 0.75f };
        std::atomic<int>   bounceMask { 0 };
        std::atomic<float> meter { 0 };
        std::atomic<bool>  midiActivity { false };
    };

    struct MidiInputInfo  { int slot; juce::String id, name; bool available, active, mpe; int bendRange; };
    struct MidiOutputInfo { int slot; juce::String id, name, alias; bool available, active; };

    explicit AudioEngine (juce::PropertiesFile& settings);
    ~AudioEngine() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    TapeEngine tapeEngine;

    TrackParams track[numTracks];
    std::atomic<float> masterFader { 0.75f }, phones { 0.6f }, meterL { 0 }, meterR { 0 };
    std::atomic<int>   mainOutPair { 0 }, phonesPair { -1 }, monitorMode { 0 };   // monitor: 0 auto (tape-style), 1 always input
    std::atomic<bool>  midiFlash { false }, midiOutFlash { false };
    std::atomic<bool>  recallDeckWithTape { false };

    // ---- tape (message thread) ----
    Tape* getTape() const noexcept { return tape.get(); }
    std::unique_ptr<Tape> swapTape (std::unique_ptr<Tape> newTape);

    // ---- sources (message thread) ----
    void setAudioSource (int t, int inputChannel, bool pair = false);
    void setHardwareSource (int t);
    void setInstrument (int t, std::unique_ptr<Instrument> inst);
    void loadPlugin (int t, const juce::PluginDescription& desc, const juce::String& base64State,
                     std::function<void (const juce::String& error)> done);
    void toggleBounceSource (int t, int sourceTrack);
    void unplug (int t);
    void setTrackMidiPort (int t, int port);
    Instrument* getInstrument (int t) const noexcept { return slots[t].inst.get(); }
    juce::String describeSource (int t) const;
    juce::String describeInput (int t) const;    // "IN 3" or "IN 3+4"

    // ---- MIDI controllers and hardware synths (message thread) ----
    std::vector<MidiInputInfo> getMidiInputs() const;
    void setMidiInputActive (int slot, bool active);
    void setMidiInputMpe (int slot, bool mpe, int bendRange);
    juce::String midiPortLabel (int port) const;             // for displays
    bool isMidiPortMpe (int port) const;
    std::vector<MidiOutputInfo> getMidiOutputs() const;
    void setMidiOutputActive (int slot, bool active);
    void setMidiOutputAlias (int slot, const juce::String& alias);
    juce::String midiOutputLabel (int slot) const;            // alias, else port name

    // ---- device info ----
    double getSampleRate() const noexcept { return sampleRate; }
    int getBufferSize() const noexcept { return bufferSize; }
    juce::String getDeviceSummary() const;
    juce::StringArray getActiveInputNames() const;
    juce::StringArray getOutputPairNames() const;

    /** The computer keyboard is MIDI controller slot 0. */
    void injectMidi (const juce::MidiMessage& m, int port = computerKeyboardPort);

    // For tests: ports with no device behind them.
    int addVirtualMidiInput (const juce::String& name);
    int addVirtualMidiOutput (const juce::String& name);
    std::function<void (int slot, const juce::MidiBuffer&)> midiOutTap;   // audio thread

    /** Set when the saved audio device couldn't be opened at start-up (the saved choice is kept). */
    juce::String getStartupProblem() const { return startupProblem; }

    // ---- deck state: everything you'd set by hand (saved with settings, and with every tape) ----
    std::unique_ptr<juce::XmlElement> createDeckState() const;
    void applyDeckState (const juce::XmlElement& deck);
    std::function<void()> onDeckStateApplied;   // so the front panel can catch up

    // ---- settings ----
    void saveSettings();

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct MidiPort
    {
        juce::String id, name;                 // message thread
        std::atomic<bool> active { false };
        std::atomic<bool> mpe { false };
        std::atomic<int> bendRange { 48 };
        juce::MidiMessageCollector collector;
        juce::MidiBuffer buffer;               // audio thread
        bool registered = false;
    };

    struct MidiOutPort
    {
        juce::String id, name, alias;
        std::atomic<bool> active { false };
        juce::SpinLock lock;
        std::unique_ptr<juce::MidiOutput> out;
        juce::MidiBuffer buffer;               // audio thread
    };

    struct Slot
    {
        juce::SpinLock lock;
        std::unique_ptr<Instrument> inst;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void saveDeviceState();
    void restoreAll();
    void restoreHardware (const juce::XmlElement* hw, const juce::XmlElement* legacyDeck);
    std::unique_ptr<juce::XmlElement> createHardwareState() const;
    int findOrCreateInputSlot (const juce::String& id, const juce::String& name);
    int findOrCreateOutputSlot (const juce::String& id, const juce::String& name);
    void refreshMidiDevices();
    static bool looksLikeMpeController (const juce::String& name);
    static void addMpeConfiguration (juce::MidiBuffer& b, bool on, int bendRange);
    void updateEq (int t);
    static float faderGain (float f) noexcept { return f <= 0.0f ? 0.0f : std::pow (f / 0.75f, 3.0f); }

    juce::PropertiesFile& props;

    juce::SpinLock tapeLock;
    std::unique_ptr<Tape> tape;
    Slot slots[numTracks];
    std::unique_ptr<MidiPort> midiPorts[maxMidiPorts];
    std::unique_ptr<MidiOutPort> midiOuts[maxMidiPorts];

    double sampleRate = 0.0;
    int bufferSize = 0, maxBlock = 8192, deviceLatency = 0;

    Biquad eq[numTracks][3];
    float eqState[numTracks][3] { { 99, 99, 99 }, { 99, 99, 99 }, { 99, 99, 99 }, { 99, 99, 99 } };
    float lastGainL[numTracks] {}, lastGainR[numTracks] {}, lastTrim[numTracks] {}, lastMaster = 0, lastPhones = 0;

    std::vector<float> tapeOut[numTracks], src[numTracks], chan[numTracks], mixL, mixR;
    float* tapeOutPtr[numTracks] {};
    juce::AudioBuffer<float> instBuffer;
    juce::MidiBuffer anyMidi, trackMidi;
    bool wasReceiving[numTracks] {};
    bool lastMpe[numTracks] {};
    int lastBend[numTracks] {};
    int lastOutRoute[numTracks] { -1, -1, -1, -1 };
    std::atomic<bool> mpeConfigPending[numTracks] {};
    // A plug-in still loading (or missing): its saved settings are kept so they aren't lost.
    std::unique_ptr<juce::XmlElement> pendingInstrument[numTracks];
    bool pluginMissing[numTracks] {};

    bool restoringDevice = false;              // saved device failed to open: don't overwrite the saved choice
    bool startingUp = true;
    std::unique_ptr<juce::XmlElement> stateAtStartup;
    juce::String startupProblem;
};
} // namespace tf
