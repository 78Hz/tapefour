#include "AudioEngine.h"
#include "PluginScan.h"
#include "ClapFormat.h"

namespace tf
{
double AudioEngine::recordLatencySamples (bool throughInterface, bool hasInstruments, bool bounce,
                                          int inLat, int outLat, double userOffsetSamples, int chainLatency) noexcept
{
    // Straight off the tape, in step with it.
    if (bounce) return chainLatency;
    // Played along to what you heard, then came back in through the interface.
    if (throughInterface || ! hasInstruments) return inLat + outLat + userOffsetSamples + chainLatency;
    // Played along to what you heard; made here, so no input latency.
    return outLat + chainLatency;
}

AudioEngine::AudioEngine (juce::PropertiesFile& settings)
    : props (settings)
{
    addAllPluginFormats (formatManager);
    knownPlugins.setCustomScanner (PluginScan::makeOutOfProcessScanner());

    if (auto known = props.getXmlValue ("knownPlugins"))
        knownPlugins.recreateFromXml (*known);

    for (auto& c : chains) c.blocks.reserve ((size_t) maxBlocks);
    allocateBuffers (maxBlock);
    trackMidi.ensureSize (8192);
    midiWork.ensureSize (8192);

    midi.isSlotInUse = [this] (int slot, bool output) { return isMidiSlotInUse (slot, output); };
    midi.onDevicesChanged = [this]
    {
        ++viewVersion;
        for (auto& p : mpeConfigPending) p = true;
        saveSettings();
    };
    midi.setMapper (&mapper, static_cast<MidiMapper::Sink*> (this));

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
    midi.setMapper (nullptr, nullptr);

    for (int t = 0; t < numTracks; ++t)
        clearChain (t);
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
    masterL.assign ((size_t) size, 0.0f);
    masterR.assign ((size_t) size, 0.0f);
    bounceSum.assign ((size_t) size, 0.0f);
    acc.setSize (2, size);
    scratch.setSize (2, size);
    dry.setSize (2, size);
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
    ++viewVersion;
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
    for (int t = 0; t < numTracks; ++t)
    {
        if (! output && track[t].midiPort.load() == slot) return true;
        if (output)
            for (auto& b : chains[t].blocks)
                if (auto* e = dynamic_cast<const ExternalBlock*> (b.get()); e != nullptr && e->outPort.load() == slot)
                    return true;
    }
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
    ++viewVersion;
    midi.setInputActive (slot, active);
    for (auto& p : mpeConfigPending) p = true;
    saveSettings();
}

void AudioEngine::setMidiInputMpe (int slot, bool mpe, int bendRange)
{
    ++viewVersion;
    midi.setInputMpe (slot, mpe, bendRange);
    for (auto& p : mpeConfigPending) p = true;
    saveSettings();
}

void AudioEngine::setMidiOutputActive (int slot, bool active)
{
    ++viewVersion;
    midi.setOutputActive (slot, active);
    saveSettings();
}

void AudioEngine::setMidiOutputAlias (int slot, const juce::String& alias)
{
    ++viewVersion;
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
// A track's inputs
//==============================================================================
std::unique_ptr<Tape> AudioEngine::swapTape (std::unique_ptr<Tape> newTape)
{
    // Recordings made on it from now on name this machine.
    if (newTape != nullptr) newTape->useRecorder (tf4DeckId);
    const juce::SpinLock::ScopedLockType sl (tapeLock);
    auto old = std::move (tape);
    tape = std::move (newTape);
    tapeEngine.setTape (tape.get());
    return old;
}

void AudioEngine::setTrackInput (int t, int inputChannel, bool pair)
{
    ++viewVersion;
    track[t].inputChannel = inputChannel < 0 ? -1 : inputChannel;
    track[t].inputPair = inputChannel >= 0 && pair;
}

void AudioEngine::setTrackMidiPort (int t, int port)
{
    ++viewVersion;
    track[t].midiPort = port;
    mpeConfigPending[t] = true;
}

void AudioEngine::setTrackFeed (int t, Feed feed)
{
    ++viewVersion;
    track[t].feed = (int) feed;
}

void AudioEngine::toggleBounceSource (int t, int sourceTrack)
{
    ++viewVersion;
    if (sourceTrack == t) return;
    track[t].bounceMask = track[t].bounceMask.load() ^ (1 << sourceTrack);
    track[t].feed = (int) Feed::bounce;
}

juce::String AudioEngine::describeInput (int t) const
{
    auto ch = track[t].inputChannel.load() + 1;
    if (ch <= 0) return "NO AUDIO IN";
    return "IN " + juce::String (ch) + (track[t].inputPair.load() ? "+" + juce::String (ch + 1) : juce::String());
}

//==============================================================================
// A track's chain
//==============================================================================
void AudioEngine::prepareBlock (Block& b)
{
    if (sampleRate > 0)
        b.prepare (sampleRate, std::max (bufferSize, 32));
}

int AudioEngine::numBlocks (int t) const
{
    return (int) chains[t].blocks.size();
}

Block* AudioEngine::findBlock (int t, int blockId) const
{
    for (auto& b : chains[t].blocks)
        if (b->id == blockId) return b.get();
    return nullptr;
}

std::vector<AudioEngine::BlockInfo> AudioEngine::getChain (int t) const
{
    std::vector<BlockInfo> out;
    for (auto& b : chains[t].blocks)
    {
        BlockInfo i;
        i.id = b->id;
        i.name = b->getName();
        i.maker = b->getMaker();
        i.role = b->role();
        i.bypass = b->bypass.load();
        i.hasEditor = b->hasEditor();
        i.canProcessInput = b->canProcessInput();
        i.processInput = b->processInput.load();
        i.producesMidi = b->producesMidi();
        i.hosted = b->isHosted();
        i.levelDb = b->levelDb.load();
        i.mix = b->mix.load();
        if (auto* p = dynamic_cast<PendingBlock*> (b.get())) i.missing = p->missing;
        if (auto* pb = dynamic_cast<ProcessorBlock*> (b.get()))
        {
            i.builtIn = pb->getBuiltInIndex();
            i.builtInSynth = pb->getKind() == ProcessorBlock::Kind::builtInSynth;
        }
        if (auto* e = dynamic_cast<ExternalBlock*> (b.get()))
        {
            i.outPort = e->outPort.load();
            i.channel = e->channel.load();
            i.returnInput = e->returnInput.load();
            i.returnPair = e->returnPair.load();
            i.name = midi.outputExists (i.outPort) ? midi.outputLabel (i.outPort) : juce::String ("HARDWARE SYNTH");
        }
        out.push_back (i);
    }
    return out;
}

int AudioEngine::insertBlock (int t, int index, std::unique_ptr<Block> block)
{
    return insertBlockWithId (t, index, std::move (block), 0);
}

int AudioEngine::insertBlockWithId (int t, int index, std::unique_ptr<Block> block, int id0)
{
    if (block == nullptr || numBlocks (t) >= maxBlocks)
        return 0;
    prepareBlock (*block);
    block->id = id0 != 0 ? id0 : nextBlockId++;
    const auto id = block->id;
    {
        const juce::SpinLock::ScopedLockType sl (chains[t].lock);
        auto& v = chains[t].blocks;
        auto at = index < 0 || index > (int) v.size() ? (int) v.size() : index;
        v.insert (v.begin() + at, std::move (block));
    }
    ++viewVersion;
    mpeConfigPending[t] = true;
    return id;
}

std::unique_ptr<Block> AudioEngine::takeBlock (int t, int blockId, int* index)
{
    auto& v = chains[t].blocks;
    for (int i = 0; i < (int) v.size(); ++i)
    {
        if (v[(size_t) i]->id != blockId) continue;
        // Its window and its MIDI mappings go before it does.
        if (onBlockRemoving) onBlockRemoving (t, blockId);
        mapper.forgetBlock (t, blockId);
        std::unique_ptr<Block> out;
        {
            const juce::SpinLock::ScopedLockType sl (chains[t].lock);
            out = std::move (v[(size_t) i]);
            v.erase (v.begin() + i);
        }
        if (index != nullptr) *index = i;
        ++viewVersion;
        return out;
    }
    return {};
}

void AudioEngine::destroyBlock (int, std::unique_ptr<Block> block)
{
    if (auto* e = dynamic_cast<ExternalBlock*> (block.get()))
        midi.allNotesOffNow (e->outPort.load());
    block.reset();
}

int AudioEngine::replaceBlock (int t, int blockId, std::unique_ptr<Block> block, bool keepId)
{
    int index = -1;
    auto old = takeBlock (t, blockId, &index);
    auto id = insertBlockWithId (t, index, std::move (block), keepId && old != nullptr ? blockId : 0);
    destroyBlock (t, std::move (old));
    return id;
}

void AudioEngine::removeBlock (int t, int blockId)
{
    destroyBlock (t, takeBlock (t, blockId));
    mpeConfigPending[t] = true;
}

void AudioEngine::moveBlock (int t, int blockId, int newIndex)
{
    ++viewVersion;
    const juce::SpinLock::ScopedLockType sl (chains[t].lock);
    auto& v = chains[t].blocks;
    for (int i = 0; i < (int) v.size(); ++i)
    {
        if (v[(size_t) i]->id != blockId) continue;
        auto b = std::move (v[(size_t) i]);
        v.erase (v.begin() + i);
        newIndex = juce::jlimit (0, (int) v.size(), newIndex);
        v.insert (v.begin() + newIndex, std::move (b));
        return;
    }
}

void AudioEngine::clearChain (int t)
{
    while (! chains[t].blocks.empty())
        removeBlock (t, chains[t].blocks.back()->id);
}

void AudioEngine::setBlockBypass (int t, int blockId, bool bypassed)
{
    ++viewVersion;
    if (auto* b = findBlock (t, blockId))
    {
        b->bypass = bypassed;
        if (bypassed)
            if (auto* e = dynamic_cast<ExternalBlock*> (b))
                midi.allNotesOffNow (e->outPort.load());
    }
}

void AudioEngine::sendAllNotesOff (int t, int blockId)
{
    if (auto* e = dynamic_cast<ExternalBlock*> (findBlock (t, blockId)))
        midi.allNotesOffNow (e->outPort.load());
}

int AudioEngine::addPlugin (int t, int index, const juce::PluginDescription& desc, const juce::String& base64State,
                            int replaceId, std::function<void (const juce::String&)> done,
                            std::unique_ptr<juce::XmlElement> savedBlock)
{
    // The placeholder keeps the block's place (and its saved settings) while the plug-in loads.
    if (savedBlock == nullptr)
    {
        savedBlock = std::make_unique<juce::XmlElement> ("BLOCK");
        savedBlock->setAttribute ("kind", "plugin");
        savedBlock->addChildElement (desc.createXml().release());
        savedBlock->setAttribute ("state", base64State);
    }
    std::shared_ptr<juce::XmlElement> saved (std::make_unique<juce::XmlElement> (*savedBlock).release());
    auto pendingId = replaceId != 0 ? replaceBlock (t, replaceId, std::make_unique<PendingBlock> (std::move (savedBlock)))
                                    : insertBlock (t, index, std::make_unique<PendingBlock> (std::move (savedBlock)));
    if (pendingId == 0)
    {
        if (done) done ("The chain is full (" + juce::String (maxBlocks) + " blocks).");
        return 0;
    }

    auto sr = sampleRate > 0 ? sampleRate : 48000.0;
    auto bs = bufferSize > 0 ? bufferSize : 512;
    std::weak_ptr<bool> engineAlive = alive;
    formatManager.createPluginInstanceAsync (desc, sr, bs,
        [this, t, pendingId, engineAlive, saved, done] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& err)
        {
            if (engineAlive.expired())
                return;

            // Something else was done with this place in the chain while the plug-in loaded.
            auto* placeholder = dynamic_cast<PendingBlock*> (findBlock (t, pendingId));
            if (placeholder == nullptr)
            {
                if (done) done ("superseded");
                return;
            }

            if (p == nullptr)
            {
                placeholder->missing = true;
                ++viewVersion;
                if (done) done (err.isNotEmpty() ? err : juce::String ("The plug-in could not be loaded."));
                return;
            }

            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (saved->getStringAttribute ("state")) && mb.getSize() > 0)
                p->setStateInformation (mb.getData(), (int) mb.getSize());

            auto block = std::make_unique<ProcessorBlock> (std::move (p), ProcessorBlock::Kind::hosted);
            block->readCommon (*saved);
            auto* processor = block->getProcessor();
            auto newId = replaceBlock (t, pendingId, std::move (block), true);
            mapper.restoreBlockMappings (*saved, t, newId, processor);
            saveSettings (true);
            if (done) done ({});
        });
    return pendingId;
}

