#include "PhraseSamplerFront.h"

namespace tf::sampler
{
namespace
{
    const char* const editNames[] { "LpMode", "SdMode", "PadVol", "AltGrp", "Note#", "StPnt", "LpPnt", "EdPnt",
                                    "CopyTo ?", "MoveTo ?", "SpltTo ?", "Compct ?", "Delete ?" };
    const char* const loopShort[] { "OneSht", "WLoop", "PLoop", "RShot", "RLoop" };
    const char* const soundShort[] { "Note", "Toggle", "Trigger" };
    enum RecFn { rGain, rGrade, rPitch, rFormat, rInput, rFader, rTrigger, rDefault, numRec };
    const char* const recNames[] { "Gain", "Grade", "Pitch", "Format", "Input", "Fader", "RecTrg", "Defalt" };
    enum UtilFn { uChannel, uRibbon, uRibbonCc, uSongDel, uInitData, uInitAll, numUtil };
    const char* const utilNames[] { "MIDI Ch", "Ribbon", "RbnCC#", "SongDel", "InitData", "InitAll" };

    /** How far a held +1 or -1 moves: one at a time at first, then faster. */
    int accelerated (int repeat, bool fine)
    {
        if (! fine) return repeat < 12 ? 1 : 4;
        return repeat < 10 ? 1 : repeat < 20 ? 10 : repeat < 30 ? 100 : 1000;
    }

    void setParam (Processor& p, const char* id, int value)
    {
        auto* r = p.param (id);
        r->beginChangeGesture();
        r->setValueNotifyingHost (r->convertTo0to1 ((float) r->getNormalisableRange().snapToLegalValue ((float) value)));
        r->endChangeGesture();
    }
    int getParam (Processor& p, const char* id) { auto* r = p.param (id); return juce::roundToInt (r->convertFrom0to1 (r->getValue())); }
}

Front::Front (Processor& p) : proc (p) {}

const char* Front::shiftLabel (int i)
{
    static const char* const l[] { "SONG 1", "SONG 2", "SONG 3", "SONG 4", "EDIT", "UTILITY",
                                   "SCALE", "FILTER", "CROSS FADER", "PITCH", "EXT SCRATCH", "EXT FILTER" };
    return l[std::clamp (i, 0, padsPerBank - 1)];
}

juce::String Front::padName (int index)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + index / padsPerBank)) + juce::String (index % padsPerBank + 1);
}

juce::String Front::inputName (const Processor::RecordSettings& r)
{
    switch (r.source)
    {
        case Source::track: return "TRK" + juce::String (r.track + 1);
        case Source::mix: return "MIX";
        case Source::file: return "FILE";
        case Source::input: default: return "IN";
    }
}

//==============================================================================
void Front::key (const juce::String& name, bool down, int repeat)
{
    if (name == "SHIFT")
    {
        shift = down;
        if (down) proc.pressKey ("SHIFT"); else proc.releaseKey ("SHIFT");
        return;
    }
    if (! down)
    {
        if (name == "SCRATCH" || name == "REV") proc.releaseKey (name);
        return;
    }
    if (name == "EXIT" && current != Mode::play)
    {
        if ((current == Mode::recSettings) && (proc.panel().armed || proc.panel().recording)) proc.stopRecord();
        current = Mode::play;
        choosingPad = sure = false;
        return;
    }
    if (name == "BANK")
    {
        // one key, round the four banks
        proc.pressKey ("BANK " + juce::String::charToString ((juce::juce_wchar) ('A' + (bank() + 1) % banks)));
        return;
    }
    switch (current)
    {
        case Mode::play: playKey (name, true); break;
        case Mode::edit: editKey (name, repeat); break;
        case Mode::utility: utilityKey (name, repeat); break;
        case Mode::recPad: case Mode::recSettings: case Mode::songRec: recKey (name, repeat); break;
    }
}

void Front::playKey (const juce::String& name, bool)
{
    if (name == "REC")
    {
        const auto st = proc.panel();
        if (st.songSelected >= 0)
        {
            if (proc.songEvents (st.songSelected) > 0) { flash ("Song not empty"); return; }
            current = Mode::songRec;
            return;
        }
        current = Mode::recPad;
        choosingPad = true;
        return;
    }
    if (name == "ENTER" || name == "<" || name == ">") return;
    proc.pressKey (name);   // SCRATCH HOLD REV EXIT +1 -1 TOP BWD STOP PLAY FWD
}

