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
        pre[t].assign ((size_t) size, 0.0f);
        feedPre[t].assign ((size_t) size, 0.0f);
        feedPost[t].assign ((size_t) size, 0.0f);
        routeIn[t].assign ((size_t) size, 0.0f);
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
    extraOut.setSize (2, size);
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

bool AudioEngine::turnTapeOver (bool sideBUp)
{
    const juce::SpinLock::ScopedLockType sl (tapeLock);
    if (tape == nullptr || tapeEngine.getMode() != Mode::stop) return false;
    if (tape->isTurnedOver() == sideBUp) return true;
    // (The counter is the machine's: turning the cassette over doesn't move it.)
    tape->turnOver (sideBUp);
    tapeEngine.setTape (tape.get(), false);   // picks up the turned position; leaves the switch alone
    return true;
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
    updateRoutes();
}

void AudioEngine::toggleBounceSource (int t, int sourceTrack)
{
    ++viewVersion;
    if (sourceTrack == t) return;
    track[t].bounceMask = track[t].bounceMask.load() ^ (1 << sourceTrack);
    track[t].feed = (int) Feed::bounce;
    updateRoutes();
}

//==============================================================================
// Routing
//==============================================================================
int AudioEngine::routeEdgesNow() const
{
    // Every route as an edge, in a fixed order; one that would make a loop (or start on a bounce
    // track, which runs after the others) is left out, so the tracks can always run in an order.
    int edges = 0;
    auto reaches = [&edges] (int from, int to)
    {
        int seen = 1 << from, frontier = 1 << from;
        while (frontier != 0)
        {
            int next = 0;
            for (int a = 0; a < numTracks; ++a)
                if (frontier & (1 << a))
                    for (int b = 0; b < numTracks; ++b)
                        if ((edges & (1 << (a * numTracks + b))) && ! (seen & (1 << b))) next |= 1 << b;
            seen |= next;
            frontier = next;
        }
        return (seen & (1 << to)) != 0;
    };
    auto add = [&] (int from, int to)
    {
        if (from == to || ! juce::isPositiveAndBelow (to, numTracks)) return;
        if ((Feed) track[from].feed.load() == Feed::bounce) return;
        if (reaches (to, from)) return;
        edges |= 1 << (from * numTracks + to);
    };
    for (int t = 0; t < numTracks; ++t)
    {
        for (auto& b : chains[t].blocks)
            for (int o = 0; o < b->numOutputs(); ++o)
                if (auto r = b->getRoute (o); r.dest >= 0) add (t, r.dest);
        for (int d = 0; d < numTracks; ++d)
            if (track[t].chainOutMask.load() & (1 << d)) add (t, d);
    }
    return edges;
}

void AudioEngine::updateRoutes()
{
    routeEdges = routeEdgesNow();
    ++viewVersion;
}

bool AudioEngine::routeAllowed (int from, int to) const
{
    if (! juce::isPositiveAndBelow (from, numTracks) || ! juce::isPositiveAndBelow (to, numTracks) || from == to) return false;
    if ((Feed) track[from].feed.load() == Feed::bounce) return false;
    // a loop: 'to' already reaches 'from'
    const int edges = routeEdgesNow();
    int seen = 1 << to, frontier = 1 << to;
    while (frontier != 0)
    {
        int next = 0;
        for (int a = 0; a < numTracks; ++a)
            if (frontier & (1 << a))
                for (int b = 0; b < numTracks; ++b)
                    if ((edges & (1 << (a * numTracks + b))) && ! (seen & (1 << b))) next |= 1 << b;
        seen |= next;
        frontier = next;
    }
    return (seen & (1 << from)) == 0;
}

bool AudioEngine::setBlockRoute (int t, int blockId, int output, int destTrack, bool home)
{
    auto* b = findBlock (t, blockId);
    if (b == nullptr || ! juce::isPositiveAndBelow (output, b->numOutputs())) return false;
    if (destTrack >= 0 && destTrack != b->getRoute (output).dest && ! routeAllowed (t, destTrack)) return false;
    // sent nowhere, the main output and a built-in's voices stay here (a plug-in's aux bus can be off)
    const bool mustStay = output == 0 || b->extrasInMain();
    b->setRoute (output, destTrack, destTrack < 0 && mustStay ? true : home);
    updateRoutes();
    return true;
}

