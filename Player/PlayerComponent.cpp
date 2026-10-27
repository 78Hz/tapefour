#include "PlayerComponent.h"
#include "SystemVolume.h"

namespace tf::pocket
{
namespace
{
    const juce::Colour screen { 0xff0a0a0a }, barColour { 0xff151413 }, keyColour { 0xff2c2a27 }, keyLine { 0xff3c3a36 },
                       keyLit { 0xff4a3a2c }, overlay { 0xee1c1b19 };

    juce::Path iconPath (PocketKey::Icon icon)
    {
        // In a 0..1 box.
        juce::Path p;
        switch (icon)
        {
            case PocketKey::Icon::play:  p.addTriangle (0.1f, 0.0f, 0.95f, 0.5f, 0.1f, 1.0f); break;
            case PocketKey::Icon::stop:  p.addRectangle (0.1f, 0.1f, 0.8f, 0.8f); break;
            case PocketKey::Icon::ff:    p.addTriangle (0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f); p.addTriangle (0.5f, 0.0f, 1.0f, 0.5f, 0.5f, 1.0f); break;
            case PocketKey::Icon::rew:   p.addTriangle (0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f); p.addTriangle (1.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f); break;
            case PocketKey::Icon::eject: p.addTriangle (0.5f, 0.0f, 1.0f, 0.6f, 0.0f, 0.6f); p.addRectangle (0.0f, 0.78f, 1.0f, 0.22f); break;
            case PocketKey::Icon::plus:  p.addRectangle (0.42f, 0.0f, 0.16f, 1.0f); p.addRectangle (0.0f, 0.42f, 1.0f, 0.16f); break;
            case PocketKey::Icon::minus: p.addRectangle (0.0f, 0.42f, 1.0f, 0.16f); break;
            case PocketKey::Icon::open:
                p.startNewSubPath (0.0f, 0.15f); p.lineTo (0.4f, 0.15f); p.lineTo (0.5f, 0.28f); p.lineTo (1.0f, 0.28f);
                p.lineTo (1.0f, 0.95f); p.lineTo (0.0f, 0.95f); p.closeSubPath();
                break;
            case PocketKey::Icon::turn:
            case PocketKey::Icon::none:
                break;
        }
        return p;
    }
}

//==============================================================================
PocketKey::PocketKey (const juce::String& word, Icon i, bool accentWhenOn) : juce::Button (word), icon (i), accent (accentWhenOn)
{
    setWantsKeyboardFocus (false);
    if (word.isNotEmpty()) setTitle (word);
}

void PocketKey::setOn (bool on)
{
    if (on == lit) return;
    lit = on;
    repaint();
}

void PocketKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    auto bg = lit ? (accent ? Pal::orange : keyLit) : keyColour;
    if (down) bg = bg.darker (0.25f);
    else if (over) bg = bg.brighter (0.08f);
    g.setColour (bg.withMultipliedAlpha (isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (r, 11.0f);
    if (! solid)
    {
        g.setColour (keyLine);
        g.drawRoundedRectangle (r, 11.0f, 1.0f);
    }
    const auto fg = (lit && accent ? juce::Colour (0xff1f1d1a) : Pal::cream).withMultipliedAlpha (isEnabled() ? 1.0f : 0.45f);
    g.setColour (fg);

    auto word = getButtonText();
    auto content = r.reduced (10.0f, 6.0f);
    if (stacked && word.isNotEmpty())
    {
        // icon above, word below
        auto top = content.removeFromTop (content.getHeight() * 0.55f);
        if (icon == Icon::turn)
        {
            auto c = top.getCentre();
            juce::Path a;
            a.addCentredArc (c.x, c.y, 9.0f, 9.0f, 0.0f, -2.6f, 0.3f, true);
            a.addCentredArc (c.x, c.y, 9.0f, 9.0f, 0.0f, 0.54f, 3.44f, true);
            g.setColour (Pal::orange);
            g.strokePath (a, juce::PathStrokeType (2.0f));
            juce::Path heads;
            heads.addTriangle (c.x + 8.0f, c.y - 8.0f, c.x + 12.0f, c.y - 1.0f, c.x + 3.0f, c.y - 2.0f);
            heads.addTriangle (c.x - 8.0f, c.y + 8.0f, c.x - 12.0f, c.y + 1.0f, c.x - 3.0f, c.y + 2.0f);
            g.fillPath (heads);
            g.setColour (fg);
        }
        else if (icon != Icon::none)
        {
            auto p = iconPath (icon);
            g.fillPath (p, p.getTransformToScaleToFit (top.withSizeKeepingCentre (16.0f, 16.0f), true));
        }
        g.setFont (lf.cond (13.0f, true).withExtraKerningFactor (0.1f));
        g.drawText (word, content, juce::Justification::centredTop, true);
        return;
    }
    if (icon == Icon::turn)
    {
        // two curved arrows, the cassette going round
        auto box = content.removeFromLeft (word.isEmpty() ? content.getWidth() : 20.0f).withSizeKeepingCentre (20.0f, 20.0f);
        juce::Path a;
        a.addCentredArc (box.getCentreX(), box.getCentreY(), 8.0f, 8.0f, 0.0f, -2.6f, 0.3f, true);
        a.addCentredArc (box.getCentreX(), box.getCentreY(), 8.0f, 8.0f, 0.0f, 0.54f, 3.44f, true);
        g.setColour (Pal::orange);
        g.strokePath (a, juce::PathStrokeType (2.0f));
        juce::Path heads;
        heads.addTriangle (box.getCentreX() + 7.0f, box.getCentreY() - 7.0f, box.getCentreX() + 11.0f, box.getCentreY() - 1.0f, box.getCentreX() + 3.0f, box.getCentreY() - 2.0f);
        heads.addTriangle (box.getCentreX() - 7.0f, box.getCentreY() + 7.0f, box.getCentreX() - 11.0f, box.getCentreY() + 1.0f, box.getCentreX() - 3.0f, box.getCentreY() + 2.0f);
        g.fillPath (heads);
        g.setColour (fg);
        content.removeFromLeft (8.0f);
    }
    else if (icon != Icon::none)
    {
        const float size = word.isEmpty() ? juce::jmin (18.0f, content.getHeight() * 0.5f) : 14.0f;
        auto box = word.isEmpty() ? content.withSizeKeepingCentre (icon == Icon::ff || icon == Icon::rew ? size * 1.6f : size, size)
                                  : content.removeFromLeft (size + 2.0f).withSizeKeepingCentre (size, size);
        auto p = iconPath (icon);
        g.fillPath (p, p.getTransformToScaleToFit (box, true));
        if (word.isNotEmpty()) content.removeFromLeft (8.0f);
    }
    if (word.isNotEmpty())
    {
        g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.12f));
        g.drawText (word, content, icon == Icon::none ? juce::Justification::centred : juce::Justification::centredLeft, true);
    }
}

