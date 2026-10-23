#pragma once

#include "TapeEngine.h"
#include "Instruments.h"
#include "MidiHub.h"
#include "ChannelStrip.h"

namespace tf
{
/**
    The deck's electronics: the audio device (whatever the platform offers: ASIO,
    WASAPI or DirectSound on Windows, CoreAudio on macOS, ALSA or JACK on Linux),
    four channel strips feeding the tape and the stereo mix, the instruments
    plugged into the tracks, and hardware synths driven over MIDI. No undo, on purpose.

    Split across files: this one runs the audio and owns the parts; MidiHub.cpp
    handles MIDI devices; ChannelStrip.cpp is the strip DSP; DeckState.cpp is
    everything saved and recalled (settings, and the deck that goes with each tape).
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::ChangeListener
{
public:
    static constexpr int maxMidiPorts = MidiHub::maxPorts;
    static constexpr int midiAny = -1;          // track listens to every active controller
    static constexpr int midiNone = -2;
    static constexpr int computerKeyboardPort = MidiHub::computerKeyboard;

    // The swept mid: 250 Hz to 5 kHz on a log scale; the default puts it at 1.2 kHz.
    static double midFreqHz (float v) noexcept { return 250.0 * std::pow (20.0, juce::jlimit (0.0, 1.0, (double) v)); }
    static constexpr float defaultMidFreq = 0.5236f;
    static float faderGain (float f) noexcept { return f <= 0.0f ? 0.0f : std::pow (f / 0.75f, 3.0f); }

    /** How far back, in samples, a track's recording belongs: see TapeEngine::writeTrack. The user's
        overdub-timing offset corrects drivers that misreport, so it goes with the interface's sources. */
    static double recordLatencySamples (Source s, int inputLatency, int outputLatency, double userOffsetSamples) noexcept;

    struct TrackParams
    {
        std::atomic<int>   source { (int) Source::none };
        std::atomic<int>   inputChannel { 0 };
        std::atomic<bool>  inputPair { false };          // mono sum of inputChannel and the next one
        std::atomic<int>   midiPort { midiAny };         // which controller plays this track
        std::atomic<int>   midiOutPort { -1 };           // hardware synth: which MIDI output
        std::atomic<int>   midiOutChannel { 1 };         // 1..16, or 0 = as played
        std::atomic<bool>  armed { false };
        std::atomic<float> gain { 1.0f / 3.0f };       // input trim: 0..1 -> -20..+40 dB
        std::atomic<float> high { 0 }, mid { 0 }, low { 0 }, pan { 0 }; // -1..1
        std::atomic<float> midFreq { defaultMidFreq };   // 0..1 -> 250 Hz..5 kHz
        std::atomic<float> fader { 0.75f };
        std::atomic<int>   bounceMask { 0 };
        std::atomic<float> meter { 0 };
        std::atomic<bool>  midiActivity { false };
    };

    using MidiInputInfo = MidiHub::InputInfo;
    using MidiOutputInfo = MidiHub::OutputInfo;

    explicit AudioEngine (juce::PropertiesFile& settings);
    ~AudioEngine() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    TapeEngine tapeEngine;
    MidiHub midi { deviceManager };

    TrackParams track[numTracks];
    std::atomic<float> masterFader { 0.75f }, phones { 0.6f }, meterL { 0 }, meterR { 0 };
    std::atomic<int>   mainOutPair { 0 }, phonesPair { -1 };
    std::atomic<bool>& midiFlash = midi.inFlash;
    std::atomic<bool>& midiOutFlash = midi.outFlash;
    std::atomic<bool>  recallDeckWithTape { false };

    // ---- tape (message thread) ----
    Tape* getTape() const noexcept { return tape.get(); }
    std::unique_ptr<Tape> swapTape (std::unique_ptr<Tape> newTape);

