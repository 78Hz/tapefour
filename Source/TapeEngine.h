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

    struct Params
    {
        std::atomic<int>   speedSel { 1 };       // 1 = 1 7/8 ips, 2 = 3 3/4 ips
        std::atomic<int>   pitchStep { 0 };      // -5..+5, 2.4% per step
        std::atomic<bool>  nrOn { true };
        std::atomic<int>   nrType { 0 };
        std::atomic<int>   formulation { 0 };    // 0 = Type I normal, 1 = Type II chrome
        std::atomic<float> hiss { 0.55f }, wow { 0.30f }, flutter { 0.25f }, sat { 0.40f },
                           hf { 0.20f }, xtalk { 0.30f }, drop { 0.10f }, drift { 0.20f };
        std::atomic<int>   latencyOffsetMs { 0 };
    };

    Params params;

    void prepare (double sampleRate, int maxBlockSize);
    void setTape (Tape* t) noexcept;          // only call while the audio callback is locked out

    void command (Command c) noexcept         { pendingCommand.store ((int) c); }
    Mode getMode() const noexcept             { return (Mode) mode.load(); }
    bool isReturningToZero() const noexcept   { return rtz.load(); }
    bool takeEndOfTapeFlag() noexcept         { return endOfTape.exchange (false); }
    bool takeNoArmFlag() noexcept             { return noArmWarning.exchange (false); }
    float getNominalSpeed() const noexcept    { return nominalSpeed.load(); }   // incl. pitch, rel. to 1 7/8

    // ---- audio thread ----
    void beginBlock (int numSamples, const bool* armed, int deviceLatencySamples) noexcept;
    void readAll (float* const* out, int numSamples) noexcept;
    void writeTrack (int track, const float* in, int numSamples) noexcept;
    void endBlock() noexcept;
    bool anyPlayback() const noexcept         { return blockHasPlayback; }
    bool isRecordingNow() const noexcept      { return blockHasRecording; }

private:
    void writeSpan (int t, double p, float v, bool nr, float spd) noexcept;
    void configureCompanders (int type);

    double sr = 48000.0;
    int maxBlock = 0;
    Tape* tape = nullptr;

    std::atomic<int> pendingCommand { 0 }, mode { 0 };
    std::atomic<bool> rtz { false }, endOfTape { false }, noArmWarning { false };
    std::atomic<float> nominalSpeed { 1.0f };

    double pos = 0.0, windVel = 0.0;
    std::vector<double> posBuf;
    std::vector<uint8_t> playing, recording;
    bool blockHasPlayback = false, blockHasRecording = false;
    bool armedNow[numTracks] {};
    double latencyCells = 0.0;
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
    float dropGain[numTracks] { 1, 1, 1, 1 }, dropTarget[numTracks] { 1, 1, 1, 1 };
    int dropTimer[numTracks] {};
    std::vector<float> raw[numTracks];

    // writing
    bool hasPrev[numTracks] {};
    double prevPos[numTracks] {};
    float prevVal[numTracks] {};
    float lastHfCut = -1, lastAaCut = -1;
};
} // namespace tf
