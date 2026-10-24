#include "ClapFormat.h"

#include <clap/clap.h>
#include <map>
#include <mutex>

#if JUCE_LINUX || JUCE_MAC
 #include <poll.h>
#endif

namespace tf
{
namespace
{
//==============================================================================
// Modules: a .clap file loaded once and kept for the life of the program. Unloading plug-in
// libraries is a common source of crashes on exit, and the scanner runs in its own process anyway.
struct ClapModule
{
    juce::DynamicLibrary library;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
};

juce::File binaryFor (const juce::File& path)
{
   #if JUCE_MAC
    // A bundle: the binary is the one file in Contents/MacOS.
    if (path.isDirectory())
    {
        auto binaries = path.getChildFile ("Contents").getChildFile ("MacOS").findChildFiles (juce::File::findFiles, false);
        return binaries.isEmpty() ? juce::File() : binaries.getFirst();
    }
   #endif
    return path;
}

ClapModule* loadModule (const juce::String& path, juce::String& error)
{
    static std::mutex lock;
    static std::map<juce::String, std::unique_ptr<ClapModule>> modules;
    const std::scoped_lock sl (lock);

    if (auto it = modules.find (path); it != modules.end())
        return it->second.get();

    auto module = std::make_unique<ClapModule>();
    auto binary = binaryFor (juce::File (path));
    if (! binary.existsAsFile() || ! module->library.open (binary.getFullPathName()))
    {
        error = "Could not load " + juce::File (path).getFileName();
        return nullptr;
    }
    module->entry = static_cast<const clap_plugin_entry_t*> (module->library.getFunction ("clap_entry"));
    if (module->entry == nullptr || ! clap_version_is_compatible (module->entry->clap_version))
    {
        error = juce::File (path).getFileName() + " is not a CLAP plug-in this version understands";
        return nullptr;
    }
    if (! module->entry->init (path.toRawUTF8()))
    {
        error = juce::File (path).getFileName() + " would not start";
        return nullptr;
    }
    module->factory = static_cast<const clap_plugin_factory_t*> (module->entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    if (module->factory == nullptr)
    {
        error = juce::File (path).getFileName() + " has no plug-ins in it";
        return nullptr;
    }
    auto* raw = module.get();
    modules[path] = std::move (module);
    return raw;
}

bool hasFeature (const clap_plugin_descriptor_t* d, const char* feature)
{
    if (d->features != nullptr)
        for (const char* const* f = d->features; *f != nullptr; ++f)
            if (std::strcmp (*f, feature) == 0)
                return true;
    return false;
}

int idHash (const char* clapId) { return juce::String (clapId).hashCode(); }

void describe (juce::PluginDescription& desc, const clap_plugin_descriptor_t* d, const juce::String& path, int countInFile)
{
    juce::StringArray features;
    if (d->features != nullptr)
        for (const char* const* f = d->features; *f != nullptr; ++f)
            features.add (*f);

    desc.name = juce::String::fromUTF8 (d->name);
    desc.descriptiveName = d->description != nullptr && *d->description != 0 ? juce::String::fromUTF8 (d->description) : desc.name;
    desc.pluginFormatName = ClapPluginFormat::formatName;
    desc.category = features.joinIntoString ("|");
    desc.manufacturerName = juce::String::fromUTF8 (d->vendor != nullptr ? d->vendor : "");
    desc.version = juce::String::fromUTF8 (d->version != nullptr ? d->version : "");
    desc.fileOrIdentifier = path;
    desc.lastFileModTime = juce::File (path).getLastModificationTime();
    desc.lastInfoUpdateTime = juce::Time::getCurrentTime();
    desc.uniqueId = desc.deprecatedUid = idHash (d->id);
    desc.isInstrument = hasFeature (d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    const bool noteEffect = hasFeature (d, CLAP_PLUGIN_FEATURE_NOTE_EFFECT);
    desc.numInputChannels = desc.isInstrument || noteEffect ? 0 : 2;
    desc.numOutputChannels = noteEffect ? 0 : 2;
    desc.hasSharedContainer = countInFile > 1;
}

thread_local bool onAudioThread = false;

/** TAPEFOUR_CLAP_LOG=1 prints what CLAP plug-ins ask of the host, for diagnosing a plug-in. */
void clapLog (const juce::String& text)
{
    static const bool on = juce::SystemStats::getEnvironmentVariable ("TAPEFOUR_CLAP_LOG", {}).isNotEmpty();
    if (on) std::fprintf (stderr, "[clap] %s\n", text.toRawUTF8());
}

//==============================================================================
// Events going into the plug-in, built per block in fixed storage.
union AnyEvent
{
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_param_value_t param;
};

struct InEvents
{
    static constexpr int capacity = 2048;
    std::array<AnyEvent, capacity> events {};
    uint32_t count = 0;
    clap_input_events_t list { this, &size, &get };

    void clear() { count = 0; }
    AnyEvent* add() { return count < (uint32_t) capacity ? &events[count++] : nullptr; }

    static uint32_t CLAP_ABI size (const clap_input_events_t* l) { return static_cast<const InEvents*> (l->ctx)->count; }
    static const clap_event_header_t* CLAP_ABI get (const clap_input_events_t* l, uint32_t i)
    {
        auto* self = static_cast<const InEvents*> (l->ctx);
        return i < self->count ? &self->events[i].header : nullptr;
    }
};

class ClapInstance;

//==============================================================================
class ClapParameter : public juce::HostedAudioProcessorParameter
{
public:
    ClapParameter (ClapInstance& o, const clap_param_info_t& info, double current)
        : owner (o), id (info.id), cookie (info.cookie), lo (info.min_value), hi (info.max_value),
          def (info.default_value), flags (info.flags), name (juce::String::fromUTF8 (info.name))
    {
        norm = toNorm (current);
    }

    float getValue() const override { return norm.load(); }
    void setValue (float v) override;
    float getDefaultValue() const override { return toNorm (def); }
    juce::String getName (int maxLength) const override { return name.substring (0, maxLength); }
    juce::String getLabel() const override { return {}; }
    juce::String getParameterID() const override { return juce::String ((juce::int64) id); }
    int getNumSteps() const override
    {
        return (flags & CLAP_PARAM_IS_STEPPED) != 0 && hi > lo ? juce::jlimit (2, 0x7fffffff, (int) std::round (hi - lo) + 1)
                                                              : AudioProcessorParameter::getNumSteps();
    }
    bool isDiscrete() const override { return (flags & CLAP_PARAM_IS_STEPPED) != 0; }
    bool isAutomatable() const override { return (flags & CLAP_PARAM_IS_AUTOMATABLE) != 0; }
    juce::String getText (float v, int maxLength) const override;
    float getValueForText (const juce::String& text) const override;

    double fromNorm (float v) const { return lo + (hi - lo) * (double) juce::jlimit (0.0f, 1.0f, v); }
    float toNorm (double v) const   { return hi > lo ? (float) juce::jlimit (0.0, 1.0, (v - lo) / (hi - lo)) : 0.0f; }

    /** The plug-in moved it (its own window, or a preset): tell listeners without sending it back. */
    void plugInChanged (double value)
    {
        auto n = toNorm (value);
        if (std::abs (n - norm.load()) < 1.0e-7f) return;
        norm = n;
        sendValueChangedMessageToListeners (n);
    }

    ClapInstance& owner;
    const clap_id id;
    void* const cookie;
    const double lo, hi, def;
    const uint32_t flags;
    const juce::String name;
    std::atomic<float> norm { 0.0f };
};

//==============================================================================
class ClapEditor;

class ClapInstance : public juce::AudioPluginInstance,
                     private juce::Timer
{
public:
    /** The host as one plug-in sees it. It exists before the plug-in does: a plug-in may register its
        timers and file descriptors from init(), before there is an instance to hold them. */
    struct Host : private juce::Timer
    {
        clap_host_t host {};
        ClapInstance* owner = nullptr;
        const clap_plugin_t* plugin = nullptr;
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);

        struct PluginTimer : juce::Timer
        {
            PluginTimer (Host& h, clap_id i) : host (h), id (i) {}
            void timerCallback() override { host.fireTimer (id); }
            Host& host;
            clap_id id;
        };
        std::map<clap_id, std::unique_ptr<PluginTimer>> timers;
        clap_id nextTimerId = 1;
        std::map<int, clap_posix_fd_flags_t> fds;

        ~Host() override { stopTimer(); timers.clear(); }

        void fireTimer (clap_id id)
        {
            if (plugin == nullptr) return;
            if (timerExt == nullptr)
                timerExt = static_cast<const clap_plugin_timer_support_t*> (plugin->get_extension (plugin, CLAP_EXT_TIMER_SUPPORT));
            if (timerExt != nullptr) timerExt->on_timer (plugin, id);
        }

        void watchFds() { if (! fds.empty() && ! isTimerRunning()) startTimer (10); }

    private:
        // The plug-in's file descriptors (its connection to the display, its own queues), polled.
        void timerCallback() override
        {
           #if JUCE_LINUX || JUCE_MAC
            if (fds.empty() || plugin == nullptr) { stopTimer(); return; }
            if (fdExt == nullptr)
                fdExt = static_cast<const clap_plugin_posix_fd_support_t*> (plugin->get_extension (plugin, CLAP_EXT_POSIX_FD_SUPPORT));
            if (fdExt == nullptr) return;
            std::vector<pollfd> set;
            for (auto& [fd, flags] : fds)
                set.push_back ({ fd, (short) (((flags & CLAP_POSIX_FD_READ) ? POLLIN : 0) | ((flags & CLAP_POSIX_FD_WRITE) ? POLLOUT : 0)), 0 });
            if (::poll (set.data(), (nfds_t) set.size(), 0) <= 0) return;
            for (auto& p : set)
                if (p.revents != 0)
                    fdExt->on_fd (plugin, p.fd, (uint32_t) (((p.revents & POLLIN) ? CLAP_POSIX_FD_READ : 0)
                                                            | ((p.revents & POLLOUT) ? CLAP_POSIX_FD_WRITE : 0)
                                                            | ((p.revents & (POLLERR | POLLHUP)) ? CLAP_POSIX_FD_ERROR : 0)));
           #else
            stopTimer();
           #endif
        }

        const clap_plugin_timer_support_t* timerExt = nullptr;
        const clap_plugin_posix_fd_support_t* fdExt = nullptr;
    };

    static std::unique_ptr<ClapInstance> create (const juce::PluginDescription& desc, juce::String& error)
    {
        auto* module = loadModule (desc.fileOrIdentifier, error);
        if (module == nullptr) return {};

        const clap_plugin_descriptor_t* found = nullptr;
        const auto count = module->factory->get_plugin_count (module->factory);
        for (uint32_t i = 0; i < count && found == nullptr; ++i)
            if (auto* d = module->factory->get_plugin_descriptor (module->factory, i))
                if (idHash (d->id) == desc.uniqueId || (desc.uniqueId == 0 && desc.name == juce::String::fromUTF8 (d->name)))
                    found = d;
        if (found == nullptr)
        {
            error = desc.name + " is no longer in " + juce::File (desc.fileOrIdentifier).getFileName();
            return {};
        }

        auto host = std::make_unique<Host>();
        initHost (*host);
        auto* plugin = module->factory->create_plugin (module->factory, &host->host, found->id);
        host->plugin = plugin;
        if (plugin == nullptr || ! plugin->init (plugin))
        {
            if (plugin != nullptr) plugin->destroy (plugin);
            error = desc.name + " would not start";
            return {};
        }

        // The main audio ports decide the processor's buses.
        int ins = 0, outs = 0;
        if (auto* ports = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            clap_audio_port_info_t info {};
            if (ports->count (plugin, true) > 0 && ports->get (plugin, 0, true, &info)) ins = (int) info.channel_count;
            if (ports->count (plugin, false) > 0 && ports->get (plugin, 0, false, &info)) outs = (int) info.channel_count;
        }
        BusesProperties buses;
        auto set = [] (int n) { return n <= 2 ? juce::AudioChannelSet::canonicalChannelSet (n) : juce::AudioChannelSet::discreteChannels (n); };
        if (ins > 0)  buses = buses.withInput ("Input", set (ins), true);
        if (outs > 0) buses = buses.withOutput ("Output", set (outs), true);

        auto instance = std::unique_ptr<ClapInstance> (new ClapInstance (buses, std::move (host), plugin, found, desc));
        instance->readExtensions();
        return instance;
    }

    ~ClapInstance() override
    {
        stopTimer();
        *host->alive = false;
        host->owner = nullptr;
        if (active) releaseResources();
        plugin->destroy (plugin);
        host->plugin = nullptr;
    }

    // ---- AudioProcessor ----
    const juce::String getName() const override { return description.name; }
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d = description;
        d.numInputChannels = getTotalNumInputChannels();
        d.numOutputChannels = getTotalNumOutputChannels();
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return notesIn; }
    bool producesMidi() const override { return notesOut; }
    bool isMidiEffect() const override { return noteEffect && getTotalNumOutputChannels() == 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void prepareToPlay (double newRate, int maxBlock) override
    {
        if (active) releaseResources();
        maxFrames = std::max (32, maxBlock);
        rate = newRate;
        allocatePorts();
        active = plugin->activate (plugin, newRate, 1, (uint32_t) maxFrames);
        if (active && latencyExt != nullptr)
            setLatencySamples ((int) latencyExt->get (plugin));
    }

    void releaseResources() override
    {
        if (! active) return;
        if (processing)
        {
            // The audio thread has let go of this block (it's being removed or the device stopped).
            onAudioThread = true;
            plugin->stop_processing (plugin);
            onAudioThread = false;
            processing = false;
        }
        plugin->deactivate (plugin);
        active = false;
    }

    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const int n = buffer.getNumSamples();
        if (! active || n > maxFrames)
        {
            buffer.clear();
            midi.clear();
            return;
        }
        onAudioThread = true;
        if (! processing)
            processing = plugin->start_processing (plugin);
        if (! processing)
        {
            onAudioThread = false;
            buffer.clear();
            return;
        }

        // events in: parameter changes first, then the MIDI in time order
        in.clear();
        takeQueuedParameters();
        for (const auto m : midi)
            addMidiEvent (m.samplePosition, m.data, m.numBytes);
        midi.clear();
        midiOut = &midi;

        // audio in: copied, so the plug-in reads its input and writes its output to different memory
        const int ins = getTotalNumInputChannels(), outs = getTotalNumOutputChannels();
        for (int c = 0; c < ins && ! inPorts.empty(); ++c)
            std::memcpy (inPorts[0].pointers[(size_t) c], buffer.getReadPointer (c), sizeof (float) * (size_t) n);
        for (size_t p = 1; p < inPorts.size(); ++p)
            for (auto* ch : inPorts[p].pointers) juce::FloatVectorOperations::clear (ch, n);
        if (! outPorts.empty())
            for (int c = 0; c < outs; ++c)
                outPorts[0].pointers[(size_t) c] = buffer.getWritePointer (c);

        for (size_t p = 0; p < inPorts.size(); ++p)  inBuffers[p].data32 = inPorts[p].pointers.data();
        for (size_t p = 0; p < outPorts.size(); ++p) outBuffers[p].data32 = outPorts[p].pointers.data();

        clap_process_t proc {};
        proc.steady_time = steadyTime;
        proc.frames_count = (uint32_t) n;
        proc.transport = nullptr;
        proc.audio_inputs = inBuffers.data();
        proc.audio_inputs_count = (uint32_t) inBuffers.size();
        proc.audio_outputs = outBuffers.data();
        proc.audio_outputs_count = (uint32_t) outBuffers.size();
        proc.in_events = &in.list;
        proc.out_events = &outEvents;

        if (plugin->process (plugin, &proc) == CLAP_PROCESS_ERROR)
            buffer.clear();
        for (int c = outs; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, n);

        steadyTime += n;
        midiOut = nullptr;
        lastProcessMs = juce::Time::getMillisecondCounter();
        onAudioThread = false;
    }

    // ---- state ----
    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (stateExt == nullptr) return;
        juce::MemoryOutputStream out (dest, false);
        clap_ostream_t stream { &out, [] (const clap_ostream_t* s, const void* data, uint64_t size) -> int64_t
        {
            return static_cast<juce::MemoryOutputStream*> (s->ctx)->write (data, (size_t) size) ? (int64_t) size : -1;
        } };
        stateExt->save (plugin, &stream);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (stateExt == nullptr || size <= 0) return;
        juce::MemoryInputStream input (data, (size_t) size, false);
        clap_istream_t stream { &input, [] (const clap_istream_t* s, void* buffer, uint64_t n) -> int64_t
        {
            return (int64_t) static_cast<juce::MemoryInputStream*> (s->ctx)->read (buffer, (int) n);
        } };
        stateExt->load (plugin, &stream);
        refreshParameterValues();
    }

