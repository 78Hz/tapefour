#include "ChainEditor.h"
#include "BlockPicker.h"

namespace tf
{
namespace
{
    const juce::Colour frame { 0xff3d3b36 };
    constexpr int pad = 16, flowTop = 48, laneH = 18, nodeH = 60, linkW = 14;
    // the status line under the chain, and the panel under that
    constexpr int statusY = ChainEditor::notchHeight + flowTop + 2 * laneH + nodeH + 12, statusH = 16;
    constexpr int panelTop = statusY + statusH + 18;
    constexpr int crossR = 7;      // the × at a block's corner: its circle's radius, its centre 3 px in and down
    constexpr double levelLo = -24.0, levelHi = 12.0;

    juce::String levelText (const AudioEngine::BlockInfo& b, bool isMix)
    {
        if (isMix) return juce::String (juce::roundToInt (b.mix * 100.0f)) + "%";
        return (b.levelDb > 0.0f ? "+" : "") + juce::String (b.levelDb, 1) + " dB";
    }
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
    s << engine.getActiveInputNames().size() << (int) sel << selId << (int) learning << (int) outsOpen;
    s << engine.routeSignature();
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
    // what each instrument sends: its node's strip, and the LEVEL in its panel
    for (auto& bi : chain)
    {
        if (bi.role != BlockRole::instrument && bi.role != BlockRole::external) continue;
        auto* blk = engine.findBlock (t, bi.id);
        if (blk == nullptr) continue;
        const float peak = blk->sendPeak.exchange (0.0f);
        auto& shown = sendShown[bi.id];
        const int before = MeterScale::lit (shown);
        shown = std::max (peak, shown * 0.86f);
        if (MeterScale::lit (shown) != before)
            for (auto& c : cells) if (c.blockId == bi.id) repaint (c.node);
    }

    // LEVEL and MIX moved from somewhere else (a MIDI controller, say): the nodes' lines follow
    for (auto& bi : chain)
        if (auto* blk = engine.findBlock (t, bi.id); blk != nullptr && bi.id != levelDragId)
        {
            const float lv = blk->levelDb.load(), mx = blk->mix.load();
            if (std::abs (lv - bi.levelDb) > 1e-6f || std::abs (mx - bi.mix) > 1e-6f)
            {
                bi.levelDb = lv;
                bi.mix = mx;
                for (auto& c : cells) if (c.blockId == bi.id) repaint (c.node);
            }
        }

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
    if (sel == Sel::block && selected() == nullptr)
    {
        if (learning) engine.mapper.cancelLearn();
        sel = Sel::none; selId = 0; learning = false;
    }
    if (armedX != 0 && infoFor (armedX) == nullptr) armedX = 0;
    if (auto* b = selected(); learning && (b == nullptr || b->role == BlockRole::empty || b->role == BlockRole::pending))
    {
        engine.mapper.cancelLearn();
        learning = false;
    }
    if (auto* b = selected(); outsOpen && (b == nullptr || (b->role != BlockRole::instrument && b->role != BlockRole::external)))
        outsOpen = false;
    lastSignature = signature();
    layoutFlow();
    clearPanel();

    // Only the chain and its status line, unless what's selected needs a panel underneath
    // (the tape block's is a single row).
    constexpr int tapePanelH = 52;
    const int h = notchHeight + (! hasPanel() ? compactHeight : (sel == Sel::tape ? panelTop - notchHeight + tapePanelH : editorHeight));
    if (getHeight() != h)
    {
        setSize (editorWidth, h);
        if (auto* parent = getParentComponent()) parent->repaint();
    }

