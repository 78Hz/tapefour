#include "MidiHub.h"

namespace tf
{
namespace
{
    juce::Array<juce::MidiDeviceInfo> availableInputs()  { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> availableOutputs() { return juce::MidiOutput::getAvailableDevices(); }

    bool contains (const juce::Array<juce::MidiDeviceInfo>& list, const juce::String& id)
    {
        for (auto& d : list) if (d.identifier == id) return true;
        return false;
    }
}

//==============================================================================
/** Sends each queued message when it is due, about a millisecond at a time. */
class MidiHub::Sender : public juce::Thread
{
public:
    explicit Sender (MidiHub& h) : juce::Thread ("TapeFour MIDI out"), hub (h)
    {
        startThread (juce::Thread::Priority::highest);
    }
    ~Sender() override { stopThread (2000); }

    void run() override
    {
        while (! threadShouldExit())
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            for (auto& op : hub.out)
            {
                auto& o = *op;
                while (o.fifo.getNumReady() > 0)
                {
                    int s1, n1, s2, n2;
                    o.fifo.prepareToRead (1, s1, n1, s2, n2);
                    const auto& e = o.queue[(size_t) s1];
                    if (e.dueMs > now) break;
                    {
                        const juce::ScopedLock sl (o.lock);
                        if (o.device != nullptr)
                            o.device->sendMessageNow (juce::MidiMessage (e.bytes, (int) e.size));
                    }
                    o.fifo.finishedRead (1);
                }
            }
            wait (1);
        }
    }

private:
    MidiHub& hub;
};

//==============================================================================
MidiHub::MidiHub (juce::AudioDeviceManager& dm) : deviceManager (dm)
{
    any.ensureSize (8192);
    // Every slot exists from the start, so the audio thread never sees one appear or vanish.
    for (int k = 0; k < maxPorts; ++k)
    {
        in[k] = std::make_unique<InPort>();
        in[k]->buffer.ensureSize (4096);
        in[k]->collector.reset (48000.0);
        out[k] = std::make_unique<OutPort>();
        out[k]->buffer.ensureSize (4096);
    }
    in[computerKeyboard]->id = "computer-keyboard";
    in[computerKeyboard]->name = "Computer keyboard (A-K)";
    in[computerKeyboard]->active = true;
    sender = std::make_unique<Sender> (*this);
}

MidiHub::~MidiHub()
{
    listConnection = {};
    sender.reset();

    for (auto& p : in)
        if (p->registered)
            deviceManager.removeMidiInputDeviceCallback (p->id, &p->collector);

    for (auto& o : out)
    {
        const juce::ScopedLock sl (o->lock);
        if (o->device != nullptr) allNotesOff (*o->device);
        o->device.reset();
    }
}

void MidiHub::startListening()
{
    listConnection = juce::MidiDeviceListConnection::make ([this] { devicesChanged(); });
}

void MidiHub::allNotesOff (juce::MidiOutput& o)
{
    for (int ch = 1; ch <= 16; ++ch)
        o.sendMessageNow (juce::MidiMessage::allNotesOff (ch));
}

bool MidiHub::looksLikeMpeController (const juce::String& name)
{
    for (auto* key : { "osmose", "seaboard", "linnstrument", "continuum", "erae", "lumi", "sensel", "roli", "joue", "striso" })
        if (name.containsIgnoreCase (key)) return true;
    return false;
}

void MidiHub::addMpeConfiguration (juce::MidiBuffer& b, bool on, int bendRange)
{
    auto cc = [&b] (int ch, int n, int v) { b.addEvent (juce::MidiMessage::controllerEvent (ch, n, v), 0); };
    // MPE Configuration Message: RPN 6 on the master channel = number of member channels (lower zone).
    // Data-entry MSB only: adding CC38 makes some receivers read a 14-bit value (15 -> 1920).
    cc (1, 101, 0); cc (1, 100, 6); cc (1, 6, on ? 15 : 0); cc (1, 101, 127); cc (1, 100, 127);
    if (on)
    {
        // Per-note pitch-bend range: RPN 0 on a member channel applies to all member channels.
        cc (2, 101, 0); cc (2, 100, 0); cc (2, 6, bendRange); cc (2, 101, 127); cc (2, 100, 127);
    }
}

//==============================================================================
void MidiHub::recycleSlot (bool output, int k)
{
    if (output)
    {
        auto& o = *out[k];
        o.active = false;
        std::unique_ptr<juce::MidiOutput> closing;
        {
            const juce::ScopedLock sl (o.lock);
            closing = std::move (o.device);
        }
        o.id = {}; o.name = {}; o.alias = {};
    }
    else
    {
        auto& p = *in[k];
        p.active = false;
        if (p.registered)
        {
            deviceManager.removeMidiInputDeviceCallback (p.id, &p.collector);
            p.registered = false;
        }
        p.id = {}; p.name = {}; p.wasAvailable = false;
    }
}

int MidiHub::findOrCreateInput (const juce::String& id, const juce::String& name)
{
    for (int k = 1; k < maxPorts; ++k)
        if (id.isNotEmpty() && in[k]->id == id) return k;
    for (int k = 1; k < maxPorts; ++k)
        if (name.isNotEmpty() && in[k]->name == name)
        {
            if (id.isNotEmpty()) in[k]->id = id;   // same device, new identifier
            return k;
        }

    auto take = [&] (int k)
    {
        in[k]->id = id.isNotEmpty() ? id : name;
        in[k]->name = name;
        in[k]->mpe = looksLikeMpeController (name);
        in[k]->bendRange = 48;
        return k;
    };
    for (int k = 1; k < maxPorts; ++k)
        if (in[k]->id.isEmpty())
            return take (k);

    // Full: reuse the slot of a controller that isn't plugged in and that no track uses.
    auto ins = availableInputs();
    for (int k = 1; k < maxPorts; ++k)
        if (! contains (ins, in[k]->id) && ! (isSlotInUse && isSlotInUse (k, false)))
        {
            recycleSlot (false, k);
            return take (k);
        }
    return -1;
}

int MidiHub::findOrCreateOutput (const juce::String& id, const juce::String& name)
{
    for (int k = 0; k < maxPorts; ++k)
        if (out[k]->id.isNotEmpty() && out[k]->id == id) return k;
    for (int k = 0; k < maxPorts; ++k)
        if (out[k]->name == name && name.isNotEmpty()) { out[k]->id = id; return k; }

    auto take = [&] (int k)
    {
        out[k]->id = id;
        out[k]->name = name;
        return k;
    };
    for (int k = 0; k < maxPorts; ++k)
        if (out[k]->id.isEmpty())
            return take (k);

    auto outs = availableOutputs();
    for (int k = 0; k < maxPorts; ++k)
        if (! contains (outs, out[k]->id) && ! (isSlotInUse && isSlotInUse (k, true)))
        {
            recycleSlot (true, k);
            return take (k);
        }
    return -1;
}

int MidiHub::adoptNewInputs()
{
    int added = 0;
    for (auto& d : availableInputs())
    {
        bool known = false;
        for (int k = 1; k < maxPorts; ++k)
            known = known || (in[k]->id.isNotEmpty() && (in[k]->id == d.identifier || in[k]->name == d.name));
        auto k = findOrCreateInput (d.identifier, d.name);
        if (! known && k > 0)
        {
            in[k]->active = true;
            ++added;
        }
    }
    return added;
}

void MidiHub::scanForNewDevices()
{
    for (auto& d : availableInputs())  findOrCreateInput (d.identifier, d.name);
    for (auto& d : availableOutputs()) findOrCreateOutput (d.identifier, d.name);
}

void MidiHub::devicesChanged()
{
    // A controller or interface was plugged in or out while running.
    adoptNewInputs();
    for (auto& d : availableOutputs()) findOrCreateOutput (d.identifier, d.name);
    refresh();
    if (onDevicesChanged) onDevicesChanged();
}

void MidiHub::refresh()
{
    auto ins = availableInputs();
    for (int k = 1; k < maxPorts; ++k)
    {
        auto& p = *in[k];
        if (p.id.isEmpty()) continue;
        const bool available = contains (ins, p.id);
        bool want = p.active.load() && available;
        if (want)
        {
            // Back after being unplugged: the old connection is dead, so open it afresh.
            if (! p.wasAvailable && deviceManager.isMidiInputDeviceEnabled (p.id))
                deviceManager.setMidiInputDeviceEnabled (p.id, false);
            deviceManager.setMidiInputDeviceEnabled (p.id, true);
            if (! p.registered)
            {
                p.collector.reset (sampleRate);
                deviceManager.addMidiInputDeviceCallback (p.id, &p.collector);
                p.registered = true;
            }
        }
        else
        {
            if (p.registered)
            {
                deviceManager.removeMidiInputDeviceCallback (p.id, &p.collector);
                p.registered = false;
            }
            if (deviceManager.isMidiInputDeviceEnabled (p.id))
                deviceManager.setMidiInputDeviceEnabled (p.id, false);
        }
        p.wasAvailable = available;
    }

    auto outs = availableOutputs();
    for (auto& op : out)
    {
        auto& o = *op;
        if (o.id.isEmpty()) continue;
        bool want = o.active.load() && contains (outs, o.id);
        if (want && o.device == nullptr)
        {
            auto opened = juce::MidiOutput::openDevice (o.id);
            const juce::ScopedLock sl (o.lock);
            o.device = std::move (opened);
        }
        else if (! want && o.device != nullptr)
        {
            std::unique_ptr<juce::MidiOutput> closing;
            {
                const juce::ScopedLock sl (o.lock);
                closing = std::move (o.device);
            }
            allNotesOff (*closing);
        }
    }
}

void MidiHub::sampleRateChanged (double sr)
{
    sampleRate = sr;
    for (auto& p : in) p->collector.reset (sr);
}

std::vector<MidiHub::InputInfo> MidiHub::getInputs() const
{
    auto ins = availableInputs();
    std::vector<InputInfo> result;
    for (int k = 0; k < maxPorts; ++k)
    {
        auto& p = *in[k];
        if (p.id.isEmpty()) continue;
        result.push_back ({ k, p.id, p.name, k == computerKeyboard || contains (ins, p.id),
                            p.active.load(), p.mpe.load(), p.bendRange.load() });
    }
    return result;
}

std::vector<MidiHub::OutputInfo> MidiHub::getOutputs() const
{
    auto outs = availableOutputs();
    std::vector<OutputInfo> result;
    for (int k = 0; k < maxPorts; ++k)
    {
        auto& o = *out[k];
        if (o.id.isEmpty()) continue;
        result.push_back ({ k, o.id, o.name, o.alias, contains (outs, o.id), o.active.load() });
    }
    return result;
}

void MidiHub::setInputActive (int slot, bool active)
{
    if (! juce::isPositiveAndBelow (slot, maxPorts)) return;
    in[slot]->active = active;
    refresh();
}

void MidiHub::setInputMpe (int slot, bool mpe, int bendRange)
{
    if (! juce::isPositiveAndBelow (slot, maxPorts)) return;
    in[slot]->mpe = mpe;
    in[slot]->bendRange = juce::jlimit (1, 96, bendRange);
}

void MidiHub::setOutputActive (int slot, bool active)
{
    if (! juce::isPositiveAndBelow (slot, maxPorts)) return;
    out[slot]->active = active;
    refresh();
}

void MidiHub::setOutputAlias (int slot, const juce::String& alias)
{
    if (! juce::isPositiveAndBelow (slot, maxPorts)) return;
    out[slot]->alias = alias.trim();
}

juce::String MidiHub::inputName (int slot) const  { return inputExists (slot) ? in[slot]->name : juce::String(); }
juce::String MidiHub::inputId (int slot) const    { return inputExists (slot) ? in[slot]->id : juce::String(); }
juce::String MidiHub::outputId (int slot) const   { return outputExists (slot) ? out[slot]->id : juce::String(); }
juce::String MidiHub::outputName (int slot) const { return outputExists (slot) ? out[slot]->name : juce::String(); }

juce::String MidiHub::outputLabel (int slot) const
{
    if (! outputExists (slot)) return "NO MIDI OUT";
    auto& o = *out[slot];
    return (o.alias.isNotEmpty() ? o.alias : o.name).toUpperCase();
}

bool MidiHub::anyMpeInput() const
{
    for (auto& p : in) if (p->active.load() && p->mpe.load()) return true;
    return false;
}

std::unique_ptr<juce::XmlElement> MidiHub::createState() const
{
    auto hw = std::make_unique<juce::XmlElement> ("MIDI");
    for (int k = 0; k < maxPorts; ++k)
    {
        auto& p = *in[k];
        if (p.id.isEmpty()) continue;
        auto* x = hw->createNewChildElement ("MIDI_IN");
        x->setAttribute ("slot", k);
        x->setAttribute ("id", p.id);
        x->setAttribute ("name", p.name);
        x->setAttribute ("active", p.active.load());
        x->setAttribute ("mpe", p.mpe.load());
        x->setAttribute ("bend", p.bendRange.load());
    }
    for (int k = 0; k < maxPorts; ++k)
    {
        auto& o = *out[k];
        if (o.id.isEmpty()) continue;
        auto* x = hw->createNewChildElement ("MIDI_OUT");
        x->setAttribute ("slot", k);
        x->setAttribute ("id", o.id);
        x->setAttribute ("name", o.name);
        x->setAttribute ("alias", o.alias);
        x->setAttribute ("active", o.active.load());
    }
    return hw;
}

void MidiHub::restoreState (const juce::XmlElement* hw, const juce::XmlElement* legacyMidi)
{
    if (hw != nullptr)
    {
        for (auto* x : hw->getChildWithTagNameIterator ("MIDI_IN"))
        {
            auto k = x->getIntAttribute ("slot", -1);
            if (! juce::isPositiveAndBelow (k, maxPorts)) continue;
            auto& p = *in[k];
            if (k != computerKeyboard)
            {
                p.id = x->getStringAttribute ("id");
                p.name = x->getStringAttribute ("name");
            }
            p.active = x->getBoolAttribute ("active");
            p.mpe = x->getBoolAttribute ("mpe");
            p.bendRange = juce::jlimit (1, 96, x->getIntAttribute ("bend", 48));
        }
        for (auto* x : hw->getChildWithTagNameIterator ("MIDI_OUT"))
        {
            auto k = x->getIntAttribute ("slot", -1);
            if (! juce::isPositiveAndBelow (k, maxPorts)) continue;
            auto& o = *out[k];
            o.id = x->getStringAttribute ("id");
            o.name = x->getStringAttribute ("name");
            o.alias = x->getStringAttribute ("alias");
            o.active = x->getBoolAttribute ("active");
        }
        // Controllers plugged in for the first time come in switched on; synth outputs wait to be ticked.
        adoptNewInputs();
        for (auto& d : availableOutputs()) findOrCreateOutput (d.identifier, d.name);
    }
    else
    {
        // First run, or settings from before per-track MIDI: carry over what was enabled.
        for (auto& d : availableInputs())
        {
            auto k = findOrCreateInput (d.identifier, d.name);
            if (k < 0) continue;
            bool on = legacyMidi == nullptr;
            if (legacyMidi != nullptr)
                for (auto* x : legacyMidi->getChildWithTagNameIterator ("INPUT"))
                    if (x->getStringAttribute ("id") == d.identifier || x->getStringAttribute ("name") == d.name)
                        on = x->getBoolAttribute ("enabled");
            in[k]->active = on;
        }
        for (auto& d : availableOutputs())
            findOrCreateOutput (d.identifier, d.name);
    }
    refresh();
}

void MidiHub::inject (const juce::MidiMessage& m, int slot)
{
    if (! juce::isPositiveAndBelow (slot, maxPorts)) return;
    auto& p = *in[slot];
    if (! p.active.load()) return;
    auto msg = m;
    msg.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    p.collector.addMessageToQueue (msg);
}

int MidiHub::addVirtualInput (const juce::String& name)
{
    auto slot = findOrCreateInput ("virtual:" + name, name);
    if (slot >= 0) in[slot]->active = true;
    return slot;
}

int MidiHub::addVirtualOutput (const juce::String& name)
{
    auto slot = findOrCreateOutput ("virtual:" + name, name);
    if (slot >= 0) out[slot]->active = true;
    return slot;
}

//==============================================================================
void MidiHub::beginBlock (int n) noexcept
{
    any.clear();
    anyMpe = false;
    anyBend = 48;
    for (auto& pp : in)
    {
        auto& p = *pp;
        p.buffer.clear();
        if (! p.active.load()) continue;
        p.collector.removeNextBlockOfMessages (p.buffer, n);
        if (! p.buffer.isEmpty())
        {
            any.addEvents (p.buffer, 0, n, 0);
            inFlash = true;
        }
        if (p.mpe.load() && ! anyMpe) { anyMpe = true; anyBend = p.bendRange.load(); }
    }
    for (auto& o : out) o->buffer.clear();
}

void MidiHub::endBlock (double blockStartMs, double sr, int) noexcept
{
    for (int k = 0; k < maxPorts; ++k)
    {
        auto& o = *out[k];
        if (o.buffer.isEmpty()) continue;
        if (outTap) outTap (k, o.buffer);
        if (! o.active.load()) continue;

        for (const auto meta : o.buffer)
        {
            // Only short messages go to synths; system exclusive from a controller is not forwarded.
            if (meta.numBytes < 1 || meta.numBytes > 3) continue;
            int s1, n1, s2, n2;
            o.fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 < 1) break;   // queue full: the sender has stalled; drop rather than wait
            auto& e = o.queue[(size_t) s1];
            e.dueMs = blockStartMs + meta.samplePosition * 1000.0 / sr;
            e.size = (juce::uint8) meta.numBytes;
            std::memcpy (e.bytes, meta.data, (size_t) meta.numBytes);
            o.fifo.finishedWrite (1);
        }
        outFlash = true;
    }
}
} // namespace tf
