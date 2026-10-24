#pragma once

#include "Common.h"

namespace tf
{
/**
    What a block does with the two streams that run through a track's chain:
    MIDI (from the track's controller) and audio (from its input, or silence).

      midiFx      changes the MIDI everything after it hears (a retuner, an arpeggiator)
      instrument  adds its sound to the audio (or, switched to process, plays what reaches it)
      audioFx     processes the audio that reaches it
      external    a hardware synth: MIDI goes out, its audio comes back in on an input
      pending     a plug-in still loading, or missing: its saved settings wait here
*/
enum class BlockRole { midiFx, instrument, audioFx, external, pending };

const char* roleName (BlockRole r);

class Block
{
public:
    virtual ~Block() = default;

    int id = 0;                                   // unique for the life of the engine
    std::atomic<bool>  bypass { false };
    std::atomic<float> levelDb { 0.0f };          // instruments and external synths: level into the mix
    std::atomic<float> mix { 1.0f };              // audio effects: how much of the effect (0 dry .. 1 wet)
    std::atomic<bool>  processInput { false };    // an instrument that takes audio processes it instead of mixing in

    virtual juce::String getName() const = 0;
    virtual juce::String getMaker() const { return {}; }
    virtual BlockRole role() const = 0;
    virtual bool canProcessInput() const { return false; }   // instruments with an audio input (a vocoder)
    virtual bool producesMidi() const { return false; }
    virtual bool isHosted() const { return false; }          // a VST3 / AU plug-in
    virtual float mixInPad() const { return 1.0f; }          // plug-in synths come in 12 dB down, as on a line input

    /** Message thread (or the audio thread stopped). blockSize: the device's buffer. */
    virtual void prepare (double /*sampleRate*/, int /*blockSize*/) {}
    /** Audio thread: processes the stereo buffer in place; midi is the MIDI in, and the MIDI out if producesMidi(). */
    virtual void process (juce::AudioBuffer<float>& /*stereo*/, int /*numSamples*/, juce::MidiBuffer& /*midi*/) {}
    virtual void applyMpe (bool /*on*/, int /*bendRange*/) noexcept {}
    virtual int latencySamples() const { return 0; }

    /** For EDIT and MIDI learn; nullptr for blocks with no parameters. */
    virtual juce::AudioProcessor* getProcessor() noexcept { return nullptr; }
    virtual bool hasEditor() const { return false; }
    virtual void setEditorOpen (bool) {}
    /** The last parameter touched in the block's own window, or -1 (for MIDI learn). */
    virtual int takeTouchedParameter() { return -1; }

    /** The block, as a BLOCK element (the engine adds what's common to every block). */
    virtual std::unique_ptr<juce::XmlElement> saveState (bool fresh) = 0;
    /** Asks the plug-in for its sound now (its window just closed, say), so the next save has it. */
    virtual void captureState() {}