bool AudioEngine::setChainOut (int from, int to, bool on)
{
    if (! juce::isPositiveAndBelow (from, numTracks) || ! juce::isPositiveAndBelow (to, numTracks)) return false;
    auto& m = track[from].chainOutMask;
    if (on && ! (m.load() & (1 << to)) && ! routeAllowed (from, to)) return false;
    m = on ? (m.load() | (1 << to)) : (m.load() & ~(1 << to));
    updateRoutes();
    return true;
}

std::vector<AudioEngine::RouteSource> AudioEngine::getRouteSources() const
{
    std::vector<RouteSource> out;
    for (int t = 0; t < numTracks; ++t)
    {
        for (auto& b : chains[t].blocks)
        {
            const auto role = b->role();
            if (role != BlockRole::instrument && role != BlockRole::external) continue;
            for (int o = 0; o < b->numOutputs(); ++o)
            {
                RouteSource s;
                s.track = t;
                s.blockId = b->id;
                s.output = o;
                s.name = b->numOutputs() > 1 ? b->outputName (o) : juce::String();   // one output: the instrument is its name
                s.owner = b->getName().toUpperCase();
                const auto r = b->getRoute (o);
                s.dest = r.dest;
                s.home = r.home;
                s.canBeOff = o > 0 && ! b->extrasInMain();
                s.channels = b->outputChannels (o);
                out.push_back (s);
            }
        }
        RouteSource c;
        c.track = t;
        c.name = "TRK " + juce::String (t + 1) + " CHAIN OUT";
        c.owner = "END OF ITS CHAIN";
        c.dest = -1;
        out.push_back (c);
    }
    return out;
}

juce::String AudioEngine::describeRoutedIn (int t) const
{
    int count = 0, fromMask = 0;
    for (auto& s : getRouteSources())
    {
        const bool here = s.blockId == 0 ? (track[s.track].chainOutMask.load() & (1 << t)) != 0 : s.dest == t;
        if (here && s.track != t) { ++count; fromMask |= 1 << s.track; }
    }
    if (count == 0) return {};
    juce::StringArray from;
    for (int k = 0; k < numTracks; ++k) if (fromMask & (1 << k)) from.add (juce::String (k + 1));
    return "FROM TRK " + from.joinIntoString ("+") + (count > 1 ? juce::String (juce::CharPointer_UTF8 (" \xc3\x97")) + juce::String (count) : juce::String());
}

