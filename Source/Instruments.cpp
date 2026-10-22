#include "Instruments.h"

namespace tf
{
namespace
{
inline float polyBlep (double t, double dt) noexcept
{
    if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
    return 0.0f;
}

//==============================================================================
/**
    Warm two-oscillator poly. MPE: per-note pitch bend, pressure opens the
    filter and swells the level, timbre (CC74) shifts the filter.
*/
class PolyVoice : public juce::MPESynthesiserVoice
{
public:
    void noteStarted() override
    {
        auto note = getCurrentlyPlayingNote();
        vel = note.noteOnVelocity.asUnsignedFloat();
        updateNote();
        amp.setSampleRate (currentSampleRate);
        fenv.setSampleRate (currentSampleRate);
        amp.setParameters ({ 0.004f, 0.35f, 0.65f, 0.35f });
        fenv.setParameters ({ 0.002f, 0.45f, 0.25f, 0.35f });
        amp.noteOn();
        fenv.noteOn();
    }

    void noteStopped (bool allowTailOff) override
    {
        if (allowTailOff) { amp.noteOff(); fenv.noteOff(); }
        else { amp.reset(); fenv.reset(); clearCurrentNote(); }
    }

    void notePitchbendChanged() override { updateNote(); }
    void notePressureChanged() override  { updateNote(); }
    void noteTimbreChanged() override    { updateNote(); }
    void noteKeyStateChanged() override  {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! amp.isActive()) return;

        const auto sr = currentSampleRate;
        const auto d1 = freq * 1.0040 / sr, d2 = freq * 0.9960 / sr;

        while (--num >= 0)
        {
            auto s = (float) (2.0 * p1 - 1.0) - polyBlep (p1, d1) + (float) (2.0 * p2 - 1.0) - polyBlep (p2, d2);
            s += 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * p3);
            p1 += d1; if (p1 >= 1.0) p1 -= 1.0;
            p2 += d2; if (p2 >= 1.0) p2 -= 1.0;
            p3 += d1 * 0.5; if (p3 >= 1.0) p3 -= 1.0;

            // smooth the expressive controls so slides and pressure don't zip
            sPressure += (pressure - sPressure) * 0.002f;
            sTimbre += (timbre - sTimbre) * 0.002f;

            auto cutoff = juce::jlimit (60.0, sr * 0.45, (500.0 + (400.0 + 5200.0 * vel) * fenv.getNextSample())
                                                          * std::pow (2.0, 2.2 * sPressure + 2.0 * (sTimbre - 0.5)));
            auto g = std::tan (juce::MathConstants<double>::pi * cutoff / sr);
            auto k = 1.0 / 2.2;
            auto a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            auto v3 = s - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1; ic2 = 2.0 * v2 - ic2;

            auto y = (float) v2 * amp.getNextSample() * (0.12f + 0.12f * vel) * (0.75f + 0.6f * sPressure);
            for (int c = out.getNumChannels(); --c >= 0;)
                out.addSample (c, start, y);
            ++start;
        }

        if (! amp.isActive()) clearCurrentNote();
    }

private:
    void updateNote()
    {
        auto note = getCurrentlyPlayingNote();
        freq = note.getFrequencyInHertz();
        pressure = note.pressure.asUnsignedFloat();
        timbre = note.timbre.asUnsignedFloat();
    }

    double freq = 440, p1 = 0, p2 = 0.37, p3 = 0, ic1 = 0, ic2 = 0;
    float vel = 0, pressure = 0, timbre = 0.5f, sPressure = 0, sTimbre = 0.5f;
    juce::ADSR amp, fenv;
};

//==============================================================================
/** Tine electric piano: two-operator FM. MPE: per-note bend, pressure adds bark and level. */
class EPianoVoice : public juce::MPESynthesiserVoice
{
public:
    void noteStarted() override
    {
        auto note = getCurrentlyPlayingNote();
        vel = note.noteOnVelocity.asUnsignedFloat();
        freq = note.getFrequencyInHertz();
        pressure = note.pressure.asUnsignedFloat();
        amp.setSampleRate (currentSampleRate);
        amp.setParameters ({ 0.002f, 2.8f, 0.0f, 0.45f });
        amp.noteOn();
        index = 1.2 + 2.6 * vel;
        bark = 0.6 * vel;
        pc = pm = pb = 0;
    }

    void noteStopped (bool allowTailOff) override
    {
        if (allowTailOff) amp.noteOff();
        else { amp.reset(); clearCurrentNote(); }
    }

    void notePitchbendChanged() override { freq = getCurrentlyPlayingNote().getFrequencyInHertz(); }
    void notePressureChanged() override  { pressure = getCurrentlyPlayingNote().pressure.asUnsignedFloat(); }
    void noteTimbreChanged() override    {}
    void noteKeyStateChanged() override  {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! amp.isActive()) return;

