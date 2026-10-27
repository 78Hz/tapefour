#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

/*
    The cassette as a mechanism: how much tape there is, how thick it is, how big the packs get,
    the leaders at each end, and how fast the spools turn. Everything that draws or moves a
    cassette (the deck, the player, the looks, wear's print-through) uses these numbers.

    Sources (docs/noise-reduction.md lists them with the rest):
      - tape thickness: C-46/C-60 about 15-18 um, C-90 about 10-12 um, C-120 about 6-9 um
        (Wikipedia "Cassette tape" and "Audio tape specifications"; Electronics Notes).
      - 1 7/8 ips = 47.625 mm/s.
      - wind time: TASCAM 424 "120 sec. (approx.) with C-60"; 414 115 s; Porta 05 100 s.
      - leader length: varies by maker (US 5,125,588); a few seconds of tape.
    The hub's winding diameter (22 mm) is the one figure without a published source here.
*/
namespace tf::Geometry
{
inline constexpr double speedMmPerSecond = 47.625;   // 1 7/8 ips
inline constexpr double hubDiameterMm = 22.0;
inline constexpr double hubRadiusMm = hubDiameterMm * 0.5;

/** Total tape thickness (um) for a cassette of this many minutes a side: longer cassettes
    use thinner tape so the full pack stays about the same size. */
inline double thicknessUm (int minutesPerSide) noexcept
{
    const double m = minutesPerSide;
    if (m <= 30.0) return 16.0;
    if (m <= 45.0) return 16.0 + (m - 30.0) * (11.0 - 16.0) / 15.0;
    if (m <= 60.0) return 11.0 + (m - 45.0) * (7.5 - 11.0) / 15.0;
    return 7.5;
}

/** A leader's length in seconds of tape travel, for one end of a cassette: between 3 and 6
    seconds, the same for a cassette wherever it is played (it comes from its id). end 0 is the
    start of side A, end 1 the end of it (the start of side B). */
inline double leaderSeconds (const char* cassetteId, int end) noexcept
{
    // FNV-1a over the id (its UTF-8 bytes) and the end: a fixed, spread-out number per cassette and end.
    uint32_t h = 2166136261u;
    for (auto* c = cassetteId; c != nullptr && *c != 0; ++c) { h ^= (uint8_t) *c; h *= 16777619u; }
    h ^= (uint32_t) (end + 1) * 0x9e3779b9u;
    h *= 16777619u;
    h ^= h >> 15;
    return 3.0 + 3.0 * (double) (h % 10000u) / 9999.0;
}

/** The full pack's diameter (mm): every millimetre of tape, leaders included, on one hub. */
inline double fullPackDiameterMm (int minutesPerSide, double leaderSecondsTotal) noexcept
{
    const double lengthMm = (minutesPerSide * 60.0 + leaderSecondsTotal) * speedMmPerSecond;
    const double area = lengthMm * thicknessUm (minutesPerSide) * 0.001;   // mm^2
    return 2.0 * std::sqrt (hubRadiusMm * hubRadiusMm + area / 3.14159265358979);
}

/** A pack's radius (mm) holding this fraction of the tape. The tape's area is the same whichever
    hub it's on, so the radius goes as the square root. */
inline double packRadiusMm (double fraction, double fullDiameterMm) noexcept
{
    const double R = fullDiameterMm * 0.5;
    return std::sqrt (hubRadiusMm * hubRadiusMm + (R * R - hubRadiusMm * hubRadiusMm) * std::clamp (fraction, 0.0, 1.0));
}

/** Winding: the take-up spindle is driven (motor, belt, slipping clutch) at about a constant
    angular speed, so the tape speeds up as the take-up pack grows. For a deck that winds a C-60
    side in windSecondsC60, the spindle's speed (rad/s): from t = 2 pi (R - r0) / (w * thickness). */
inline double windRadPerSecond (double windSecondsC60) noexcept
{
    const double R = fullPackDiameterMm (30, 9.0) * 0.5;
    return 2.0 * 3.14159265358979 * (R - hubRadiusMm) / (std::max (10.0, windSecondsC60) * thicknessUm (30) * 0.001);
}
} // namespace tf::Geometry
