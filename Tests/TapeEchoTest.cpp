// TF TAPE ECHO (Source/TapeEcho.h): a cassette loop delay.
//
//   A head's echo comes as long after the sound as the tape takes to get from the record head to it;
//   RATE (the motor) stretches every head's time; a head can sit anywhere on the loop, but behind the
//   erase head it plays wiped tape; with ERASE off what was recorded comes round again a turn later
//   (sound on sound); HOLD keeps the loop and records nothing new; the splice ticks once a turn;
//   repeats lose treble; INTENSITY past 100% runs away but the tape's saturation holds it; the tempo
//   readout and its -/+; heads too close to build; the state comes back, and an old echo's too.

#include "../Source/TapeEcho.h"

#include <cstdio>
#include <cstdlib>

using namespace tf;
using namespace tf::echo;

namespace
{
int failures = 0;
const char* annotate = std::getenv ("GITHUB_ACTIONS");
constexpr double sr = 48000.0;

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-78s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok)
    {
        ++failures;
        if (annotate != nullptr) std::printf ("::error::TapeEchoTest: %s (got %.4f, want %.4f)\n", what, got, want);
    }
}

/** A quiet, steady machine: no wow, a new tape, no splice. */
void quiet (Processor& p)
{
    p.set ("wow", 0.0f);
    p.set ("age", 0.0f);
    p.set ("splice", 0.0f);
    p.set ("intensity", 0.0f);
    p.set ("treble", 0.0f);
    p.set ("bass", 0.0f);
    for (int h = 0; h < numHeads; ++h) p.setHeadOn (h, false);
    p.prepareToPlay (sr, 512);
}

/** Runs it for this long; in(i) is the input at sample i. Returns the left output. */
std::vector<float> run (Processor& p, double seconds, std::function<float (int)> in = {})
{
    const int total = (int) (seconds * sr);
    std::vector<float> out;
    out.reserve ((size_t) total);
    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer m;
    for (int at = 0; at < total; at += 512)
    {
        const int n = std::min (512, total - at);
        b.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            const float x = in ? in (at + i) : 0.0f;
            b.setSample (0, i, x);
            b.setSample (1, i, x);
        }
        p.processBlock (b, m);
        for (int i = 0; i < n; ++i) out.push_back (b.getSample (0, i));
    }
    return out;
}

/** A short burst of a 1 kHz tone at the start. */
float burst (int i) { return i < 480 ? 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr) : 0.0f; }

/** Where the output first goes over a level (seconds), from a time on; -1 if never. */
double firstAbove (const std::vector<float>& v, float level, double from = 0.0)
{
    for (size_t i = (size_t) (from * sr); i < v.size(); ++i) if (std::abs (v[i]) > level) return (double) i / sr;
    return -1.0;
}

float peakIn (const std::vector<float>& v, double from, double to)
{
    float p = 0.0f;
    for (size_t i = (size_t) (from * sr); i < std::min (v.size(), (size_t) (to * sr)); ++i) p = std::max (p, std::abs (v[i]));
    return p;
}

