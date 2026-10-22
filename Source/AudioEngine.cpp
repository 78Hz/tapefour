#include "AudioEngine.h"
#include "PluginScan.h"

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

AudioEngine::AudioEngine (juce::PropertiesFile& settings)
    : props (settings)
{
    juce::addDefaultFormatsToManager (formatManager);
    knownPlugins.setCustomScanner (PluginScan::makeOutOfProcessScanner());

    if (auto known = props.getXmlValue ("knownPlugins"))
        knownPlugins.recreateFromXml (*known);

    for (int t = 0; t < numTracks; ++t)
    {
        tapeOut[t].assign ((size_t) maxBlock, 0.0f);
        src[t].assign ((size_t) maxBlock, 0.0f);
        chan[t].assign ((size_t) maxBlock, 0.0f);
        tapeOutPtr[t] = tapeOut[t].data();
    }
    mixL.assign ((size_t) maxBlock, 0.0f);
    mixR.assign ((size_t) maxBlock, 0.0f);
    instBuffer.setSize (2, maxBlock);
    anyMidi.ensureSize (8192);
    trackMidi.ensureSize (8192);

    // All MIDI slots exist from the start, so the audio thread never sees one appear or vanish.
    for (int k = 0; k < maxMidiPorts; ++k)
    {
        midiPorts[k] = std::make_unique<MidiPort>();
        midiPorts[k]->buffer.ensureSize (4096);
        midiPorts[k]->collector.reset (48000.0);
        midiOuts[k] = std::make_unique<MidiOutPort>();
        midiOuts[k]->buffer.ensureSize (4096);
    }
    midiPorts[computerKeyboardPort]->id = "computer-keyboard";
    midiPorts[computerKeyboardPort]->name = "Computer keyboard (A-K)";
    midiPorts[computerKeyboardPort]->active = true;

    auto saved = props.getXmlValue ("audioDevice");
    deviceManager.initialise (8, 8, saved.get(), true);

    if (saved != nullptr)
    {
        // If the saved driver couldn't be opened (switched off, in use by another program),
        // JUCE falls back to a default device. Keep the user's choice on disk until they pick again.
        auto wantType = saved->getStringAttribute ("deviceType");
        auto wantName = saved->getStringAttribute ("audioOutputDeviceName");
        if (wantName.isEmpty()) wantName = saved->getStringAttribute ("audioInputDeviceName");
        auto* dev = deviceManager.getCurrentAudioDevice();
        bool ok = dev != nullptr
                  && (wantType.isEmpty() || deviceManager.getCurrentAudioDeviceType() == wantType)
                  && (wantName.isEmpty() || dev->getName() == wantName);
        if (! ok)
        {
            restoringDevice = true;
            startupProblem = "COULD NOT OPEN " + (wantName.isNotEmpty() ? wantName : wantType).toUpperCase();
        }
    }
    else
    {
        // First run: prefer ASIO when a driver is installed, then open every input and output.
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            if (type->getTypeName() == "ASIO")
            {
                type->scanForDevices();
                if (! type->getDeviceNames().isEmpty())
                {
                    deviceManager.setCurrentAudioDeviceType ("ASIO", true);
                    if (deviceManager.getCurrentAudioDevice() == nullptr)
                        deviceManager.initialise (8, 8, nullptr, true);
                }
                break;
            }
        }

        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.useDefaultInputChannels = false;
            setup.useDefaultOutputChannels = false;
            setup.inputChannels.clear();
            setup.outputChannels.clear();
            setup.inputChannels.setRange (0, std::min (16, dev->getInputChannelNames().size()), true);
            setup.outputChannels.setRange (0, std::min (16, dev->getOutputChannelNames().size()), true);
            deviceManager.setAudioDeviceSetup (setup, true);
        }
    }

    recallDeckWithTape = props.getBoolValue ("recallDeckWithTape", false);
    restoreAll();
    stateAtStartup = deviceManager.createStateXml();
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    midiListConnection = juce::MidiDeviceListConnection::make ([this] { midiDevicesChanged(); });
    startingUp = false;
    saveSettings();
}

AudioEngine::~AudioEngine()
{
    midiListConnection = {};
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);

    for (auto& p : midiPorts)
        if (p->registered)
            deviceManager.removeMidiInputDeviceCallback (p->id, &p->collector);

    for (auto& o : midiOuts)
    {
        if (o->out != nullptr)
            for (int ch = 1; ch <= 16; ++ch)
                o->out->sendMessageNow (juce::MidiMessage::allNotesOff (ch));
        o->out.reset();
    }

    for (auto& s : slots)
        s.inst.reset();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Any change the user makes in Audio Setup is written to disk straight away.
    if (restoringDevice)
    {
        // Only an audio change counts: MIDI devices coming and going mustn't overwrite the saved interface.
        auto audioOnly = [] (std::unique_ptr<juce::XmlElement> x)
        {
            if (x != nullptr)
            {
                x->deleteAllChildElementsWithTagName ("MIDIINPUT");
                x->removeAttribute ("defaultMidiOutput");
                x->removeAttribute ("defaultMidiOutputDevice");
            }
            return x;
        };
        auto now = audioOnly (deviceManager.createStateXml());
        auto then = audioOnly (stateAtStartup != nullptr ? std::make_unique<juce::XmlElement> (*stateAtStartup) : nullptr);
        bool changed = (now == nullptr) != (then == nullptr)
                       || (now != nullptr && ! now->isEquivalentTo (then.get(), true));
        if (! changed) return;
        restoringDevice = false;
        startupProblem = {};
    }
    saveSettings();
}

void AudioEngine::saveDeviceState()
{
    if (restoringDevice) return;
    if (auto dev = deviceManager.createStateXml())
        props.setValue ("audioDevice", dev.get());
}

