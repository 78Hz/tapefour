#include "Blocks.h"
#include "ElectricPiano.h"

namespace tf
{
const char* roleName (BlockRole r)
{
    switch (r)
    {
        case BlockRole::midiFx:     return "MIDI EFFECT";
        case BlockRole::instrument: return "INSTRUMENT";
        case BlockRole::audioFx:    return "AUDIO EFFECT";
        case BlockRole::external:   return "EXTERNAL INSTRUMENT";
        case BlockRole::pending:    return "PLUG-IN";
    }
    return "";
}

void Block::writeCommon (juce::XmlElement& x) const
{
    x.setAttribute ("bypass", bypass.load());
    x.setAttribute ("level", levelDb.load());
    x.setAttribute ("mix", mix.load());
    x.setAttribute ("process", processInput.load());
}

void Block::readCommon (const juce::XmlElement& x)
{
    bypass = x.getBoolAttribute ("bypass");
    levelDb = (float) juce::jlimit (-60.0, 12.0, x.getDoubleAttribute ("level", levelDb.load()));
    mix = (float) juce::jlimit (0.0, 1.0, x.getDoubleAttribute ("mix", mix.load()));
    processInput = x.getBoolAttribute ("process");
}

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
        coefCountdown = 0;
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

    using juce::MPESynthesiserVoice::renderNextBlock;
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
            auto env = fenv.getNextSample();

            // The filter's coefficients follow the envelope every 16 samples (0.3 ms): the tan and pow
            // were most of the synth's cost when done every sample, and the difference is inaudible.
            if (--coefCountdown <= 0)
            {
                coefCountdown = 16;
                auto cutoff = juce::jlimit (60.0, sr * 0.45, (500.0 + (400.0 + 5200.0 * vel) * env)
                                                              * std::pow (2.0, 2.2 * sPressure + 2.0 * (sTimbre - 0.5)));
                auto g = std::tan (juce::MathConstants<double>::pi * cutoff / sr);
                auto k = 1.0 / 2.2;
                a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
            }
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

    double freq = 440, p1 = 0, p2 = 0.37, p3 = 0, ic1 = 0, ic2 = 0, a1 = 1, a2 = 0, a3 = 0;
    float vel = 0, pressure = 0, timbre = 0.5f, sPressure = 0, sTimbre = 0.5f;
    int coefCountdown = 0;
    juce::ADSR amp, fenv;
};

//==============================================================================
/** The built-in synths, as ordinary processors so they sit in a chain like any plug-in. */
class BuiltInSynthProcessor : public juce::AudioProcessor
{
public:
    explicit BuiltInSynthProcessor (int k)
        : juce::AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)), kind (k)
    {
        for (int i = 0; i < 15; ++i)
            synth.addVoice (new PolyVoice());
        synth.enableLegacyMode (2);
        synth.setVoiceStealingEnabled (true);
    }

    void applyMpe (bool on, int bendRange) noexcept
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

    const juce::String getName() const override { return BuiltIn::synthName (kind); }
    void prepareToPlay (double sr, int) override { synth.setCurrentPlaybackSampleRate (sr); }
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;   // the double-precision overload stays the base one
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        b.clear();
        synth.renderNextBlock (b, m, 0, b.getNumSamples());
        m.clear();
    }
    double getTailLengthSeconds() const override { return 3.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    int kind;
    bool mpeOn = false;
    int range = 2;
    juce::MPESynthesiser synth;
};

//==============================================================================
/** Base for the built-in effects: stereo in and out, a few knobs, a generic editor. */
class FxProcessor : public juce::AudioProcessor
{
public:
    FxProcessor() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                           .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    double getTailLengthSeconds() const override { return 4.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void releaseResources() override {}

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        juce::XmlElement x ("FX");
        for (auto* p : getParameters())
            if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
                x.setAttribute (f->paramID, (double) f->get());
        copyXmlToBinary (x, mb);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto x = getXmlFromBinary (data, size))
            for (auto* p : getParameters())
                if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
                    if (x->hasAttribute (f->paramID))
                        *f = (float) x->getDoubleAttribute (f->paramID);
    }

protected:
    juce::AudioParameterFloat* param (const juce::String& id, const juce::String& name, float lo, float hi, float def, const juce::String& unit = {})
    {
        auto* p = new juce::AudioParameterFloat (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi), def,
                                                 juce::AudioParameterFloatAttributes().withLabel (unit));
        addParameter (p);
        return p;
    }
    double sr = 48000.0;
};