//==============================================================================
struct ShelfView::Tile : public juce::Component
{
    Tile (const CassetteEntry& e, CassetteArtwork art, bool isCurrent) : entry (e), current (isCurrent)
    {
        missing = ! e.file.existsAsFile();
        look.setArtwork (std::move (art));
        look.setSide (e.side);
        look.setPlayed (e.fraction, 0.0);
        look.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (look);
        setTitle (e.label.isNotEmpty() ? e.label : e.product);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        look.setBounds (r.removeFromTop (juce::roundToInt ((float) r.getWidth() * 638.0f / 1004.0f)));
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (current ? juce::Colour (0xff2e251d) : juce::Colour (0xff1b1a18));
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (current ? Pal::orange : juce::Colour (0xff33312d));
        g.drawRoundedRectangle (r, 12.0f, 1.0f);

        auto text = getLocalBounds().reduced (12, 10);
        text.removeFromTop (look.getHeight() + 6);
        g.setColour (Pal::paper);
        g.setFont (lf.hand (22.0f));
        g.drawText (entry.label.isNotEmpty() ? entry.label : entry.product, text.removeFromTop (24), juce::Justification::centredLeft, true);
        g.setColour (Pal::muted);
        g.setFont (lf.cond (13.0f).withExtraKerningFactor (0.06f));
        g.drawText (entry.product + utf (" \xc2\xb7 ") + (entry.stereo ? "STEREO" : "4-TRACK") + utf (" \xc2\xb7 ") + entry.grade.toUpperCase(),
                    text.removeFromTop (17), juce::Justification::centredLeft, true);
        g.setColour (missing ? Pal::red : Pal::lcdText);
        g.setFont (lf.mono (14.0f));
        g.drawText (missing ? juce::String ("FILE MISSING") : (current ? juce::String ("IN THE PLAYER  ") : juce::String())
                        + (entry.side == 0 ? "SIDE A" : "SIDE B"),
                    text.removeFromTop (18), juce::Justification::centredLeft, true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onClick) onClick();
    }