    // ---- sources (message thread) ----
    void setAudioSource (int t, int inputChannel, bool pair = false);
    void setHardwareSource (int t);
    void setInstrument (int t, std::unique_ptr<Instrument> inst);
    /** Loads in the background. If anything else is plugged into the track before it arrives,
        the late arrival is discarded. */
    void loadPlugin (int t, const juce::PluginDescription& desc, const juce::String& base64State,
                     std::function<void (const juce::String& error)> done);
    void toggleBounceSource (int t, int sourceTrack);
    void unplug (int t);
    void setTrackMidiPort (int t, int port);
    Instrument* getInstrument (int t) const noexcept { return slots[t].inst.get(); }
    juce::String describeSource (int t) const;
    juce::String describeInput (int t) const;    // "IN 3" or "IN 3+4"
    /** Called just before a track's instrument is replaced or removed, so its window can close first. */
    std::function<void (int track)> onInstrumentChanging;

    // ---- MIDI controllers and hardware synths (message thread) ----
    std::vector<MidiInputInfo> getMidiInputs();
    void setMidiInputActive (int slot, bool active);
    void setMidiInputMpe (int slot, bool mpe, int bendRange);
    juce::String midiPortLabel (int port) const;             // for displays
    bool isMidiPortMpe (int port) const;
    std::vector<MidiOutputInfo> getMidiOutputs();
    void setMidiOutputActive (int slot, bool active);
    void setMidiOutputAlias (int slot, const juce::String& alias);
    juce::String midiOutputLabel (int slot) const { return midi.outputLabel (slot); }

    // ---- device info ----
    double getSampleRate() const noexcept { return sampleRate; }
    int getBufferSize() const noexcept { return bufferSize; }
    juce::String getDeviceSummary() const;
    juce::StringArray getActiveInputNames() const;
    juce::StringArray getOutputPairNames() const;

    /** The computer keyboard is MIDI controller slot 0. */
    void injectMidi (const juce::MidiMessage& m, int port = computerKeyboardPort);

    // For tests: ports with no device behind them.
    int addVirtualMidiInput (const juce::String& name)  { return midi.addVirtualInput (name); }
    int addVirtualMidiOutput (const juce::String& name) { return midi.addVirtualOutput (name); }
    std::function<void (int slot, const juce::MidiBuffer&)>& midiOutTap = midi.outTap;   // audio thread

    /** Set when the saved audio device couldn't be opened at start-up (the saved choice is kept). */
    juce::String getStartupProblem() const { return startupProblem; }

    // ---- deck state: everything you'd set by hand (saved with settings, and with every tape) ----
    /** freshPluginState: ask plug-ins for their settings now, rather than reuse a recent copy. */
    std::unique_ptr<juce::XmlElement> createDeckState (bool freshPluginState = false);
    void applyDeckState (const juce::XmlElement& deck);
    std::function<void()> onDeckStateApplied;   // so the front panel can catch up

    // ---- settings ----
    /** Writes the settings file if anything in it changed. Cheap enough to call every few seconds. */
    void saveSettings (bool freshPluginState = false);

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct Slot
    {
        juce::SpinLock lock;
        std::unique_ptr<Instrument> inst;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void openStartupDevice();
    void saveDeviceState();
    void restoreAll();
    std::unique_ptr<juce::XmlElement> createHardwareState() const;
    void restoreHardware (const juce::XmlElement* hw, const juce::XmlElement* legacyDeck);
    bool isMidiSlotInUse (int slot, bool output) const;
    void allocateBuffers (int size);

    juce::PropertiesFile& props;

    juce::SpinLock tapeLock;
    std::unique_ptr<Tape> tape;
    Slot slots[numTracks];
    ChannelStrip strips[numTracks];
    int loadGeneration[numTracks] {};
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    double sampleRate = 0.0;
    int bufferSize = 0, maxBlock = 8192, inputLatency = 0, outputLatency = 0;

    std::vector<float> tapeOut[numTracks], src[numTracks], chan[numTracks], mixL, mixR;
    float* tapeOutPtr[numTracks] {};
    juce::AudioBuffer<float> instBuffer;
    juce::MidiBuffer trackMidi;
    bool wasReceiving[numTracks] {};
    bool lastMpe[numTracks] {};
    int lastBend[numTracks] {};
    struct OutRoute { int port = -1, channel = 0; bool operator!= (const OutRoute& o) const { return port != o.port || channel != o.channel; } };
    OutRoute lastOutRoute[numTracks];
    float lastMaster = 0, lastPhones = 0;
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
