#pragma once

#include "../Source/LookView.h"
#include "../Source/Tape.h"

namespace tf::pocket
{
/** A cassette the player has opened: where its file is, what it is, and where it was stopped. */
struct CassetteEntry
{
    juce::File file;
    juce::String id, label, product, grade;
    int model = defaultTapeModel;
    bool stereo = true;
    int side = 0;
    double position = 0;       // cells from the start of the side facing up
    double lengthCells = 0;
    double fraction = 0;       // how much of the tape is on the right-hand spool (for drawing it)
    juce::int64 lastPlayed = 0;   // ms since 1970
};

/** A cassette opened for playing: the tape (read where it lies), its look and its entry. */
struct OpenedCassette
{
    std::unique_ptr<Tape> tape;
    CassetteArtwork art;
    CassetteEntry entry;
    juce::String error;
};

/** The cassettes the player knows, most recently played first, kept in its settings. */
class Library
{
public:
    explicit Library (juce::PropertiesFile& settings);

    /** The colour theme chosen on the shelf (Theme::all()'s id). */
    juce::String theme() const { return props.getValue ("theme", "teal"); }
    void setTheme (const juce::String& id) { props.setValue ("theme", id); props.saveIfNeeded(); }

    std::vector<CassetteEntry> recent() const;
    /** Where this cassette was left, if the player has played it before. */
    std::optional<CassetteEntry> find (const juce::String& id) const;
    void remember (const CassetteEntry&);
    void forget (const juce::File&);

    /** Opens a .cass for playing, with its look (or, for a cassette without one, the built-in look of its kind). */
    OpenedCassette open (const juce::File& cassFile) const;
    /** Just its look, for showing it on the shelf. */
    static CassetteArtwork artworkOf (const juce::File& cassFile, const Tape* tapeIfOpen = nullptr);

    /** Phones: where the player keeps its cassettes (iOS: its Documents folder, which the Files app shows;
        Android: its own private storage).
        Files shared to the player arrive here, or in its Inbox inside it. */
    static juce::File cassettesFolder();
    /** Brings in .cass files that have arrived in the cassettes folder (and its Inbox), so they
        show among the cassettes. Returns the new ones, newest first. */
    juce::Array<juce::File> takeInNewArrivals();

    static constexpr int maxRemembered = 24;

private:
    juce::PropertiesFile& props;
};
} // namespace tf::pocket