    auto area = juce::Rectangle<int> (pad, panelTop, editorWidth - 2 * pad, notchHeight + editorHeight - 14 - panelTop);
    if (sel == Sel::inputs)
        buildInputsPanel (area);
    else if (sel == Sel::add)
        buildPickerPanel (area, 0);
    else if (sel == Sel::tape)
        buildTapePanel (area.withHeight (tapePanelH - 16));
    else if (auto* b = selected())
    {
        if (b->role == BlockRole::empty)         buildPickerPanel (area, b->id);
        else if (learning)                       buildLearnPanel (*b, area);
        else if (outsOpen)                       buildOutsPanel (*b, area);
        else if (b->role == BlockRole::external) buildExternalPanel (*b, area);
    }
    resized();
    repaint();
}

bool ChainEditor::hasPanel() const
{
    if (sel == Sel::inputs || sel == Sel::add || sel == Sel::tape) return true;
    if (auto* b = selected(); sel == Sel::block && b != nullptr)
        return b->role == BlockRole::empty || b->role == BlockRole::external || learning || outsOpen;
    return false;
}

const AudioEngine::BlockInfo* ChainEditor::infoFor (int blockId) const
{
    for (auto& b : chain) if (b.id == blockId) return &b;
    return nullptr;
}

int ChainEditor::slotNumber (int blockId) const
{
    for (int i = 0; i < (int) chain.size(); ++i) if (chain[(size_t) i].id == blockId) return i + 1;
    return 0;
}

juce::String ChainEditor::statusText (juce::Colour& colour) const
{
    colour = Pal::lcdDim;
    const auto dot = utf (" \xc2\xb7 ");
    if (armedX != 0)
        if (auto* a = infoFor (armedX))
        {
            colour = Pal::lamp;
            return a->name.toUpperCase() + utf (": CLICK \xc3\x97 AGAIN TO UNLOAD IT (ANYWHERE ELSE: KEEP IT)");
        }
    if (levelDragId != 0)
        if (auto* d = infoFor (levelDragId))
        {
            const bool isMix = d->role == BlockRole::audioFx;
            colour = Pal::lcdText;
            return d->name.toUpperCase() + dot + (isMix ? "MIX " : "LEVEL ") + levelText (*d, isMix) + dot + "DOUBLE-CLICK: " + (isMix ? "100%" : "0 dB");
        }
    if (sel == Sel::inputs) return "SELECTED: TRACK INPUTS" + dot + "WHAT PLAYS THE TRACK, AND WHAT IT HEARS";
    if (sel == Sel::add)    return "ADD A BLOCK AT THE END OF THE CHAIN" + dot + "PICK IT BELOW";
    if (sel == Sel::tape)   return "SELECTED: TO TAPE" + dot + "WHAT LEAVES TRACK " + juce::String (t + 1) + "'S CHAIN" + dot + "ESC: DONE";
    auto* b = selected();
    if (sel != Sel::block || b == nullptr)
        return "TRACK " + juce::String (t + 1) + dot + "CLICK A BLOCK TO SELECT IT" + dot + "INPUTS: WHAT PLAYS THE TRACK";

    const auto slot = juce::String (slotNumber (b->id));
    if (b->role == BlockRole::empty)
        return "SLOT " + slot + " IS EMPTY" + dot + utf ("PICK WHAT TO LOAD INTO IT") + dot + utf ("ITS \xc3\x97 TAKES THE SLOT OUT OF THE CHAIN");
    if (b->role == BlockRole::pending)
        return b->missing ? "MISSING: " + b->name.toUpperCase() + dot + utf ("RESCAN IN SETUP \xe2\x80\xba PLUG-INS, OR \xc3\x97 TO UNLOAD IT")
                          : "LOADING " + b->name.toUpperCase() + dot + "ITS SETTINGS ARE APPLIED WHEN IT ARRIVES";
    if (outsOpen)
    {
        colour = Pal::lcdText;
        return "OUTS: " + b->name.toUpperCase() + dot + "CLICK A TRACK TO SEND AN OUTPUT THERE" + dot + "THIS CHAIN: KEEP IT HERE TOO" + dot + "ESC OR OUTS: DONE";
    }
    if (learning)
    {
        colour = Pal::midiBright;
        return "MIDI LEARN: " + b->name.toUpperCase() + dot + "PICK A PARAMETER, OR TOUCH ONE IN ITS WINDOW, THEN MOVE A CONTROL" + dot + "ESC: DONE";
    }
    juce::String s = "SELECTED: " + b->name.toUpperCase();
    if (b->hasEditor) s << dot << "DOUBLE-CLICK TO EDIT";
    s << dot << utf ("\xc3\x97 UNLOADS IT");
    for (auto& c : cells)
    {
        if (c.blockId == b->id && c.hasBadge) s << dot << "MIDI: LEARN ITS CONTROLS";
        if (c.blockId == b->id && c.hasOuts)  s << dot << "OUTS: WHERE ITS OUTPUTS GO";
    }
    if (b->canProcessInput) s << dot << (b->processInput ? "PROCESS" : "+ MIX") << ": CLICK TO SWAP";
    return s;
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
    int y = area.getY();
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
    y = chipRow (y, x, right, "AUDIO IN", ins, 40);

    // what other tracks send this one: their instruments' outputs, and their finished sound
    std::vector<LcdChip*> plugs, chains;
    for (auto& src : engine.getRouteSources())
    {
        if (src.track == t) continue;
        const auto from = "TRK " + juce::String (src.track + 1);
        if (src.blockId == 0)
        {
            const bool on = (engine.track[src.track].chainOutMask.load() & (1 << t)) != 0;
            const int k = src.track;
            chains.push_back (chip (src.name, on, [this, k, on]
            {
                if (! engine.setChainOut (k, t, ! on) && onMessage) onMessage (utf ("NOT ALLOWED: THAT WOULD MAKE A LOOP (OR TRK ") + juce::String (k + 1) + " BOUNCES)");
            }, "A COPY OF ITS SOUND"));
            continue;
        }
        const bool here = src.dest == t;
        const auto label = src.name.isEmpty() ? src.owner : src.name;
        const auto sub = from + utf (" \xc2\xb7 ") + (src.name.isEmpty() ? juce::String ("ITS OUTPUT") : src.owner)
                         + (here ? (src.home ? utf (" \xc2\xb7 COPY") : utf (" \xc2\xb7 MOVED")) : juce::String());
        auto* c = new LcdChip (label, sub);
        c->chosen = here;
        c->setTooltip ("Click: moved here. Again: a copy (it stays on its own track too). Again: not sent.");
        c->onClick = [this, src] { routeClicked (src); changed(); };
        panel.add (c);
        addAndMakeVisible (c);
        plugs.push_back (c);
    }
    if (! plugs.empty()) y = chipRow (y, x, right, "FROM PLUG-INS", plugs, 34);
    if (! chains.empty()) chipRow (y, x, right, "FROM TRACKS", chains, 34);
}

void ChainEditor::routeClicked (const AudioEngine::RouteSource& src)
{
    // not sent here: moved here; moved: a copy (it stays on its own track too); a copy: not sent
    bool ok = true;
    if (src.dest != t)       ok = engine.setBlockRoute (src.track, src.blockId, src.output, t, false);
    else if (! src.home)     ok = engine.setBlockRoute (src.track, src.blockId, src.output, t, true);
    else                     ok = engine.setBlockRoute (src.track, src.blockId, src.output, -1, ! src.canBeOff);
    if (! ok && onMessage)
        onMessage ("NOT ALLOWED: THAT WOULD MAKE A LOOP (OR TRK " + juce::String (src.track + 1) + " BOUNCES)");
}

juce::String ChainEditor::whyNot (int from, int to) const
{
    if ((AudioEngine::Feed) engine.track[from].feed.load() == AudioEngine::Feed::bounce)
        return "TRK " + juce::String (from + 1) + " BOUNCES FROM TAPE: ITS SOUND CAN'T BE SENT ON";
    return "TRK " + juce::String (to + 1) + " ALREADY FEEDS TRK " + juce::String (from + 1) + ": SENDING IT BACK WOULD MAKE A LOOP";
}

void ChainEditor::buildOutsPanel (const AudioEngine::BlockInfo& b, juce::Rectangle<int> area)
{
    // each output: whether it stays in this chain, and which track it is sent to
    std::vector<AudioEngine::RouteSource> outs;
    for (auto& src : engine.getRouteSources())
        if (src.track == t && src.blockId == b.id) outs.push_back (src);
    const bool twoCols = outs.size() > 8;
    const int perCol = twoCols ? (int) (outs.size() + 1) / 2 : (int) outs.size();
    const int colW = twoCols ? area.getWidth() / 2 : area.getWidth();
    constexpr int rowH = 29, chipH = 24;
    for (int c = 0; c < (twoCols ? 2 : 1); ++c)
    {
        const int x0 = area.getX() + c * colW;
        captions.push_back ({ "OUTPUT", { x0, area.getY(), 90, 14 }, Pal::lcdDim, 10.0f });
        captions.push_back ({ "HERE", { x0 + 150, area.getY(), 60, 14 }, Pal::lcdDim, 10.0f });
        captions.push_back ({ "SENT TO", { x0 + 252, area.getY(), 80, 14 }, Pal::lcdDim, 10.0f });
    }
    for (int i = 0; i < (int) outs.size(); ++i)
    {
        const auto src = outs[(size_t) i];
        const int col = twoCols ? i / perCol : 0, row = twoCols ? i % perCol : i;
        const int x0 = area.getX() + col * colW, y = area.getY() + 20 + row * rowH;
        const bool multi = outs.size() > 1;
        captions.push_back ({ src.name.isEmpty() ? juce::String ("MAIN") : src.name, { x0, y, 92, chipH }, Pal::lcdText, 12.5f });
        captions.push_back ({ src.channels == 1 ? "MONO" : "STEREO", { x0 + 94, y, 54, chipH }, Pal::lcdDim, 9.5f });

        auto* here = chip ("THIS CHAIN", src.home, [this, src]
        {
            if (src.dest < 0 && ! src.canBeOff)
            {
                if (onMessage) onMessage ("SEND IT TO A TRACK FIRST: THEN IT CAN LEAVE THIS CHAIN");
                return;
            }
            engine.setBlockRoute (t, src.blockId, src.output, src.dest, ! src.home);
        });
        here->textSize = 11.0f;
        here->setBounds (x0 + 150, y, 92, chipH);
        int x = x0 + 252;
        for (int k = 0; k < numTracks; ++k)
        {
            if (k == t) continue;
            const bool on = src.dest == k, allowed = on || engine.routeAllowed (t, k);
            const auto why = allowed ? juce::String() : whyNot (t, k);
            auto* c = chip ("TRK " + juce::String (k + 1), on, [this, src, k, on, allowed, why]
            {
                if (! allowed) { if (onMessage) onMessage ("NOT ALLOWED: " + why); return; }
                if (on) engine.setBlockRoute (t, src.blockId, src.output, -1, src.home);
                else    engine.setBlockRoute (t, src.blockId, src.output, k, src.dest >= 0 ? src.home : false);   // first sent: moved
            });
            c->textSize = 11.0f;
            c->hatched = ! allowed;
            if (! allowed) c->setTooltip (why.toLowerCase());
            c->setBounds (x, y, 52, chipH);
            x += 56;
        }
        if (! twoCols)
        {
            juce::String note;
            if (src.dest >= 0) note = (src.home ? "COPY: HERE AND ON TRK " : "MOVED TO TRK ") + juce::String (src.dest + 1);
            else if (src.output == 0 && multi) note = "EVERYTHING NOT MOVED AWAY";
            else if (! src.home) note = "OFF: NOT HEARD";
            captions.push_back ({ note, { x + 6, y, area.getRight() - x - 6, chipH }, src.dest >= 0 ? Pal::lcdText : Pal::lcdDim, 10.5f });
        }
    }
}

void ChainEditor::buildTapePanel (juce::Rectangle<int> area)
{
    // what leaves this track's chain: copied to other tracks' inputs, too
    const int x = area.getX(), y = area.getY();
    captions.push_back ({ "CHAIN OUT", { x, y, 110, 16 }, Pal::lcdDim });
    captions.push_back ({ "A COPY TO", { x, y + 16, 110, 14 }, Pal::lcdDim, 9.5f });
    const int mask = engine.track[t].chainOutMask.load();
    int cx = x + 110;
    juce::StringArray to;
    for (int k = 0; k < numTracks; ++k)
    {
        if (k == t) continue;
        const bool on = (mask & (1 << k)) != 0, allowed = on || engine.routeAllowed (t, k);
        if (on) to.add (juce::String (k + 1));
        const auto why = allowed ? juce::String() : whyNot (t, k);
        auto* c = chip ("TRK " + juce::String (k + 1), on, [this, k, on, allowed, why]
        {
            if (! allowed) { if (onMessage) onMessage ("NOT ALLOWED: " + why); return; }
            engine.setChainOut (t, k, ! on);
        });
        c->hatched = ! allowed;
        if (! allowed) c->setTooltip (why.toLowerCase());
        c->setBounds (cx, y, 66, 30);
        cx += 72;
    }
    captions.push_back ({ to.isEmpty() ? "COPY THIS TRACK'S FINISHED SOUND INTO ANOTHER TRACK'S INPUTS"
                                       : "THE FINISHED SOUND ALSO GOES INTO TRK " + to.joinIntoString (" + TRK ") + (to.size() > 1 ? "'S INPUTS" : "'S INPUTS"),
                          { cx + 8, y, area.getRight() - cx - 8, 30 }, to.isEmpty() ? Pal::lcdDim : Pal::lcdText, 11.0f });
}

void ChainEditor::buildLearnPanel (const AudioEngine::BlockInfo& b, juce::Rectangle<int> area)
{
    // left: what's learned for this block, each with its × to forget it; right: its parameters
    auto left = area.removeFromLeft (310);
    area.removeFromLeft (18);
    const int x = left.getX(), id = b.id;
    int y = left.getY();
    captions.push_back ({ "LEARNED", { x, y, 310, 16 }, Pal::midiBright });
    y += 20;

    std::vector<MidiMapper::Mapping> mine;
    for (auto& m : engine.mapper.getMappings())
        if (m.target.plugin && m.target.track == t && m.target.blockId == id) mine.push_back (m);
    if (mine.empty())
        captions.push_back ({ "NOTHING YET. PICK A PARAMETER ON THE RIGHT (OR TOUCH ONE IN THE PLUG-IN'S WINDOW), THEN MOVE A KNOB, FADER OR KEY ON YOUR CONTROLLER.",
                              { x, y, 300, 64 }, Pal::lcdDim });
    for (auto& m : mine)
    {
        if (y > left.getBottom() - 26) break;
        captions.push_back ({ m.paramName.toUpperCase().substring (0, 18) + utf (" \xe2\x86\x90 ") + m.portName.toUpperCase().substring (0, 10)
                                  + " " + engine.mapper.describeMessage (m), { x, y, 280, 24 }, Pal::midiBright });
        auto mapId = m.id;
        auto* xb = chip (utf ("\xc3\x97"), false, [this, mapId] { engine.mapper.remove (mapId); });
        xb->setTooltip ("Forget this mapping");
        xb->setBounds (x + 284, y, 26, 24);
        y += 28;
    }

    auto* blk = engine.findBlock (t, id);
    if (blk == nullptr || blk->getProcessor() == nullptr) return;
    params = std::make_unique<ParamPicker> (*blk->getProcessor());
    params->onPick = [this] (int i) { pickParameter (i); };
    addAndMakeVisible (*params);
    params->setBounds (area);
    if (learnParam >= 0) params->highlight (learnParam);
}

void ChainEditor::buildExternalPanel (const AudioEngine::BlockInfo& b, juce::Rectangle<int> area)
{
    const int id = b.id;
    // the lamp bypasses it, its × unloads it, its line is its LEVEL: here, only where it goes
    auto top = area.removeFromTop (26);
    auto* off = chip ("ALL NOTES OFF", false, [this, id] { engine.sendAllNotesOff (t, id); });
    off->setBounds (top.removeFromRight (off->idealWidth()));
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

void ChainEditor::buildPickerPanel (juce::Rectangle<int> area, int slotId)
{
    // "+": onto the end of the chain; an empty slot: into that slot
    picker = std::make_unique<BlockPicker> (engine, juce::String(),
                                            slotId != 0 ? "LOAD INTO SLOT " + juce::String (slotNumber (slotId)) + utf (" \xe2\x80\xba") : utf ("ADD \xe2\x80\xba"));
    picker->onFind = [this] { if (onFindPlugins) onFindPlugins(); };
    picker->onPick = [this, slotId] (const BlockPicker::Item& it)
    {
        std::unique_ptr<Block> block;
        using K = BlockPicker::Kind;
        if (it.kind == K::synth)      block = BuiltIn::createSynth (it.index);
        else if (it.kind == K::fx)    block = BuiltIn::createFx (it.index);
        else if (it.kind == K::external) block = std::make_unique<ExternalBlock> (it.index);

        const int into = engine.findBlock (t, slotId) != nullptr ? slotId : 0;
        int newId = 0;
        if (block != nullptr)
            newId = into != 0 ? engine.replaceBlock (t, into, std::move (block)) : engine.insertBlock (t, -1, std::move (block));
        else
            newId = engine.addPlugin (t, -1, it.desc, {}, into, [safe = juce::Component::SafePointer<ChainEditor> (this)] (const juce::String& err)
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
    const bool audioIn = bounce || p.inputChannel.load() >= 0 || engine.describeRoutedIn (t).isNotEmpty();
    const bool canAdd = (int) chain.size() < AudioEngine::maxBlocks;

    const int nodes = 1 + (int) chain.size() + (canAdd ? 1 : 0) + 1;
    const int fixed = 116 + 64 + (canAdd ? 40 : 0) + (nodes - 1) * linkW;
    const int blockW = chain.empty() ? 100 : juce::jlimit (60, 170, (editorWidth - 2 * pad - fixed) / (int) chain.size());

    // Where the MIDI stream ends: the last block that listens to it.
    int lastMidi = midiIn ? 0 : -1;
    bool anyMidiFx = false;
    for (int i = 0; i < (int) chain.size(); ++i)
    {
        auto& b = chain[(size_t) i];
        if (b.bypass || b.role == BlockRole::pending || b.role == BlockRole::empty) continue;
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
        c.empty = b.role == BlockRole::empty;
        c.cross = { c.node.getRight() - 3, c.node.getY() + 3 };
        c.wide = blockW >= 110;
        {
            // the bottom row: the MIDI badge (a block with parameters to learn), then LEVEL or MIX
            auto* blk = engine.findBlock (t, b.id);
            auto* proc = blk != nullptr ? blk->getProcessor() : nullptr;
            c.hasBadge = proc != nullptr && ! proc->getParameters().isEmpty();
            for (auto& m : engine.mapper.getMappings())
                if (m.target.plugin && m.target.track == t && m.target.blockId == b.id) ++c.midiCount;
            c.isMix = b.role == BlockRole::audioFx;
            c.hasSlider = b.role == BlockRole::audioFx || b.role == BlockRole::instrument || b.role == BlockRole::external;
            // OUTS: every instrument; how many of its outputs are sent to other tracks
            c.hasOuts = blk != nullptr && (b.role == BlockRole::instrument || b.role == BlockRole::external);
            if (c.hasOuts)
                for (int o = 0; o < blk->numOutputs(); ++o)
                    if (blk->getRoute (o).dest >= 0) ++c.outsCount;
            const int rowY = c.node.getBottom() - 17;
            int left = c.node.getX() + 7;
            if (c.hasBadge)
            {
                const int bw = c.wide ? (c.midiCount > 0 ? 40 : 30) : (c.midiCount > 0 ? 24 : 16);
                c.badge = { left, rowY, bw, 11 };
                left = c.badge.getRight() + 6;
            }
            if (c.hasOuts)
            {
                const int bw = c.wide ? (c.outsCount > 0 ? 40 : 30) : (c.outsCount > 0 ? 24 : 16);
                c.outsBadge = { left, rowY, bw, 11 };
                left = c.outsBadge.getRight() + 7;
            }
            if (c.hasSlider)
            {
                const int right = c.node.getRight() - 7;
                c.showValue = right - 45 - left >= 30;   // the value beside the line, when there's room
                if (c.showValue) c.value = { right - 40, rowY, 40, 11 };
                c.slider = { left, rowY - 2, (c.showValue ? c.value.getX() - 5 : right) - left, 15 };
                if (c.slider.getWidth() < 12) c.hasSlider = false;
            }
        }
        const bool live = ! b.bypass && b.role != BlockRole::pending && ! c.empty;
        {
            juce::StringArray parts;
            for (auto& src : engine.getRouteSources())
                if (src.track == t && src.blockId == b.id && src.dest >= 0)
                    parts.add ((src.name.isEmpty() ? juce::String() : src.name.trimCharactersAtStart ("0123456789 ") + " ")
                               + (src.home ? "+ " : utf ("\xe2\x80\xba ")) + juce::String (src.dest + 1));
            c.routeTag = parts.joinIntoString ("  ");
        }
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
            if (b.canProcessInput)   // the tag swaps between mixing in and processing what reaches it
                c.audioTagArea = juce::Rectangle<int> (64, 16).withCentre ({ c.node.getCentreX(), c.node.getBottom() + 11 });
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
    auto body = juce::Rectangle<float> (0, (float) notchHeight, (float) editorWidth, (float) (getHeight() - notchHeight));

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
        const bool ghost = c.add || c.empty || (b != nullptr && (b->bypass || b->role == BlockRole::pending));
        const bool beingDragged = dragging && b != nullptr && b->id == dragId;

        juce::Colour fill = juce::Colours::transparentBlack, ink = Pal::lcdText, sub = Pal::lcdDim;
        const bool tapeSel = c.tape && sel == Sel::tape;
        if (tapeSel)     { fill = Pal::lcdText; ink = sub = Pal::lcd; }
        else if (c.tape) { fill = Pal::accent; ink = sub = juce::Colour (0xff1a1917); }
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
            if (c.inputs || c.add || c.empty || (b != nullptr && b->role == BlockRole::pending))
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
            const int copies = engine.track[t].chainOutMask.load();
            g.setFont (lf.mono (14.0f));
            g.drawText ("TRK " + juce::String (t + 1), copies != 0 ? in.removeFromTop (24) : in, juce::Justification::centredLeft);
            if (copies != 0)
            {
                juce::StringArray to;
                for (int k = 0; k < numTracks; ++k) if (copies & (1 << k)) to.add (juce::String (k + 1));
                g.setFont (lf.mono (9.5f));
                g.drawFittedText ("+ TRK " + to.joinIntoString ("+"), in, juce::Justification::topLeft, 1, 0.7f);
            }
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
                const auto routed = engine.describeRoutedIn (t);
                const bool audio = p.inputChannel.load() >= 0;
                g.setColour (! audio && routed.isEmpty() ? sub : ink);
                g.drawFittedText (audio ? engine.describeInput (t) + (routed.isNotEmpty() ? " + " + routed.fromFirstOccurrenceOf ("FROM ", false, false) : juce::String())
                                        : (routed.isNotEmpty() ? routed : juce::String ("NO AUDIO IN")),
                                  in, juce::Justification::centredLeft, 1, 0.75f);
            }
            continue;
        }

        if (b == nullptr) continue;
        auto drawCross = [&]
        {
            // the × at the corner: unloads the block (an empty slot's takes the slot out)
            const bool armed = armedX == b->id;
            auto circle = juce::Rectangle<float> (2.0f * crossR, 2.0f * crossR).withCentre (c.cross.toFloat());
            g.setColour (armed ? Pal::red : Pal::lcd);
            g.fillEllipse (circle);
            g.setColour (armed ? Pal::red : Pal::lcdDim);
            g.drawEllipse (circle.reduced (0.5f), 1.0f);
            g.setColour (armed ? Pal::lcd : Pal::lcdText);
            const auto o = c.cross.toFloat();
            g.drawLine (o.x - 2.5f, o.y - 2.5f, o.x + 2.5f, o.y + 2.5f, 1.2f);
            g.drawLine (o.x - 2.5f, o.y + 2.5f, o.x + 2.5f, o.y - 2.5f, 1.2f);
        };
        if (c.empty)
        {
            g.setColour ((isSel ? ink : Pal::lcdText).withMultipliedAlpha (isSel ? 1.0f : 0.7f));
            g.setFont (lf.mono (18.0f));
            g.drawText ("+", r.withHeight (r.getHeight() - 22).translated (0, 4), juce::Justification::centred);
            g.setColour (sub);
            g.setFont (lf.mono (9.0f));
            g.drawFittedText ("EMPTY: CLICK TO LOAD", r.reduced (5, 0).withTop (r.getBottom() - 20).withHeight (14), juce::Justification::centred, 1, 0.6f);
            drawCross();
            continue;
        }
        // lamp: on, or bypassed
        auto lamp = juce::Rectangle<float> ((float) r.getX() + 7, (float) r.getY() + 7, 8, 8);
        if (b->role != BlockRole::pending)
        {
            // in the screen's own colour: lit when on, hollow when bypassed
            g.setColour (ink);
            if (b->bypass) g.drawEllipse (lamp.reduced (0.5f), 1.0f);
            else           g.fillEllipse (lamp);
        }
        g.setColour (sub.withMultipliedAlpha (alpha));
        g.setFont (lf.mono (9.0f));
        auto kind = b->role == BlockRole::pending ? juce::String (b->missing ? "MISSING" : "LOADING")
                    : juce::String (roleName (b->role)) + (b->bypass ? utf (" · BYP") : juce::String());
        g.drawFittedText (kind, in.removeFromTop (12).withTrimmedLeft (b->role == BlockRole::pending ? 0 : 12), juce::Justification::centredLeft, 1, 0.7f);
        g.setColour (ink.withMultipliedAlpha (alpha));
        g.setFont (lf.mono (12.0f));
        const bool bottomRow = c.hasBadge || c.hasSlider;
        g.drawFittedText (b->name.toUpperCase(), bottomRow ? in.withBottom (r.getBottom() - 18) : in.withTrimmedBottom (4),
                          juce::Justification::centredLeft, 2, 0.75f);

        if (c.hasBadge)
        {
            // MIDI: how many of its controls are learned; clicked, MIDI learn opens underneath
            const bool open = learning && isSel;
            const auto col = open ? Pal::midiBright : (c.midiCount > 0 ? (isSel ? ink : Pal::midiBright) : sub.withMultipliedAlpha (0.8f));
            auto bf = c.badge.toFloat().reduced (0.5f);
            if (open) { g.setColour (col); g.fillRoundedRectangle (bf, 2.0f); }
            else      { g.setColour (col.withMultipliedAlpha (alpha)); g.drawRoundedRectangle (bf, 2.0f, 1.0f); }
            g.setColour (open ? juce::Colour (0xff10151c) : col.withMultipliedAlpha (alpha));
            g.setFont (lf.mono (8.5f));
            const juce::String n = c.midiCount > 0 ? juce::String (c.midiCount) : juce::String();
            g.drawText (c.wide ? ("MIDI " + n).trim() : ("M" + n), c.badge, juce::Justification::centred, false);
        }
        if (c.hasOuts)
        {
            // OUTS: how many of its outputs are sent to other tracks; clicked, where they go opens underneath
            const bool open = outsOpen && isSel;
            const auto col = c.outsCount > 0 ? ink : sub.withMultipliedAlpha (0.8f);
            auto bf = c.outsBadge.toFloat().reduced (0.5f);
            if (open) { g.setColour (ink); g.fillRoundedRectangle (bf, 2.0f); }
            else      { g.setColour (col.withMultipliedAlpha (alpha)); g.drawRoundedRectangle (bf, 2.0f, 1.0f); }
            g.setColour (open ? (isSel ? Pal::lcdText : Pal::lcd) : col.withMultipliedAlpha (alpha));
            g.setFont (lf.mono (8.5f));
            const juce::String n = c.outsCount > 0 ? juce::String (c.outsCount) : juce::String();
            g.drawText (c.wide ? ("OUTS " + n).trim() : ("O" + n), c.outsBadge, juce::Justification::centred, false);
        }
        if (c.hasSlider)
        {
            // LEVEL (dB) or MIX (%): a hairline, the value as a thinner line along it, a tick where it ends
            const double frac = c.isMix ? (double) b->mix : (b->levelDb - levelLo) / (levelHi - levelLo);
            const float x0 = (float) c.slider.getX(), x1 = (float) c.slider.getRight(), ly = (float) c.slider.getCentreY();
            const float xm = x0 + (x1 - x0) * (float) juce::jlimit (0.0, 1.0, frac);
            g.setColour ((isSel ? Pal::lcd.withAlpha (0.45f) : Pal::lcdRule).withMultipliedAlpha (alpha));
            g.fillRect (juce::Rectangle<float> (x0, ly - 0.5f, x1 - x0, 1.0f));
            g.setColour (ink.withMultipliedAlpha (alpha));
            g.fillRect (juce::Rectangle<float> (x0, ly - 1.0f, xm - x0, 2.0f));
            g.fillRect (juce::Rectangle<float> (xm - 0.5f, ly - 4.0f, 1.0f, 8.0f));
            if (c.showValue)
            {
                g.setFont (lf.mono (9.0f));
                g.drawText (levelText (*b, c.isMix), c.value, juce::Justification::centredRight, false);
            }
        }

        if (b->role == BlockRole::instrument || b->role == BlockRole::external)
        {
            // what it sends into the track, on the meters' scale
            auto it = sendShown.find (b->id);
            const int lit = MeterScale::lit (it != sendShown.end() ? it->second : 0.0f);
            auto strip = rf.reduced (7.0f, 0.0f).withTop (rf.getBottom() - 4.0f).withHeight (2.0f);
            const float segW = (strip.getWidth() - 19.0f) / 20.0f;
            for (int seg = 0; seg < 20; ++seg)
            {
                const auto v = MeterScale::segments()[seg];
                auto col = v >= 3 ? Pal::red : (v >= 0 ? Pal::lamp : (isSel ? Pal::lcd : Pal::green));
                g.setColour (seg < lit ? col : (isSel ? Pal::lcd.withAlpha (0.25f) : Pal::lcdRule));
                g.fillRect (juce::Rectangle<float> (strip.getX() + (float) seg * (segW + 1.0f), strip.getY(), segW, strip.getHeight()));
            }
        }
        drawCross();
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
        if (c.routeTag.isNotEmpty())
        {
            // where its outputs go, beside its audio tag: › moved, + a copy
            g.setFont (lf.mono (9.0f));
            const auto tagW = [&] (const juce::String& s) { return (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), s) + 10; };
            const int left = c.node.getCentreX() + (c.audioTag.isNotEmpty() ? tagW (c.audioTag) / 2 + 6 : 0);
            auto r = juce::Rectangle<int> (left, c.node.getBottom() + 4, tagW (c.routeTag), 14);
            g.setColour (Pal::lcd);
            g.fillRoundedRectangle (r.toFloat(), 2.0f);
            g.setColour (Pal::lcdText);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 2.0f, 1.0f);
            g.drawText (c.routeTag, r, juce::Justification::centred);
        }
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

    // the status line: what's selected, and what can be done with it
    juce::Colour statusColour;
    auto status = statusText (statusColour);
    g.setColour (statusColour);
    g.setFont (lf.mono (11.0f));
    g.drawFittedText (status, { pad, statusY, editorWidth - 2 * pad, statusH }, juce::Justification::centredLeft, 1, 0.7f);

    // divider and captions for the panel underneath
    if (hasPanel())
    {
        g.setColour (Pal::lcdRule);
        g.fillRect (pad, panelTop - 9, editorWidth - 2 * pad, 1);
    }
    for (auto& cap : captions)
    {
        g.setColour (cap.colour);
        g.setFont (lf.mono (cap.size > 0.0f ? cap.size : (cap.colour == Pal::lcdText ? 17.0f : 11.5f)));
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

void ChainEditor::crossClicked (int blockId)
{
    auto* b = infoFor (blockId);
    if (b == nullptr) return;
    if (b->role == BlockRole::empty)
    {
        // nothing in it to lose: the slot comes out of the chain at once
        if (selId == blockId) { sel = Sel::none; selId = 0; }
        armedX = 0;
        engine.removeBlock (t, blockId);
        changed();
        return;
    }
    if (armedX != blockId)
    {
        // the first click arms it; the second unloads it
        if (learning) engine.mapper.cancelLearn();
        learning = false;
        learnParam = -1;
        armedX = blockId;
        sel = Sel::block;
        selId = blockId;
        rebuild();
        return;
    }
    const auto name = b->name.toUpperCase();
    armedX = 0;
    const int slot = slotNumber (blockId);
    if (learning) engine.mapper.cancelLearn();
    learning = false;
    if (engine.unloadBlock (t, blockId) != 0)
    {
        sel = Sel::none;
        selId = 0;
        if (onMessage) onMessage ("UNLOADED " + name + ": SLOT " + juce::String (slot) + " IS EMPTY");
    }
    changed();
}

void ChainEditor::setNodeValue (const Cell& c, int mouseX)
{
    auto* blk = engine.findBlock (t, c.blockId);
    if (blk == nullptr || c.slider.getWidth() <= 0) return;
    const double frac = juce::jlimit (0.0, 1.0, (double) (mouseX - c.slider.getX()) / (double) c.slider.getWidth());
    if (c.isMix)
        blk->mix = (float) (juce::roundToInt (frac * 100.0) / 100.0);
    else
        blk->levelDb = (float) (juce::roundToInt ((levelLo + frac * (levelHi - levelLo)) * 2.0) / 2.0);   // half-dB steps
    for (auto& bi : chain)
        if (bi.id == c.blockId) { bi.mix = blk->mix.load(); bi.levelDb = blk->levelDb.load(); }
    repaint();
}

void ChainEditor::resetNodeValue (const Cell& c)
{
    auto* blk = engine.findBlock (t, c.blockId);
    if (blk == nullptr) return;
    if (c.isMix) blk->mix = 1.0f; else blk->levelDb = 0.0f;
    for (auto& bi : chain)
        if (bi.id == c.blockId) { bi.mix = blk->mix.load(); bi.levelDb = blk->levelDb.load(); }
    engine.saveSettings();
    repaint();
}

void ChainEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragging = false;
    dragId = 0;
    downHandled = false;
    const auto pos = e.getPosition();

    // a block's ×, which sits over its corner (a little outside the block)
    if (! e.mods.isPopupMenu())
        for (auto& c : cells)
            if (c.blockId != 0 && c.cross.getDistanceFrom (pos) <= crossR + 1)
            {
                downHandled = true;
                crossClicked (c.blockId);
                return;
            }
    if (armedX != 0) { armedX = 0; repaint(); }   // anywhere else: keep it

    auto i = cellAt (pos);
    if (i < 0)
    {
        // an audio tag below a block: mix in, or process what reaches it
        for (auto& c : cells)
            if (! c.audioTagArea.isEmpty() && c.audioTagArea.contains (pos))
                if (auto* blk = engine.findBlock (t, c.blockId))
                {
                    blk->processInput = ! blk->processInput.load();
                    downHandled = true;
                    changed();
                    return;
                }
        return;
    }
    auto& c = cells[(size_t) i];

    if (c.blockId != 0 && e.mods.isPopupMenu())
    {
        auto id = c.blockId;
        auto* b = infoFor (id);
        if (b == nullptr) return;
        juce::PopupMenu m;
        if (b->role == BlockRole::empty)
        {
            m.addItem ("Load into this slot...", [this, id] { sel = Sel::block; selId = id; rebuild(); });
            m.addItem ("Take the slot out", [this, id] { crossClicked (id); });
        }
        else
        {
            m.addItem ("Edit", b->hasEditor, false, [this, id] { editBlock (id); });
            if (b->role != BlockRole::pending)
                m.addItem (b->bypass ? "Switch on" : "Bypass", [this, id, by = b->bypass] { engine.setBlockBypass (t, id, ! by); changed(); });
            if (c.hasBadge)
                m.addItem ("MIDI learn...", [this, id] { sel = Sel::block; selId = id; learning = true; learnParam = -1; rebuild(); });
            m.addSeparator();
            m.addItem ("Unload (leaves an empty slot)", [this, id] { if (engine.unloadBlock (t, id) != 0 && selId == id) { sel = Sel::none; selId = 0; } changed(); });
            m.addItem ("Remove from the chain", [this, id] { engine.removeBlock (t, id); changed(); });
        }
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (c.node)));
        return;
    }