/** A plate: dense and smooth, with a little pre-delay. */
class PlateFx : public FxProcessor
{
public:
    PlateFx()
    {
        size = param ("size", "Size", 0.0f, 1.0f, 0.62f);
        damping = param ("damping", "Damping", 0.0f, 1.0f, 0.45f);
        predelay = param ("predelay", "Pre-delay", 0.0f, 80.0f, 12.0f, "ms");
    }
    const juce::String getName() const override { return BuiltIn::fxName (BuiltIn::plate); }

    void prepareToPlay (double rate, int) override
    {
        sr = rate;
        reverb.setSampleRate (rate);
        reverb.reset();
        pre.setSize (2, (int) (rate * 0.1) + 8);
        pre.clear();
        prePos = 0;
    }

    using juce::AudioProcessor::processBlock;   // the double-precision overload stays the base one
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::Reverb::Parameters p;
        p.roomSize = 0.55f + 0.44f * size->get();
        p.damping = damping->get();
        p.width = 1.0f;
        p.wetLevel = 0.5f;
        p.dryLevel = 0.0f;
        reverb.setParameters (p);

        const int n = b.getNumSamples();
        const int d = juce::jlimit (0, pre.getNumSamples() - 1, (int) (predelay->get() * 0.001 * sr));
        auto* l = b.getWritePointer (0);
        auto* r = b.getWritePointer (1);
        auto* pl = pre.getWritePointer (0);
        auto* pr = pre.getWritePointer (1);
        const int len = pre.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            pl[prePos] = l[i];
            pr[prePos] = r[i];
            auto rd = (prePos - d + len) % len;
            l[i] = pl[rd];
            r[i] = pr[rd];
            prePos = (prePos + 1) % len;
        }
        reverb.processStereo (l, r, n);
    }

private:
    juce::AudioParameterFloat *size, *damping, *predelay;
    juce::Reverb reverb;
    juce::AudioBuffer<float> pre;
    int prePos = 0;
};

/** A spring tank: a chain of all-passes inside a feedback loop gives the drip and the boing. */
class SpringFx : public FxProcessor
{
public:
    SpringFx()
    {
        decay = param ("decay", "Decay", 0.0f, 1.0f, 0.55f);
        tone = param ("tone", "Tone", 1500.0f, 7000.0f, 3800.0f, "Hz");
        drip = param ("drip", "Drip", 0.0f, 1.0f, 0.6f);
    }
    const juce::String getName() const override { return BuiltIn::fxName (BuiltIn::spring); }

    void prepareToPlay (double rate, int) override
    {
        sr = rate;
        for (int c = 0; c < 2; ++c)
        {
            auto& s = springs[c];
            s.line.assign ((size_t) (rate * (c == 0 ? 0.0331 : 0.0417)) + 1, 0.0f);
            s.pos = 0;
            s.lp = s.hp = s.hpIn = 0.0f;
            for (auto& z : s.ap) z = 0.0f;
        }
    }

    using juce::AudioProcessor::processBlock;   // the double-precision overload stays the base one
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const auto fb = 0.45f + 0.47f * decay->get();
        const auto lpC = (float) std::exp (-juce::MathConstants<double>::twoPi * tone->get() / sr);
        const auto hpC = (float) std::exp (-juce::MathConstants<double>::twoPi * 180.0 / sr);
        const auto a = -(0.35f + 0.5f * drip->get());   // stronger dispersion, more drip
        const int n = b.getNumSamples();
        auto* l = b.getWritePointer (0);
        auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const auto in = 0.5f * (l[i] + r[i]);
            for (int c = 0; c < 2; ++c)
            {
                auto& s = springs[c];
                // springs barely carry bass
                s.hp = hpC * (s.hp + in - s.hpIn);
                s.hpIn = in;
                auto y = s.line[(size_t) s.pos];
                s.lp = y * (1.0f - lpC) + s.lp * lpC;
                auto x = 0.6f * s.hp + fb * s.lp;
                for (auto& z : s.ap)
                {
                    auto o = a * x + z;
                    z = x - a * o;
                    x = o;
                }
                s.line[(size_t) s.pos] = std::tanh (x);
                s.pos = (s.pos + 1) % (int) s.line.size();
                (c == 0 ? l : r)[i] = y;
            }
        }
    }