void Front::pad (int i, bool down, float velocity)
{
    i = std::clamp (i, 0, padsPerBank - 1);
    const int index = bank() * padsPerBank + i;
    const auto st = proc.panel();
    // FILTER with SHIFT held: pads 1-4 choose its type, 7-9 its resonance (the processor does it)
    const bool filterPad = shift && st.effect == Effect::filter && (i <= 3 || (i >= 6 && i <= 8));
    if (current == Mode::play || current == Mode::songRec)
    {
        if (shift && ! filterPad) { if (down) shiftPad (i); return; }
        if (down) proc.pressPad (i, velocity); else proc.releasePad (i);
        return;
    }
    if (! down)
    {
        if (current == Mode::edit && ! choosingPad) proc.playPad (padEdited, false);
        return;
    }
    switch (current)
    {
        case Mode::recPad:
        {
            if (proc.pad (index).used) { flash ("Pad not empty"); return; }
            auto r = proc.recordSettings(); r.toPad = index; proc.setRecordSettings (r);
            break;
        }
        case Mode::edit:
            if (choosingPad) { padEdited = index; proc.editorPad = index; break; }
            if (fn == copyTo || fn == moveTo || fn == spltTo)
            {
                if (proc.pad (index).used) { flash ("Pad not empty"); return; }
                destination = index;
                break;
            }
            proc.playPad (padEdited, true);   // hear what's being edited
            break;
        case Mode::play: case Mode::songRec: case Mode::recSettings: case Mode::utility: default: break;
    }
}

void Front::shiftPad (int i)
{
    if (i <= 3) { proc.pressKey ("SONG " + juce::String (i + 1)); return; }
    if (i == 4)
    {
        current = Mode::edit;
        choosingPad = true;
        sure = false;
        const int last = proc.panel().lastPad;
        padEdited = last >= 0 ? last : proc.editorPad;
        return;
    }
    if (i == 5) { current = Mode::utility; util = 0; sure = false; return; }
    static const char* const fx[] { "SCALE", "FILTER", "CROSSFADER", "PITCH", "EXT SCRATCH", "EXT FILTER" };
    proc.pressKey (fx[i - 6]);
}

//==============================================================================
void Front::moveFunction (int dir)
{
    const bool partial = proc.pad (padEdited).loop == Loop::partialLoop;
    fn = (fn + dir + numFunctions) % numFunctions;
    if (fn == lpPnt && ! partial) fn = (fn + dir + numFunctions) % numFunctions;   // only in PARTIAL LOOP
    sure = false;
    destination = -1;
}

void Front::editKey (const juce::String& name, int repeat)
{
    if (choosingPad)
    {
        if (name == "+1" || name == "-1") { padEdited = (padEdited + (name == "+1" ? 1 : numPads - 1)) % numPads; proc.editorPad = padEdited; }
        else if (name == "ENTER")
        {
            if (! proc.pad (padEdited).used) { flash ("Pad Empty"); return; }
            choosingPad = false; fn = lpMode; sure = false; destination = -1;
            proc.editorPad = padEdited;
        }
        return;
    }
    const bool partial = proc.pad (padEdited).loop == Loop::partialLoop;
    if (name == ">") moveFunction (1);
    else if (name == "<") moveFunction (-1);
    else if (name == "TOP") { fn = stPnt; sure = false; }
    else if (name == "BWD") { if (partial) fn = lpPnt; sure = false; }
    else if (name == "FWD") { fn = edPnt; sure = false; }
    else if (name == "+1" || name == "-1") change (name == "+1" ? 1 : -1, repeat);
    else if (name == "ENTER")
    {
        juce::String error;
        if (fn == copyTo || fn == moveTo || fn == spltTo)
        {
            if (destination < 0) { flash ("Select a pad"); return; }
            const bool ok = fn == copyTo ? proc.copyPad (padEdited, destination, error)
                          : fn == moveTo ? proc.movePad (padEdited, destination, error)
                                         : proc.splitPad (padEdited, destination, error);
            if (! ok) { flash (error.isNotEmpty() ? error : juce::String ("Memory Full")); return; }
            if (fn == moveTo) { padEdited = destination; proc.editorPad = padEdited; }
            destination = -1;
            flash ("Complete");
        }
        else if (fn == compct || fn == del)
        {
            if (! sure) { sure = true; return; }
            sure = false;
            if (fn == compct) proc.compactPad (padEdited);
            else { proc.deletePad (padEdited); choosingPad = true; }   // back to "Which pad?", as the unit does
            flash ("Complete");
        }
    }
    else if (name == "PLAY") proc.playPad (padEdited, true);
    else if (name == "STOP") proc.playPad (padEdited, false);
}

