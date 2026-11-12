#pragma once

#include "MidiHub.h"

namespace tf
{
/**
    MIDI learn: knobs, faders and buttons on a controller moving the deck's controls
    and plug-in parameters.

    A mapped message is used by its mapping and not passed on to the instruments,
    unless the mapping says "also plays". Knobs pick up (they only take over once
    they pass the control's current position); endless encoders are recognised when
    learned and move the control relatively; 14-bit CC pairs are combined; pitch bend
    (what Mackie Control faders send, one channel a fader) is a 14-bit fader.
    Transport buttons that send MIDI Machine Control are followed without learning.
*/
class MidiMapper
{
public:
    enum class Control
    {
        gain, high, mid, freq, low, pan, fader, mute, solo, arm,      // per track
        masterFader, phones, pitch, speed, nr,                        // deck
        play, stop, rec, rew, ff, rtz,                                // transport
        markAdd, markPrev, markNext,                                  // the inlay's counter marks
        numControls
    };

    struct Target
    {
        bool plugin = false;
        Control control = Control::fader;
        int track = -1;               // per-track controls and plug-ins
        int blockId = -1, param = -1; // plug-ins
        bool operator== (const Target& o) const
        {
            return plugin == o.plugin && track == o.track
                   && (plugin ? (blockId == o.blockId && param == o.param) : control == o.control);
        }
    };

    enum class Msg { cc, cc14, note, bend };   // bend: pitch bend, 14-bit (Mackie-style faders)
    // signedPosition: where a pot is, as a direction and a distance from its middle (bit 6 set: left
    // of it), the way a TASCAM Model 12's PAN knobs send it in Mackie Control mode
    enum class Mode { absolute, relTwos, relOffset, relSign, toggle, signedPosition };

    struct Mapping
    {
        int id = 0;
        int slot = -1;                 // MidiHub input slot
        juce::String portId, portName;
        int channel = 1;
        Msg msg = Msg::cc;
        int number = 0;
        Mode mode = Mode::absolute;
        bool pickup = true, passThrough = false;
        Target target;
        juce::String paramName;        // plug-ins: the parameter's name, for display

        // audio thread
        juce::AudioProcessorParameter* param = nullptr;
        bool pickedUp = false, held = false;
        float lastIn = -1.0f, lastSet = -1.0f;
        int msb = 0;
        bool builtIn = false;          // Mackie Control's own (not saved, not listed; a learned mapping wins)
        bool autoPosition = false;     // a Mackie V-pot that turns out to send where it is, not steps
    };

    /** What a mapping acts on; the engine implements it (audio thread). */
    struct Sink
    {
        virtual ~Sink() = default;
        virtual float getValue (const Target&, juce::AudioProcessorParameter*) = 0;   // 0..1
        virtual void setValue (const Target&, juce::AudioProcessorParameter*, float) = 0;
        virtual void toggle (const Target&) = 0;
        virtual void command (Control) = 0;
    };

    static bool isSwitch (Control c)  { return c == Control::mute || c == Control::solo || c == Control::arm || c == Control::speed || c == Control::nr; }
    static bool isCommand (Control c) { return c >= Control::play && c <= Control::markNext; }
    static bool isPerTrack (Control c) { return c <= Control::arm; }
    static juce::String controlName (Control c);

    explicit MidiMapper (MidiHub& hub);

    // ---- message thread ----
    /** The learned mappings (and with builtIn, Mackie Control's too). */
    std::vector<Mapping> getMappings (bool withBuiltIn = false) const;
    void remove (int id);
    void removeForTarget (const Target&);
    /** A block is going: its mappings go first, so nothing points at it. */
    void forgetBlock (int track, int blockId);
    void setPickup (int id, bool on);
    void setPassThrough (int id, bool on);
    juce::String describeMessage (const Mapping&) const;
    juce::String describeTarget (const Mapping&) const;
    /** The message mapped to a target, e.g. "CC 21", or empty. */
    juce::String labelFor (const Target&) const;

