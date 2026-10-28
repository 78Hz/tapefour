#pragma once

#include "Tape.h"
#include "NoiseReduction.h"

namespace tf
{
// Noise reduction (encode on record, decode on playback): NoiseReduction.h, modelled on the
// published B and C designs and the 2:1 compander. If the switch doesn't match how the tape was
// recorded, you hear exactly the kind of wrongness a real deck gives you.

class TapeEngine
{
public:
    enum class Command : int { none = 0, play, stop, rec, ff, rew, rtz };

    /** The operator's controls on the deck (kept in the session). */
    struct Params
    {
        std::atomic<int>   speedSel { 1 };       // 1 = 1 7/8 ips, 2 = 3 3/4 ips
        std::atomic<float> pitch { 0.0f };      // varispeed, percent: -12..+12, continuous
        std::atomic<bool>  nrOn { true };
        std::atomic<int>   nrType { 0 };
        // sat ("tape drive"): where 0 VU lands on the tape, -6..+6 dB around Dolby level.
        std::atomic<float> sat { 0.50f };
        std::atomic<int>   latencyOffsetMs { 0 };
        std::atomic<int>   tapeSwitch { 0 };     // 0 NORMAL, 1 CHROME, 2 METAL: record bias and EQ, playback EQ
    };

    /** The machine itself: its mechanism and heads (kept in TF-4.deck, docs/formats/deck-v1.md). */
    struct Machine
    {
        std::atomic<float> wowPct { 0.15f }, flutterPct { 0.075f }, driftPct { 0.2f }, speedErrorPct { 0.0f };
        std::atomic<float> cutoffHz { 15157.17f };     // where the heads start losing treble (playback)
        std::atomic<float> crosstalkDb { -65.0f };     // each neighbouring track, as the head picks it up; -120 = none
        /** Off: every cassette plays as if new (its formulation counts; its condition, wear and age don't). */
        std::atomic<bool>  honoursTapeCondition { true };
        /** Fast wind: seconds for one side of a C-60. The take-up spindle is driven, so the tape
            speeds up as the take-up pack grows (Geometry::windRadPerSecond). */
        std::atomic<float> windSecondsC60 { 120.0f };
        /** Cue and review: how far down the lightly touching head is, and how far off the tape it rides. */
        std::atomic<float> cueLevelDb { -18.0f }, cueSpacingUm { 3.0f };
        /** Cue and review wind slower than a fast wind: this fraction of its spindle speed. */
        std::atomic<float> cueWindRatio { 0.3f };
        /** The counter: false a mechanical one, countsPerTurn counts per turn of the take-up spindle;
            true a real-time one (seconds of playing time). */
        std::atomic<bool>  counterRealtime { false };
        std::atomic<float> countsPerTurn { 0.75f };
    };

    Params params;
    Machine machine;

    /** Test hook: measurements that need silence between the notes turn the tape's own hiss off.
        (No real cassette is without it, so the deck never does.) */
    void setHissForTests (bool on) noexcept { hissOn = on; }

    /** An engine that only plays a tape for rendering (exports): it doesn't move the tape in the deck
        or wear it. Set before use. */
    void setReadOnly (bool on) noexcept { readOnly = on; }
    /** For a read-only engine: where on the tape to start playing. */
    void seek (double cells) noexcept   { pos = lastBlockPos = cells; }
    /** Where the engine's head is (cells): for the thread that runs it. */
    double getPosition() const noexcept { return pos; }

    void prepare (double sampleRate, int maxBlockSize);
    void setTape (Tape* t, bool newTape = true) noexcept;          // only call while the audio callback is locked out