int AudioEngine::addExternal (int t, int index, int outPort)
{
    return insertBlock (t, index, std::make_unique<ExternalBlock> (outPort));
}

juce::String AudioEngine::describeSource (int t) const
{
    auto& p = track[t];
    if ((Feed) p.feed.load() == Feed::bounce)
    {
        juce::StringArray names;
        for (int s = 0; s < numTracks; ++s)
            if (p.bounceMask.load() & (1 << s))
                names.add (juce::String (s + 1));
        return names.isEmpty() ? juce::String ("BOUNCE: NO TRACKS") : "BOUNCE " + names.joinIntoString ("+");
    }

    juce::StringArray sounds;
    juce::String waiting;
    for (auto& b : getChain (t))
    {
        if (b.role == BlockRole::instrument || b.role == BlockRole::external)
            sounds.add (b.name.toUpperCase());
        if (b.role == BlockRole::pending && waiting.isEmpty())
            waiting = (b.missing ? "MISSING: " : "LOADING ") + b.name.toUpperCase();
    }
    if (! sounds.isEmpty())
        return sounds[0] + (sounds.size() > 1 ? " +" + juce::String (sounds.size() - 1) : juce::String());
    if (waiting.isNotEmpty()) return waiting;
    if (p.inputChannel.load() >= 0) return "AUDIO " + describeInput (t);
    auto chain = getChain (t);
    return chain.empty() ? juce::String ("NOTHING PLUGGED IN") : chain.front().name.toUpperCase();
}

