#include "MidiMapper.h"

namespace tf
{
namespace
{
    const char* controlIds[] { "gain", "high", "mid", "freq", "low", "pan", "fader", "mute", "solo", "arm",
                               "masterFader", "phones", "pitch", "speed", "nr", "play", "stop", "rec", "rew", "ff", "rtz" };
    const char* modeIds[] { "absolute", "relTwos", "relOffset", "relSign", "toggle" };
    const char* msgIds[] { "cc", "cc14", "note" };

    template <size_t N>
    int indexOf (const char* (&ids)[N], const juce::String& s, int fallback)
    {
        for (size_t i = 0; i < N; ++i) if (s == ids[i]) return (int) i;
        return fallback;
    }
}

juce::String MidiMapper::controlName (Control c)
{
    switch (c)
    {
        case Control::gain: return "GAIN";
        case Control::high: return "HIGH";
        case Control::mid: return "MID";
        case Control::freq: return "FREQ";
        case Control::low: return "LOW";
        case Control::pan: return "PAN";
        case Control::fader: return "FADER";
        case Control::mute: return "MUTE";
        case Control::solo: return "SOLO";
        case Control::arm: return "ARM";
        case Control::masterFader: return "MASTER FADER";
        case Control::phones: return "PHONES";
        case Control::pitch: return "PITCH";
        case Control::speed: return "TAPE SPEED";
        case Control::nr: return "NOISE REDUCTION";
        case Control::play: return "PLAY";
        case Control::stop: return "STOP";
        case Control::rec: return "REC";
        case Control::rew: return "REW";
        case Control::ff: return "F.FWD";
        case Control::rtz: return "RTZ";
        case Control::numControls: default: break;
    }
    return {};
}

MidiMapper::MidiMapper (MidiHub& h) : hub (h)
{
    scratch.ensureSize (4096);
    mappings.reserve (256);
}

//==============================================================================
std::vector<MidiMapper::Mapping> MidiMapper::getMappings() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return mappings;
}

void MidiMapper::remove (int id)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [id] (const Mapping& m) { return m.id == id; }), mappings.end());
}

void MidiMapper::removeForTarget (const Target& t)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [&t] (const Mapping& m) { return m.target == t; }), mappings.end());
}

void MidiMapper::forgetBlock (int track, int blockId)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [=] (const Mapping& m)
                    { return m.target.plugin && m.target.track == track && m.target.blockId == blockId; }), mappings.end());
    if (learnTarget.plugin && learnTarget.blockId == blockId)
        learning = false;
}

void MidiMapper::setPickup (int id, bool on)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& m : mappings) if (m.id == id) { m.pickup = on; m.pickedUp = false; }
}

void MidiMapper::setPassThrough (int id, bool on)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& m : mappings) if (m.id == id) m.passThrough = on;
}

juce::String MidiMapper::describeMessage (const Mapping& m) const
{
    juce::String s;
    switch (m.msg)
    {
        case Msg::cc:   s = "CC " + juce::String (m.number); break;
        case Msg::cc14: s = "CC " + juce::String (m.number) + "+" + juce::String (m.number + 32) + " 14-BIT"; break;
        case Msg::note: s = "NOTE " + juce::MidiMessage::getMidiNoteName (m.number, true, true, 3); break;
    }
    if (m.channel != 1) s << " CH" << m.channel;
    return s;
}

juce::String MidiMapper::describeTarget (const Mapping& m) const
{
    if (m.target.plugin)
        return "TRK " + juce::String (m.target.track + 1) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + m.paramName.toUpperCase();
    auto name = controlName (m.target.control);
    return isPerTrack (m.target.control) ? "TRK " + juce::String (m.target.track + 1) + " " + name : name;
}

juce::String MidiMapper::labelFor (const Target& t) const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& m : mappings)
        if (m.target == t)
        {
            switch (m.msg)
            {
                case Msg::cc:   return "CC " + juce::String (m.number);
                case Msg::cc14: return "CC " + juce::String (m.number) + "+";
                case Msg::note: return juce::MidiMessage::getMidiNoteName (m.number, true, true, 3);
            }
        }
    return {};
}

