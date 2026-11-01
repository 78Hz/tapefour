#include "N5Model.h"

namespace tf::n5
{
namespace
{
    const char* const lfoWaves = "TRI|SAW UP|SAW DN|SQ 1|SQ 2|RND";
    const char* const ktModes = "OFF|LOW|HIGH|ALL";
    const char* const oscWaves = "SINE|TRIANGLE|SAW|SQUARE|PULSE 25%|PULSE 12%|NOISE|DRAWN";

    // Every range is the manual's (Chapter 3, Program Edit mode); the initial values are a plain,
    // playable program (the manual gives none for programs).
    const Field layerFields[numLayer] {
        { "wave", "WAVE", 0, 7, 2, Fmt::choice, oscWaves },
        { "drawn", "DRAWN WAVE", 1, 16, 1, Fmt::plain },
        { "octave", "OCTAVE", 0, 3, 2, Fmt::octave },
        { "level", "OSC LEVEL", 0, 127, 100, Fmt::plain },
        { "transpose", "KEY TRANSPOSE", -12, 12, 0, Fmt::signed_ },
        { "fine", "FINE TUNE", -99, 99, 0, Fmt::cents },
        { "velBottom", "VEL WINDOW BOTTOM", 1, 127, 1, Fmt::plain },
        { "velTop", "VEL WINDOW TOP", 1, 127, 127, Fmt::plain },
        { "delay", "DELAY START", 0, 127, 0, Fmt::egTime },
        { "slope", "PITCH SLOPE", -10, 20, 10, Fmt::slope },
        { "plWave", "LFO WAVEFORM", 0, 5, 0, Fmt::choice, lfoWaves },
        { "plFreq", "LFO FREQUENCY", 0, 127, 90, Fmt::lfoFreq },
        { "plInt", "LFO INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "plDelay", "LFO DELAY", 0, 127, 0, Fmt::egTime },
        { "plFade", "LFO FADE-IN", 0, 127, 0, Fmt::egTime },
        { "plModInt", "MOD WHEEL INTENSITY", 0, 127, 20, Fmt::plain },
        { "plModSpeed", "MOD WHEEL SPEED", 0, 127, 0, Fmt::plain },
        { "plAftInt", "AFTERTOUCH INTENSITY", 0, 127, 0, Fmt::plain },
        { "plAftSpeed", "AFTERTOUCH SPEED", 0, 127, 0, Fmt::plain },
        { "peStart", "EG START LEVEL", -128, 127, 0, Fmt::signed_ },
        { "peAttT", "EG ATTACK TIME", 0, 127, 0, Fmt::egTime },
        { "peAttL", "EG ATTACK LEVEL", -128, 127, 0, Fmt::signed_ },
        { "peDecT", "EG DECAY TIME", 0, 127, 0, Fmt::egTime },
        { "peRelT", "EG RELEASE TIME", 0, 127, 0, Fmt::egTime },
        { "peRelL", "EG RELEASE LEVEL", -128, 127, 0, Fmt::signed_ },
        { "peInt", "EG INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "peIntVel", "EG INT VELOCITY", -128, 127, 0, Fmt::signed_ },
        { "peTimeVel", "EG TIME VELOCITY", -128, 127, 0, Fmt::signed_ },
        { "cutoff", "CUTOFF", 0, 127, 100, Fmt::cutoff },
        { "color", "COLOR", 0, 127, 0, Fmt::plain },
        { "colorVel", "COLOR VELOCITY", -128, 127, 0, Fmt::signed_ },
        { "fKtMode", "KEY TRACK MODE", 0, 3, 3, Fmt::choice, ktModes },
        { "fKtKey", "KEY TRACK KEY", 0, 127, 60, Fmt::note },
        { "fKtInt", "KEY TRACK INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "fKtEgTime", "KEY TRACK EG TIME", -128, 127, 0, Fmt::signed_ },
        { "fKtSwA", "KEY TRACK EG TIME SW", 0, 2, 0, Fmt::quad },
        { "fKtSwD", "", 0, 2, 0, Fmt::quad },
        { "fKtSwS", "", 0, 2, 0, Fmt::quad },
        { "fKtSwR", "", 0, 2, 0, Fmt::quad },
        { "flWave", "LFO WAVEFORM", 0, 5, 0, Fmt::choice, lfoWaves },
        { "flFreq", "LFO FREQUENCY", 0, 127, 64, Fmt::lfoFreq },
        { "flInt", "LFO INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "flDelay", "LFO DELAY", 0, 127, 0, Fmt::egTime },
        { "flFade", "LFO FADE-IN", 0, 127, 0, Fmt::egTime },
        { "feAttT", "EG ATTACK TIME", 0, 127, 0, Fmt::egTime },
        { "feAttL", "EG ATTACK LEVEL", -128, 127, 0, Fmt::signed_ },
        { "feDecT", "EG DECAY TIME", 0, 127, 50, Fmt::egTime },
        { "feBreak", "EG BREAK POINT", -128, 127, 0, Fmt::signed_ },
        { "feSlopeT", "EG SLOPE TIME", 0, 127, 50, Fmt::egTime },
        { "feSus", "EG SUSTAIN LEVEL", -128, 127, 0, Fmt::signed_ },
        { "feRelT", "EG RELEASE TIME", 0, 127, 50, Fmt::egTime },
        { "feRelL", "EG RELEASE LEVEL", -128, 127, 0, Fmt::signed_ },
        { "feInt", "EG INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "feIntVel", "EG INT VELOCITY", -128, 127, 0, Fmt::signed_ },
        { "feTimeVel", "EG TIME VELOCITY", 0, 127, 0, Fmt::plain },
        { "feTvSwA", "EG TIME VEL SW", 0, 2, 0, Fmt::quad },
        { "feTvSwD", "", 0, 2, 0, Fmt::quad },
        { "feTvSwS", "", 0, 2, 0, Fmt::quad },
        { "feTvSwR", "", 0, 2, 0, Fmt::quad },
        { "aKtMode", "KEY TRACK MODE", 0, 3, 0, Fmt::choice, ktModes },
        { "aKtKey", "KEY TRACK KEY", 0, 127, 60, Fmt::note },
        { "aKtInt", "KEY TRACK INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "aKtEgTime", "KEY TRACK EG TIME", -128, 127, 0, Fmt::signed_ },
        { "aKtSwA", "KEY TRACK EG TIME SW", 0, 2, 0, Fmt::quad },
        { "aKtSwD", "", 0, 2, 0, Fmt::quad },
        { "aKtSwS", "", 0, 2, 0, Fmt::quad },
        { "aKtSwR", "", 0, 2, 0, Fmt::quad },
        { "alWave", "LFO WAVEFORM", 0, 5, 0, Fmt::choice, lfoWaves },
        { "alFreq", "LFO FREQUENCY", 0, 127, 64, Fmt::lfoFreq },
        { "alInt", "LFO INTENSITY", -128, 127, 0, Fmt::signed_ },
        { "alDelay", "LFO DELAY", 0, 127, 0, Fmt::egTime },
        { "alFade", "LFO FADE-IN", 0, 127, 0, Fmt::egTime },
        { "aeAttT", "EG ATTACK TIME", 0, 127, 0, Fmt::egTime },
        { "aeAttL", "EG ATTACK LEVEL", 0, 127, 127, Fmt::plain },
        { "aeDecT", "EG DECAY TIME", 0, 127, 60, Fmt::egTime },
        { "aeBreak", "EG BREAK POINT", 0, 127, 110, Fmt::plain },
        { "aeSlopeT", "EG SLOPE TIME", 0, 127, 70, Fmt::egTime },
        { "aeSus", "EG SUSTAIN LEVEL", 0, 127, 100, Fmt::plain },
        { "aeRelT", "EG RELEASE TIME", 0, 127, 40, Fmt::egTime },
        { "aeAmpVel", "EG AMP VELOCITY", -128, 127, 50, Fmt::signed_ },
        { "aeTimeVel", "EG TIME VELOCITY", 0, 127, 0, Fmt::plain },
        { "aeTvSwA", "EG TIME VEL SW", 0, 2, 0, Fmt::quad },
        { "aeTvSwD", "", 0, 2, 0, Fmt::quad },
        { "aeTvSwS", "", 0, 2, 0, Fmt::quad },
        { "aeTvSwR", "", 0, 2, 0, Fmt::quad },
        { "pan", "OSC PANPOT", 0, 128, 64, Fmt::pan },
        { "sendC", "C SEND LEVEL", 0, 127, 40, Fmt::plain },
        { "sendD", "D SEND LEVEL", 0, 127, 0, Fmt::plain },
    };

    const Field globalFields[numGlobal] {
        { "oscMode", "OSC MODE", 0, 1, 0, Fmt::choice, "SINGLE|DOUBLE" },
        { "mono", "MONO/POLY", 0, 1, 1, Fmt::choice, "MONO|POLY" },
        { "portaSw", "PORTAMENTO", 0, 1, 0, Fmt::choice, "OFF|ON" },
        { "portaTime", "PORTAMENTO TIME", 0, 127, 40, Fmt::egTime },
        { "bendRange", "PITCH BEND RANGE", -24, 24, 2, Fmt::signed_ },
        { "bendVdf", "BEND WHEEL VDF", -128, 127, 0, Fmt::signed_ },
        { "modVdf", "MOD WHEEL VDF", 0, 127, 0, Fmt::plain },
        { "aftBend", "AFTERTOUCH BEND RANGE", -24, 24, 0, Fmt::signed_ },
        { "aftVda", "AFTERTOUCH VDA", -128, 127, 0, Fmt::signed_ },
        { "aftVdf", "AFTERTOUCH VDF", -128, 127, 0, Fmt::signed_ },
        { "aftVdfLfo", "AFTERTOUCH VDF LFO", 0, 127, 0, Fmt::plain },
    };

    const Field timbreFields[numTimbre] {
        { "program", "PROGRAM", -1, numPrograms - 1, -1, Fmt::plain },
        { "volume", "VOLUME", 0, 127, 100, Fmt::plain },
        { "pan", "PANPOT", 0, 128, 64, Fmt::pan },
        { "transpose", "TRANSPOSE", -24, 24, 0, Fmt::signed_ },
        { "fine", "FINE TUNE", -50, 50, 0, Fmt::cents },
        { "sendC", "C SEND", 0, 127, 40, Fmt::plain },
        { "sendD", "D SEND", 0, 127, 0, Fmt::plain },
        { "noteLo", "NOTE WINDOW BOTTOM", 0, 127, 0, Fmt::note },
        { "noteHi", "NOTE WINDOW TOP", 0, 127, 127, Fmt::note },
        { "velLo", "VEL WINDOW BOTTOM", 1, 127, 1, Fmt::plain },
        { "velHi", "VEL WINDOW TOP", 1, 127, 127, Fmt::plain },
        { "rxNoteOn", "RX NOTE ON", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "rxCc", "RX CONTROL CHANGE", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "rxBend", "RX PITCH BEND", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "rxAft", "RX AFTERTOUCH", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "rxDamper", "RX DAMPER", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "rxPorta", "RX PORTAMENTO", 0, 1, 1, Fmt::choice, "OFF|ON" },
    };

    // Performance Edit mode's arpeggiator: STEP velocity and gate are 128 and 101
    const Field arpFields[numArp] {
        { "on", "ARPEGGIATOR", 0, 1, 0, Fmt::choice, "OFF|ON" },
        { "type", "TYPE", 1, 20, 1, Fmt::choice, "UP|DOWN|ALT1|ALT2|RANDOM|ARP 1|ARP 2|ARP 3|ARP 4|ARP 5|ARP 6|B-TECHNO|B-DANCE|B-FUNK|B-SOUL|B-JAZZ|D-TECHNO|D-JUNGLE|D-FUNK|D-R&B" },
        { "octave", "OCTAVE", 1, 4, 1, Fmt::plain },
        { "speed", "SPEED (BPM)", 40, 240, 120, Fmt::plain },
        { "latch", "LATCH / KEY SYNC", 0, 3, 0, Fmt::choice, "OFF|LATCH|K.SYNC|L&K.S" },
        { "step", "STEP", 0, 5, 4, Fmt::choice, "1/4|1/4 T|1/8|1/8 T|1/16|1/16 T" },
        { "velocity", "VELOCITY", 0, 128, 0, Fmt::arpVelocity },
        { "gate", "GATE", 1, 101, 80, Fmt::arpGate },
        { "sort", "SORT", 0, 1, 1, Fmt::choice, "OFF|ON" },
        { "swing", "SWING", -99, 99, 0, Fmt::signed_ },
    };

    const char* const arpNames[20] { "UP", "DOWN", "ALT1", "ALT2", "RANDOM", "ARP 1", "ARP 2", "ARP 3", "ARP 4", "ARP 5", "ARP 6",
                                     "B-TECHNO", "B-DANCE", "B-FUNK", "B-SOUL", "B-JAZZ", "D-TECHNO", "D-JUNGLE", "D-FUNK", "D-R&B" };

    using P = EffectParam;
    constexpr auto N = P::num, S = P::signed_, T = P::tenthSec, M = P::ms, V = P::speed, W = P::wave2, H = P::shape,
                   B = P::balance, D = P::db, G = P::trigger, I = P::index;

    // The manual's effect parameter table (pp. 48-49): ranges and [initial values]; dynamic: its ● marks.
    // Speeds 0.03..30 Hz are stored as steps of speedHz(); balances 0..100 (DRY = 0, 80:20 = 20, FX = 100).
#define REVERB(name, t, pd, er, hd, lo, hi, bal, tmax) \
    { name, "REVERB", 7, { { "REVERB TIME", 2, tmax, t, T, false }, { "PRE DELAY", 0, 200, pd, M, false }, { "ER LEVEL", 0, 99, er, N, false }, \
                           { "HIGH DAMP", 0, 99, hd, N, false }, { "EQ LOW", -12, 12, lo, D, false }, { "EQ HIGH", -12, 12, hi, D, false }, \
                           { "DRY : FX", 0, 100, bal, B, true } } }
#define PLATE(name, t, pd, er, hd, lo, hi, bal) \
    { name, "REVERB", 7, { { "REVERB TIME", 0, 99, t, N, false }, { "PRE DELAY", 0, 200, pd, M, false }, { "ER LEVEL", 1, 10, er, N, false }, \
                           { "HIGH DAMP", 0, 99, hd, N, false }, { "EQ LOW", -12, 12, lo, D, false }, { "EQ HIGH", -12, 12, hi, D, false }, \
                           { "DRY : FX", 0, 100, bal, B, true } } }
#define ER(name, t, pd, lo, hi, bal) \
    { name, "EARLY REFLECTION", 5, { { "ER TIME", 100, 800, t, M, false }, { "PRE DELAY", 0, 200, pd, M, false }, \
                                     { "EQ LOW", -12, 12, lo, D, false }, { "EQ HIGH", -12, 12, hi, D, false }, { "DRY : FX", 0, 100, bal, B, true } } }

    const EffectType effectTypes[numEffectTypes] {
        { "NO EFFECT", "", 0, {} },
        REVERB ("HALL", 23, 60, 62, 31, -3, -1, 20, 99),
        REVERB ("ENSEMBLE HALL", 31, 15, 23, 32, -1, -3, 20, 99),
        REVERB ("CONCERT HALL", 33, 80, 46, 41, -2, -4, 20, 99),
        REVERB ("ROOM", 13, 8, 68, 36, 1, 2, 22, 49),
        REVERB ("LARGE ROOM", 24, 25, 51, 32, -1, 2, 22, 49),
        REVERB ("LIVE STAGE", 22, 12, 81, 36, -5, -4, 25, 49),
        PLATE ("WET PLATE", 59, 29, 7, 51, 0, -4, 20),
        PLATE ("DRY PLATE", 30, 26, 5, 47, 2, 2, 20),
        PLATE ("SPRING REVERB", 25, 0, 9, 30, 2, -4, 22),
        ER ("EARLY REFLECTION 1", 220, 0, -4, -4, 32),
        ER ("EARLY REFLECTION 2", 180, 30, 1, 0, 35),
        ER ("EARLY REFLECTION 3", 300, 90, 0, 0, 25),
        { "STEREO DELAY", "STEREO DELAY", 7, { { "DELAY TIME L", 0, 500, 185, M, false }, { "DELAY TIME R", 0, 500, 370, M, false }, { "FEEDBACK", -99, 99, -40, S, false },
                                              { "HIGH DAMP", 0, 99, 10, N, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 20, B, true } } },
        { "CROSS DELAY", "STEREO DELAY", 7, { { "DELAY TIME L", 0, 500, 190, M, false }, { "DELAY TIME R", 0, 500, 380, M, false }, { "FEEDBACK", -99, 99, 40, S, false },
                                             { "HIGH DAMP", 0, 99, 10, N, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 20, B, true } } },
        { "DUAL MONO DELAY", "DUAL MONO DELAY", 8, { { "DELAY TIME L", 0, 500, 20, M, false }, { "FEEDBACK L", -99, 99, 0, S, false }, { "HIGH DAMP L", 0, 99, 0, N, false },
                                                    { "DRY : FX L", 0, 100, 50, B, false }, { "DELAY TIME R", 0, 500, 40, M, false }, { "FEEDBACK R", -99, 99, 0, S, false },
                                                    { "HIGH DAMP R", 0, 99, 0, N, false }, { "DRY : FX R", 0, 100, 65, B, true } } },
        { "MULTI-TAP DELAY 1", "MULTI-TAP DELAY", 6, { { "DELAY TIME 1", 0, 500, 175, M, false }, { "DELAY TIME 2", 0, 500, 350, M, false }, { "FEEDBACK", -99, 99, 30, S, false },
                                                      { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 20, B, true } } },
        { "MULTI-TAP DELAY 2", "MULTI-TAP DELAY", 6, { { "DELAY TIME 1", 0, 500, 200, M, false }, { "DELAY TIME 2", 0, 500, 400, M, false }, { "FEEDBACK", -99, 99, 0, S, false },
                                                      { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 30, B, true } } },
        { "MULTI-TAP DELAY 3", "MULTI-TAP DELAY", 6, { { "DELAY TIME 1", 0, 500, 250, M, false }, { "DELAY TIME 2", 0, 500, 500, M, false }, { "FEEDBACK", -99, 99, 20, S, false },
                                                      { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 25, B, true } } },
        { "CHORUS 1", "CHORUS", 7, { { "DELAY TIME", 0, 200, 3, M, false }, { "MOD SPEED", 0, 99, 34, V, false }, { "MOD DEPTH", 0, 99, 99, N, false },
                                    { "MOD WAVEFORM", 0, 1, 1, W, false }, { "EQ LOW", -12, 12, 4, D, false }, { "EQ HIGH", -12, 12, 4, D, false }, { "DRY : FX", 0, 100, 50, B, true } } },
        { "CHORUS 2", "CHORUS", 7, { { "DELAY TIME", 0, 200, 2, M, false }, { "MOD SPEED", 0, 99, 38, V, false }, { "MOD DEPTH", 0, 99, 84, N, false },
                                    { "MOD WAVEFORM", 0, 1, 0, W, false }, { "EQ LOW", -12, 12, 3, D, false }, { "EQ HIGH", -12, 12, 4, D, false }, { "DRY : FX", 0, 100, 40, B, true } } },
        { "QUADRATURE CHORUS", "CHORUS", 8, { { "DELAY TIME L", 0, 250, 24, M, false }, { "DELAY TIME R", 0, 250, 12, M, false }, { "MOD SPEED", 1, 99, 30, N, true },
                                             { "MOD DEPTH", 0, 99, 50, N, false }, { "MOD SHAPE", 0, 41, 10, H, false }, { "EQ LOW", -12, 12, 0, D, false },
                                             { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "CROSSOVER CHORUS", "CHORUS", 8, { { "DELAY TIME L", 0, 250, 2, M, false }, { "DELAY TIME R", 0, 250, 24, M, false }, { "MOD SPEED", 1, 99, 16, N, true },
                                            { "MOD DEPTH", 0, 99, 99, N, false }, { "MOD SHAPE", 0, 41, 10, H, false }, { "EQ LOW", -12, 12, 0, D, false },
                                            { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "HARMONIC CHORUS", "HARMONIC CHORUS", 6, { { "DELAY TIME 1", 0, 500, 4, M, false }, { "DELAY TIME 2", 0, 500, 12, M, false }, { "MOD SPEED", 1, 99, 36, N, true },
                                                    { "MOD DEPTH", 0, 99, 99, N, false }, { "SPLIT POINT", 0, 18, 3, N, false }, { "DRY : FX", 0, 100, 75, B, false } } },
        { "SYMPHONIC ENSEMBLE", "SYMPHONIC ENSEMBLE", 4, { { "MOD DEPTH", 0, 99, 92, N, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false },
                                                          { "DRY : FX", 0, 100, 33, B, true } } },
        { "FLANGER 1", "FLANGER", 7, { { "DELAY TIME", 0, 200, 5, M, false }, { "MOD DEPTH", 0, 99, 50, N, false }, { "MOD SPEED", 1, 99, 20, N, true },
                                      { "RESONANCE", -99, 99, 80, S, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "FLANGER 2", "FLANGER", 7, { { "DELAY TIME", 0, 200, 24, M, false }, { "MOD DEPTH", 0, 99, 99, N, false }, { "MOD SPEED", 1, 99, 42, N, true },
                                      { "RESONANCE", -99, 99, 36, S, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "CROSSOVER FLANGER", "FLANGER", 7, { { "DELAY TIME", 0, 200, 1, M, false }, { "MOD DEPTH", 0, 99, 60, N, false }, { "MOD SPEED", 1, 99, 22, N, true },
                                              { "RESONANCE", -99, 99, 80, S, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "EXCITER", "EXCITER", 5, { { "BLEND", -99, 99, 60, S, false }, { "EMPHATIC POINT", 1, 10, 1, N, false }, { "EQ LOW", -12, 12, 3, D, false },
                                    { "EQ HIGH", -12, 12, 3, D, false }, { "DRY : FX", 0, 100, 50, B, true } } },
        { "ENHANCER", "ENHANCER", 7, { { "HARMONIC DENSITY", 1, 99, 28, N, false }, { "HOT SPOT", 1, 20, 3, N, false }, { "STEREO WIDTH", 0, 99, 85, N, false },
                                      { "DELAY TIME", 1, 99, 25, M, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, true } } },
        { "DISTORTION", "DISTORTION", 7, { { "DRIVE", 1, 111, 107, N, false }, { "HOT SPOT", 0, 99, 99, N, true }, { "RESONANCE", 0, 99, 7, N, false },
                                          { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "OUT LEVEL", 0, 99, 6, N, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "OVERDRIVE", "DISTORTION", 7, { { "DRIVE", 1, 111, 85, N, false }, { "HOT SPOT", 0, 99, 70, N, true }, { "RESONANCE", 0, 99, 63, N, false },
                                         { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "OUT LEVEL", 0, 99, 8, N, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "STEREO PHASER 1", "PHASER", 6, { { "MANUAL", 0, 99, 98, N, false }, { "MOD SPEED", 0, 99, 30, V, true }, { "MOD DEPTH", 0, 99, 90, N, false },
                                           { "FEEDBACK", -99, 99, 96, S, false }, { "MOD WAVEFORM", 0, 1, 1, W, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "STEREO PHASER 2", "PHASER", 6, { { "MANUAL", 0, 99, 96, N, false }, { "MOD SPEED", 0, 99, 30, V, true }, { "MOD DEPTH", 0, 99, 90, N, false },
                                           { "FEEDBACK", -99, 99, 90, S, false }, { "MOD WAVEFORM", 0, 1, 0, W, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "ROTARY SPEAKER", "ROTARY SPEAKER", 5, { { "VIBRATO DEPTH", 0, 15, 2, N, false }, { "ACCELERATION", 1, 15, 12, N, false }, { "SLOW SPEED", 1, 99, 25, N, false },
                                                  { "FAST SPEED", 1, 99, 69, N, false }, { "DRY : FX", 0, 100, 66, B, false } } },
        { "AUTO PAN", "TREMOLO", 7, { { "MOD WAVEFORM", 0, 1, 1, W, false }, { "MOD SHAPE", -99, 99, 96, S, false }, { "MOD SPEED", 0, 99, 28, V, false },
                                     { "MOD DEPTH", 0, 99, 96, N, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 80, B, true } } },
        { "TREMOLO", "TREMOLO", 7, { { "MOD WAVEFORM", 0, 1, 1, W, false }, { "MOD SHAPE", -99, 99, -99, S, false }, { "MOD SPEED", 0, 99, 70, V, false },
                                    { "MOD DEPTH", 0, 99, 99, N, false }, { "EQ LOW", -12, 12, 0, D, false }, { "EQ HIGH", -12, 12, 0, D, false }, { "DRY : FX", 0, 100, 50, B, true } } },
        { "PARAMETRIC EQ", "PARAMETRIC EQ", 8, { { "LOW FREQ", 0, 29, 15, I, false }, { "LOW GAIN", -12, 12, 6, D, false }, { "MID FREQ", 0, 99, 50, I, true },
                                                { "MID GAIN", -12, 12, 6, D, false }, { "MID WIDTH", 0, 99, 50, N, false }, { "HIGH FREQ", 0, 29, 12, I, false },
                                                { "HIGH GAIN", -12, 12, 6, D, false }, { "DRY : FX", 0, 100, 50, B, false } } },
        { "CHORUS-DELAY", "COMBINATION EFFECT SERIAL", 7, { { "CHO DELAY", 0, 50, 24, M, false }, { "CHO FEEDBACK", -99, 99, 24, S, false }, { "MOD SPEED", 1, 99, 12, N, false },
                                                           { "MOD DEPTH", 0, 99, 75, N, false }, { "DELAY TIME", 0, 450, 120, M, false }, { "DELAY FEEDBACK", -99, 99, 16, S, false },
                                                           { "DRY : FX", 0, 100, 40, B, true } } },
        { "FLANGER-DELAY", "COMBINATION EFFECT SERIAL", 7, { { "FLG DELAY", 0, 50, 1, M, false }, { "FLG FEEDBACK", -99, 99, 80, S, false }, { "MOD SPEED", 1, 99, 4, N, false },
                                                            { "MOD DEPTH", 0, 99, 99, N, false }, { "DELAY TIME", 0, 450, 300, M, false }, { "DELAY FEEDBACK", -99, 99, 30, S, false },
                                                            { "DRY : FX", 0, 100, 50, B, true } } },
        { "DELAY / HALL REVERB", "COMBINATION EFFECT PARALLEL", 8, { { "DELAY TIME", 0, 500, 30, M, false }, { "FEEDBACK", -99, 99, 0, S, false }, { "HIGH DAMP", 0, 99, 0, N, false },
                                                                    { "DELAY DRY : FX", 0, 100, 100, B, true }, { "REVERB TIME", 2, 99, 30, T, false }, { "PRE DELAY", 0, 150, 68, M, false },
                                                                    { "REVERB HIGH DAMP", 0, 99, 34, N, false }, { "REVERB DRY : FX", 0, 100, 30, B, true } } },
        { "DELAY / ROOM REVERB", "COMBINATION EFFECT PARALLEL", 8, { { "DELAY TIME", 0, 500, 20, M, false }, { "FEEDBACK", -99, 99, 0, S, false }, { "HIGH DAMP", 0, 99, 0, N, false },
                                                                    { "DELAY DRY : FX", 0, 100, 100, B, true }, { "REVERB TIME", 2, 49, 11, T, false }, { "PRE DELAY", 0, 150, 0, M, false },
                                                                    { "REVERB HIGH DAMP", 0, 99, 28, N, false }, { "REVERB DRY : FX", 0, 100, 35, B, true } } },
        { "DELAY / CHORUS", "COMBINATION EFFECT PARALLEL", 8, { { "DELAY TIME", 0, 500, 220, M, false }, { "FEEDBACK", -99, 99, 15, S, false }, { "HIGH DAMP", 0, 99, 50, N, false },
                                                               { "DELAY DRY : FX", 0, 100, 30, B, true }, { "MOD SPEED", 0, 99, 37, V, false }, { "MOD DEPTH", 0, 99, 99, N, false },
                                                               { "MOD WAVEFORM", 0, 1, 1, W, false }, { "CHORUS DRY : FX", 0, 100, 50, B, true } } },
        { "DELAY / FLANGER", "COMBINATION EFFECT PARALLEL", 8, { { "DELAY TIME", 0, 500, 400, M, false }, { "FEEDBACK", -99, 99, 20, S, false }, { "HIGH DAMP", 0, 99, 60, N, false },
                                                                { "DELAY DRY : FX", 0, 100, 30, B, true }, { "MOD SPEED", 0, 99, 28, V, false }, { "MOD DEPTH", 0, 99, 96, N, false },
                                                                { "FEEDBACK", -99, 99, -75, S, false }, { "FLANGER DRY : FX", 0, 100, 50, B, true } } },
        { "DELAY / DISTORTION", "COMBINATION EFFECT PARALLEL", 7, { { "DELAY TIME", 0, 500, 250, M, false }, { "FEEDBACK", -99, 99, 40, S, false },
                                                                   { "DELAY DRY : FX", 0, 100, 21, B, false }, { "DRIVE", 1, 111, 105, N, false }, { "HOT SPOT", 1, 99, 99, N, false },
                                                                   { "RESONANCE", 0, 99, 7, N, false }, { "OUT LEVEL", 1, 99, 10, N, false } } },
        { "DELAY / OVERDRIVE", "COMBINATION EFFECT PARALLEL", 7, { { "DELAY TIME", 0, 500, 350, M, false }, { "FEEDBACK", -99, 99, 50, S, false },
                                                                  { "DELAY DRY : FX", 0, 100, 25, B, false }, { "DRIVE", 1, 111, 65, N, false }, { "HOT SPOT", 1, 99, 90, N, false },
                                                                  { "RESONANCE", 0, 99, 63, N, false }, { "OUT LEVEL", 1, 99, 20, N, false } } },
        { "DELAY / PHASER", "COMBINATION EFFECT PARALLEL", 8, { { "DELAY TIME", 0, 500, 300, M, false }, { "FEEDBACK", -99, 99, 15, S, false }, { "HIGH DAMP", 0, 99, 60, N, false },
                                                               { "DELAY DRY : FX", 0, 100, 40, B, true }, { "MOD SPEED", 0, 99, 45, V, false }, { "MOD DEPTH", 0, 99, 90, N, false },
                                                               { "FEEDBACK", -99, 99, 99, S, false }, { "PHASER DRY : FX", 0, 100, 75, B, true } } },
        { "DELAY / ROTARY SPEAKER", "COMBINATION EFFECT PARALLEL", 7, { { "DELAY TIME", 0, 500, 280, M, false }, { "FEEDBACK", -99, 99, 15, S, false },
                                                                       { "DELAY DRY : FX", 0, 100, 30, B, false }, { "ACCELERATION", 1, 15, 10, N, false }, { "SLOW SPEED", 1, 99, 25, N, false },
                                                                       { "FAST SPEED", 1, 99, 69, N, false }, { "ROTARY DRY : FX", 0, 100, 70, B, false } } },
        { "RESONANCE FILTER", "RESONANCE FILTER", 8, { { "TRIM", 0, 99, 70, N, false }, { "RESONANCE", 0, 99, 90, N, true }, { "FC", 0, 99, 0, N, false },
                                                      { "EG INT", -99, 99, 99, S, false }, { "ATTACK TIME", 0, 127, 20, N, false }, { "DECAY TIME", 0, 127, 50, N, false },
                                                      { "TRIGGER", 0, 2, 1, G, false }, { "LFO INT", 0, 127, 0, N, false } } },
    };
#undef REVERB
#undef PLATE
#undef ER
}

const Field& layerField (int i) { return layerFields[std::clamp (i, 0, numLayer - 1)]; }
const Field& globalField (int i) { return globalFields[std::clamp (i, 0, numGlobal - 1)]; }
const Field& timbreField (int i) { return timbreFields[std::clamp (i, 0, numTimbre - 1)]; }
const Field& arpField (int i) { return arpFields[std::clamp (i, 0, numArp - 1)]; }
const char* arpTypeName (int t) { return arpNames[std::clamp (t, 1, 20) - 1]; }
const EffectType& effectType (int t) { return effectTypes[std::clamp (t, 0, numEffectTypes - 1)]; }

double speedHz (int i) { return 0.03 * std::pow (1000.0, std::clamp (i, 0, 99) / 99.0); }

juce::String noteName (int note)
{
    static const char* n[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (n[(note % 12 + 12) % 12]) + juce::String (note / 12 - 1);
}

juce::String effectValueText (const EffectParam& p, int v)
{
    switch (p.kind)
    {
        case EffectParam::signed_: return (v > 0 ? "+" : "") + juce::String (v);
        case EffectParam::tenthSec: return juce::String (v / 10.0, 1) + " S";
        case EffectParam::ms: return juce::String (v) + " MS";
        case EffectParam::speed: { const double hz = speedHz (v); return juce::String (hz, hz < 1.0 ? 2 : 1) + " HZ"; }
        case EffectParam::wave2: return v == 0 ? "SIN" : "TRI";
        case EffectParam::shape: return v <= 20 ? "T" + juce::String (10 - v >= 0 ? "+" : "") + juce::String (10 - v) : "S" + juce::String (v - 31 >= 0 ? "+" : "") + juce::String (v - 31);
        case EffectParam::balance: return v <= 0 ? juce::String ("DRY") : v >= 100 ? juce::String ("FX") : juce::String (100 - v).paddedLeft ('0', 2) + ":" + juce::String (v).paddedLeft ('0', 2);
        case EffectParam::db: return (v > 0 ? "+" : "") + juce::String (v) + " DB";
        case EffectParam::trigger: { static const char* t[] { "SINGLE", "MULTI 1", "MULTI 2" }; return t[std::clamp (v, 0, 2)]; }
        case EffectParam::num: case EffectParam::index: default: return juce::String (v);
    }
}

void FxSettings::setType (int u, int t)
{
    auto& unitRef = unit[std::clamp (u, 0, 1)];
    unitRef.type = std::clamp (t, 0, numEffectTypes - 1);
    const auto& et = effectType (unitRef.type);
    for (int i = 0; i < maxEffectParams; ++i) unitRef.p[i] = i < et.count ? et.params[i].def : 0;
}

void ProgramData::init()
{
    for (auto& l : layer) for (int i = 0; i < numLayer; ++i) l[i] = layerFields[i].def;
    for (int i = 0; i < numGlobal; ++i) global[i] = globalFields[i].def;
    fx = {};
    fx.placement = 1;
    fx.setType (0, 1);    // 01 Hall and 19 Chorus 1: what the N5 sets after GM ON (the manual, p.45)
    fx.setType (1, 19);
}

void CombiData::init()
{
    for (int t = 0; t < numTimbres; ++t) for (int i = 0; i < numTimbre; ++i) timbre[t][i] = timbreFields[i].def;
    timbre[0][tProgram] = 0;
    fx = {};
    fx.placement = 1;
    fx.setType (0, 1);
    fx.setType (1, 19);
}

void BankData::init()
{
    for (auto& p : programs) p.init();
    for (auto& c : combis) c.init();
    for (int i = 0; i < numArp; ++i) arp[i] = arpFields[i].def;
    combiMode = 0; program = 0; combi = 0;
}

// ---- the numbers' meanings: the manual doesn't give them; these curves are set by ear ----
double egSeconds (int v) { return v <= 0 ? 0.0 : 0.002 * std::pow (10000.0, v / 127.0); }   // 0, 2 ms .. 20 s
double lfoHz (int v) { return 0.03 * std::pow (1000.0, std::clamp (v, 0, 127) / 127.0); }   // 0.03 .. 30 Hz, as the effects' speeds
double cutoffHz (double v) { return 16.0 * std::pow (2.0, std::clamp (v, -20.0, 147.0) / 127.0 * 10.3); }   // 16 Hz .. 20 kHz

juce::String fieldText (const Field& f, int v)
{
    switch (f.fmt)
    {
        case Fmt::signed_: return (v > 0 ? "+" : "") + juce::String (v);
        case Fmt::choice:
        {
            const auto labels = juce::StringArray::fromTokens (f.labels, "|", "");
            return labels[std::clamp (v - f.lo, 0, labels.size() - 1)];
        }
        case Fmt::note: return noteName (v);
        case Fmt::pan: return v <= 0 ? juce::String ("RND") : v >= 128 ? juce::String ("OFF") : v == 64 ? juce::String ("CNT")
                            : v < 64 ? "L" + juce::String (64 - v) : "R" + juce::String (v - 64);
        case Fmt::slope: return (v > 0 ? "+" : "") + juce::String (v / 10.0, 1);
        case Fmt::egTime:
        {
            const double s = egSeconds (v);
            return juce::String (v).paddedLeft ('0', 3) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "))
                 + (s < 1.0 ? juce::String (juce::roundToInt (s * 1000.0)) + " MS" : juce::String (s, 1) + " S");
        }
        case Fmt::lfoFreq: return juce::String (v).paddedLeft ('0', 3) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + juce::String (lfoHz (v), lfoHz (v) < 1.0 ? 2 : 1) + " HZ";
        case Fmt::cutoff:
        {
            const double hz = cutoffHz (v);
            return juce::String (v).paddedLeft ('0', 3) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + (hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " HZ" : juce::String (hz / 1000.0, 1) + " KHZ");
        }
        case Fmt::quad: { static const char* q[] { "OFF", "+", "-" }; return q[std::clamp (v, 0, 2)]; }
        case Fmt::octave: { static const char* o[] { "32'", "16'", "8'", "4'" }; return o[std::clamp (v, 0, 3)]; }
        case Fmt::cents: return (v > 0 ? "+" : "") + juce::String (v) + " C";
        case Fmt::arpVelocity: return v <= 0 ? juce::String ("KEY") : v >= 128 ? juce::String ("STEP") : juce::String (v).paddedLeft ('0', 3);
        case Fmt::arpGate: return v >= 101 ? juce::String ("STEP") : juce::String (v).paddedLeft ('0', 3) + " %";
        case Fmt::panOut: return v < 0 ? juce::String ("OFF") : v == 0 ? juce::String ("L") : v >= 100 ? juce::String ("R") : juce::String (100 - v).paddedLeft ('0', 2) + ":" + juce::String (v).paddedLeft ('0', 2);
        case Fmt::plain: default: return juce::String (v).paddedLeft ('0', f.hi >= 100 ? 3 : 2);
    }
}
} // namespace tf::n5
