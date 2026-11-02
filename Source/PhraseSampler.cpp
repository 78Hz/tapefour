#include "PhraseSampler.h"
#include "PhraseSamplerFront.h"

namespace tf::sampler
{
namespace
{
    constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;

    // The four clocks at pitch 0 %. HI at +10 % is a true 44.1 kHz (the owner's manual); the others are
    // in the ratios of the published recording times (19, 27, 38, 54 s): 1, 8/11, 1/2, 4/11.
    constexpr double hiHz = 44100.0 / 1.1;
    constexpr double baseHz[4] { hiHz, hiHz * 8.0 / 11.0, hiHz / 2.0, hiHz * 4.0 / 11.0 };

    constexpr double scratchWindowSeconds = 0.6;   // the SCRATCH window (not published: set by ear)
    constexpr double extScratchSeconds = 1.5;      // EXTERNAL SCRATCH's buffer (not published: set by ear)

    // Yamaha's ADPCM step multipliers (x/256), as its sample chips document them
    constexpr int stepScale[8] { 230, 230, 230, 230, 307, 409, 512, 614 };

    /** A second-order section (RBJ), for the anti-alias and reconstruction filters. */
    struct Section
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void lowPass (double f, double q, double sr)
        {
            const double w = twoPi * std::min (f, sr * 0.49) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q), a0 = 1 + al;
            b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
        double process (double x) noexcept { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void reset() { z1 = z2 = 0; }
    };

    /** Fourth-order Butterworth low-pass: two sections. */
    struct LowPass4
    {
        Section s1, s2;
        void set (double f, double sr) { s1.lowPass (f, 0.5412, sr); s2.lowPass (f, 1.3066, sr); }
        double process (double x) noexcept { return s2.process (s1.process (x)); }
        void reset() { s1.reset(); s2.reset(); }
    };

    /** The ribbon's filter: two state-variable sections in a row (24 dB an octave), low, band or high pass. */
    struct RibbonFilter
    {
        double ic1[2] {}, ic2[2] {}, jc1[2] {}, jc2[2] {};
        double g = 0.1, k = 1.4, a1 = 0, a2 = 0, a3 = 0;
        void set (double hz, double q, double sr)
        {
            g = std::tan (pi * std::clamp (hz, 20.0, sr * 0.45) / sr);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
        }
        double stage (double x, double& s1, double& s2, FilterType t) noexcept
        {
            const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
            s1 = 2 * v1 - s1; s2 = 2 * v2 - s2;
            switch (t)
            {
                case FilterType::lpf: return v2;
                case FilterType::bpf: return v1 * k;
                case FilterType::hpf: return x - k * v1 - v2;
                case FilterType::bypass: default: return x;
            }
        }
        double process (double x, int ch, FilterType t) noexcept
        {
            if (t == FilterType::bypass) return x;
            return stage (stage (x, ic1[ch], ic2[ch], t), jc1[ch], jc2[ch], t);
        }
    };

    /** The input gain, 1..16: 3 dB a step, 8 at unity (the steps aren't published: set by ear). */
    double gainOf (int step) { return std::pow (10.0, (step - 8) * 3.0 / 20.0); }

    int16_t toInt16 (double x) { return (int16_t) std::clamp ((int) std::lround (x * 32767.0), -32768, 32767); }

    enum Key
    {
        kShift, kScratch, kHold, kRev, kBankA, kBankB, kBankC, kBankD, kScale, kFilter, kCrossfader, kPitch, kExtScratch,
        kExtFilter, kExit, kRec, kTop, kBwd, kStop, kPlay, kFwd, kSong1, kSong2, kSong3, kSong4, kPlus, kMinus, numKeys
    };
    const char* const keyNames[] { "SHIFT", "SCRATCH", "HOLD", "REV", "BANK A", "BANK B", "BANK C", "BANK D", "SCALE", "FILTER",
                                   "CROSSFADER", "PITCH", "EXT SCRATCH", "EXT FILTER", "EXIT", "REC", "TOP", "BWD", "STOP", "PLAY",
                                   "FWD", "SONG 1", "SONG 2", "SONG 3", "SONG 4", "+1", "-1" };

    struct Command
    {
        enum Type : uint8_t { padDown, padUp, anyPadDown, anyPadUp, keyDown, keyUp, ribbon, armRecord, stopRecord, allOff } type = padDown;
        int a = 0;
        float f = 0.0f;
        double ms = 0.0;   // when it was pressed: a press the audio didn't take in time is dropped, not played late
    };

    enum class Message { none, padEmpty, notAtGrade, memoryFull, songFull, songNotEmpty, recStandby, recording, selectPad, padNotEmpty };
}

const char* gradeName (Grade g) { static const char* n[] { "HI", "STD", "LONG", "EXLG" }; return n[(int) g]; }
double clockHz (Grade g, double pitch) { return baseHz[(int) g] * (1.0 + pitch / 100.0); }
double memorySeconds (Grade g) { return memoryNibbles / baseHz[(int) g]; }
const char* loopName (Loop l) { static const char* n[] { "ONE SHOT", "WHOLE LOOP", "PARTIAL LOOP", "REVERSE SHOT", "REVERSE LOOP" }; return n[(int) l]; }
const char* soundingName (Sounding s) { static const char* n[] { "NOTE", "TOGGLE", "TRIGGER" }; return n[(int) s]; }
const char* effectName (Effect e) { static const char* n[] { "", "SCALE", "FILTER", "CROSSFADE", "PITCH", "EXT SCRATCH", "EXT FILTER" }; return n[(int) e]; }

int polyphony (Grade g, Effect e, bool scratching)
{
    // the owner's manual's table (mono voices; a stereo sample takes two). The external modes aren't in it: as normal.
    const int gi = (int) g;
    if (scratching) { static const int t[4] { 0, 2, 2, 2 }; return t[gi]; }
    switch (e)
    {
        case Effect::scale:     { static const int t[4] { 0, 0, 2, 2 }; return t[gi]; }
        case Effect::filter:    { static const int t[4] { 0, 0, 2, 2 }; return t[gi]; }
        case Effect::crossfade: { static const int t[4] { 0, 2, 4, 4 }; return t[gi]; }
        case Effect::pitch:     { static const int t[4] { 0, 2, 4, 4 }; return t[gi]; }
        case Effect::none: case Effect::extScratch: case Effect::extFilter: default:
                                { static const int t[4] { 2, 4, 4, 4 }; return t[gi]; }
    }
}

bool effectAllowed (Grade g, Effect e, bool scratching) { return polyphony (g, e, scratching) > 0; }

int Adpcm::decode (int nibble) noexcept
{
    int d = ((2 * (nibble & 7) + 1) * step) >> 3;
    if (nibble & 8) d = -d;
    signal = std::clamp (signal + d, -32768, 32767);
    step = std::clamp ((step * stepScale[nibble & 7]) >> 8, 127, 24576);
    return signal;
}

int Adpcm::encode (int sample) noexcept
{
    int diff = sample - signal, n = 0;
    if (diff < 0) { n = 8; diff = -diff; }
    n |= std::min (7, (diff * 4) / step);
    decode (n);
    return n;
}

//==============================================================================
struct Processor::Impl : private juce::Timer
{
    Processor& owner;
    explicit Impl (Processor& p) : owner (p), memory ((size_t) memoryNibbles, 0)
    {
        startTimerHz (20);
    }
    ~Impl() override { stopTimer(); }

    // parameters
    juce::AudioParameterFloat* ribbonP {};
    juce::AudioParameterChoice *gradeP {}, *filterTypeP {}, *resonanceP {}, *ribbonSourceP {};
    juce::AudioParameterInt *pitchP {}, *volumeP {}, *channelP {}, *ribbonCcP {};

    // ---- what's kept (message thread writes under dataLock; the audio thread reads it under a try-lock) ----
    juce::SpinLock dataLock;
    std::vector<uint8_t> memory;                  // one nibble a byte
    int used = 0;                                 // nibbles
    std::array<Pad, numPads> pads {};
    std::array<std::vector<int16_t>, numPads> decoded {};   // each pad's sample, as the ADPCM gives it back
    std::array<std::vector<SongEvent>, numSongs> songs {};
    std::array<std::atomic<int>, numSongs> songCount {};
    std::array<std::atomic<uint32_t>, numSongs> songLastMs {};
    void noteSongs() noexcept
    {
        for (size_t i = 0; i < (size_t) numSongs; ++i)
        {
            songCount[i] = (int) songs[i].size();
            songLastMs[i] = songs[i].empty() ? 0u : songs[i].back().ms;
        }
    }
    Processor::RecordSettings record;

    // ---- commands from the panel ----
    juce::AbstractFifo fifo { 512 };
    std::array<Command, 512> commands {};
    void push (Command c)
    {
        c.ms = juce::Time::getMillisecondCounterHiRes();
        const auto scope = fifo.write (1);
        if (scope.blockSize1 > 0) commands[(size_t) scope.startIndex1] = c;
        else if (scope.blockSize2 > 0) commands[(size_t) scope.startIndex2] = c;
    }

    // ---- the panel's state, for the editor ----
    juce::SpinLock panelLock;
    PanelState shown;
    std::atomic<int> message { 0 };
    std::atomic<double> messageUntil { 0.0 };
    std::atomic<float> ribbonShown { 0.5f };