    if (c.blockId != 0 && ! c.empty)
    {
        auto* b = infoFor (c.blockId);
        // the lamp: bypass
        if (juce::Rectangle<int> (c.node.getX(), c.node.getY(), 22, 22).contains (pos))
        {
            if (b != nullptr && b->role != BlockRole::pending)
            {
                engine.setBlockBypass (t, b->id, ! b->bypass);
                changed();
            }
            return;
        }
        // the OUTS badge: where its outputs go opens (or closes) underneath
        if (c.hasOuts && c.outsBadge.expanded (2).contains (pos))
        {
            downHandled = true;
            const bool closing = outsOpen && sel == Sel::block && selId == c.blockId;
            if (learning) engine.mapper.cancelLearn();
            learning = false;
            outsOpen = ! closing;
            sel = Sel::block;
            selId = c.blockId;
            rebuild();
            return;
        }
        // the MIDI badge: MIDI learn opens (or closes) underneath
        if (c.hasBadge && c.badge.expanded (2).contains (pos))
        {
            downHandled = true;
            const bool closing = learning && sel == Sel::block && selId == c.blockId;
            if (learning) engine.mapper.cancelLearn();
            outsOpen = false;
            learning = ! closing;
            learnParam = -1;
            sel = Sel::block;
            selId = c.blockId;
            rebuild();
            return;
        }
        // LEVEL / MIX: drag along the line
        if (c.hasSlider && c.slider.expanded (2, 2).contains (pos))
        {
            downHandled = true;
            levelDragId = c.blockId;
            setNodeValue (c, e.x);
            return;
        }
        dragId = c.blockId;
        dragX = e.x;
    }
}

void ChainEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (levelDragId != 0)
    {
        for (auto& c : cells) if (c.blockId == levelDragId) setNodeValue (c, e.x);
        return;
    }
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
    if (levelDragId != 0)
    {
        levelDragId = 0;
        engine.saveSettings();
        repaint();
        return;
    }
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
    if (downHandled) { downHandled = false; return; }
    if (e.mods.isPopupMenu()) return;
    auto i = cellAt (e.getPosition());
    if (i < 0) return;
    auto& c = cells[(size_t) i];
    if (c.inputs)            { sel = Sel::inputs; selId = 0; }
    else if (c.add)          { sel = Sel::add; selId = 0; }
    else if (c.tape)         { sel = Sel::tape; selId = 0; }
    else if (c.blockId != 0) { sel = Sel::block; selId = c.blockId; }
    else return;
    if (learning) engine.mapper.cancelLearn();
    learning = false;
    outsOpen = false;
    learnParam = -1;
    rebuild();
}

void ChainEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto i = cellAt (e.getPosition());
    if (i < 0) return;
    auto& c = cells[(size_t) i];
    if (c.blockId == 0 || c.empty) return;
    if (c.cross.getDistanceFrom (e.getPosition()) <= crossR + 1) return;   // the second click of the × did its job
    if (c.hasSlider && c.slider.expanded (2, 2).contains (e.getPosition()))
    {
        resetNodeValue (c);   // 0 dB, or 100%
        return;
    }
    if (c.hasBadge && c.badge.expanded (2).contains (e.getPosition())) return;
    if (c.hasOuts && c.outsBadge.expanded (2).contains (e.getPosition())) return;
    editBlock (c.blockId);
}

void ChainEditor::editBlock (int blockId)
{
    // every block with an editor opens it in a window of its own (the built-in instruments too)
    if (onEditBlock) onEditBlock (t, blockId);
}

bool ChainEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (armedX != 0) { armedX = 0; repaint(); return true; }
        if (learning) { learning = false; engine.mapper.cancelLearn(); rebuild(); return true; }
        if (outsOpen) { outsOpen = false; rebuild(); return true; }
        if (sel == Sel::tape) { sel = Sel::none; rebuild(); return true; }
        if (onClose) onClose();
        return true;
    }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && sel == Sel::block && selId != 0)
    {
        crossClicked (selId);   // as its ×: twice to unload, once to take out an empty slot
        return true;
    }
    return false;
}
} // namespace tf
