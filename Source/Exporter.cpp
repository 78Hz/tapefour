#include "Exporter.h"

#include <lame.h>

namespace tf
{
//==============================================================================
juce::String ExportFormats::describe() const
{
    if (all()) return "ALL FORMATS";
    juce::StringArray f;
    if (wav)  f.add ("WAV");
    if (flac) f.add ("FLAC");
    if (mp3)  f.add ("MP3");
    return f.isEmpty() ? juce::String ("NO FILES") : f.joinIntoString (" + ");
}

std::unique_ptr<juce::XmlElement> ExportFormats::toXml() const
{
    auto x = std::make_unique<juce::XmlElement> ("EXPORT_FORMATS");
    x->setAttribute ("wav", wav);
    x->setAttribute ("flac", flac);
    x->setAttribute ("mp3", mp3);
    x->setAttribute ("wavBits", wavBits);
    x->setAttribute ("flacBits", flacBits);
    x->setAttribute ("mp3Kbps", mp3Kbps);
    return x;
}

ExportFormats ExportFormats::fromXml (const juce::XmlElement* x)
{
    ExportFormats f;
    if (x == nullptr) return f;
    f.wav  = x->getBoolAttribute ("wav", true);
    f.flac = x->getBoolAttribute ("flac", false);
    f.mp3  = x->getBoolAttribute ("mp3", false);
    if (! f.any()) f.wav = true;
    auto pick = [] (int v, std::initializer_list<int> allowed, int fallback)
    {
        for (auto a : allowed) if (a == v) return v;
        return fallback;
    };
    f.wavBits  = pick (x->getIntAttribute ("wavBits", 24), { 16, 24, 32 }, 24);
    f.flacBits = pick (x->getIntAttribute ("flacBits", 24), { 16, 24 }, 24);
    f.mp3Kbps  = pick (x->getIntAttribute ("mp3Kbps", 320), { 192, 256, 320 }, 320);
    return f;
}

bool ExportFormats::operator== (const ExportFormats& o) const
{
    return wav == o.wav && flac == o.flac && mp3 == o.mp3 && wavBits == o.wavBits && flacBits == o.flacBits && mp3Kbps == o.mp3Kbps;
}

//==============================================================================
/** An MP3 file, encoded by LAME as it is written. */
class AudioFileSet::Mp3Writer
{
public:
    Mp3Writer (const juce::File& file, double sampleRate, int numChannels, int kbps)
        : out (file), channels (numChannels)
    {
        out.setPosition (0);
        out.truncate();
        if (! out.openedOk()) { problem = "could not create " + file.getFileName(); return; }
        lame = lame_init();
        if (lame == nullptr) { problem = "the MP3 encoder didn't start"; return; }
        lame_set_in_samplerate (lame, juce::roundToInt (sampleRate));
        // MP3 tops out at 48 kHz: faster interfaces are resampled by the encoder.
        if (sampleRate > 48000.0) lame_set_out_samplerate (lame, juce::approximatelyEqual (std::fmod (sampleRate, 44100.0), 0.0) ? 44100 : 48000);
        lame_set_num_channels (lame, numChannels);
        lame_set_mode (lame, numChannels == 1 ? MONO : JOINT_STEREO);
        lame_set_VBR (lame, vbr_off);
        lame_set_brate (lame, kbps);
        lame_set_quality (lame, 2);   // near the best LAME does; still many times faster than real time
        lame_set_bWriteVbrTag (lame, 1);   // an Info frame, filled in at the end, so players know the length
        if (lame_init_params (lame) < 0) problem = "the MP3 encoder refused these settings";
    }

    ~Mp3Writer()
    {
        if (lame != nullptr) lame_close (lame);
    }

    bool ok() const noexcept { return problem.isEmpty(); }
    const juce::String& error() const noexcept { return problem; }

    bool write (const float* const* data, int n)
    {
        if (! ok()) return false;
        buffer.ensureSize ((size_t) (n * 5 / 4 + 7200));
        auto got = lame_encode_buffer_ieee_float (lame, data[0], channels > 1 ? data[1] : data[0], n,
                                                  static_cast<unsigned char*> (buffer.getData()), (int) buffer.getSize());
        if (got < 0) { problem = "MP3 encoding failed"; return false; }
        return got == 0 || out.write (buffer.getData(), (size_t) got);
    }

    bool finish()
    {
        if (! ok()) return false;
        buffer.ensureSize (7200);
        auto got = lame_encode_flush (lame, static_cast<unsigned char*> (buffer.getData()), (int) buffer.getSize());
        if (got > 0) out.write (buffer.getData(), (size_t) got);
        // The Info frame at the start, now that the length is known.
        unsigned char tag[2880];
        auto tagSize = lame_get_lametag_frame (lame, tag, sizeof (tag));
        if (tagSize > 0 && tagSize <= sizeof (tag))
        {
            out.flush();
            out.setPosition (0);
            out.write (tag, tagSize);
        }
        out.flush();
        return out.getStatus().wasOk();
    }

private:
    juce::FileOutputStream out;
    lame_global_flags* lame = nullptr;
    juce::MemoryBlock buffer;
    juce::String problem;
    int channels = 1;
};

//==============================================================================
AudioFileSet::AudioFileSet (const juce::File& base, const ExportFormats& f, double sampleRate, int numChannels)
    : channels (numChannels)
{
    auto open = [&] (juce::AudioFormat& format, const juce::String& ext, int bits, bool floating) -> std::unique_ptr<juce::AudioFormatWriter>
    {
        auto file = base.getSiblingFile (base.getFileName() + ext);
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr) { error = "could not create " + file.getFileName(); return {}; }
        auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (numChannels).withBitsPerSample (bits)
                           .withSampleFormat (floating ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                       : juce::AudioFormatWriterOptions::SampleFormat::integral);
        auto w = format.createWriterFor (stream, options);
        if (w == nullptr) { error = "could not write " + file.getFileName(); return {}; }
        files.add (file);
        return w;
    };