    // ---- audio thread ----
    double sr = 48000.0;
    int64_t now = 0;
    struct Voice
    {
        bool on = false;
        int pad = -1;
        double pos = 0.0, speed = 1.0;
        int dir = 1;
        bool looping = false, latched = false, keyDown = false, fromSong = false, stopping = false;
        Loop loop = Loop::oneShot;
        float gain = 1.0f, fade = 1.0f;
        int64_t age = 0;
    };
    std::array<Voice, 8> voices {};
    int bank = 0, lastPad = -1;
    bool shift = false, holdKey = false, revKey = false, scratchKey = false;
    Effect effect = Effect::none;
    int scalePad = -1, scratchPad = -1;
    bool selectingScale = false;
    double scratchStart = 0.0, scratchPos = 0.0;
    float scratchFade = 0.0f;
    float ribbon = 0.5f, lastRibbonParam = 0.5f;
    bool ribbonTouched = false, ribbonUsed = false;
    double pitchFromRibbon = 0.0;
    RibbonFilter filter;
    Section recon[2];
    double reconHz = 0.0;
    double dcIn[2] {}, dcOut[2] {};
    // EXTERNAL SCRATCH
    std::vector<float> extBuf[2];
    int extWrite = 0;
    double extAnchor = 0.0, extPos = 0.0;
    bool extWasTouched = false;
    // the other tracks
    TrackFeed feed;
    bool haveFeed = false;
    std::atomic<int> ownTrack { -1 };
    std::atomic<bool> repeatSetting { false };
    std::atomic<double> lastMidiMs { 0.0 };

    // recording
    enum class Rec { idle, armed, recording, done };
    std::atomic<Rec> recState { Rec::idle };
    std::vector<int16_t> recBuf;                  // at the clock, interleaved if stereo
    int recFrames = 0, recCapacity = 0;
    double recPhase = 0.0, recClock = 0.0;
    LowPass4 recAA[2];
    double recPrev[2] {};
    std::atomic<bool> recClipped { false };
    std::atomic<float> inputPeak { 0.0f };
    Processor::RecordSettings recNow;
    Grade recGrade = Grade::lng;
    double recPitch = 0.0;

    // songs
    int song = -1;                                // the SONG key lit (-1 none)
    bool songPlaying = false, songRecording = false, songRepeat = false;
    int64_t songStart = 0;
    size_t songIndex = 0;
    double songPosMs = 0.0;
    bool songStarted = false;   // recording: the first event starts the clock

    void showMessage (Message m) { message = (int) m; messageUntil = juce::Time::getMillisecondCounterHiRes() + 1500.0; }

    Grade grade() const { return (Grade) gradeP->getIndex(); }
    double pitch() const { return effect == Effect::pitch && ribbonUsed ? pitchFromRibbon : (double) pitchP->get(); }

    // ---- voices ----
    int voicesInUse() const
    {
        int n = 0;
        for (auto& v : voices) if (v.on) n += pads[(size_t) v.pad].stereo ? 2 : 1;
        return n;
    }

    void stopVoice (Voice& v, bool fadeOut = true) { if (fadeOut) v.stopping = true; else v.on = false; }

    void startPad (int padIndex, float velocity, bool fromSong, bool reverse, bool hold)
    {
        if (padIndex < 0 || padIndex >= numPads) return;
        lastPad = padIndex;
        if (selectingScale) { scalePad = padIndex; selectingScale = false; return; }
        if ((effect == Effect::extFilter || (effect == Effect::filter && shift)) && ! fromSong)
        {
            // the pads choose the filter here, as on the original: 1-4 its type, 7-9 its resonance
            const int k = padIndex % padsPerBank;
            if (k <= 3) { filterTypeP->setValueNotifyingHost (filterTypeP->convertTo0to1 ((float) k)); return; }
            if (k >= 6 && k <= 8) { resonanceP->setValueNotifyingHost (resonanceP->convertTo0to1 ((float) (k - 6))); return; }
            if (effect == Effect::extFilter) return;
        }
        if (scratchKey) { scratchPad = pads[(size_t) padIndex].used ? padIndex : -1; scratchStart = pads[(size_t) padIndex].start; scratchPos = scratchStart; if (scratchPad < 0) showMessage (Message::padEmpty); return; }

        int sourcePad = padIndex;
        double speed = 1.0;
        if (effect == Effect::scale && scalePad >= 0)
        {
            // one sample across the 12 pads: pad 12 its own pitch, pad 1 eleven semitones down; one at a time
            sourcePad = scalePad;
            speed = std::pow (2.0, ((padIndex % padsPerBank) - 11) / 12.0);
            for (auto& v : voices) if (v.on) stopVoice (v);
        }
        const auto& p = pads[(size_t) sourcePad];
        if (! p.used || decoded[(size_t) sourcePad].empty()) { showMessage (Message::padEmpty); return; }
        if (songRecording) recordEvent (padIndex, 0, velocity, reverse, hold);

        // a pad already sounding: TOGGLE stops it, the others start again
        for (auto& v : voices)
            if (v.on && ! v.stopping && v.pad == sourcePad && effect != Effect::scale)
            {
                if (p.sounding == Sounding::toggle && ! v.fromSong) { stopVoice (v); return; }
                stopVoice (v);
            }
        // alternate groups cut each other off
        if (p.group > 0)
            for (auto& v : voices) if (v.on && v.pad != sourcePad && pads[(size_t) v.pad].group == p.group) stopVoice (v);

        // polyphony: the oldest unheld voice goes first
        const int limit = std::max (1, polyphony (grade(), effect, false));
        const int weight = p.stereo ? 2 : 1;
        while (voicesInUse() + weight > limit)
        {
            Voice* oldest = nullptr;
            for (auto& v : voices) if (v.on && ! v.latched && (oldest == nullptr || v.age < oldest->age)) oldest = &v;
            if (oldest == nullptr) for (auto& v : voices) if (v.on && (oldest == nullptr || v.age < oldest->age)) oldest = &v;
            if (oldest == nullptr) break;
            oldest->on = false;
        }
        Voice* slot = nullptr;
        for (auto& v : voices) if (! v.on) { slot = &v; break; }
        if (slot == nullptr) return;
        auto& v = *slot;
        v = Voice();
        v.on = true;
        v.pad = sourcePad;
        v.speed = speed;
        v.fromSong = fromSong;
        v.keyDown = true;
        v.latched = hold && p.sounding == Sounding::note;
        v.age = now;
        v.gain = (float) (p.volume / 127.0) * velocity;
        auto loop = p.sounding == Sounding::trigger ? (p.loop == Loop::reverseShot || p.loop == Loop::reverseLoop ? Loop::reverseShot : Loop::oneShot) : p.loop;
        // REV: forward modes play backwards, reverse modes forwards (not in SCALE)
        if (reverse && effect != Effect::scale)
        {
            static const Loop flipped[] { Loop::reverseShot, Loop::reverseLoop, Loop::reverseLoop, Loop::oneShot, Loop::wholeLoop };
            loop = flipped[(int) loop];
        }
        if (effect == Effect::scale && (loop == Loop::reverseShot || loop == Loop::reverseLoop))
            loop = loop == Loop::reverseShot ? Loop::oneShot : Loop::wholeLoop;
        v.loop = loop;
        const bool backwards = loop == Loop::reverseShot || loop == Loop::reverseLoop;
        v.dir = backwards ? -1 : 1;
        v.pos = backwards ? (double) p.end - 1.0 : (double) p.start;
    }

    void releasePad (int padIndex, bool fromSong)
    {
        if (padIndex < 0 || padIndex >= numPads) return;
        if (songRecording && ! fromSong) recordEvent (padIndex, 1, 0.0f, false, false);
        const int sourcePad = effect == Effect::scale && scalePad >= 0 ? scalePad : padIndex;
        for (auto& v : voices)
            if (v.on && v.pad == sourcePad && v.keyDown && v.fromSong == fromSong)
            {
                v.keyDown = false;
                if (pads[(size_t) sourcePad].sounding == Sounding::note && ! v.latched) stopVoice (v);
            }
    }

    // ---- songs ----
    void recordEvent (int padIndex, int kind, float velocity, bool reverse, bool hold)
    {
        int total = 0;
        for (auto& s : songs) total += (int) s.size();
        if (total >= songEventLimit) { songRecording = false; showMessage (Message::songFull); return; }
        if (! songStarted) { songStarted = true; songStart = now; }
        SongEvent e;
        e.ms = (uint32_t) ((now - songStart) * 1000 / (int64_t) sr);
        e.pad = (uint8_t) padIndex;
        e.kind = (uint8_t) kind;
        e.velocity = (uint8_t) juce::jlimit (1, 127, (int) (velocity * 127.0f));
        e.flags = (uint8_t) ((reverse ? 1 : 0) | (hold ? 2 : 0));
        // (reserved while recording: the vector has room for the whole song memory)
        if (songs[(size_t) song].size() < songs[(size_t) song].capacity()) songs[(size_t) song].push_back (e);
        noteSongs();
    }

    void songTick()
    {
        if (! songPlaying || song < 0) return;
        const auto& ev = songs[(size_t) song];
        songPosMs += 1000.0 / sr;
        while (songIndex < ev.size() && ev[songIndex].ms <= songPosMs)
        {
            const auto& e = ev[songIndex++];
            if (e.kind == 0) startPad (e.pad, e.velocity / 127.0f, true, (e.flags & 1) != 0, (e.flags & 2) != 0);
            else releasePad (e.pad, true);
        }
        if (songIndex >= ev.size())
        {
            if (songRepeat && ! ev.empty()) { songIndex = 0; songPosMs = 0.0; }
            else songPlaying = false;
        }
    }