void Front::change (int d, int repeat)
{
    auto p = proc.pad (padEdited);
    if (! p.used) return;
    switch (fn)
    {
        case lpMode: p.loop = (Loop) std::clamp ((int) p.loop + d, 0, 4); break;
        case sdMode: p.sounding = (Sounding) std::clamp ((int) p.sounding + d, 0, 2); break;
        case padVol: p.volume = std::clamp (p.volume + d * accelerated (repeat, false), 0, 127); break;
        case altGrp: p.group = std::clamp (p.group + d, 0, 3); break;
        case noteNum: p.note = std::clamp (p.note + d * accelerated (repeat, false), -1, 127); break;
        case stPnt: p.start = std::clamp (p.start + d * accelerated (repeat, true), 0, p.end - 1); p.loopPoint = std::max (p.loopPoint, p.start); break;
        case lpPnt: p.loopPoint = std::clamp (p.loopPoint + d * accelerated (repeat, true), p.start, p.end - 1); break;
        case edPnt: p.end = std::clamp (p.end + d * accelerated (repeat, true), p.start + 1, p.frames); p.loopPoint = std::min (p.loopPoint, p.end - 1); break;
        case copyTo: case moveTo: case spltTo:
        {
            // the next empty pad that way
            int k = destination < 0 ? padEdited : destination;
            for (int n = 0; n < numPads; ++n)
            {
                k = (k + d + numPads) % numPads;
                if (! proc.pad (k).used) { destination = k; break; }
            }
            return;
        }
        default: return;
    }
    proc.setPad (padEdited, p);
}

//==============================================================================
void Front::utilityKey (const juce::String& name, int repeat)
{
    if (name == ">" || name == "<") { util = (util + (name == ">" ? 1 : numUtil - 1)) % numUtil; sure = false; return; }
    if (name == "+1" || name == "-1")
    {
        const int d = name == "+1" ? 1 : -1;
        switch (util)
        {
            case uChannel: setParam (proc, "channel", getParam (proc, "channel") + d); break;
            case uRibbon: setParam (proc, "ribbonSource", getParam (proc, "ribbonSource") + d); break;
            case uRibbonCc: setParam (proc, "ribbonCc", getParam (proc, "ribbonCc") + d * accelerated (repeat, false)); break;
            case uSongDel: utilSong = std::clamp (utilSong + d, 0, numSongs - 1); sure = false; break;
            default: break;
        }
        return;
    }
    if (name == "ENTER" && (util == uSongDel || util == uInitData || util == uInitAll))
    {
        if (util == uSongDel && proc.songEvents (utilSong) == 0) { flash ("Song empty"); return; }
        if (! sure) { sure = true; return; }
        sure = false;
        if (util == uSongDel) proc.deleteSong (utilSong);
        else if (util == uInitData) proc.deleteAllSamples();
        else proc.resetEverything();
        flash ("Complete");
    }
}

//==============================================================================
void Front::recKey (const juce::String& name, int repeat)
{
    auto r = proc.recordSettings();
    if (current == Mode::songRec)
    {
        if (name == "PLAY") { proc.pressKey ("REC"); current = Mode::play; }   // records from the first pad
        else if (name == "STOP") current = Mode::play;
        return;
    }
    if (current == Mode::recPad)
    {
        if (name == "+1" || name == "-1")
        {
            int k = r.toPad;
            for (int n = 0; n < numPads; ++n)
            {
                k = (k + (name == "+1" ? 1 : numPads - 1)) % numPads;
                if (! proc.pad (k).used) break;
            }
            r.toPad = k;
            proc.setRecordSettings (r);
        }
        else if (name == "ENTER")
        {
            if (proc.pad (r.toPad).used) { flash ("Pad not empty"); return; }
            current = Mode::recSettings;
            recFn = 0;
        }
        return;
    }
    // the recording settings
    const auto st = proc.panel();
    if (name == "PLAY")
    {
        juce::String error;
        if (! proc.armRecord (error)) flash (error);
        return;
    }
    if (name == "STOP")
    {
        if (st.armed || st.recording) { proc.stopRecord(); flash ("Complete"); current = Mode::play; }
        return;
    }
    if (st.armed || st.recording) return;
    if (name == ">" || name == "<") { recFn = (recFn + (name == ">" ? 1 : numRec - 1)) % numRec; return; }
    if (name != "+1" && name != "-1") return;
    const int d = name == "+1" ? 1 : -1;
    switch (recFn)
    {
        case rGain: r.gain = std::clamp (r.gain + d, 1, 16); break;
        case rGrade: setParam (proc, "grade", getParam (proc, "grade") + d); return;
        case rPitch: setParam (proc, "pitch", getParam (proc, "pitch") + d * accelerated (repeat, false)); return;
        case rFormat: r.stereo = ! r.stereo; break;
        case rInput:
        {
            // IN, TRK1..TRK4 (not its own), MIX
            std::vector<std::pair<Source, int>> list { { Source::input, 0 } };
            for (int t = 0; t < 4; ++t) if (t != proc.ownTrack()) list.push_back ({ Source::track, t });
            list.push_back ({ Source::mix, 0 });
            int at = 0;
            for (int k = 0; k < (int) list.size(); ++k)
                if (list[(size_t) k].first == r.source && (r.source != Source::track || list[(size_t) k].second == r.track)) at = k;
            at = std::clamp (at + d, 0, (int) list.size() - 1);
            r.source = list[(size_t) at].first;
            r.track = list[(size_t) at].second;
            break;
        }
        case rFader: r.postFader = ! r.postFader; break;
        case rTrigger: r.trigger = std::clamp (r.trigger + d, 0, 7); break;
        case rDefault: r.defaultLoop = (Loop) std::clamp ((int) r.defaultLoop + d, 0, 4); break;
        default: break;
    }
    proc.setRecordSettings (r);
}

