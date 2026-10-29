#pragma once

#include "PlayerEngine.h"
#include "Library.h"
#include "../Source/DeckLookAndFeel.h"

#if JUCE_ANDROID
 #include <juce_opengl/juce_opengl.h>
#endif

namespace tf::pocket
{
/** A key on the player: rounded, with an icon and/or a word, and a lit state for the latched key. */
class PocketKey : public juce::Button
{
public:
    enum class Icon { none, play, stop, ff, rew, turn, eject, plus, minus, open };
    PocketKey (const juce::String& word, Icon icon, bool accentWhenOn = false);
    void setOn (bool on);
    bool isOn() const noexcept { return lit; }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool solid = false;   // a raised key (the transport) rather than an outlined one
    bool stacked = false; // the icon above the word (narrow keys)

private:
    Icon icon;
    bool lit = false, accent;
};

/** The cassettes the player knows, as small cassettes to pick from, and a way to open another. */
class ShelfView : public juce::Component
{
public:
    ShelfView (Library&, bool touch);
    void refresh (const juce::String& currentId);
    std::function<void (const juce::File&)> onPick;
    std::function<void()> onOpenFile, onClose;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Tile;
    Library& library;
    bool touch;
    PocketKey openKey { "OPEN A .CASS FILE", PocketKey::Icon::open, true }, closeKey { "CLOSE", PocketKey::Icon::none };
    PocketKey themeKey { "THEME", PocketKey::Icon::none };   // each press, the next colour theme
    void showTheme();
    juce::Viewport viewport;
    juce::Component grid;
    juce::OwnedArray<Tile> tiles;
    std::map<juce::String, CassetteArtwork> artCache;   // by cassette id
};

/**
    The pocket player: the cassette, filling the screen, and its keys. On a phone the keys appear when
    you tap the cassette (and hide when you tap again); on a computer they sit in a bar beneath it.
*/
class PlayerComponent : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::Timer
{
public:
    PlayerComponent (PlayerEngine&, Library&, bool touchLayout);
    ~PlayerComponent() override;

    /** Opens a .cass and puts it in, where it was last stopped. */
    void openCassette (const juce::File&);
    /** Remembers where the cassette in the player is (on quitting, say). */
    void rememberPosition();
    void showShelf (bool show);
    void chooseFile();
    /** A cassette from another app (a content:// address on Android, a file elsewhere): copied into
        the player's own folder, then put in. */
    void importAndOpen (const juce::URL&);

    /** On a computer: the screen's pixels per millimetre, so the cassette is drawn at a real cassette's size
        (100.4 x 63.8 mm); the window is sized to it (desktopSize). */
    void setPixelsPerMm (double pxPerMm);
    juce::Point<int> desktopSize() const;
    bool isTouch() const noexcept { return touch; }
    static constexpr double cassetteWidthMm = 100.4, cassetteHeightMm = 63.8;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
#if JUCE_ANDROID
    // Android draws in software by default: too slow for a full-screen cassette at 30 frames a second.
    juce::OpenGLContext gl;
#endif
    void timerCallback() override;
    void layoutTouch();
    void layoutBar();
    double pixelsPerMm = 96.0 / 25.4;
    static constexpr int margin = 12, keyRow = 34, rowGap = 8;
    void refreshKeys();
    void say (const juce::String& message, double seconds = 4.0);
    juce::String counterText() const;

    PlayerEngine& engine;
    Library& library;
    const bool touch;
    bool controlsShown = false;
    LookView look;
    PocketKey rew { {}, PocketKey::Icon::rew }, stop { {}, PocketKey::Icon::stop }, play { {}, PocketKey::Icon::play, true },
              ff { {}, PocketKey::Icon::ff }, turn { "TURN OVER", PocketKey::Icon::turn }, eject { "CASSETTES", PocketKey::Icon::eject },
              nr { "NR B", PocketKey::Icon::none }, volUp { {}, PocketKey::Icon::plus }, volDown { {}, PocketKey::Icon::minus },
              openFirst { "OPEN A .CASS FILE", PocketKey::Icon::open, true };
    juce::Slider volume;
    /** The dark panels the phone's keys sit on: under the keys, over the cassette. */
    struct Backdrop : public juce::Component
    {
        Backdrop() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
    };
    Backdrop pillBack, volumeBack;
    ShelfView shelf;
    std::unique_ptr<juce::FileChooser> chooser;

    CassetteEntry current;
    bool haveCassette = false;
    double lastPosition = 0.0;
    int lastSide = 0, ticksSinceSave = 0;
    juce::String message;
    double messageUntil = 0;
    juce::Rectangle<int> barArea, counterArea, volumeArea, pillArea, leftColumn, rightColumn;

    JUCE_DECLARE_NON_COPYABLE (PlayerComponent)
};
} // namespace tf::pocket
