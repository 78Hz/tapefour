#pragma once

#include "Tape.h"

namespace tf
{
/** Where whole tapes live on disk: Documents/TapeFour/Tape Shelf/<tape id>/, each an unpacked cassette
    (docs/formats/cass-v1.md) with its session beside it. */
struct TapeShelf
{
    struct Entry
    {
        juce::String id, name, created;
        int type = 1, model = defaultTapeModel;   // the nearest standard length, the built-in formulation (for its look)
        int minutes = 30;                         // per side
        juce::String formulation, grade;          // as the cassette says
        tfmt::Layout layout = tfmt::Layout::fourTrackOneWay;   // a stereo one is a master tape
        double usedSeconds = 0;
        int tracksUsed = 0;
        juce::Time modified;
    };

    static juce::File getFolder();
    /** Test hook: keep the shelf somewhere else (an empty file puts it back). */
    static void setFolderForTests (const juce::File& folder);
    static juce::File folderFor (const juce::String& id) { return getFolder().getChildFile (id); }
    static std::vector<Entry> list();
    static bool save (Tape& tape);
    static std::unique_ptr<Tape> load (const juce::String& id);
    /** A new blank cassette; grade is its condition: new, used, worn or chewed. */
    static std::unique_ptr<Tape> createBlank (int type, int model = defaultTapeModel, const juce::String& grade = "new");

    // ---- .cass files (docs/formats/cass-v1.md) ----
    /** Packs a cassette on the shelf (as last saved) into a .cass file. Its session stays on the shelf. */
    static bool exportCassette (const juce::String& id, const juce::File& target, juce::String& error);
    /** What a .cass file is, without unpacking it. */
    static bool peek (const juce::File& cassFile, tfmt::CassetteInfo& info, juce::String& error);
    /** Puts a .cass file on the shelf (replacing the copy there, if any, but keeping its session).
        Fails, changing nothing, if this deck can't play the cassette. */
    static bool importCassette (const juce::File& cassFile, juce::String& id, juce::String& error);

    /** The disk space a full tape of this length takes (all four tracks recorded end to end), with a margin. */
    static juce::int64 bytesForFullTape (const TapeTypeInfo& type);

    /** Free space where tapes are kept; 0 if the system can't say. */
    static juce::int64 freeSpace();

    /** Free space that's known to be short of what's needed. An unknown amount is never short. */
    static bool isShort (juce::int64 needed) { auto free = freeSpace(); return free > 0 && free < needed; }
};
} // namespace tf
