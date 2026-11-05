#include "StepSeq.h"

namespace tf::step
{
juce::String noteName (int note)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    note = std::clamp (note, 0, 127);
    return juce::String (names[note % 12]) + juce::String (note / 12 - 1);
}

juce::String msText (double ms)
{
    if (ms < 1000.0) return juce::String (juce::roundToInt (ms)) + " ms";
    return juce::String (ms / 1000.0, ms < 10000.0 ? 2 : 1) + " s";
}

double State::cycleMs() const
{
    double t = 0.0;
    for (int i = 0; i < std::clamp (length, 1, numSteps); ++i) t += steps[(size_t) i].ms;
    return t;
}

//==============================================================================
Processor::Processor() : juce::AudioProcessor (BusesProperties())
{
    run = edit;
}

Processor::~Processor() = default;

void Processor::changed()
{
    auto copy = std::make_unique<State> (edit);
    const juce::SpinLock::ScopedLockType sl (pendingLock);
    pending = std::move (copy);
}

void Processor::setPlaying (bool on) { playing = on; }

void Processor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void Processor::noteOff (juce::MidiBuffer& out, int at)
{
    if (held >= 0) out.addEvent (juce::MidiMessage::noteOff (1, held), at);
    held = -1;
}

void Processor::startStep (juce::MidiBuffer& out, int at)
{
    const int len = std::clamp (run.length, 1, numSteps);
    const bool tiedIn = held >= 0 && offAt < 0.0;     // the last step was tied into this one
    index = index + 1 >= len ? 0 : index + 1;
    const auto& st = run.steps[(size_t) index];
    const double samples = std::max (1.0, std::clamp (st.ms, shortestMs, longestMs) * sr / 1000.0);
    const double startAt = nextStepAt;
    nextStepAt = startAt + samples;
    shownStep = index;

    if (st.rest)
    {
        noteOff (out, at);
        offAt = -1.0;
        return;
    }
    const int note = std::clamp (st.note + shift.load(), 0, 127);
    if (! (tiedIn && held == note))
    {
        // tied: the new note first, then the old one off, so a mono synth slides from one to the other
        out.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) std::clamp (st.velocity, 1, 127)), at);
        if (held >= 0) out.addEvent (juce::MidiMessage::noteOff (1, held), at);
        held = note;
    }
    offAt = st.tie ? -1.0 : startAt + std::max (1.0, samples * std::clamp (st.gate, 1, 100) / 100.0);
}

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    {
        const juce::SpinLock::ScopedTryLockType sl (pendingLock);
        if (sl.isLocked() && pending != nullptr)
        {
            run = *pending;
            pending.reset();
        }
    }

    // the keyboard: a key transposes the sequence (and it stays until the next); the rest passes on
    juce::MidiBuffer out;
    for (const auto m : midi)
    {
        const auto msg = m.getMessage();
        if (msg.isNoteOn())
        {
            if (run.transpose) shift = msg.getNoteNumber() - run.homeNote;
            continue;
        }
        if (msg.isNoteOff()) continue;
        if (msg.isAllNotesOff() || msg.isAllSoundOff()) { held = -1; offAt = -1.0; }
        out.addEvent (msg, m.samplePosition);
    }

    const int n = buffer.getNumSamples();
    const bool want = playing.load();
    if (want && ! running)
    {
        running = true;
        now = 0;
        nextStepAt = 0.0;
        offAt = -1.0;
        index = -1;
        noteOff (out, 0);
    }
    if (! want && running)
    {
        running = false;
        noteOff (out, 0);
        offAt = -1.0;
        index = -1;
        shownStep = -1;
    }
    if (running && n > 0)
    {
        // every gate's end and step's start in this block, in order (a gate ending as the next step
        // starts goes first, so the note is struck again)
        const auto end = (double) (now + n);
        for (int guard = 0; guard < 4 * n + 64; ++guard)
        {
            const double nextOff = offAt >= 0.0 ? offAt : std::numeric_limits<double>::max();
            const double next = std::min (nextStepAt, nextOff);
            if (next >= end) break;
            const int at = juce::jlimit (0, n - 1, (int) std::floor (next - (double) now));
            if (nextOff <= nextStepAt) { noteOff (out, at); offAt = -1.0; }
            else startStep (out, at);
        }
        now += n;
    }
    midi.swapWith (out);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("TFSTEP");
    x->setAttribute ("version", 1);
    x->setAttribute ("length", edit.length);
    x->setAttribute ("transpose", edit.transpose);
    x->setAttribute ("home", edit.homeNote);
    for (int i = 0; i < numSteps; ++i)
    {
        const auto& st = edit.steps[(size_t) i];
        auto* e = x->createNewChildElement ("STEP");
        e->setAttribute ("index", i);
        e->setAttribute ("note", st.note);
        e->setAttribute ("rest", st.rest);
        e->setAttribute ("tie", st.tie);
        e->setAttribute ("ms", st.ms);
        e->setAttribute ("gate", st.gate);
        e->setAttribute ("velocity", st.velocity);
    }
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("TFSTEP")) return;
    State s;
    s.length = std::clamp (x.getIntAttribute ("length", numSteps), 1, numSteps);
    s.transpose = x.getBoolAttribute ("transpose", true);
    s.homeNote = std::clamp (x.getIntAttribute ("home", 36), lowestNote, highestNote);
    for (auto* e : x.getChildWithTagNameIterator ("STEP"))
    {
        const int i = e->getIntAttribute ("index", -1);
        if (i < 0 || i >= numSteps) continue;
        auto& st = s.steps[(size_t) i];
        st.note = std::clamp (e->getIntAttribute ("note", 36), lowestNote, highestNote);
        st.rest = e->getBoolAttribute ("rest");
        st.tie = e->getBoolAttribute ("tie");
        st.ms = std::clamp (e->getDoubleAttribute ("ms", defaultMs), shortestMs, longestMs);
        st.gate = std::clamp (e->getIntAttribute ("gate", 50), 1, 100);
        st.velocity = std::clamp (e->getIntAttribute ("velocity", 100), 1, 127);
    }
    edit = s;
    changed();
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
    static const presets::Kind kind { "Step", "tfst", "TFSTEP", "SEQUENCE", [] (const juce::XmlElement& s)
    {
        State st;
        st.length = std::clamp (s.getIntAttribute ("length", numSteps), 1, numSteps);
        for (auto* e : s.getChildWithTagNameIterator ("STEP"))
            if (const int i = e->getIntAttribute ("index", -1); i >= 0 && i < numSteps)
                st.steps[(size_t) i].ms = std::clamp (e->getDoubleAttribute ("ms", defaultMs), shortestMs, longestMs);
        return juce::String (st.length) + " STEPS" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + msText (st.cycleMs());
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return step::presetKind(); }
} // namespace tf::step
