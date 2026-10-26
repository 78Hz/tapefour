#pragma once

#include "Tape.h"

namespace tf
{
/**
    Noise reduction, as encode/decode pairs. What goes on tape is encoded;
    playback decodes. If the switch doesn't match how the tape was recorded,
    you hear exactly the kind of wrongness a real deck gives you.

    type 0: 2:1 broadband compander with pre/de-emphasis (dbx-style)
    type 1: single sliding band, up to 10 dB of HF lift (B-style)
    type 2: deeper sliding band, up to 20 dB of HF lift (C-style)
*/
class Compander
{
public:
    void configure (double sampleRate, int type, bool encoder);
    float process (float x) noexcept;
    void reset() noexcept;

private:
    inline float detect (float v) noexcept;

    int type = 0;
    bool isEncoder = true;
    Biquad emphasis;
    float env2 = 0, lpState = 0, lpCoef = 0, atk = 0, rel = 0, ref = 0.1f, maxBoost = 31.6f;
};

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
    };

    Params params;
    Machine machine;

    /** Test hook: measurements that need silence between the notes turn the tape's own hiss off.
        (No real cassette is without it, so the deck never does.) */
    void setHissForTests (bool on) noexcept { hissOn = on; }

    void prepare (double sampleRate, int maxBlockSize);
    void setTape (Tape* t, bool newTape = true) noexcept;          // only call while the audio callback is locked out

    void command (Command c) noexcept         { pendingCommand.store ((int) c); }
    Mode getMode() const noexcept             { return (Mode) mode.load(); }
    bool isReturningToZero() const noexcept   { return rtz.load(); }
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

private:
    double sr = 48000.0;
    int maxBlock = 0;
    std::atomic<bool> hissOn { true };
    Tape* tape = nullptr;

    std::atomic<int> pendingCommand { 0 }, mode { 0 };
    std::atomic<bool> rtz { false }, endOfTape { false }, noArmWarning { false };
    std::atomic<float> nominalSpeed { 1.0f };
    double pitchGlide = 1.0;              // audio thread: the varispeed motor, easing to the knob

    double pos = 0.0, windVel = 0.0;
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
    Compander decoder[numTracks], encoder[numTracks];
    int companderType = -1;
    Biquad hfLoss[numTracks][2];
    Biquad antiAlias[numTracks][2];
    float pink[numTracks][3] {};
    Noise hissNoise[numTracks] { Noise (11u), Noise (23u), Noise (37u), Noise (51u) };
    float dropGain[numTracks] { 1, 1, 1, 1 };
    // The cassette's dropouts are fixed places on it: worked out from its id, per block of tape.
    struct Dropout { int64_t block = -1; double start = 0, end = 0; float depth = 1.0f; };
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
    Biquad sigEq[numTracks], hissEq[numTracks];
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