    CassetteEntry entry;
    LookView look;
    bool current, missing = false;
    std::function<void()> onClick;
};

ShelfView::ShelfView (Library& l, bool isTouch) : library (l), touch (isTouch)
{
    openKey.onClick = [this] { if (onOpenFile) onOpenFile(); };
    closeKey.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (openKey);
    addAndMakeVisible (closeKey);
    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible (viewport);
}

void ShelfView::refresh (const juce::String& currentId)
{
    tiles.clear();
    for (auto& e : library.recent())
    {
        auto it = artCache.find (e.id);
        if (it == artCache.end() && e.file.existsAsFile())
            it = artCache.emplace (e.id, Library::artworkOf (e.file)).first;
        auto* t = tiles.add (new Tile (e, it != artCache.end() ? it->second : CassetteArtwork(), e.id == currentId));
        auto file = e.file;
        t->onClick = [this, file, missing = t->missing]
        {
            if (missing) { library.forget (file); refresh ({}); return; }
            if (onPick) onPick (file);
        };
        grid.addAndMakeVisible (t);
    }
    resized();
    repaint();
}

void ShelfView::resized()
{
    auto r = getLocalBounds().reduced (touch ? 24 : 20, 18);
    auto head = r.removeFromTop (44);
    closeKey.setBounds (head.removeFromRight (100));
    head.removeFromRight (12);
    openKey.setBounds (head.removeFromRight (220));
    r.removeFromTop (16);
    viewport.setBounds (r);

    const int columns = juce::jmax (1, r.getWidth() / 200);
    const int gap = 14;
    const int w = (r.getWidth() - gap * (columns - 1) - (viewport.isVerticalScrollBarShown() ? 8 : 0)) / columns;
    const int h = juce::roundToInt ((float) (w - 20) * 638.0f / 1004.0f) + 86;
    for (int i = 0; i < tiles.size(); ++i)
        tiles[i]->setBounds ((i % columns) * (w + gap), (i / columns) * (h + gap), w, h);
    grid.setSize (r.getWidth(), tiles.isEmpty() ? 10 : ((tiles.size() + columns - 1) / columns) * (h + gap));
}

void ShelfView::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    g.fillAll (juce::Colour (0xff0f0f0e));
    auto r = getLocalBounds().reduced (touch ? 24 : 20, 18);
    g.setColour (Pal::cream);
    g.setFont (lf.cond (26.0f, true).withExtraKerningFactor (0.08f));
    g.drawText ("CASSETTES", r.removeFromTop (44), juce::Justification::centredLeft);
    if (tiles.isEmpty())
    {
        g.setColour (Pal::muted);
        g.setFont (lf.cond (16.0f));
        g.drawText ("No cassettes yet. Open a .cass file" + juce::String (touch ? ", or send one to the player from another app." : ", or drop one onto the player."),
                    r.withTrimmedTop (40).removeFromTop (30), juce::Justification::centredLeft);
    }
}

