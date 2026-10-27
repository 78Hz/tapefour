#pragma once

#include <algorithm>
#include <cmath>
#include "Geometry.h"

namespace tf::Wear
{
/*
    What use and age add to a cassette's condition, on top of the condition it was made in.

    Kept deliberately on the gentle side of what real cassettes do: a good tape played a hundred
    times is still a good tape, and one left on a shelf barely changes. These are the knobs for
    tuning by ear: TAPEFOUR_WEAR_RENDER=<folder> EngineTest renders a take at several pass counts
    and ages for listening.

    passes: how many times this stretch of tape has gone past the heads (play or record).
    years:  how long since the cassette was made (oxide and binder), or since a stretch was
            recorded (print-through).
*/

/** Treble lost at 10 kHz (dB) as the oxide's surface wears smooth and sheds: most of a dB after
    hundreds of passes, then no more; age adds a little, up to 0.3 dB. */
inline float trebleLossDb (float passes, float years) noexcept
{
    return 1.0f * passes / (passes + 400.0f) + std::min (0.3f, 0.02f * std::max (0.0f, years));
}

/** Added modulation noise (dB): a worn surface makes the signal a little rougher. */
inline float modNoiseDb (float passes) noexcept
{
    return 2.0f * passes / (passes + 500.0f);
}

/** Dropouts become more frequent as oxide comes away: twice as many after 400 passes. */
inline float dropoutFactor (float passes) noexcept
{
    return 1.0f + passes / 400.0f;
}

/** Print-through: a recording slowly copies itself onto the turns of tape either side of it while
    the cassette sits wound. Level of the copy re the recording (dB): about -61 after a day,
    -54 after a year, -51 after ten. Nothing until a stretch has been recorded. */
inline float printThroughDb (float yearsSinceRecorded) noexcept
{
    if (yearsSinceRecorded <= 0.0f) return -200.0f;
    return -62.0f + 3.0f * std::log10 (1.0f + yearsSinceRecorded * 365.25f);
}

/** How long one turn of tape is (cells) at a point along it, wound on the supply reel with the
    start outermost (rewound for storage): from a full pack's circumference down to the hub's. */
inline double turnCells (double fractionAlong, float fullPackMm, int cellsPerSecond) noexcept
{
    constexpr double hubRadiusMm = Geometry::hubRadiusMm, speedMmPerSecond = Geometry::speedMmPerSecond;
    const double outer = std::max (hubRadiusMm + 1.0, (double) fullPackMm * 0.5);
    const double r = std::sqrt (outer * outer - (outer * outer - hubRadiusMm * hubRadiusMm) * std::clamp (fractionAlong, 0.0, 1.0));
    return 2.0 * 3.14159265358979 * r / speedMmPerSecond * cellsPerSecond;
}
} // namespace tf::Wear
