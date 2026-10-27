// The noise reduction models (Source/NoiseReduction.h), measured the way the published figures
// were: a steady tone into the encoder, its level out against its level in.
//
//   B: Dolby Laboratories' average encoder response ("Dolby encoder output for constant level
//      input", Signetics NE645/646 data sheet), 200 Hz to 20 kHz, 0 to -40 dB re Dolby level.
//   C: the test points and limits of the Philips TEA0665 data sheet (record mode).
//   2:1: the compander's law (2:1 on record, 1:2 on playback) and its emphasis.
// And each system's decoder must give back what its encoder was given.

#include "../Source/NoiseReduction.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tf::nr;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-62s got %9.3f  want %9.3f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::NoiseReductionTest: %s (got %.3f, want %.3f)\n", what, got, want);
    }
}

constexpr double sr = 48000.0, pi = 3.14159265358979;

// Steady-state gain (dB) of an encoder for a tone at f, level dB re Dolby level.
double encodeGain (int type, double f, double levelDb)
{
    NoiseReduction e;
    e.configure (sr, type, true);
    const double A = NoiseReduction::dolbyPeak * std::pow (10.0, levelDb / 20.0);
    const int N = (int) (sr * 1.0), M = (int) (sr * 0.25);
    double re = 0, im = 0;
    for (int n = 0; n < N; ++n)
    {
        const double ph = 2.0 * pi * f * n / sr;
        const double y = e.process ((float) (A * std::sin (ph)));
        if (n >= N - M) { re += y * std::cos (ph); im += y * std::sin (ph); }
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / M / A);
}

// The decoder after the encoder: how far the output strays from the input (dB re the input's RMS).
double roundTripErrorDb (int type)
{
    NoiseReduction e, d;
    e.configure (sr, type, true);
    d.configure (sr, type, false);
    double sig = 0, err = 0;
    uint32_t rng = 1;
    const int N = (int) (sr * 6.0);
    std::vector<float> in ((size_t) N), out ((size_t) N);
    for (int n = 0; n < N; ++n)
    {
        // Notes of different pitches and levels with silences between: what makes a compander work.
        const int note = n / (int) (sr * 0.25);
        const double f = 150.0 * std::pow (2.0, (note * 7) % 36 / 6.0);
        const double level = std::pow (10.0, (-((note * 13) % 50)) / 20.0);
        const double env = (n % (int) (sr * 0.25)) < (int) (sr * 0.18) ? 1.0 : 0.0;
        rng = rng * 1664525u + 1013904223u;
        const double noise = ((double) (rng >> 8) / 16777216.0 - 0.5) * 0.02;
        in[(size_t) n] = (float) (NoiseReduction::dolbyPeak * level * env * (std::sin (2.0 * pi * f * n / sr) + noise));
        out[(size_t) n] = d.process (e.process (in[(size_t) n]));
    }
    for (int n = (int) sr; n < N; ++n)
    {
        sig += (double) in[(size_t) n] * in[(size_t) n];
        const double e2 = (double) out[(size_t) n] - in[(size_t) n];
        err += e2 * e2;
    }
    return 10.0 * std::log10 (err / sig + 1.0e-30);
}
}

