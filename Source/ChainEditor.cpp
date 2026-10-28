#include "ChainEditor.h"
#include "BlockPicker.h"

namespace tf
{
namespace
{
    const juce::Colour frame { 0xff3d3b36 };
    constexpr int pad = 16, flowTop = 48, laneH = 18, nodeH = 60, linkW = 14;
}
//==============================================================================
ChainEditor::ChainEditor (AudioEngine& e, int track, int notch)
    : engine (e), t (track), notchX (notch)
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    closeX.onClick = [this] { if (onClose) onClose(); };
    closeX.setTooltip ("Close (Esc)");
    addAndMakeVisible (closeX);
    rebuild();
    startTimerHz (15);
}

ChainEditor::~ChainEditor()
{
    if (learning) engine.mapper.cancelLearn();
}

const AudioEngine::BlockInfo* ChainEditor::selected() const
{
    for (auto& b : chain) if (b.id == selId) return &b;
    return nullptr;
}

juce::String ChainEditor::signature() const
{
    auto& p = engine.track[t];
    juce::String s;
    s << p.feed.load() << "|" << p.inputChannel.load() << (int) p.inputPair.load() << "|" << p.midiPort.load() << "|" << p.bounceMask.load() << "|";
    for (auto& b : engine.getChain (t))
        s << b.id << (int) b.role << (int) b.bypass << (int) b.processInput << b.outPort << b.channel << b.returnInput << (int) b.returnPair << (int) b.missing << ",";
    for (auto& m : engine.mapper.getMappings()) s << m.id << ".";
    for (auto& i : engine.getMidiInputs()) s << i.slot << (int) i.active << (int) i.mpe;
    for (auto& o : engine.getMidiOutputs()) s << o.slot << (int) o.active;
    s << engine.getActiveInputNames().size() << (int) sel << selId << (int) learning;
    return s;
}

void ChainEditor::changed()
{
    ++engine.viewVersion;   // the track display shows some of this
    engine.saveSettings();
    juce::Component::SafePointer<ChainEditor> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuild(); });
}

void ChainEditor::timerCallback()
{
    // signal bars on the input chips
    for (auto& [c, ch] : inputChips)
    {
        auto level = juce::isPositiveAndBelow (ch, AudioEngine::maxInputs) ? engine.inputPeak[ch].exchange (0.0f) : 0.0f;
        auto v = juce::jlimit (0.0f, 1.0f, 1.0f + juce::Decibels::gainToDecibels (level, -60.0f) / 60.0f);
        c->signal = std::max (v, c->signal * 0.8f);
        c->repaint();
    }

    // MIDI learn: a parameter touched in the plug-in's window is the one to learn
    if (learning && sel == Sel::block)
        if (auto* b = engine.findBlock (t, selId))
            if (auto touched = b->takeTouchedParameter(); touched >= 0)
            {
                if (params != nullptr) params->highlight (touched);
                pickParameter (touched);
            }
    if (learning && learnParam >= 0 && ! engine.mapper.isLearning())
    {
        learning = false;
        learnParam = -1;
    }

    if (signature() != lastSignature)
        rebuild();
}

void ChainEditor::pickParameter (int index)
{
    auto* b = engine.findBlock (t, selId);
    if (b == nullptr || b->getProcessor() == nullptr) return;
    auto ps = b->getProcessor()->getParameters();
    if (! juce::isPositiveAndBelow (index, ps.size())) return;
    learnParam = index;
    MidiMapper::Target target;
    target.plugin = true;
    target.track = t;
    target.blockId = selId;
    target.param = index;
    engine.mapper.beginLearn (target, ps[index], ps[index]->getName (40));
    if (onMessage) onMessage ("LEARN: MOVE A CONTROL FOR " + ps[index]->getName (24).toUpperCase());
}

//==============================================================================
void ChainEditor::rebuild()
{
    chain = engine.getChain (t);
    if (sel == Sel::block && selected() == nullptr) { sel = Sel::inputs; selId = 0; learning = false; }
    lastSignature = signature();
    layoutFlow();
    clearPanel();

    auto area = juce::Rectangle<int> (pad, notchHeight + flowTop + 2 * laneH + nodeH + 24, editorWidth - 2 * pad,
                                      notchHeight + editorHeight - 14 - (notchHeight + flowTop + 2 * laneH + nodeH + 24));
    if (sel == Sel::inputs)
        buildInputsPanel (area);
    else if (sel == Sel::add)
        buildPickerPanel (area, false);
    else if (auto* b = selected())
    {
        if (b->role == BlockRole::external) buildExternalPanel (*b, area);
        else                                buildBlockPanel (*b, area);
    }
    resized();
    repaint();
}

void ChainEditor::clearPanel()
{
    picker.reset();
    params.reset();
    panel.clear();
    captions.clear();
    inputChips.clear();
}

