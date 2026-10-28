#pragma once

#include <array>
#include <string>
#include <vector>

/*
    Tuning systems for the electric piano's reeds: what frequency each key's reed is tuned to.

      equal         12 equal semitones
      just          5-limit just intonation on the key (1/1 16/15 9/8 6/5 5/4 4/3 45/32 3/2 8/5 5/3 9/5 15/8)
      pythagorean   pure fifths from E-flat to G-sharp of the key (the wolf between them)
      meantone      quarter-comma: every fifth from E-flat to G-sharp narrowed by 1/4 syntonic comma
      werckmeister  Werckmeister III: C-G, G-D, D-A and B-F# narrowed by 1/4 Pythagorean comma, the rest pure
      kirnberger    Kirnberger III: C-G, G-D, D-A and A-E narrowed by 1/4 syntonic comma, F#-C# by a schisma
      vallotti      F-C, C-G, G-D, D-A, A-E and E-B narrowed by 1/6 Pythagorean comma, the rest pure
      custom        each of the 12 notes (C to B) set by hand, in cents from equal temperament
      edo           N equal divisions of the octave, one step per key, A4 on its key
      scala         a Scala scale (.scl), mapped by a keyboard map (.kbm) or the default one

    The historical systems are built from their circles of fifths, so the tables come from their
    definitions, not from rounded published figures. A4 sounds at the reference pitch in every system
    but Scala's own keyboard maps (which say which key sounds at what frequency). Octave stretch
    widens every octave by a few cents, as piano tuners do.

    Plain C++, no JUCE: the tables are tested on their own.
*/
namespace tf::tuning
{
enum class System { equal = 0, just, pythagorean, meantone, werckmeister, kirnberger, vallotti, custom, edo, scala, numSystems };

const char* systemName (System);

/** A Scala scale: the degrees in cents above 1/1, the last one being the period (usually 1200). */
struct Scale
{
    std::string description;
    std::vector<double> cents;
    bool valid() const { return ! cents.empty() && cents.back() > 0.0; }
};

/** A Scala keyboard map. The default (no .kbm): every key mapped, key 60 is degree 0, key 69 is A4 at the reference. */
struct KeyboardMap
{
    int size = 0;                   // 0: linear, every key one degree
    int firstNote = 0, lastNote = 127;
    int middleNote = 60;            // the key degree 0 sits on
    int referenceNote = 69;
    double referenceHz = 0.0;       // 0: the instrument's A4 reference
    int periodDegree = 0;           // the degree that counts as the period (0: the scale's own size)
    std::vector<int> mapping;       // per position in the map: the degree, or -1 unmapped
    bool valid = true;
};

/** Parses a .scl file's text. On error returns false and says why. */
bool parseScala (const std::string& text, Scale& out, std::string& error);
/** Parses a .kbm file's text. */
bool parseKeyboardMap (const std::string& text, KeyboardMap& out, std::string& error);

struct Settings
{
    System system = System::equal;
    int key = 0;                            // 0 = C .. 11 = B: the key the historical systems are built on
    double a4Hz = 440.0;
    double stretchCentsPerOctave = 0.0;
    std::array<double, 12> customCents {};  // C .. B, cents from equal temperament
    int edoSteps = 12;
    Scale scale;
    KeyboardMap keyboardMap;
};

/** One octave of a 12-note system, in cents above the key's note (12 values, the first 0). */
std::array<double, 12> octaveOf (System s);

/** Every key's frequency (MIDI note 0..127); 0 for a key a keyboard map leaves unmapped. */
std::array<double, 128> frequencies (const Settings& s);

/** Each pitch class's offset from equal temperament (cents, C..B), for display; zeros for EDO and Scala. */
std::array<double, 12> centsFromEqual (const Settings& s);
} // namespace tf::tuning