//==============================================================================
// MIDI controllers in, hardware synths out
//==============================================================================
bool AudioEngine::looksLikeMpeController (const juce::String& name)
{
    for (auto* key : { "osmose", "seaboard", "linnstrument", "continuum", "erae", "lumi", "sensel", "roli", "joue", "striso" })
        if (name.containsIgnoreCase (key)) return true;
    return false;
}

int AudioEngine::findOrCreateInputSlot (const juce::String& id, const juce::String& name)
{
    for (int k = 1; k < maxMidiPorts; ++k)
        if (id.isNotEmpty() && midiPorts[k]->id == id) return k;
    for (int k = 1; k < maxMidiPorts; ++k)
        if (name.isNotEmpty() && midiPorts[k]->name == name)
        {
            if (id.isNotEmpty()) midiPorts[k]->id = id;   // same device, new identifier
            return k;
        }
    for (int k = 1; k < maxMidiPorts; ++k)
        if (midiPorts[k]->id.isEmpty())
        {
            midiPorts[k]->id = id.isNotEmpty() ? id : name;
            midiPorts[k]->name = name;
            midiPorts[k]->mpe = looksLikeMpeController (name);
            midiPorts[k]->bendRange = 48;
            return k;
        }
    return -1;
}

int AudioEngine::findOrCreateOutputSlot (const juce::String& id, const juce::String& name)
{
    for (int k = 0; k < maxMidiPorts; ++k)
        if (midiOuts[k]->id.isNotEmpty() && midiOuts[k]->id == id) return k;
    for (int k = 0; k < maxMidiPorts; ++k)
        if (midiOuts[k]->name == name && name.isNotEmpty()) { midiOuts[k]->id = id; return k; }
    for (int k = 0; k < maxMidiPorts; ++k)
        if (midiOuts[k]->id.isEmpty())
        {
            midiOuts[k]->id = id;
            midiOuts[k]->name = name;
            return k;
        }
    return -1;
}

int AudioEngine::adoptNewMidiInputs()
{
    int added = 0;
    for (auto& d : availableInputs())
    {
        bool known = false;
        for (int k = 1; k < maxMidiPorts; ++k)
            known = known || (midiPorts[k]->id.isNotEmpty() && (midiPorts[k]->id == d.identifier || midiPorts[k]->name == d.name));
        auto k = findOrCreateInputSlot (d.identifier, d.name);
        if (! known && k > 0)
        {
            midiPorts[k]->active = true;
            ++added;
        }
    }
    return added;
}

void AudioEngine::midiDevicesChanged()
{
    // A controller or interface was plugged in or out while running.
    adoptNewMidiInputs();
    for (auto& d : availableOutputs()) findOrCreateOutputSlot (d.identifier, d.name);
    refreshMidiDevices();
    for (int t = 0; t < numTracks; ++t) mpeConfigPending[t] = true;
    saveSettings();
}

void AudioEngine::refreshMidiDevices()
{
    auto ins = availableInputs();
    for (int k = 1; k < maxMidiPorts; ++k)
    {
        auto& p = *midiPorts[k];
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
                p.collector.reset (sampleRate > 0 ? sampleRate : 48000.0);
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
    for (auto& op : midiOuts)
    {
        auto& o = *op;
        if (o.id.isEmpty()) continue;
        bool want = o.active.load() && contains (outs, o.id);
        if (want && o.out == nullptr)
        {
            auto opened = juce::MidiOutput::openDevice (o.id);
            const juce::SpinLock::ScopedLockType sl (o.lock);
            o.out = std::move (opened);
        }
        else if (! want && o.out != nullptr)
        {
            std::unique_ptr<juce::MidiOutput> closing;
            {
                const juce::SpinLock::ScopedLockType sl (o.lock);
                closing = std::move (o.out);
            }
            for (int ch = 1; ch <= 16; ++ch)
                closing->sendMessageNow (juce::MidiMessage::allNotesOff (ch));
        }
    }
}

std::vector<AudioEngine::MidiInputInfo> AudioEngine::getMidiInputs() const
{
    // Make sure every connected controller has a slot.
    auto ins = availableInputs();
    for (auto& d : ins)
        const_cast<AudioEngine*> (this)->findOrCreateInputSlot (d.identifier, d.name);

    std::vector<MidiInputInfo> out;
    for (int k = 0; k < maxMidiPorts; ++k)
    {
        auto& p = *midiPorts[k];
        if (p.id.isEmpty()) continue;
        out.push_back ({ k, p.id, p.name, k == computerKeyboardPort || contains (ins, p.id),
                         p.active.load(), p.mpe.load(), p.bendRange.load() });
    }
    return out;
}

void AudioEngine::setMidiInputActive (int slot, bool active)
{
    if (! juce::isPositiveAndBelow (slot, maxMidiPorts)) return;
    midiPorts[slot]->active = active;
    refreshMidiDevices();
    for (int t = 0; t < numTracks; ++t) mpeConfigPending[t] = true;
    saveSettings();
}

void AudioEngine::setMidiInputMpe (int slot, bool mpe, int bendRange)
{
    if (! juce::isPositiveAndBelow (slot, maxMidiPorts)) return;
    midiPorts[slot]->mpe = mpe;
    midiPorts[slot]->bendRange = juce::jlimit (1, 96, bendRange);
    for (int t = 0; t < numTracks; ++t) mpeConfigPending[t] = true;
    saveSettings();
}

juce::String AudioEngine::midiPortLabel (int port) const
{
    if (port == midiAnyWhenArmed) return "ANY";
    if (port == midiNone || ! juce::isPositiveAndBelow (port, maxMidiPorts) || midiPorts[port]->id.isEmpty()) return "NONE";
    if (port == computerKeyboardPort) return "COMPUTER KEYS";
    return midiPorts[port]->name.toUpperCase();
}

bool AudioEngine::isMidiPortMpe (int port) const
{
    if (port == midiAnyWhenArmed)
    {
        for (auto& p : midiPorts) if (p->active.load() && p->mpe.load()) return true;
        return false;
    }
    return juce::isPositiveAndBelow (port, maxMidiPorts) && midiPorts[port]->mpe.load();
}

std::vector<AudioEngine::MidiOutputInfo> AudioEngine::getMidiOutputs() const
{
    auto outs = availableOutputs();
    for (auto& d : outs)
        const_cast<AudioEngine*> (this)->findOrCreateOutputSlot (d.identifier, d.name);

    std::vector<MidiOutputInfo> result;
    for (int k = 0; k < maxMidiPorts; ++k)
    {
        auto& o = *midiOuts[k];
        if (o.id.isEmpty()) continue;
        result.push_back ({ k, o.id, o.name, o.alias, contains (outs, o.id), o.active.load() });
    }
    return result;
}

void AudioEngine::setMidiOutputActive (int slot, bool active)
{
    if (! juce::isPositiveAndBelow (slot, maxMidiPorts)) return;
    midiOuts[slot]->active = active;
    refreshMidiDevices();
    saveSettings();
}

void AudioEngine::setMidiOutputAlias (int slot, const juce::String& alias)
{
    if (! juce::isPositiveAndBelow (slot, maxMidiPorts)) return;
    midiOuts[slot]->alias = alias.trim();
    saveSettings();
}

juce::String AudioEngine::midiOutputLabel (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, maxMidiPorts) || midiOuts[slot]->id.isEmpty()) return "NO MIDI OUT";
    auto& o = *midiOuts[slot];
    return (o.alias.isNotEmpty() ? o.alias : o.name).toUpperCase();
}