void MidiMapper::addMapping (Mapping m)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    // One message moves one control, and one control has one message.
    mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [&m] (const Mapping& o)
                    {
                        return o.target == m.target
                               || (o.slot == m.slot && o.channel == m.channel && o.number == m.number
                                   && (o.msg == Msg::note) == (m.msg == Msg::note));
                    }), mappings.end());
    if (m.id == 0) m.id = nextId++;
    else nextId = std::max (nextId, m.id + 1);
    if (mappings.size() < mappings.capacity())
        mappings.push_back (m);
}

//==============================================================================
void MidiMapper::beginLearn (const Target& t, juce::AudioProcessorParameter* param, const juce::String& paramName)
{
    learnTarget = t;
    learnParam = param;
    learnParamName = paramName;
    learnSeen.clear();
    learnFirstMs = 0.0;
    learnFifo.reset();
    learning = true;
}

void MidiMapper::cancelLearn()
{
    learning = false;
    learnSeen.clear();
}

void MidiMapper::poll()
{
    if (! learning.load())
        return;

    // Collect what the audio thread caught.
    int s1, n1, s2, n2;
    learnFifo.prepareToRead (learnFifo.getNumReady(), s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) learnSeen.push_back (learnEvents[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) learnSeen.push_back (learnEvents[(size_t) (s2 + i)]);
    learnFifo.finishedRead (n1 + n2);

    if (learnSeen.empty())
        return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (learnFirstMs == 0.0) learnFirstMs = now;

    const auto& first = learnSeen.front();
    const bool isNote = (first.status & 0xf0) == 0x90;
    // A button is known at once; a knob is watched for a moment to see what kind it is.
    if (! isNote && learnSeen.size() < 4 && now - learnFirstMs < 300.0)
        return;

    Mapping m;
    m.slot = first.slot;
    m.portId = hub.inputId (first.slot);
    m.portName = hub.inputName (first.slot);
    m.channel = (first.status & 0x0f) + 1;
    m.target = learnTarget;
    m.param = learnParam;
    m.paramName = learnParamName;
    const bool switchLike = ! learnTarget.plugin && (isSwitch (learnTarget.control) || isCommand (learnTarget.control));

    if (isNote)
    {
        m.msg = Msg::note;
        m.number = first.d1;
        m.mode = Mode::toggle;
    }
    else
    {
        m.number = first.d1;
        std::vector<int> values;
        bool lsbPartner = false;
        for (auto& e : learnSeen)
        {
            if (e.slot != first.slot || e.status != first.status) continue;
            if (e.d1 == first.d1) values.push_back (e.d2);
            if (first.d1 < 32 && e.d1 == first.d1 + 32) lsbPartner = true;
        }
        m.msg = lsbPartner ? Msg::cc14 : Msg::cc;
        m.mode = switchLike ? Mode::toggle : Mode::absolute;

        // Endless encoders send the same small step over and over.
        if (! switchLike && values.size() >= 3 && std::all_of (values.begin(), values.end(), [&] (int v) { return v == values.front(); }))
        {
            auto v = values.front();
            if (v == 63 || v == 65 || v == 62 || v == 66)               m.mode = Mode::relOffset;
            else if ((v >= 1 && v <= 3) || (v >= 125 && v <= 127))       m.mode = Mode::relTwos;
            else if (v >= 65 && v <= 67)                                m.mode = Mode::relSign;
        }
        if (m.mode != Mode::absolute) m.pickup = false;
    }

    learning = false;
    learnSeen.clear();
    addMapping (m);
    if (onLearned) onLearned (m);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> MidiMapper::mappingXml (const Mapping& m) const
{
    auto x = std::make_unique<juce::XmlElement> ("MAP");
    x->setAttribute ("portId", m.portId);
    x->setAttribute ("portName", m.portName);
    x->setAttribute ("channel", m.channel);
    x->setAttribute ("msg", msgIds[(int) m.msg]);
    x->setAttribute ("number", m.number);
    x->setAttribute ("mode", modeIds[(int) m.mode]);
    x->setAttribute ("pickup", m.pickup);
    x->setAttribute ("pass", m.passThrough);
    if (m.target.plugin)
    {
        x->setAttribute ("param", m.target.param);
        x->setAttribute ("paramName", m.paramName);
    }
    else
    {
        x->setAttribute ("control", controlIds[(int) m.target.control]);
        x->setAttribute ("track", m.target.track);
    }
    return x;
}

bool MidiMapper::readMapping (const juce::XmlElement& x, Mapping& m)
{
    m.portId = x.getStringAttribute ("portId");
    m.portName = x.getStringAttribute ("portName");
    m.slot = hub.findOrCreateInput (m.portId, m.portName);
    if (m.slot < 0) return false;
    m.channel = juce::jlimit (1, 16, x.getIntAttribute ("channel", 1));
    m.msg = (Msg) indexOf (msgIds, x.getStringAttribute ("msg"), 0);
    m.number = juce::jlimit (0, 127, x.getIntAttribute ("number"));
    m.mode = (Mode) indexOf (modeIds, x.getStringAttribute ("mode"), 0);
    m.pickup = x.getBoolAttribute ("pickup", true);
    m.passThrough = x.getBoolAttribute ("pass");
    return true;
}

std::unique_ptr<juce::XmlElement> MidiMapper::createDeckState() const
{
    auto x = std::make_unique<juce::XmlElement> ("MIDI_MAPS");
    x->setAttribute ("mmc", followMmc.load());
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& m : mappings)
        if (! m.target.plugin)
            x->addChildElement (mappingXml (m).release());
    return x;
}

void MidiMapper::restoreDeckState (const juce::XmlElement* maps)
{
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        mappings.erase (std::remove_if (mappings.begin(), mappings.end(), [] (const Mapping& m) { return ! m.target.plugin; }), mappings.end());
    }
    if (maps == nullptr) return;
    followMmc = maps->getBoolAttribute ("mmc", true);
    for (auto* x : maps->getChildWithTagNameIterator ("MAP"))
    {
        Mapping m;
        if (! readMapping (*x, m)) continue;
        auto c = indexOf (controlIds, x->getStringAttribute ("control"), -1);
        if (c < 0) continue;
        m.target.control = (Control) c;
        m.target.track = isPerTrack (m.target.control) ? juce::jlimit (0, numTracks - 1, x->getIntAttribute ("track")) : -1;
        addMapping (m);
    }
}

