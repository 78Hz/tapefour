#pragma once

#include "TapeEngine.h"
#include "Blocks.h"
#include "MidiHub.h"
#include "MidiMapper.h"
#include "ChannelStrip.h"
#include "Deck.h"
#include "MasterDeck.h"

namespace tf
{
/**
    The deck's electronics: the audio device (whatever the platform offers: ASIO,
    WASAPI or DirectSound on Windows, CoreAudio on macOS, ALSA or JACK on Linux),
    each track's inputs and signal chain, four channel strips feeding the tape and
    the stereo mix, and MIDI learn. No undo, on purpose.

    A track's chain is one row of blocks with two streams running through it: MIDI
    from the track's controller (changed by MIDI effects, played by instruments) and
    audio from its input (added to by instruments, processed by effects). What comes
    out of the end is what goes on tape.

    Split across files: this one runs the audio and owns the parts; MidiHub.cpp handles
    MIDI devices; MidiMapper.cpp is MIDI learn; ChannelStrip.cpp is the strip DSP;
    Blocks.cpp the things in a chain; DeckState.cpp everything saved and recalled.
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::ChangeListener,
                    private MidiMapper::Sink
{
public:
    static constexpr int maxMidiPorts = MidiHub::maxPorts;
    static constexpr int midiAny = -1;          // track listens to every active controller
    static constexpr int midiNone = -2;
    static constexpr int computerKeyboardPort = MidiHub::computerKeyboard;
    static constexpr int maxBlocks = 8;
    static constexpr int maxInputs = 16;

    // The swept mid: 250 Hz to 5 kHz on a log scale; the default puts it at 1.2 kHz.
    static double midFreqHz (float v) noexcept { return 250.0 * std::pow (20.0, juce::jlimit (0.0, 1.0, (double) v)); }
    static constexpr float defaultMidFreq = 0.5236f;
    static float faderGain (float f) noexcept { return f <= 0.0f ? 0.0f : std::pow (f / 0.75f, 3.0f); }

    /** How a track is fed: live (its controller and input, through the chain) or a bounce of other tracks from tape. */
    enum class Feed : int { live = 0, bounce = 1 };

    /** How far back, in samples, a track's recording belongs: see TapeEngine::writeTrack.
        Anything that came in through the interface (an input, a hardware synth) was played along to
        what was heard: the round trip, plus the user's offset for drivers that misreport. Instruments
        only wait for the output. A bounce comes straight off the tape. The chain's own delay adds on. */
    static double recordLatencySamples (bool throughInterface, bool hasInstruments, bool bounce,
                                        int inputLatency, int outputLatency, double userOffsetSamples, int chainLatency) noexcept;

    struct TrackParams
    {
        std::atomic<int>   feed { (int) Feed::live };
        std::atomic<int>   inputChannel { -1 };         // audio in: -1 = none
        std::atomic<bool>  inputPair { false };         // mono sum of inputChannel and the next one
        std::atomic<int>   midiPort { midiAny };        // which controller plays this track
        std::atomic<bool>  armed { false }, mute { false }, solo { false };
        std::atomic<float> gain { 1.0f / 3.0f };        // input trim: 0..1 -> -20..+40 dB
        std::atomic<float> high { 0 }, mid { 0 }, low { 0 }, pan { 0 }; // -1..1
        std::atomic<float> midFreq { defaultMidFreq };  // 0..1 -> 250 Hz..5 kHz
        std::atomic<float> fader { 0.75f };
        std::atomic<int>   bounceMask { 0 };
        std::atomic<float> meter { 0 };
        std::atomic<bool>  midiActivity { false };
    };

    /** A block as the front panel sees it. */
    struct BlockInfo
    {
        int id = 0;
        juce::String name, maker;
        BlockRole role = BlockRole::audioFx;
        bool bypass = false, hasEditor = false, canProcessInput = false, processInput = false, producesMidi = false;
        bool missing = false, hosted = false;
        float levelDb = 0.0f, mix = 1.0f;
        int builtIn = -1;           // which built-in synth or effect, if it is one
        bool builtInSynth = false;
        // external synths
        int outPort = -1, channel = 1, returnInput = -1;
        bool returnPair = false;
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
    MidiMapper mapper { midi };