void AudioEngine::addMpeConfiguration (juce::MidiBuffer& b, bool on, int bendRange)
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

int AudioEngine::addVirtualMidiInput (const juce::String& name)
{
    auto slot = findOrCreateInputSlot ("virtual:" + name, name);
    if (slot >= 0) midiPorts[slot]->active = true;
    return slot;
}

int AudioEngine::addVirtualMidiOutput (const juce::String& name)
{
    auto slot = findOrCreateOutputSlot ("virtual:" + name, name);
    if (slot >= 0) midiOuts[slot]->active = true;
    return slot;
}

void AudioEngine::injectMidi (const juce::MidiMessage& m, int port)
{
    if (! juce::isPositiveAndBelow (port, maxMidiPorts)) return;
    auto& p = *midiPorts[port];
    if (sampleRate <= 0 || ! p.active.load()) return;
    auto msg = m;
    msg.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    p.collector.addMessageToQueue (msg);
}

//==============================================================================
// Sources
//==============================================================================
std::unique_ptr<Tape> AudioEngine::swapTape (std::unique_ptr<Tape> newTape)
{
    const juce::SpinLock::ScopedLockType sl (tapeLock);
    auto old = std::move (tape);
    tape = std::move (newTape);
    tapeEngine.setTape (tape.get());
    return old;
}

void AudioEngine::setAudioSource (int t, int inputChannel, bool pair)
{
    setInstrument (t, nullptr);
    track[t].inputChannel = inputChannel;
    track[t].inputPair = pair;
    track[t].source = (int) Source::audio;
}

void AudioEngine::setHardwareSource (int t)
{
    setInstrument (t, nullptr);
    track[t].source = (int) Source::hardware;
}

void AudioEngine::unplug (int t)
{
    setInstrument (t, nullptr);
    track[t].source = (int) Source::none;
}

void AudioEngine::setTrackMidiPort (int t, int port)
{
    track[t].midiPort = port;
    mpeConfigPending[t] = true;
}

void AudioEngine::setInstrument (int t, std::unique_ptr<Instrument> inst)
{
    pendingInstrument[t].reset();
    pluginMissing[t] = false;
    if (inst != nullptr && sampleRate > 0)
        inst->prepare (sampleRate, maxBlock);

    std::unique_ptr<Instrument> old;
    {
        const juce::SpinLock::ScopedLockType sl (slots[t].lock);
        old = std::move (slots[t].inst);
        slots[t].inst = std::move (inst);
    }

    mpeConfigPending[t] = true;   // tell the new instrument about the MPE zone
    if (slots[t].inst != nullptr)
        track[t].source = (int) Source::instrument;
}

void AudioEngine::loadPlugin (int t, const juce::PluginDescription& desc, const juce::String& base64State,
                              std::function<void (const juce::String&)> done)
{
    auto sr = sampleRate > 0 ? sampleRate : 48000.0;
    auto bs = bufferSize > 0 ? bufferSize : 512;

    formatManager.createPluginInstanceAsync (desc, sr, bs,
        [this, t, base64State, done] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& err)
        {
            if (p == nullptr)
            {
                pluginMissing[t] = pendingInstrument[t] != nullptr;
                if (done) done (err.isNotEmpty() ? err : juce::String ("The plug-in could not be loaded."));
                return;
            }

            if (base64State.isNotEmpty())
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (base64State))
                    p->setStateInformation (mb.getData(), (int) mb.getSize());
            }

            setInstrument (t, std::make_unique<PluginInstrument> (std::move (p)));
            saveSettings();
            if (done) done ({});
        });
}

void AudioEngine::toggleBounceSource (int t, int sourceTrack)
{
    if (sourceTrack == t) return;
    auto wasBounce = track[t].source.load() == (int) Source::bounce;
    if (! wasBounce) setInstrument (t, nullptr);
    auto mask = wasBounce ? track[t].bounceMask.load() : 0;
    mask ^= (1 << sourceTrack);
    track[t].bounceMask = mask;
    track[t].source = (int) Source::bounce;
}