void MidiMapper::writeBlockMappings (juce::XmlElement& block, int track, int blockId) const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& m : mappings)
        if (m.target.plugin && m.target.track == track && m.target.blockId == blockId)
            block.addChildElement (mappingXml (m).release());
}

void MidiMapper::restoreBlockMappings (const juce::XmlElement& block, int track, int blockId, juce::AudioProcessor* processor)
{
    if (processor == nullptr) return;
    auto params = processor->getParameters();
    for (auto* x : block.getChildWithTagNameIterator ("MAP"))
    {
        Mapping m;
        if (! readMapping (*x, m)) continue;
        auto p = x->getIntAttribute ("param", -1);
        if (! juce::isPositiveAndBelow (p, params.size())) continue;
        m.target.plugin = true;
        m.target.track = track;
        m.target.blockId = blockId;
        m.target.param = p;
        m.param = params[p];
        m.paramName = x->getStringAttribute ("paramName", params[p]->getName (40));
        addMapping (m);
    }
}

//==============================================================================
void MidiMapper::apply (Mapping& m, int value, int range, Sink& sink) noexcept
{
    const bool switchLike = ! m.target.plugin && (isSwitch (m.target.control) || isCommand (m.target.control));
    if (switchLike || m.mode == Mode::toggle)
    {
        // Buttons: act on the press, not the release.
        const bool down = value * 2 >= range;
        if (down && ! m.held)
        {
            if (! m.target.plugin && isCommand (m.target.control)) sink.command (m.target.control);
            else sink.toggle (m.target);
        }
        m.held = down;
        return;
    }

    const auto current = sink.getValue (m.target, m.param);
    float v;
    if (m.mode == Mode::absolute)
    {
        v = (float) value / (float) range;
        if (m.pickup)
        {
            // Someone moved the control another way since we last set it: pick up again.
            if (m.pickedUp && m.lastSet >= 0.0f && std::abs (current - m.lastSet) > 0.02f)
                m.pickedUp = false;
            if (! m.pickedUp)
            {
                const bool close = std::abs (v - current) <= 0.025f;
                const bool crossed = m.lastIn >= 0.0f && (m.lastIn - current) * (v - current) <= 0.0f;
                if (! close && ! crossed)
                {
                    m.lastIn = v;
                    pickupWaitingId = m.id;
                    pickupDirection = v < current ? 1 : -1;
                    return;
                }
                m.pickedUp = true;
            }
        }
    }
    else
    {
        int steps = 0;
        if (m.mode == Mode::relTwos)        steps = value < 64 ? value : value - 128;
        else if (m.mode == Mode::relOffset) steps = value - 64;
        else                                steps = (value & 64) ? -(value & 63) : (value & 63);
        v = juce::jlimit (0.0f, 1.0f, current + (float) steps / 100.0f);
    }

    sink.setValue (m.target, m.param, v);
    m.lastSet = v;
    m.lastIn = v;
}