juce::StringArray AudioEngine::blockAbbreviations (int t) const
{
    juce::StringArray out;
    for (auto& b : getChain (t))
    {
        auto name = b.name.toUpperCase().trim();
        if (name.startsWith ("TF ")) name = name.substring (3);
        auto words = juce::StringArray::fromTokens (name, " -_", "");
        words.removeEmptyStrings();
        juce::String a;
        if (words.size() > 1)
            for (int i = 0; i < std::min (3, words.size()); ++i) a << words[i].substring (0, 1);
        else
            a = name.removeCharacters (" ").substring (0, 4);
        out.add (a.isEmpty() ? juce::String ("?") : a);
    }
    return out;
}

bool AudioEngine::anySolo() const noexcept
{
    for (auto& p : track) if (p.solo.load()) return true;
    return false;
}

bool AudioEngine::soloedTracksAllMuted() const noexcept
{
    bool any = false;
    for (auto& p : track)
        if (p.solo.load())
        {
            any = true;
            if (! p.mute.load()) return false;
        }
    return any;
}

//==============================================================================
// MIDI learn's sink: what a mapped control does (audio thread)
//==============================================================================
float AudioEngine::getValue (const MidiMapper::Target& target, juce::AudioProcessorParameter* param)
{
    using C = MidiMapper::Control;
    if (target.plugin) return param != nullptr ? param->getValue() : 0.0f;
    auto bip = [] (float v) { return (v + 1.0f) * 0.5f; };
    auto t = juce::jlimit (0, numTracks - 1, target.track);
    switch (target.control)
    {
        case C::gain: return track[t].gain.load();
        case C::high: return bip (track[t].high.load());
        case C::mid: return bip (track[t].mid.load());
        case C::freq: return track[t].midFreq.load();
        case C::low: return bip (track[t].low.load());
        case C::pan: return bip (track[t].pan.load());
        case C::fader: return track[t].fader.load();
        case C::masterFader: return masterFader.load();
        case C::phones: return phones.load();
        case C::pitch: return (tapeEngine.params.pitch.load() + 12.0f) / 24.0f;
        // switches and transport keys have no position
        case C::mute: case C::solo: case C::arm: case C::speed: case C::nr:
        case C::play: case C::stop: case C::rec: case C::rew: case C::ff: case C::rtz:
        case C::numControls: return 0.0f;
    }
    return 0.0f;
}