private:
    struct Spring { std::vector<float> line; int pos = 0; float lp = 0, hp = 0, hpIn = 0; float ap[12] {}; };
    juce::AudioParameterFloat *decay, *tone, *drip;
    Spring springs[2];
};

/** A tape echo: the repeats darken and saturate, and the tape wobbles a little. */
class TapeEchoFx : public FxProcessor
{
public:
    TapeEchoFx()
    {
        time = param ("time", "Time", 60.0f, 800.0f, 340.0f, "ms");
        feedback = param ("feedback", "Feedback", 0.0f, 0.95f, 0.45f);
        tone = param ("tone", "Tone", 1000.0f, 8000.0f, 3200.0f, "Hz");
        wow = param ("wow", "Wow", 0.0f, 1.0f, 0.3f);
    }
    const juce::String getName() const override { return BuiltIn::fxName (BuiltIn::tapeEcho); }

    void prepareToPlay (double rate, int) override
    {
        sr = rate;
        line.assign ((size_t) (rate * 1.0) + 8, 0.0f);
        pos = 0;
        lp = 0.0f;
        smoothTime = time->get() * 0.001 * rate;
        phase = 0.0;
    }

    using juce::AudioProcessor::processBlock;   // the double-precision overload stays the base one
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples();
        const auto len = (int) line.size();
        const auto target = juce::jlimit (8.0, (double) len - 8.0, time->get() * 0.001 * sr);
        const auto fb = feedback->get();
        const auto lpC = (float) std::exp (-juce::MathConstants<double>::twoPi * tone->get() / sr);
        const auto depth = wow->get() * 0.0025 * sr;      // up to 2.5 ms of wobble
        const auto inc = juce::MathConstants<double>::twoPi * 0.7 / sr;
        auto* l = b.getWritePointer (0);
        auto* r = b.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            // Turning TIME moves the head, as on a real echo: the repeats bend in pitch.
            smoothTime += (target - smoothTime) * 0.0004;
            phase += inc;
            if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            auto d = smoothTime + depth * (1.0 + std::sin (phase)) * 0.5;
            auto rp = (double) pos - d;
            while (rp < 0) rp += len;
            auto i0 = (int) rp;
            auto f = (float) (rp - i0);
            auto y = line[(size_t) i0] + (line[(size_t) ((i0 + 1) % len)] - line[(size_t) i0]) * f;

            lp = y * (1.0f - lpC) + lp * lpC;
            const auto in = 0.5f * (l[i] + r[i]);
            line[(size_t) pos] = std::tanh (in + fb * 1.05f * lp);
            pos = (pos + 1) % len;
            l[i] = r[i] = y;
        }
    }

private:
    juce::AudioParameterFloat *time, *feedback, *tone, *wow;
    std::vector<float> line;
    int pos = 0;
    float lp = 0.0f;
    double smoothTime = 0.0, phase = 0.0;
};
} // namespace

//==============================================================================
ProcessorBlock::ProcessorBlock (std::unique_ptr<juce::AudioProcessor> p, Kind k, int index)
    : processor (std::move (p)), kind (k), builtInIndex (index)
{
    if (kind == Kind::builtInSynth)      naturalRole = BlockRole::instrument;
    else if (kind == Kind::builtInFx)    naturalRole = BlockRole::audioFx;
    else if (processor->isMidiEffect())  naturalRole = BlockRole::midiFx;
    else
    {
        bool isInstrument = false;
        if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (processor.get()))
            isInstrument = inst->getPluginDescription().isInstrument;
        const auto ins = processor->getTotalNumInputChannels(), outs = processor->getTotalNumOutputChannels();
        if (isInstrument)                                                         naturalRole = BlockRole::instrument;
        else if (processor->acceptsMidi() && processor->producesMidi() && outs == 0) naturalRole = BlockRole::midiFx;
        else if (processor->acceptsMidi() && ins == 0 && outs > 0)                 naturalRole = BlockRole::instrument;
        else                                                                      naturalRole = BlockRole::audioFx;
    }
    name = processor->getName();
    if (kind != Kind::hosted)
        maker = juce::String (juce::CharPointer_UTF8 ("TAPE\xc2\xb7" "FOUR"));
    else if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (processor.get()))
        maker = inst->getPluginDescription().manufacturerName;
    editorAvailable = processor->hasEditor();
    makesMidi = processor->producesMidi();
    numIns = processor->getTotalNumInputChannels();
    processor->addListener (this);
}

