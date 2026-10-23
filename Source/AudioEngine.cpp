#include "AudioEngine.h"
#include "PluginScan.h"

namespace tf
{
double AudioEngine::recordLatencySamples (Source s, int inLat, int outLat, double userOffsetSamples) noexcept
{
    switch (s)
    {
        // Played along to what you heard, then came back in through the interface.
        case Source::audio:
        case Source::hardware:   return inLat + outLat + userOffsetSamples;
        // Played along to what you heard; generated here, so no input latency.
        case Source::instrument: return outLat;
        // Straight off the tape, in step with it.
        case Source::bounce:     return 0.0;
        case Source::none: default: return inLat + outLat + userOffsetSamples;
    }
}

AudioEngine::AudioEngine (juce::PropertiesFile& settings)
    : props (settings)
{
    juce::addDefaultFormatsToManager (formatManager);
    knownPlugins.setCustomScanner (PluginScan::makeOutOfProcessScanner());

    if (auto known = props.getXmlValue ("knownPlugins"))
        knownPlugins.recreateFromXml (*known);

    allocateBuffers (maxBlock);
    trackMidi.ensureSize (8192);

    midi.isSlotInUse = [this] (int slot, bool output) { return isMidiSlotInUse (slot, output); };
    midi.onDevicesChanged = [this]
    {
        for (auto& p : mpeConfigPending) p = true;
        saveSettings();
    };

    openStartupDevice();

    recallDeckWithTape = props.getBoolValue ("recallDeckWithTape", false);
    restoreAll();
    stateAtStartup = deviceManager.createStateXml();
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    midi.startListening();
    startingUp = false;
    saveSettings();
}

AudioEngine::~AudioEngine()
{
    *alive = false;   // plug-ins still loading arrive to nothing
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);

    for (int t = 0; t < numTracks; ++t)
    {
        if (onInstrumentChanging && slots[t].inst != nullptr) onInstrumentChanging (t);
        slots[t].inst.reset();
    }
}

void AudioEngine::allocateBuffers (int size)
{
    for (int t = 0; t < numTracks; ++t)
    {
        tapeOut[t].assign ((size_t) size, 0.0f);
        src[t].assign ((size_t) size, 0.0f);
        chan[t].assign ((size_t) size, 0.0f);
        tapeOutPtr[t] = tapeOut[t].data();
    }
    mixL.assign ((size_t) size, 0.0f);
    mixR.assign ((size_t) size, 0.0f);
    instBuffer.setSize (2, size);
}

void AudioEngine::openStartupDevice()
{
    auto saved = props.getXmlValue ("audioDevice");
    deviceManager.initialise (16, 16, saved.get(), true);

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
        return;
    }

   #if JUCE_WINDOWS
    // First run on Windows: ASIO if a driver is installed (lowest latency), otherwise the system's
    // own Windows Audio. Every other driver type stays one click away in Setup.
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == "ASIO")
        {
            type->scanForDevices();
            if (! type->getDeviceNames().isEmpty())
            {
                deviceManager.setCurrentAudioDeviceType ("ASIO", true);
                if (deviceManager.getCurrentAudioDevice() == nullptr)
                    deviceManager.initialise (16, 16, nullptr, true);
            }
            break;
        }
    }
   #endif
    // macOS starts on CoreAudio's default device and Linux on ALSA's (JACK is offered when a JACK server runs).

    // Open every input and output the interface has, up to 16 of each.
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
// MIDI
//==============================================================================
bool AudioEngine::isMidiSlotInUse (int slot, bool output) const
{
    for (auto& p : track)
        if ((output ? p.midiOutPort.load() : p.midiPort.load()) == slot)
            return true;
    return false;
}

std::vector<AudioEngine::MidiInputInfo> AudioEngine::getMidiInputs()
{
    midi.scanForNewDevices();
    return midi.getInputs();
}

std::vector<AudioEngine::MidiOutputInfo> AudioEngine::getMidiOutputs()
{
    midi.scanForNewDevices();
    return midi.getOutputs();
}

void AudioEngine::setMidiInputActive (int slot, bool active)
{
    midi.setInputActive (slot, active);
    for (auto& p : mpeConfigPending) p = true;
    saveSettings();
}

void AudioEngine::setMidiInputMpe (int slot, bool mpe, int bendRange)
{
    midi.setInputMpe (slot, mpe, bendRange);
    for (auto& p : mpeConfigPending) p = true;
    saveSettings();
}

void AudioEngine::setMidiOutputActive (int slot, bool active)
{
    midi.setOutputActive (slot, active);
    saveSettings();
}

void AudioEngine::setMidiOutputAlias (int slot, const juce::String& alias)
{
    midi.setOutputAlias (slot, alias);
    saveSettings();
}

juce::String AudioEngine::midiPortLabel (int port) const
{
    if (port == midiAny) return "ANY";
    if (port == midiNone || ! midi.inputExists (port)) return "NONE";
    if (port == computerKeyboardPort) return "COMPUTER KEYS";
    return midi.inputName (port).toUpperCase();
}