    // ---- editor ----
    bool hasEditor() const override
    {
        return guiExt != nullptr && guiExt->is_api_supported (plugin, windowApi(), false);
    }
    juce::AudioProcessorEditor* createEditor() override;

    static const char* windowApi()
    {
       #if JUCE_WINDOWS
        return CLAP_WINDOW_API_WIN32;
       #elif JUCE_MAC
        return CLAP_WINDOW_API_COCOA;
       #else
        return CLAP_WINDOW_API_X11;
       #endif
    }

    // ---- parameters (any thread) ----
    void queueParameter (const ClapParameter& p, double value)
    {
        {
            const juce::SpinLock::ScopedLockType sl (queueLock);
            if (queued.size() < queued.capacity())
                queued.push_back ({ p.id, p.cookie, value });
        }
        flushWanted = true;
    }

    juce::String valueToText (const ClapParameter& p, double v) const
    {
        char text[256] {};
        if (paramsExt != nullptr && juce::MessageManager::existsAndIsCurrentThread()
            && paramsExt->value_to_text (plugin, p.id, v, text, sizeof (text)))
            return juce::String::fromUTF8 (text);
        return juce::String (v, 2);
    }

    bool textToValue (const ClapParameter& p, const juce::String& text, double& v) const
    {
        return paramsExt != nullptr && juce::MessageManager::existsAndIsCurrentThread()
               && paramsExt->text_to_value (plugin, p.id, text.toRawUTF8(), &v);
    }

    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* guiExt = nullptr;
    ClapEditor* editor = nullptr;

private:
    struct Queued { clap_id id; void* cookie; double value; };
    struct Port
    {
        juce::AudioBuffer<float> storage;
        std::vector<float*> pointers;
    };

