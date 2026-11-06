#include "Nymphes.h"

namespace tf::nym
{
namespace
{
    const char* lfoTypes = "BPM|LOW|HIGH|TRACK";
    const char* offOn = "OFF|ON";

    // Dreadbox's CC chart (Nymphes Owner's Manual v2.0, section 8). Defaults are ours: a plain sound.
    const Control controls[numControls] {
        { "oscWave",   "OSC WAVE",        70, 127, 0,   Kind::knob },
        { "oscPw",     "PULSE WIDTH",     12, 127, 64,  Kind::knob },
        { "oscLevel",  "OSC LEVEL",        9, 127, 100, Kind::knob },
        { "oscSub",    "SUB LEVEL",       10, 127, 0,   Kind::knob },
        { "oscNoise",  "NOISE LEVEL",     11, 127, 0,   Kind::knob },
        { "oscDetune", "DETUNE",          15, 127, 0,   Kind::knob },
        { "oscChord",  "CHORD",           16, 127, 0,   Kind::knob },
        { "oscGlide",  "GLIDE",            5, 127, 0,   Kind::knob },
        { "oscLfo",    "LFO 1 TO PITCH",  13, 127, 0,   Kind::knob },
        { "oscEg",     "EG TO PITCH",     14, 127, 0,   Kind::knob },
        { "lpfCutoff", "LPF CUTOFF",      74, 127, 100, Kind::knob },
        { "lpfRes",    "RESONANCE",       71, 127, 0,   Kind::knob },
        { "lpfEg",     "LPF EG DEPTH",     3, 127, 0,   Kind::knob },
        { "lpfTrack",  "LPF TRACKING",     4, 127, 64,  Kind::knob },
        { "lpfLfo",    "LFO 1 TO CUTOFF",  8, 127, 0,   Kind::knob },
        { "hpfCutoff", "HPF CUTOFF",      81, 127, 0,   Kind::knob },
        { "revSize",   "REVERB SIZE",     75, 127, 64,  Kind::knob },
        { "revDecay",  "REVERB DECAY",    76, 127, 64,  Kind::knob },
        { "revFilter", "REVERB FILTER",   77, 127, 64,  Kind::knob },
        { "revMix",    "REVERB MIX",      78, 127, 0,   Kind::knob },
        { "ampLevel",  "AMP LEVEL",        7, 127, 100, Kind::knob },
        { "fegA",      "FILTER EG ATTACK",  79, 127, 0,   Kind::knob },
        { "fegD",      "FILTER EG DECAY",   80, 127, 64,  Kind::knob },
        { "fegS",      "FILTER EG SUSTAIN", 82, 127, 64,  Kind::knob },
        { "fegR",      "FILTER EG RELEASE", 83, 127, 32,  Kind::knob },
        { "aegA",      "AMP EG ATTACK",     73, 127, 0,   Kind::knob },
        { "aegD",      "AMP EG DECAY",      84, 127, 64,  Kind::knob },
        { "aegS",      "AMP EG SUSTAIN",    85, 127, 127, Kind::knob },
        { "aegR",      "AMP EG RELEASE",    72, 127, 32,  Kind::knob },
        { "lfo1Rate",  "LFO 1 RATE",      18, 127, 64,  Kind::knob },
        { "lfo1Wave",  "LFO 1 WAVE",      19, 127, 0,   Kind::knob },
        { "lfo1Delay", "LFO 1 DELAY",     20, 127, 0,   Kind::knob },
        { "lfo1Fade",  "LFO 1 FADE",      21, 127, 0,   Kind::knob },
        { "lfo1Type",  "LFO 1 TYPE",      22, 3,   1,   Kind::choice, lfoTypes },
        { "lfo1Sync",  "LFO 1 KEY SYNC",  23, 1,   0,   Kind::toggle, offOn },
        { "lfo2Rate",  "LFO 2 RATE",      24, 127, 64,  Kind::knob },
        { "lfo2Wave",  "LFO 2 WAVE",      25, 127, 0,   Kind::knob },
        { "lfo2Delay", "LFO 2 DELAY",     26, 127, 0,   Kind::knob },
        { "lfo2Fade",  "LFO 2 FADE",      27, 127, 0,   Kind::knob },
        { "lfo2Type",  "LFO 2 TYPE",      28, 3,   1,   Kind::choice, lfoTypes },
        { "lfo2Sync",  "LFO 2 KEY SYNC",  29, 1,   0,   Kind::toggle, offOn },
        { "playMode",  "PLAY MODE",       17, 5,   0,   Kind::choice, "POLY|UNI A|UNI B|TRI|DUO|MONO" },
        { "legato",    "LEGATO",          68, 1,   0,   Kind::toggle, offOn },
    };