juce::String AudioEngine::routeSignature() const
{
    juce::String s;
    s << routeEdges.load() << ":";
    for (auto& r : getRouteSources())
        s << r.track << "." << r.blockId << "." << r.output << ">" << r.dest << (r.home ? "h" : "") << " " << r.name << ",";
    for (int t = 0; t < numTracks; ++t) s << track[t].chainOutMask.load() << ".";
    return s;
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
    updateRoutes();
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
        updateRoutes();
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

int AudioEngine::unloadBlock (int t, int blockId)
{
    if (findBlock (t, blockId) == nullptr) return 0;
    auto id = replaceBlock (t, blockId, std::make_unique<EmptyBlock>());
    mpeConfigPending[t] = true;
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
    if (auto routed = describeRoutedIn (t); routed.isNotEmpty()) return routed;
    if (p.inputChannel.load() >= 0) return "AUDIO " + describeInput (t);
    for (auto& b : getChain (t))
        if (b.role != BlockRole::empty) return b.name.toUpperCase();
    return "NOTHING PLUGGED IN";
}

juce::StringArray AudioEngine::blockAbbreviations (int t) const
{
    juce::StringArray out;
    for (auto& b : getChain (t))
    {
        if (b.role == BlockRole::empty) { out.add ("-"); continue; }
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

    // The chain runs in stereo and the track is mono: the two sides are summed with equal power (each times
    // 1/sqrt 2), so a stereo instrument keeps its level however wide it is. A mono source goes in on both
    // sides at 1/sqrt 2, so it comes out of the sum at the level it went in.
    constexpr float side = 0.70710678f;
    auto sendOf = [n] (const juce::AudioBuffer<float>& b, float gain)
    {
        const auto* bl = b.getReadPointer (0);
        const auto* br = b.getReadPointer (1);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (bl[i] + br[i]));
        return peak * side * gain;
    };
    auto noteSend = [] (Block& b, float peak) { if (peak > b.sendPeak.load()) b.sendPeak = peak; };

    // ---- the audio stream starts with the track's input (or the bounce, or silence) ----
    if (bounceIn != nullptr)
    {
        acc.copyFrom (0, 0, bounceIn, n, side);
        acc.copyFrom (1, 0, bounceIn, n, side);
    }
    else if (p.inputChannel.load() >= 0)
    {
        copyInput (p.inputChannel.load(), p.inputPair.load(), inputs, numInputs, n, acc.getWritePointer (0), side);
        acc.copyFrom (1, 0, acc, 0, 0, n);
        r.throughInterface = true;
    }

    // ---- with what other tracks send it (their instruments' outputs, their chain outs) ----
    if (routeUsed[t])
    {
        acc.addFrom (0, 0, routeIn[t].data(), n, side);
        acc.addFrom (1, 0, routeIn[t].data(), n, side);
        r.throughInterface = r.throughInterface || routeFrom[t].throughInterface;
        r.hasInstruments = r.hasInstruments || routeFrom[t].hasInstruments;
        r.latency += routeFrom[t].latency;
    }

    // An instrument's extra outputs: sent to their tracks, and (a plug-in's aux bus kept here) mixed in.
    auto sendExtras = [&] (Block& blk, float g, const ChainResult& info)
    {
        for (int o = 1; o < blk.numOutputs(); ++o)
        {
            const auto rt = blk.getRoute (o);
            const bool mixHere = rt.home && ! blk.extrasInMain();
            if (rt.dest < 0 && ! mixHere) continue;
            if (! blk.readOutput (o, extraOut, n)) continue;
            if (rt.dest >= 0) sendToTrack (t, rt.dest, extraOut, n, g, info);
            if (mixHere)
            {
                acc.addFrom (0, 0, extraOut, 0, 0, n, g);
                acc.addFrom (1, 0, extraOut, 1, 0, n, g);
            }
        }
    };

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
                feedNow.ownTrack = t;
                b.setTapeFeed (&feedNow);
                b.process (audio, n, midiWork);
                if (b.producesMidi())
                    trackMidi.swapWith (midiWork);
            };

            switch (role)
            {
                case BlockRole::pending:
                case BlockRole::empty:
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
                        auto range = juce::FloatVectorOperations::findMinAndMax (scratch.getReadPointer (0), n);
                        noteSend (b, std::max (-range.getStart(), range.getEnd()));
                        r.throughInterface = true;
                        const auto rt = b.getRoute (0);
                        if (rt.dest >= 0)
                        {
                            extraOut.clear (0, n);
                            extraOut.addFrom (0, 0, scratch, 0, 0, n, side);
                            extraOut.addFrom (1, 0, scratch, 0, 0, n, side);
                            auto info = r;
                            info.hasInstruments = true;
                            sendToTrack (t, rt.dest, extraOut, n, 1.0f, info);
                        }
                        if (rt.home)
                        {
                            acc.addFrom (0, 0, scratch, 0, 0, n, side);
                            acc.addFrom (1, 0, scratch, 0, 0, n, side);
                        }
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
                        const auto g = dbToGain (b.levelDb.load());
                        noteSend (b, sendOf (scratch, g));
                        auto info = r;
                        info.hasInstruments = true;
                        info.latency += b.latencySamples();
                        // its main output: here, sent to another track, or both
                        const auto rt = b.getRoute (0);
                        if (rt.dest >= 0) sendToTrack (t, rt.dest, scratch, n, g, info);
                        if (rt.home)
                        {
                            acc.addFrom (0, 0, scratch, 0, 0, n, g);
                            acc.addFrom (1, 0, scratch, 1, 0, n, g);
                        }
                        sendExtras (b, g, info);
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
                    r.latency += b.latencySamples();
                    if (role == BlockRole::instrument)
                    {
                        const auto g = dbToGain (b.levelDb.load());
                        acc.applyGain (0, n, g);
                        noteSend (b, sendOf (acc, 1.0f));
                        r.hasInstruments = true;
                        // processing what reaches it: its main output is the chain's sound from here
                        const auto rt = b.getRoute (0);
                        if (rt.dest >= 0) sendToTrack (t, rt.dest, acc, n, 1.0f, r);
                        if (! rt.home) acc.clear (0, n);
                        sendExtras (b, g, r);
                    }
                    break;
                }
            }
        }
    }

    // ---- tracks are mono ----
    auto* l = acc.getReadPointer (0);
    auto* rr = acc.getReadPointer (1);
    for (int i = 0; i < n; ++i)
        monoOut[i] = (l[i] + rr[i]) * side;
    return r;
}

