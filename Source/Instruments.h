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
    virtual std::unique_ptr<juce::XmlElement> saveState() const = 0;
};

namespace BuiltIn
{
    enum Kind { poly = 0, epiano = 1, numKinds };
    juce::String nameOf (int kind);
    std::unique_ptr<Instrument> create (int kind);
}

class PluginInstrument : public Instrument
{
public:
    explicit PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p);
    ~PluginInstrument() override;

    juce::String getName() const override { return plugin->getName(); }
    void prepare (double sampleRate, int maxBlockSize) override;
    void render (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi) override;
    juce::AudioPluginInstance* getPlugin() noexcept override { return plugin.get(); }
    std::unique_ptr<juce::XmlElement> saveState() const override;

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> work;
    bool prepared = false;
};
} // namespace tf