void MidiMapper::process (int slot, juce::MidiBuffer& buffer, Sink& sink) noexcept
{
    if (buffer.isEmpty())
        return;

    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked())
        return;

    const bool learnNow = learning.load() && slot != MidiHub::computerKeyboard;
    const bool mmc = followMmc.load();
    if (mappings.empty() && ! learnNow && ! mmc)
        return;

    scratch.clear();
    bool removed = false;

    for (const auto meta : buffer)
    {
        const auto* d = meta.data;
        bool consumed = false;

        if (meta.numBytes >= 6 && d[0] == 0xf0 && d[1] == 0x7f && d[3] == 0x06)
        {
            // MIDI Machine Control: F0 7F <device> 06 <command> F7
            if (mmc)
            {
                switch (d[4])
                {
                    case 0x01: case 0x09: sink.command (Control::stop); break;
                    case 0x02: case 0x03: sink.command (Control::play); break;
                    case 0x04: sink.command (Control::ff); break;
                    case 0x05: sink.command (Control::rew); break;
                    case 0x06: sink.command (Control::rec); break;
                    case 0x07: sink.command (Control::play); break;   // record exit: punch out, keep playing
                    default: break;
                }
                consumed = true;
            }
        }
        else if (meta.numBytes == 3)
        {
            const int type = d[0] & 0xf0, ch = (d[0] & 0x0f) + 1;
            const bool isCc = type == 0xb0, isOn = type == 0x90 && d[2] > 0, isOff = type == 0x80 || (type == 0x90 && d[2] == 0);

            if (learnNow && (isCc || isOn || isOff))
            {
                if (isCc || isOn)
                {
                    int s1, n1, s2, n2;
                    learnFifo.prepareToWrite (1, s1, n1, s2, n2);
                    if (n1 > 0) { learnEvents[(size_t) s1] = { slot, d[0], d[1], d[2] }; learnFifo.finishedWrite (1); }
                }
                consumed = true;
            }
            else if (isCc || isOn || isOff)
            {
                for (auto& m : mappings)
                {
                    if (m.slot != slot || m.channel != ch) continue;
                    if (m.msg == Msg::note)
                    {
                        if ((isOn || isOff) && d[1] == m.number)
                        {
                            apply (m, isOn ? 127 : 0, 127, sink);
                            consumed = ! m.passThrough;
                        }
                    }
                    else if (isCc)
                    {
                        if (m.msg == Msg::cc && d[1] == m.number)
                        {
                            apply (m, d[2], 127, sink);
                            consumed = ! m.passThrough;
                        }
                        else if (m.msg == Msg::cc14 && d[1] == m.number)
                        {
                            m.msb = d[2];
                            apply (m, m.msb * 128, 16383, sink);
                            consumed = ! m.passThrough;
                        }
                        else if (m.msg == Msg::cc14 && d[1] == m.number + 32)
                        {
                            apply (m, m.msb * 128 + d[2], 16383, sink);
                            consumed = ! m.passThrough;
                        }
                    }
                }
            }
        }

        if (consumed) removed = true;
        else          scratch.addEvent (d, meta.numBytes, meta.samplePosition);
    }

    if (removed)
        buffer.swapWith (scratch);
}
} // namespace tf
