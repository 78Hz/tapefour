#pragma once

#include "Common.h"

namespace tf
{
/** Something a track can "plug in": a hosted VST3 instrument or one of the built-in ones. */
class Instrument
{
public:
    virtual ~Instrument() = default;
    virtual juce::String getName() const = 0;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    /** Renders into the first two channels of `stereo` (which has room for maxBlockSize samples). */
    virtual void render (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi) = 0;
    virtual juce::AudioPluginInstance* getPlugin() noexcept { return nullptr; }
    /** Audio thread: MPE on (lower zone, 15 member channels) or off (plain MIDI, all channels). */
    virtual void applyMpe (bool /*on*/, int /*bendRange*/) noexcept {}
    /** Message thread. fresh = ask a plug-in for its settings now; otherwise a copy is reused
        unless something suggests the sound has changed (cheap enough to call every few seconds). */
    virtual std::unique_ptr<juce::XmlElement> saveState (bool fresh) = 0;
    /** While a plug-in's window is open its sound may change at any moment. */
    virtual void setEditorOpen (bool) {}
};

namespace BuiltIn
{
    enum Kind { poly = 0, epiano = 1, numKinds };
    juce::String nameOf (int kind);
    std::unique_ptr<Instrument> create (int kind);
}

class PluginInstrument : public Instrument,
                         private juce::AudioProcessorListener
{
public:
    explicit PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p);
    ~PluginInstrument() override;

    juce::String getName() const override { return plugin->getName(); }
    /** maxBlockSize is the device's buffer; longer blocks are rendered in pieces. */
    void prepare (double sampleRate, int maxBlockSize) override;
    void render (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi) override;
    juce::AudioPluginInstance* getPlugin() noexcept override { return plugin.get(); }
    std::unique_ptr<juce::XmlElement> saveState (bool fresh) override;
    void setEditorOpen (bool open) override { editorOpen = open; if (! open) stateMaybeChanged = true; }

private:
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { stateMaybeChanged = true; }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override  { stateMaybeChanged = true; }

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer pieceMidi;
    int pieceSize = 512;
    bool prepared = false;
    std::atomic<bool> stateMaybeChanged { true };
    bool editorOpen = false;
    juce::String cachedState;
};
} // namespace tf