ProcessorBlock::~ProcessorBlock()
{
    processor->removeListener (this);
    if (prepared)
        processor->releaseResources();
}

juce::String ProcessorBlock::getName() const  { return name; }
juce::String ProcessorBlock::getMaker() const { return maker; }

void ProcessorBlock::audioProcessorParameterChanged (juce::AudioProcessor*, int, float)
{
    stateMaybeChanged = true;
}

void ProcessorBlock::prepare (double sampleRate, int blockSize)
{
    if (prepared)
        processor->releaseResources();

    pieceSize = juce::jlimit (32, 8192, blockSize);
    processor->setRateAndBufferSizeDetails (sampleRate, pieceSize);
    processor->prepareToPlay (sampleRate, pieceSize);
    work.setSize (std::max ({ 2, processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels() }), pieceSize);
    pieceMidi.ensureSize (8192);
    midiOut.ensureSize (8192);
    prepared = true;
}

void ProcessorBlock::applyMpe (bool on, int bendRange) noexcept
{
    if (auto* s = dynamic_cast<BuiltInSynthProcessor*> (processor.get()))
        s->applyMpe (on, bendRange);
    else if (auto* e = dynamic_cast<ep::Processor*> (processor.get()))
        e->applyMpe (on, bendRange);
}

void ProcessorBlock::process (juce::AudioBuffer<float>& stereo, int numSamples, juce::MidiBuffer& midi)
{
    if (! prepared)
        return;

    // Busy (being restarted or reconfigured on another thread) or suspended: this block goes without it.
    const juce::ScopedTryLock sl (processor->getCallbackLock());
    if (! sl.isLocked() || processor->isSuspended())
        return;

    // A program change from a controller changes the sound: worth saving next time.
    for (const auto m : midi)
        if (m.numBytes > 0 && (m.data[0] & 0xf0) == 0xc0) { stateMaybeChanged = true; break; }

    const auto ins = processor->getTotalNumInputChannels(), outs = processor->getTotalNumOutputChannels();
    const bool midiOutWanted = makesMidi;
    if (midiOutWanted) midiOut.clear();

    // The processor was promised blocks no longer than the device's; a longer one goes in pieces.
    for (int start = 0; start < numSamples; start += pieceSize)
    {
        const int len = std::min (pieceSize, numSamples - start);
        pieceMidi.clear();
        pieceMidi.addEvents (midi, start, len, -start);

        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), len);
        view.clear();
        if (ins >= 2)
        {
            view.copyFrom (0, 0, stereo, 0, start, len);
            view.copyFrom (1, 0, stereo, 1, start, len);
        }
        else if (ins == 1)
        {
            view.addFrom (0, 0, stereo, 0, start, len, 0.5f);
            view.addFrom (0, 0, stereo, 1, start, len, 0.5f);
        }

        processor->processBlock (view, pieceMidi);

        if (outs >= 2)
        {
            stereo.copyFrom (0, start, view, 0, 0, len);
            stereo.copyFrom (1, start, view, 1, 0, len);
        }
        else if (outs == 1)
        {
            stereo.copyFrom (0, start, view, 0, 0, len);
            stereo.copyFrom (1, start, view, 0, 0, len);
        }

        if (midiOutWanted)
            midiOut.addEvents (pieceMidi, 0, len, start);
    }

    if (midiOutWanted)
        midi.swapWith (midiOut);
}

std::unique_ptr<juce::XmlElement> ProcessorBlock::saveState (bool fresh)
{
    auto x = std::make_unique<juce::XmlElement> ("BLOCK");
    switch (kind)
    {
        case Kind::builtInSynth:
        {
            x->setAttribute ("kind", "synth");
            x->setAttribute ("which", builtInIndex);
            juce::MemoryBlock state;
            processor->getStateInformation (state);
            if (state.getSize() > 0)
                x->setAttribute ("state", state.toBase64Encoding());
            break;
        }

        case Kind::builtInFx:
        {
            x->setAttribute ("kind", "fx");
            x->setAttribute ("which", builtInIndex);
            juce::MemoryBlock state;
            processor->getStateInformation (state);
            x->setAttribute ("state", state.toBase64Encoding());
            break;
        }

        case Kind::hosted:
        {
            // Asking a big synth or sampler for its state can take a while and makes megabytes of text.
            // It's asked at the natural moments (fresh: the tape stops, is saved or ejected, the program
            // quits, its window closes). Between them, if the sound may have changed (a parameter moved,
            // a program change, its window open) it's asked at most once a minute, as a crash backstop.
            const auto now = juce::Time::getMillisecondCounterHiRes();
            const bool due = now - lastAskedMs >= stateRefreshSeconds * 1000.0;
            if (fresh || cachedState.isEmpty() || (due && (editorOpen || stateMaybeChanged.load())))
                captureState();
            x->setAttribute ("kind", "plugin");
            if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (processor.get()))
                x->addChildElement (inst->getPluginDescription().createXml().release());
            x->setAttribute ("state", cachedState);
            break;
        }
    }
    return x;
}