void AudioEngine::setValue (const MidiMapper::Target& target, juce::AudioProcessorParameter* param, float v)
{
    using C = MidiMapper::Control;
    v = juce::jlimit (0.0f, 1.0f, v);
    if (target.plugin)
    {
        if (param != nullptr) param->setValueNotifyingHost (v);
        return;
    }
    auto bip = v * 2.0f - 1.0f;
    auto t = juce::jlimit (0, numTracks - 1, target.track);
    switch (target.control)
    {
        case C::gain: track[t].gain = v; break;
        case C::high: track[t].high = bip; break;
        case C::mid: track[t].mid = bip; break;
        case C::freq: track[t].midFreq = v; break;
        case C::low: track[t].low = bip; break;
        case C::pan: track[t].pan = bip; break;
        case C::fader: track[t].fader = v; break;
        case C::masterFader: masterFader = v; break;
        case C::phones: phones = v; break;
        case C::pitch: tapeEngine.params.pitch = v * 24.0f - 12.0f; break;
        case C::mute: case C::solo: case C::arm: case C::speed: case C::nr:
        case C::play: case C::stop: case C::rec: case C::rew: case C::ff: case C::rtz:
        case C::numControls: break;   // these are toggled or pressed, not set
    }
}

void AudioEngine::toggle (const MidiMapper::Target& target)
{
    using C = MidiMapper::Control;
    auto t = juce::jlimit (0, numTracks - 1, target.track);
    switch (target.control)
    {
        case C::mute: track[t].mute = ! track[t].mute.load(); break;
        case C::solo: track[t].solo = ! track[t].solo.load(); break;
        case C::arm:  track[t].armed = ! track[t].armed.load(); break;
        case C::nr:   tapeEngine.params.nrOn = ! tapeEngine.params.nrOn.load(); break;
        case C::speed: tapeEngine.params.speedSel = tapeEngine.params.speedSel.load() == 2 ? 1 : 2; break;
        case C::gain: case C::high: case C::mid: case C::freq: case C::low: case C::pan: case C::fader:
        case C::masterFader: case C::phones: case C::pitch:
        case C::play: case C::stop: case C::rec: case C::rew: case C::ff: case C::rtz:
        case C::numControls: break;   // knobs and transport keys don't toggle
    }
}