    TrackParams track[numTracks];
    std::atomic<float> masterFader { 0.75f }, phones { 0.6f }, meterL { 0 }, meterR { 0 };
    std::atomic<int>   mainOutPair { 0 }, phonesPair { -1 };
    std::atomic<bool>& midiFlash = midi.inFlash;
    std::atomic<bool>& midiOutFlash = midi.outFlash;
    std::atomic<bool>  recallDeckWithTape { false };
    std::atomic<float> inputPeak[maxInputs] {};          // per interface input, for the signal bars
    /** Goes up whenever anything a track's display shows changes (its chain, inputs, routing, devices),
        so the displays rebuild their text only then. */
    std::atomic<juce::uint32> viewVersion { 1 };
    /** A transport command from a controller (MMC or a mapped button), for the deck to carry out. */
    std::atomic<int>   remoteCommand { 0 };

    // ---- the master deck: the stereo cassette the mix goes to (MasterDeck.h) ----
    MasterDeck masterDeck;
    juce::File getMasterMachineFile() const;
    /** Peak of the master bus since the last reset, per side: for the master strip's readout. */
    std::atomic<float> peakHoldL { 0 }, peakHoldR { 0 };

    // ---- tape (message thread) ----
    Tape* getTape() const noexcept { return tape.get(); }
    std::unique_ptr<Tape> swapTape (std::unique_ptr<Tape> newTape);
    /** Turns the cassette in the deck over (side B up) or back. Only with the transport stopped:
        false otherwise, or with no tape in. The counter keeps the number it showed, as on a real deck. */
    bool turnTapeOver (bool sideBUp);

    // ---- a track's inputs (message thread) ----
    void setTrackInput (int t, int inputChannel, bool pair = false);   // -1: no audio in
    void setTrackMidiPort (int t, int port);
    void setTrackFeed (int t, Feed feed);
    void toggleBounceSource (int t, int sourceTrack);

    // ---- a track's chain (message thread) ----
    std::vector<BlockInfo> getChain (int t) const;
    Block* findBlock (int t, int blockId) const;
    int numBlocks (int t) const;
    /** Puts a block in the chain at index (-1: the end). Returns its id, or 0 if the chain is full. */
    int insertBlock (int t, int index, std::unique_ptr<Block> block);
    /** Replaces a block, keeping its place. Returns the new block's id (the old one's, with keepId:
        a plug-in arriving in its placeholder stays the same block to everything that knows it). */
    int replaceBlock (int t, int blockId, std::unique_ptr<Block> block, bool keepId = false);
    void removeBlock (int t, int blockId);
    void moveBlock (int t, int blockId, int newIndex);
    void clearChain (int t);
    void setBlockBypass (int t, int blockId, bool bypassed);
    /** Loads a plug-in into the chain at index (or in place of replaceId). A placeholder holds its place
        while it loads, so the order is kept and anything done to the track meanwhile is respected. */
    int addPlugin (int t, int index, const juce::PluginDescription& desc, const juce::String& base64State,
                   int replaceId = 0, std::function<void (const juce::String& error)> done = {},
                   std::unique_ptr<juce::XmlElement> savedBlock = {});
    int addExternal (int t, int index, int outPort);
    void sendAllNotesOff (int t, int blockId);
    /** Called just before a block is removed or replaced, so its window can close first. */
    std::function<void (int track, int blockId)> onBlockRemoving;

    juce::String describeSource (int t) const;    // what the display's top line says
    juce::String describeInput (int t) const;     // "IN 3" or "IN 3+4"
    /** Short names for the blocks, for the row on the display. */
    juce::StringArray blockAbbreviations (int t) const;

    // ---- mute and solo ----
    bool anySolo() const noexcept;
    /** True when every soloed track is also muted: the deck is silent and that's why. */
    bool soloedTracksAllMuted() const noexcept;

    // ---- MIDI controllers and hardware synths (message thread) ----
    std::vector<MidiInputInfo> getMidiInputs();
    void setMidiInputActive (int slot, bool active);
    void setMidiInputMpe (int slot, bool mpe, int bendRange);
    juce::String midiPortLabel (int port) const;
    bool isMidiPortMpe (int port) const;
    std::vector<MidiOutputInfo> getMidiOutputs();
    void setMidiOutputActive (int slot, bool active);
    void setMidiOutputAlias (int slot, const juce::String& alias);
    juce::String midiOutputLabel (int slot) const { return midi.outputLabel (slot); }

    // ---- device info ----
    juce::String getDeviceSummary() const;
    juce::StringArray getActiveInputNames() const;
    juce::StringArray getOutputPairNames() const;

    /** The computer keyboard is MIDI controller slot 0. */
    void injectMidi (const juce::MidiMessage& m, int port = computerKeyboardPort);