juce::String AudioEngine::describeInput (int t) const
{
    auto ch = track[t].inputChannel.load() + 1;
    return "IN " + juce::String (ch) + (track[t].inputPair.load() ? "+" + juce::String (ch + 1) : juce::String());
}

juce::String AudioEngine::describeSource (int t) const
{
    switch ((Source) track[t].source.load())
    {
        case Source::audio:
            return "ASIO " + describeInput (t);
        case Source::hardware:
            return midiOutputLabel (track[t].midiOutPort.load());
        case Source::instrument:
            if (slots[t].inst != nullptr) return slots[t].inst->getName().toUpperCase();
            if (pendingInstrument[t] != nullptr)
            {
                auto name = pendingInstrument[t]->getChildByName ("PLUGIN") != nullptr
                                ? pendingInstrument[t]->getChildByName ("PLUGIN")->getStringAttribute ("name") : juce::String ("PLUG-IN");
                return pluginMissing[t] ? "MISSING: " + name.toUpperCase() : "LOADING " + name.toUpperCase();
            }
            return "NO INSTRUMENT";
        case Source::bounce:
        {
            juce::StringArray names;
            for (int s = 0; s < numTracks; ++s)
                if (track[t].bounceMask.load() & (1 << s))
                    names.add (juce::String (s + 1));
            return names.isEmpty() ? juce::String ("BOUNCE: NO TRACKS") : "BOUNCE " + names.joinIntoString ("+");
        }
        case Source::none: default:
            return "NO SOURCE";
    }
}

juce::String AudioEngine::getDeviceSummary() const
{
    auto* dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
        return "NO AUDIO DEVICE";

    auto sr = dev->getCurrentSampleRate();
    auto bs = dev->getCurrentBufferSizeSamples();
    auto rt = sr > 0 ? (dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples()) / sr * 1000.0 : 0.0;
    return deviceManager.getCurrentAudioDeviceType().toUpperCase() + " " + juce::String (sr / 1000.0, 1).trimCharactersAtEnd (".0")
           + "k " + juce::String (bs) + " " + juce::String (rt, 1) + "ms";
}

juce::StringArray AudioEngine::getActiveInputNames() const
{
    juce::StringArray out;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        auto names = dev->getInputChannelNames();
        auto active = dev->getActiveInputChannels();
        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                out.add (names[i]);
    }
    return out;
}

juce::StringArray AudioEngine::getOutputPairNames() const
{
    juce::StringArray out;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        auto names = dev->getOutputChannelNames();
        auto active = dev->getActiveOutputChannels();
        juce::StringArray act;
        for (int i = 0; i < names.size(); ++i)
            if (active[i])
                act.add (names[i]);
        for (int i = 0; i < act.size(); i += 2)
            out.add ("OUT " + juce::String (i + 1) + (i + 1 < act.size() ? "/" + juce::String (i + 2) : juce::String())
                     + "  (" + act[i] + ")");
    }
    return out;
}

//==============================================================================
// Deck state and settings
//==============================================================================
std::unique_ptr<juce::XmlElement> AudioEngine::createDeckState() const
{
    auto deck = std::make_unique<juce::XmlElement> ("DECK");
    deck->setAttribute ("version", 2);

    for (int t = 0; t < numTracks; ++t)
    {
        auto& p = track[t];
        auto* x = deck->createNewChildElement ("TRACK");
        x->setAttribute ("index", t);
        x->setAttribute ("source", p.source.load());
        x->setAttribute ("input", p.inputChannel.load());
        x->setAttribute ("pair", p.inputPair.load());
        x->setAttribute ("gain", p.gain.load());
        x->setAttribute ("high", p.high.load());
        x->setAttribute ("mid", p.mid.load());
        x->setAttribute ("midFreq", p.midFreq.load());
        x->setAttribute ("low", p.low.load());
        x->setAttribute ("pan", p.pan.load());
        x->setAttribute ("fader", p.fader.load());
        x->setAttribute ("bounce", p.bounceMask.load());

        auto port = p.midiPort.load();
        if (port == midiAnyWhenArmed)      x->setAttribute ("midiIn", "any");
        else if (! juce::isPositiveAndBelow (port, maxMidiPorts) || midiPorts[port]->id.isEmpty()) x->setAttribute ("midiIn", "none");
        else { x->setAttribute ("midiIn", "port"); x->setAttribute ("midiInId", midiPorts[port]->id); x->setAttribute ("midiInName", midiPorts[port]->name); }

        auto outPort = p.midiOutPort.load();
        if (juce::isPositiveAndBelow (outPort, maxMidiPorts) && midiOuts[outPort]->id.isNotEmpty())
        {
            x->setAttribute ("midiOutId", midiOuts[outPort]->id);
            x->setAttribute ("midiOutName", midiOuts[outPort]->name);
        }
        x->setAttribute ("midiOutChannel", p.midiOutChannel.load());

        if (slots[t].inst != nullptr)
            x->addChildElement (slots[t].inst->saveState().release());
        else if (pendingInstrument[t] != nullptr)
            x->addChildElement (new juce::XmlElement (*pendingInstrument[t]));
    }

    auto* m = deck->createNewChildElement ("MASTER");
    m->setAttribute ("fader", masterFader.load());
    m->setAttribute ("phones", phones.load());
    m->setAttribute ("monitor", monitorMode.load());

    auto& tp = tapeEngine.params;
    auto* x = deck->createNewChildElement ("TAPE_DECK");
    x->setAttribute ("speed", tp.speedSel.load());
    x->setAttribute ("pitch", tp.pitchStep.load());
    x->setAttribute ("nr", tp.nrOn.load());
    x->setAttribute ("nrType", tp.nrType.load());
    x->setAttribute ("hiss", tp.hiss.load());
    x->setAttribute ("wow", tp.wow.load());
    x->setAttribute ("flutter", tp.flutter.load());
    x->setAttribute ("sat", tp.sat.load());
    x->setAttribute ("hf", tp.hf.load());
    x->setAttribute ("xtalk", tp.xtalk.load());
    x->setAttribute ("drop", tp.drop.load());
    x->setAttribute ("drift", tp.drift.load());
    return deck;
}