LcdChip* ChainEditor::chip (const juce::String& text, bool chosen, std::function<void()> action, const juce::String& sub)
{
    auto* c = new LcdChip (text, sub);
    c->chosen = chosen;
    c->onClick = [this, action] { action(); changed(); };
    panel.add (c);
    addAndMakeVisible (c);
    return c;
}

int ChainEditor::chipRow (int y, int x, int right, const juce::String& caption, const std::vector<LcdChip*>& chips, int chipH, juce::Colour captionColour)
{
    const int labelW = 110;
    captions.push_back ({ caption, { x, y, labelW, chipH }, captionColour });
    int cx = x + labelW, cy = y;
    for (auto* c : chips)
    {
        auto w = std::min (c->idealWidth(), right - (x + labelW));
        if (cx + w > right) { cx = x + labelW; cy += chipH + 6; }
        c->setBounds (cx, cy, w, chipH);
        cx += w + 6;
    }
    return cy + chipH + 10;
}

void ChainEditor::buildInputsPanel (juce::Rectangle<int> area)
{
    auto& p = engine.track[t];
    const bool bounce = (AudioEngine::Feed) p.feed.load() == AudioEngine::Feed::bounce;
    captions.push_back ({ utf ("SELECTED · TRACK INPUTS"), area.removeFromTop (18), Pal::lcdDim });
    int y = area.getY() + 4;
    const int x = area.getX(), right = area.getRight();

    y = chipRow (y, x, right, "FEED", {
        chip ("LIVE INPUTS", ! bounce, [this] { engine.setTrackFeed (t, AudioEngine::Feed::live); }),
        chip ("BOUNCE FROM TAPE", bounce, [this] { engine.setTrackFeed (t, AudioEngine::Feed::bounce); }) });

    if (bounce)
    {
        std::vector<LcdChip*> tracks;
        for (int s = 0; s < numTracks; ++s)
            if (s != t)
                tracks.push_back (chip ("TRACK " + juce::String (s + 1), (p.bounceMask.load() & (1 << s)) != 0,
                                        [this, s] { engine.toggleBounceSource (t, s); }));
        y = chipRow (y, x, right, "MIX FROM TAPE", tracks);
        captions.push_back ({ "SOURCES PLAY THROUGH THEIR OWN STRIPS; A MUTED TRACK IS LEFT OUT. THE CHAIN STILL APPLIES.",
                              { x + 110, y, right - x - 110, 18 }, Pal::lcdDim });
        return;
    }

    auto port = p.midiPort.load();
    std::vector<LcdChip*> ctl {
        chip ("NONE", port == AudioEngine::midiNone, [this] { engine.setTrackMidiPort (t, AudioEngine::midiNone); }),
        chip ("ANY", port == AudioEngine::midiAny, [this] { engine.setTrackMidiPort (t, AudioEngine::midiAny); }) };
    for (auto& in : engine.getMidiInputs())
    {
        if (! in.active) continue;
        auto label = (in.slot == AudioEngine::computerKeyboardPort ? juce::String ("COMPUTER KEYS") : in.name.toUpperCase()) + (in.mpe ? " MPE" : "");
        auto slot = in.slot;
        ctl.push_back (chip (label, port == slot, [this, slot] { engine.setTrackMidiPort (t, slot); }));
    }
    y = chipRow (y, x, right, "PLAYED BY", ctl, 30, Pal::midiBright);

    auto names = engine.getActiveInputNames();
    auto ch = p.inputChannel.load();
    auto pair = p.inputPair.load();
    std::vector<LcdChip*> ins { chip ("NONE", ch < 0, [this] { engine.setTrackInput (t, -1); }) };
    for (int i = 0; i < names.size() && i < AudioEngine::maxInputs; ++i)
    {
        auto* c = chip ("IN " + juce::String (i + 1), ch == i && ! pair, [this, i] { engine.setTrackInput (t, i, false); }, names[i].toUpperCase());
        c->signal = 0.0f;
        inputChips.push_back ({ c, i });
        ins.push_back (c);
    }
    for (int i = 0; i + 1 < names.size() && i + 1 < AudioEngine::maxInputs; i += 2)
        ins.push_back (chip ("IN " + juce::String (i + 1) + "+" + juce::String (i + 2), ch == i && pair,
                             [this, i] { engine.setTrackInput (t, i, true); }, "MONO SUM"));
    if (names.isEmpty())
        ins.push_back (chip ("NO INPUTS ACTIVE: SEE SETUP", false, [] {}));
    chipRow (y, x, right, "AUDIO IN", ins, 40);
}