    // ---- the keys ----
    void keyDown (int k)
    {
        auto setEffect = [this] (Effect e)
        {
            if (! effectAllowed (grade(), e, false)) { showMessage (Message::notAtGrade); return; }
            effect = effect == e ? Effect::none : e;
            ribbonUsed = false;
            selectingScale = effect == Effect::scale;
            if (effect == Effect::scale) showMessage (Message::selectPad);
        };
        switch (k)
        {
            case kShift: shift = true; break;
            case kScratch:
                if (scratchPad >= 0) { scratchPad = -1; break; }   // again: out of SCRATCH
                if (! effectAllowed (grade(), Effect::none, true)) { showMessage (Message::notAtGrade); break; }
                scratchKey = true; break;
            case kHold:
                holdKey = ! holdKey;
                if (! holdKey) for (auto& v : voices) if (v.on && v.latched) { v.latched = false; if (! v.keyDown) stopVoice (v); }
                break;
            case kRev: revKey = true; break;
            case kBankA: case kBankB: case kBankC: case kBankD: bank = k - kBankA; break;
            case kScale: setEffect (Effect::scale); break;
            case kFilter: setEffect (Effect::filter); break;
            case kCrossfader: setEffect (Effect::crossfade); break;
            case kPitch: setEffect (Effect::pitch); break;
            case kExtScratch: setEffect (Effect::extScratch); break;
            case kExtFilter: setEffect (Effect::extFilter); break;
            case kExit: effect = Effect::none; ribbonUsed = false; selectingScale = false; scratchPad = -1; break;
            case kRec:
                if (song >= 0)
                {
                    if (! songs[(size_t) song].empty()) { showMessage (Message::songNotEmpty); break; }
                    songRecording = true; songStarted = false; songPlaying = false;
                }
                else armRecording();
                break;
            case kStop:
                if (recState == Rec::armed || recState == Rec::recording) { recState = Rec::done; break; }
                songPlaying = false;
                songRecording = false;
                for (auto& v : voices) if (v.fromSong) stopVoice (v);
                break;
            case kPlay:
                // from where it stopped, or from the top once it's played through (the manual, p. 28)
                if (song >= 0 && ! songs[(size_t) song].empty())
                {
                    if (songIndex >= songs[(size_t) song].size()) { songIndex = 0; songPosMs = 0.0; }
                    songPlaying = true; songRepeat = holdKey || repeatSetting.load();
                }
                break;
            case kTop: songPosMs = 0.0; songIndex = 0; if (scratchPad >= 0) scratchStart = pads[(size_t) scratchPad].start; break;
            case kFwd: case kBwd:
            {
                const double dir = k == kFwd ? 1.0 : -1.0;
                if (scratchPad >= 0)
                {
                    const auto& p = pads[(size_t) scratchPad];
                    const double win = scratchWindowSeconds * p.recordedHz();
                    scratchStart = std::clamp (scratchStart + dir * win * 0.25, (double) p.start, std::max ((double) p.start, (double) p.end - win));
                }
                else if (! songPlaying && song >= 0)
                {
                    songPosMs = std::max (0.0, songPosMs + dir * 1000.0);
                    songIndex = 0;
                    while (songIndex < songs[(size_t) song].size() && songs[(size_t) song][songIndex].ms < songPosMs) ++songIndex;
                }
                break;
            }
            case kSong1: case kSong2: case kSong3: case kSong4:
                song = song == k - kSong1 ? -1 : k - kSong1;
                songPlaying = songRecording = false;
                break;
            case kPlus: case kMinus:
                volumeStep (k == kPlus ? 1 : -1);
                break;
            default: break;
        }
    }

    void volumeStep (int d)
    {
        // (a parameter change from the audio thread: its listeners are the host's and the editor's)
        volumeP->setValueNotifyingHost (volumeP->convertTo0to1 ((float) juce::jlimit (0, 127, volumeP->get() + d)));
    }

    void keyUp (int k)
    {
        switch (k)
        {
            case kShift: shift = false; break;
            case kScratch: scratchKey = false; break;
            case kRev: revKey = false; break;
            default: break;
        }
    }

    void armRecording()
    {
        if (recState == Rec::armed || recState == Rec::recording) return;
        recNow = record;
        if (pads[(size_t) recNow.toPad].used) { showMessage (Message::padNotEmpty); return; }   // (the editor checks this first)
        recGrade = grade();
        recPitch = (double) pitchP->get();
        recClock = clockHz (recGrade, recPitch);
        const int room = memoryNibbles - used;
        recCapacity = std::min ((int) recBuf.size(), room) / (recNow.stereo ? 2 : 1);
        if (recCapacity < 256) { showMessage (Message::memoryFull); return; }
        recFrames = 0;
        recPhase = 0.0;
        recClipped = false;
        for (auto& f : recAA) { f.set (recClock * 0.45, sr); f.reset(); }
        recPrev[0] = recPrev[1] = 0.0;
        recState = recNow.trigger == 0 ? Rec::recording : Rec::armed;
    }