void ProcessorBlock::captureState()
{
    if (kind != Kind::hosted) return;
    stateMaybeChanged = false;
    juce::MemoryBlock state;
    processor->getStateInformation (state);
    cachedState = state.toBase64Encoding();
    lastAskedMs = juce::Time::getMillisecondCounterHiRes();
    ++stateRequests;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> ExternalBlock::saveState (bool)
{
    auto x = std::make_unique<juce::XmlElement> ("BLOCK");
    x->setAttribute ("kind", "external");
    x->setAttribute ("channel", channel.load());
    x->setAttribute ("return", returnInput.load());
    x->setAttribute ("returnPair", returnPair.load());
    return x;
}

PendingBlock::PendingBlock (std::unique_ptr<juce::XmlElement> s) : saved (std::move (s))
{
    pluginName = "PLUG-IN";
    if (saved != nullptr)
        if (auto* d = saved->getChildByName ("PLUGIN"))
            pluginName = d->getStringAttribute ("name", pluginName);
    if (saved == nullptr)
        saved = std::make_unique<juce::XmlElement> ("BLOCK");
    readCommon (*saved);
}

//==============================================================================
juce::String BuiltIn::synthName (int which) { return which == epiano ? "TF ELECTRIC PIANO" : "TF POLY SYNTH"; }

juce::String BuiltIn::fxName (int which)
{
    switch (which)
    {
        case spring:   return "TF SPRING REVERB";
        case tapeEcho: return "TF TAPE ECHO";
        default:       return "TF PLATE";
    }
}

std::unique_ptr<Block> BuiltIn::createSynth (int which)
{
    which = juce::jlimit (0, numSynths - 1, which);
    if (which == epiano)
        return std::make_unique<ProcessorBlock> (std::make_unique<ep::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
    return std::make_unique<ProcessorBlock> (std::make_unique<BuiltInSynthProcessor> (which), ProcessorBlock::Kind::builtInSynth, which);
}

std::unique_ptr<Block> BuiltIn::createFx (int which)
{
    which = juce::jlimit (0, numFx - 1, which);
    std::unique_ptr<juce::AudioProcessor> p;
    if (which == spring)        p = std::make_unique<SpringFx>();
    else if (which == tapeEcho) p = std::make_unique<TapeEchoFx>();
    else                        p = std::make_unique<PlateFx>();
    auto b = std::make_unique<ProcessorBlock> (std::move (p), ProcessorBlock::Kind::builtInFx, which);
    b->mix = which == tapeEcho ? 0.35f : 0.3f;   // send-like by default
    return b;
}

ep::Processor* BuiltIn::electricPiano (Block* b) noexcept
{
    auto* pb = dynamic_cast<ProcessorBlock*> (b);
    return pb != nullptr ? dynamic_cast<ep::Processor*> (pb->getProcessor()) : nullptr;
}

std::unique_ptr<Block> BuiltIn::restore (const juce::XmlElement& x)
{
    std::unique_ptr<Block> b;
    auto kind = x.getStringAttribute ("kind");
    if (kind == "synth")
    {
        b = createSynth (x.getIntAttribute ("which"));
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (x.getStringAttribute ("state")) && mb.getSize() > 0)
            b->getProcessor()->setStateInformation (mb.getData(), (int) mb.getSize());
    }
    else if (kind == "fx")
    {
        b = createFx (x.getIntAttribute ("which"));
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (x.getStringAttribute ("state")) && mb.getSize() > 0)
            b->getProcessor()->setStateInformation (mb.getData(), (int) mb.getSize());
    }
    if (b != nullptr)
        b->readCommon (x);
    return b;
}
} // namespace tf