    struct Dest { const char* group; const char* name; int cc; bool perNote; };
    // the mod depth CCs (CC 38 isn't in the chart)
    const Dest dests[numDests] {
        { "OSC", "WAVE", 31, false }, { "OSC", "LEVEL", 32, false }, { "OSC", "SUB", 33, false }, { "OSC", "NOISE", 34, false },
        { "OSC", "LFO 1", 35, true }, { "OSC", "PW", 36, false }, { "OSC", "GLIDE", 37, true }, { "OSC", "DETUNE", 39, false },
        { "OSC", "CHORD", 40, false }, { "OSC", "EG", 41, true },
        { "FILTER", "CUTOFF", 42, true }, { "FILTER", "RES", 43, false }, { "FILTER", "EG", 44, true }, { "FILTER", "HPF", 45, false },
        { "FILTER", "TRACK", 46, true }, { "FILTER", "LFO 1", 47, true },
        { "FILTER EG", "A", 48, true }, { "FILTER EG", "D", 49, true }, { "FILTER EG", "S", 50, true }, { "FILTER EG", "R", 51, true },
        { "AMP EG", "A", 52, true }, { "AMP EG", "D", 53, true }, { "AMP EG", "S", 54, true }, { "AMP EG", "R", 55, true },
        { "LFO 1", "RATE", 56, true }, { "LFO 1", "WAVE", 57, true }, { "LFO 1", "DELAY", 58, true }, { "LFO 1", "FADE", 59, true },
        { "LFO 2", "RATE", 60, false }, { "LFO 2", "WAVE", 61, false }, { "LFO 2", "DELAY", 62, false }, { "LFO 2", "FADE", 63, false },
        { "REVERB", "SIZE", 86, false }, { "REVERB", "DECAY", 87, false }, { "REVERB", "FILT", 88, false }, { "REVERB", "MIX", 89, false },
    };
    const char* sources[numSources] { "LFO 2", "MOD WHEEL", "VELOCITY", "PRESSURE" };

    int controlForCc (int cc)
    {
        for (int c = 0; c < numControls; ++c) if (controls[c].cc == cc) return c;
        return -1;
    }
    int destForCc (int cc)
    {
        for (int d = 0; d < numDests; ++d) if (dests[d].cc == cc) return d;
        return -1;
    }
    void addMpeSetup (juce::MidiBuffer& b, bool on, int bendRange)
    {
        // MPE Configuration Message on the master channel (RPN 6: 15 member channels, a lower zone), then the
        // members' pitch-bend range (RPN 0 on one member channel applies to all)
        auto cc = [&b] (int ch, int n, int v) { b.addEvent (juce::MidiMessage::controllerEvent (ch, n, v), 0); };
        cc (1, 101, 0); cc (1, 100, 6); cc (1, 6, on ? 15 : 0); cc (1, 101, 127); cc (1, 100, 127);
        if (on) { cc (2, 101, 0); cc (2, 100, 0); cc (2, 6, std::clamp (bendRange, 0, 96)); cc (2, 101, 127); cc (2, 100, 127); }
    }
}

const Control& control (int index) { return controls[std::clamp (index, 0, numControls - 1)]; }
const char* sourceName (int s) { return sources[std::clamp (s, 0, numSources - 1)]; }
const char* destName (int d) { return dests[std::clamp (d, 0, numDests - 1)].name; }
const char* destGroup (int d) { return dests[std::clamp (d, 0, numDests - 1)].group; }
int destCc (int d) { return dests[std::clamp (d, 0, numDests - 1)].cc; }
bool destPerNote (int d) { return dests[std::clamp (d, 0, numDests - 1)].perNote; }
bool destOnSynth (int s, int d) { return s != 0 || d < 24; }                 // LFO 2: the first 24 (the manual)
bool destUnsure (int s, int d) { return destOnSynth (s, d) && d >= 28; }     // LFO 2 and REVERB depths: in the chart, not in the 28

State::State()
{
    for (int c = 0; c < numControls; ++c) values[(size_t) c] = controls[c].def;
    for (auto& row : depth) row.fill (-1);
}

//==============================================================================
/** A control as a parameter, so it can be MIDI-learned like a plug-in's (the value is the processor's). */
class Processor::Param : public juce::AudioProcessorParameter
{
public:
    Param (Processor& p, int c) : proc (p), index (c) {}
    float getValue() const override { return (float) proc.value (index) / (float) controls[index].hi; }
    void setValue (float v) override { proc.values[(size_t) index] = juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * (float) controls[index].hi); }
    float getDefaultValue() const override { return (float) controls[index].def / (float) controls[index].hi; }
    juce::String getName (int max) const override { return juce::String (controls[index].name).substring (0, max); }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override { return controls[index].hi + 1; }
    bool isDiscrete() const override { return true; }
    juce::String getText (float v, int) const override
    {
        const int n = juce::roundToInt (v * (float) controls[index].hi);
        if (controls[index].choices != nullptr) return juce::StringArray::fromTokens (controls[index].choices, "|", "")[n];
        return juce::String (n);
    }
    float getValueForText (const juce::String& t) const override { return (float) t.getIntValue() / (float) controls[index].hi; }

private:
    Processor& proc;
    int index;
};