    // ---- the audio ----
    void render (juce::AudioBuffer<float>& buffer, int n)
    {
        auto* outL = buffer.getWritePointer (0);
        auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
        // the input (the track's audio): what's in the buffer before we write over it
        float inL[512], inR[512];
        const int g = (int) grade();
        const double clock = clockHz ((Grade) g, pitch());
        if (std::abs (clock - reconHz) > 1.0)
        {
            reconHz = clock;
            for (auto& s : recon) s.lowPass (std::min (clock * 0.45, sr * 0.45), 0.707, sr);
        }
        const auto ft = (FilterType) filterTypeP->getIndex();
        static const double qs[3] { 0.6, 2.0, 6.0 };
        const double q = qs[resonanceP->getIndex()];
        const double cutoff = 40.0 * std::pow (400.0, (double) ribbon);   // 40 Hz .. 16 kHz across the ribbon (set by ear)
        filter.set (cutoff, q, sr);
        const float volume = (float) volumeP->get() / 127.0f;
        const double recGain = gainOf (recNow.gain), meterGain = gainOf (record.gain);

        for (int start = 0; start < n; start += 512)
        {
            const int len = std::min (512, n - start);
            for (int i = 0; i < len; ++i)
            {
                inL[i] = outL[start + i];
                inR[i] = outR != nullptr ? outR[start + i] : inL[i];
            }
            for (int i = 0; i < len; ++i)
            {
                songTick();
                // ---- recording: the source, at the clock (and the SAMPLE tab's meter, recording or not) ----
                const auto rs = recState.load();
                const bool taking = rs == Rec::armed || rs == Rec::recording;
                const auto& src = taking ? recNow : record;
                double sl = 0, sr2 = 0;
                const int own = haveFeed ? feed.ownTrack : -1;
                if (src.source == Source::track && src.track != own)
                {
                    const int tt = src.track;
                    const float* t = ! haveFeed || tt >= feed.numTracks ? nullptr : (src.postFader ? feed.postFader[tt] : feed.preFader[tt]);
                    sl = sr2 = t != nullptr ? t[start + i] : 0.0;
                }
                else if (src.source == Source::mix)
                {
                    // the other tracks as the mix has them (mute and solo), in mono
                    if (haveFeed)
                        for (int k = 0; k < feed.numTracks; ++k)
                            if (k != own && feed.heard[k])
                                if (const float* t = src.postFader ? feed.postFader[k] : feed.preFader[k]) sl += t[start + i];
                    sr2 = sl;
                }
                else if (src.source == Source::input || src.source == Source::track) { sl = inL[i]; sr2 = inR[i]; }
                const double gain = taking ? recGain : meterGain;
                sl *= gain; sr2 *= gain;
                const float pk = (float) std::max (std::abs (sl), std::abs (sr2));
                if (pk > inputPeak.load()) inputPeak = pk;
                if (taking)
                {
                    if (pk > 1.0f) recClipped = true;
                    if (rs == Rec::armed)
                    {
                        const double threshold = std::pow (10.0, (-48.0 + 6.0 * recNow.trigger) / 20.0);   // levels 1..7 (set by ear)
                        if (pk >= threshold) recState = Rec::recording;
                    }
                    if (recState.load() == Rec::recording)
                    {
                        const double fl = recAA[0].process (sl), fr = recAA[1].process (recNow.stereo ? sr2 : sl);
                        // one point at each tick of the clock, between this sample and the last
                        recPhase += recClock / sr;
                        while (recPhase >= 1.0 && recFrames < recCapacity)
                        {
                            recPhase -= 1.0;
                            const double t = 1.0 - recPhase * sr / recClock;   // where in this host sample the tick fell
                            const double x0 = recPrev[0] + (fl - recPrev[0]) * std::clamp (t, 0.0, 1.0);
                            const double x1 = recPrev[1] + (fr - recPrev[1]) * std::clamp (t, 0.0, 1.0);
                            if (recNow.stereo) { recBuf[(size_t) recFrames * 2] = toInt16 (x0); recBuf[(size_t) recFrames * 2 + 1] = toInt16 (x1); }
                            else recBuf[(size_t) recFrames] = toInt16 (x0);
                            ++recFrames;
                        }
                        recPrev[0] = fl; recPrev[1] = fr;
                        if (recFrames >= recCapacity) recState = Rec::done;
                    }
                }

                // ---- the voices, at the clock, through the DAC's filter ----
                double l = 0.0, r = 0.0;
                for (auto& v : voices)
                {
                    if (! v.on) continue;
                    const auto& p = pads[(size_t) v.pad];
                    const auto& data = decoded[(size_t) v.pad];
                    if (data.empty()) { v.on = false; continue; }
                    const int ch = p.stereo ? 2 : 1;
                    const int frames = (int) data.size() / ch;
                    const int ip = std::clamp ((int) v.pos, 0, frames - 1), ip2 = std::min (frames - 1, ip + 1);
                    const double fr = v.pos - std::floor (v.pos);
                    const double a = (data[(size_t) (ip * ch)] + (data[(size_t) (ip2 * ch)] - data[(size_t) (ip * ch)]) * fr) / 32768.0;
                    const double b = p.stereo ? (data[(size_t) (ip * ch + 1)] + (data[(size_t) (ip2 * ch + 1)] - data[(size_t) (ip * ch + 1)]) * fr) / 32768.0 : a;
                    if (v.stopping) { v.fade -= 1.0f / (0.004f * (float) sr); if (v.fade <= 0.0f) { v.on = false; continue; } }
                    const double gv = v.gain * v.fade;
                    l += a * gv; r += b * gv;
                    // move on, at this clock against the one it was recorded at
                    v.pos += v.dir * v.speed * clock / sr;   // every sample plays at today's clock, whatever it was made at
                    const double s0 = p.start, e0 = p.end, lp = p.loopPoint;
                    if (v.dir > 0 && v.pos >= e0)
                    {
                        if (v.loop == Loop::wholeLoop) v.pos = s0 + std::fmod (v.pos - s0, std::max (1.0, e0 - s0));
                        else if (v.loop == Loop::partialLoop) v.pos = lp + std::fmod (v.pos - lp, std::max (1.0, e0 - lp));
                        else v.on = false;
                    }
                    else if (v.dir < 0 && v.pos < s0)
                    {
                        if (v.loop == Loop::reverseLoop) v.pos = e0 - std::fmod (s0 - v.pos, std::max (1.0, e0 - s0));
                        else v.on = false;
                    }
                }
                // SCRATCH: the window under the ribbon, the sample following your finger
                if (scratchPad >= 0 && pads[(size_t) scratchPad].used && ! decoded[(size_t) scratchPad].empty())
                {
                    const auto& p = pads[(size_t) scratchPad];
                    const auto& data = decoded[(size_t) scratchPad];
                    const int ch = p.stereo ? 2 : 1, frames = (int) data.size() / ch;
                    const double win = scratchWindowSeconds * p.recordedHz();
                    const double target = scratchStart + ribbon * win;
                    scratchPos += (target - scratchPos) * (1.0 - std::exp (-1.0 / (0.006 * sr)));
                    scratchFade = ribbonTouched ? std::min (1.0f, scratchFade + 1.0f / (0.004f * (float) sr)) : std::max (0.0f, scratchFade - 1.0f / (0.02f * (float) sr));
                    const int ip = std::clamp ((int) scratchPos, 0, frames - 1), ip2 = std::min (frames - 1, ip + 1);
                    const double fr = scratchPos - std::floor (scratchPos);
                    const double a = (data[(size_t) (ip * ch)] + (data[(size_t) (ip2 * ch)] - data[(size_t) (ip * ch)]) * fr) / 32768.0;
                    const double b = p.stereo ? (data[(size_t) (ip * ch + 1)] + (data[(size_t) (ip2 * ch + 1)] - data[(size_t) (ip * ch + 1)]) * fr) / 32768.0 : a;
                    // a record held still is silent: the scratch's steady level comes off
                    double xs[2] { a * scratchFade, b * scratchFade };
                    for (int c = 0; c < 2; ++c) { const double y = xs[c] - dcIn[c] + 0.995 * dcOut[c]; dcIn[c] = xs[c]; dcOut[c] = y; xs[c] = y; }
                    l += xs[0] * (p.volume / 127.0); r += xs[1] * (p.volume / 127.0);
                }
                l = recon[0].process (l);
                r = recon[1].process (r);
                if (effect == Effect::filter) { l = filter.process (l, 0, ft); r = filter.process (r, 1, ft); }
                l *= volume; r *= volume;

                // ---- the input, in the modes that use it ----
                if (effect == Effect::crossfade)
                {
                    const double a = (double) ribbon * pi * 0.5;
                    l = l * std::cos (a) + inL[i] * std::sin (a);
                    r = r * std::cos (a) + inR[i] * std::sin (a);
                }
                else if (effect == Effect::extFilter)
                {
                    l += filter.process (inL[i], 0, ft);
                    r += filter.process (inR[i], 1, ft);
                }
                else if (effect == Effect::extScratch)
                {
                    const int extLen = (int) extBuf[0].size();
                    if (ribbonTouched)
                    {
                        // touched: the buffer stops and the ribbon moves through what just came in
                        if (! extWasTouched) { extAnchor = extWrite; extPos = extAnchor; }
                        const double target = extAnchor - (1.0 - ribbon) * (extLen - 2);
                        extPos += (target - extPos) * (1.0 - std::exp (-1.0 / (0.006 * sr)));
                        double p = extPos;
                        while (p < 0) p += extLen;
                        const int i0 = (int) p % extLen, i1 = (i0 + 1) % extLen;
                        const double fr = p - std::floor (p);
                        l += extBuf[0][(size_t) i0] + (extBuf[0][(size_t) i1] - extBuf[0][(size_t) i0]) * fr;
                        r += extBuf[1][(size_t) i0] + (extBuf[1][(size_t) i1] - extBuf[1][(size_t) i0]) * fr;
                    }
                    else
                    {
                        extBuf[0][(size_t) extWrite] = inL[i];
                        extBuf[1][(size_t) extWrite] = inR[i];
                        extWrite = (extWrite + 1) % extLen;
                        l += inL[i]; r += inR[i];
                    }
                    extWasTouched = ribbonTouched;
                }
                outL[start + i] = (float) l;
                if (outR != nullptr) outR[start + i] = (float) r;
                ++now;
            }
        }
    }

    void handle (const Command& c)
    {
        // (presses from before the audio ran, the track not playing its chain, would all go off at once)
        const bool press = c.type == Command::padDown || c.type == Command::anyPadDown || c.type == Command::keyDown;
        if (press && c.ms > 0.0 && juce::Time::getMillisecondCounterHiRes() - c.ms > 250.0) return;
        switch (c.type)
        {
            case Command::padDown: startPad (bank * padsPerBank + c.a, c.f, false, revKey, holdKey); break;
            case Command::padUp: releasePad (bank * padsPerBank + c.a, false); break;
            case Command::anyPadDown: startPad (c.a, c.f, false, false, false); break;
            case Command::anyPadUp: releasePad (c.a, false); break;
            case Command::keyDown: keyDown (c.a); break;
            case Command::keyUp: keyUp (c.a); break;
            case Command::ribbon:
                ribbon = std::clamp (c.f, 0.0f, 1.0f);
                ribbonTouched = c.a != 0;
                if (ribbonTouched)
                {
                    ribbonUsed = true;
                    if (effect == Effect::pitch) pitchFromRibbon = -20.0 + 30.0 * ribbon;   // left lower, right higher
                }
                break;
            case Command::armRecord: armRecording(); break;
            case Command::stopRecord: if (recState == Rec::armed || recState == Rec::recording) recState = Rec::done; break;
            case Command::allOff: for (auto& v : voices) v.on = false; break;
            default: break;
        }
    }