//==============================================================================
PlayerComponent::PlayerComponent (PlayerEngine& e, Library& l, bool touchLayout)
    : engine (e), library (l), touch (touchLayout), shelf (l, touchLayout)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    look.setInterceptsMouseClicks (false, false);
    addChildComponent (look);

    addChildComponent (pillBack);      // under the keys, over the cassette
    addChildComponent (volumeBack);
    for (auto* k : { &rew, &stop, &play, &ff }) { k->solid = true; addChildComponent (k); }
    if (touch) { turn.stacked = true; eject.stacked = true; }
    rew.setTitle ("Rewind");
    stop.setTitle ("Stop");
    play.setTitle ("Play");
    ff.setTitle ("Fast forward");
    volUp.setTitle ("Volume up");
    volDown.setTitle ("Volume down");
    rew.onClick  = [this] { engine.press (PlayerEngine::Key::rew); };
    stop.onClick = [this] { engine.press (PlayerEngine::Key::stop); };
    play.onClick = [this] { engine.press (PlayerEngine::Key::play); };
    ff.onClick   = [this] { engine.press (PlayerEngine::Key::ff); };
    turn.onClick = [this]
    {
        engine.turnOver();
        look.setSide (engine.getSide());
        say (engine.getSide() == 0 ? "SIDE A" : "SIDE B", 1.5);
    };
    eject.onClick = [this] { showShelf (true); };
    nr.onClick = [this]
    {
        engine.setNoiseReduction (! engine.getNoiseReduction());
        refreshKeys();
    };
    // A phone has one volume, its own: these keys set it, as the side buttons do.
    auto stepVolume = [this] (int direction)
    {
        if (SystemVolume::available())
            SystemVolume::set (SystemVolume::get() + (float) direction / (float) SystemVolume::steps());
        else
        {
            engine.setVolume (engine.getVolume() + direction);
            volume.setValue (engine.getVolume(), juce::dontSendNotification);
        }
        repaint();
    };
    volUp.onClick = [stepVolume] { stepVolume (1); };
    volDown.onClick = [stepVolume] { stepVolume (-1); };
    openFirst.onClick = [this] { chooseFile(); };
    for (auto* k : { &turn, &eject, &nr }) addChildComponent (k);
    addChildComponent (volUp);
    addChildComponent (volDown);
    addChildComponent (openFirst);

    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setRange (0, 10, 1);
    volume.setValue (engine.getVolume(), juce::dontSendNotification);
    volume.setColour (juce::Slider::trackColourId, Pal::orange);
    volume.setColour (juce::Slider::backgroundColourId, keyColour);
    volume.setColour (juce::Slider::thumbColourId, Pal::cream);
    volume.setTitle ("Volume");
    volume.onValueChange = [this] { engine.setVolume ((int) volume.getValue()); repaint (counterArea); repaint (volumeArea); };
    addChildComponent (volume);

    shelf.onPick = [this] (const juce::File& f) { showShelf (false); openCassette (f); };
    shelf.onOpenFile = [this] { chooseFile(); };
    shelf.onClose = [this] { showShelf (false); };
    addChildComponent (shelf);

    refreshKeys();
    startTimerHz (30);
#if JUCE_ANDROID
    gl.setComponentPaintingEnabled (true);
    gl.attachTo (*this);
#endif
}

PlayerComponent::~PlayerComponent()
{
#if JUCE_ANDROID
    gl.detach();
#endif
    rememberPosition();
}

void PlayerComponent::chooseFile()
{
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Open a cassette", start, touch ? juce::String() : juce::String ("*.cass"));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<PlayerComponent> (this), isTouch = touch] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto url = fc.getURLResult();
        if (url.isEmpty()) return;
        safe->showShelf (false);
        auto local = url.isLocalFile() ? url.getLocalFile() : juce::File();
        if (! isTouch && local.existsAsFile())
        {
            safe->openCassette (local);
            return;
        }
        safe->importAndOpen (url);
    });
}