    // ---- test hooks: not used by the app ----
    /** MIDI ports with no device behind them, for driving the engine without hardware. */
    int addVirtualMidiInput (const juce::String& name)  { return midi.addVirtualInput (name); }
    int addVirtualMidiOutput (const juce::String& name) { return midi.addVirtualOutput (name); }
    /** Sees every block of outgoing MIDI, on the audio thread. Set it only while the audio callback
        isn't running (the tests drive the callback themselves). */
    void setMidiOutTapForTests (std::function<void (int slot, const juce::MidiBuffer&)> tap) { midi.outTap = std::move (tap); }

    juce::String getStartupProblem() const { return startupProblem; }

    // ---- deck state: everything you'd set by hand (saved with settings, and with every tape) ----
    std::unique_ptr<juce::XmlElement> createDeckState (bool freshPluginState = false);
    void applyDeckState (const juce::XmlElement& deck);
    std::function<void()> onDeckStateApplied;

    // ---- the machine itself: TF-4.deck beside the settings (docs/formats/deck-v1.md) ----
    /** The machine as it is now (the Setup sliders write straight to tapeEngine.machine). */
    tfmt::DeckDescription getMachine() const;
    /** Applies a whole machine description; it is saved with the settings. */
    void setMachine (const tfmt::DeckDescription&);
    /** The TF-4 as it leaves the factory, serviced. */
    static tfmt::DeckDescription factoryMachine();
    juce::File getMachineFile() const;
    /** A name for the machine's state after the sliders moved: one of the presets, or "custom". */
    void setMachineCondition (const juce::String& name) { machineBase.condition = name.toStdString(); }

    // ---- settings ----
    void saveSettings (bool freshPluginState = false);

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct TrackChain
    {
        juce::SpinLock lock;                          // the audio thread only ever tries it
        std::vector<std::unique_ptr<Block>> blocks;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void openStartupDevice();
    void saveDeviceState();
    void restoreAll();
    std::unique_ptr<juce::XmlElement> createHardwareState() const;
    void restoreHardware (const juce::XmlElement* hw, const juce::XmlElement* legacyDeck);
    void restoreMachine (const juce::XmlElement* legacyDeck);
    void saveMachine();
    void restoreTrackChain (int t, const juce::XmlElement& chainXml);
    void migrateOldTrack (int t, const juce::XmlElement& x);
    bool isMidiSlotInUse (int slot, bool output) const;
    void allocateBuffers (int size);
    void prepareBlock (Block&);
    int insertBlockWithId (int t, int index, std::unique_ptr<Block> block, int id);
    std::unique_ptr<Block> takeBlock (int t, int blockId, int* index = nullptr);
    void destroyBlock (int t, std::unique_ptr<Block> block);

    // audio thread
    struct ChainResult { bool throughInterface = false, hasInstruments = false; int latency = 0; };
    ChainResult runChain (int t, const float* const* inputs, int numInputs, int n, bool armed, float* monoOut,
                          const float* bounceIn);
    void copyInput (int channel, bool pair, const float* const* inputs, int numInputs, int n, float* dest, float gain = 1.0f) const;

    // MidiMapper::Sink
    float getValue (const MidiMapper::Target&, juce::AudioProcessorParameter*) override;
    void setValue (const MidiMapper::Target&, juce::AudioProcessorParameter*, float) override;
    void toggle (const MidiMapper::Target&) override;
    void command (MidiMapper::Control) override;

    juce::PropertiesFile& props;
    tfmt::DeckDescription machineBase;   // what the atomics don't hold: identity, capabilities, condition name
    std::string lastSavedMachine;

    juce::SpinLock tapeLock;
    std::unique_ptr<Tape> tape;
    TrackChain chains[numTracks];
    ChannelStrip strips[numTracks];
    int nextBlockId = 1;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    double sampleRate = 0.0;
    int bufferSize = 0, maxBlock = 8192, inputLatency = 0, outputLatency = 0;

    std::vector<float> tapeOut[numTracks], src[numTracks], chan[numTracks], mixL, mixR, bounceSum, masterL, masterR;
    float* tapeOutPtr[numTracks] {};
    juce::AudioBuffer<float> acc, scratch, dry;
    juce::MidiBuffer trackMidi, midiWork;
    bool wasReceiving[numTracks] {};
    bool lastMpe[numTracks] {};
    int lastBend[numTracks] {};
    ChainResult chainResult[numTracks];
    float lastMaster = 0, lastPhones = 0;
    std::atomic<bool> mpeConfigPending[numTracks] {};

    bool restoringDevice = false;
    bool startingUp = true;
    std::unique_ptr<juce::XmlElement> stateAtStartup;
    juce::String startupProblem;
};
} // namespace tf