int main()
{
    // ---- B ----
    {
        const double levels[] { 0, -5, -10, -15, -20, -25, -30, -35, -40 };
        struct Row { double kHz; double dB[9]; };
        const Row table[] {
            { 0.2, { 0, .3, .4, .5, .5, .6, .6, .5, .5 } },     { 0.3, { 0, .3, .6, 1.1, 1.3, 1.3, 1.3, 1.3, 1.3 } },
            { 0.5, { 0, .3, .8, 1.8, 2.6, 2.9, 2.9, 3.0, 2.9 } }, { 0.7, { 0, .4, .9, 2.1, 3.5, 4.3, 4.4, 4.5, 4.4 } },
            { 1.0, { 0, .4, 1.0, 2.3, 4.2, 5.7, 6.1, 6.3, 6.2 } }, { 1.4, { 0, .3, .9, 2.3, 4.4, 6.6, 7.5, 7.7, 7.7 } },
            { 2.0, { .1, .4, .9, 2.2, 4.3, 7.0, 8.5, 8.9, 8.9 } }, { 3.0, { .2, .6, .9, 1.9, 3.9, 6.6, 8.8, 9.7, 9.7 } },
            { 5.0, { .3, .6, 1.0, 1.7, 3.2, 5.4, 8.2, 10.0, 10.3 } }, { 7.0, { .3, .6, 1.0, 1.7, 2.8, 4.7, 7.3, 9.7, 10.4 } },
            { 10.0, { .4, .7, 1.1, 1.7, 2.6, 4.2, 6.5, 9.1, 10.4 } }, { 14.0, { .5, .8, 1.1, 1.8, 2.7, 4.4, 6.5, 8.7, 10.3 } },
        };
        double sum = 0, worst = 0;
        int count = 0;
        for (auto& r : table)
            for (int i = 0; i < 9; ++i)
            {
                const auto e = encodeGain (1, r.kHz * 1000.0, levels[i]) - r.dB[i];
                sum += e * e;
                worst = std::max (worst, std::abs (e));
                ++count;
            }
        const auto rms = std::sqrt (sum / count);
        check (rms < 0.6, "B: Dolby's encoder response, 200 Hz-14 kHz, 0 to -40 dB (rms error)", rms, 0.0);
        check (worst < 1.3, "B: ... and nowhere more than 1.3 dB out", worst, 0.0);
        const auto low = encodeGain (1, 5000.0, -60.0);
        check (std::abs (low - 10.3) < 0.6, "B: low-level treble lift, 5 kHz at -60 dB", low, 10.3);
    }

    // ---- C: the TEA0665's record-mode limits (dB of output re the test point, for the input level shown) ----
    {
        struct Point { double kHz, levelDb, lo, hi; const char* what; };
        const Point points[] {
            { 0.2, -40, -33.4 + 40, -30.4 + 40, "C: 200 Hz at -40 dB" },
            { 1.0, -30, -20.1 + 30, -17.1 + 30, "C: 1 kHz at -30 dB" },
            { 1.0, -20, -16.1 + 20, -12.1 + 20, "C: 1 kHz at -20 dB" },
            { 5.0,   0,  -3.8,       -0.8,      "C: 5 kHz at Dolby level (anti-saturation)" },
            { 5.0, -20, -19.1 + 20, -15.1 + 20, "C: 5 kHz at -20 dB" },
            { 5.0, -40, -28.5 + 40, -24.5 + 40, "C: 5 kHz at -40 dB" },
        };
        for (auto& p : points)
        {
            const auto g = encodeGain (2, p.kHz * 1000.0, p.levelDb);
            check (g >= p.lo - 0.1 && g <= p.hi + 0.1, p.what, g, 0.5 * (p.lo + p.hi));
        }
        const auto low = encodeGain (2, 4000.0, -70.0);
        check (std::abs (low - 20.0) < 1.5, "C: low-level lift, 4 kHz at -70 dB (two stages of 10)", low, 20.0);
    }

    // ---- 2:1 ----
    {
        // Below the emphasis, a 2:1 law about the reference: 20 dB down in, 10 dB down out.
        const auto a = encodeGain (0, 200.0, -20.0);
        const auto b = encodeGain (0, 200.0, -40.0);
        // gain = output re input: input 20 dB lower, output 10 dB lower, so 10 dB more gain
        check (std::abs ((b - a) - 10.0) < 1.0, "2:1: 20 dB less in gives 10 dB less out (200 Hz)", b - a, 10.0);
    }

    // ---- each decoder undoes its encoder ----
    for (int type = 0; type < 3; ++type)
    {
        const auto e = roundTripErrorDb (type);
        const char* names[] { "2:1: decode(encode(x)) is x, within -40 dB", "B: decode(encode(x)) is x, within -40 dB",
                              "C: decode(encode(x)) is x, within -40 dB" };
        check (e < -40.0, names[type], e, -40.0);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