Processor::Processor() : juce::AudioProcessor (BusesProperties())
{
    State s;
    for (int c = 0; c < numControls; ++c) { values[(size_t) c] = s.values[(size_t) c]; sent[(size_t) c] = s.values[(size_t) c]; }
    for (auto& row : depths) for (auto& d : row) d = -1;
    for (auto& row : sentDepth) row.fill (-1);
    for (int c = 0; c < numControls; ++c) addParameter (new Param (*this, c));
}

Processor::~Processor() = default;

void Processor::setValue (int c, int v)
{
    if (c < 0 || c >= numControls) return;
    values[(size_t) c] = std::clamp (v, 0, controls[c].hi);
}

void Processor::setDepth (int s, int d, int v)
{
    if (s < 0 || s >= numSources || d < 0 || d >= numDests || ! destOnSynth (s, d)) return;
    depths[(size_t) s][(size_t) d] = v < 0 ? -1 : std::clamp (v, 0, 127);
}

void Processor::setMpe (bool on) { mpe = on; mpePending = true; }
void Processor::setBendRange (int semis) { bend = std::clamp (semis, 1, 96); mpePending = true; }
void Processor::setDepthsCentred (bool on) { centred = on; }

State Processor::snapshot() const
{
    State s;
    for (int c = 0; c < numControls; ++c) s.values[(size_t) c] = value (c);
    for (int i = 0; i < numSources; ++i) for (int d = 0; d < numDests; ++d) s.depth[(size_t) i][(size_t) d] = depth (i, d);
    s.mpe = isMpe(); s.bendRange = bendRange(); s.keepInStep = keepsInStep(); s.depthCentred = depthsCentred();
    return s;
}