    void midiIn (const juce::MidiMessage& m)
    {
        const int ch = channelP->get();
        if (ch > 0 && m.getChannel() != ch) return;
        auto padFor = [this] (int note)
        {
            for (int i = 0; i < numPads; ++i) if (pads[(size_t) i].note == note) return i;
            return -1;
        };
        if (m.isNoteOn())
        {
            const int p = padFor (m.getNoteNumber());
            if (p >= 0) startPad (p, m.getFloatVelocity(), false, revKey, holdKey);
        }
        else if (m.isNoteOff())
        {
            const int p = padFor (m.getNoteNumber());
            if (p >= 0) releasePad (p, false);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff()) { for (auto& v : voices) if (! v.latched) stopVoice (v); }
        else
        {
            // the ribbon from a controller: pitch bend (centred is let go) or a CC (0 is let go)
            const int source = ribbonSourceP->getIndex();
            if (source == 1 && m.isPitchWheel())
            {
                const int v = m.getPitchWheelValue();
                handle ({ Command::ribbon, v != 8192 ? 1 : 0, (float) v / 16383.0f });
            }
            else if (source == 2 && m.isController() && m.getControllerNumber() == ribbonCcP->get())
            {
                const int v = m.getControllerValue();
                handle ({ Command::ribbon, v > 0 ? 1 : 0, (float) v / 127.0f });
            }
        }
    }

    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        const int n = buffer.getNumSamples();
        const juce::SpinLock::ScopedTryLockType sl (dataLock);
        if (! sl.isLocked()) { buffer.clear(); return; }   // an edit swapping the memory: one block silent
        // the panel's commands, then the MIDI, at the block's start (a few ms early at most)
        const auto scope = fifo.read (fifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) handle (commands[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i) handle (commands[(size_t) (scope.startIndex2 + i)]);
        for (const auto m : midi) midiIn (m.getMessage());
        if (! midi.isEmpty()) lastMidiMs = juce::Time::getMillisecondCounterHiRes();
        // effects not at this grade go off
        if (! effectAllowed (grade(), effect, false)) effect = Effect::none;
        if (scratchPad >= 0 && ! effectAllowed (grade(), Effect::none, true)) scratchPad = -1;
        render (buffer, n);
        // the panel
        const juce::SpinLock::ScopedTryLockType pl (panelLock);
        if (pl.isLocked())
        {
            shown.bank = bank; shown.lastPad = lastPad; shown.voices = voicesInUse(); shown.maxVoices = polyphony (grade(), effect, scratchPad >= 0);
            shown.effect = effect; shown.scratchArmed = scratchKey; shown.shift = shift; shown.hold = holdKey; shown.reverse = revKey;
            shown.songSelected = song; shown.songPlaying = songPlaying; shown.songRecording = songRecording;
            shown.songSeconds = songPlaying ? songPosMs / 1000.0 : (songRecording && songStarted ? (double) (now - songStart) / sr : songPosMs / 1000.0);
            const auto rs = recState.load();
            shown.recording = rs == Rec::recording; shown.armed = rs == Rec::armed; shown.clipped = recClipped;
            shown.recordSeconds = (double) recFrames / std::max (1.0, recClock);
            shown.scalePad = scalePad; shown.scratchPad = scratchPad;
            shown.ribbonPitch = pitchFromRibbon;
            shown.songRepeat = songPlaying && songRepeat;
            shown.midiIn = juce::Time::getMillisecondCounterHiRes() - lastMidiMs.load() < 250.0;
            shown.padsSounding = 0;
            for (auto& v : voices) if (v.on && ! v.stopping) shown.padsSounding |= (uint64_t) 1 << v.pad;
        }
        ribbonShown = ribbon;
    }

    // ---- the message thread ----
    void timerCallback() override
    {
        if (recState.load() == Rec::done) commitRecording();
    }

    /** A recording into its pad: encoded, into memory, and decoded back for playing. */
    void commitRecording()
    {
        if (recState.exchange (Rec::idle) != Rec::done) return;
        const int frames = recFrames;
        if (frames < 16) return;
        const int ch = recNow.stereo ? 2 : 1;
        std::vector<int16_t> pcm (recBuf.begin(), recBuf.begin() + frames * ch);
        Pad p;
        p.used = true;
        p.stereo = recNow.stereo;
        p.grade = recGrade;
        p.pitchPercent = recPitch;
        p.frames = frames;
        p.start = 0; p.loopPoint = 0; p.end = frames;
        p.loop = recNow.defaultLoop;
        p.note = defaultNote (recNow.toPad);
        p.name = recNow.source == Source::track ? "track " + juce::String (recNow.track + 1)
               : recNow.source == Source::mix ? juce::String ("mix") : juce::String ("sample");
        store (recNow.toPad, p, pcm);
    }

    static int defaultNote (int pad) { return 36 + pad; }   // A1..D12 on C1..B4 (Yamaha's C1 is MIDI 36)

    /** Encodes pcm (interleaved) into memory after what's there, and makes the pad. */
    bool store (int padIndex, Pad p, const std::vector<int16_t>& pcm)
    {
        const int ch = p.stereo ? 2 : 1;
        std::vector<uint8_t> nibbles (pcm.size());
        Adpcm enc[2];
        for (size_t i = 0; i < pcm.size(); ++i) nibbles[i] = (uint8_t) enc[i % (size_t) ch].encode (pcm[i]);
        auto audio = decode (nibbles, ch);
        const juce::SpinLock::ScopedLockType sl (dataLock);
        if (used + (int) nibbles.size() > memoryNibbles) return false;
        std::copy (nibbles.begin(), nibbles.end(), memory.begin() + used);
        p.offset = used;
        used += (int) nibbles.size();
        pads[(size_t) padIndex] = p;
        decoded[(size_t) padIndex].swap (audio);
        return true;
    }

    static std::vector<int16_t> decode (const std::vector<uint8_t>& nibbles, int ch)
    {
        std::vector<int16_t> out (nibbles.size());
        Adpcm dec[2];
        for (size_t i = 0; i < nibbles.size(); ++i) out[i] = (int16_t) dec[i % (size_t) ch].decode (nibbles[i]);
        return out;
    }

    std::vector<uint8_t> nibblesOf (int padIndex) const
    {
        const auto& p = pads[(size_t) padIndex];
        if (! p.used) return {};
        return std::vector<uint8_t> (memory.begin() + p.offset, memory.begin() + p.offset + p.frames * (p.stereo ? 2 : 1));
    }

    /** Takes a pad's sample out of memory, closing the gap. (Under dataLock.) */
    void removeFromMemory (int padIndex)
    {
        auto& p = pads[(size_t) padIndex];
        if (! p.used) return;
        const int len = p.frames * (p.stereo ? 2 : 1), at = p.offset;
        std::move (memory.begin() + at + len, memory.begin() + used, memory.begin() + at);
        used -= len;
        for (auto& q : pads) if (q.used && q.offset > at) q.offset -= len;
        p = Pad();
        decoded[(size_t) padIndex].clear();
    }

    void prepareBuffers()
    {
        recBuf.assign ((size_t) memoryNibbles, 0);
        const int len = std::max (2, (int) (extScratchSeconds * sr));
        for (auto& b : extBuf) b.assign ((size_t) len, 0.0f);
        extWrite = 0;
        for (auto& s : songs) s.reserve ((size_t) songEventLimit);
    }
};

//==============================================================================
Processor::Processor()
    : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
      impl (std::make_unique<Impl> (*this))
{
    auto& i = *impl;
    i.ribbonP = new juce::AudioParameterFloat (juce::ParameterID { "ribbon", 1 }, "Ribbon", 0.0f, 1.0f, 0.5f);
    addParameter (i.ribbonP);
    i.gradeP = new juce::AudioParameterChoice (juce::ParameterID { "grade", 1 }, "Grade", { "HI", "STD", "LONG", "EXLG" }, 2);   // LONG, as it comes
    addParameter (i.gradeP);
    i.pitchP = new juce::AudioParameterInt (juce::ParameterID { "pitch", 1 }, "Pitch", -20, 10, 0, juce::AudioParameterIntAttributes().withLabel ("%"));
    addParameter (i.pitchP);
    i.volumeP = new juce::AudioParameterInt (juce::ParameterID { "volume", 1 }, "Volume", 0, 127, 100);
    addParameter (i.volumeP);
    i.filterTypeP = new juce::AudioParameterChoice (juce::ParameterID { "filterType", 1 }, "Filter type", { "LPF", "BPF", "HPF", "BYPASS" }, 0);
    addParameter (i.filterTypeP);
    i.resonanceP = new juce::AudioParameterChoice (juce::ParameterID { "resonance", 1 }, "Resonance", { "FLAT", "WEAK", "STRONG" }, 0);
    addParameter (i.resonanceP);
    i.channelP = new juce::AudioParameterInt (juce::ParameterID { "channel", 1 }, "MIDI channel", 0, 16, 0);   // 0: any
    addParameter (i.channelP);
    i.ribbonSourceP = new juce::AudioParameterChoice (juce::ParameterID { "ribbonSource", 1 }, "Ribbon from", { "OFF", "PITCH BEND", "CONTROLLER" }, 2);
    addParameter (i.ribbonSourceP);
    i.ribbonCcP = new juce::AudioParameterInt (juce::ParameterID { "ribbonCc", 1 }, "Ribbon controller", 0, 120, 1);
    addParameter (i.ribbonCcP);
    for (int p = 0; p < numPads; ++p) i.pads[(size_t) p].note = Impl::defaultNote (p);
    i.prepareBuffers();
}

Processor::~Processor() = default;

Front& Processor::front()
{
    if (frontPanel == nullptr) frontPanel = std::make_unique<Front> (*this);
    return *frontPanel;
}

juce::String Processor::flashMessage() const
{
    if (juce::Time::getMillisecondCounterHiRes() >= impl->messageUntil.load()) return {};
    static const char* m[] { "", "Pad Empty", "Not at this grade", "Memory Full", "Song memory full", "Song not empty",
                             "Rec standby", "Recording", "Select a pad", "Pad not empty" };
    return m[std::clamp (impl->message.load(), 0, 9)];
}

const juce::String Processor::getName() const { return "TF PHRASE SAMPLER"; }

juce::RangedAudioParameter* Processor::param (const juce::String& id) const
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID == id)
            return r;
    return nullptr;
}

Grade Processor::grade() const { return impl->grade(); }
double Processor::pitchPercent() const { return (double) impl->pitchP->get(); }

void Processor::prepareToPlay (double sampleRate, int)
{
    impl->sr = sampleRate;
    impl->reconHz = 0.0;
    impl->prepareBuffers();
}

void Processor::tapeFeed (const TrackFeed& f) noexcept
{
    impl->feed = f;
    impl->haveFeed = true;
    impl->ownTrack = f.ownTrack;
}

int Processor::ownTrack() const { return impl->ownTrack.load(); }
void Processor::setSongRepeat (bool on) { impl->repeatSetting = on; }
bool Processor::songRepeat() const { return impl->repeatSetting.load(); }

void Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // the ribbon as a parameter (learned or automated): the panel's and the controller's ribbon too
    auto& i = *impl;
    const float rp = i.ribbonP->get();
    if (! juce::exactlyEqual (rp, i.lastRibbonParam))
    {
        i.lastRibbonParam = rp;
        i.push ({ Command::ribbon, 1, rp });
    }
    i.process (buffer, midi);
    i.haveFeed = false;
    midi.clear();
}

//==============================================================================
// The pads and memory change only on the message thread (under dataLock, against the audio thread), so the
// message thread reads them without it: the audio thread never waits on the editor's drawing.
Pad Processor::pad (int index) const
{
    return impl->pads[(size_t) std::clamp (index, 0, numPads - 1)];
}

void Processor::setPad (int index, const Pad& p)
{
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    auto& q = impl->pads[(size_t) std::clamp (index, 0, numPads - 1)];
    if (! q.used) { q.note = p.note; return; }
    q.start = std::clamp (p.start, 0, std::max (0, q.frames - 128));
    q.end = std::clamp (p.end, q.start + 128, q.frames);
    q.loopPoint = std::clamp (p.loopPoint, q.start, std::max (q.start, q.end - 128));   // a loop of at least 128
    q.loop = p.loop;
    q.sounding = p.sounding;
    q.volume = std::clamp (p.volume, 0, 127);
    q.group = std::clamp (p.group, 0, 3);
    q.note = p.note;
    q.name = p.name;
}

int Processor::memoryUsedNibbles() const { return impl->used; }
double Processor::usedSeconds() const { return memoryUsedNibbles() / clockHz (grade(), pitchPercent()); }
double Processor::freeSeconds() const { return (memoryNibbles - memoryUsedNibbles()) / clockHz (grade(), pitchPercent()); }

std::vector<int16_t> Processor::padAudio (int index) const
{
    return impl->decoded[(size_t) std::clamp (index, 0, numPads - 1)];
}

bool Processor::copyPad (int from, int to, juce::String& error)
{
    auto p = pad (from);
    if (! p.used) { error = "PAD EMPTY"; return false; }
    if (pad (to).used) { error = "THE OTHER PAD ISN'T EMPTY"; return false; }
    std::vector<uint8_t> nib;
    nib = impl->nibblesOf (from);
    if (memoryUsedNibbles() + (int) nib.size() > memoryNibbles) { error = "MEMORY FULL"; return false; }
    auto audio = Impl::decode (nib, p.stereo ? 2 : 1);
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    std::copy (nib.begin(), nib.end(), impl->memory.begin() + impl->used);
    p.offset = impl->used;
    p.note = Impl::defaultNote (to);
    impl->used += (int) nib.size();
    impl->pads[(size_t) to] = p;
    impl->decoded[(size_t) to].swap (audio);
    return true;
}

bool Processor::movePad (int from, int to, juce::String& error)
{
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    if (! impl->pads[(size_t) from].used) { error = "PAD EMPTY"; return false; }
    if (impl->pads[(size_t) to].used) { error = "THE OTHER PAD ISN'T EMPTY"; return false; }
    const int note = impl->pads[(size_t) to].note;
    std::swap (impl->pads[(size_t) from], impl->pads[(size_t) to]);
    std::swap (impl->decoded[(size_t) from], impl->decoded[(size_t) to]);
    impl->pads[(size_t) from].note = impl->pads[(size_t) to].note;   // the notes stay with the pads
    impl->pads[(size_t) to].note = note;
    return true;
}

bool Processor::splitPad (int index, int to, juce::String& error)
{
    auto p = pad (index);
    if (! p.used) { error = "PAD EMPTY"; return false; }
    if (pad (to).used) { error = "THE OTHER PAD ISN'T EMPTY"; return false; }
    if (p.frames - p.end < 128) { error = "NOTHING AFTER THE END POINT"; return false; }
    // the part after the end point becomes the other pad's (the memory isn't moved: re-encoded as its own)
    const auto audio = padAudio (index);
    const int ch = p.stereo ? 2 : 1;
    std::vector<int16_t> tail (audio.begin() + p.end * ch, audio.end());
    {
        const juce::SpinLock::ScopedLockType sl (impl->dataLock);
        auto& q = impl->pads[(size_t) index];
        std::vector<int16_t> head (audio.begin(), audio.begin() + p.end * ch);
        // the first pad keeps what's before the end
        const int keep = p.end * ch;
        const int cut = p.frames * ch - keep;
        std::move (impl->memory.begin() + q.offset + q.frames * ch, impl->memory.begin() + impl->used, impl->memory.begin() + q.offset + keep);
        impl->used -= cut;
        for (auto& o : impl->pads) if (o.used && o.offset > q.offset) o.offset -= cut;
        q.frames = p.end;
        q.end = p.end;
        q.loopPoint = std::min (q.loopPoint, q.end - 1);
        impl->decoded[(size_t) index].swap (head);
    }
    Pad np = p;
    np.frames = (int) tail.size() / ch;
    np.start = 0; np.loopPoint = 0; np.end = np.frames;
    np.note = Impl::defaultNote (to);
    np.name = p.name + " 2";
    if (! impl->store (to, np, tail)) { error = "MEMORY FULL"; return false; }
    return true;
}

bool Processor::compactPad (int index)
{
    auto p = pad (index);
    if (! p.used || (p.start == 0 && p.end == p.frames)) return false;
    const auto audio = padAudio (index);
    const int ch = p.stereo ? 2 : 1;
    // the kept part, re-encoded from its decoded audio (the original recompacts its memory the same way)
    std::vector<int16_t> kept (audio.begin() + p.start * ch, audio.begin() + p.end * ch);
    {
        const juce::SpinLock::ScopedLockType sl (impl->dataLock);
        impl->removeFromMemory (index);
    }
    Pad np = p;
    np.frames = p.end - p.start;
    np.loopPoint = p.loopPoint - p.start;
    np.start = 0;
    np.end = np.frames;
    return impl->store (index, np, kept);
}

void Processor::deletePad (int index)
{
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    const int note = impl->pads[(size_t) index].note;
    impl->removeFromMemory (index);
    impl->pads[(size_t) index].note = note;
}

bool Processor::importFile (const juce::File& f, int toPad, juce::String& error)
{
    if (pad (toPad).used) { error = "PAD NOT EMPTY: DELETE IT FIRST"; return false; }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr) { error = "CAN'T READ " + f.getFileName().toUpperCase(); return false; }
    const auto rs = recordSettings();
    const int ch = rs.stereo ? 2 : 1;
    const double clock = clockHz (grade(), pitchPercent());
    const int freeFrames = (memoryNibbles - memoryUsedNibbles()) / ch;
    const auto inFrames = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (freeFrames * reader->sampleRate / clock) + 16);
    juce::AudioBuffer<float> in ((int) std::max (1u, reader->numChannels), std::max (1, inFrames));
    reader->read (&in, 0, inFrames, 0, true, true);
    // as if through the input: the same anti-alias filter, sampled at the clock
    LowPass4 aa[2];
    for (auto& a : aa) a.set (clock * 0.45, reader->sampleRate);
    std::vector<int16_t> pcm;
    pcm.reserve ((size_t) (freeFrames * ch));
    double phase = 0.0, prev[2] {};
    const double gain = gainOf (rs.gain);
    for (int i = 0; i < inFrames && (int) pcm.size() / ch < freeFrames; ++i)
    {
        const double l = in.getSample (0, i) * gain, r = in.getSample (in.getNumChannels() > 1 ? 1 : 0, i) * gain;
        const double fl = aa[0].process (l), fr = aa[1].process (rs.stereo ? r : l);
        phase += clock / reader->sampleRate;
        while (phase >= 1.0 && (int) pcm.size() / ch < freeFrames)
        {
            phase -= 1.0;
            const double t = std::clamp (1.0 - phase * reader->sampleRate / clock, 0.0, 1.0);
            pcm.push_back (toInt16 (prev[0] + (fl - prev[0]) * t));
            if (rs.stereo) pcm.push_back (toInt16 (prev[1] + (fr - prev[1]) * t));
        }
        prev[0] = fl; prev[1] = fr;
    }
    if (pcm.size() < (size_t) (16 * ch)) { error = "MEMORY FULL"; return false; }
    Pad p;
    p.used = true;
    p.stereo = rs.stereo;
    p.grade = grade();
    p.pitchPercent = pitchPercent();
    p.frames = (int) pcm.size() / ch;
    p.end = p.frames;
    p.note = Impl::defaultNote (toPad);
    p.name = f.getFileNameWithoutExtension().substring (0, 16);
    if (! impl->store (toPad, p, pcm)) { error = "MEMORY FULL"; return false; }
    if ((double) reader->lengthInSamples / reader->sampleRate > (double) p.frames / clock + 0.01) error = "MEMORY FULL: CUT SHORT";
    return true;
}

double Processor::loopTempo (int index) const
{
    const auto p = pad (index);
    if (! p.used) return 0.0;
    const int from = p.loop == Loop::partialLoop ? p.loopPoint : p.start;
    const double secs = (p.end - from) / clockHz (grade(), pitchPercent());
    if (secs <= 0.01) return 0.0;
    // as many beats as put it between 80 and 160 BPM
    double bpm = 60.0 / secs;
    while (bpm < 80.0) bpm *= 2.0;
    while (bpm >= 160.0) bpm /= 2.0;
    return bpm;
}