    /** The common settings, written into and read back from a BLOCK element. */
    void writeCommon (juce::XmlElement& x) const;
    void readCommon (const juce::XmlElement& x);
};

//==============================================================================
/** Any juce::AudioProcessor in the chain: a hosted plug-in, a built-in synth or a built-in effect. */
class ProcessorBlock : public Block,
                       private juce::AudioProcessorListener
{
public:
    enum class Kind { hosted, builtInSynth, builtInFx };

    ProcessorBlock (std::unique_ptr<juce::AudioProcessor> processor, Kind kind, int builtInIndex = -1);
    ~ProcessorBlock() override;

    juce::String getName() const override;
    juce::String getMaker() const override;
    BlockRole role() const override         { return naturalRole; }
    bool canProcessInput() const override   { return naturalRole == BlockRole::instrument && numIns > 0; }
    bool producesMidi() const override      { return makesMidi; }
    bool isHosted() const override          { return kind == Kind::hosted; }
    float mixInPad() const override         { return kind == Kind::hosted ? 0.25f : 1.0f; }

    void prepare (double sampleRate, int blockSize) override;
    void process (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi) override;
    void applyMpe (bool on, int bendRange) noexcept override;
    int latencySamples() const override     { return processor->getLatencySamples(); }

    juce::AudioProcessor* getProcessor() noexcept override { return processor.get(); }
    bool hasEditor() const override         { return editorAvailable; }
    void setEditorOpen (bool open) override { editorOpen = open; if (! open) stateMaybeChanged = true; }
    int stateRequests = 0;   // for tests: how often the plug-in was asked for its state
    int takeTouchedParameter() override     { return touched.exchange (-1); }

    std::unique_ptr<juce::XmlElement> saveState (bool fresh) override;
    void captureState() override;

    /** How long a plug-in's sound may go unasked while its window is open or its parameters move. */
    static constexpr double stateRefreshSeconds = 60.0;

    Kind getKind() const noexcept           { return kind; }
    int getBuiltInIndex() const noexcept    { return builtInIndex; }

private:
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override { stateMaybeChanged = true; }
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { touched = index; }

    std::unique_ptr<juce::AudioProcessor> processor;
    const Kind kind;
    const int builtInIndex;
    BlockRole naturalRole = BlockRole::audioFx;

    // Asked once, when the block is made. The front panel asks for these many times a second, and some
    // answers are expensive: a VST3's hasEditor() makes and throws away a whole editor view each time.
    juce::String name, maker;
    bool editorAvailable = false, makesMidi = false;
    int numIns = 0;

    juce::AudioBuffer<float> work;
    juce::MidiBuffer pieceMidi, midiOut;
    int pieceSize = 512;
    bool prepared = false;
    std::atomic<bool> stateMaybeChanged { true };
    std::atomic<int> touched { -1 };
    bool editorOpen = false;
    juce::String cachedState;
    double lastAskedMs = 0.0;
};

//==============================================================================
/** A hardware synth: the chain's MIDI goes out to it, its audio comes back in on an input. */
class ExternalBlock : public Block
{
public:
    /** outPort: the MidiHub output slot it plays (-1: none yet). */
    explicit ExternalBlock (int port = -1, int midiChannel = 1, int input = -1, bool pair = false)
    {
        outPort = port;
        channel = juce::jlimit (0, 16, midiChannel);
        returnInput = input;
        returnPair = pair;
    }
    /** The engine shows the output's name (its alias on the deck) in place of this. */
    juce::String getName() const override   { return "HARDWARE SYNTH"; }
    BlockRole role() const override         { return BlockRole::external; }
    std::unique_ptr<juce::XmlElement> saveState (bool) override;   // the engine adds the port's id and name

    std::atomic<int>  outPort { -1 };         // MidiHub output slot
    std::atomic<int>  channel { 1 };          // 1..16, or 0 = as played (MPE)
    std::atomic<int>  returnInput { -1 };     // audio comes back on this input (-1 none)
    std::atomic<bool> returnPair { false };   // ...and the next one, summed to mono

    // audio thread
    int lastPort = -1, lastChannel = 0;
    bool wasSending = false;
};

//==============================================================================
/** A plug-in that is loading, or couldn't be found: its saved settings are kept until it can load. */
class PendingBlock : public Block
{
public:
    explicit PendingBlock (std::unique_ptr<juce::XmlElement> saved);
    juce::String getName() const override   { return pluginName; }
    BlockRole role() const override         { return BlockRole::pending; }
    std::unique_ptr<juce::XmlElement> saveState (bool) override { return std::make_unique<juce::XmlElement> (*saved); }

    bool missing = false;                     // message thread
    juce::String pluginName;

private:
    std::unique_ptr<juce::XmlElement> saved;
};

//==============================================================================
/** TAPE·FOUR's own instruments and effects. */
namespace BuiltIn
{
    enum Synth { poly = 0, epiano = 1, numSynths };
    enum Fx { plate = 0, spring = 1, tapeEcho = 2, numFx };

    juce::String synthName (int which);
    juce::String fxName (int which);
    std::unique_ptr<Block> createSynth (int which);
    std::unique_ptr<Block> createFx (int which);
    /** A block from its saved BLOCK element, if it's a built-in (nullptr for plug-ins and external synths). */
    std::unique_ptr<Block> restore (const juce::XmlElement& block);
}
} // namespace tf
