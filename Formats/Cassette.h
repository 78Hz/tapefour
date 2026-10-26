#pragma once

/*
    The cassette format (.cass), version 1. The specification is docs/formats/cass-v1.md;
    this is its reference reader and writer. Plain C++17.

    A cassette is a set of files. Unpacked, they sit in a folder (how TAPE-FOUR keeps the
    tapes on its shelf); packed, they are the entries of a .cass file, a zip archive whose
    entries are stored, not compressed. The files are the same either way, byte for byte.
*/

#include "Json.h"
#include "Zip.h"   // pathFromUtf8

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tfmt
{
inline constexpr int cassFormatVersion = 1;

namespace CassFiles
{
    inline constexpr const char* manifest = "cassette.json";   // what the cassette is
    inline constexpr const char* state    = "state.json";      // what has happened to it since
    inline constexpr const char* wear     = "wear.bin";        // passes over the heads, per second of tape
    std::string pcm (int track);   // track1.pcm ... (track counts from 0 here)
    std::string map (int track);   // track1.map ...
    inline constexpr const char* artFolder = "art/";           // reserved: cover art and shell design
}

/** How the tracks lie across the tape. */
enum class Layout
{
    fourTrackOneWay,   // a multitracker's tape: four tracks, all played in one direction
    stereoTwoSides     // a normal cassette: tracks 1+2 are side A left and right, 3+4 side B, running the other way
};
const char* layoutName (Layout) noexcept;
std::optional<Layout> layoutFromName (const std::string&) noexcept;

/** The tape's formulation: measured figures (dB re Dolby level at 1 7/8 ips). */
struct Formulation
{
    std::string name, line, position, basedOn, accent;   // printed names; accent "#rrggbb"
    int iecType = 1;              // 1 normal, 2 chrome, 4 metal
    int eqMicroseconds = 120;     // playback EQ the tape is made for: 120 or 70
    double mol100 = 0, mol400 = 0, sol10k = 0;
    double snrDbA = 0;            // bias noise below Dolby level, A-weighted, no NR
    double modNoiseDb = 0;        // modulation noise re signal
    double sensitivityDb = 0;     // output re the IEC reference tape

    bool operator== (const Formulation&) const;
};

/** What the cassette was like when it was made. Wear and age (in the state) add to it. */
struct Condition
{
    std::string grade = "new";      // new, used, worn, chewed
    double hissDb = 0;              // hiss above the formulation's measured figure
    double dropoutsPerMinute = 0;   // per track, at a fresh pass
    double trebleLossDb = 0;        // at 10 kHz, from oxide wear
    double modNoiseDb = 0;          // added modulation noise
    double shellFlutterPct = 0;     // flutter from the shell, pads and guides

    static Condition forGrade (const std::string& grade);
    static const std::vector<std::string>& grades();
    bool operator== (const Condition&) const;
};

struct TrackInfo
{
    std::string pcm, map;                  // file names
    std::string encoding = "pcm-s16le";    // version 1 has only this
    double fluxScale = 0.5;                // the cell value that means full flux, as a fraction of full scale
    bool operator== (const TrackInfo&) const;
};

/** cassette.json */
struct CassetteInfo
{
    int version = cassFormatVersion;
    std::string id;           // unique: a UUID
    std::string created;      // ISO 8601, UTC: "2026-10-27T09:30:00Z"
    std::string madeBy;       // the program that made it
    std::string label;        // written on the label

    Formulation formulation;
    int minutesPerSide = 30;
    int cellRate = 32000;             // cells per second of tape at the reference speed
    double referenceSpeedIps = 1.875;
    int64_t cellsPerTrack = 0;
    int blockCells = 3200;            // the resolution of the track maps
    Layout layout = Layout::fourTrackOneWay;
    std::vector<TrackInfo> tracks;
    std::vector<std::string> recorders;   // decks that have recorded on it; the maps refer to them by number (1 = the first)

    Condition condition;
    std::string inlayLayout;              // how to read the inlay lines, e.g. "tf4-inlay-1"
    std::vector<std::string> inlay;

    Json unknown;   // members from a newer version, kept so rewriting the file doesn't lose them

    int64_t numBlocks() const noexcept  { return blockCells > 0 ? (cellsPerTrack + blockCells - 1) / blockCells : 0; }
    int64_t numSeconds() const noexcept { return cellRate > 0 ? (cellsPerTrack + cellRate - 1) / cellRate : 0; }

    /** A new cassette's description: id, date and tracks filled in. */
    static CassetteInfo make (const Formulation&, int minutesPerSide, Layout, const Condition&, const std::string& madeBy);
};

/** state.json: what can change after the cassette is made. */
struct CassetteState
{
    bool protectA = false, protectB = false;   // write-protect tabs broken out
    double position = 0;                       // where the tape is wound to, in cells along it
    std::string lastUsed;                      // ISO 8601, UTC
    // A summary for lists of cassettes, so they needn't read every map: may be recomputed from the maps.
    double usedSeconds = 0;                    // to the end of the last recording
    int tracksUsed = 0;
    Json unknown;
};

/** One track's map: per block of tape, what was recorded there. */
struct TrackMap
{
    enum : uint8_t { recorded = 1, nrEncoded = 2 };
    // flags: bit 0 recorded, bit 1 NR encoded, bits 2-3 tape switch position + 1 (0 unknown),
    //        bits 4-6 NR system + 1 (0 unknown), bit 7 reserved
    std::vector<uint8_t> flags;
    std::vector<uint8_t> recorder;      // index into CassetteInfo::recorders + 1; 0 unknown
    std::vector<uint16_t> speedMilli;   // speed relative to the reference, x 1000 (incl. varispeed)

    void resize (size_t blocks) { flags.resize (blocks); recorder.resize (blocks); speedMilli.resize (blocks); }
};

Json toJson (const CassetteInfo&);
bool fromJson (const Json&, CassetteInfo&, std::string& error);
Json toJson (const CassetteState&);
bool fromJson (const Json&, CassetteState&, std::string& error);

/** The current time as ISO 8601 UTC. */
std::string isoNow();
/** A new random UUID. */
std::string newUuid();

//==============================================================================
// Files in a folder. Writes replace the file atomically (written beside it, then renamed).

bool readManifest (const std::filesystem::path& folder, CassetteInfo&, std::string& error);
bool writeManifest (const std::filesystem::path& folder, const CassetteInfo&, std::string& error);
bool readState (const std::filesystem::path& folder, CassetteState&, std::string& error);
bool writeState (const std::filesystem::path& folder, const CassetteState&, std::string& error);
bool readMap (const std::filesystem::path& file, size_t blocks, TrackMap&);
bool writeMap (const std::filesystem::path& file, const TrackMap&);
bool readWear (const std::filesystem::path& file, size_t seconds, std::vector<uint16_t>& passes);
bool writeWear (const std::filesystem::path& file, const std::vector<uint16_t>& passes);
bool writeTextAtomically (const std::filesystem::path& file, const std::string& text);

//==============================================================================
// Packed .cass files.

/** The files of a cassette in a folder, in the order they are packed: the ones that exist. */
std::vector<std::string> cassetteFiles (const std::filesystem::path& folder, const CassetteInfo&);
/** Packs the cassette in folder into a .cass file (written beside it, then renamed). */
bool packCassette (const std::filesystem::path& folder, const std::filesystem::path& cassFile, std::string& error);
/** Reads the description of a packed cassette without unpacking it. */
bool readPackedManifest (const std::filesystem::path& cassFile, CassetteInfo&, std::string& error);
/** Unpacks a .cass file into folder (created). Only the files the format defines are unpacked. */
bool unpackCassette (const std::filesystem::path& cassFile, const std::filesystem::path& folder, std::string& error);
} // namespace tfmt