void AudioEngine::sendToTrack (int from, int dest, const juce::AudioBuffer<float>& stereo, int n, float gain, const ChainResult& info)
{
    // Only to a track that runs later in this block (the order puts every track after those feeding it).
    if (! juce::isPositiveAndBelow (dest, numTracks) || orderPos[dest] <= orderPos[from]) return;
    constexpr float side = 0.70710678f;
    auto* d = routeIn[dest].data();
    const auto* l = stereo.getReadPointer (0);
    const auto* r = stereo.getReadPointer (1);
    const float g = gain * side;
    for (int i = 0; i < n; ++i) d[i] += (l[i] + r[i]) * g;
    auto& f = routeFrom[dest];
    if (! routeUsed[dest]) f = info;
    else
    {
        f.throughInterface = f.throughInterface || info.throughInterface;
        f.hasInstruments = f.hasInstruments || info.hasInstruments;
        f.latency = std::max (f.latency, info.latency);
    }
    routeUsed[dest] = true;
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

    // The electronics' own noise (the machine's .deck): re 0 VU, a sine of Dolby level's peak.
    const auto vuRms = tapeRefPeak / std::sqrt (2.0f);
    auto noiseOf = [vuRms] (float db) { return db > -139.0f ? vuRms * dbToGain (db) : 0.0f; };   // -140: none (the tests)
    const auto chNoise = noiseOf (channelNoiseDb.load()), busNoise = noiseOf (busNoiseDb.load());
    auto controls = [&] (int t) -> ChannelStrip::Controls
    {
        auto& p = track[t];
        return { -20.0f + 60.0f * p.gain.load(), p.low.load(), p.mid.load(), (float) midFreqHz (p.midFreq.load()),
                 p.high.load(), faderGain (p.fader.load()), p.pan.load(), armed[t], chNoise };
    };

    // A chain is only heard while its track is armed, or fading back to tape after. Otherwise it's
    // left alone (its plug-ins carry on from where they were when the track is armed again). It runs
    // one more block after its MIDI stops, so its notes-off reach every block and hardware synth.
    // An instrument whose window is open is heard too, armed or not, so it can be played from there.
    bool auditioning[numTracks] {};
    for (int t = 0; t < numTracks; ++t)
    {
        const juce::SpinLock::ScopedTryLockType cl (chains[t].lock);
        if (cl.isLocked())
            for (auto& b : chains[t].blocks) if (b->auditions()) { auditioning[t] = true; break; }
    }
    auto chainHeard = [&] (int t) { return armed[t] || strips[t].liveAmount() > 0.0f || wasReceiving[t] || auditioning[t]; };

    // ---- the routes: which tracks feed which (instruments' outputs, chain outs) ----
    const int edges = routeEdges.load();
    auto edge = [edges] (int a, int b) { return (edges & (1 << (a * numTracks + b))) != 0; };
    // The running order: the live tracks, then the bounces, each after every track that feeds it.
    int order[numTracks] {}, placed = 0, done = 0;
    for (int phase = 0; phase < 2; ++phase)
    {
        for (bool progress = true; progress;)
        {
            progress = false;
            for (int t = 0; t < numTracks; ++t)
            {
                if ((done & (1 << t)) || bounce[t] != (phase == 1)) continue;
                bool ready = true;
                for (int k = 0; k < numTracks; ++k)
                    if (k != t && edge (k, t) && ! (done & (1 << k)) && bounce[k] == (phase == 1)) ready = false;
                if (ready) { order[placed++] = t; done |= 1 << t; progress = true; }
            }
        }
        for (int t = 0; t < numTracks; ++t)   // a loop can't be made; if one were, index order
            if (! (done & (1 << t)) && bounce[t] == (phase == 1)) { order[placed++] = t; done |= 1 << t; }
    }
    for (int i = 0; i < numTracks; ++i) orderPos[order[i]] = i;
    // A chain runs while a track it feeds is heard, and hears its controller while one is armed.
    bool needed[numTracks] {}, midiArmed[numTracks] {};
    for (int i = numTracks - 1; i >= 0; --i)
    {
        const int t = order[i];
        needed[t] = chainHeard (t);
        midiArmed[t] = armed[t];
        for (int d = 0; d < numTracks; ++d)
            if (edge (t, d) && orderPos[d] > i) { needed[t] = needed[t] || needed[d]; midiArmed[t] = midiArmed[t] || midiArmed[d]; }
    }
    for (int t = 0; t < numTracks; ++t)
    {
        std::fill_n (routeIn[t].begin(), n, 0.0f);
        routeUsed[t] = false;
        routeFrom[t] = {};
    }

    // What a sampler can take: the tape now, and each track's output from the block before.
    if (feedLength < n)
        for (int t = 0; t < numTracks; ++t)
        {
            std::fill (feedPre[t].begin() + feedLength, feedPre[t].begin() + n, 0.0f);
            std::fill (feedPost[t].begin() + feedLength, feedPost[t].begin() + n, 0.0f);
        }
    feedNow.numTracks = numTracks;
    for (int t = 0; t < numTracks; ++t)
    {
        feedNow.tape[t] = tapeOutPtr[t];
        feedNow.preFader[t] = feedPre[t].data();
        feedNow.postFader[t] = feedPost[t].data();
        feedNow.heard[t] = feedHeard[t];
    }
    auto runOrRest = [&] (int t, const float* bounceIn)
    {
        if (needed[t])
        {
            chainResult[t] = runChain (t, inputs, numInputs, n, midiArmed[t], src[t].data(), bounceIn);
            // its finished sound, copied to the tracks it feeds
            const int outs = track[t].chainOutMask.load();
            for (int d = 0; d < numTracks; ++d)
                if ((outs & (1 << d)) && edge (t, d) && orderPos[d] > orderPos[t])
                {
                    juce::FloatVectorOperations::add (routeIn[d].data(), src[t].data(), n);
                    auto& f = routeFrom[d];
                    if (! routeUsed[d]) f = chainResult[t];
                    else { f.throughInterface = f.throughInterface || chainResult[t].throughInterface;
                           f.hasInstruments = f.hasInstruments || chainResult[t].hasInstruments;
                           f.latency = std::max (f.latency, chainResult[t].latency); }
                    routeUsed[d] = true;
                }
        }
        else
        {
            std::fill_n (src[t].begin(), n, 0.0f);
            chainResult[t] = {};
        }
        auto c = controls (t);
        c.audition = auditioning[t] && ! armed[t];
        strips[t].process (tapeOut[t].data(), src[t].data(), chan[t].data(), n, c, pre[t].data());
    };

    // ---- live tracks, in their running order: their chains, then their strips ----
    for (int i = 0; i < numTracks; ++i)
        if (! bounce[order[i]])
            runOrRest (order[i], nullptr);

    // ---- bounce tracks: the other tracks off tape, through their strips (mute silences a source) ----
    for (int i = 0; i < numTracks; ++i)
    {
        const int t = order[i];
        if (! bounce[t]) continue;
        if (needed[t])
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
        feedHeard[t] = heard;
        std::copy (pre[t].begin(), pre[t].begin() + n, feedPre[t].begin());
        std::copy (c, c + n, feedPost[t].begin());
        // A bounce destination is fed by the other tracks, which you already hear: it leaves the
        // mix as it goes live.
        auto gain = heard ? (bounce[t] ? 1.0f - strips[t].liveAmount() : 1.0f) : 0.0f;
        strips[t].addToMix (c, mixL.data(), mixR.data(), n, track[t].pan.load(), gain);
    }
    feedLength = n;

    // ---- the master bus: after the master fader, it's what you hear and what a mixdown takes ----
    auto mg = faderGain (masterFader.load());
    for (int i = 0; i < n; ++i)
    {
        auto g = lastMaster + (mg - lastMaster) * (float) i / (float) n;
        // the mix bus and outputs: their own noise, the same whatever the faders and pans do
        masterL[(size_t) i] = mixL[(size_t) i] * g + busNoise * 1.7320508f * busRngL.next();
        masterR[(size_t) i] = mixR[(size_t) i] * g + busNoise * 1.7320508f * busRngR.next();
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