juce::String Tempo::text() const
{
    if (! shown) return {};
    return wholeBeats ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2);
}

Tempo tempoOf (double secs, bool reverse)
{
    Tempo t;
    if (secs < tempoShortestSeconds) return t;
    double bpm = 60.0 / secs;
    while (bpm < 80.0) bpm *= 2.0;
    while (bpm >= 160.0) bpm /= 2.0;
    t.shown = true;
    t.wholeBeats = reverse || secs > tempoWholeBeatsSeconds;
    // to the hundredth (or the beat), never showing 160
    t.bpm = t.wholeBeats ? std::min (159.0, std::floor (bpm + 0.5)) : std::min (159.99, std::round (bpm * 100.0) / 100.0);
    return t;
}

bool Processor::loops (int index) const
{
    const auto p = pad (index);
    return p.used && (p.loop == Loop::wholeLoop || p.loop == Loop::partialLoop || p.loop == Loop::reverseLoop);
}

Tempo Processor::tempo (int index) const
{
    const auto p = pad (index);
    if (! loops (index)) return {};
    const int from = p.loop == Loop::partialLoop ? p.loopPoint : p.start;
    return tempoOf ((p.end - from) / clockHz (grade(), pitchPercent()), p.loop == Loop::reverseLoop);
}

bool Processor::nudgeTempo (int index, int steps)
{
    auto p = pad (index);
    if (! p.used || steps == 0) return false;
    const int from = p.loop == Loop::partialLoop ? p.loopPoint : p.start;
    const double clock = clockHz (grade(), pitchPercent());
    const double secs = (p.end - from) / clock;
    if (secs <= 0.0) return false;
    // the beats the display counts in the loop (a power of two), held while the tempo moves
    double bpm = 60.0 / secs, beats = 1.0;
    while (bpm < 80.0) { bpm *= 2.0; beats *= 2.0; }
    while (bpm >= 160.0) { bpm /= 2.0; beats /= 2.0; }
    const double target = std::max (1.0, std::round (bpm * 100.0) / 100.0 + steps * 0.01);
    int end = from + (int) std::lround (beats * 60.0 / target * clock);
    // faster is shorter: at least one frame each way
    if (steps > 0 && end >= p.end) end = p.end - 1;
    if (steps < 0 && end <= p.end) end = p.end + 1;
    end = std::clamp (end, from + 1, p.frames);
    if (end == p.end) return false;
    p.end = end;
    setPad (index, p);
    return true;
}

void Processor::deleteAllSamples()
{
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    for (int i = 0; i < numPads; ++i)
    {
        const int note = impl->pads[(size_t) i].note;
        impl->pads[(size_t) i] = Pad();
        impl->pads[(size_t) i].note = note;
        impl->decoded[(size_t) i].clear();
    }
    impl->used = 0;
    for (auto& s : impl->songs) s.clear();
    impl->noteSongs();
    for (auto& v : impl->voices) v.on = false;
}

void Processor::resetEverything()
{
    deleteAllSamples();
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            r->setValueNotifyingHost (r->getDefaultValue());
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    for (int i = 0; i < numPads; ++i) impl->pads[(size_t) i].note = Impl::defaultNote (i);
    impl->record = {};
    impl->repeatSetting = false;
}

Processor::RecordSettings Processor::recordSettings() const { return impl->record; }
void Processor::setRecordSettings (const RecordSettings& r) { const juce::SpinLock::ScopedLockType sl (impl->dataLock); impl->record = r; }

bool Processor::armRecord (juce::String& error)
{
    const auto r = recordSettings();
    if (pad (r.toPad).used) { error = "PAD NOT EMPTY: DELETE IT FIRST"; return false; }
    if (memoryNibbles - memoryUsedNibbles() < 512) { error = "MEMORY FULL"; return false; }
    impl->push ({ Command::armRecord, 0, 0.0f });
    return true;
}

void Processor::stopRecord()
{
    impl->push ({ Command::stopRecord, 0, 0.0f });
    // (with no audio running, nothing would take the command: end it here)
    if (impl->recState.load() == Impl::Rec::armed || impl->recState.load() == Impl::Rec::recording)
    {
        const juce::SpinLock::ScopedLockType sl (impl->dataLock);
        auto expected = impl->recState.load();
        impl->recState.compare_exchange_strong (expected, Impl::Rec::done);
    }
    impl->commitRecording();
}

void Processor::pressPad (int padInBank, float velocity) { impl->push ({ Command::padDown, std::clamp (padInBank, 0, padsPerBank - 1), velocity }); }
void Processor::playPad (int index, bool down) { impl->push ({ down ? Command::anyPadDown : Command::anyPadUp, std::clamp (index, 0, numPads - 1), 1.0f }); }
void Processor::releasePad (int padInBank) { impl->push ({ Command::padUp, std::clamp (padInBank, 0, padsPerBank - 1), 0.0f }); }

static int keyIndex (const juce::String& key)
{
    for (int k = 0; k < numKeys; ++k) if (key == keyNames[k]) return k;
    return -1;
}

void Processor::pressKey (const juce::String& key) { if (const int k = keyIndex (key); k >= 0) impl->push ({ Command::keyDown, k, 0.0f }); }
void Processor::releaseKey (const juce::String& key) { if (const int k = keyIndex (key); k >= 0) impl->push ({ Command::keyUp, k, 0.0f }); }
void Processor::setRibbon (float position, bool touched) { impl->push ({ Command::ribbon, touched ? 1 : 0, position }); impl->ribbonShown = position; }
float Processor::takeInputPeak() { return impl->inputPeak.exchange (0.0f); }
float Processor::ribbonPosition() const { return impl->ribbonShown.load(); }

PanelState Processor::panel() const
{
    const juce::SpinLock::ScopedLockType sl (impl->panelLock);
    return impl->shown;
}

juce::String Processor::lcdLine (int line) const
{
    const auto s = panel();
    if (line == 0)
    {
        juce::String t = s.songRecording ? "SONG REC" : s.songPlaying ? "SONG PLAY" : "PLAY";
        t << "  BANK " << juce::String::charToString ((juce::juce_wchar) ('A' + s.bank)) << "  " << gradeName (grade());
        if (s.effect != Effect::none) t << "  " << effectName (s.effect);
        if (s.effect == Effect::filter || s.effect == Effect::extFilter)
            t << " " << impl->filterTypeP->getCurrentChoiceName();
        if (s.scratchPad >= 0) t << "  SCRATCH";
        if (s.hold) t << "  H";
        return t;
    }
    if (line == 1)
    {
        if (juce::Time::getMillisecondCounterHiRes() < impl->messageUntil.load())
        {
            static const char* m[] { "", "Pad Empty", "Not at this grade", "Memory Full", "Song memory full", "Song not empty: delete it first",
                                     "Rec standby", "Recording", "Select a pad", "Pad not empty" };
            return m[std::clamp (impl->message.load(), 0, 9)];
        }
        if (s.armed) return "Rec standby";
        if (s.recording) return "Recording " + juce::String (s.recordSeconds, 1) + " s";
        if (s.lastPad >= 0)
        {
            const auto p = pad (s.lastPad);
            return juce::String::charToString ((juce::juce_wchar) ('A' + s.lastPad / padsPerBank)) + juce::String (s.lastPad % padsPerBank + 1)
                 + "  " + (p.used ? p.name : juce::String ("Pad Empty"));
        }
        return "Ready";
    }
    switch (s.effect)
    {
        case Effect::pitch: return "PITCH " + juce::String (s.ribbonPitch >= 0 ? "+" : "") + juce::String (juce::roundToInt (s.ribbonPitch)) + "%";
        case Effect::filter: case Effect::extFilter: return "CUTOFF";
        case Effect::crossfade: return "SAMPLE <> INPUT";
        case Effect::scale: return s.scalePad >= 0 ? "SCALE ON PAD " + juce::String (s.scalePad % padsPerBank + 1) : juce::String ("SELECT A PAD");
        case Effect::extScratch: return "TOUCH TO SCRATCH";
        case Effect::none: default: return s.scratchPad >= 0 ? juce::String ("SCRATCH WINDOW") : juce::String ("SONG ") + (s.songSelected >= 0 ? juce::String (s.songSelected + 1) : juce::String ("-"))
                                                               + "  " + juce::String (s.songSeconds, 1) + " S";
    }
}

