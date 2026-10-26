#pragma once

#include "TapeEngine.h"

namespace tf
{
/** Which files an export or a mixdown makes. Remembered in the settings. */
struct ExportFormats
{
    bool wav = true, flac = false, mp3 = false;
    int wavBits = 24;     // 16, 24 or 32 (32 is floating point)
    int flacBits = 24;    // 16 or 24
    int mp3Kbps = 320;    // 192, 256 or 320 (constant bit rate)

    bool any() const noexcept { return wav || flac || mp3; }
    bool all() const noexcept { return wav && flac && mp3; }
    /** "WAV + MP3", "ALL FORMATS"... */
    juce::String describe() const;
    std::unique_ptr<juce::XmlElement> toXml() const;
    static ExportFormats fromXml (const juce::XmlElement*);
    bool operator== (const ExportFormats&) const;
};

/**
    One recording written to every chosen format at once. Not for the audio thread: feed it from
    a background thread (a mixdown does, through a FIFO). MP3 is encoded with LAME as it goes;
    its header is filled in when the file is finished, so players show the right length.
*/
class AudioFileSet
{
public:
    /** base: the path without an extension; ".wav", ".flac" and ".mp3" are added. */
    AudioFileSet (const juce::File& base, const ExportFormats&, double sampleRate, int numChannels);
    ~AudioFileSet();

    bool ok() const noexcept                    { return error.isEmpty(); }
    const juce::String& getError() const noexcept { return error; }
    juce::Array<juce::File> getFiles() const    { return files; }

    bool write (const float* const* channels, int numSamples);
    /** Flushes and closes every file. */
    bool finish();

private:
    class Mp3Writer;
    std::unique_ptr<juce::AudioFormatWriter> wav, flac;
    std::unique_ptr<Mp3Writer> mp3;
    juce::Array<juce::File> files;
    juce::String error;
    int channels = 1;
    bool finished = false;
};

/**
    The tracks of a cassette as the deck plays them (through its heads, EQ, noise reduction and
    the tape's character), one file per recorded track, rendered faster than real time. The tape
    in the deck isn't moved or worn. Runs on a background thread.
*/
struct TrackExport
{
    juce::File folder;
    juce::String baseName;         // "demo songs" -> "demo songs - track 1.wav"
    ExportFormats formats;
    double sampleRate = 48000.0;

    /** Renders and writes; progress (0..1) is reported from the same thread. */
    bool run (Tape& tape, const TapeEngine::Params& operatorControls, const TapeEngine::Machine& machine,
              std::function<void (float)> progress = {});

    juce::Array<juce::File> written;
    juce::String error;
};
} // namespace tf