void ChainEditor::buildBlockPanel (const AudioEngine::BlockInfo& b, juce::Rectangle<int> area)
{
    auto left = area.removeFromLeft (310);
    area.removeFromLeft (18);
    const int x = left.getX();
    int y = left.getY();
    const int id = b.id;
    int index = 0;
    for (int i = 0; i < (int) chain.size(); ++i) if (chain[(size_t) i].id == id) index = i + 1;

    captions.push_back ({ utf ("SELECTED · BLOCK ") + juce::String (index) + utf (" \xc2\xb7 ") + roleName (b.role), { x, y, 310, 16 }, Pal::lcdDim });
    y += 18;
    if (b.role == BlockRole::pending)
    {
        captions.push_back ({ (b.missing ? "MISSING: " : "LOADING ") + b.name.toUpperCase(), { x, y, 310, 24 }, Pal::lcdText });
        y += 26;
        captions.push_back ({ b.missing ? utf ("RESCAN IN SETUP › PLUG-INS, OR PICK A REPLACEMENT. ITS SETTINGS ARE KEPT UNTIL THEN.")
                                        : "ITS SETTINGS ARE APPLIED WHEN IT ARRIVES.", { x, y, 310, 34 }, Pal::lcdDim });
        y += 40;
        auto* rm = chip ("REMOVE", false, [this, id] { engine.removeBlock (t, id); });
        rm->setBounds (x, y, rm->idealWidth(), 30);
        buildPickerPanel (area, true);
        return;
    }

    captions.push_back ({ b.name.toUpperCase(), { x, y, 310, 24 }, Pal::lcdText });
    y += 24;
    captions.push_back ({ b.maker.toUpperCase() + (b.hosted ? utf (" · PLUG-IN") : juce::String()), { x, y, 310, 16 }, Pal::lcdDim });
    y += 22;

    std::vector<LcdChip*> buttons;
    if (b.hasEditor)
        buttons.push_back (chip ("EDIT", true, [this, id] { if (onEditBlock) onEditBlock (t, id); }));
    buttons.push_back (chip ("BYPASS", b.bypass, [this, id, by = b.bypass] { engine.setBlockBypass (t, id, ! by); }));
    auto* blk = engine.findBlock (t, id);
    const bool hasParams = blk != nullptr && blk->getProcessor() != nullptr && ! blk->getProcessor()->getParameters().isEmpty();
    if (hasParams)
        buttons.push_back (chip ("LEARN", learning, [this] { learning = ! learning; learnParam = -1; if (! learning) engine.mapper.cancelLearn(); }));
    buttons.push_back (chip ("REMOVE", false, [this, id] { engine.removeBlock (t, id); }));
    int bx = x;
    for (auto* c : buttons) { auto w = c->idealWidth(); c->setBounds (bx, y, w, 30); bx += w + 6; }
    y += 40;

    if (b.role == BlockRole::instrument || b.role == BlockRole::audioFx)
    {
        const bool isMix = b.role == BlockRole::audioFx;
        auto* s = new LcdSlider (isMix ? "MIX" : "LEVEL", isMix ? 0.0 : -24.0, isMix ? 1.0 : 12.0, isMix ? 1.0 : 0.0,
                                 [isMix] (double v) { return isMix ? juce::String (juce::roundToInt (v * 100.0)) + "%" : (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; });
        s->setValue (isMix ? b.mix : b.levelDb);
        s->onChange = [this, id, isMix] (double v)
        {
            if (auto* bb = engine.findBlock (t, id)) { if (isMix) bb->mix = (float) v; else bb->levelDb = (float) v; }
        };
        panel.add (s);
        addAndMakeVisible (s);
        s->setBounds (x, y, 310, 22);
        y += 30;
    }
    if (b.canProcessInput)
    {
        auto* mixIn = chip ("MIX IN", ! b.processInput, [this, id] { if (auto* bb = engine.findBlock (t, id)) bb->processInput = false; });
        auto* proc = chip ("PROCESS INPUT", b.processInput, [this, id] { if (auto* bb = engine.findBlock (t, id)) bb->processInput = true; });
        mixIn->setBounds (x, y, mixIn->idealWidth(), 26);
        proc->setBounds (x + mixIn->idealWidth() + 6, y, proc->idealWidth(), 26);
        y += 34;
    }

    // MIDI mappings of this block's parameters
    std::vector<MidiMapper::Mapping> mine;
    for (auto& m : engine.mapper.getMappings())
        if (m.target.plugin && m.target.track == t && m.target.blockId == id) mine.push_back (m);
    if (! mine.empty())
    {
        captions.push_back ({ "MIDI CONTROL", { x, y, 310, 16 }, Pal::lcdDim });
        y += 18;
        for (auto& m : mine)
        {
            if (y > left.getBottom() - 26) break;
            captions.push_back ({ m.paramName.toUpperCase().substring (0, 18) + utf (" \xe2\x86\x90 ") + m.portName.toUpperCase().substring (0, 10)
                                      + " " + engine.mapper.describeMessage (m), { x, y, 280, 24 }, Pal::midiBright });
            auto mapId = m.id;
            auto* xb = chip ("X", false, [this, mapId] { engine.mapper.remove (mapId); });
            xb->setBounds (x + 284, y, 26, 24);
            y += 28;
        }
    }

    if (learning && blk != nullptr && blk->getProcessor() != nullptr)
    {
        params = std::make_unique<ParamPicker> (*blk->getProcessor());
        params->onPick = [this] (int i) { pickParameter (i); };
        addAndMakeVisible (*params);
        params->setBounds (area);
        if (learnParam >= 0) params->highlight (learnParam);
    }
    else
    {
        buildPickerPanel (area, true);
    }
}

void ChainEditor::buildExternalPanel (const AudioEngine::BlockInfo& b, juce::Rectangle<int> area)
{
    const int id = b.id;
    auto top = area.removeFromTop (30);
    captions.push_back ({ utf ("SELECTED \xc2\xb7 EXTERNAL INSTRUMENT \xc2\xb7 ") + b.name.toUpperCase(), top.removeFromLeft (360), Pal::lcdDim });
    for (auto* c : { chip ("REMOVE", false, [this, id] { engine.removeBlock (t, id); }),
                     chip ("ALL NOTES OFF", false, [this, id] { engine.sendAllNotesOff (t, id); }),
                     chip ("BYPASS", b.bypass, [this, id, by = b.bypass] { engine.setBlockBypass (t, id, ! by); }) })
    {
        c->setBounds (top.removeFromRight (c->idealWidth()));
        top.removeFromRight (6);
    }
    auto* level = new LcdSlider ("LEVEL", -24.0, 12.0, 0.0, [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; });
    level->setValue (b.levelDb);
    level->onChange = [this, id] (double v) { if (auto* bb = engine.findBlock (t, id)) bb->levelDb = (float) v; };
    panel.add (level);
    addAndMakeVisible (level);
    level->setBounds (top.reduced (0, 4).withTrimmedRight (10));
    area.removeFromTop (10);

    auto col = area.getWidth() / 3;
    auto c1 = area.removeFromLeft (col - 12); area.removeFromLeft (12);
    auto c2 = area.removeFromLeft (col - 12); area.removeFromLeft (12);
    auto c3 = area;
    auto ext = [this, id] { return dynamic_cast<ExternalBlock*> (engine.findBlock (t, id)); };

    captions.push_back ({ "SEND MIDI TO", c1.removeFromTop (18), Pal::midiBright });
    int shown = 0;
    for (auto& o : engine.getMidiOutputs())
    {
        if (! o.active) continue;
        auto slot = o.slot;
        auto* c = chip (engine.midiOutputLabel (slot), b.outPort == slot,
                        [this, ext, slot] { if (auto* e = ext()) { engine.sendAllNotesOff (t, e->id); e->outPort = slot; } },
                        o.name.toUpperCase());
        c->setBounds (c1.removeFromTop (38));
        c1.removeFromTop (6);
        ++shown;
    }
    if (shown == 0)
        captions.push_back ({ utf ("TICK A MIDI OUTPUT IN SETUP › MIDI & DECK"), c1.removeFromTop (30), Pal::lcdDim });

    captions.push_back ({ "ON CHANNEL", c2.removeFromTop (18), Pal::midiBright });
    auto grid = c2.removeFromTop (4 * 30 + 3 * 4);
    for (int ch = 1; ch <= 16; ++ch)
    {
        auto* c = chip (juce::String (ch), b.channel == ch, [ext, ch] { if (auto* e = ext()) e->channel = ch; });
        auto cw = (grid.getWidth() - 3 * 4) / 4;
        c->setBounds (grid.getX() + ((ch - 1) % 4) * (cw + 4), grid.getY() + ((ch - 1) / 4) * 34, cw, 30);
    }
    c2.removeFromTop (6);
    chip ("AS PLAYED (MPE)", b.channel == 0, [ext] { if (auto* e = ext()) e->channel = 0; })->setBounds (c2.removeFromTop (28));

    captions.push_back ({ "AUDIO BACK ON", c3.removeFromTop (18), Pal::lcdDim });
    auto names = engine.getActiveInputNames();
    auto place = [&] (LcdChip* c) { c->setBounds (c3.removeFromTop (30)); c3.removeFromTop (5); };
    place (chip ("NONE", b.returnInput < 0, [ext] { if (auto* e = ext()) e->returnInput = -1; }));
    for (int i = 0; i < names.size() && c3.getHeight() >= 30; ++i)
    {
        auto* c = chip ("IN " + juce::String (i + 1) + "  " + names[i].toUpperCase(), b.returnInput == i && ! b.returnPair,
                        [ext, i] { if (auto* e = ext()) { e->returnInput = i; e->returnPair = false; } });
        c->signal = 0.0f;
        inputChips.push_back ({ c, i });
        place (c);
    }
    for (int i = 0; i + 1 < names.size() && c3.getHeight() >= 30; i += 2)
        place (chip ("IN " + juce::String (i + 1) + "+" + juce::String (i + 2) + "  MONO SUM", b.returnInput == i && b.returnPair,
                     [ext, i] { if (auto* e = ext()) { e->returnInput = i; e->returnPair = true; } }));
}

void ChainEditor::buildPickerPanel (juce::Rectangle<int> area, bool replacing)
{
    auto* cur = replacing ? selected() : nullptr;
    picker = std::make_unique<BlockPicker> (engine, cur != nullptr ? cur->name : juce::String(),
                                            replacing ? utf ("REPLACE WITH \xe2\x80\xba") : utf ("ADD \xe2\x80\xba"));
    const int replaceId = replacing && cur != nullptr ? cur->id : 0;
    picker->onFind = [this] { if (onFindPlugins) onFindPlugins(); };
    picker->onPick = [this, replaceId] (const BlockPicker::Item& it)
    {
        std::unique_ptr<Block> block;
        using K = BlockPicker::Kind;
        if (it.kind == K::synth)      block = BuiltIn::createSynth (it.index);
        else if (it.kind == K::fx)    block = BuiltIn::createFx (it.index);
        else if (it.kind == K::external) block = std::make_unique<ExternalBlock> (it.index);

        int newId = 0;
        if (block != nullptr)
            newId = replaceId != 0 ? engine.replaceBlock (t, replaceId, std::move (block)) : engine.insertBlock (t, -1, std::move (block));
        else
            newId = engine.addPlugin (t, -1, it.desc, {}, replaceId, [safe = juce::Component::SafePointer<ChainEditor> (this)] (const juce::String& err)
            {
                if (safe != nullptr && err.isNotEmpty() && err != "superseded" && safe->onMessage)
                    safe->onMessage ("COULD NOT LOAD: " + err.toUpperCase().substring (0, 40));
            });

        if (newId == 0)
        {
            if (onMessage) onMessage ("THE CHAIN IS FULL: " + juce::String (AudioEngine::maxBlocks) + " BLOCKS");
            return;
        }
        sel = Sel::block;
        selId = newId;
        changed();
    };
    addAndMakeVisible (*picker);
    picker->setBounds (area);
}

//==============================================================================
void ChainEditor::layoutFlow()
{
    cells.clear();
    auto& p = engine.track[t];
    const bool bounce = (AudioEngine::Feed) p.feed.load() == AudioEngine::Feed::bounce;
    const bool midiIn = ! bounce && p.midiPort.load() != AudioEngine::midiNone;
    const bool audioIn = bounce || p.inputChannel.load() >= 0;
    const bool canAdd = (int) chain.size() < AudioEngine::maxBlocks;

    const int nodes = 1 + (int) chain.size() + (canAdd ? 1 : 0) + 1;
    const int fixed = 116 + 64 + (canAdd ? 40 : 0) + (nodes - 1) * linkW;
    const int blockW = chain.empty() ? 100 : juce::jlimit (60, 136, (editorWidth - 2 * pad - fixed) / (int) chain.size());

    // Where the MIDI stream ends: the last block that listens to it.
    int lastMidi = midiIn ? 0 : -1;
    bool anyMidiFx = false;
    for (int i = 0; i < (int) chain.size(); ++i)
    {
        auto& b = chain[(size_t) i];
        if (b.bypass || b.role == BlockRole::pending) continue;
        if (b.role == BlockRole::midiFx) anyMidiFx = true;
        if (midiIn && (b.role == BlockRole::midiFx || b.role == BlockRole::instrument || b.role == BlockRole::external || b.producesMidi))
            lastMidi = i + 1;
    }

    int x = pad;
    const int y = notchHeight + flowTop + laneH;
    bool midiChanged = false, audioOn = audioIn;
    const juce::Colour none = juce::Colours::transparentBlack;

    auto midiColour = [&] { return midiChanged || ! anyMidiFx ? Pal::midiBright : Pal::midiDim; };
    auto addLink = [&] (int nodeIndex)
    {
        Cell c;
        c.node = { x, y, linkW, nodeH };
        c.midiL = c.midiR = nodeIndex < lastMidi ? midiColour() : none;
        c.audioL = c.audioR = audioOn ? Pal::lcdText : none;
        c.midiTick = c.audioTick = none;
        cells.push_back (c);
        x += linkW;
    };

    // the inputs
    {
        Cell c;
        c.inputs = true;
        c.node = { x, y, 116, nodeH };
        c.midiL = none;
        c.midiR = lastMidi > 0 ? midiColour() : none;
        c.midiTick = lastMidi > 0 ? midiColour() : none;
        c.audioL = none;
        c.audioR = audioOn ? Pal::lcdText : none;
        c.audioTick = audioOn ? Pal::lcdText : none;
        cells.push_back (c);
        x += 116;
    }

    for (int i = 0; i < (int) chain.size(); ++i)
    {
        addLink (i);
        auto& b = chain[(size_t) i];
        Cell c;
        c.blockId = b.id;
        c.node = { x, y, blockW, nodeH };
        const bool live = ! b.bypass && b.role != BlockRole::pending;
        const bool inMidi = i + 1 <= lastMidi;
        c.midiL = inMidi ? midiColour() : none;
        c.midiTick = live && inMidi && (b.role != BlockRole::audioFx || b.producesMidi) ? midiColour() : none;
        if (live && b.role == BlockRole::midiFx) { midiChanged = true; c.midiTag = "CHANGES MIDI"; }
        c.midiR = i + 1 < lastMidi ? midiColour() : none;

        c.audioL = audioOn ? Pal::lcdText : none;
        if (live && (b.role == BlockRole::instrument || b.role == BlockRole::external))
        {
            audioOn = true;
            c.audioTag = b.role == BlockRole::external ? (b.returnInput >= 0 ? "+ IN " + juce::String (b.returnInput + 1) : juce::String ("NO RETURN"))
                                                       : (b.processInput ? juce::String ("PROCESS") : juce::String ("+ MIX"));
        }
        c.audioTick = live && audioOn && b.role != BlockRole::midiFx ? Pal::lcdText : none;
        c.audioR = audioOn ? Pal::lcdText : none;
        if (b.role == BlockRole::external && live) c.midiTag = "OUT";
        cells.push_back (c);
        x += blockW;
    }

    if (canAdd)
    {
        addLink ((int) chain.size());
        Cell c;
        c.add = true;
        c.node = { x, y, 40, nodeH };
        c.midiL = c.midiR = c.midiTick = none;
        c.audioL = c.audioR = audioOn ? Pal::lcdText : none;
        c.audioTick = none;
        cells.push_back (c);
        x += 40;
    }

    addLink (999);
    Cell tape;
    tape.tape = true;
    tape.node = { x, y, 64, nodeH };
    tape.midiL = tape.midiR = tape.midiTick = none;
    tape.audioL = audioOn ? Pal::lcdText : none;
    tape.audioR = none;
    tape.audioTick = audioOn ? Pal::lcdText : none;
    cells.push_back (tape);
}

void ChainEditor::resized()
{
    closeX.setBounds (editorWidth - pad - 26, notchHeight + 12, 26, 26);
}

void ChainEditor::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto body = juce::Rectangle<float> (0, (float) notchHeight, (float) editorWidth, (float) editorHeight);

    // the card, and the notch pointing up at the track's display
    juce::Path notch;
    auto nx = (float) juce::jlimit (20, editorWidth - 20, notchX);
    notch.addTriangle (nx - 11, (float) notchHeight + 1, nx + 11, (float) notchHeight + 1, nx, 0);
    g.setColour (frame);
    g.fillPath (notch);
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (body, 7.0f);
    g.setColour (frame);
    g.fillRoundedRectangle (body.reduced (1.0f), 6.5f);
    g.setColour (Pal::lcd);
    g.fillRoundedRectangle (body.reduced (3.0f), 5.0f);

    // header
    auto head = juce::Rectangle<int> (pad, notchHeight + 12, editorWidth - 2 * pad - 36, 26);
    g.setColour (Pal::lcdText);
    g.setFont (lf.mono (15.0f));
    g.drawText ("TRACK " + juce::String (t + 1) + utf (" \xc2\xb7 SIGNAL CHAIN"), head.removeFromLeft (240), juce::Justification::centredLeft);
    auto legend = head.removeFromRight (150);
    g.setFont (lf.mono (10.0f));
    g.setColour (Pal::midiBright);
    g.fillRect (legend.getX(), legend.getCentreY(), 16, 2);
    g.drawText ("MIDI", legend.getX() + 20, legend.getY(), 40, legend.getHeight(), juce::Justification::centredLeft);
    g.setColour (Pal::lcdText);
    g.fillRect (legend.getX() + 70, legend.getCentreY(), 16, 2);
    g.drawText ("AUDIO", legend.getX() + 90, legend.getY(), 50, legend.getHeight(), juce::Justification::centredLeft);
    g.setColour (Pal::lcdDim);
    g.drawText (utf ("CLICK A BLOCK \xc2\xb7 LAMP = BYPASS \xc2\xb7 DRAG TO MOVE \xc2\xb7 DOUBLE-CLICK TO EDIT"), head, juce::Justification::centredLeft);

    // the flow: MIDI lane on top, audio lane underneath
    auto& p = engine.track[t];
    for (int i = 0; i < (int) cells.size(); ++i)
    {
        auto& c = cells[(size_t) i];
        auto r = c.node;
        const int midiY = r.getY() - laneH + 6, audioY = r.getBottom() + 10;
        auto lane = [&] (int ly, juce::Colour l, juce::Colour rr)
        {
            g.setColour (l);  g.fillRect (r.getX(), ly, r.getWidth() / 2, 2);
            g.setColour (rr); g.fillRect (r.getCentreX(), ly, r.getRight() - r.getCentreX(), 2);
        };
        lane (midiY, c.midiL, c.midiR);
        lane (audioY, c.audioL, c.audioR);
        g.setColour (c.midiTick);  g.fillRect (r.getCentreX() - 1, midiY, 2, r.getY() - midiY);
        g.setColour (c.audioTick); g.fillRect (r.getCentreX() - 1, r.getBottom(), 2, audioY - r.getBottom());

        if (c.blockId == 0 && ! c.inputs && ! c.add && ! c.tape)
            continue;   // a link

        auto rf = r.toFloat().reduced (0.5f);
        const AudioEngine::BlockInfo* b = nullptr;
        for (auto& bi : chain) if (bi.id == c.blockId) b = &bi;
        const bool isSel = (c.inputs && sel == Sel::inputs) || (c.add && sel == Sel::add) || (b != nullptr && sel == Sel::block && selId == b->id);
        const bool ghost = c.add || (b != nullptr && (b->bypass || b->role == BlockRole::pending));
        const bool beingDragged = dragging && b != nullptr && b->id == dragId;

        juce::Colour fill = juce::Colours::transparentBlack, ink = Pal::lcdText, sub = Pal::lcdDim;
        if (c.tape)      { fill = Pal::accent; ink = sub = juce::Colour (0xff1a1917); }
        else if (isSel)  { fill = Pal::lcdText; ink = sub = Pal::lcd; }
        const float alpha = (ghost && ! isSel) || beingDragged ? 0.55f : 1.0f;

        if (! fill.isTransparent())
        {
            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (rf, 3.0f);
        }
        else
        {
            g.setColour (Pal::lcdDim.withAlpha (alpha));
            if (c.inputs || c.add || (b != nullptr && b->role == BlockRole::pending))
            {
                juce::Path path;
                path.addRoundedRectangle (rf, 3.0f);
                const float dash[] { 4.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (path, path, dash, 2);
                g.fillPath (path);
            }
            else g.drawRoundedRectangle (rf, 3.0f, 1.0f);
        }

        auto in = r.reduced (7, 6);
        if (c.add)
        {
            g.setColour (isSel ? ink : Pal::lcdText.withAlpha (alpha));
            g.setFont (lf.mono (20.0f));
            g.drawText ("+", r, juce::Justification::centred);
            continue;
        }
        if (c.tape)
        {
            g.setColour (ink);
            g.setFont (lf.mono (10.0f));
            g.drawText ("TO TAPE", in.removeFromTop (12), juce::Justification::topLeft);
            g.setFont (lf.mono (14.0f));
            g.drawText ("TRK " + juce::String (t + 1), in, juce::Justification::centredLeft);
            continue;
        }
        if (c.inputs)
        {
            const bool bounce = (AudioEngine::Feed) p.feed.load() == AudioEngine::Feed::bounce;
            g.setColour (sub);
            g.setFont (lf.mono (9.5f));
            g.drawText ("INPUTS", in.removeFromTop (12), juce::Justification::topLeft);
            g.setFont (lf.mono (11.0f));
            if (bounce)
            {
                g.setColour (ink);
                g.drawFittedText (engine.describeSource (t), in, juce::Justification::centredLeft, 2, 0.8f);
            }
            else
            {
                auto port = p.midiPort.load();
                g.setColour (port == AudioEngine::midiNone ? sub : (isSel ? ink : Pal::midiBright));
                g.drawFittedText (port == AudioEngine::midiNone ? juce::String ("NO MIDI") : "MIDI " + engine.midiPortLabel (port),
                                  in.removeFromTop (in.getHeight() / 2), juce::Justification::centredLeft, 1, 0.75f);
                g.setColour (p.inputChannel.load() < 0 ? sub : ink);
                g.drawFittedText (p.inputChannel.load() < 0 ? juce::String ("NO AUDIO IN") : engine.describeInput (t),
                                  in, juce::Justification::centredLeft, 1, 0.75f);
            }
            continue;
        }

        if (b == nullptr) continue;
        // lamp: on, or bypassed
        auto lamp = juce::Rectangle<float> ((float) r.getX() + 7, (float) r.getY() + 7, 8, 8);
        if (b->role != BlockRole::pending)
        {
            if (isSel)
            {
                g.setColour (Pal::lcd);
                g.fillEllipse (lamp.expanded (1.5f));
            }
            g.setColour (b->bypass ? Pal::lcdRule : Pal::green);
            g.fillEllipse (lamp);
        }
        g.setColour (sub.withMultipliedAlpha (alpha));
        g.setFont (lf.mono (9.0f));
        auto kind = b->role == BlockRole::pending ? juce::String (b->missing ? "MISSING" : "LOADING")
                    : juce::String (roleName (b->role)) + (b->bypass ? utf (" · BYP") : juce::String());
        g.drawFittedText (kind, in.removeFromTop (12).withTrimmedLeft (b->role == BlockRole::pending ? 0 : 12), juce::Justification::centredLeft, 1, 0.7f);
        g.setColour (ink.withMultipliedAlpha (alpha));
        g.setFont (lf.mono (12.0f));
        g.drawFittedText (b->name.toUpperCase(), in, juce::Justification::centredLeft, 2, 0.75f);
    }

    // lane tags
    for (auto& c : cells)
    {
        auto tag = [&] (const juce::String& text, int y, juce::Colour bg)
        {
            if (text.isEmpty()) return;
            g.setFont (lf.mono (9.0f));
            auto w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 10;
            auto r = juce::Rectangle<int> (w, 14).withCentre ({ c.node.getCentreX(), y });
            g.setColour (bg);
            g.fillRoundedRectangle (r.toFloat(), 2.0f);
            g.setColour (juce::Colour (0xff10151c));
            g.drawText (text, r, juce::Justification::centred);
        };
        tag (c.midiTag, c.node.getY() - laneH + 7, Pal::midiBright);
        tag (c.audioTag, c.node.getBottom() + 11, Pal::lcdText);
    }

    // where a dragged block would land
    if (dragging && dropIndex >= 0)
    {
        int xs = -1, k = 0;
        for (auto& c : cells)
        {
            if (c.blockId == 0 || c.blockId == dragId) continue;
            if (k == dropIndex) { xs = c.node.getX() - linkW / 2; break; }
            ++k;
        }
        if (xs < 0)
            for (auto& c : cells) if (c.add || c.tape) { xs = c.node.getX() - linkW / 2; break; }
        g.setColour (Pal::lcdText);
        g.fillRect (xs - 1, cells.front().node.getY() - 4, 3, nodeH + 8);
    }

    // divider and captions for the panel underneath
    g.setColour (Pal::lcdRule);
    g.fillRect (pad, notchHeight + flowTop + 2 * laneH + nodeH + 12, editorWidth - 2 * pad, 1);
    for (auto& cap : captions)
    {
        g.setColour (cap.colour);
        g.setFont (lf.mono (cap.colour == Pal::lcdText ? 17.0f : 11.5f));
        g.drawFittedText (cap.text, cap.area, juce::Justification::centredLeft, 2, 0.75f);
    }
}

//==============================================================================
int ChainEditor::cellAt (juce::Point<int> pt) const
{
    for (int i = 0; i < (int) cells.size(); ++i)
    {
        auto& c = cells[(size_t) i];
        if ((c.blockId != 0 || c.inputs || c.add || c.tape) && c.node.contains (pt))
            return i;
    }
    return -1;
}

void ChainEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragging = false;
    dragId = 0;
    auto i = cellAt (e.getPosition());
    if (i < 0) return;
    auto& c = cells[(size_t) i];

    if (c.blockId != 0 && e.mods.isPopupMenu())
    {
        auto id = c.blockId;
        const AudioEngine::BlockInfo* b = nullptr;
        for (auto& bi : chain) if (bi.id == id) b = &bi;
        if (b == nullptr) return;
        juce::PopupMenu m;
        m.addItem ("Edit", b->hasEditor, false, [this, id] { if (onEditBlock) onEditBlock (t, id); });
        m.addItem (b->bypass ? "Switch on" : "Bypass", [this, id, by = b->bypass] { engine.setBlockBypass (t, id, ! by); changed(); });
        m.addItem ("Remove", [this, id] { engine.removeBlock (t, id); changed(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (c.node)));
        return;
    }

    if (c.blockId != 0)
    {
        // the lamp: bypass
        if (juce::Rectangle<int> (c.node.getX(), c.node.getY(), 22, 22).contains (e.getPosition()))
        {
            for (auto& bi : chain)
                if (bi.id == c.blockId && bi.role != BlockRole::pending)
                {
                    engine.setBlockBypass (t, bi.id, ! bi.bypass);
                    changed();
                }
            return;
        }
        dragId = c.blockId;
        dragX = e.x;
    }
}

void ChainEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragId == 0) return;
    if (! dragging && std::abs (e.x - dragX) < 6) return;
    dragging = true;
    // the drop index among the other blocks
    dropIndex = 0;
    for (auto& c : cells)
        if (c.blockId != 0 && c.blockId != dragId && c.node.getCentreX() < e.x)
            ++dropIndex;
    repaint();
}

void ChainEditor::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        engine.moveBlock (t, dragId, dropIndex);
        dragging = false;
        dragId = 0;
        dropIndex = -1;
        changed();
        return;
    }
    dragId = 0;
    if (e.mods.isPopupMenu()) return;
    auto i = cellAt (e.getPosition());
    if (i < 0) return;
    auto& c = cells[(size_t) i];
    if (c.inputs)            { sel = Sel::inputs; selId = 0; }
    else if (c.add)          { sel = Sel::add; selId = 0; }
    else if (c.blockId != 0) { sel = Sel::block; selId = c.blockId; }
    else return;
    if (learning) engine.mapper.cancelLearn();
    learning = false;
    learnParam = -1;
    rebuild();
}

void ChainEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto i = cellAt (e.getPosition());
    if (i >= 0 && cells[(size_t) i].blockId != 0 && onEditBlock)
        onEditBlock (t, cells[(size_t) i].blockId);
}

bool ChainEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (learning) { learning = false; engine.mapper.cancelLearn(); rebuild(); return true; }
        if (onClose) onClose();
        return true;
    }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && sel == Sel::block && selId != 0)
    {
        engine.removeBlock (t, selId);
        changed();
        return true;
    }
    return false;
}
} // namespace tf
