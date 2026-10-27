#pragma once

#include "Exporter.h"
#include "Deck.h"

namespace tf
{
/** How the master deck is set (remembered in the settings). */
struct MasterSettings
{
    int model = defaultTapeModel;   // the formulation of new master tapes; -1: DIRECT (no tape at all)
    int length = 1;                 // C-46, C-60 or C-90, for new master tapes
    juce::String grade = "new";     // the condition new master tapes come in
    bool nr = true;                 // record (and play) the master with noise reduction
    float recLevelDb = 0.0f;        // the master deck's record level: -12 .. +6 dB
    juce::String masterId;          // the master cassette, on the shelf
    int side = 0;                   // 0 side A, 1 side B

    bool direct() const noexcept { return model < 0; }
    std::unique_ptr<juce::XmlElement> toXml() const;
    static MasterSettings fromXml (const juce::XmlElement*);
};

/** One mixdown: where it is on the master tape, and the files it made. */
struct MixTake
{
    int number = 0;
    int side = 0;
    double start = 0, length = 0;   // cells along its side
    juce::String created;           // ISO 8601
    juce::String onTape;            // "SD60 · NR", or "DIRECT"
    juce::String formats;           // "WAV + MP3"
    juce::StringArray files;        // full paths

    tfmt::Json toJson() const;
    static MixTake fromJson (const tfmt::Json&);
};

/**
    The stereo cassette deck the mix goes to. Its cassette is a real two-sided .cass on the shelf;
    each mixdown is recorded onto the current side after the last one, then played straight back
    off the tape, as a three-head deck does, and that is what the files hold. DIRECT skips the tape.

    Threads: settings, arming and takes on the message thread; process() and listenInto() on the
    audio thread; the files are written on a thread of its own, fed through a FIFO, so the audio
    thread never waits for the disk.
*/
class MasterDeck
{
public:
    MasterDeck();
    ~MasterDeck();

    // ---- message thread ----
    /** Called while the audio is stopped (allocates). */
    void prepare (double sampleRate, int maxBlock);
    /** The audio device is stopping: a running take is finished and saved. */
    void stopForDeviceChange();
    void setMachine (const tfmt::DeckDescription&);
    tfmt::DeckDescription getMachine() const { return machine; }
    static tfmt::DeckDescription factoryMachine();

    MasterSettings settings;
    /** Pushes the record level, NR and side to the audio thread (not while a take is running). */
    void applySettings (int nrType);
    /** The record level alone: any time, even during a take, as on a real deck. */
    void setRecLevel (float db) noexcept { recGain = dbToGain (db); }

    Tape* getTape() const noexcept { return tape.get(); }
    /** Puts a master cassette in (or none); the old one comes back. Not while a take is running. */
    std::unique_ptr<Tape> swapTape (std::unique_ptr<Tape>);
    /** The folder of the master cassette on the shelf (where takes.json lives). */
    juce::File tapeFolder() const;

    std::vector<MixTake> takes;   // on the master cassette in, oldest first
    void loadTakes();
    void saveTakes() const;
    /** Where the next take would start on the current side (cells); -1 if the side is full. */
    double nextStart() const;
    double sideUsedSeconds (int side) const;
    double sideLengthSeconds() const;

    enum class State : int { off, armed, recording, tailing, finishing, done };
    State getState() const noexcept { return (State) state.load(); }
    /** Ready to record a take as soon as the deck plays. False (with why) if it can't. */
    bool arm (const ExportFormats&, const juce::File& folder, const juce::String& baseName, int nrType, juce::String& whyNot);
    /** Off; a take that is running is finished first. */
    void disarm();
    bool isArmedByUser() const noexcept { return wantArmed; }
    /** A take that has finished, to add to the list (message thread, polled). */
    bool collectFinished (MixTake& take, juce::String& problem);

    // what the displays show
    int currentTakeNumber() const noexcept       { return pending.number; }
    double takeSeconds() const noexcept          { return (double) takeSamples.load() / std::max (1.0, sr); }
    double clippedAtSeconds() const noexcept     { auto c = clipAt.load(); return c < 0 ? -1.0 : (double) c / std::max (1.0, sr); }
    juce::String describeTape() const;           // "SD60 · NR", "DIRECT"

    // listening to a take
    bool listen (const MixTake&);
    void stopListening() noexcept               { listening = false; }
    bool isListening() const noexcept           { return listening.load(); }

    // ---- audio thread ----
    /** The master bus after the master fader. deckPlaying: the deck's transport is playing or recording. */
    void process (const float* l, const float* r, int n, bool deckPlaying) noexcept;
    /** While listening to a take, the master tape replaces the mix. True if it wrote into l and r. */
    bool listenInto (float* l, float* r, int n) noexcept;

    /** The gap between takes on a side, and the leader before the first. */
    static constexpr double gapSeconds = 4.0, leaderSeconds = 2.0;
    /** How far the playback head sits after the record head: the delay of off-tape monitoring. */
    static constexpr double headGapSeconds = 0.1;

private:
    class Writer;
    void configureEngines (int nrType);
    void startTake() noexcept;
    void push (const float* l, const float* r, int n) noexcept;

    double sr = 48000.0;
    int maxBlock = 512;
    tfmt::DeckDescription machine;
    std::unique_ptr<Tape> tape;
    TapeEngine rec, play;                 // the record head's transport, and the playback head's
    std::vector<float> inL, inR, outs[numTracks];
    float* outPtrs[numTracks] {};

    std::atomic<int> state { (int) State::off };
    std::atomic<bool> wantArmed { false }, stopRequested { false }, listening { false };
    std::atomic<int64_t> takeSamples { 0 }, clipAt { -1 }, dropped { 0 };
    std::atomic<float> recGain { 1.0f };
    bool directTake = false;              // set with the pending take, read by the audio thread after arming
    int takeSide = 0;
    double takeStart = 0;
    int64_t skipSamples = 0, tailSamples = 0, gapSamples = 0;
    double listenEnd = 0;

    struct Pending
    {
        int number = 0;
        juce::File base;
        ExportFormats formats;
        juce::String onTape;
    } pending;

    juce::AbstractFifo fifo { 1 };
    std::vector<float> fifoL, fifoR;
    std::unique_ptr<Writer> writer;

    juce::CriticalSection finishedLock;
    std::unique_ptr<MixTake> finished;
    juce::String finishedProblem;

    JUCE_DECLARE_NON_COPYABLE (MasterDeck)
};

/** A whole side of a master cassette as one file, as the master deck plays it. Background thread. */
struct SideExport
{
    juce::File base;   // without an extension
    ExportFormats formats;
    double sampleRate = 48000.0;
    int side = 0;
    bool run (Tape& masterTape, const tfmt::DeckDescription& machine, bool nr, int nrType);
    juce::Array<juce::File> written;
    juce::String error;
};
} // namespace tf