    if (f.wav)
    {
        juce::WavAudioFormat format;
        wav = open (format, ".wav", f.wavBits, f.wavBits == 32);
    }
    if (f.flac)
    {
        juce::FlacAudioFormat format;
        flac = open (format, ".flac", f.flacBits, false);
    }
    if (f.mp3)
    {
        auto file = base.getSiblingFile (base.getFileName() + ".mp3");
        file.deleteFile();
        mp3 = std::make_unique<Mp3Writer> (file, sampleRate, numChannels, f.mp3Kbps);
        if (mp3->ok()) files.add (file);
        else if (error.isEmpty()) error = mp3->error();
    }
}

AudioFileSet::~AudioFileSet()
{
    finish();
}

bool AudioFileSet::write (const float* const* data, int n)
{
    if (finished) return false;
    bool good = true;
    if (wav != nullptr)  good = wav->writeFromFloatArrays (data, channels, n) && good;
    if (flac != nullptr) good = flac->writeFromFloatArrays (data, channels, n) && good;
    if (mp3 != nullptr && mp3->ok()) good = mp3->write (data, n) && good;
    if (! good && error.isEmpty()) error = "writing failed (is the disk full?)";
    return good;
}

bool AudioFileSet::finish()
{
    if (finished) return ok();
    finished = true;
    bool good = true;
    if (wav != nullptr)  { good = wav->flush() && good; wav.reset(); }
    if (flac != nullptr) { flac.reset(); }
    if (mp3 != nullptr)  { good = mp3->finish() && good; mp3.reset(); }
    if (! good && error.isEmpty()) error = "finishing the files failed";
    return good && ok();
}

//==============================================================================
bool TrackExport::run (Tape& tape, const TapeEngine::Params& op, const TapeEngine::Machine& machine, std::function<void (float)> progress)
{
    if (tape.isTurnedOver()) { error = "turn the tape back to side A first"; return false; }   // tracks go out as on side A
    // A deck of its own, set up as the one on screen, that only plays.
    TapeEngine eng;
    constexpr int block = 4096;
    eng.prepare (sampleRate, block);
    eng.setReadOnly (true);
    eng.params.speedSel = op.speedSel.load();
    eng.params.pitch = op.pitch.load();
    eng.params.nrOn = op.nrOn.load();
    eng.params.nrType = op.nrType.load();
    eng.params.sat = op.sat.load();
    eng.machine.wowPct = machine.wowPct.load();
    eng.machine.flutterPct = machine.flutterPct.load();
    eng.machine.driftPct = machine.driftPct.load();
    eng.machine.speedErrorPct = machine.speedErrorPct.load();
    eng.machine.cutoffHz = machine.cutoffHz.load();
    eng.machine.crosstalkDb = machine.crosstalkDb.load();
    eng.machine.honoursTapeCondition = machine.honoursTapeCondition.load();
    eng.setTape (&tape, false);
    eng.params.tapeSwitch = op.tapeSwitch.load();
    eng.seek (0.0);

    // The tracks with anything on them, from the start of the tape to the end of the last recording.
    std::vector<int> tracks;
    const auto blocks = tape.blockAt ((double) tape.getLengthCells());
    for (int t = 0; t < numTracks; ++t)
        for (int b = 0; b <= blocks; ++b)
            if (tape.isRecorded (t, b)) { tracks.push_back (t); break; }
    if (tracks.empty()) { error = "nothing recorded"; return false; }
    const double endCells = std::min ((double) tape.getLengthCells(), (tape.usedSeconds() + 0.5) * cellRate);

    std::vector<std::unique_ptr<AudioFileSet>> sets;
    for (auto t : tracks)
    {
        auto set = std::make_unique<AudioFileSet> (folder.getChildFile (juce::File::createLegalFileName (baseName + " - track " + juce::String (t + 1))),
                                                   formats, sampleRate, 1);
        if (! set->ok()) { error = set->getError(); return false; }
        sets.push_back (std::move (set));
    }

    std::vector<std::vector<float>> outs (numTracks, std::vector<float> ((size_t) block));
    float* outPtrs[numTracks] { outs[0].data(), outs[1].data(), outs[2].data(), outs[3].data() };
    const bool armed[numTracks] {};
    eng.command (TapeEngine::Command::play);
    double pos = 0.0;
    int count = 0;
    while (pos < endCells)
    {
        eng.beginBlock (block, armed);
        eng.readAll (outPtrs, block);
        eng.endBlock();
        for (size_t k = 0; k < tracks.size(); ++k)
        {
            // A mono file; both pointers are the track (GCC 11 can't see that the second isn't read).
            const float* ch[] { outs[(size_t) tracks[k]].data(), outs[(size_t) tracks[k]].data() };
            if (! sets[k]->write (ch, block)) { error = sets[k]->getError(); return false; }
        }
        if (eng.getMode() != Mode::play) break;   // the end of the tape
        // Where the tape has got to: the engine moves it as a deck would, wow and all.
        pos += block * eng.getNominalSpeed() * cellRate / sampleRate;
        if (progress && ++count % 16 == 0) progress ((float) juce::jmin (1.0, pos / endCells));
    }
    for (auto& set : sets)
    {
        if (! set->finish()) { error = set->getError(); return false; }
        written.addArray (set->getFiles());
    }
    return true;
}
} // namespace tf