bool AudioEngine::isMidiPortMpe (int port) const
{
    return port == midiAny ? midi.anyMpeInput() : midi.isInputMpe (port);
}

void AudioEngine::injectMidi (const juce::MidiMessage& m, int port)
{
    if (sampleRate > 0)
        midi.inject (m, port);
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
    // Whatever was loading for this track is now out of date.
    ++loadGeneration[t];
    pendingInstrument[t].reset();
    pluginMissing[t] = false;
    if (inst != nullptr && sampleRate > 0)
        inst->prepare (sampleRate, bufferSize > 0 ? bufferSize : 512);

    // Its window (if open) goes before the plug-in does.
    if (slots[t].inst != nullptr && onInstrumentChanging)
        onInstrumentChanging (t);

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
    const auto generation = ++loadGeneration[t];
    std::weak_ptr<bool> engineAlive = alive;

    formatManager.createPluginInstanceAsync (desc, sr, bs,
        [this, t, generation, engineAlive, base64State, done] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& err)
        {
            if (engineAlive.expired())
                return;

            // Something else was chosen for this track while this one loaded: throw it away.
            if (generation != loadGeneration[t])
            {
                if (done) done ("superseded");
                return;
            }

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
            saveSettings (true);
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
            return "AUDIO " + describeInput (t);
        case Source::hardware:
            return midi.outputLabel (track[t].midiOutPort.load());
        case Source::instrument:
            if (slots[t].inst != nullptr) return slots[t].inst->getName().toUpperCase();
            if (pendingInstrument[t] != nullptr)
            {
                auto* px = pendingInstrument[t]->getChildByName ("PLUGIN");
                auto name = px != nullptr ? px->getStringAttribute ("name") : juce::String ("PLUG-IN");
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
    // Short names for the driver types, so the summary fits the display.
    auto type = deviceManager.getCurrentAudioDeviceType();
    if (type.startsWith ("Windows Audio")) type = type.contains ("Exclusive") ? "WASAPI EXCL" : type.contains ("Low Latency") ? "WASAPI LL" : "WASAPI";
    else if (type == "DirectSound") type = "DSOUND";
    return type.toUpperCase() + " " + juce::String (sr / 1000.0, 1).trimCharactersAtEnd (".0")
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
// Audio
//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    sampleRate = dev->getCurrentSampleRate();
    bufferSize = dev->getCurrentBufferSizeSamples();
    inputLatency = dev->getInputLatencyInSamples();
    outputLatency = dev->getOutputLatencyInSamples();

    auto needed = std::max (8192, bufferSize * 2);
    if (needed > maxBlock)
    {
        maxBlock = needed;
        allocateBuffers (maxBlock);
    }

    {
        const juce::SpinLock::ScopedLockType sl (tapeLock);
        tapeEngine.prepare (sampleRate, maxBlock);
        tapeEngine.setTape (tape.get(), false);   // same tape: leave the switch where it is
    }
    midi.sampleRateChanged (sampleRate);
    for (auto& p : mpeConfigPending) p = true;

    for (int t = 0; t < numTracks; ++t)
    {
        strips[t].prepare (sampleRate);
        const juce::SpinLock::ScopedLockType sl (slots[t].lock);
        if (slots[t].inst != nullptr)
            slots[t].inst->prepare (sampleRate, std::max (bufferSize, 32));
    }
}

void AudioEngine::audioDeviceStopped()
{
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int n, const juce::AudioIODeviceCallbackContext&)
{
    const auto blockStartMs = juce::Time::getMillisecondCounterHiRes();

    for (int c = 0; c < numOutputs; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], n);

    if (n > maxBlock || n <= 0 || sampleRate <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    // ---- gather each controller's MIDI ----
    midi.beginBlock (n);

    bool armed[numTracks];
    Source source[numTracks];
    for (int t = 0; t < numTracks; ++t)
    {
        armed[t] = track[t].armed.load();
        source[t] = (Source) track[t].source.load();
    }

    const juce::SpinLock::ScopedTryLockType tapeGuard (tapeLock);
    const bool haveTape = tapeGuard.isLocked() && tape != nullptr;

    if (haveTape)
    {
        tapeEngine.beginBlock (n, armed);
        tapeEngine.readAll (tapeOutPtr, n);
    }
    else
    {
        for (auto& v : tapeOut) std::fill_n (v.begin(), n, 0.0f);
    }

    // Which controller plays this track, and whether it speaks MPE.
    // An instrument or synth only plays when its track is armed; the choice of controller
    // just says which keyboard that is.
    auto controllerFor = [&] (int t, bool& mpe, int& bend) -> const juce::MidiBuffer*
    {
        auto port = track[t].midiPort.load();
        if (port == midiAny)
        {
            mpe = midi.anyIsMpe(); bend = midi.anyBendRange();
            return armed[t] ? &midi.anyBuffer() : nullptr;
        }
        if (armed[t] && midi.isInputActive (port))
        {
            mpe = midi.isInputMpe (port);
            bend = midi.inputBendRange (port);
            return &midi.portBuffer (port);
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
        const auto* in = controllerFor (t, mpe, bend);
        const bool receives = in != nullptr;

        // Hardware synth: forward the controller to its MIDI output, notes-off when the route changes.
        OutRoute route;
        if (source[t] == Source::hardware)
        {
            auto outPort = track[t].midiOutPort.load();
            if (midi.isOutputActive (outPort))
                route = { outPort, track[t].midiOutChannel.load() };
        }
        if (route != lastOutRoute[t] || (wasReceiving[t] && ! receives))
        {
            if (lastOutRoute[t].port >= 0)
                for (int ch = 1; ch <= 16; ++ch)
                    midi.outBuffer (lastOutRoute[t].port).addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            lastOutRoute[t] = route;
        }

        if (source[t] == Source::audio)
        {
            copyInput (t, s);
        }
        else if (source[t] == Source::hardware)
        {
            copyInput (t, s);
            if (route.port >= 0 && receives)
            {
                auto& out = midi.outBuffer (route.port);
                for (const auto meta : *in)
                {
                    auto msg = meta.getMessage();
                    // MPE needs every note on its own channel, so it passes through untouched.
                    if (! mpe && route.channel >= 1 && msg.getChannel() > 0)
                        msg.setChannel (route.channel);
                    out.addEvent (msg, meta.samplePosition);
                }
                if (! in->isEmpty()) track[t].midiActivity = true;
            }
        }
        else if (source[t] == Source::instrument)
        {
            trackMidi.clear();
            if (mpe != lastMpe[t] || bend != lastBend[t])
            {
                lastMpe[t] = mpe;
                lastBend[t] = bend;
                mpeConfigPending[t] = true;
            }
            if (mpeConfigPending[t].exchange (false))
                MidiHub::addMpeConfiguration (trackMidi, mpe, bend);
            if (receives)
                trackMidi.addEvents (*in, 0, n, 0);
            if (wasReceiving[t] && ! receives)
                for (int ch = 1; ch <= 16; ++ch)
                    trackMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);

            if (receives && ! in->isEmpty())
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

    // ---- MIDI out to hardware synths, time-stamped for the sender thread ----
    midi.endBlock (blockStartMs, sampleRate, n);

    // ---- channel strips ----
    // ARM is the live switch: armed tracks play their input, unarmed ones the tape (crossfaded).
    auto controls = [&] (int t) -> ChannelStrip::Controls
    {
        auto& p = track[t];
        return { -20.0f + 60.0f * p.gain.load(), p.low.load(), p.mid.load(), (float) midFreqHz (p.midFreq.load()),
                 p.high.load(), faderGain (p.fader.load()), p.pan.load(), armed[t] };
    };
    auto isLiveBounce = [&] (int t) { return armed[t] && source[t] == Source::bounce; };

    for (int t = 0; t < numTracks; ++t)
        if (! isLiveBounce (t))
            strips[t].process (tapeOut[t].data(), src[t].data(), chan[t].data(), n, controls (t));

    // A bounce destination's input is the other tracks, through their own strips.
    for (int t = 0; t < numTracks; ++t)
    {
        if (! isLiveBounce (t))
            continue;

        auto* s = src[t].data();
        std::fill_n (s, n, 0.0f);
        auto mask = track[t].bounceMask.load();
        for (int k = 0; k < numTracks; ++k)
            if (k != t && (mask & (1 << k)) && ! isLiveBounce (k))
                juce::FloatVectorOperations::add (s, chan[k].data(), n);
        strips[t].process (tapeOut[t].data(), s, chan[t].data(), n, controls (t));
    }

    // ---- onto the tape ----
    if (haveTape)
    {
        if (tapeEngine.isRecordingNow())
        {
            const auto userOffset = tapeEngine.params.latencyOffsetMs.load() * sampleRate / 1000.0;
            for (int t = 0; t < numTracks; ++t)
                if (armed[t])
                    tapeEngine.writeTrack (t, chan[t].data(), n,
                                           recordLatencySamples (source[t], inputLatency, outputLatency, userOffset));
        }
        tapeEngine.endBlock();
    }

    // ---- stereo mix ----
    std::fill_n (mixL.begin(), n, 0.0f);
    std::fill_n (mixR.begin(), n, 0.0f);

    for (int t = 0; t < numTracks; ++t)
    {
        auto* c = chan[t].data();
        auto peak = juce::FloatVectorOperations::findMaximum (c, n);
        peak = std::max (peak, -juce::FloatVectorOperations::findMinimum (c, n));
        if (peak > track[t].meter.load()) track[t].meter = peak;

        // A bounce destination is fed by the other tracks, which you already hear: it leaves the
        // mix as it goes live.
        const auto gain = source[t] == Source::bounce ? 1.0f - strips[t].liveAmount() : 1.0f;
        strips[t].addToMix (c, mixL.data(), mixR.data(), n, track[t].pan.load(), gain);
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
