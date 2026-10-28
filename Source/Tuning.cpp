#include "Tuning.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tf::tuning
{
namespace
{
    const double pureFifth = 1200.0 * std::log2 (1.5);                       // 701.955
    const double pythagoreanComma = 1200.0 * std::log2 (531441.0 / 524288.0);  // 23.460
    const double syntonicComma = 1200.0 * std::log2 (81.0 / 80.0);           // 21.506
    const double schisma = pythagoreanComma - syntonicComma;                  // 1.954

    double wrap (double c) { c = std::fmod (c, 1200.0); return c < 0.0 ? c + 1200.0 : c; }

    // An octave from a chain of fifths. up[i]: the fifth from C's i-th fifth upward to the next
    // (C-G, G-D, D-A, A-E, E-B, B-F#, F#-C#, C#-G#); down[i]: the fifth below (F-C, Bb-F, Eb-Bb).
    std::array<double, 12> fromFifths (const std::array<double, 8>& up, const std::array<double, 3>& down)
    {
        std::array<double, 12> o {};
        double c = 0.0;
        int pc = 0;
        for (auto f : up) { c += f; pc = (pc + 7) % 12; o[(size_t) pc] = wrap (c); }
        c = 0.0; pc = 0;
        for (auto f : down) { c -= f; pc = (pc + 5) % 12; o[(size_t) pc] = wrap (c); }
        o[0] = 0.0;
        return o;
    }

    std::string trim (const std::string& s)
    {
        auto a = s.find_first_not_of (" \t\r\n");
        if (a == std::string::npos) return {};
        auto b = s.find_last_not_of (" \t\r\n");
        return s.substr (a, b - a + 1);
    }

    // The lines of a Scala file that aren't comments.
    std::vector<std::string> contentLines (const std::string& text, bool keepFirstEmpty)
    {
        std::vector<std::string> out;
        std::istringstream in (text);
        std::string line;
        while (std::getline (in, line))
        {
            if (! line.empty() && line.back() == '\r') line.pop_back();
            if (! line.empty() && line[0] == '!') continue;
            if (! keepFirstEmpty && trim (line).empty()) continue;
            out.push_back (line);
            keepFirstEmpty = false;   // only the description may be empty
        }
        return out;
    }

    double centsOfDegree (const Scale& s, long degree)
    {
        const long n = (long) s.cents.size();
        long o = degree / n, r = degree % n;
        if (r < 0) { r += n; --o; }
        return (double) o * s.cents.back() + (r == 0 ? 0.0 : s.cents[(size_t) r - 1]);
    }
}

const char* systemName (System s)
{
    switch (s)
    {
        case System::equal:        return "EQUAL";
        case System::just:         return "JUST";
        case System::pythagorean:  return "PYTHAGOREAN";
        case System::meantone:     return "MEANTONE";
        case System::werckmeister: return "WERCKMEISTER III";
        case System::kirnberger:   return "KIRNBERGER III";
        case System::vallotti:     return "VALLOTTI";
        case System::custom:       return "PER NOTE";
        case System::edo:          return "EDO";
        case System::scala:        return "SCALA FILE";
        case System::numSystems:   break;
    }
    return "EQUAL";
}

std::array<double, 12> octaveOf (System s)
{
    const double p = pureFifth, q = pureFifth - syntonicComma / 4.0;
    switch (s)
    {
        case System::just:
        {
            const double r[12] { 1.0, 16.0 / 15, 9.0 / 8, 6.0 / 5, 5.0 / 4, 4.0 / 3, 45.0 / 32, 3.0 / 2, 8.0 / 5, 5.0 / 3, 9.0 / 5, 15.0 / 8 };
            std::array<double, 12> o {};
            for (int i = 0; i < 12; ++i) o[(size_t) i] = 1200.0 * std::log2 (r[i]);
            return o;
        }
        case System::pythagorean:  return fromFifths ({ p, p, p, p, p, p, p, p }, { p, p, p });
        case System::meantone:     return fromFifths ({ q, q, q, q, q, q, q, q }, { q, q, q });
        case System::werckmeister:
        {
            const double t = pureFifth - pythagoreanComma / 4.0;
            return fromFifths ({ t, t, t, p, p, t, p, p }, { p, p, p });
        }
        case System::kirnberger:
            return fromFifths ({ q, q, q, q, p, p, pureFifth - schisma, p }, { p, p, p });
        case System::vallotti:
        {
            const double t = pureFifth - pythagoreanComma / 6.0;
            return fromFifths ({ t, t, t, t, t, p, p, p }, { t, p, p });
        }
        case System::equal: case System::custom: case System::edo: case System::scala: case System::numSystems: break;
    }
    std::array<double, 12> o {};
    for (int i = 0; i < 12; ++i) o[(size_t) i] = 100.0 * i;
    return o;
}

bool parseScala (const std::string& text, Scale& out, std::string& error)
{
    auto lines = contentLines (text, true);
    if (lines.size() < 2) { error = "NOT A SCALA FILE"; return false; }
    Scale s;
    s.description = trim (lines[0]);
    int count = 0;
    {
        std::istringstream n (lines[1]);
        if (! (n >> count) || count < 1 || count > 1024) { error = "BAD NOTE COUNT"; return false; }
    }
    if ((int) lines.size() < 2 + count) { error = "FEWER NOTES THAN IT SAYS"; return false; }
    for (int i = 0; i < count; ++i)
    {
        std::istringstream w (lines[(size_t) (2 + i)]);
        std::string tok;
        w >> tok;
        double c = 0.0;
        try
        {
            if (tok.find ('.') != std::string::npos)
                c = std::stod (tok);
            else
            {
                auto slash = tok.find ('/');
                const double num = std::stod (tok.substr (0, slash));
                const double den = slash == std::string::npos ? 1.0 : std::stod (tok.substr (slash + 1));
                if (num <= 0.0 || den <= 0.0) { error = "BAD RATIO: " + tok; return false; }
                c = 1200.0 * std::log2 (num / den);
            }
        }
        catch (...) { error = "BAD PITCH: " + tok; return false; }
        s.cents.push_back (c);
    }
    if (s.cents.back() <= 0.0) { error = "THE PERIOD MUST BE ABOVE 1/1"; return false; }
    out = s;
    return true;
}

bool parseKeyboardMap (const std::string& text, KeyboardMap& out, std::string& error)
{
    auto lines = contentLines (text, false);
    if (lines.size() < 7) { error = "NOT A KEYBOARD MAP"; return false; }
    KeyboardMap m;
    try
    {
        m.size = std::stoi (trim (lines[0]));
        m.firstNote = std::stoi (trim (lines[1]));
        m.lastNote = std::stoi (trim (lines[2]));
        m.middleNote = std::stoi (trim (lines[3]));
        m.referenceNote = std::stoi (trim (lines[4]));
        m.referenceHz = std::stod (trim (lines[5]));
        m.periodDegree = std::stoi (trim (lines[6]));
        for (int i = 0; i < m.size; ++i)
        {
            const auto e = 7 + (size_t) i < lines.size() ? trim (lines[7 + (size_t) i]) : std::string ("x");
            m.mapping.push_back (e.empty() || e[0] == 'x' || e[0] == 'X' ? -1 : std::stoi (e));
        }
    }
    catch (...) { error = "BAD KEYBOARD MAP"; return false; }
    if (m.size < 0 || m.size > 1024 || m.referenceHz < 0.0) { error = "BAD KEYBOARD MAP"; return false; }
    out = m;
    return true;
}

std::array<double, 128> frequencies (const Settings& s)
{
    std::array<double, 128> f {};
    const double a4 = std::clamp (s.a4Hz, 100.0, 1000.0);

    if (s.system == System::scala && s.scale.valid())
    {
        const auto& m = s.keyboardMap;
        const long n = (long) s.scale.cents.size();
        const long period = m.periodDegree > 0 ? m.periodDegree : n;
        const double periodCents = centsOfDegree (s.scale, period);
        auto cents = [&] (int key, bool& mapped) -> double
        {
            mapped = true;
            if (m.size == 0) return centsOfDegree (s.scale, key - m.middleNote);
            if (key < m.firstNote || key > m.lastNote) { mapped = false; return 0.0; }
            long idx = key - m.middleNote, o = idx / m.size, r = idx % m.size;
            if (r < 0) { r += m.size; --o; }
            const int deg = m.mapping[(size_t) r];
            if (deg < 0) { mapped = false; return 0.0; }
            return (double) o * periodCents + centsOfDegree (s.scale, deg);
        };
        bool refMapped = true;
        const double refCents = cents (m.referenceNote, refMapped);
        const double refHz = m.referenceHz > 0.0 ? m.referenceHz : a4;
        for (int k = 0; k < 128; ++k)
        {
            bool mapped = true;
            const double c = cents (k, mapped);
            f[(size_t) k] = mapped ? refHz * std::pow (2.0, (c - refCents) / 1200.0) : 0.0;
        }
    }
    else if (s.system == System::edo)
    {
        const int n = std::clamp (s.edoSteps, 1, 999);
        for (int k = 0; k < 128; ++k) f[(size_t) k] = a4 * std::pow (2.0, (k - 69) / (double) n);
    }
    else
    {
        const auto o = s.system == System::custom ? octaveOf (System::equal) : octaveOf (s.system);
        const int key = s.system == System::custom ? 0 : ((s.key % 12) + 12) % 12;
        auto cents = [&] (int k)
        {
            const int rel = k - key, pc = ((rel % 12) + 12) % 12;
            const double extra = s.system == System::custom ? s.customCents[(size_t) (((k % 12) + 12) % 12)] : 0.0;
            return 1200.0 * std::floor (rel / 12.0) + o[(size_t) pc] + extra;
        };
        const double ref = cents (69);
        for (int k = 0; k < 128; ++k) f[(size_t) k] = a4 * std::pow (2.0, (cents (k) - ref) / 1200.0);
    }

    if (std::abs (s.stretchCentsPerOctave) > 1.0e-9)
        for (auto& v : f)
            if (v > 0.0) v *= std::pow (2.0, s.stretchCentsPerOctave * std::log2 (v / a4) / 1200.0);
    return f;
}

std::array<double, 12> centsFromEqual (const Settings& s)
{
    std::array<double, 12> d {};
    if (s.system == System::edo || s.system == System::scala) return d;
    if (s.system == System::custom) return s.customCents;
    const auto o = octaveOf (s.system);
    const int key = ((s.key % 12) + 12) % 12;
    // relative to equal temperament with the same A: A's own offset taken out
    const int aRel = ((9 - key) % 12 + 12) % 12;
    const double aOff = o[(size_t) aRel] - 100.0 * aRel;
    for (int pc = 0; pc < 12; ++pc)
    {
        const int rel = ((pc - key) % 12 + 12) % 12;
        d[(size_t) pc] = o[(size_t) rel] - 100.0 * rel - aOff;
    }
    return d;
}
} // namespace tf::tuning