        const auto sr = currentSampleRate;
        const auto tp = juce::MathConstants<double>::twoPi;
        const auto idxDecay = std::exp (-1.0 / (0.35 * sr)), barkDecay = std::exp (-1.0 / (0.03 * sr));

        while (--num >= 0)
        {
            sPressure += (pressure - sPressure) * 0.002f;
            auto mod = std::sin (tp * pm) * (index + 1.5 * sPressure) + std::sin (tp * pb) * bark;
            auto y = (float) std::sin (tp * pc + mod) * amp.getNextSample() * (0.10f + 0.16f * vel) * (0.8f + 0.5f * sPressure);
            pc += freq / sr; pm += freq / sr; pb += freq * 14.0 / sr;
            pc -= std::floor (pc); pm -= std::floor (pm); pb -= std::floor (pb);
            index *= idxDecay; bark *= barkDecay;

            for (int c = out.getNumChannels(); --c >= 0;)
                out.addSample (c, start, y);
            ++start;
        }

        if (! amp.isActive()) clearCurrentNote();
    }

private:
    double freq = 440, pc = 0, pm = 0, pb = 0, index = 1, bark = 0;
    float vel = 0, pressure = 0, sPressure = 0;
    juce::ADSR amp;
};

//==============================================================================
class BuiltInInstrument : public Instrument
{
public:
    explicit BuiltInInstrument (int k) : kind (k)
    {
        for (int i = 0; i < 15; ++i)
        {
            if (kind == BuiltIn::epiano) synth.addVoice (new EPianoVoice());
            else                         synth.addVoice (new PolyVoice());
        }
        synth.enableLegacyMode (2);
        synth.setVoiceStealingEnabled (true);
    }

    juce::String getName() const override { return BuiltIn::nameOf (kind); }

    void prepare (double sampleRate, int) override
    {
        synth.setCurrentPlaybackSampleRate (sampleRate);
    }

    void applyMpe (bool on, int bendRange) noexcept override
    {
        if (on == mpeOn && bendRange == range) return;
        mpeOn = on;
        range = bendRange;
        if (on)
        {
            juce::MPEZoneLayout layout;
            layout.setLowerZone (15, bendRange, 2);
            synth.setZoneLayout (layout);
        }
        else
        {
            synth.enableLegacyMode (2);
        }
    }

    void render (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi) override
    {
        synth.renderNextBlock (stereo, midi, 0, numSamples);
    }

    std::unique_ptr<juce::XmlElement> saveState() const override
    {
        auto x = std::make_unique<juce::XmlElement> ("INSTRUMENT");
        x->setAttribute ("kind", "builtin");
        x->setAttribute ("which", kind);
        return x;
    }

private:
    int kind;
    bool mpeOn = false;
    int range = 2;
    juce::MPESynthesiser synth;
};
} // namespace

//==============================================================================
juce::String BuiltIn::nameOf (int kind)
{
    return kind == epiano ? "TF TINE PIANO" : "TF POLY SYNTH";
}

std::unique_ptr<Instrument> BuiltIn::create (int kind)
{
    return std::make_unique<BuiltInInstrument> (juce::jlimit (0, numKinds - 1, kind));
}

//==============================================================================
PluginInstrument::PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p)
    : plugin (std::move (p))
{
}

PluginInstrument::~PluginInstrument()
{
    if (prepared)
        plugin->releaseResources();
}

void PluginInstrument::prepare (double sampleRate, int maxBlockSize)
{
    if (prepared)
        plugin->releaseResources();

    plugin->setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    plugin->prepareToPlay (sampleRate, maxBlockSize);
    work.setSize (std::max ({ 2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels() }), maxBlockSize);
    prepared = true;
}

void PluginInstrument::render (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi)
{
    if (! prepared || numSamples > work.getNumSamples())
        return;

    const juce::ScopedTryLock sl (plugin->getCallbackLock());
    if (! sl.isLocked())
        return;

    juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), numSamples);
    view.clear();
    plugin->processBlock (view, midi);

    auto outs = plugin->getTotalNumOutputChannels();
    if (outs <= 0) return;
    // Soft synths come out near 0 dBFS; a real synth into a tape deck's line input
    // sits around -12 dB below that, which is where the deck's 0 VU is.
    constexpr float pad = 0.25f;
    stereo.addFrom (0, 0, view, 0, 0, numSamples, pad);
    stereo.addFrom (1, 0, view, outs > 1 ? 1 : 0, 0, numSamples, pad);
}

std::unique_ptr<juce::XmlElement> PluginInstrument::saveState() const
{
    auto x = std::make_unique<juce::XmlElement> ("INSTRUMENT");
    x->setAttribute ("kind", "plugin");
    x->addChildElement (plugin->getPluginDescription().createXml().release());

    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    x->setAttribute ("state", state.toBase64Encoding());
    return x;
}
} // namespace tf
