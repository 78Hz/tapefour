#pragma once

#include "Json.h"

#include <array>
#include <filesystem>
#include <string>

namespace tfmt
{
/**
    How a cassette looks: art/look.json and the images beside it (docs/formats/cass-v1.md, "The look").

    A player draws the cassette in five layers, back to front:
      1. back      the shell behind the works (an image)
      2. packs     the tape wound on each hub: drawn by the player, sized from the position
      3. tape      the tape's path round the guides: drawn by the player
      4. hubs      the hub image, at each reel, turned as the tape moves
      5. front     everything in front of the works, label included: one image per side

    Every coordinate is in tenths of a millimetre on the shell, from its top left corner, as the
    cassette lies side A up. The images cover the whole shell at `scale` pixels per tenth of a mm.
*/
struct Look
{
    struct Point { double x = 0, y = 0; };
    struct Rect  { double x = 0, y = 0, w = 0, h = 0; };

    std::string design;            // who drew it and which design: "tapefour.sd" (informative)
    std::string labelText;         // the label text written on the fronts (a writer redraws them when it changes)
    double width = 1004, height = 638;   // the shell, in tenths of a mm (a compact cassette: 100.4 x 63.8 mm)
    double scale = 2;              // image pixels per tenth of a mm

    std::string back, frontA, frontB, hub;   // image files, relative to the cassette ("art/back.png")
    double hubSize = 224;          // the hub image is a square this wide, centred on each reel
    std::array<Point, 2> reels { { { 292, 300 }, { 712, 300 } } };

    struct Pack
    {
        double minDiameter = 216;  // the leader alone, on an empty hub
        double fullDiameter = 0;   // all of the tape on one hub
        std::string inner, outer;  // a radial gradient from the centre to the pack's edge (#rrggbbaa)
        std::string ring;          // fine rings across the pack, like wound tape catching the light
        double ringWidth = 2, ringSpacing = 26, ringInset = 10, ringStop = 104;
    } pack;

    struct Tape
    {
        std::string colour;
        double width = 4;
        std::array<Point, 2> guides { { { 118, 586 }, { 886, 586 } } };
        double wrapRadius = 30;    // the tape runs round the underside of each guide at this radius
    } tape;

    Rect label { 68, 398, 868, 80 };   // the write-on label, for players that show it off
    Json unknown;                  // members from a newer version, kept on rewrite
};

inline constexpr const char* lookFile = "art/look.json";

Json toJson (const Look&);
bool fromJson (const Json&, Look&, std::string& error);
/** Reads art/look.json from an unpacked cassette. False (with why) if it has none or it can't be read. */
bool readLook (const std::filesystem::path& cassetteFolder, Look& look, std::string& error);
/** How big each pack is, as a diameter in tenths of a mm, with `fractionOnIt` of the tape wound on it (0..1). */
double packDiameter (const Look&, double fractionOnIt) noexcept;
} // namespace tfmt