void AudioEngine::command (MidiMapper::Control c)
{
    // The deck carries it out, with the same checks as its own keys.
    remoteCommand = (int) c + 1;
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
    masterDeck.prepare (sampleRate, maxBlock);
    masterDeck.applySettings (tapeEngine.params.nrType.load());

    for (int t = 0; t < numTracks; ++t)
    {
        strips[t].prepare (sampleRate);
        const juce::SpinLock::ScopedLockType sl (chains[t].lock);
        for (auto& b : chains[t].blocks)
            b->prepare (sampleRate, std::max (bufferSize, 32));
    }
}

void AudioEngine::audioDeviceStopped()
{
    masterDeck.stopForDeviceChange();
}

void AudioEngine::copyInput (int ch, bool pair, const float* const* inputs, int numInputs, int n, float* dest, float gain) const
{
    if (ch < 0 || ch >= numInputs || inputs[ch] == nullptr) return;
    if (pair && ch + 1 < numInputs && inputs[ch + 1] != nullptr)
    {
        // mono sum
        juce::FloatVectorOperations::addWithMultiply (dest, inputs[ch], 0.5f * gain, n);
        juce::FloatVectorOperations::addWithMultiply (dest, inputs[ch + 1], 0.5f * gain, n);
    }
    else
    {
        juce::FloatVectorOperations::addWithMultiply (dest, inputs[ch], gain, n);
    }
}

