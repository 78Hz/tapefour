#pragma once

#include "Common.h"

/*
    TF N5: the Korg N5's (1998) programs, combinations, effects and arpeggiator, built to its owner's
    manual (N1/N5 Owner's Manual, Chapter 3), with simple and hand-drawn waveforms in place of its PCM
    samples. docs/n5.md says which values come from the manual and which are set by ear.

    Every setting is an integer in the manual's own range, described by a Field, so the window, the
    presets and the engine all work from the same tables.
*/
namespace tf::n5
{
enum class Fmt
{
    plain,      // 064
    signed_,    // +12
    choice,     // a label
    note,       // C4
    pan,        // RND, L63..CNT..R63, OFF (0 = RND, 1..127, 128 = OFF)
    slope,      // tenths: -1.0..+2.0
    egTime,     // 0..127, with the seconds shown
    lfoFreq,    // 0..127, with the Hz shown
    cutoff,     // 0..127, with the Hz shown
    quad,       // four OFF/+/- switches (AT, DT, ST, RT), in four values
    octave,     // 32', 16', 8', 4'
    cents,      // fine tune
    arpVelocity,// 0 KEY, 1..127, 128 STEP
    arpGate,    // 1..100 %, 101 STEP
    panOut      // OFF (-1), L (0), 99:01..01:99, R (100)
};

struct Field
{
    const char* id;
    const char* caption;
    int lo, hi, def;
    Fmt fmt;
    const char* labels = nullptr;   // for choices: separated by |
};

// ---- a layer: one oscillator, its filter and amplifier (a DOUBLE program has two) ----
enum Layer
{
    // OSC
    oWave, oDrawn, oOctave, oLevel, oTranspose, oFine, oVelBottom, oVelTop, oDelay, oSlope,
    // PITCH LFO
    plWave, plFreq, plInt, plDelay, plFade, plModInt, plModSpeed, plAftInt, plAftSpeed,
    // PITCH EG
    peStart, peAttackTime, peAttackLevel, peDecayTime, peReleaseTime, peReleaseLevel, peInt, peIntVel, peTimeVel,
    // VDF
    fCutoff, fColor, fColorVel, fKtMode, fKtKey, fKtInt, fKtEgTime, fKtSwA, fKtSwD, fKtSwS, fKtSwR,
    // VDF LFO
    flWave, flFreq, flInt, flDelay, flFade,
    // VDF EG
    feAttackTime, feAttackLevel, feDecayTime, feBreak, feSlopeTime, feSustain, feReleaseTime, feReleaseLevel,
    feInt, feIntVel, feTimeVel, feTvSwA, feTvSwD, feTvSwS, feTvSwR,
    // VDA
    aKtMode, aKtKey, aKtInt, aKtEgTime, aKtSwA, aKtSwD, aKtSwS, aKtSwR,
    // VDA LFO
    alWave, alFreq, alInt, alDelay, alFade,
    // VDA EG
    aeAttackTime, aeAttackLevel, aeDecayTime, aeBreak, aeSlopeTime, aeSustain, aeReleaseTime, aeAmpVel, aeTimeVel,
    aeTvSwA, aeTvSwD, aeTvSwS, aeTvSwR,
    // PAN/FX
    xPan, xSendC, xSendD,
    numLayer
};

enum Global
{
    gOscMode,
    gMonoPoly, gPortaSw, gPortaTime, gBendRange, gBendVdf, gModVdf, gAftBend, gAftVda, gAftVdf, gAftVdfLfo,
    numGlobal
};

const Field& layerField (int index);
const Field& globalField (int index);
inline constexpr int numWaves = 8, numDrawn = 16, drawnPoints = 256;
enum Wave { sine = 0, triangle, saw, square, pulse25, pulse12, noise, drawn };

// ---- effects (the manual's Effect Edit mode and its effect parameter table) ----
inline constexpr int numEffectTypes = 49;   // 0 no effect, 1..48
inline constexpr int maxEffectParams = 8;

struct EffectParam
{
    const char* name;
    int lo, hi, def;
    enum Kind { num, signed_, tenthSec, ms, speed, wave2, shape, balance, db, trigger, index } kind;
    bool dynamic;   // the dynamic modulation source moves it
};

struct EffectType
{
    const char* name;
    const char* group;
    int count;
    EffectParam params[maxEffectParams];
};
const EffectType& effectType (int type);
juce::String effectValueText (const EffectParam&, int value);
/** The modulation speeds 0.03..30 Hz, in 100 steps (the manual's displays round them). */
double speedHz (int index);

struct FxSettings
{
    int placement = 1;   // SERIAL, PARA.1, PARA.2, PARA.3
    struct Unit { int type = 0, on = 1, modSource = 0, modInt = 0; int p[maxEffectParams] {}; } unit[2];
    int pan3 = 0, pan4 = 100;           // L .. R (0 = L, 100 = R, -1 OFF)
    int level[4] { 9, 9, 9, 9 };        // PARA.3: 1-L, 1-R, 2-L, 2-R
    void setType (int u, int type);     // and the type's initial values
};

// ---- a program, a combination, the arpeggiator, the whole memory ----
struct ProgramData
{
    int layer[2][numLayer];
    int global[numGlobal];
    FxSettings fx;
    void init();
};

inline constexpr int numTimbres = 8;
enum Timbre { tProgram, tVolume, tPan, tTranspose, tFine, tSendC, tSendD, tNoteLo, tNoteHi, tVelLo, tVelHi,
              tRxNoteOn, tRxCc, tRxBend, tRxAft, tRxDamper, tRxPorta, numTimbre };
const Field& timbreField (int index);

struct CombiData
{
    int timbre[numTimbres][numTimbre];
    FxSettings fx;
    void init();
};

enum Arp { aOn, aType, aOctave, aSpeed, aLatch, aStep, aVelocity, aGate, aSort, aSwing, numArp };
const Field& arpField (int index);
const char* arpTypeName (int type);   // 1..20

inline constexpr int numPrograms = 32, numCombis = 16;

/** What the engine plays from: plain numbers, copied to the audio thread whole. */
struct BankData
{
    std::array<ProgramData, numPrograms> programs;
    std::array<CombiData, numCombis> combis;
    int arp[numArp];
    int combiMode = 0, program = 0, combi = 0;
    void init();
};

// physical meanings (set by ear: the manual gives only the 0..127 ranges)
double egSeconds (int v);
double lfoHz (int v);
double cutoffHz (double v);
juce::String fieldText (const Field&, int value);
juce::String noteName (int note);
} // namespace tf::n5