// (the audio thread records songs: their sizes and lengths are kept as atomics for the editor)
int Processor::songEvents (int song) const { return impl->songCount[(size_t) std::clamp (song, 0, 3)].load(); }
int Processor::songEventsTotal() const { int t = 0; for (int s = 0; s < numSongs; ++s) t += songEvents (s); return t; }
double Processor::songLength (int song) const
{
    return impl->songLastMs[(size_t) std::clamp (song, 0, 3)].load() / 1000.0;
}
void Processor::deleteSong (int song)
{
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    impl->songs[(size_t) std::clamp (song, 0, 3)].clear();
    impl->noteSongs();
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Processor::toXml (bool) const
{
    auto x = std::make_unique<juce::XmlElement> ("SAMPLER");
    x->setAttribute ("version", 1);
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && r->paramID != "ribbon")
            x->setAttribute (r->paramID, (double) r->convertFrom0to1 (r->getValue()));
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    // the memory, as the ADPCM nibbles packed two a byte, like the original's bulk dump
    juce::MemoryBlock mb ((size_t) (impl->used + 1) / 2, true);
    auto* d = static_cast<uint8_t*> (mb.getData());
    for (int i = 0; i < impl->used; ++i) d[i / 2] |= (uint8_t) ((impl->memory[(size_t) i] & 15) << ((i & 1) ? 0 : 4));
    auto* mem = x->createNewChildElement ("MEMORY");
    mem->setAttribute ("nibbles", impl->used);
    mem->addTextElement (mb.toBase64Encoding());
    auto* pads = x->createNewChildElement ("PADS");
    for (int i = 0; i < numPads; ++i)
    {
        const auto& p = impl->pads[(size_t) i];
        auto* e = pads->createNewChildElement ("PAD");
        e->setAttribute ("i", i);
        e->setAttribute ("note", p.note);
        if (! p.used) continue;
        e->setAttribute ("stereo", p.stereo);
        e->setAttribute ("grade", (int) p.grade);
        e->setAttribute ("pitch", p.pitchPercent);
        e->setAttribute ("offset", p.offset);
        e->setAttribute ("frames", p.frames);
        e->setAttribute ("start", p.start);
        e->setAttribute ("loopPoint", p.loopPoint);
        e->setAttribute ("end", p.end);
        e->setAttribute ("loop", (int) p.loop);
        e->setAttribute ("sounding", (int) p.sounding);
        e->setAttribute ("volume", p.volume);
        e->setAttribute ("group", p.group);
        e->setAttribute ("name", p.name);
    }
    auto* songs = x->createNewChildElement ("SONGS");
    for (int s = 0; s < numSongs; ++s)
    {
        auto* e = songs->createNewChildElement ("SONG");
        juce::MemoryOutputStream out;
        for (auto& ev : impl->songs[(size_t) s]) { out.writeInt ((int) ev.ms); out.writeByte ((char) ev.pad); out.writeByte ((char) ev.kind); out.writeByte ((char) ev.velocity); out.writeByte ((char) ev.flags); }
        e->addTextElement (out.getMemoryBlock().toBase64Encoding());
    }
    auto* rec = x->createNewChildElement ("RECORD");
    rec->setAttribute ("source", (int) impl->record.source);
    rec->setAttribute ("track", impl->record.track);
    rec->setAttribute ("postFader", impl->record.postFader);
    rec->setAttribute ("defaultLoop", (int) impl->record.defaultLoop);
    x->setAttribute ("songRepeat", impl->repeatSetting.load());
    rec->setAttribute ("stereo", impl->record.stereo);
    rec->setAttribute ("gain", impl->record.gain);
    rec->setAttribute ("trigger", impl->record.trigger);
    rec->setAttribute ("toPad", impl->record.toPad);
    return x;
}

void Processor::fromXml (const juce::XmlElement& x)
{
    if (! x.hasTagName ("SAMPLER")) return;
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p); r != nullptr && x.hasAttribute (r->paramID))
            r->setValueNotifyingHost (r->convertTo0to1 ((float) x.getDoubleAttribute (r->paramID)));
    std::vector<uint8_t> memory ((size_t) memoryNibbles, 0);
    int used = 0;
    if (auto* m = x.getChildByName ("MEMORY"))
    {
        juce::MemoryBlock mb;
        mb.fromBase64Encoding (m->getAllSubText().trim());
        used = std::clamp (m->getIntAttribute ("nibbles"), 0, std::min (memoryNibbles, (int) mb.getSize() * 2));
        const auto* d = static_cast<const uint8_t*> (mb.getData());
        for (int i = 0; i < used; ++i) memory[(size_t) i] = (uint8_t) ((d[i / 2] >> ((i & 1) ? 0 : 4)) & 15);
    }
    std::array<Pad, numPads> pads {};
    std::array<std::vector<int16_t>, numPads> decoded {};
    for (int i = 0; i < numPads; ++i) pads[(size_t) i].note = Impl::defaultNote (i);
    if (auto* ps = x.getChildByName ("PADS"))
        for (auto* e : ps->getChildIterator())
        {
            const int i = e->getIntAttribute ("i", -1);
            if (i < 0 || i >= numPads) continue;
            auto& p = pads[(size_t) i];
            p.note = e->getIntAttribute ("note", Impl::defaultNote (i));
            if (! e->hasAttribute ("frames")) continue;
            p.stereo = e->getBoolAttribute ("stereo");
            p.grade = (Grade) std::clamp (e->getIntAttribute ("grade", 2), 0, 3);
            p.pitchPercent = e->getDoubleAttribute ("pitch");
            p.offset = e->getIntAttribute ("offset");
            p.frames = e->getIntAttribute ("frames");
            const int ch = p.stereo ? 2 : 1;
            if (p.offset < 0 || p.frames <= 0 || p.offset + p.frames * ch > used) continue;
            p.used = true;
            p.start = std::clamp (e->getIntAttribute ("start"), 0, p.frames);
            p.end = std::clamp (e->getIntAttribute ("end", p.frames), p.start, p.frames);
            p.loopPoint = std::clamp (e->getIntAttribute ("loopPoint"), p.start, p.end);
            p.loop = (Loop) std::clamp (e->getIntAttribute ("loop"), 0, 4);
            p.sounding = (Sounding) std::clamp (e->getIntAttribute ("sounding"), 0, 2);
            p.volume = std::clamp (e->getIntAttribute ("volume", 127), 0, 127);
            p.group = std::clamp (e->getIntAttribute ("group"), 0, 3);
            p.name = e->getStringAttribute ("name");
            decoded[(size_t) i] = Impl::decode (std::vector<uint8_t> (memory.begin() + p.offset, memory.begin() + p.offset + p.frames * ch), ch);
        }
    std::array<std::vector<SongEvent>, numSongs> songs {};
    if (auto* ss = x.getChildByName ("SONGS"))
    {
        int s = 0;
        for (auto* e : ss->getChildIterator())
        {
            if (s >= numSongs) break;
            juce::MemoryBlock mb;
            mb.fromBase64Encoding (e->getAllSubText().trim());
            juce::MemoryInputStream in (mb, false);
            auto& v = songs[(size_t) s++];
            v.reserve ((size_t) songEventLimit);
            while (in.getNumBytesRemaining() >= 8 && (int) v.size() < songEventLimit)
            {
                SongEvent ev;
                ev.ms = (uint32_t) in.readInt(); ev.pad = (uint8_t) std::min (47, (int) (uint8_t) in.readByte()); ev.kind = (uint8_t) in.readByte();
                ev.velocity = (uint8_t) in.readByte(); ev.flags = (uint8_t) in.readByte();
                v.push_back (ev);
            }
        }
    }
    Processor::RecordSettings rs;
    if (auto* r = x.getChildByName ("RECORD"))
    {
        rs.source = (Source) std::clamp (r->getIntAttribute ("source"), 0, 3);   // (1 was a track's tape: now its output)
        rs.track = std::clamp (r->getIntAttribute ("track", r->getIntAttribute ("tapeTrack")), 0, 3);
        rs.postFader = r->getBoolAttribute ("postFader", true);
        rs.defaultLoop = (Loop) std::clamp (r->getIntAttribute ("defaultLoop"), 0, 4);
        rs.stereo = r->getBoolAttribute ("stereo");
        rs.gain = std::clamp (r->getIntAttribute ("gain", 8), 1, 16);
        rs.trigger = std::clamp (r->getIntAttribute ("trigger"), 0, 7);
        rs.toPad = std::clamp (r->getIntAttribute ("toPad"), 0, numPads - 1);
    }
    for (auto& s : songs) s.reserve ((size_t) songEventLimit);
    const juce::SpinLock::ScopedLockType sl (impl->dataLock);
    impl->memory.swap (memory);
    impl->used = used;
    impl->pads = pads;
    impl->decoded.swap (decoded);
    impl->songs.swap (songs);
    impl->record = rs;
    impl->repeatSetting = x.getBoolAttribute ("songRepeat");
    impl->noteSongs();
    for (auto& v : impl->voices) v.on = false;
}

void Processor::getStateInformation (juce::MemoryBlock& mb)
{
    auto x = toXml (true);
    writePresetAttributes (*x);
    copyXmlToBinary (*x, mb);
}

void Processor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr || ! x->hasTagName ("SAMPLER")) return;
    fromXml (*x);
    readPresetAttributes (*x);
}

const presets::Kind& presetKind()
{
    static const presets::Kind kind { "Phrase Sampler", "tfps", "TFPS", "SAMPLER", [] (const juce::XmlElement& s)
    {
        int padsUsed = 0;
        if (auto* ps = s.getChildByName ("PADS")) for (auto* e : ps->getChildIterator()) if (e->hasAttribute ("frames")) ++padsUsed;
        const int nib = s.getChildByName ("MEMORY") != nullptr ? s.getChildByName ("MEMORY")->getIntAttribute ("nibbles") : 0;
        const auto g = (Grade) std::clamp (s.getIntAttribute ("grade", 2), 0, 3);
        const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
        return juce::String (padsUsed) + " PADS" + dot + juce::String (nib / clockHz (g, s.getDoubleAttribute ("pitch")), 1) + " S" + dot + gradeName (g);
    } };
    return kind;
}

const presets::Kind& Processor::presetKind() const { return sampler::presetKind(); }
} // namespace tf::sampler
