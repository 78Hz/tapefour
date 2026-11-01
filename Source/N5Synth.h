#pragma once

#include "N5Model.h"
#include "N5Effects.h"
#include "InstrumentPresets.h"

/*
    TF N5: the engine. 64 oscillator voices (a DOUBLE program's note takes two, so 32 notes), each an
    oscillator, a VDF (the N5's filter: low-pass, its "Color" a gentle lift at the cutoff, no resonance),
    a VDA, three EGs and three LFOs; combinations of up to 8 programs; the two effects units; the arpeggiator.
*/
namespace tf::n5
{
/** A waveform as a stack of band-limited tables, one an octave (so high notes don't alias). */
struct WaveTable
{
    static constexpr int size = 2048, levels = 11;
    std::array<std::array<float, size + 1>, levels> table {};
    /** From harmonic amplitudes and phases (1..1024). */
    void build (const std::vector<std::complex<double>>& harmonics);
    float read (double phase, double increment) const noexcept;
};

class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the memory (message thread) ----
    BankData& bank() noexcept { return edit; }
    /** Call after changing bank(): the engine takes the change at its next block. */
    void changed();
    juce::String programName (int index) const { return programNames[(size_t) std::clamp (index, 0, numPrograms - 1)]; }
    void setProgramName (int index, const juce::String& n) { programNames[(size_t) std::clamp (index, 0, numPrograms - 1)] = n.substring (0, 10); }
    juce::String combiName (int index) const { return combiNames[(size_t) std::clamp (index, 0, numCombis - 1)]; }
    void setCombiName (int index, const juce::String& n) { combiNames[(size_t) std::clamp (index, 0, numCombis - 1)] = n.substring (0, 10); }

    // ---- drawn waves (message thread) ----
    const std::array<float, drawnPoints>& drawnWave (int slot) const { return drawnShapes[(size_t) std::clamp (slot, 0, numDrawn - 1)]; }
    juce::String drawnName (int slot) const { return drawnNames[(size_t) std::clamp (slot, 0, numDrawn - 1)]; }
    void setDrawnName (int slot, const juce::String& n) { drawnNames[(size_t) std::clamp (slot, 0, numDrawn - 1)] = n.substring (0, 12); }
    /** A drawn wave changed: its tables are rebuilt and handed to the engine. */
    void setDrawnWave (int slot, const std::array<float, drawnPoints>& points);
    /** Harmonic amplitudes (1..16) of a drawn wave, for the editor. */
    std::array<float, 16> harmonicsOf (int slot) const;

    /** The panel's keyboard (a note from the window, as if from MIDI). */
    void playNote (int note, int velocity, bool on);
    int voicesSounding() const noexcept { return sounding.load(); }

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 4.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return editorFactory != nullptr; }
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int editorTab = 0, editorLayer = 0, editorTimbre = 0, editorDrawn = 0;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // presets::Presettable: the whole memory (programs, combinations, drawn waves, arpeggiator)
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    struct Engine;

private:
    void rebuildWave (int slot);

    BankData edit;
    std::array<juce::String, numPrograms> programNames;
    std::array<juce::String, numCombis> combiNames;
    std::array<std::array<float, drawnPoints>, numDrawn> drawnShapes {};
    std::array<juce::String, numDrawn> drawnNames;
    std::unique_ptr<Engine> engine;
    std::atomic<int> sounding { 0 };
};

const presets::Kind& presetKind();
} // namespace tf::n5