/** The share of a stretch's energy above 4 kHz (a first difference stands in for a high-pass). */
double trebleShare (const std::vector<float>& v, double from, double to)
{
    double all = 0, high = 0;
    for (size_t i = (size_t) (from * sr) + 1; i < std::min (v.size(), (size_t) (to * sr)); ++i)
    {
        all += (double) v[i] * v[i];
        const double d = v[i] - v[i - 1];
        high += d * d;
    }
    return all > 0 ? high / all : 0.0;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---- one head: the echo comes when the tape reaches it ----
    {
        Processor p;
        quiet (p);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 0.25);
        auto out = run (p, 1.0, burst);
        const double at = firstAbove (out, 0.02f);
        check (std::abs (at - 0.25) < 0.003, "head 1 at 0.25 s of tape: the echo comes 0.25 s after", at, 0.25);
        check (peakIn (out, 0.0, 0.2) < 0.005f, "nothing before it (the echo is the wet sound only)", peakIn (out, 0.0, 0.2), 0.0);
    }
    // ---- RATE: half speed, twice as long ----
    {
        Processor p;
        quiet (p);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 0.25);
        p.set ("rate", 0.5f);
        p.prepareToPlay (sr, 512);
        auto out = run (p, 1.5, burst);
        const double at = firstAbove (out, 0.02f);
        check (std::abs (at - 0.5) < 0.006, "RATE 50%: the same head's echo comes after 0.5 s", at, 0.5);
        check (std::abs (p.toSeconds (p.headPos (0)) - 0.5) < 1e-6, "and the window says so", p.toSeconds (p.headPos (0)), 0.5);
    }
    // ---- feedback: repeats, each darker ----
    {
        Processor p;
        quiet (p);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 0.2);
        p.set ("intensity", 0.8f);
        auto noise = [] (int i) { static juce::Random r (5); return i < 2400 ? 0.15f * (r.nextFloat() * 2.0f - 1.0f) : 0.0f; };
        auto out = run (p, 1.3, noise);
        const float first = peakIn (out, 0.19, 0.27), third = peakIn (out, 0.59, 0.67);
        check (first > 0.02f && third > 0.005f && third < first, "INTENSITY: the echo comes round again, quieter", third / first, 0.5);
        const double t1 = trebleShare (out, 0.2, 0.25), t3 = trebleShare (out, 0.6, 0.65);
        check (t3 < t1 * 0.8, "each time round it has lost treble (the tape at 1 7/8 ips)", t3 / t1, 0.6);
    }
    // ---- a head anywhere on the loop ----
    {
        Processor p;
        quiet (p);
        p.set ("loop", 0.0f);   // 10 s
        p.setHeadOn (2, true);
        p.setHeadPos (2, 7.5);
        auto out = run (p, 8.0, burst);
        const double at = firstAbove (out, 0.02f);
        check (std::abs (at - 7.5) < 0.01, "a head 7.5 s along a 10 s loop: its echo comes 7.5 s after", at, 7.5);
    }
    // ---- behind the erase head: wiped tape ----
    for (const bool erase : { true, false })
    {
        Processor p;
        quiet (p);
        p.set ("loop", 0.0f);
        p.set ("erase", erase ? 1.0f : 0.0f);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 9.9);   // between the erase head (9.75 s) and the record head
        auto out = run (p, 10.2, burst);
        const float got = peakIn (out, 9.8, 10.0);
        if (erase) check (got < 0.005f, "a head behind the erase head plays wiped tape (nothing)", got, 0.0);
        else       check (got > 0.02f, "with ERASE off, the same head plays what's on the tape", got, 0.1);
    }
    // ---- ERASE off: a turn later it's still there (sound on sound); HOLD keeps the loop ----
    {
        Processor p;
        quiet (p);
        p.set ("loop", 0.0f);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 1.0);
        p.set ("erase", 0.0f);
        auto out = run (p, 11.4, burst);
        check (peakIn (out, 10.9, 11.2) > 0.01f, "ERASE off: the sound comes round again a turn of the loop later", peakIn (out, 10.9, 11.2), 0.1);

        Processor q;
        quiet (q);
        q.set ("loop", 0.0f);
        q.setHeadOn (0, true);
        q.setHeadPos (0, 1.0);
        auto first = run (q, 1.5, burst);
        q.set ("hold", 1.0f);
        auto later = run (q, 10.0, [] (int i) { return i < 4800 ? 0.3f : 0.0f; });   // new sound while held: not recorded
        check (peakIn (later, 9.4, 9.7) > 0.02f, "HOLD: the loop keeps what's on it, turn after turn", peakIn (later, 9.4, 9.7), 0.1);
        check (peakIn (later, 0.9, 1.3) < 0.01f, "HOLD: nothing new is recorded", peakIn (later, 0.9, 1.3), 0.0);
    }
    // ---- the splice: a tick once a turn ----
    {
        Processor p;
        quiet (p);
        p.set ("loop", 0.0f);
        p.set ("splice", 1.0f);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 0.3);
        auto out = run (p, 25.0);
        int ticks = 0;
        for (double t = 0.0; t < 25.0; t += 0.05)
            if (peakIn (out, t, t + 0.05) > 0.01f) { ++ticks; t += 1.0; }
        check (ticks == 2 || ticks == 3, "the splice ticks once a turn of the loop (10 s): 2 or 3 in 25 s", ticks, 2);
    }
    // ---- runaway: past 100% it grows, but the tape holds it ----
    {
        Processor p;
        quiet (p);
        p.setHeadOn (0, true);
        p.setHeadPos (0, 0.1);
        p.set ("intensity", 1.2f);
        auto out = run (p, 6.0, burst);
        bool finite = true;
        for (auto v : out) finite = finite && std::isfinite (v);
        const float late = peakIn (out, 5.0, 6.0);
        check (finite && late > 0.05f && late < 1.0f, "INTENSITY 120%: it runs away, and the tape's saturation holds it", late, 0.5);
    }
    // ---- the tempo, -/+, PLACE, heads too close ----
    {
        Processor p;
        p.setHeadPos (0, 0.21);
        p.setNoteValue (2);   // 1/8
        check (std::abs (p.bpm() - 142.857) < 0.01, "BPM: head 1 at 210 ms as an eighth is 142.86", p.bpm(), 142.857);
        p.setBpm (120.0);
        check (std::abs (p.bpm() - 120.0) < 0.01 && std::abs (p.rate() - 0.84) < 0.001, "setting 120 BPM moves the motor (84%)", p.rate(), 0.84);
        const double before = p.bpm();
        p.setBpm (before + 0.01);
        check (std::abs (p.bpm() - before - 0.01) < 0.002, "+ moves it 0.01 BPM", p.bpm() - before, 0.01);
        p.place (0);
        check (std::abs (p.headPos (1) - 0.42) < 1e-3 && std::abs (p.headPos (2) - 0.63) < 1e-3, "PLACE STRAIGHT: heads at 1:2:3", p.headPos (2), 0.63);
        p.setHeadPos (1, 0.26);
        check (p.tooClose (1) && p.tooClose (0) && ! p.tooClose (2), "heads 5 mm apart or less are too close to build", p.tooClose (1), 1);
    }
    // ---- state, and an echo from before ----
    {
        Processor p;
        p.setHeadPos (2, 12.5);
        p.set ("loop", 2.0f);
        p.set ("erase", 0.0f);
        p.set ("tape", 2.0f);
        p.set ("rate", 0.7f);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        Processor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool same = std::abs (q.headPos (2) - 12.5) < 1e-3 && juce::exactlyEqual (q.loopNominal(), 30.0) && q.get ("erase") < 0.5f
                          && juce::roundToInt (q.get ("tape")) == 2 && std::abs (q.rate() - 0.7) < 1e-4;
        check (same, "state: the heads, the loop, the tape and the motor come back", same, 1);

        juce::XmlElement old ("FX");
        old.setAttribute ("time", 500.0);
        old.setAttribute ("feedback", 0.6);
        old.setAttribute ("tone", 3200.0);
        old.setAttribute ("wow", 0.2);
        juce::MemoryBlock ob;
        juce::AudioProcessor::copyXmlToBinary (old, ob);
        Processor r;
        r.setStateInformation (ob.getData(), (int) ob.getSize());
        const bool migrated = std::abs (r.headPos (0) - 0.5) < 1e-3 && r.headOn (0) && ! r.headOn (1) && ! r.headOn (2) && std::abs (r.get ("intensity") - 0.6f) < 1e-3;
        check (migrated, "an echo saved before the cassette loop: one head at its TIME, its FEEDBACK", migrated, 1);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
