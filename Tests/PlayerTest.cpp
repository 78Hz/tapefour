// Tests for the pocket player (Player/): it plays a packed cassette where it lies, both sides,
// remembers where each cassette was stopped, and never changes the file.

#include <cstdlib>
#include "../Player/PlayerEngine.h"
#include "../Player/Library.h"
#include "../Source/CassetteComponent.h"
#include "../Source/DeckLookAndFeel.h"
#include "Cassette.h"

#include <cstdio>

using namespace tf;
using namespace tf::pocket;

namespace
{
constexpr double sr = 48000.0;
constexpr int bs = 256;
int failures = 0;

void check (bool ok, const char* what, double got, double want)
{
    std::printf ("%s  %-60s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (! ok) ++failures;
    if (! ok && std::getenv ("GITHUB_ACTIONS") != nullptr)
        std::printf ("::error::PlayerTest: %s (got %.4f, want %.4f)\n", what, got, want);
}

class FakeDevice : public juce::AudioIODevice
{
public:
    FakeDevice() : juce::AudioIODevice ("Fake", "Fake") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override  { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { sr }; }
    juce::Array<int> getAvailableBufferSizes() override { return { bs }; }
    int getDefaultBufferSize() override { return bs; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bs; }
    double getCurrentSampleRate() override { return sr; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override  { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

struct Heard { std::vector<float> l, r; };

Heard run (PlayerEngine& p, double seconds)
{
    Heard h;
    std::vector<float> l ((size_t) bs), r ((size_t) bs);
    float* out[2] { l.data(), r.data() };
    for (int b = 0; b < (int) (seconds * sr / bs); ++b)
    {
        p.audioDeviceIOCallbackWithContext (nullptr, 0, out, 2, bs, {});
        h.l.insert (h.l.end(), l.begin(), l.end());
        h.r.insert (h.r.end(), r.begin(), r.end());
    }
    return h;
}

double rms (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    double s = 0;
    for (auto i = a; i < b; ++i) s += (double) v[i] * v[i];
    return b > a ? std::sqrt (s / (double) (b - a)) : 0.0;
}

// The frequency, from rising zero crossings.
double pitchOf (const std::vector<float>& v, double from, double to)
{
    auto a = (size_t) (from * sr), b = std::min (v.size(), (size_t) (to * sr));
    int crossings = 0;
    for (auto i = a + 1; i < b; ++i) if (v[i - 1] < 0.0f && v[i] >= 0.0f) ++crossings;
    return crossings / ((double) (b - a) / sr);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    DeckLookAndFeel lookAndFeel;   // the fonts a built-in look is drawn with
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tf_player_test");
    dir.deleteRecursively();
    dir.createDirectory();

    // A stereo cassette, as the master deck makes them: 440 Hz on side A's left, 880 Hz on side B's left.
    auto cass = dir.getChildFile ("demo.cass");
    {
        auto t = Tape::makeStereo (0, defaultTapeModel, tfmt::Condition());
        t->name = "demo";
        for (int64_t c = 0; c < cellRate * 6; ++c)
        {
            const auto ph = juce::MathConstants<double>::twoPi * (double) c / cellRate;
            t->writeCell (0, c, 0.3f * (float) std::sin (440.0 * ph), false, 0, 1.0f, 0);
            t->writeCell (2, c, 0.3f * (float) std::sin (880.0 * ph), false, 0, 1.0f, 0);
        }
        auto folder = dir.getChildFile ("demo");
        std::string err;
        check (t->save (folder) && tfmt::packCassette (tfmt::pathFromUtf8 (folder.getFullPathName().toStdString()),
                                                       tfmt::pathFromUtf8 (cass.getFullPathName().toStdString()), err),
               "a cassette to play", 1, 1);
    }
    const auto sizeBefore = cass.getSize();
    const auto timeBefore = cass.getLastModificationTime();

    juce::PropertiesFile::Options o;
    o.applicationName = "PlayerTest";
    juce::PropertiesFile props (dir.getChildFile ("player.settings"), o);
    Library library (props);

    auto opened = library.open (cass);
    check (opened.tape != nullptr && opened.tape->isReadOnly() && opened.tape->mappedTracks() == 4,
           "opens a .cass where it lies", opened.tape != nullptr ? opened.tape->mappedTracks() : -1, 4);
    check (opened.art.isValid() && opened.art.look.design.rfind ("tapefour.", 0) == 0,
           "a cassette without a look gets the built-in look of its kind", opened.art.isValid() ? 1 : 0, 1);
    check (! opened.art.isBlank(), "the built-in look is drawn (not empty images)", opened.art.isBlank() ? 1 : 0, 0);

    {
        // The noise reduction the cassette was recorded with is the one it's decoded with.
        auto t = Tape::makeStereo (0, defaultTapeModel, tfmt::Condition());
        const auto none = PlayerEngine::encodedWith (*t);
        for (int64_t c = 0; c < cellRate * 2; ++c) t->writeCell (0, c, 0.1f, true, 0, 1.0f, 0);
        for (int64_t c = 0; c < cellRate / 2; ++c) t->writeCell (1, c, 0.1f, true, 2, 1.0f, 0);
        const auto found = PlayerEngine::encodedWith (*t);
        check (none == -1 && found == 0, "plays a cassette with the noise reduction it was recorded with (2:1)", found, 0);
    }
    {
        // The TF-4 0.2 on Windows packed its looks with empty images: the player draws those again.
        auto t = Tape::makeStereo (0, defaultTapeModel, tfmt::Condition());
        t->name = "blank look";
        auto folder = dir.getChildFile ("blank");
        auto blankCass = dir.getChildFile ("blank.cass");
        std::string err;
        bool ok = t->save (folder) && CassetteComponent::writeArt (CassetteComponent::ArtSpec::of (*t), folder);
        juce::Image empty (juce::Image::ARGB, 64, 40, true, juce::SoftwareImageType());
        for (auto name : { "art/back.png", "art/front-a.png", "art/front-b.png" })
        {
            auto f = folder.getChildFile (name);
            f.deleteFile();
            juce::FileOutputStream out (f);
            ok = ok && juce::PNGImageFormat().writeImageToStream (empty, out);
        }
        ok = ok && tfmt::packCassette (tfmt::pathFromUtf8 (folder.getFullPathName().toStdString()),
                                       tfmt::pathFromUtf8 (blankCass.getFullPathName().toStdString()), err);
        tfmt::PackedCassette p;
        p.file = tfmt::pathFromUtf8 (blankCass.getFullPathName().toStdString());
        auto packed = CassetteArtwork::fromPacked (p);
        auto shown = Library::artworkOf (blankCass);
        check (ok && packed.isValid() && packed.isBlank() && shown.isValid() && ! shown.isBlank(),
               "a TF-4 look packed with empty images is drawn again", shown.isBlank() ? 1 : 0, 0);
    }

    PlayerEngine player;
    FakeDevice device;
    player.audioDeviceAboutToStart (&device);
    player.setVolume (10);
    player.setNoiseReduction (false);
    player.load (std::move (opened.tape), 0, 0.0);
    player.press (PlayerEngine::Key::play);
    auto a = run (player, 2.0);
    const auto pa = pitchOf (a.l, 0.5, 1.9);
    // A portable runs a little fast (pocket.deck: +0.4%), and wow and flutter move it about.
    check (std::abs (pa - 440.0 * 1.004) < 6.0 && rms (a.l, 0.5, 1.9) > 0.05 && rms (a.r, 0.5, 1.9) < 0.01,
           "side A: its left on the left, its right on the right", pa, 440.0 * 1.004);
    const auto posA = player.getPosition();
    check (std::abs (posA / cellRate - 2.0 * 1.004) < 0.1, "the tape runs at 1 7/8 ips", posA / cellRate, 2.008);

    player.turnOver();
    check (player.getSide() == 1 && std::abs (player.getPosition() - ((double) player.getTape()->getLengthCells() - posA)) < 1.0,
           "turned over: the same place on the tape, from side B", player.getPosition() / cellRate, 0);
    // side B from its start
    juce::String why;
    player.load (Tape::openPacked (cass, why), 1, 0.0);
    player.press (PlayerEngine::Key::play);
    auto b = run (player, 2.0);
    const auto pb = pitchOf (b.l, 0.5, 1.9);
    check (std::abs (pb - 880.0 * 1.004) < 10.0 && rms (b.r, 0.5, 1.9) < 0.01, "side B plays its own left and right, forwards", pb, 880.0 * 1.004);

    player.setVolume (0);
    auto quiet = run (player, 0.5);
    check (rms (quiet.l, 0.2, 0.5) < 1e-6, "volume 0 is silence", rms (quiet.l, 0.2, 0.5), 0);
    player.setVolume (5);
    auto mid = run (player, 0.5);
    player.setVolume (10);
    auto full = run (player, 0.5);
    const auto stepDb = juce::Decibels::gainToDecibels (rms (mid.l, 0.2, 0.5) / rms (full.l, 0.2, 0.5));
    check (std::abs (stepDb + 22.5) < 1.0, "the wheel: 5 steps down is 22.5 dB down", stepDb, -22.5);

    // Where it was left is remembered, per cassette.
    CassetteEntry e = opened.entry;
    e.side = player.getSide();
    e.position = player.getPosition();
    library.remember (e);
    auto again = library.open (cass);
    check (again.entry.side == 1 && std::abs (again.entry.position - e.position) < 1.0 && library.recent().size() == 1,
           "remembers each cassette's side and place", again.entry.position / cellRate, e.position / cellRate);

    player.press (PlayerEngine::Key::stop);
    run (player, 0.1);
    player.load (nullptr, 0, 0.0);
    check (cass.getSize() == sizeBefore && cass.getLastModificationTime() == timeBefore, "never changes the file", 1, 1);

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    dir.deleteRecursively();
    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
