#include "AudioEngine.h"

namespace tf
{
AudioEngine::AudioEngine (juce::PropertiesFile& settings)
    : props (settings)
{
    juce::addDefaultFormatsToManager (formatManager);

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
    midiIn.ensureSize (8192);
    trackMidi.ensureSize (8192);

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

        for (auto& d : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
    }

    deviceManager.addMidiInputDeviceCallback ({}, &midiCollector);
    restoreSettings();
    stateAtStartup = deviceManager.createStateXml();
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    saveSettings();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Any change the user makes in Audio Setup is written to disk straight away.
    if (restoringDevice)
    {
        auto now = deviceManager.createStateXml();
        bool changed = (now == nullptr) != (stateAtStartup == nullptr)
                       || (now != nullptr && ! now->isEquivalentTo (stateAtStartup.get(), true));
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

void AudioEngine::setMidiInputEnabled (const juce::String& id, bool enabled)
{
    deviceManager.setMidiInputDeviceEnabled (id, enabled);
    saveSettings();
}

void AudioEngine::setMpe (bool enabled, int bendRange)
{
    mpeEnabled = enabled;
    mpeBendRange = juce::jlimit (1, 96, bendRange);
    for (auto& p : mpeConfigPending) p = true;
    saveSettings();
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

AudioEngine::~AudioEngine()
{
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, &midiCollector);
    for (auto& s : slots)
        s.inst.reset();
}

//==============================================================================
std::unique_ptr<Tape> AudioEngine::swapTape (std::unique_ptr<Tape> newTape)
{
    const juce::SpinLock::ScopedLockType sl (tapeLock);
    auto old = std::move (tape);
    tape = std::move (newTape);
    tapeEngine.setTape (tape.get());
    return old;
}

void AudioEngine::setAudioSource (int t, int inputChannel)
{
    pendingInstrument[t].reset();
    pluginMissing[t] = false;
    track[t].inputChannel = inputChannel;
    track[t].source = (int) Source::audio;
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
    pendingInstrument[t].reset();
    pluginMissing[t] = false;
    auto mask = track[t].source.load() == (int) Source::bounce ? track[t].bounceMask.load() : 0;
    mask ^= (1 << sourceTrack);
    track[t].bounceMask = mask;
    track[t].source = (int) Source::bounce;
}

juce::String AudioEngine::describeSource (int t) const
{
    switch ((Source) track[t].source.load())
    {
        case Source::audio:
            return "ASIO IN " + juce::String (track[t].inputChannel.load() + 1);
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
        for (int i = 0; i + 1 < act.size() || i < act.size(); i += 2)
            out.add ("OUT " + juce::String (i + 1) + (i + 1 < act.size() ? "/" + juce::String (i + 2) : juce::String())
                     + "  (" + act[i] + ")");
    }
    return out;
}

void AudioEngine::injectMidi (const juce::MidiMessage& m)
{
    if (sampleRate <= 0) return;
    auto msg = m;
    msg.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    midiCollector.addMessageToQueue (msg);
}

//==============================================================================
void AudioEngine::saveSettings()
{
    saveDeviceState();

    if (auto known = knownPlugins.createXml())
        props.setValue ("knownPlugins", known.get());

    juce::XmlElement deck ("DECK");
    for (int t = 0; t < numTracks; ++t)
    {
        auto& p = track[t];
        auto* x = deck.createNewChildElement ("TRACK");
        x->setAttribute ("index", t);
        x->setAttribute ("source", p.source.load());
        x->setAttribute ("input", p.inputChannel.load());
        x->setAttribute ("gain", p.gain.load());
        x->setAttribute ("high", p.high.load());
        x->setAttribute ("mid", p.mid.load());
        x->setAttribute ("low", p.low.load());
        x->setAttribute ("pan", p.pan.load());
        x->setAttribute ("fader", p.fader.load());
        x->setAttribute ("bounce", p.bounceMask.load());
        if (slots[t].inst != nullptr)
            x->addChildElement (slots[t].inst->saveState().release());
        else if (pendingInstrument[t] != nullptr)
            x->addChildElement (new juce::XmlElement (*pendingInstrument[t]));
    }

    auto* m = deck.createNewChildElement ("MASTER");
    m->setAttribute ("fader", masterFader.load());
    m->setAttribute ("phones", phones.load());
    m->setAttribute ("mainOut", mainOutPair.load());
    m->setAttribute ("phonesOut", phonesPair.load());
    m->setAttribute ("monitor", monitorMode.load());
    m->setAttribute ("followArm", followArm.load());
    m->setAttribute ("mpe", mpeEnabled.load());
    m->setAttribute ("mpeBendRange", mpeBendRange.load());

    auto* midi = deck.createNewChildElement ("MIDI_INPUTS");
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        auto* x = midi->createNewChildElement ("INPUT");
        x->setAttribute ("id", d.identifier);
        x->setAttribute ("name", d.name);
        x->setAttribute ("enabled", deviceManager.isMidiInputDeviceEnabled (d.identifier));
    }

    auto& tp = tapeEngine.params;
    auto* x = deck.createNewChildElement ("TAPE_DECK");
    x->setAttribute ("speed", tp.speedSel.load());
    x->setAttribute ("pitch", tp.pitchStep.load());
    x->setAttribute ("nr", tp.nrOn.load());
    x->setAttribute ("nrType", tp.nrType.load());
    x->setAttribute ("formulation", tp.formulation.load());
    x->setAttribute ("hiss", tp.hiss.load());
    x->setAttribute ("wow", tp.wow.load());
    x->setAttribute ("flutter", tp.flutter.load());
    x->setAttribute ("sat", tp.sat.load());
    x->setAttribute ("hf", tp.hf.load());
    x->setAttribute ("xtalk", tp.xtalk.load());
    x->setAttribute ("drop", tp.drop.load());
    x->setAttribute ("drift", tp.drift.load());
    x->setAttribute ("latencyOffsetMs", tp.latencyOffsetMs.load());

    props.setValue ("deck", &deck);
    props.saveIfNeeded();
}

void AudioEngine::restoreSettings()
{
    auto deck = props.getXmlValue ("deck");

    if (deck == nullptr)
    {
        // Factory setup: two built-in instruments and two audio inputs.
        setInstrument (0, BuiltIn::create (BuiltIn::poly));
        setInstrument (1, BuiltIn::create (BuiltIn::epiano));
        setAudioSource (2, 0);
        setAudioSource (3, 1);
        return;
    }

    for (auto* x : deck->getChildWithTagNameIterator ("TRACK"))
    {
        auto t = x->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;
        auto& p = track[t];
        p.inputChannel = x->getIntAttribute ("input", t);
        p.gain  = (float) x->getDoubleAttribute ("gain", 1.0 / 3.0);
        p.high  = (float) x->getDoubleAttribute ("high");
        p.mid   = (float) x->getDoubleAttribute ("mid");
        p.low   = (float) x->getDoubleAttribute ("low");
        p.pan   = (float) x->getDoubleAttribute ("pan");
        p.fader = (float) x->getDoubleAttribute ("fader", 0.75);
        p.bounceMask = x->getIntAttribute ("bounce");
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
                        p.source = (int) Source::instrument;
                        pendingInstrument[t] = std::make_unique<juce::XmlElement> (*ix);
                        loadPlugin (t, desc, ix->getStringAttribute ("state"), nullptr);
                    }
                }
            }
        }
        else
        {
            p.source = source;
        }
    }

    if (auto* m = deck->getChildByName ("MASTER"))
    {
        masterFader = (float) m->getDoubleAttribute ("fader", 0.75);
        phones      = (float) m->getDoubleAttribute ("phones", 0.6);
        mainOutPair = m->getIntAttribute ("mainOut");
        phonesPair  = m->getIntAttribute ("phonesOut", -1);
        monitorMode = m->getIntAttribute ("monitor");
        followArm   = m->getBoolAttribute ("followArm", true);
        mpeEnabled  = m->getBoolAttribute ("mpe", true);
        mpeBendRange = juce::jlimit (1, 96, m->getIntAttribute ("mpeBendRange", 48));
    }

    // MIDI inputs: exactly the ones that were chosen last time. Devices never seen before stay off.
    if (auto* midi = deck->getChildByName ("MIDI_INPUTS"))
    {
        for (auto& d : juce::MidiInput::getAvailableDevices())
        {
            bool enabled = false;
            for (auto* x : midi->getChildWithTagNameIterator ("INPUT"))
                if (x->getStringAttribute ("id") == d.identifier || x->getStringAttribute ("name") == d.name)
                    enabled = x->getBoolAttribute ("enabled");
            deviceManager.setMidiInputDeviceEnabled (d.identifier, enabled);
        }
    }

    if (auto* x = deck->getChildByName ("TAPE_DECK"))
    {
        auto& tp = tapeEngine.params;
        tp.speedSel    = x->getIntAttribute ("speed", 1);
        tp.pitchStep   = x->getIntAttribute ("pitch");
        tp.nrOn        = x->getBoolAttribute ("nr", true);
        tp.nrType      = x->getIntAttribute ("nrType");
        tp.formulation = x->getIntAttribute ("formulation");
        tp.hiss    = (float) x->getDoubleAttribute ("hiss", 0.55);
        tp.wow     = (float) x->getDoubleAttribute ("wow", 0.30);
        tp.flutter = (float) x->getDoubleAttribute ("flutter", 0.25);
        tp.sat     = (float) x->getDoubleAttribute ("sat", 0.40);
        tp.hf      = (float) x->getDoubleAttribute ("hf", 0.20);
        tp.xtalk   = (float) x->getDoubleAttribute ("xtalk", 0.30);
        tp.drop    = (float) x->getDoubleAttribute ("drop", 0.10);
        tp.drift   = (float) x->getDoubleAttribute ("drift", 0.20);
        tp.latencyOffsetMs = x->getIntAttribute ("latencyOffsetMs");
    }
}

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
        tapeEngine.setTape (tape.get());
    }
    midiCollector.reset (sampleRate);
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
    auto l = track[t].low.load(), m = track[t].mid.load(), h = track[t].high.load();
    if (l != eqState[t][0]) { eq[t][0].setLowShelf (sampleRate, 100.0, l * 12.0); eqState[t][0] = l; }
    if (m != eqState[t][1]) { eq[t][1].setPeak (sampleRate, 1200.0, 0.8, m * 12.0); eqState[t][1] = m; }
    if (h != eqState[t][2]) { eq[t][2].setHighShelf (sampleRate, 10000.0, h * 12.0); eqState[t][2] = h; }
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

    midiIn.clear();
    midiCollector.removeNextBlockOfMessages (midiIn, n);
    if (! midiIn.isEmpty())
        midiFlash = true;

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

    // ---- who hears the keyboard ----
    bool receives[numTracks] {};
    const bool mpeOn = mpeEnabled.load();
    const int bendRange = mpeBendRange.load();
    // MPE spreads one keyboard across 15 channels, so it always goes to the armed track.
    const bool follow = followArm.load() || mpeOn;
    if (follow)
    {
        bool any = false;
        for (int t = 0; t < numTracks; ++t)
            if (armed[t] && source[t] == (int) Source::instrument) { receives[t] = true; any = true; }

        auto f = focusedTrack.load();
        if (! any && f >= 0 && f < numTracks && source[f] == (int) Source::instrument)
            receives[f] = true;
    }
    else
    {
        for (int t = 0; t < numTracks; ++t)
            receives[t] = source[t] == (int) Source::instrument;
    }

    // ---- what is plugged into each track ----
    for (int t = 0; t < numTracks; ++t)
    {
        auto* s = src[t].data();
        std::fill_n (s, n, 0.0f);

        if (source[t] == (int) Source::audio)
        {
            auto ch = track[t].inputChannel.load();
            if (ch >= 0 && ch < numInputs && inputs[ch] != nullptr)
                juce::FloatVectorOperations::copy (s, inputs[ch], n);
        }
        else if (source[t] == (int) Source::instrument)
        {
            trackMidi.clear();
            if (mpeConfigPending[t].exchange (false))
                addMpeConfiguration (trackMidi, mpeOn, bendRange);
            if (receives[t])
            {
                if (follow)
                    trackMidi.addEvents (midiIn, 0, n, 0);
                else
                    for (const auto meta : midiIn)
                    {
                        auto msg = meta.getMessage();
                        if (msg.getChannel() == t + 1)
                            trackMidi.addEvent (msg, meta.samplePosition);
                    }
            }
            if (wasReceiving[t] && ! receives[t])
                for (int ch = 1; ch <= 16; ++ch)
                    trackMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            wasReceiving[t] = receives[t];

            if (receives[t] && ! trackMidi.isEmpty())
                track[t].midiActivity = true;

            instBuffer.clear (0, n);
            {
                const juce::SpinLock::ScopedTryLockType sl (slots[t].lock);
                if (sl.isLocked() && slots[t].inst != nullptr)
                {
                    slots[t].inst->applyMpe (mpeOn, bendRange);
                    slots[t].inst->render (instBuffer, n, trackMidi);
                }
            }
            auto* l = instBuffer.getReadPointer (0);
            auto* r = instBuffer.getReadPointer (1);
            for (int i = 0; i < n; ++i)
                s[i] = (l[i] + r[i]) * 0.5f;
        }
    }

    // ---- channel strips ----
    bool monitorInput[numTracks];
    for (int t = 0; t < numTracks; ++t)
        monitorInput[t] = armed[t] && (mode != Mode::play || monitorMode.load() == 1);

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