//==============================================================================
void Processor::noteVoices (const juce::MidiMessage& m)
{
    const int ch = m.getChannel();
    const bool perChannel = mpe.load();
    auto each = [&] (auto&& fn) { for (auto& v : voices) if (v.note.load() >= 0 && (! perChannel || v.channel.load() == ch)) fn (v); };
    if (m.isNoteOn())
    {
        Voice* slot = nullptr;
        for (auto& v : voices) if (v.note.load() < 0) { slot = &v; break; }
        if (slot == nullptr) slot = &voices[0];
        slot->channel = ch;
        slot->bend = 0.0f; slot->pressure = 0.0f; slot->slide = 0.0f;
        slot->note = m.getNoteNumber();
    }
    else if (m.isNoteOff())
    {
        for (auto& v : voices) if (v.note.load() == m.getNoteNumber() && v.channel.load() == ch) v.note = -1;
    }
    else if (m.isPitchWheel())      each ([&] (Voice& v) { v.bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f; });
    else if (m.isChannelPressure()) each ([&] (Voice& v) { v.pressure = (float) m.getChannelPressureValue() / 127.0f; });
    else if (m.isAftertouch())
    {
        for (auto& v : voices) if (v.note.load() == m.getNoteNumber() && v.channel.load() == ch) v.pressure = (float) m.getAfterTouchValue() / 127.0f;
    }
    else if (m.isController() && m.getControllerNumber() == 74 && (! perChannel || ch != 1))
        each ([&] (Voice& v) { v.slide = (float) m.getControllerValue() / 127.0f; });
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        for (auto& v : voices) v.note = -1;
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    juce::MidiBuffer out;
    const bool zoned = mpe.load();
    auto send = [&] (int cc, int v)
    {
        out.addEvent (juce::MidiMessage::controllerEvent (1, cc, v), 0);
        lastSentCc = (cc << 8) | v;
    };
    auto chooseSource = [&] (int s)
    {
        if (synthSource == s) return;
        send (30, s);
        synthSource = s;
    };

    // ---- what comes in: notes and the rest pass on; with KEEP IN STEP, our CCs move the knobs ----
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isController() && keep.load() && (! zoned || m.getChannel() == 1))
        {
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (cc == 30) { inSource = std::clamp (v, 0, numSources - 1); continue; }
            if (const int c = controlForCc (cc); c >= 0)
            {
                const int val = std::clamp (v, 0, controls[c].hi);
                values[(size_t) c] = val;
                if (val != sent[(size_t) c]) { send (cc, val); sent[(size_t) c] = val; }   // new to the synth: on it goes; its own echo stops here
                continue;
            }
            if (const int d = destForCc (cc); d >= 0 && destOnSynth (inSource, d))
            {
                depths[(size_t) inSource][(size_t) d] = v;
                if (sentDepth[(size_t) inSource][(size_t) d] != v)
                {
                    chooseSource (inSource);
                    send (cc, v);
                    sentDepth[(size_t) inSource][(size_t) d] = v;
                }
                continue;
            }
        }
        noteVoices (m);
        out.addEvent (m, meta.samplePosition);
    }

    // ---- SEND ALL (a button, a loaded preset, a reopened deck) ----
    if (sendAllPending.exchange (false))
    {
        sent.fill (-1);
        for (auto& row : sentDepth) row.fill (-2);   // every set cell again
        synthSource = -1;
        mpePending = true;
    }
    if (mpePending.exchange (false)) addMpeSetup (out, zoned, bend.load());

    // ---- what changed since the synth was last told ----
    for (int c = 0; c < numControls; ++c)
        if (const int v = values[(size_t) c].load(); v != sent[(size_t) c])
        {
            send (controls[c].cc, v);
            sent[(size_t) c] = v;
        }
    const int none = centred.load() ? 64 : 0;
    for (int s = 0; s < numSources; ++s)
        for (int d = 0; d < numDests; ++d)
        {
            const int v = depths[(size_t) s][(size_t) d].load();
            auto& was = sentDepth[(size_t) s][(size_t) d];
            if (v == was || (v < 0 && was == -2)) { if (v < 0) was = -1; continue; }
            if (! destOnSynth (s, d)) continue;
            chooseSource (s);
            send (dests[d].cc, v < 0 ? none : v);   // unset after being set: back to no modulation
            was = v;
        }
    midi.swapWith (out);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("TFNYMPHES");
    x->setAttribute ("version", 1);
    for (int c = 0; c < numControls; ++c) x->setAttribute (controls[c].id, value (c));
    x->setAttribute ("mpe", isMpe());
    x->setAttribute ("bend", bendRange());
    x->setAttribute ("keep", keepsInStep());
    x->setAttribute ("centred", depthsCentred());
    for (int s = 0; s < numSources; ++s)
        for (int d = 0; d < numDests; ++d)
            if (const int v = depth (s, d); v >= 0)
            {
                auto* e = x->createNewChildElement ("MOD");
                e->setAttribute ("source", s);
                e->setAttribute ("cc", dests[d].cc);
                e->setAttribute ("depth", v);
            }
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("TFNYMPHES")) return;
    for (int c = 0; c < numControls; ++c) setValue (c, x.getIntAttribute (controls[c].id, controls[c].def));
    mpe = x.getBoolAttribute ("mpe");
    bend = std::clamp (x.getIntAttribute ("bend", 48), 1, 96);
    keep = x.getBoolAttribute ("keep", true);
    centred = x.getBoolAttribute ("centred");
    for (auto& row : depths) for (auto& d : row) d = -1;
    for (auto* e : x.getChildWithTagNameIterator ("MOD"))
        if (const int d = destForCc (e->getIntAttribute ("cc")); d >= 0)
            setDepth (e->getIntAttribute ("source"), d, e->getIntAttribute ("depth", -1));
    sendAll();   // the synth gets the whole sound
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    writePresetAttributes (*x);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        fromXml (*x);
        readPresetAttributes (*x);
    }
}

const presets::Kind& presetKind()
{
    static const presets::Kind kind { "Nymphes", "tfny", "TFNYMPHES", "SOUND", [] (const juce::XmlElement& s)
    {
        const auto modes = juce::StringArray::fromTokens (controls[playMode].choices, "|", "");
        int mods = 0;
        for (auto* e : s.getChildWithTagNameIterator ("MOD")) { juce::ignoreUnused (e); ++mods; }
        return modes[std::clamp (s.getIntAttribute ("playMode"), 0, 5)] + (s.getBoolAttribute ("mpe") ? " MPE" : "")
               + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + juce::String (mods) + (mods == 1 ? " ROUTE" : " ROUTES");
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return nym::presetKind(); }
} // namespace tf::nym