void PlayerComponent::importAndOpen (const juce::URL& url)
{
    auto local = url.isLocalFile() ? url.getLocalFile() : juce::File();
    if (local.isAChildOf (Library::cassettesFolder())) { openCassette (local); return; }
    // A phone: the cassette is copied into the player's own folder first (another app's file
    // may only be readable for a moment), then played from there.
    auto name = juce::URL::removeEscapeChars (url.getFileName()).upToLastOccurrenceOf (".", false, false);
    auto target = Library::cassettesFolder().getNonexistentChildFile (
        juce::File::createLegalFileName (name.isNotEmpty() ? name : juce::String ("cassette")), ".cass", false);
    say ("COPYING THE CASSETTE...", 60.0);
    juce::Thread::launch ([safe = juce::Component::SafePointer<PlayerComponent> (this), url, target]
    {
        bool ok = false;
        if (auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)))
        {
            juce::FileOutputStream out (target);
            ok = out.openedOk() && out.writeFromInputStream (*in, -1) > 0;
            out.flush();
            ok = ok && out.getStatus().wasOk();
        }
        if (! ok) target.deleteFile();
        juce::MessageManager::callAsync ([safe, target, ok]
        {
            if (safe == nullptr) return;
            if (ok) safe->openCassette (target);
            else    safe->say ("COULD NOT COPY THE CASSETTE");
        });
    });
}

void PlayerComponent::openCassette (const juce::File& file)
{
    rememberPosition();
    auto opened = library.open (file);
    if (opened.tape == nullptr)
    {
        say ("COULD NOT OPEN: " + opened.error.toUpperCase());
        return;
    }
    current = opened.entry;
    haveCassette = true;
    look.setArtwork (std::move (opened.art));
    engine.load (std::move (opened.tape), current.side, current.position);
    look.setSide (engine.getSide());
    lastPosition = engine.getPosition();
    lastSide = engine.getSide();
    look.setPlayed (engine.getFraction(), 0.0);
    library.remember (current);
    controlsShown = touch;   // a phone shows its keys once, so you can see where they are
    resized();
    refreshKeys();
    say (current.label.isNotEmpty() ? current.label.toUpperCase() : current.product, 2.5);
}

void PlayerComponent::rememberPosition()
{
    if (! haveCassette || engine.getTape() == nullptr) return;
    current.side = engine.getSide();
    current.position = engine.getPosition();
    current.fraction = engine.getFraction();
    current.lastPlayed = juce::Time::currentTimeMillis();
    library.remember (current);
}

void PlayerComponent::showShelf (bool show)
{
    if (show)
    {
        shelf.refresh (haveCassette ? current.id : juce::String());
        shelf.setBounds (getLocalBounds());
        shelf.toFront (false);
    }
    shelf.setVisible (show);
    repaint();
    if (! show) grabKeyboardFocus();
}

void PlayerComponent::say (const juce::String& m, double seconds)
{
    message = m;
    messageUntil = juce::Time::getMillisecondCounterHiRes() * 0.001 + seconds;
    repaint();
}

juce::String PlayerComponent::counterText() const
{
    // A portable has no counter: you see where you are from the spools. It shows which side is up.
    const auto mode = engine.getMode();
    if (engine.isCueing()) return mode == Mode::rew ? "REVIEW" : "CUE";
    return engine.getSide() == 0 ? "SIDE A" : "SIDE B";
}

void PlayerComponent::refreshKeys()
{
    const auto mode = engine.getMode();
    play.setOn (mode == Mode::play || engine.isCueing());
    ff.setOn (mode == Mode::ff);
    rew.setOn (mode == Mode::rew);
    static const char* systems[] { "NR 2:1", "NR B", "NR C" };
    nr.setButtonText (engine.getNoiseReduction() ? systems[juce::jlimit (0, 2, engine.getNoiseReductionType())] : "NR OFF");
    nr.setOn (engine.getNoiseReduction());

    const bool keysVisible = haveCassette && (! touch || controlsShown);
    for (auto* k : { &rew, &stop, &play, &ff, &turn, &eject, &nr })
        k->setVisible (keysVisible);
    // the cassettes key stays reachable even before a cassette is in
    if (! haveCassette && ! touch) eject.setVisible (true);
    pillBack.setVisible (keysVisible && touch);
    volumeBack.setVisible (keysVisible && touch);
    volUp.setVisible (keysVisible && touch);
    volDown.setVisible (keysVisible && touch);
    volume.setVisible (keysVisible && ! touch);
    look.setVisible (haveCassette);
    openFirst.setVisible (! haveCassette);
}