AudioEngine::ChainResult AudioEngine::runChain (int t, const float* const* inputs, int numInputs, int n, bool armed,
                                                float* monoOut, const float* bounceIn)
{
    ChainResult r;
    auto& p = track[t];
    acc.clear (0, n);

    // ---- the audio stream starts with the track's input (or the bounce, or silence) ----
    if (bounceIn != nullptr)
    {
        acc.copyFrom (0, 0, bounceIn, n);
        acc.copyFrom (1, 0, bounceIn, n);
    }
    else if (p.inputChannel.load() >= 0)
    {
        copyInput (p.inputChannel.load(), p.inputPair.load(), inputs, numInputs, n, acc.getWritePointer (0));
        acc.copyFrom (1, 0, acc, 0, 0, n);
        r.throughInterface = true;
    }

    // ---- the MIDI stream starts with the track's controller, while armed ----
    bool mpe = false;
    int bend = 48;
    const juce::MidiBuffer* in = nullptr;
    auto port = p.midiPort.load();
    if (port == midiAny)
    {
        mpe = midi.anyIsMpe(); bend = midi.anyBendRange();
        if (armed) in = &midi.anyBuffer();
    }
    else if (armed && midi.isInputActive (port))
    {
        mpe = midi.isInputMpe (port);
        bend = midi.inputBendRange (port);
        in = &midi.portBuffer (port);
    }
    const bool receives = in != nullptr;

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
    {
        trackMidi.addEvents (*in, 0, n, 0);
        if (! in->isEmpty()) p.midiActivity = true;
    }
    if (wasReceiving[t] && ! receives)
        for (int ch = 1; ch <= 16; ++ch)
            trackMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
    wasReceiving[t] = receives;

    // ---- the blocks, left to right ----
    const juce::SpinLock::ScopedTryLockType sl (chains[t].lock);
    if (sl.isLocked())
    {
        for (auto& bp : chains[t].blocks)
        {
            auto& b = *bp;
            if (b.bypass.load())
                continue;   // MIDI and audio pass straight through
            b.applyMpe (mpe, bend);
            const auto role = b.role();

            auto runWithMidi = [&] (juce::AudioBuffer<float>& audio)
            {
                // A block that doesn't make MIDI leaves the stream as it was, whatever it does with its copy.
                midiWork.clear();
                midiWork.addEvents (trackMidi, 0, n, 0);
                b.process (audio, n, midiWork);
                if (b.producesMidi())
                    trackMidi.swapWith (midiWork);
            };

            switch (role)
            {
                case BlockRole::pending:
                    break;

                case BlockRole::external:
                {
                    auto& e = static_cast<ExternalBlock&> (b);
                    int outPort = e.outPort.load();
                    if (! midi.isOutputActive (outPort)) outPort = -1;
                    const int channel = e.channel.load();
                    const bool sending = outPort >= 0 && receives;
                    // The route changed or the playing stopped: nothing left hanging.
                    if ((outPort != e.lastPort || channel != e.lastChannel || (e.wasSending && ! sending)) && e.lastPort >= 0)
                        for (int ch = 1; ch <= 16; ++ch)
                            midi.outBuffer (e.lastPort).addEvent (juce::MidiMessage::allNotesOff (ch), 0);
                    e.lastPort = outPort;
                    e.lastChannel = channel;
                    e.wasSending = sending;
                    if (sending)
                    {
                        auto& out = midi.outBuffer (outPort);
                        for (const auto meta : trackMidi)
                        {
                            auto msg = meta.getMessage();
                            // MPE needs every note on its own channel, so it passes through untouched.
                            if (! mpe && channel >= 1 && msg.getChannel() > 0)
                                msg.setChannel (channel);
                            out.addEvent (msg, meta.samplePosition);
                        }
                    }
                    if (e.returnInput.load() >= 0)
                    {
                        scratch.clear (0, n);
                        copyInput (e.returnInput.load(), e.returnPair.load(), inputs, numInputs, n, scratch.getWritePointer (0),
                                   dbToGain (e.levelDb.load()));
                        acc.addFrom (0, 0, scratch, 0, 0, n);
                        acc.addFrom (1, 0, scratch, 0, 0, n);
                        r.throughInterface = true;
                    }
                    r.hasInstruments = true;
                    break;
                }

                case BlockRole::midiFx:
                    scratch.clear (0, n);
                    runWithMidi (scratch);
                    break;

                case BlockRole::instrument:
                    if (! b.processInput.load())
                    {
                        // Adds its sound to what's already there.
                        scratch.clear (0, n);
                        runWithMidi (scratch);
                        const auto g = dbToGain (b.levelDb.load()) * b.mixInPad();
                        acc.addFrom (0, 0, scratch, 0, 0, n, g);
                        acc.addFrom (1, 0, scratch, 1, 0, n, g);
                        r.hasInstruments = true;
                        r.latency += b.latencySamples();
                        break;
                    }
                    [[fallthrough]];

                case BlockRole::audioFx:
                {
                    const auto wet = role == BlockRole::audioFx ? b.mix.load() : 1.0f;
                    if (wet < 0.999f)
                    {
                        dry.copyFrom (0, 0, acc, 0, 0, n);
                        dry.copyFrom (1, 0, acc, 1, 0, n);
                    }
                    runWithMidi (acc);
                    if (wet < 0.999f)
                    {
                        acc.applyGain (0, n, wet);
                        acc.addFrom (0, 0, dry, 0, 0, n, 1.0f - wet);
                        acc.addFrom (1, 0, dry, 1, 0, n, 1.0f - wet);
                    }
                    if (role == BlockRole::instrument)
                    {
                        acc.applyGain (0, n, dbToGain (b.levelDb.load()));
                        r.hasInstruments = true;
                    }
                    r.latency += b.latencySamples();
                    break;
                }
            }
        }
    }

    // ---- tracks are mono ----
    auto* l = acc.getReadPointer (0);
    auto* rr = acc.getReadPointer (1);
    for (int i = 0; i < n; ++i)
        monoOut[i] = (l[i] + rr[i]) * 0.5f;
    return r;
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

    // ---- each controller's MIDI (MIDI learn takes its messages out here) ----
    midi.beginBlock (n);

    // ---- what's coming in, for the signal bars ----
    for (int c = 0; c < std::min (numInputs, maxInputs); ++c)
        if (inputs[c] != nullptr)
        {
            auto range = juce::FloatVectorOperations::findMinAndMax (inputs[c], n);
            auto peak = std::max (-range.getStart(), range.getEnd());
            if (peak > inputPeak[c].load()) inputPeak[c] = peak;
        }

    bool armed[numTracks], muted[numTracks], bounce[numTracks];
    for (int t = 0; t < numTracks; ++t)
    {
        armed[t] = track[t].armed.load();
        muted[t] = track[t].mute.load();
        bounce[t] = (Feed) track[t].feed.load() == Feed::bounce;
    }
    const bool soloing = anySolo();

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

    auto controls = [&] (int t) -> ChannelStrip::Controls
    {
        auto& p = track[t];
        return { -20.0f + 60.0f * p.gain.load(), p.low.load(), p.mid.load(), (float) midFreqHz (p.midFreq.load()),
                 p.high.load(), faderGain (p.fader.load()), p.pan.load(), armed[t] };
    };

    // A chain is only heard while its track is armed, or fading back to tape after. Otherwise it's
    // left alone (its plug-ins carry on from where they were when the track is armed again). It runs
    // one more block after its MIDI stops, so its notes-off reach every block and hardware synth.
    auto chainHeard = [&] (int t) { return armed[t] || strips[t].liveAmount() > 0.0f || wasReceiving[t]; };
    auto runOrRest = [&] (int t, const float* bounceIn)
    {
        if (chainHeard (t))
            chainResult[t] = runChain (t, inputs, numInputs, n, armed[t], src[t].data(), bounceIn);
        else
        {
            std::fill_n (src[t].begin(), n, 0.0f);
            chainResult[t] = {};
        }
        strips[t].process (tapeOut[t].data(), src[t].data(), chan[t].data(), n, controls (t));
    };

    // ---- live tracks: their chains, then their strips ----
    for (int t = 0; t < numTracks; ++t)
        if (! bounce[t])
            runOrRest (t, nullptr);

    // ---- bounce tracks: the other tracks off tape, through their strips (mute silences a source) ----
    for (int t = 0; t < numTracks; ++t)
    {
        if (! bounce[t]) continue;
        if (chainHeard (t))
        {
            std::fill_n (bounceSum.begin(), n, 0.0f);
            auto mask = track[t].bounceMask.load();
            for (int k = 0; k < numTracks; ++k)
                if (k != t && (mask & (1 << k)) && ! bounce[k] && ! muted[k])
                    juce::FloatVectorOperations::add (bounceSum.data(), chan[k].data(), n);
        }
        runOrRest (t, bounceSum.data());
    }

    // ---- MIDI out to hardware synths, time-stamped for the sender thread ----
    midi.endBlock (blockStartMs, sampleRate, n);

    // ---- onto the tape (muted tracks still record) ----
    if (haveTape)
    {
        if (tapeEngine.isRecordingNow())
        {
            const auto userOffset = tapeEngine.params.latencyOffsetMs.load() * sampleRate / 1000.0;
            for (int t = 0; t < numTracks; ++t)
                if (armed[t])
                {
                    auto& cr = chainResult[t];
                    tapeEngine.writeTrack (t, chan[t].data(), n,
                                           recordLatencySamples (cr.throughInterface, cr.hasInstruments, bounce[t],
                                                                 inputLatency, outputLatency, userOffset, cr.latency));
                }
        }
        tapeEngine.endBlock();
    }

    // ---- stereo mix: solo changes what's heard; mute wins over solo ----
    std::fill_n (mixL.begin(), n, 0.0f);
    std::fill_n (mixR.begin(), n, 0.0f);

    for (int t = 0; t < numTracks; ++t)
    {
        auto* c = chan[t].data();
        auto range = juce::FloatVectorOperations::findMinAndMax (c, n);
        auto peak = std::max (-range.getStart(), range.getEnd());
        if (peak > track[t].meter.load()) track[t].meter = peak;

        const bool heard = ! muted[t] && (! soloing || track[t].solo.load());
        // A bounce destination is fed by the other tracks, which you already hear: it leaves the
        // mix as it goes live.
        auto gain = heard ? (bounce[t] ? 1.0f - strips[t].liveAmount() : 1.0f) : 0.0f;
        strips[t].addToMix (c, mixL.data(), mixR.data(), n, track[t].pan.load(), gain);
    }

    // ---- the master bus: after the master fader, it's what you hear and what a mixdown takes ----
    auto mg = faderGain (masterFader.load());
    for (int i = 0; i < n; ++i)
    {
        auto g = lastMaster + (mg - lastMaster) * (float) i / (float) n;
        masterL[(size_t) i] = mixL[(size_t) i] * g;
        masterR[(size_t) i] = mixR[(size_t) i] * g;
    }
    lastMaster = mg;
    const auto deckMode = tapeEngine.getMode();
    masterDeck.process (masterL.data(), masterR.data(), n, haveTape && (deckMode == Mode::play || deckMode == Mode::rec));
    if (deckMode != Mode::stop) masterDeck.stopListening();   // the deck playing takes over from a take being listened to
    masterDeck.listenInto (masterL.data(), masterR.data(), n);

    auto pg = std::pow (phones.load() / 0.6f, 2.0f);
    float pl = 0, pr = 0;
    auto mainPair = mainOutPair.load(), phonesPairIdx = phonesPair.load();

    for (int i = 0; i < n; ++i)
    {
        auto f = (float) i / (float) n;
        auto l = masterL[(size_t) i], r = masterR[(size_t) i];
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
    lastPhones = pg;

    if (pl > meterL.load()) meterL = pl;
    if (pr > meterR.load()) meterR = pr;
    if (pl > peakHoldL.load()) peakHoldL = pl;
    if (pr > peakHoldR.load()) peakHoldR = pr;
}
} // namespace tf
