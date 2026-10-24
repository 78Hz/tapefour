#pragma once

#include "Common.h"

namespace tf
{
class MidiMapper;

/**
    Every MIDI controller coming in and every hardware synth going out.

    Controllers each get a slot that stays theirs (so tracks can name them),
    devices plugged in while running are picked up, and MIDI going out to synths
    is handed to a sender thread with a time stamp, so the audio thread never
    waits on a driver and notes keep their spacing within a block.
*/
class MidiHub
{
public:
    static constexpr int maxPorts = 64;
    static constexpr int computerKeyboard = 0;

    struct InputInfo  { int slot; juce::String id, name; bool available, active, mpe; int bendRange; };
    struct OutputInfo { int slot; juce::String id, name, alias; bool available, active; };

    explicit MidiHub (juce::AudioDeviceManager&);
    ~MidiHub();

    /** Says whether a track uses a slot, so a slot is never recycled from under it. */
    std::function<bool (int slot, bool output)> isSlotInUse;
    /** Called on the message thread after controllers come or go. */
    std::function<void()> onDevicesChanged;

    // ---- message thread ----
    void startListening();                     // hot-plug notifications
    void refresh();                            // open what's wanted and available, close the rest
    void scanForNewDevices();                  // give every connected device a slot
    std::vector<InputInfo> getInputs() const;
    std::vector<OutputInfo> getOutputs() const;
    void setInputActive (int slot, bool active);
    void setInputMpe (int slot, bool mpe, int bendRange);
    void setOutputActive (int slot, bool active);
    void setOutputAlias (int slot, const juce::String& alias);
    int findOrCreateInput (const juce::String& id, const juce::String& name);
    int findOrCreateOutput (const juce::String& id, const juce::String& name);
    juce::String inputName (int slot) const;   // empty for an unused slot
    juce::String inputId (int slot) const;
    juce::String outputId (int slot) const;
    juce::String outputName (int slot) const;
    juce::String outputLabel (int slot) const; // alias, else port name
    bool inputExists (int slot) const   { return juce::isPositiveAndBelow (slot, maxPorts) && in[slot]->id.isNotEmpty(); }
    bool outputExists (int slot) const  { return juce::isPositiveAndBelow (slot, maxPorts) && out[slot]->id.isNotEmpty(); }
    bool isInputActive (int slot) const { return juce::isPositiveAndBelow (slot, maxPorts) && in[slot]->active.load(); }
    bool isInputMpe (int slot) const    { return juce::isPositiveAndBelow (slot, maxPorts) && in[slot]->mpe.load(); }
    int inputBendRange (int slot) const { return juce::isPositiveAndBelow (slot, maxPorts) ? in[slot]->bendRange.load() : 48; }
    bool isOutputActive (int slot) const { return juce::isPositiveAndBelow (slot, maxPorts) && out[slot]->active.load(); }
    bool anyMpeInput() const;
    /** Silences a synth on an output straight away (when its block goes or is bypassed). */
    void allNotesOffNow (int slot);
    void sampleRateChanged (double sampleRate);

    std::unique_ptr<juce::XmlElement> createState() const;
    void restoreState (const juce::XmlElement* hw, const juce::XmlElement* legacyMidi);

    /** Puts a message on a controller's queue (the computer keyboard, or a test). */
    void inject (const juce::MidiMessage& m, int slot);
    int addVirtualInput (const juce::String& name);
    int addVirtualOutput (const juce::String& name);

    /** MIDI learn: mapped messages are taken out of each controller's MIDI before any track sees it. */
    void setMapper (MidiMapper* m, void* sink) noexcept { mapper = m; mapperSink = sink; }

    // ---- audio thread ----
    /** Collects this block from every active controller. */
    void beginBlock (int numSamples) noexcept;
    const juce::MidiBuffer& portBuffer (int slot) const noexcept { return in[(size_t) slot]->buffer; }
    const juce::MidiBuffer& anyBuffer() const noexcept           { return any; }
    bool anyIsMpe() const noexcept                               { return anyMpe; }
    int anyBendRange() const noexcept                            { return anyBend; }
    juce::MidiBuffer& outBuffer (int slot) noexcept              { return out[(size_t) slot]->buffer; }
    /** Hands this block's outgoing MIDI to the sender. blockStartMs is when the block began, in
        Time::getMillisecondCounterHiRes() terms. */
    void endBlock (double blockStartMs, double sampleRate, int numSamples) noexcept;

    std::function<void (int slot, const juce::MidiBuffer&)> outTap;   // test hook (AudioEngine::setMidiOutTapForTests): sees outgoing MIDI, audio thread
    std::atomic<bool> inFlash { false }, outFlash { false };

    static bool looksLikeMpeController (const juce::String& name);
    static void addMpeConfiguration (juce::MidiBuffer& b, bool on, int bendRange);

private:
    struct InPort
    {
        juce::String id, name;                 // message thread
        std::atomic<bool> active { false };
        std::atomic<bool> mpe { false };
        std::atomic<int> bendRange { 48 };
        juce::MidiMessageCollector collector;
        juce::MidiBuffer buffer;               // audio thread
        bool registered = false, wasAvailable = false;
    };

    struct OutEvent { double dueMs; juce::uint8 bytes[3]; juce::uint8 size; };

    struct OutPort
    {
        static constexpr int queueSize = 4096;
        juce::String id, name, alias;
        std::atomic<bool> active { false };
        juce::CriticalSection lock;            // message thread vs sender thread
        std::unique_ptr<juce::MidiOutput> device;
        juce::MidiBuffer buffer;               // audio thread
        juce::AbstractFifo fifo { queueSize }; // audio thread -> sender thread
        std::vector<OutEvent> queue = std::vector<OutEvent> ((size_t) queueSize);
    };

    class Sender;

    void devicesChanged();
    int adoptNewInputs();
    void recycleSlot (bool output, int slot);
    static void allNotesOff (juce::MidiOutput&);

    juce::AudioDeviceManager& deviceManager;
    std::unique_ptr<InPort> in[maxPorts];
    std::unique_ptr<OutPort> out[maxPorts];
    juce::MidiBuffer any;
    bool anyMpe = false;
    int anyBend = 48;
    double sampleRate = 48000.0;
    juce::MidiDeviceListConnection listConnection;
    juce::Array<juce::MidiDeviceInfo> devicesIn, devicesOut;   // as of the last change (message thread)
    void updateDeviceLists();
    std::unique_ptr<Sender> sender;
    MidiMapper* mapper = nullptr;
    void* mapperSink = nullptr;

    JUCE_DECLARE_NON_COPYABLE (MidiHub)
};
} // namespace tf
