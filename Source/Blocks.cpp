#include "Blocks.h"
#include "ElectricPiano.h"
#include "TransistorOrgan.h"
#include "PhraseSampler.h"
#include "SpringReverb.h"
#include "N5Synth.h"
#include "FmDrums.h"
#include "StepSeq.h"
#include "Nymphes.h"
#include "TapeEcho.h"
#include "Plate.h"

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
        case BlockRole::empty:      return "EMPTY SLOT";
    }
    return "";
}

void Block::writeCommon (juce::XmlElement& x) const
{
    x.setAttribute ("bypass", bypass.load());
    x.setAttribute ("level", levelDb.load());
    x.setAttribute ("mix", mix.load());
    x.setAttribute ("process", processInput.load());
    // where its outputs go, where that isn't as made
    for (int o = 0; o < maxOutputs; ++o)
        if (outDest[o].load() >= 0 || outHome[o].load() >= 0)
        {
            auto* r = x.createNewChildElement ("OUT");
            r->setAttribute ("index", o);
            r->setAttribute ("dest", outDest[o].load());
            if (outHome[o].load() >= 0) r->setAttribute ("home", outHome[o].load() != 0);
        }
}

Block::Route Block::getRoute (int o) const noexcept
{
    if (o < 0 || o >= maxOutputs) return {};
    Route r;
    r.dest = outDest[o].load();
    const int h = outHome[o].load();
    r.home = h >= 0 ? h != 0 : (o == 0 || extrasInMain());
    return r;
}

void Block::setRoute (int o, int destTrack, bool home)
{
    if (o < 0 || o >= maxOutputs) return;
    outDest[o] = destTrack;
    // as made: the main output and a built-in's voices are here; a plug-in's aux buses aren't
    const bool asMade = o == 0 || extrasInMain();
    outHome[o] = home == asMade ? -1 : (home ? 1 : 0);
    routeChanged (o);
}

void Block::readCommon (const juce::XmlElement& x)
{
    bypass = x.getBoolAttribute ("bypass");
    levelDb = (float) juce::jlimit (-60.0, 12.0, x.getDoubleAttribute ("level", levelDb.load()));
    mix = (float) juce::jlimit (0.0, 1.0, x.getDoubleAttribute ("mix", mix.load()));
    processInput = x.getBoolAttribute ("process");
    for (auto* r : x.getChildWithTagNameIterator ("OUT"))
    {
        const int o = r->getIntAttribute ("index", -1);
        if (o < 0 || o >= maxOutputs) continue;
        const bool asMade = o == 0 || extrasInMain();
        setRoute (o, juce::jlimit (-1, 7, r->getIntAttribute ("dest", -1)), r->getBoolAttribute ("home", asMade));
    }
}


//==============================================================================
ProcessorBlock::ProcessorBlock (std::unique_ptr<juce::AudioProcessor> p, Kind k, int index)
    : processor (std::move (p)), kind (k), builtInIndex (index)
{
    // A plug-in instrument's extra outputs (a drum sampler's separate outs, say) are switched on, so
    // they can be sent to other tracks. Most plug-ins leave them silent until a sound is assigned to one.
    if (kind == Kind::hosted)
        if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (processor.get()); inst != nullptr && inst->getPluginDescription().isInstrument)
            for (int i = 1; i < processor->getBusCount (false); ++i)
                if (auto* bus = processor->getBus (false, i); bus != nullptr && ! bus->isEnabled())
                    bus->enable (true);
    multiOut = dynamic_cast<MultiOutSource*> (processor.get());
    findOutputs();

    if (kind == Kind::builtInSynth)      naturalRole = BlockRole::instrument;
    else if (kind == Kind::builtInFx)    naturalRole = processor->isMidiEffect() ? BlockRole::midiFx : BlockRole::audioFx;
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
    tapeListener = dynamic_cast<TapeFeedListener*> (processor.get());
    processor->addListener (this);
}

void ProcessorBlock::findOutputs()
{
    outBuses.clear();
    int offset = 0;
    for (int i = 0; i < processor->getBusCount (false); ++i)
    {
        auto* bus = processor->getBus (false, i);
        if (bus == nullptr || ! bus->isEnabled()) continue;
        const int ch = bus->getNumberOfChannels();
        if (ch > 0 && (int) outBuses.size() < maxOutputs)
            outBuses.push_back ({ offset, ch, bus->getName() });
        offset += ch;
    }
}

juce::String ProcessorBlock::outputName (int o) const
{
    if (o == 0) return "MAIN";
    if (multiOut != nullptr) return multiOut->outputLabel (o - 1);
    if (juce::isPositiveAndBelow (o, (int) outBuses.size()) && outBuses[(size_t) o].name.isNotEmpty())
        return outBuses[(size_t) o].name.toUpperCase();
    return Block::outputName (o);
}

int ProcessorBlock::outputChannels (int o) const
{
    return juce::isPositiveAndBelow (o, (int) outBuses.size()) ? std::min (2, outBuses[(size_t) o].channels) : 2;
}

bool ProcessorBlock::readOutput (int o, juce::AudioBuffer<float>& dest, int numSamples)
{
    if (o < 1 || o >= (int) outBuses.size() || outBuses.empty() || extrasFilled < numSamples) return false;
    const auto& bus = outBuses[(size_t) o];
    const int first = bus.offset - outBuses[0].channels;
    if (first < 0 || first >= extras.getNumChannels()) return false;
    if (bus.channels == 1 || first + 1 >= extras.getNumChannels())
    {
        constexpr float side = 0.70710678f;
        dest.clear (0, numSamples);
        dest.addFrom (0, 0, extras, first, 0, numSamples, side);
        dest.addFrom (1, 0, extras, first, 0, numSamples, side);
    }
    else
    {
        dest.copyFrom (0, 0, extras, first, 0, numSamples);
        dest.copyFrom (1, 0, extras, first + 1, 0, numSamples);
    }
    return true;
}

