#pragma once

#include "N5Model.h"
#include "SpringReverb.h"

/** TF N5's two effects units and their placements (the manual's Effect Edit mode). */
namespace tf::n5
{
/** What an effect can follow while playing: the dynamic modulation source, note-ons (the resonance
    filter's EG) and the first oscillator's VDF LFO speed (its LFO). */
struct FxContext
{
    float mod[6] {};          // NONE, MOD1 (CC1), MOD2 (CC16), MOD3 (CC17), AFTR.T, VDA-EG: 0..1
    int noteOns = 0;          // in this block
    bool firstNoteOn = false; // from no notes held
    int held = 0;
    double lfoHz = 1.0;
};

class EffectUnit
{
public:
    EffectUnit();
    ~EffectUnit();
    void prepare (double sampleRate);
    void reset();
    /** Processes in place (stereo). */
    void process (float* l, float* r, int n, const FxSettings::Unit&, const FxContext&) noexcept;

    struct State;

private:
    std::unique_ptr<State> st;
    int lastType = -1;
};

class EffectRack
{
public:
    void prepare (double sampleRate, int maxBlock);
    void reset();
    /** The four inputs (A/B: the dry, panned sound; C and D: the sends), to the stereo output. */
    void process (const float* const* ab, const float* const* c, const float* const* d, float* outL, float* outR, int n,
                  const FxSettings&, const FxContext&) noexcept;

private:
    EffectUnit units[2];
    juce::AudioBuffer<float> work1, work2;
};
} // namespace tf::n5