//==============================================================================
Front::Lcd Front::lcd() const
{
    Lcd l;
    const auto st = proc.panel();
    const auto rs = proc.recordSettings();
    l.grade = gradeName (proc.grade());
    l.mode = current == Mode::edit ? "EDIT" : current == Mode::utility ? "UTIL" : "PLAY";
    const int shownPad = current == Mode::edit ? padEdited : current == Mode::recPad || current == Mode::recSettings ? rs.toPad : st.lastPad;
    if (shownPad >= 0)
    {
        const auto p = proc.pad (shownPad);
        if (p.used) { l.loop = loopShort[(int) p.loop]; l.format = p.stereo ? "STEREO" : "MONO"; }
    }
    l.input = inputName (rs);
    l.bank = juce::String::charToString ((juce::juce_wchar) ('A' + st.bank));
    switch (st.effect)
    {
        case Effect::pitch: l.effect = "PCH"; break;
        case Effect::scale: case Effect::filter: case Effect::crossfade: case Effect::extFilter: l.effect = "EFCT"; break;
        case Effect::extScratch: l.effect = "SCH"; break;
        case Effect::none: default: if (st.scratchPad >= 0) l.effect = "SCH"; break;
    }
    if (st.songSelected >= 0) l.song = "S" + juce::String (st.songSelected + 1);
    l.songRepeat = st.songRepeat;
    l.midi = st.midiIn;
    l.time = st.recording;
    l.rec = st.armed || st.recording || st.songRecording || current == Mode::recPad || current == Mode::recSettings || current == Mode::songRec;
    l.bar = proc.ribbonPosition();

    const bool noting = juce::Time::getMillisecondCounterHiRes() < noteUntil;
    const auto secs = [] (double s) { return juce::String (s, 3) + " s"; };
    const double clk = clock();
    auto freeMeter = [&]
    {
        l.meterLabel = "FREE";
        l.meter = juce::String (std::max (0.0, proc.freeSeconds() - (st.recording ? st.recordSeconds : 0.0)), 1);
    };

    switch (current)
    {
        case Mode::play:
        {
            if (st.armed || st.recording)
            {
                l.top = padName (rs.toPad); l.bottom = st.armed ? "Waiting" : "Recording"; freeMeter();
            }
            else if (st.songRecording)
            {
                l.top = "Song " + juce::String (st.songSelected + 1); l.bottom = "Recording";
                l.meterLabel = "LEFT"; l.meter = juce::String (songEventLimit - proc.songEventsTotal());
            }
            else if (st.songPlaying)
            {
                l.top = "Song " + juce::String (st.songSelected + 1); l.bottom = "Playing";
                l.meterLabel = "SONG"; l.meter = juce::String (st.songSeconds, 1);
            }
            else if (st.lastPad >= 0)
            {
                const auto p = proc.pad (st.lastPad);
                l.top = padName (st.lastPad); l.bottom = p.used ? p.name : juce::String ("Pad Empty");
            }
            else { l.top = shift ? "SHIFT" : ""; l.bottom = shift ? "Select function" : "Ready"; }
            if (shift && ! st.recording && ! st.songPlaying) { l.top = "SHIFT"; l.bottom = "Select function"; }
            const auto m = proc.flashMessage();
            if (m.isNotEmpty()) l.bottom = m;
            break;
        }
        case Mode::songRec:
            l.top = "Song " + juce::String (st.songSelected + 1); l.bottom = "Rec: press PLAY";
            l.meterLabel = "LEFT"; l.meter = juce::String (songEventLimit - proc.songEventsTotal());
            break;
        case Mode::recPad:
            l.top = "Rec: Which pad?";
            l.bottom = padName (rs.toPad) + (proc.pad (rs.toPad).used ? "  full" : "");
            freeMeter();
            l.bar = -2.0f;   // the input level
            break;
        case Mode::recSettings:
        {
            freeMeter();
            l.bar = -2.0f;
            if (st.armed || st.recording) { l.top = padName (rs.toPad); l.bottom = st.armed ? "Waiting" : "Recording"; break; }
            l.top = recNames[recFn];
            switch (recFn)
            {
                case rGain: l.bottom = juce::String (rs.gain); break;
                case rGrade: { static const char* g[] { "High", "Std", "Long", "ExLg" }; l.bottom = g[(int) proc.grade()]; break; }
                case rPitch: { const int pc = juce::roundToInt (proc.pitchPercent()); l.bottom = (pc > 0 ? "+" : "") + juce::String (pc) + "%"; break; }
                case rFormat: l.bottom = rs.stereo ? "Stereo" : "Mono"; break;
                case rInput: l.bottom = rs.source == Source::track ? "Trk" + juce::String (rs.track + 1) + " Out" : rs.source == Source::mix ? "Mix" : "This track"; break;
                case rFader: l.bottom = rs.postFader ? "Post" : "Pre"; break;
                case rTrigger: l.bottom = rs.trigger == 0 ? juce::String ("Manual") : "Level " + juce::String (rs.trigger); break;
                case rDefault: l.bottom = loopShort[(int) rs.defaultLoop]; break;
                default: break;
            }
            break;
        }
        case Mode::edit:
        {
            const auto p = proc.pad (padEdited);
            if (choosingPad) { l.top = "Which pad?"; l.bottom = padName (padEdited) + "  " + (p.used ? p.name : juce::String ("Pad Empty")); break; }
            l.top = editNames[fn];
            switch (fn)
            {
                case lpMode: l.bottom = loopShort[(int) p.loop]; break;
                case sdMode: l.bottom = soundShort[(int) p.sounding]; break;
                case padVol: l.bottom = juce::String (p.volume); break;
                case altGrp: l.bottom = p.group == 0 ? juce::String ("Off") : juce::String (p.group); break;
                case noteNum: l.bottom = p.note < 0 ? juce::String ("Off") : juce::MidiMessage::getMidiNoteName (p.note, true, true, 3); break;
                case stPnt: l.bottom = secs (p.start / clk); break;
                case lpPnt: l.bottom = secs (p.loopPoint / clk); break;
                case edPnt: l.bottom = secs (p.end / clk); break;
                case copyTo: case moveTo: case spltTo: l.bottom = destination >= 0 ? padName (destination) : juce::String ("Pad?"); break;
                case compct: case del:
                    if (sure) { l.top = (fn == del ? "Del " : "Compct ") + padName (padEdited); l.bottom = "Sure?"; }
                    else l.bottom = padName (padEdited);
                    break;
                default: break;
            }
            // the loop's tempo while its points are edited (manual pp. 46-48)
            const bool forward = p.loop == Loop::wholeLoop || p.loop == Loop::reverseLoop;
            if ((fn == stPnt && forward) || (fn == lpPnt && p.loop == Loop::partialLoop) || (fn == edPnt && proc.loops (padEdited)))
            {
                l.tempo = true;
                l.meterLabel = "TEMPO";
                const auto t = proc.tempo (padEdited);
                l.meter = t.shown ? t.text() : juce::String ("---");
            }
            break;
        }
        case Mode::utility:
            l.top = utilNames[util];
            switch (util)
            {
                case uChannel: { const int c = getParam (proc, "channel"); l.bottom = c == 0 ? juce::String ("Any") : juce::String (c); break; }
                case uRibbon: { static const char* r[] { "Off", "Bend", "CC" }; l.bottom = r[std::clamp (getParam (proc, "ribbonSource"), 0, 2)]; break; }
                case uRibbonCc: l.bottom = juce::String (getParam (proc, "ribbonCc")); break;
                case uSongDel: l.bottom = "Song " + juce::String (utilSong + 1) + (proc.songEvents (utilSong) == 0 ? "  empty" : ""); break;
                case uInitData: l.bottom = "All samples"; break;
                case uInitAll: l.bottom = "Everything"; break;
                default: break;
            }
            if (sure) l.bottom = "Sure?";
            break;
    }
    if (noting) l.bottom = note;
    return l;
}
} // namespace tf::sampler