    void beginLearn (const Target&, juce::AudioProcessorParameter* param = nullptr, const juce::String& paramName = {});
    void cancelLearn();
    bool isLearning() const noexcept { return learning.load(); }
    /** Goes up with every mapping added, removed or changed (for redrawing only when needed). */
    std::atomic<int> changes { 0 };
    Target getLearnTarget() const { return learnTarget; }
    /** Finishes learning once a control has been moved; call a few times a second. */
    void poll();
    std::function<void (const Mapping&)> onLearned;

    std::atomic<bool> followMmc { true };

    /** Mackie Control (a control surface in MCU mode; laid out for the TASCAM Model 12's "MCU - Reaper"):
        strips 1-4 are tracks 1-4 (fader: pitch bend; pan: V-pot; REC, SOLO, MUTE), the master fader is
        the master, the transport keys are the transport, MARKER/F-keys add and find the inlay's marks.
        Everything else on the surface (strips 5-8, the jog dial...) is left for MIDI learn, and nothing
        from it reaches the tracks as notes. A learned mapping takes over from the built-in one. */
    void setMackie (int inputSlot, int outputSlot);
    /** Its V-pots are pots that send where they are, not steps (a TASCAM Model 12, 16 or 24: told by its port's name). */
    bool mackiePotsSendPositions() const noexcept { return mackiePositions.load(); }
    static bool potsSendPositions (const juce::String& portName);
    int mackieInput() const noexcept  { return mackieIn.load(); }
    int mackieOutput() const noexcept { return mackieOut.load(); }
    /** Whether the ports were ever chosen (by the user or found): if not, a port named for DAW control is taken. */
    bool mackieChosen = false;
    /** The last message from the surface: (status << 16) | (d1 << 8) | d2, or -1. For Setup's monitor. */
    std::atomic<int> mackieLast { -1 };
    static juce::String describeMackieMessage (int packed);
    // The MCU notes and controls TF-4 answers to.
    enum : int { mcuRec = 0x00, mcuSolo = 0x08, mcuMute = 0x10, mcuVpot = 0x10, mcuMarker = 0x54, mcuRew = 0x5b, mcuFf = 0x5c,
                 mcuStop = 0x5d, mcuPlay = 0x5e, mcuRecord = 0x5f, mcuLeft = 0x62, mcuRight = 0x63, mcuMasterChannel = 9 };

    /** Deck mappings, kept with the settings. */
    std::unique_ptr<juce::XmlElement> createDeckState() const;
    void restoreDeckState (const juce::XmlElement* maps);
    /** Plug-in mappings, kept inside the block's own element (so they travel with the tape). */
    void writeBlockMappings (juce::XmlElement& block, int track, int blockId) const;
    void restoreBlockMappings (const juce::XmlElement& block, int track, int blockId, juce::AudioProcessor* processor);

    /** A pickup that's waiting: which mapping, and +1 (turn up) or -1 (turn down). Read and cleared by the UI. */
    std::atomic<int> pickupWaitingId { 0 }, pickupDirection { 0 };

    // ---- audio thread ----
    /** Acts on the mapped messages in one controller's block of MIDI and takes them out of it. */
    void process (int slot, juce::MidiBuffer& buffer, Sink& sink) noexcept;

private:
    void apply (Mapping& m, int value, int range, Sink& sink) noexcept;
    void addMapping (Mapping m);
    std::unique_ptr<juce::XmlElement> mappingXml (const Mapping& m) const;
    bool readMapping (const juce::XmlElement& x, Mapping& m);

    void rebuildMackieLocked();
    bool matches (Mapping& m, int slot, int ch, const juce::uint8* d, Sink& sink) noexcept;

    MidiHub& hub;
    std::atomic<int> mackieIn { -1 }, mackieOut { -1 };
    std::atomic<bool> mackiePositions { false };
    juce::SpinLock lock;              // the audio thread only ever tries it
    std::vector<Mapping> mappings;
    int nextId = 1;
    juce::MidiBuffer scratch;

    std::atomic<bool> learning { false };
    Target learnTarget;
    juce::AudioProcessorParameter* learnParam = nullptr;
    juce::String learnParamName;
    struct Captured { int slot; juce::uint8 status, d1, d2; };
    juce::AbstractFifo learnFifo { 256 };
    std::array<Captured, 256> learnEvents {};
    std::vector<Captured> learnSeen;
    double learnFirstMs = 0.0;
};
} // namespace tf