void AudioEngine::applyDeckState (const juce::XmlElement& deck)
{
    for (auto* x : deck.getChildWithTagNameIterator ("TRACK"))
    {
        auto t = x->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;
        auto& p = track[t];
        p.inputChannel = x->getIntAttribute ("input", t);
        p.inputPair = x->getBoolAttribute ("pair");
        p.gain  = (float) x->getDoubleAttribute ("gain", 1.0 / 3.0);
        p.high  = (float) x->getDoubleAttribute ("high");
        p.mid   = (float) x->getDoubleAttribute ("mid");
        p.midFreq = (float) x->getDoubleAttribute ("midFreq", defaultMidFreq);
        p.low   = (float) x->getDoubleAttribute ("low");
        p.pan   = (float) x->getDoubleAttribute ("pan");
        p.fader = (float) x->getDoubleAttribute ("fader", 0.75);
        p.bounceMask = x->getIntAttribute ("bounce");

        auto midiIn = x->getStringAttribute ("midiIn", "any");
        if (midiIn == "none") p.midiPort = midiNone;
        else if (midiIn == "port")
        {
            auto slot = findOrCreateInputSlot (x->getStringAttribute ("midiInId"), x->getStringAttribute ("midiInName"));
            p.midiPort = slot >= 0 ? slot : midiNone;
        }
        else p.midiPort = midiAnyWhenArmed;

        if (x->hasAttribute ("midiOutId"))
            p.midiOutPort = findOrCreateOutputSlot (x->getStringAttribute ("midiOutId"), x->getStringAttribute ("midiOutName"));
        else
            p.midiOutPort = -1;
        p.midiOutChannel = juce::jlimit (0, 16, x->getIntAttribute ("midiOutChannel", 1));

        auto source = x->getIntAttribute ("source");
        if (source == (int) Source::instrument)
        {
            if (auto* ix = x->getChildByName ("INSTRUMENT"))
            {
                if (ix->getStringAttribute ("kind") == "builtin")
                {
                    setInstrument (t, BuiltIn::create (ix->getIntAttribute ("which")));
                }
                else if (auto* dx = ix->getChildByName ("PLUGIN"))
                {
                    juce::PluginDescription desc;
                    if (desc.loadFromXml (*dx))
                    {
                        setInstrument (t, nullptr);
                        p.source = (int) Source::instrument;
                        pendingInstrument[t] = std::make_unique<juce::XmlElement> (*ix);
                        loadPlugin (t, desc, ix->getStringAttribute ("state"), nullptr);
                    }
                }
            }
        }
        else
        {
            setInstrument (t, nullptr);
            p.source = source;
        }
        mpeConfigPending[t] = true;
    }

    if (auto* m = deck.getChildByName ("MASTER"))
    {
        masterFader = (float) m->getDoubleAttribute ("fader", 0.75);
        phones      = (float) m->getDoubleAttribute ("phones", 0.6);
        monitorMode = m->getIntAttribute ("monitor");
    }

    if (auto* x = deck.getChildByName ("TAPE_DECK"))
    {
        auto& tp = tapeEngine.params;
        tp.speedSel    = x->getIntAttribute ("speed", 1);
        tp.pitchStep   = x->getIntAttribute ("pitch");
        tp.nrOn        = x->getBoolAttribute ("nr", true);
        tp.nrType      = x->getIntAttribute ("nrType");
        tp.hiss    = (float) x->getDoubleAttribute ("hiss", 0.55);
        tp.wow     = (float) x->getDoubleAttribute ("wow", 0.30);
        tp.flutter = (float) x->getDoubleAttribute ("flutter", 0.25);
        tp.sat     = (float) x->getDoubleAttribute ("sat", 0.40);
        tp.hf      = (float) x->getDoubleAttribute ("hf", 0.20);
        tp.xtalk   = (float) x->getDoubleAttribute ("xtalk", 0.30);
        tp.drop    = (float) x->getDoubleAttribute ("drop", 0.10);
        tp.drift   = (float) x->getDoubleAttribute ("drift", 0.20);
    }

    if (onDeckStateApplied) onDeckStateApplied();
}