void ProcessorBlock::routeChanged (int o)
{
    if (multiOut != nullptr && o >= 1)
        multiOut->setOutputLeavesMain (o - 1, ! getRoute (o).home);
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
    findOutputs();
    const int mainCh = outBuses.empty() ? 0 : outBuses[0].channels;
    const int extraCh = processor->getTotalNumOutputChannels() - mainCh;
    extras.setSize (std::max (0, extraCh), extraCh > 0 ? 8192 : 0);   // the engine's longest block
    extrasFilled = 0;
    pieceMidi.ensureSize (8192);
    midiOut.ensureSize (8192);
    prepared = true;
}

void ProcessorBlock::applyMpe (bool on, int bendRange) noexcept
{
    if (auto* e = dynamic_cast<ep::Processor*> (processor.get()))
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

    const auto ins = processor->getTotalNumInputChannels();
    const int allOuts = processor->getTotalNumOutputChannels();
    const int outs = outBuses.empty() ? allOuts : outBuses[0].channels;   // the main bus: what joins the chain
    const int mainCh = outs;
    extrasFilled = 0;
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

        if (tapeListener != nullptr)
        {
            // this piece of each track's tape and output
            TrackFeed piece;
            if (feed != nullptr)
            {
                piece = *feed;
                auto at = [start] (const float* p) { return p != nullptr ? p + start : nullptr; };
                for (int t = 0; t < piece.numTracks; ++t)
                {
                    piece.tape[t] = at (piece.tape[t]);
                    piece.preFader[t] = at (piece.preFader[t]);
                    piece.postFader[t] = at (piece.postFader[t]);
                }
            }
            tapeListener->tapeFeed (piece);
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
        // the extra outputs, kept for the engine to send on
        for (int c = mainCh; c < allOuts && c - mainCh < extras.getNumChannels() && start + len <= extras.getNumSamples(); ++c)
            extras.copyFrom (c - mainCh, start, view, c, 0, len);
        if (extras.getNumChannels() > 0 && start + len <= extras.getNumSamples()) extrasFilled = start + len;

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
juce::String BuiltIn::synthName (int which) { return which == epiano ? "TF ELECTRIC PIANO" : which == sampler ? "TF PHRASE SAMPLER" : which == n5 ? "TF N5" : which == fmDrums ? "TF FM DRUMS" : "TF TRANSISTOR"; }

juce::String BuiltIn::fxName (int which)
{
    switch (which)
    {
        case spring:   return "TF SPRING REVERB";
        case tapeEcho: return "TF TAPE ECHO";
        case stepSeq:  return "TF STEP";
        case nymphes:  return "TF NYMPHES";
        default:       return "TF PLATE";
    }
}

std::unique_ptr<Block> BuiltIn::createSynth (int which)
{
    which = juce::jlimit (0, numSynths - 1, which);
    if (which == epiano)
        return std::make_unique<ProcessorBlock> (std::make_unique<ep::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
    if (which == n5)
        return std::make_unique<ProcessorBlock> (std::make_unique<n5::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
    if (which == fmDrums)
        return std::make_unique<ProcessorBlock> (std::make_unique<fmd::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
    if (which == sampler)
    {
        // it samples the track's input, so it takes it (and plays it on in CROSSFADER and the EXTERNAL modes)
        auto b = std::make_unique<ProcessorBlock> (std::make_unique<sampler::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
        b->processInput = true;
        return b;
    }
    return std::make_unique<ProcessorBlock> (std::make_unique<organ::Processor>(), ProcessorBlock::Kind::builtInSynth, which);
}

std::unique_ptr<Block> BuiltIn::createFx (int which)
{
    which = juce::jlimit (0, numFx - 1, which);
    std::unique_ptr<juce::AudioProcessor> p;
    if (which == spring)        p = std::make_unique<spring::Processor>();
    else if (which == tapeEcho) p = std::make_unique<echo::Processor>();
    else if (which == stepSeq)  return std::make_unique<ProcessorBlock> (std::make_unique<step::Processor>(), ProcessorBlock::Kind::builtInFx, which);
    else if (which == nymphes)  return std::make_unique<ProcessorBlock> (std::make_unique<nym::Processor>(), ProcessorBlock::Kind::builtInFx, which);
    else                        p = std::make_unique<plate::Processor>();
    auto b = std::make_unique<ProcessorBlock> (std::move (p), ProcessorBlock::Kind::builtInFx, which);
    b->mix = which == tapeEcho ? 0.35f : 0.3f;   // send-like by default
    return b;
}

tf::organ::Processor* BuiltIn::transistorOrgan (Block* b) noexcept
{
    auto* pb = dynamic_cast<ProcessorBlock*> (b);
    return pb != nullptr ? dynamic_cast<organ::Processor*> (pb->getProcessor()) : nullptr;
}

sampler::Processor* BuiltIn::phraseSampler (Block* b) noexcept
{
    auto* pb = dynamic_cast<ProcessorBlock*> (b);
    return pb != nullptr ? dynamic_cast<sampler::Processor*> (pb->getProcessor()) : nullptr;
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