void PlayerComponent::resized()
{
    if (touch) layoutTouch();
    else       layoutBar();
    openFirst.setBounds (getLocalBounds().withSizeKeepingCentre (260, 56));
    if (shelf.isVisible()) shelf.setBounds (getLocalBounds());
}

void PlayerComponent::layoutBar()
{
    auto r = getLocalBounds();
    barArea = r.removeFromBottom (68);
    look.setBounds (r.reduced (20, 16));

    auto b = barArea.reduced (16, 12);
    rew.setBounds (b.removeFromLeft (56)); b.removeFromLeft (8);
    stop.setBounds (b.removeFromLeft (56)); b.removeFromLeft (8);
    play.setBounds (b.removeFromLeft (68)); b.removeFromLeft (8);
    ff.setBounds (b.removeFromLeft (56)); b.removeFromLeft (12);
    counterArea = b.removeFromLeft (96);
    b.removeFromLeft (8);
    turn.setBounds (b.removeFromLeft (118)); b.removeFromLeft (8);
    nr.setBounds (b.removeFromLeft (76));
    eject.setBounds (b.removeFromRight (122));
    b.removeFromRight (16);
    volumeArea = b.removeFromRight (juce::jmin (190, juce::jmax (0, b.getWidth() - 16)));
    volume.setBounds (volumeArea.withTrimmedLeft (36));
}

void PlayerComponent::layoutTouch()
{
    auto r = getLocalBounds();
    look.setBounds (r.reduced (8));

    leftColumn = r.removeFromLeft (112).withSizeKeepingCentre (88, 140);
    turn.setBounds (leftColumn.removeFromTop (64));
    leftColumn.removeFromTop (12);
    eject.setBounds (leftColumn.removeFromTop (64));

    rightColumn = getLocalBounds().removeFromRight (112).withSizeKeepingCentre (88, 210);
    auto v = rightColumn.removeFromTop (150);
    volumeArea = v;
    volumeBack.setBounds (v);
    volUp.setBounds (v.removeFromTop (48).reduced (4));
    volDown.setBounds (v.removeFromBottom (48).reduced (4));
    rightColumn.removeFromTop (12);
    nr.setBounds (rightColumn.removeFromTop (48));

    pillArea = juce::Rectangle<int> (60 * 4 + 72 + 6 * 5 + 110, 60).withCentre ({ getWidth() / 2, getHeight() - 44 });
    pillBack.setBounds (pillArea);
    auto p = pillArea.reduced (6);
    rew.setBounds (p.removeFromLeft (60)); p.removeFromLeft (6);
    stop.setBounds (p.removeFromLeft (60)); p.removeFromLeft (6);
    play.setBounds (p.removeFromLeft (72)); p.removeFromLeft (6);
    ff.setBounds (p.removeFromLeft (60)); p.removeFromLeft (6);
    counterArea = p;
}

void PlayerComponent::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    g.fillAll (screen);

    if (! touch)
    {
        g.setColour (barColour);
        g.fillRect (barArea);
        g.setColour (keyLine);
        g.fillRect (barArea.removeFromTop (1));
    }

    if (! haveCassette)
    {
        g.setColour (Pal::muted);
        g.setFont (lf.cond (16.0f));
        g.drawText (touch ? "Or send a .cass to the player from another app" : "Or drop a .cass file anywhere on the player",
                    openFirst.getBounds().translated (0, 50).withSizeKeepingCentre (500, 24), juce::Justification::centred);
        g.setColour (Pal::orange);
        g.setFont (lf.cond (34.0f, true));
        g.drawText ("TF-P1", openFirst.getBounds().translated (0, -90).withSizeKeepingCentre (300, 40), juce::Justification::centred);
        g.setColour (Pal::cream);
        g.setFont (lf.cond (16.0f, true).withExtraKerningFactor (0.22f));
        g.drawText ("POCKET CASSETTE PLAYER", openFirst.getBounds().translated (0, -56).withSizeKeepingCentre (400, 24), juce::Justification::centred);
    }
}