    void command (Command c) noexcept         { pendingCommand.store ((int) c); }
    Mode getMode() const noexcept             { return (Mode) mode.load(); }
    bool isReturningToZero() const noexcept   { return rtz.load(); }
    /** PLAY pressed while winding: the head rides lightly on the moving tape. */
    bool isCueing() const noexcept            { return cueing.load(); }
    bool takeRecInCueFlag() noexcept          { return recInCue.exchange (false); }
    /** The machine's counter: counts (mechanical) or seconds (real time), from where it was last reset.
        It belongs to the machine: it keeps its reading when cassettes are changed or turned over. */
    double getCounter() const noexcept        { return counter.load(); }
    void resetCounter() noexcept              { counterReset = true; }
    void setCounter (double v) noexcept       { counter = v; counterRebase = true; }
    bool takeEndOfTapeFlag() noexcept         { return endOfTape.exchange (false); }
    bool takeNoArmFlag() noexcept             { return noArmWarning.exchange (false); }
    float getNominalSpeed() const noexcept    { return nominalSpeed.load(); }   // incl. pitch, rel. to 1 7/8

    // ---- audio thread ----
    void beginBlock (int numSamples, const bool* armed) noexcept;
    void readAll (float* const* out, int numSamples) noexcept;
    /** Records a block onto one track. latencySamples is how long ago, in the tape's terms, the sound
        being recorded was meant to happen: what came in through the interface was played along to
        what was heard (input + output latency); an instrument only waits for the output; a bounce
        comes straight off the tape and needs none. */
    void writeTrack (int track, const float* in, int numSamples, double latencySamples) noexcept;
    void endBlock() noexcept;

    /** Punch-in and punch-out crossfade, like the gap between a deck's erase and record heads. */
    static constexpr double punchFadeSeconds = 0.006;
    bool isRecordingNow() const noexcept      { return blockHasRecording; }

private:
    void writeSpan (int t, double p, float v, bool nr, float spd, int switchPos) noexcept;
    void finishPunch (int t) noexcept;
    void configureCompanders (int type);
    void configureModel (const TapeModel& model, int speedSel, int switchPos);
    void calibrateHiss();

public:
    // Shared with the tests and the setup page.
    static float driveDb (float satParam) noexcept          { return (satParam - 0.5f) * 12.0f; }
    static float tanhThirdHarmonicPoint();                  // input peak giving 3% H3 through tanh
    static float magnitudeAt (const Biquad& b, double f, double sampleRate);

    /** What recording a tape at a switch position does, in dB, from how far the bias is off the tape's own. */
    struct BiasEffect { float biasDb, treble10k, midLevel, mol, extraModNoise; bool eraseBleed; };
    static BiasEffect biasEffect (int tapeIecType, int switchPos) noexcept;
    /** First-order filter for playing tape recorded with one EQ time constant through another. */
    static void setEqMismatch (Biquad& b, double sampleRate, double recordedUs, double playbackUs) noexcept;
    /** The playback EQ's response to tape noise (flux that's flat along the tape). */
    static void setFluxNoiseEq (Biquad& b, double sampleRate, double playbackUs) noexcept;

private:
    double sr = 48000.0;
    int maxBlock = 0;
    std::atomic<bool> hissOn { true };
    bool readOnly = false;
    int64_t lastPassSecond = -1;
    float printLp[numTracks] {}, printCoef = 0.3f;
    Tape* tape = nullptr;

    std::atomic<int> pendingCommand { 0 }, mode { 0 };
    std::atomic<bool> rtz { false }, endOfTape { false }, noArmWarning { false };
    std::atomic<bool> cueing { false }, recInCue { false }, counterReset { false };
    std::atomic<double> counter { 0.0 };
    // The counter reads counterOffset + counterTurns (pos): a function of where the tape is, so it
    // comes back to exactly the same reading at the same place, however the tape got there.
    std::atomic<bool> counterRebase { true };
    double counterOffset = 0.0;
    const Tape* counterTape = nullptr;
    int counterKey = -1;
    double counterTurns (double position, int speedSel) const noexcept;
    bool blockCue = false;
    float cueGain = 1.0f, cueLpCoef = 0.0f, cueLp[numTracks] {};
    std::vector<double> prevPosBuf;
    std::vector<uint8_t> onOxide;
    int64_t lastCueSecond = -1;
    uint32_t cueCrossings = 0;
    std::atomic<float> nominalSpeed { 1.0f };
    double pitchGlide = 1.0;              // audio thread: the varispeed motor, easing to the knob