    ClapInstance (const BusesProperties& buses, std::unique_ptr<Host> h, const clap_plugin_t* p,
                  const clap_plugin_descriptor_t* d, const juce::PluginDescription& desc)
        : AudioPluginInstance (buses), plugin (p), host (std::move (h)), description (desc)
    {
        host->owner = this;
        noteEffect = hasFeature (d, CLAP_PLUGIN_FEATURE_NOTE_EFFECT);
        description.name = juce::String::fromUTF8 (d->name);
        queued.reserve (1024);
        outEvents = { this, &pushOut };
    }

    void readExtensions()
    {
        auto ext = [this] (const char* id) { return plugin->get_extension (plugin, id); };
        paramsExt  = static_cast<const clap_plugin_params_t*> (ext (CLAP_EXT_PARAMS));
        stateExt   = static_cast<const clap_plugin_state_t*> (ext (CLAP_EXT_STATE));
        latencyExt = static_cast<const clap_plugin_latency_t*> (ext (CLAP_EXT_LATENCY));
        guiExt     = static_cast<const clap_plugin_gui_t*> (ext (CLAP_EXT_GUI));
        audioPorts = static_cast<const clap_plugin_audio_ports_t*> (ext (CLAP_EXT_AUDIO_PORTS));

        if (auto* notes = static_cast<const clap_plugin_note_ports_t*> (ext (CLAP_EXT_NOTE_PORTS)))
        {
            clap_note_port_info_t info {};
            if (notes->count (plugin, true) > 0 && notes->get (plugin, 0, true, &info))
            {
                notesIn = true;
                midiDialect = (info.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
                clapDialect = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
            }
            notesOut = notes->count (plugin, false) > 0;
        }

        if (paramsExt != nullptr)
        {
            const auto count = paramsExt->count (plugin);
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_param_info_t info {};
                if (! paramsExt->get_info (plugin, i, &info) || (info.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                    continue;
                double value = info.default_value;
                paramsExt->get_value (plugin, info.id, &value);
                auto p = std::make_unique<ClapParameter> (*this, info, value);
                byId[info.id] = p.get();
                addHostedParameter (std::move (p));
            }
        }
        startTimerHz (20);   // flushes parameter changes while the audio isn't running
    }

    void allocatePorts()
    {
        auto build = [this] (bool input, std::vector<Port>& ports, std::vector<clap_audio_buffer_t>& buffers)
        {
            ports.clear();
            buffers.clear();
            const uint32_t count = audioPorts != nullptr ? audioPorts->count (plugin, input) : 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_audio_port_info_t info {};
                if (! audioPorts->get (plugin, i, input, &info)) continue;
                Port port;
                port.storage.setSize ((int) std::max (1u, info.channel_count), maxFrames);
                port.storage.clear();
                for (int c = 0; c < (int) info.channel_count; ++c)
                    port.pointers.push_back (port.storage.getWritePointer (c));
                ports.push_back (std::move (port));
                clap_audio_buffer_t b {};
                b.channel_count = info.channel_count;
                buffers.push_back (b);
            }
        };
        build (true, inPorts, inBuffers);
        build (false, outPorts, outBuffers);
    }

    void takeQueuedParameters()
    {
        const juce::SpinLock::ScopedTryLockType sl (queueLock);
        if (! sl.isLocked()) return;
        for (auto& q : queued)
            if (auto* e = in.add())
            {
                auto& p = e->param;
                p.header = { sizeof (clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                p.param_id = q.id;
                p.cookie = q.cookie;
                p.note_id = -1;
                p.port_index = -1;
                p.channel = -1;
                p.key = -1;
                p.value = q.value;
            }
        queued.clear();
    }

    void addMidiEvent (int time, const juce::uint8* d, int size)
    {
        if (! notesIn || size < 1 || size > 3) return;
        const int type = d[0] & 0xf0;
        if (midiDialect)
        {
            if (auto* e = in.add())
            {
                auto& m = e->midi;
                m.header = { sizeof (clap_event_midi_t), (uint32_t) time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                m.port_index = 0;
                m.data[0] = d[0];
                m.data[1] = size > 1 ? d[1] : 0;
                m.data[2] = size > 2 ? d[2] : 0;
            }
            return;
        }
        if (! clapDialect || size < 3) return;
        const bool on = type == 0x90 && d[2] > 0, off = type == 0x80 || (type == 0x90 && d[2] == 0);
        if (! on && ! off) return;   // notes are all a CLAP-only plug-in can take from MIDI
        if (auto* e = in.add())
        {
            auto& note = e->note;
            note.header = { sizeof (clap_event_note_t), (uint32_t) time, CLAP_CORE_EVENT_SPACE_ID,
                            (uint16_t) (on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
            note.note_id = -1;
            note.port_index = 0;
            note.channel = (int16_t) (d[0] & 0x0f);
            note.key = (int16_t) d[1];
            note.velocity = d[2] / 127.0;
        }
    }

    static bool CLAP_ABI pushOut (const clap_output_events_t* list, const clap_event_header_t* e)
    {
        auto& self = *static_cast<ClapInstance*> (list->ctx);
        if (e->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
        switch (e->type)
        {
            case CLAP_EVENT_MIDI:
                if (self.midiOut != nullptr)
                {
                    auto* m = reinterpret_cast<const clap_event_midi_t*> (e);
                    self.midiOut->addEvent (m->data, juce::MidiMessage::getMessageLengthFromFirstByte (m->data[0]), (int) e->time);
                }
                break;
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
                if (self.midiOut != nullptr)
                {
                    auto* n = reinterpret_cast<const clap_event_note_t*> (e);
                    const int ch = juce::jlimit (1, 16, n->channel + 1), key = juce::jlimit (0, 127, (int) n->key);
                    self.midiOut->addEvent (e->type == CLAP_EVENT_NOTE_ON
                                                ? juce::MidiMessage::noteOn (ch, key, (float) juce::jlimit (0.0, 1.0, n->velocity))
                                                : juce::MidiMessage::noteOff (ch, key, (float) juce::jlimit (0.0, 1.0, n->velocity)),
                                            (int) e->time);
                }
                break;
            case CLAP_EVENT_PARAM_VALUE:
                if (auto it = self.byId.find (reinterpret_cast<const clap_event_param_value_t*> (e)->param_id); it != self.byId.end())
                    it->second->plugInChanged (reinterpret_cast<const clap_event_param_value_t*> (e)->value);
                break;
            case CLAP_EVENT_PARAM_GESTURE_BEGIN:
            case CLAP_EVENT_PARAM_GESTURE_END:
                // A gesture in the plug-in's own window: what MIDI learn listens for.
                if (auto it = self.byId.find (reinterpret_cast<const clap_event_param_gesture_t*> (e)->param_id); it != self.byId.end())
                {
                    if (e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) it->second->beginChangeGesture();
                    else                                           it->second->endChangeGesture();
                }
                break;
            default: break;
        }
        return true;
    }

    void refreshParameterValues()
    {
        if (paramsExt == nullptr) return;
        for (auto& [id, p] : byId)
        {
            double v = 0;
            if (paramsExt->get_value (plugin, id, &v))
                p->plugInChanged (v);
        }
    }

    void timerCallback() override
    {
        // Parameter changes with no audio running to carry them: flush them on the main thread.
        const bool audioIdle = ! processing || juce::Time::getMillisecondCounter() - lastProcessMs > 250;
        if (paramsExt != nullptr && audioIdle && ! active && flushWanted.exchange (false))
        {
            in.clear();
            takeQueuedParameters();
            midiOut = nullptr;
            paramsExt->flush (plugin, &in.list, &outEvents);
        }
    }

    // ---- host side ----
    static ClapInstance* owner (const clap_host_t* h) { return static_cast<Host*> (h->host_data)->owner; }
    static Host& hostOf (const clap_host_t* h) { return *static_cast<Host*> (h->host_data); }

    static void initHost (Host& h)
    {
        auto& c = h.host;
        c.clap_version = CLAP_VERSION;
        c.host_data = &h;
        c.name = "TAPE-FOUR";
        c.vendor = "78Hz";
        c.url = "https://github.com/78Hz/tapefour";
        c.version = "0.2";
        c.get_extension = &hostExtension;
        c.request_restart = [] (const clap_host_t* clapHost)
        {
            onMain (clapHost, [] (ClapInstance& self)
            {
                if (! self.active) return;
                const auto sr = self.rate;
                const auto bs = self.maxFrames;
                // Held for the whole restart: the audio thread only tries this lock, so it skips the
                // plug-in until it's active again, rather than calling into it half way through.
                const juce::ScopedLock sl (self.getCallbackLock());
                self.prepareToPlay (sr, bs);
            });
        };
        c.request_process = [] (const clap_host_t*) {};
        c.request_callback = [] (const clap_host_t* clapHost)
        {
            onMain (clapHost, [] (ClapInstance& self) { self.plugin->on_main_thread (self.plugin); });
        };
    }

    /** Runs fn on the message thread, if the instance is still there by then. */
    static void onMain (const clap_host_t* host, std::function<void (ClapInstance&)> fn)
    {
        auto* h = static_cast<Host*> (host->host_data);
        std::weak_ptr<bool> alive = h->alive;
        juce::MessageManager::callAsync ([h, alive, fn]
        {
            if (! alive.expired() && h->owner != nullptr)
                fn (*h->owner);
        });
    }

    static const void* CLAP_ABI hostExtension (const clap_host_t*, const char* id)
    {
        static const clap_host_thread_check_t threadCheck {
            [] (const clap_host_t*) { return juce::MessageManager::existsAndIsCurrentThread(); },
            [] (const clap_host_t*) { return onAudioThread; } };
        static const clap_host_log_t log {
            [] (const clap_host_t*, clap_log_severity, const char* msg) { juce::ignoreUnused (msg); DBG ("CLAP: " << msg); } };
        static const clap_host_params_t params {
            [] (const clap_host_t* h, clap_param_rescan_flags flags)
            {
                if ((flags & CLAP_PARAM_RESCAN_VALUES) != 0)
                    if (auto* self = owner (h)) self->refreshParameterValues();
            },
            [] (const clap_host_t*, clap_id, clap_param_clear_flags) {},
            [] (const clap_host_t* h) { if (auto* self = owner (h)) self->flushWanted = true; } };
        static const clap_host_state_t state {
            [] (const clap_host_t* h)
            {
                onMain (h, [] (ClapInstance& self)
                {
                    self.updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
                });
            } };
        static const clap_host_latency_t latency {
            [] (const clap_host_t* h)
            {
                onMain (h, [] (ClapInstance& self)
                {
                    if (self.active && self.latencyExt != nullptr)
                        self.setLatencySamples ((int) self.latencyExt->get (self.plugin));
                });
            } };
        static const clap_host_audio_ports_t audioPorts {
            [] (const clap_host_t*, uint32_t) { return false; },
            [] (const clap_host_t*, uint32_t) {} };
        static const clap_host_note_ports_t notePorts {
            [] (const clap_host_t*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE; },
            [] (const clap_host_t*, uint32_t) {} };
        static const clap_host_gui_t gui {
            [] (const clap_host_t*) {},
            &requestResize,
            [] (const clap_host_t*) { return true; },
            [] (const clap_host_t*) { return false; },
            [] (const clap_host_t*, bool) {} };
        static const clap_host_timer_support_t timers { &registerTimer, &unregisterTimer };
       #if JUCE_LINUX || JUCE_MAC
        static const clap_host_posix_fd_support_t fds { &registerFd, &modifyFd, &unregisterFd };
       #endif

        const std::string_view e (id);
        clapLog ("host extension asked: " + juce::String (id));
        if (e == CLAP_EXT_THREAD_CHECK)  return &threadCheck;
        if (e == CLAP_EXT_LOG)           return &log;
        if (e == CLAP_EXT_PARAMS)        return &params;
        if (e == CLAP_EXT_STATE)         return &state;
        if (e == CLAP_EXT_LATENCY)       return &latency;
        if (e == CLAP_EXT_AUDIO_PORTS)   return &audioPorts;
        if (e == CLAP_EXT_NOTE_PORTS)    return &notePorts;
        if (e == CLAP_EXT_GUI)           return &gui;
        if (e == CLAP_EXT_TIMER_SUPPORT) return &timers;
       #if JUCE_LINUX || JUCE_MAC
        if (e == CLAP_EXT_POSIX_FD_SUPPORT) return &fds;
       #endif
        return nullptr;
    }

    static bool CLAP_ABI requestResize (const clap_host_t* h, uint32_t w, uint32_t hgt);

    // Timers and file descriptors the plug-in asks for (plug-in windows on Linux run on them).
    static bool CLAP_ABI registerTimer (const clap_host_t* h, uint32_t periodMs, clap_id* id)
    {
        if (id == nullptr || ! juce::MessageManager::existsAndIsCurrentThread()) return false;
        auto& host = hostOf (h);
        *id = host.nextTimerId++;
        clapLog ("register_timer " + juce::String (periodMs) + " ms -> " + juce::String (*id));
        auto t = std::make_unique<Host::PluginTimer> (host, *id);
        t->startTimer ((int) juce::jlimit (10u, 1000u, periodMs));
        host.timers[*id] = std::move (t);
        return true;
    }

    static bool CLAP_ABI unregisterTimer (const clap_host_t* h, clap_id id)
    {
        return hostOf (h).timers.erase (id) > 0;
    }

   #if JUCE_LINUX || JUCE_MAC
    static bool CLAP_ABI registerFd (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags)
    {
        auto& host = hostOf (h);
        host.fds[fd] = flags;
        host.watchFds();
        clapLog ("register_fd " + juce::String (fd) + " flags " + juce::String (flags));
        return true;
    }
    static bool CLAP_ABI modifyFd (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) { return registerFd (h, fd, flags); }
    static bool CLAP_ABI unregisterFd (const clap_host_t* h, int fd)
    {
        return hostOf (h).fds.erase (fd) > 0;
    }
   #endif

    std::unique_ptr<Host> host;
    juce::PluginDescription description;
    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_audio_ports_t* audioPorts = nullptr;
    bool notesIn = false, notesOut = false, midiDialect = false, clapDialect = false, noteEffect = false;

    bool active = false;
    std::atomic<bool> processing { false };
    double rate = 48000.0;
    int maxFrames = 512;
    int64_t steadyTime = 0;
    std::atomic<juce::uint32> lastProcessMs { 0 };

    std::vector<Port> inPorts, outPorts;
    std::vector<clap_audio_buffer_t> inBuffers, outBuffers;
    InEvents in;
    clap_output_events_t outEvents {};
    juce::MidiBuffer* midiOut = nullptr;

    juce::SpinLock queueLock;
    std::vector<Queued> queued;
    std::atomic<bool> flushWanted { false };
    std::map<clap_id, ClapParameter*> byId;
};

//==============================================================================
void ClapParameter::setValue (float v)
{
    norm = juce::jlimit (0.0f, 1.0f, v);
    owner.queueParameter (*this, fromNorm (v));
}

juce::String ClapParameter::getText (float v, int maxLength) const
{
    return owner.valueToText (*this, fromNorm (v)).substring (0, maxLength);
}

float ClapParameter::getValueForText (const juce::String& text) const
{
    double v = 0;
    return owner.textToValue (*this, text, v) ? toNorm (v) : toNorm (text.getDoubleValue());
}

//==============================================================================
/** The plug-in's own window, put inside ours. */
class ClapEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    explicit ClapEditor (ClapInstance& o) : juce::AudioProcessorEditor (o), owner (o), gui (o.guiExt)
    {
        setOpaque (true);
        created = gui != nullptr && gui->create (owner.plugin, ClapInstance::windowApi(), false);
        clapLog ("gui create " + juce::String (created ? "ok" : "failed"));
        uint32_t w = 400, h = 300;
        if (created) gui->get_size (owner.plugin, &w, &h);
        setResizable (created && gui->can_resize (owner.plugin), false);
        setSize ((int) std::max (40u, w), (int) std::max (40u, h));
        owner.editor = this;
        startTimer (30);   // attach once the window is on screen
    }

    ~ClapEditor() override
    {
        owner.editor = nullptr;
        if (created)
        {
            if (attached) gui->hide (owner.plugin);
            gui->destroy (owner.plugin);
        }
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    void resized() override
    {
        if (! attached || resizingFromPlugin || ! isResizable()) return;
        uint32_t w = (uint32_t) getWidth(), h = (uint32_t) getHeight();
        if (gui->adjust_size (owner.plugin, &w, &h))
        {
            gui->set_size (owner.plugin, w, h);
            if ((int) w != getWidth() || (int) h != getHeight())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ClapEditor> (this), w, h]
                {
                    if (safe != nullptr) safe->setSize ((int) w, (int) h);
                });
        }
    }

    /** The plug-in wants another size (its own resize handle, or a different view). */
    void resizeFromPlugin (int w, int h)
    {
        const juce::ScopedValueSetter<bool> s (resizingFromPlugin, true);
        setSize (w, h);
        if (attached) gui->set_size (owner.plugin, (uint32_t) w, (uint32_t) h);
    }

private:
    void timerCallback() override
    {
        if (! created || attached) { stopTimer(); return; }
        auto* peer = getPeer();
        if (peer == nullptr || ! isShowing()) return;
        stopTimer();

        clap_window_t window {};
        window.api = ClapInstance::windowApi();
       #if JUCE_WINDOWS
        window.win32 = peer->getNativeHandle();
       #elif JUCE_MAC
        window.cocoa = peer->getNativeHandle();
       #else
        window.x11 = (clap_xwnd) (juce::pointer_sized_uint) peer->getNativeHandle();
       #endif
       #if ! JUCE_MAC
        gui->set_scale (owner.plugin, peer->getPlatformScaleFactor());
       #endif
        attached = gui->set_parent (owner.plugin, &window);
        clapLog ("gui set_parent " + juce::String (attached ? "ok" : "failed"));
        if (attached) gui->show (owner.plugin);
    }

    ClapInstance& owner;
    const clap_plugin_gui_t* gui;
    bool created = false, attached = false, resizingFromPlugin = false;
};

juce::AudioProcessorEditor* ClapInstance::createEditor()
{
    return hasEditor() ? new ClapEditor (*this) : nullptr;
}

bool CLAP_ABI ClapInstance::requestResize (const clap_host_t* h, uint32_t w, uint32_t hgt)
{
    onMain (h, [w, hgt] (ClapInstance& self)
    {
        if (self.editor != nullptr) self.editor->resizeFromPlugin ((int) w, (int) hgt);
    });
    return true;
}
} // namespace

//==============================================================================
void ClapPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& path)
{
    if (! fileMightContainThisPluginType (path)) return;
    juce::String error;
    auto* module = loadModule (path, error);
    if (module == nullptr) return;
    const auto count = (int) module->factory->get_plugin_count (module->factory);
    for (int i = 0; i < count; ++i)
        if (auto* d = module->factory->get_plugin_descriptor (module->factory, (uint32_t) i))
        {
            auto desc = std::make_unique<juce::PluginDescription>();
            describe (*desc, d, path, count);
            results.add (desc.release());
        }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const juce::String& path)
{
    juce::File f (path);
   #if JUCE_MAC
    return f.hasFileExtension (".clap") && f.isDirectory();
   #else
    return f.hasFileExtension (".clap") && f.existsAsFile();
   #endif
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier (const juce::String& path)
{
    return juce::File (path).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).exists();
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins (const juce::FileSearchPath& dirs, bool recursive, bool)
{
    juce::StringArray found;
    for (int i = 0; i < dirs.getNumPaths(); ++i)
    {
        juce::File dir (dirs.getRawString (i));
       #if JUCE_MAC
        for (auto& f : dir.findChildFiles (juce::File::findDirectories, recursive, "*.clap"))
            found.add (f.getFullPathName());
       #else
        for (auto& f : dir.findChildFiles (juce::File::findFiles, recursive, "*.clap"))
            found.add (f.getFullPathName());
       #endif
    }
    return found;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    // The CLAP specification's locations, plus CLAP_PATH.
    juce::FileSearchPath paths;
   #if JUCE_WINDOWS
    paths.add (juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("Common Files").getChildFile ("CLAP"));
    paths.add (juce::File (juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {})).getChildFile ("Programs").getChildFile ("Common").getChildFile ("CLAP"));
    const char sep = ';';
   #elif JUCE_MAC
    paths.add (juce::File ("/Library/Audio/Plug-Ins/CLAP"));
    paths.add (juce::File ("~/Library/Audio/Plug-Ins/CLAP"));
    const char sep = ':';
   #else
    paths.add (juce::File ("~/.clap"));
    paths.add (juce::File ("/usr/lib/clap"));
    paths.add (juce::File ("/usr/local/lib/clap"));
    const char sep = ':';
   #endif
    for (auto& p : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("CLAP_PATH", {}), juce::String::charToString (sep), {}))
        if (p.isNotEmpty())
            paths.add (juce::File (p));
    paths.removeNonExistentPaths();
    return paths;
}

void ClapPluginFormat::createPluginInstance (const juce::PluginDescription& desc, double sampleRate, int blockSize,
                                             PluginCreationCallback callback)
{
    juce::String error;
    auto instance = ClapInstance::create (desc, error);
    if (instance != nullptr)
    {
        instance->setRateAndBufferSizeDetails (sampleRate, blockSize);
        callback (std::move (instance), {});
    }
    else
    {
        callback (nullptr, error.isNotEmpty() ? error : juce::String ("The CLAP plug-in could not be loaded."));
    }
}

//==============================================================================
void addAllPluginFormats (juce::AudioPluginFormatManager& manager)
{
    juce::addDefaultFormatsToManager (manager);
    manager.addFormat (std::make_unique<ClapPluginFormat>());
}

int formatPreference (const juce::String& f)
{
    // CLAP first: built for hosts like this one (per-note expression, thread-safe parameters).
    static const char* order[] { "CLAP", "VST3", "AudioUnit", "LV2", "LADSPA" };
    for (int i = 0; i < (int) std::size (order); ++i)
        if (f.equalsIgnoreCase (order[i])) return i;
    return (int) std::size (order);
}
} // namespace tf