void PlayerComponent::timerCallback()
{
    // The hubs turn by the tape that passed since the last frame.
    const auto pos = engine.getPosition();
    const auto side = engine.getSide();
    const auto moved = side == lastSide ? (pos - lastPosition) / cellRate : 0.0;
    lastPosition = pos;
    lastSide = side;
    if (haveCassette) look.setPlayed (engine.getFraction(), moved);
    refreshKeys();
    repaint (counterArea);

    if (! juce::exactlyEqual (moved, 0.0)) ticksSinceSave++;
    if (ticksSinceSave > 30 * 5) { ticksSinceSave = 0; rememberPosition(); }   // where it is, every five seconds of running
    if (! message.isEmpty() && juce::Time::getMillisecondCounterHiRes() * 0.001 > messageUntil) { message.clear(); repaint(); }
}

void PlayerComponent::paintOverChildren (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    if (haveCassette && (! touch || controlsShown) && ! shelf.isVisible())
    {
        if (touch)
        {
            g.setColour (Pal::lcdText);
            g.setFont (lf.mono (22.0f));
            const auto shown = SystemVolume::available() ? juce::roundToInt (SystemVolume::get() * 10.0f) : engine.getVolume();
            g.drawText (juce::String (shown), volumeArea.withSizeKeepingCentre (80, 26).translated (0, -6), juce::Justification::centred);
            g.setColour (Pal::muted);
            g.setFont (lf.cond (11.0f, true).withExtraKerningFactor (0.2f));
            g.drawText ("VOLUME", volumeArea.withSizeKeepingCentre (88, 16).translated (0, 14), juce::Justification::centred);
        }
        else
        {
            g.setColour (Pal::muted);
            g.setFont (lf.cond (12.0f, true).withExtraKerningFactor (0.2f));
            g.drawText ("VOL", volumeArea.withWidth (36), juce::Justification::centredLeft);
        }
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (20.0f));
        g.drawText (counterText(), counterArea, juce::Justification::centred);
    }

    if (message.isNotEmpty())
    {
        auto box = juce::Rectangle<int> (juce::jmin (getWidth() - 40, 520), 40).withCentre ({ getWidth() / 2, 36 });
        g.setColour (overlay);
        g.fillRoundedRectangle (box.toFloat(), 10.0f);
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (17.0f));
        g.drawText (message, box.reduced (12, 0), juce::Justification::centred, true);
    }
}

void PlayerComponent::Backdrop::paint (juce::Graphics& g)
{
    g.setColour (overlay);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 16.0f);
    g.setColour (keyLine);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 16.0f, 1.0f);
}

void PlayerComponent::mouseUp (const juce::MouseEvent& e)
{
    // On a phone, tapping the cassette shows or hides its keys.
    if (touch && haveCassette && e.mouseWasClicked())
    {
        controlsShown = ! controlsShown;
        refreshKeys();
        repaint();
    }
}

bool PlayerComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        engine.press (engine.getMode() == Mode::play ? PlayerEngine::Key::stop : PlayerEngine::Key::play);
        return true;
    }
    if (k == juce::KeyPress::escapeKey && shelf.isVisible()) { showShelf (false); return true; }
    if (k == juce::KeyPress::rightKey) { engine.press (PlayerEngine::Key::ff); return true; }
    if (k == juce::KeyPress::leftKey)  { engine.press (PlayerEngine::Key::rew); return true; }
    if (k == juce::KeyPress::upKey)    { volUp.triggerClick(); return true; }
    if (k == juce::KeyPress::downKey)  { volDown.triggerClick(); return true; }
    return false;
}

bool PlayerComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (f.endsWithIgnoreCase (".cass")) return true;
    return false;
}

void PlayerComponent::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".cass")) { showShelf (false); openCassette (juce::File (f)); return; }
}
} // namespace tf::pocket