    double pos = 0.0, windVel = 0.0, lastBlockPos = 0.0;
    std::vector<double> posBuf;
    std::vector<uint8_t> playing, recording;
    bool blockHasPlayback = false, blockHasRecording = false;
    bool armedNow[numTracks] {};
    int lastMode = 0;

    // modulation
    double ph[4] {}, driftVal = 0.0, driftTarget = 0.0;
    int driftCounter = 0;
    Noise rng { 0x9e3779b9u };

    // per-track playback colour
    nr::NoiseReduction decoder[numTracks], encoder[numTracks];
    int companderType = -1;
    Biquad hfLoss[numTracks][2];
    Biquad antiAlias[numTracks][2];
    // Each machine's tape noise its own: decks playing at once never hiss in step.
    static uint32_t engineSeed() noexcept { static std::atomic<uint32_t> n { 0 }; return 0x6a09e667u * ++n; }
    const uint32_t hissSeed = engineSeed();
    Noise hissNoise[numTracks] { Noise (hissSeed + 11u), Noise (hissSeed + 23u), Noise (hissSeed + 37u), Noise (hissSeed + 51u) };
    float dropGain[numTracks] { 1, 1, 1, 1 };
    // The cassette's dropouts are fixed places on it: worked out from its id, per block of tape.
    struct Dropout { int64_t block = -1; int track = -1; double start = 0, end = 0; float depth = 1.0f; };
    Dropout dropHere[numTracks], dropBefore[numTracks];
    static Dropout dropoutAt (uint64_t seed, int track, int64_t block, float perBlock) noexcept;
    Biquad oxideLoss[numTracks];
    float lastOxideLoss = -1.0f;
    std::vector<float> raw[numTracks];

    // writing
    bool hasPrev[numTracks] {}, wroteThisBlock[numTracks] {};
    double prevPos[numTracks] {};
    float prevVal[numTracks] {};

    // Punch crossfades: the new recording fades in over what was there; at punch-out the last
    // stretch is blended back towards what it replaced, which is kept in this ring.
    static constexpr int punchRing = 1024;
    struct Replaced { int64_t cell; float old; };
    Replaced replaced[numTracks][punchRing] {};
    int replacedHead[numTracks] {}, replacedCount[numTracks] {};
    int64_t runCells[numTracks] {};
    bool runNr[numTracks] {};
    float runSpeed[numTracks] {};
    int runPos[numTracks] {};
    float lastHfCut = -1, lastAaCut = -1;

    // the tape model
    const TapeModel* cfgModel = nullptr;
    int cfgSpeed = -1, cfgPos = -1;
    Biquad biasHi[numTracks];
    float biasMidGain = 1.0f, recModDepth = 0.0f, recModLp[numTracks] {};
    bool eraseBleed = false;
    float eraseLp[numTracks] {};
    // playback EQ against what's on the tape
    Biquad sigEq[numTracks];
    int sigEqKey[numTracks] { -1, -1, -1, -1 }, hissEqKey = -1;
    float satK = 1.0f, sensGain = 1.0f, modDepth = 0.0f;
    Biquad emphLo[numTracks], emphHi[numTracks], deemphLo[numTracks], deemphHi[numTracks];
    Biquad preAA[numTracks][2];
    Biquad hissShape[numTracks];
    float hissCal[2] { 1.0f, 1.0f };            // A-weighted RMS of the hiss generator: 120 us, 70 us
    float modLp[numTracks] {}, modCoef = 0.0f, modNorm = 1.0f;
    Noise modRng[numTracks] { Noise (71u), Noise (73u), Noise (79u), Noise (83u) };
};
} // namespace tf