std::unique_ptr<juce::XmlElement> AudioEngine::createHardwareState() const
{
    auto hw = std::make_unique<juce::XmlElement> ("HARDWARE");
    hw->setAttribute ("mainOut", mainOutPair.load());
    hw->setAttribute ("phonesOut", phonesPair.load());
    hw->setAttribute ("latencyOffsetMs", tapeEngine.params.latencyOffsetMs.load());

    for (int k = 0; k < maxMidiPorts; ++k)
    {
        auto& p = *midiPorts[k];
        if (p.id.isEmpty()) continue;
        auto* x = hw->createNewChildElement ("MIDI_IN");
        x->setAttribute ("slot", k);
        x->setAttribute ("id", p.id);
        x->setAttribute ("name", p.name);
        x->setAttribute ("active", p.active.load());
        x->setAttribute ("mpe", p.mpe.load());
        x->setAttribute ("bend", p.bendRange.load());
    }
    for (int k = 0; k < maxMidiPorts; ++k)
    {
        auto& o = *midiOuts[k];
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

void AudioEngine::restoreHardware (const juce::XmlElement* hw, const juce::XmlElement* legacyDeck)
{
    if (hw != nullptr)
    {
        mainOutPair = hw->getIntAttribute ("mainOut");
        phonesPair  = hw->getIntAttribute ("phonesOut", -1);
        tapeEngine.params.latencyOffsetMs = hw->getIntAttribute ("latencyOffsetMs");

        for (auto* x : hw->getChildWithTagNameIterator ("MIDI_IN"))
        {
            auto k = x->getIntAttribute ("slot", -1);
            if (! juce::isPositiveAndBelow (k, maxMidiPorts)) continue;
            auto& p = *midiPorts[k];
            if (k != computerKeyboardPort)
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
            if (! juce::isPositiveAndBelow (k, maxMidiPorts)) continue;
            auto& o = *midiOuts[k];
            o.id = x->getStringAttribute ("id");
            o.name = x->getStringAttribute ("name");
            o.alias = x->getStringAttribute ("alias");
            o.active = x->getBoolAttribute ("active");
        }
        // Controllers plugged in for the first time come in switched on; synth outputs wait to be ticked.
        adoptNewMidiInputs();
        for (auto& d : availableOutputs()) findOrCreateOutputSlot (d.identifier, d.name);
    }
    else
    {
        // First run, or settings from before per-track MIDI: carry over what was enabled.
        const juce::XmlElement* legacyMidi = legacyDeck != nullptr ? legacyDeck->getChildByName ("MIDI_INPUTS") : nullptr;
        for (auto& d : availableInputs())
        {
            auto k = findOrCreateInputSlot (d.identifier, d.name);
            if (k < 0) continue;
            bool on = legacyMidi == nullptr;
            if (legacyMidi != nullptr)
                for (auto* x : legacyMidi->getChildWithTagNameIterator ("INPUT"))
                    if (x->getStringAttribute ("id") == d.identifier || x->getStringAttribute ("name") == d.name)
                        on = x->getBoolAttribute ("enabled");
            midiPorts[k]->active = on;
        }
        for (auto& d : availableOutputs())
            findOrCreateOutputSlot (d.identifier, d.name);

        if (legacyDeck != nullptr)
        {
            if (auto* m = legacyDeck->getChildByName ("MASTER"))
            {
                mainOutPair = m->getIntAttribute ("mainOut");
                phonesPair  = m->getIntAttribute ("phonesOut", -1);
            }
            if (auto* x = legacyDeck->getChildByName ("TAPE_DECK"))
                tapeEngine.params.latencyOffsetMs = x->getIntAttribute ("latencyOffsetMs");
        }
    }
    refreshMidiDevices();
}

void AudioEngine::restoreAll()
{
    auto hw = props.getXmlValue ("hardware");
    auto deck = props.getXmlValue ("deck");
    restoreHardware (hw.get(), deck.get());

    if (deck != nullptr)
    {
        applyDeckState (*deck);
        return;
    }

    // Factory setup: two built-in instruments and two audio inputs.
    setInstrument (0, BuiltIn::create (BuiltIn::poly));
    setInstrument (1, BuiltIn::create (BuiltIn::epiano));
    setAudioSource (2, 0);
    setAudioSource (3, 1);
}

void AudioEngine::saveSettings()
{
    if (startingUp) return;
    saveDeviceState();

    if (auto known = knownPlugins.createXml())
        props.setValue ("knownPlugins", known.get());

    if (auto deck = createDeckState()) props.setValue ("deck", deck.get());
    if (auto hw = createHardwareState()) props.setValue ("hardware", hw.get());
    props.setValue ("recallDeckWithTape", recallDeckWithTape.load());
    props.saveIfNeeded();
}

//==============================================================================
// Audio
//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    sampleRate = dev->getCurrentSampleRate();
    bufferSize = dev->getCurrentBufferSizeSamples();
    deviceLatency = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();

    auto needed = std::max (8192, bufferSize * 2);
    if (needed > maxBlock)
    {
        maxBlock = needed;
        for (int t = 0; t < numTracks; ++t)
        {
            tapeOut[t].assign ((size_t) maxBlock, 0.0f);
            src[t].assign ((size_t) maxBlock, 0.0f);
            chan[t].assign ((size_t) maxBlock, 0.0f);
            tapeOutPtr[t] = tapeOut[t].data();
        }
        mixL.assign ((size_t) maxBlock, 0.0f);
        mixR.assign ((size_t) maxBlock, 0.0f);
        instBuffer.setSize (2, maxBlock);
    }

    {
        const juce::SpinLock::ScopedLockType sl (tapeLock);
        tapeEngine.prepare (sampleRate, maxBlock);
        tapeEngine.setTape (tape.get(), false);   // same tape: leave the switch where it is
    }
    for (auto& p : midiPorts) p->collector.reset (sampleRate);
    for (auto& p : mpeConfigPending) p = true;

    for (int t = 0; t < numTracks; ++t)
    {
        for (auto& b : eq[t]) b.reset();
        for (auto& s : eqState[t]) s = 99.0f;
        const juce::SpinLock::ScopedLockType sl (slots[t].lock);
        if (slots[t].inst != nullptr)
            slots[t].inst->prepare (sampleRate, maxBlock);
    }
}

void AudioEngine::audioDeviceStopped()
{
}

void AudioEngine::updateEq (int t)
{
    auto l = track[t].low.load(), m = track[t].mid.load(), h = track[t].high.load(), mf = track[t].midFreq.load();
    // Shelves voiced like a cassette portastudio's: LOW is at full strength by 100 Hz (half at 250 Hz),
    // HIGH at nearly full strength by 10 kHz (half at 4.5 kHz), so both are heard well inside the tape's range.
    if (l != eqState[t][0]) { eq[t][0].setLowShelf (sampleRate, 250.0, l * 12.0); eqState[t][0] = l; }
    if (m != eqState[t][1] || mf != eqState[t][3])
    {
        eq[t][1].setPeak (sampleRate, midFreqHz (mf), 0.8, m * 12.0);
        eqState[t][1] = m;
        eqState[t][3] = mf;
    }
    if (h != eqState[t][2]) { eq[t][2].setHighShelf (sampleRate, 4500.0, h * 12.0); eqState[t][2] = h; }
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int n, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOutputs; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], n);

    if (n > maxBlock || n <= 0 || sampleRate <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    // ---- gather each controller's MIDI ----
    anyMidi.clear();
    bool anyMpe = false;
    int anyBend = 48;
    for (auto& pp : midiPorts)
    {
        auto& p = *pp;
        p.buffer.clear();
        if (! p.active.load()) continue;
        p.collector.removeNextBlockOfMessages (p.buffer, n);
        if (! p.buffer.isEmpty())
        {
            anyMidi.addEvents (p.buffer, 0, n, 0);
            midiFlash = true;
        }
        if (p.mpe.load() && ! anyMpe) { anyMpe = true; anyBend = p.bendRange.load(); }
    }
    for (auto& o : midiOuts) o->buffer.clear();

    bool armed[numTracks];
    int source[numTracks];
    for (int t = 0; t < numTracks; ++t)
    {
        armed[t] = track[t].armed.load();
        source[t] = track[t].source.load();
    }

    const juce::SpinLock::ScopedTryLockType tapeGuard (tapeLock);
    const bool haveTape = tapeGuard.isLocked() && tape != nullptr;

    if (haveTape)
    {
        tapeEngine.beginBlock (n, armed, deviceLatency);
        tapeEngine.readAll (tapeOutPtr, n);
    }
    else
    {
        for (auto& v : tapeOut) std::fill_n (v.begin(), n, 0.0f);
    }

    const auto mode = haveTape ? tapeEngine.getMode() : Mode::stop;

    // Which controller plays this track, and whether it speaks MPE.
    auto controllerFor = [&] (int t, bool& mpe, int& bend) -> const juce::MidiBuffer*
    {
        auto port = track[t].midiPort.load();
        if (port == midiAnyWhenArmed)
        {
            mpe = anyMpe; bend = anyBend;
            return armed[t] ? &anyMidi : nullptr;
        }
        // An instrument or synth only plays when its track is armed; the choice of controller
        // just says which keyboard that is.
        if (armed[t] && juce::isPositiveAndBelow (port, maxMidiPorts) && midiPorts[(size_t) port]->active.load())
        {
            mpe = midiPorts[(size_t) port]->mpe.load();
            bend = midiPorts[(size_t) port]->bendRange.load();
            return &midiPorts[(size_t) port]->buffer;
        }
        mpe = false; bend = 48;
        return nullptr;
    };

    auto copyInput = [&] (int t, float* s)
    {
        auto ch = track[t].inputChannel.load();
        if (ch < 0 || ch >= numInputs || inputs[ch] == nullptr) return;
        juce::FloatVectorOperations::copy (s, inputs[ch], n);
        if (track[t].inputPair.load() && ch + 1 < numInputs && inputs[ch + 1] != nullptr)
        {
            juce::FloatVectorOperations::add (s, inputs[ch + 1], n);
            juce::FloatVectorOperations::multiply (s, 0.5f, n);   // mono sum
        }
    };

    // ---- what is plugged into each track ----
    for (int t = 0; t < numTracks; ++t)
    {
        auto* s = src[t].data();
        std::fill_n (s, n, 0.0f);

        bool mpe = false;
        int bend = 48;
        const auto* midi = controllerFor (t, mpe, bend);
        const bool receives = midi != nullptr;

        // Hardware synth: forward the controller to its MIDI output, notes-off when the route changes.
        int route = -1;
        if (source[t] == (int) Source::hardware)
        {
            auto outPort = track[t].midiOutPort.load();
            if (juce::isPositiveAndBelow (outPort, maxMidiPorts) && midiOuts[(size_t) outPort]->active.load())
                route = outPort * 17 + track[t].midiOutChannel.load();
        }
        if (route != lastOutRoute[t] || (wasReceiving[t] && ! receives))
        {
            if (lastOutRoute[t] >= 0)
                for (int ch = 1; ch <= 16; ++ch)
                    midiOuts[(size_t) (lastOutRoute[t] / 17)]->buffer.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            lastOutRoute[t] = route;
        }

        if (source[t] == (int) Source::audio)
        {
            copyInput (t, s);
        }
        else if (source[t] == (int) Source::hardware)
        {
            copyInput (t, s);
            if (route >= 0 && receives)
            {
                auto& out = midiOuts[(size_t) (route / 17)]->buffer;
                auto ch = track[t].midiOutChannel.load();
                for (const auto meta : *midi)
                {
                    auto msg = meta.getMessage();
                    // MPE needs every note on its own channel, so it passes through untouched.
                    if (! mpe && ch >= 1 && msg.getChannel() > 0)
                        msg.setChannel (ch);
                    out.addEvent (msg, meta.samplePosition);
                }
                if (! midi->isEmpty()) track[t].midiActivity = true;
            }
        }
        else if (source[t] == (int) Source::instrument)
        {
            trackMidi.clear();
            if (mpe != lastMpe[t] || bend != lastBend[t])
            {
                lastMpe[t] = mpe;
                lastBend[t] = bend;
                mpeConfigPending[t] = true;
            }
            if (mpeConfigPending[t].exchange (false))
                addMpeConfiguration (trackMidi, mpe, bend);
            if (receives)
                trackMidi.addEvents (*midi, 0, n, 0);
            if (wasReceiving[t] && ! receives)
                for (int ch = 1; ch <= 16; ++ch)
                    trackMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);

            if (receives && ! midi->isEmpty())
                track[t].midiActivity = true;

            instBuffer.clear (0, n);
            {
                const juce::SpinLock::ScopedTryLockType sl (slots[t].lock);
                if (sl.isLocked() && slots[t].inst != nullptr)
                {
                    slots[t].inst->applyMpe (mpe, bend);
                    slots[t].inst->render (instBuffer, n, trackMidi);
                }
            }
            auto* l = instBuffer.getReadPointer (0);
            auto* r = instBuffer.getReadPointer (1);
            for (int i = 0; i < n; ++i)
                s[i] = (l[i] + r[i]) * 0.5f;
        }

        wasReceiving[t] = receives;
    }

    // ---- MIDI out to hardware synths ----
    for (int k = 0; k < maxMidiPorts; ++k)
    {
        auto& o = *midiOuts[k];
        if (o.buffer.isEmpty()) continue;
        if (midiOutTap) midiOutTap (k, o.buffer);
        const juce::SpinLock::ScopedTryLockType sl (o.lock);
        if (sl.isLocked() && o.out != nullptr)
        {
            o.out->sendBlockOfMessagesNow (o.buffer);
            midiOutFlash = true;
        }
    }

    // ---- channel strips ----
    bool monitorInput[numTracks];
    for (int t = 0; t < numTracks; ++t)
        monitorInput[t] = armed[t];   // ARM is the live switch: armed tracks play their input, unarmed ones the tape

    auto processChannel = [&] (int t, const float* in)
    {
        updateEq (t);
        auto trim = dbToGain (-20.0f + 60.0f * track[t].gain.load());
        auto fg = faderGain (track[t].fader.load());
        auto* out = chan[t].data();
        auto& e = eq[t];
        // Trim only applies to live inputs; the tape comes back at the level it was recorded.
        bool live = monitorInput[t];
        auto t0 = live ? lastTrim[t] : 1.0f, t1 = live ? trim : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            auto g = t0 + (t1 - t0) * (float) i / (float) n;
            out[i] = e[2].process (e[1].process (e[0].process (in[i] * g))) * fg;
        }
        lastTrim[t] = trim;
    };

    for (int t = 0; t < numTracks; ++t)
        if (! (monitorInput[t] && source[t] == (int) Source::bounce))
            processChannel (t, monitorInput[t] ? src[t].data() : tapeOut[t].data());

    for (int t = 0; t < numTracks; ++t)
    {
        if (! (monitorInput[t] && source[t] == (int) Source::bounce))
            continue;

        auto* s = src[t].data();
        std::fill_n (s, n, 0.0f);
        auto mask = track[t].bounceMask.load();
        for (int k = 0; k < numTracks; ++k)
            if (k != t && (mask & (1 << k)) && ! (monitorInput[k] && source[k] == (int) Source::bounce))
                juce::FloatVectorOperations::add (s, chan[k].data(), n);
        processChannel (t, s);
    }

    // ---- onto the tape ----
    if (haveTape)
    {
        if (tapeEngine.isRecordingNow())
            for (int t = 0; t < numTracks; ++t)
                if (armed[t])
                    tapeEngine.writeTrack (t, chan[t].data(), n);
        tapeEngine.endBlock();
    }

    // ---- stereo mix ----
    std::fill_n (mixL.begin(), n, 0.0f);
    std::fill_n (mixR.begin(), n, 0.0f);

    for (int t = 0; t < numTracks; ++t)
    {
        auto* c = chan[t].data();
        float peak = 0;
        for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (c[i]));
        if (peak > track[t].meter.load()) track[t].meter = peak;

        // A bounce destination is fed by the other tracks, which you already hear.
        if (monitorInput[t] && source[t] == (int) Source::bounce)
            continue;

        auto angle = (track[t].pan.load() + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        auto gl = std::cos (angle), gr = std::sin (angle);
        for (int i = 0; i < n; ++i)
        {
            auto f = (float) i / (float) n;
            mixL[(size_t) i] += c[i] * (lastGainL[t] + (gl - lastGainL[t]) * f);
            mixR[(size_t) i] += c[i] * (lastGainR[t] + (gr - lastGainR[t]) * f);
        }
        lastGainL[t] = gl;
        lastGainR[t] = gr;
    }

    auto mg = faderGain (masterFader.load());
    auto pg = std::pow (phones.load() / 0.6f, 2.0f);
    float pl = 0, pr = 0;
    auto mainPair = mainOutPair.load(), phonesPairIdx = phonesPair.load();

    for (int i = 0; i < n; ++i)
    {
        auto f = (float) i / (float) n;
        auto g = lastMaster + (mg - lastMaster) * f;
        auto l = mixL[(size_t) i] * g, r = mixR[(size_t) i] * g;
        pl = std::max (pl, std::abs (l));
        pr = std::max (pr, std::abs (r));

        auto ml = mainPair * 2, mr = mainPair * 2 + 1;
        if (ml < numOutputs && outputs[ml] != nullptr) outputs[ml][i] = l;
        if (mr < numOutputs && outputs[mr] != nullptr) outputs[mr][i] = r;

        if (phonesPairIdx >= 0 && phonesPairIdx != mainPair)
        {
            auto ph = lastPhones + (pg - lastPhones) * f;
            auto hl = phonesPairIdx * 2, hr = phonesPairIdx * 2 + 1;
            if (hl < numOutputs && outputs[hl] != nullptr) outputs[hl][i] = l * ph;
            if (hr < numOutputs && outputs[hr] != nullptr) outputs[hr][i] = r * ph;
        }
    }
    lastMaster = mg;
    lastPhones = pg;

    if (pl > meterL.load()) meterL = pl;
    if (pr > meterR.load()) meterR = pr;
}
} // namespace tf
