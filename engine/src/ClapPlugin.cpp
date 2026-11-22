#include "dawg/ClapPlugin.h"

#include <algorithm>
#include <atomic>
#include <clap/clap.h>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#if JUCE_LINUX || JUCE_BSD
 #include <poll.h>
#endif

// A CLAP host on the plain C API (docs/design/clap-hosting.md). The plugin sits behind juce::AudioPluginInstance:
// notes arrive as a MidiBuffer, and parameters as a queue that becomes events in the next block.

namespace dawg
{

namespace
{
thread_local bool isClapAudioThread = false;

// Marks the running thread as the plugin's audio thread for a scope.
struct ScopedAudioThread
{
    ScopedAudioThread() noexcept { isClapAudioThread = true; }
    ~ScopedAudioThread() { isClapAudioThread = false; }
    ScopedAudioThread (const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator= (const ScopedAudioThread&) = delete;
    ScopedAudioThread (ScopedAudioThread&&) = delete;
    ScopedAudioThread& operator= (ScopedAudioThread&&) = delete;
};

[[nodiscard]] juce::File codeFileOf (const juce::File& file)
{
#if JUCE_MAC
    // A bundle: the library is inside.
    if (file.isDirectory())
        return file.getChildFile ("Contents/MacOS").getChildFile (file.getFileNameWithoutExtension());
#endif
    return file;
}

// An opened .clap file, initialised once for as long as anything uses it.
class ClapLibrary final
{
public:
    [[nodiscard]] static std::shared_ptr<ClapLibrary> open (const juce::File& file)
    {
        static std::map<juce::String, std::weak_ptr<ClapLibrary>> opened;
        auto& slot = opened[file.getFullPathName()];

        if (auto existing = slot.lock())
            return existing;

        auto library = std::shared_ptr<ClapLibrary> (new ClapLibrary());

        if (! library->m_library.open (codeFileOf (file).getFullPathName()))
            return nullptr;

        library->m_entry = static_cast<const clap_plugin_entry_t*> (library->m_library.getFunction ("clap_entry"));

        if (library->m_entry == nullptr || ! clap_version_is_compatible (library->m_entry->clap_version)
            || ! library->m_entry->init (file.getFullPathName().toRawUTF8()))
        {
            library->m_entry = nullptr;
            return nullptr;
        }

        library->m_factory =
            static_cast<const clap_plugin_factory_t*> (library->m_entry->get_factory (CLAP_PLUGIN_FACTORY_ID));

        if (library->m_factory == nullptr)
            return nullptr;

        slot = library;
        return library;
    }

    ~ClapLibrary()
    {
        if (m_entry != nullptr)
            m_entry->deinit();
    }

    ClapLibrary (const ClapLibrary&) = delete;
    ClapLibrary& operator= (const ClapLibrary&) = delete;
    ClapLibrary (ClapLibrary&&) = delete;
    ClapLibrary& operator= (ClapLibrary&&) = delete;

    [[nodiscard]] const clap_plugin_factory_t& factory() const noexcept { return *m_factory; }

private:
    ClapLibrary() = default;

    juce::DynamicLibrary m_library;
    const clap_plugin_entry_t* m_entry = nullptr;
    const clap_plugin_factory_t* m_factory = nullptr;
};

[[nodiscard]] juce::String text (const char* value)
{
    return value != nullptr ? juce::String::fromUTF8 (value) : juce::String();
}

[[nodiscard]] bool hasFeature (const clap_plugin_descriptor_t& descriptor, const char* feature)
{
    if (descriptor.features != nullptr)
        for (auto* const* it = descriptor.features; *it != nullptr; ++it)
            if (std::strcmp (*it, feature) == 0)
                return true;

    return false;
}

// The plugin's features ("instrument|synthesizer|stereo"), joined like a VST3's subcategories ("Fx|EQ"), so the add
// menus file both formats' plugins by what they are.
[[nodiscard]] juce::String featuresOf (const clap_plugin_descriptor_t& descriptor, bool instrument)
{
    juce::StringArray features;

    if (descriptor.features != nullptr)
        for (auto* const* it = descriptor.features; *it != nullptr; ++it)
            features.add (text (*it));

    return features.isEmpty() ? juce::String (instrument ? "Instrument" : "Effect") : features.joinIntoString ("|");
}

void describe (juce::PluginDescription& description, const clap_plugin_descriptor_t& descriptor, const juce::File& file)
{
    const auto instrument = hasFeature (descriptor, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    description.name = text (descriptor.name);
    description.descriptiveName = text (descriptor.id);
    description.pluginFormatName = ClapPluginFormat::formatName;
    description.category = featuresOf (descriptor, instrument);
    description.manufacturerName = text (descriptor.vendor);
    description.version = text (descriptor.version);
    description.fileOrIdentifier = file.getFullPathName();
    description.lastFileModTime = file.getLastModificationTime();
    description.lastInfoUpdateTime = juce::Time::getCurrentTime();
    description.uniqueId = text (descriptor.id).hashCode();
    description.deprecatedUid = description.uniqueId;
    description.isInstrument = instrument;
    description.numInputChannels = instrument ? 0 : 2;
    description.numOutputChannels = 2;
    description.hasSharedContainer = false;
}

// The events of one block for the plugin, kept in time order in storage that never grows on the audio thread.
class InputEvents final
{
public:
    InputEvents()
    {
        m_storage.resize (capacityBytes);
        m_offsets.reserve (capacityBytes / sizeof (clap_event_header_t));
        m_list.ctx = this;
        m_list.size = [] (const clap_input_events_t* list) -> uint32_t
        { return static_cast<uint32_t> (static_cast<const InputEvents*> (list->ctx)->m_offsets.size()); };
        m_list.get = [] (const clap_input_events_t* list, uint32_t index) -> const clap_event_header_t*
        {
            const auto& self = *static_cast<const InputEvents*> (list->ctx);
            return index < self.m_offsets.size()
                       ? reinterpret_cast<const clap_event_header_t*> (self.m_storage.data() + self.m_offsets[index])
                       : nullptr;
        };
    }

    void clear() noexcept
    {
        m_used = 0;
        m_offsets.clear();
    }

    // Adds an event after those at the same time or earlier. A full list drops it.
    template <typename Event>
    void add (const Event& event) noexcept
    {
        if (m_used + sizeof (Event) > m_storage.size() || m_offsets.size() == m_offsets.capacity())
            return;

        std::memcpy (m_storage.data() + m_used, &event, sizeof (Event));
        const auto at = std::ranges::upper_bound (
            m_offsets, event.header.time, {}, [this] (size_t offset)
            { return reinterpret_cast<const clap_event_header_t*> (m_storage.data() + offset)->time; });
        m_offsets.insert (at, m_used);
        m_used += sizeof (Event);
    }

    [[nodiscard]] const clap_input_events_t* list() const noexcept { return &m_list; }

private:
    static constexpr size_t capacityBytes = size_t { 256 } * 1024;

    std::vector<std::byte> m_storage;
    std::vector<size_t> m_offsets;
    size_t m_used = 0;
    clap_input_events_t m_list {};
};

template <typename Event>
[[nodiscard]] Event eventOf (uint16_t type, uint32_t time) noexcept
{
    Event event {};
    event.header.size = sizeof (Event);
    event.header.time = time;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    return event;
}

class ClapPluginInstance;

// What the plugin calls back on. It exists before the plugin, which may call it while it is created.
struct ClapHost
{
    clap_host_t host {};
    std::atomic<ClapPluginInstance*> instance { nullptr };

    // Timers and file descriptors a plugin asks for in init(), before its instance exists (Surge XT's editor does on
    // Linux): the instance takes them over once it does.
    std::vector<std::pair<clap_id, uint32_t>> earlyTimers; // id, period in ms
    clap_id nextEarlyTimer = 1;
    std::map<int, clap_posix_fd_flags_t> earlyFds;
};

class ClapParameter final : public juce::HostedAudioProcessorParameter
{
public:
    ClapParameter (ClapPluginInstance& owner, const clap_param_info_t& info, double value)
        : m_owner (owner),
          m_info (info),
          m_value (normalised (value))
    {
    }

    [[nodiscard]] const clap_param_info_t& info() const noexcept { return m_info; }

    [[nodiscard]] float getValue() const override { return m_value.load(); }

    void setValue (float newValue) override;

    // A value already on its way to the plugin in an event.
    void setStored (float value) noexcept { m_value.store (value); }

    // What modulation last sent (audio thread).
    double lastModulation = 0.0;

    // From the plugin: nothing to tell it, only the listeners.
    void setFromPlugin (double plain) noexcept { m_value.store (normalised (plain)); }

    [[nodiscard]] float getDefaultValue() const override { return normalised (m_info.default_value); }

    [[nodiscard]] juce::String getName (int maximumStringLength) const override
    {
        return juce::String::fromUTF8 (m_info.name).substring (0, maximumStringLength);
    }

    [[nodiscard]] juce::String getLabel() const override { return {}; }

    [[nodiscard]] juce::String getText (float normalisedValue, int maximumStringLength) const override;

    [[nodiscard]] float getValueForText (const juce::String& text) const override;

    [[nodiscard]] bool isAutomatable() const override
    {
        return (m_info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0 && (m_info.flags & CLAP_PARAM_IS_HIDDEN) == 0;
    }

    [[nodiscard]] bool isDiscrete() const override { return (m_info.flags & CLAP_PARAM_IS_STEPPED) != 0; }

    [[nodiscard]] int getNumSteps() const override
    {
        return isDiscrete() ? std::max (2, static_cast<int> (m_info.max_value - m_info.min_value) + 1)
                            : juce::AudioProcessor::getDefaultNumParameterSteps();
    }

    // The CLAP id as text: stable across versions, as automation lanes need (C4).
    [[nodiscard]] juce::String getParameterID() const override { return juce::String (m_info.id); }

    [[nodiscard]] double plainFrom (float normalisedValue) const noexcept
    {
        const auto plain =
            m_info.min_value + static_cast<double> (normalisedValue) * (m_info.max_value - m_info.min_value);
        return isDiscrete() ? std::round (plain) : plain;
    }

private:
    [[nodiscard]] float normalised (double plain) const noexcept
    {
        const auto range = m_info.max_value - m_info.min_value;
        return range > 0.0 ? static_cast<float> (std::clamp ((plain - m_info.min_value) / range, 0.0, 1.0)) : 0.0f;
    }

    ClapPluginInstance& m_owner;
    clap_param_info_t m_info;
    std::atomic<float> m_value;
};

class ClapPluginInstance final : public juce::AudioPluginInstance,
                                 public ClapEditorAccess,
                                 public ClapNativeInput,
                                 public ParameterListSource,
                                 public DuplicateStateSource,
                                 private juce::AsyncUpdater,
                                 private juce::MultiTimer
{
public:
    struct Ports
    {
        std::vector<clap_audio_port_info_t> inputs;
        std::vector<clap_audio_port_info_t> outputs;
    };

    [[nodiscard]] static std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& description,
                                                                            juce::String& error)
    {
        const juce::File file (description.fileOrIdentifier);
        auto library = ClapLibrary::open (file);

        if (library == nullptr)
        {
            error = "Could not load " + file.getFileName();
            return nullptr;
        }

        auto host = std::make_unique<ClapHost>();
        fillHost (*host);
        const auto id = description.descriptiveName;
        const auto* plugin = library->factory().create_plugin (&library->factory(), &host->host, id.toRawUTF8());

        if (plugin == nullptr)
        {
            error = "Could not create " + description.name;
            return nullptr;
        }

        if (! plugin->init (plugin))
        {
            plugin->destroy (plugin);
            error = "Could not initialise " + description.name;
            return nullptr;
        }

        auto ports = readPorts (*plugin);
        return std::unique_ptr<juce::AudioPluginInstance> (
            new ClapPluginInstance (description, std::move (library), std::move (host), plugin, std::move (ports)));
    }

    ~ClapPluginInstance() override
    {
        cancelPendingUpdate();
        for (clap_id id = 1; id < m_nextTimer; ++id)
            stopTimer (static_cast<int> (id));

#if JUCE_LINUX || JUCE_BSD
        for (const auto& [fd, flags] : m_fds)
            juce::LinuxEventLoop::unregisterFdCallback (fd);
#endif

        destroyEditor();
        m_host->instance.store (nullptr);
        deactivate();
        m_plugin->destroy (m_plugin);
    }

    ClapPluginInstance (const ClapPluginInstance&) = delete;
    ClapPluginInstance& operator= (const ClapPluginInstance&) = delete;
    ClapPluginInstance (ClapPluginInstance&&) = delete;
    ClapPluginInstance& operator= (ClapPluginInstance&&) = delete;

    void fillInPluginDescription (juce::PluginDescription& description) const override { description = m_description; }

    [[nodiscard]] const juce::String getName() const override { return m_description.name; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        m_listFixed = true;
        m_sampleRate = sampleRate;
        m_maxBlock = std::max (1, samplesPerBlock);
        deactivate();
        allocatePorts();
        activate();
    }

    void releaseResources() override { deactivate(); }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto matches =
            [] (const juce::Array<juce::AudioChannelSet>& buses, const std::vector<clap_audio_port_info_t>& ports)
        {
            if (static_cast<size_t> (buses.size()) != ports.size())
                return false;

            // A main bus has the port's channels; any other may also be off.
            for (size_t i = 0; i < ports.size(); ++i)
            {
                const auto& bus = buses.getReference (static_cast<int> (i));

                if (bus.size() != static_cast<int> (ports[i].channel_count) && (i == 0 || ! bus.isDisabled()))
                    return false;
            }

            return true;
        };

        return matches (layouts.inputBuses, m_ports.inputs) && matches (layouts.outputBuses, m_ports.outputs);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const ScopedAudioThread audioThread;
        const auto numSamples = std::min (buffer.getNumSamples(), m_maxBlock);

        if (! m_active || numSamples <= 0)
        {
            buffer.clear();
            return;
        }

        if (! m_processing)
            m_processing = m_plugin->start_processing (m_plugin);

        if (! m_processing)
        {
            buffer.clear();
            return;
        }

        queueParameterChanges();
        queueMidi (midi, numSamples);
        copyInputs (buffer, numSamples);

        clap_event_transport_t transport {};
        fillTransport (transport);

        clap_process_t process {};
        process.steady_time = m_steadyTime;
        process.frames_count = static_cast<uint32_t> (numSamples);
        process.transport = &transport;
        process.audio_inputs = m_inputBuffers.data();
        process.audio_outputs = m_outputBuffers.data();
        process.audio_inputs_count = static_cast<uint32_t> (m_inputBuffers.size());
        process.audio_outputs_count = static_cast<uint32_t> (m_outputBuffers.size());
        process.in_events = m_events.list();
        process.out_events = &m_outEvents;

        const auto status = m_plugin->process (m_plugin, &process);
        m_events.clear();

        if (status == CLAP_PROCESS_ERROR)
        {
            buffer.clear();
            return;
        }

        m_steadyTime += numSamples;
        copyOutputs (buffer, numSamples);
        midi.clear();
    }

    using juce::AudioPluginInstance::processBlock;

    [[nodiscard]] double getTailLengthSeconds() const override
    {
        const auto* tail = extension<clap_plugin_tail_t> (CLAP_EXT_TAIL);
        return tail != nullptr && m_sampleRate > 0.0 ? tail->get (m_plugin) / m_sampleRate : 0.0;
    }

    [[nodiscard]] bool acceptsMidi() const override { return m_description.isInstrument || m_notePorts > 0; }

    [[nodiscard]] bool producesMidi() const override { return false; }

    // Export: the plugin may take its time (docs/design/clap-hosting.md, section 3.4).
    void setNonRealtime (bool isNonRealtime) noexcept override
    {
        juce::AudioPluginInstance::setNonRealtime (isNonRealtime);

        if (const auto* render = extension<clap_plugin_render_t> (CLAP_EXT_RENDER))
            std::ignore = render->set (m_plugin, isNonRealtime ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
    }

    [[nodiscard]] bool takesNativeNotes() const noexcept override { return m_acceptsClapNotes; }

    [[nodiscard]] bool playsOverlappingNotes() const noexcept override { return m_overlappingNotes; }

    void noteOn (uint32_t time, int32_t noteId, int key, double velocity) noexcept override
    {
        sendNote (CLAP_EVENT_NOTE_ON, time, noteId, key, velocity);
    }

    void noteOff (uint32_t time, int32_t noteId, int key, double velocity) noexcept override
    {
        sendNote (CLAP_EVENT_NOTE_OFF, time, noteId, key, velocity);
    }

    void noteChoke (uint32_t time, int32_t noteId, int key) noexcept override
    {
        sendNote (CLAP_EVENT_NOTE_CHOKE, time, noteId, key, 0.0);
    }

    void chokeAll (uint32_t time) noexcept override { sendNote (CLAP_EVENT_NOTE_CHOKE, time, -1, -1, 0.0); }

    void noteExpression (uint32_t time, int32_t noteId, int key, Expression which, double value) noexcept override
    {
        auto event = eventOf<clap_event_note_expression_t> (CLAP_EVENT_NOTE_EXPRESSION, time);
        event.expression_id = CLAP_NOTE_EXPRESSION_BRIGHTNESS;

        if (which == Expression::tuning)
            event.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
        else if (which == Expression::pressure)
            event.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
        event.note_id = noteId;
        event.port_index = 0;
        event.channel = 0;
        event.key = static_cast<int16_t> (key);
        event.value = value;
        m_events.add (event);
    }

    void controller (uint32_t time, uint8_t status, uint8_t first, uint8_t second) noexcept override
    {
        if (! m_acceptsMidi)
            return;

        auto event = eventOf<clap_event_midi_t> (CLAP_EVENT_MIDI, time);
        event.port_index = 0;
        event.data[0] = status;
        event.data[1] = first;
        event.data[2] = second;
        m_events.add (event);
    }

    [[nodiscard]] bool isModulatable (int parameter) const noexcept override
    {
        const auto* found = parameterAt (parameter);
        return found != nullptr && (found->info().flags & CLAP_PARAM_IS_MODULATABLE) != 0;
    }

    void automateParameter (int parameter, float value, uint32_t time) noexcept override
    {
        auto* found = const_cast<ClapParameter*> (parameterAt (parameter));

        if (found == nullptr || ! m_active || std::abs (found->getValue() - value) < 1.0e-6f)
            return;

        found->setStored (value);
        auto event = eventOf<clap_event_param_value_t> (CLAP_EVENT_PARAM_VALUE, time);
        event.param_id = found->info().id;
        event.cookie = found->info().cookie;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = found->plainFrom (value);
        m_events.add (event);
    }

    void modulateParameter (int parameter, double amount, uint32_t time) noexcept override
    {
        auto* found = const_cast<ClapParameter*> (parameterAt (parameter));

        if (found == nullptr || ! m_active || std::abs (found->lastModulation - amount) < 1.0e-6)
            return;

        found->lastModulation = amount;
        auto event = eventOf<clap_event_param_mod_t> (CLAP_EVENT_PARAM_MOD, time);
        event.param_id = found->info().id;
        event.cookie = found->info().cookie;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.amount = amount * (found->info().max_value - found->info().min_value);
        m_events.add (event);
    }

    [[nodiscard]] bool isModulatablePerNote (int parameter) const noexcept override
    {
        const auto* found = parameterAt (parameter);
        return m_acceptsClapNotes && found != nullptr
               && (found->info().flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) != 0;
    }

    void modulateNoteParameter (uint32_t time, int32_t noteId, int key, int parameter, double amount) noexcept override
    {
        const auto* found = parameterAt (parameter);

        if (found == nullptr)
            return;

        auto event = eventOf<clap_event_param_mod_t> (CLAP_EVENT_PARAM_MOD, time);
        event.param_id = found->info().id;
        event.cookie = found->info().cookie;
        event.note_id = noteId;
        event.port_index = 0;
        event.channel = 0;
        event.key = static_cast<int16_t> (key);
        event.amount = amount * (found->info().max_value - found->info().min_value);
        m_events.add (event);
    }

    void setNoteEndListener (ClapNoteEndListener* listener) noexcept override { m_noteEnds = listener; }

    // The app embeds the editor (ClapEditorAccess): this module has no windows.
    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    // The windowing system editors embed in here: Win32 windows on Windows, X11 on Linux (section 3.3). macOS isn't
    // built yet.
    [[nodiscard]] static const char* windowApi() noexcept
    {
#if JUCE_WINDOWS
        return CLAP_WINDOW_API_WIN32;
#elif JUCE_LINUX || JUCE_BSD
        return CLAP_WINDOW_API_X11;
#else
        return nullptr;
#endif
    }

    [[nodiscard]] bool hasEmbeddedEditor() const override
    {
        const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI);
        return windowApi() != nullptr && gui != nullptr && gui->is_api_supported (m_plugin, windowApi(), false);
    }

    [[nodiscard]] bool createEditor (double scale) override
    {
        const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI);

        if (windowApi() == nullptr || gui == nullptr || m_guiCreated || ! gui->create (m_plugin, windowApi(), false))
            return false;

        m_guiCreated = true;
        std::ignore = gui->set_scale (m_plugin, scale);
        return true;
    }

    [[nodiscard]] bool attachEditor (void* parent) override
    {
        const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI);

        if (windowApi() == nullptr || gui == nullptr || ! m_guiCreated)
            return false;

        clap_window_t window {};
        window.api = windowApi();
#if JUCE_WINDOWS
        window.win32 = parent;
#else
        window.x11 = static_cast<clap_xwnd> (reinterpret_cast<uintptr_t> (parent));
#endif

        if (! gui->set_parent (m_plugin, &window))
            return false;

        std::ignore = gui->show (m_plugin);
        return true;
    }

    void destroyEditor() override
    {
        if (! m_guiCreated)
            return;

        m_guiCreated = false;

        if (const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI))
            gui->destroy (m_plugin);
    }

    [[nodiscard]] std::pair<uint32_t, uint32_t> editorSize() const override
    {
        uint32_t width = 400;
        uint32_t height = 300;

        if (const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI); gui != nullptr && m_guiCreated)
            std::ignore = gui->get_size (m_plugin, &width, &height);

        return { width, height };
    }

    [[nodiscard]] bool editorCanResize() const override
    {
        const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI);
        return gui != nullptr && m_guiCreated && gui->can_resize (m_plugin);
    }

    void setEditorSize (uint32_t width, uint32_t height) override
    {
        const auto* gui = extension<clap_plugin_gui_t> (CLAP_EXT_GUI);

        if (gui == nullptr || ! m_guiCreated || ! gui->can_resize (m_plugin))
            return;

        if (gui->adjust_size (m_plugin, &width, &height))
            std::ignore = gui->set_size (m_plugin, width, height);
    }

    void setEditorResizeCallback (std::function<void (uint32_t, uint32_t)> callback) override
    {
        m_editorResize = std::move (callback);
    }

    // The plugin asks for an editor size: the window follows.
    [[nodiscard]] bool requestEditorResize (uint32_t width, uint32_t height)
    {
        if (m_editorResize == nullptr)
            return false;

        m_editorResize (width, height);
        return true;
    }

    // Timers for the plugin, on the message thread.
    [[nodiscard]] bool registerTimer (uint32_t periodMs, clap_id& id)
    {
        id = m_nextTimer++;
        startTimer (static_cast<int> (id), static_cast<int> (std::max<uint32_t> (1, periodMs)));
        return true;
    }

    [[nodiscard]] bool unregisterTimer (clap_id id)
    {
        stopTimer (static_cast<int> (id));
        return true;
    }

#if JUCE_LINUX || JUCE_BSD
    // File descriptors the plugin watches, on JUCE's event loop, which runs on the message thread (posix-fd-support).
    // Editors on Linux need it: theirs is how X11 events reach them.
    [[nodiscard]] bool registerFd (int fd, clap_posix_fd_flags_t flags)
    {
        if (fd < 0 || m_fds.contains (fd))
            return false;

        m_fds[fd] = flags;
        short events = 0;

        if ((flags & CLAP_POSIX_FD_READ) != 0)
            events |= POLLIN;

        if ((flags & CLAP_POSIX_FD_WRITE) != 0)
            events |= POLLOUT;

        if ((flags & CLAP_POSIX_FD_ERROR) != 0)
            events |= POLLERR;

        juce::LinuxEventLoop::registerFdCallback (
            fd,
            [this] (int ready)
            {
                const auto found = m_fds.find (ready);
                const auto* support = extension<clap_plugin_posix_fd_support_t> (CLAP_EXT_POSIX_FD_SUPPORT);

                if (found != m_fds.end() && support != nullptr)
                    support->on_fd (m_plugin, ready, found->second);
            },
            events);
        return true;
    }

    [[nodiscard]] bool modifyFd (int fd, clap_posix_fd_flags_t flags)
    {
        return unregisterFd (fd) && registerFd (fd, flags);
    }

    [[nodiscard]] bool unregisterFd (int fd)
    {
        if (m_fds.erase (fd) == 0)
            return false;

        juce::LinuxEventLoop::unregisterFdCallback (fd);
        return true;
    }
#endif

    [[nodiscard]] bool hasEditor() const override { return false; }

    [[nodiscard]] int getNumPrograms() override { return 1; }

    [[nodiscard]] int getCurrentProgram() override { return 0; }

    void setCurrentProgram (int /*index*/) override {}

    [[nodiscard]] const juce::String getProgramName (int /*index*/) override { return {}; }

    void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        saveState (destData, CLAP_STATE_CONTEXT_FOR_PROJECT);
    }

    void getStateForDuplicate (juce::MemoryBlock& destData) override
    {
        saveState (destData, CLAP_STATE_CONTEXT_FOR_DUPLICATE);
    }

    // The state through the plugin's state context where it has one, which says why it's saved.
    void saveState (juce::MemoryBlock& destData, clap_plugin_state_context_type why)
    {
        destData.reset();
        const auto* state = extension<clap_plugin_state_t> (CLAP_EXT_STATE);

        if (state == nullptr)
            return;

        juce::MemoryOutputStream stream (destData, false);
        clap_ostream_t out {};
        out.ctx = &stream;
        out.write = [] (const clap_ostream_t* self, const void* data, uint64_t size) -> int64_t
        {
            return static_cast<juce::MemoryOutputStream*> (self->ctx)->write (data, static_cast<size_t> (size))
                       ? static_cast<int64_t> (size)
                       : -1;
        };

        const auto* context = extension<clap_plugin_state_context_t> (CLAP_EXT_STATE_CONTEXT);
        const auto saved = context != nullptr ? context->save (m_plugin, &out, why) : state->save (m_plugin, &out);

        if (! saved)
            destData.reset();
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        const auto* state = extension<clap_plugin_state_t> (CLAP_EXT_STATE);

        if (state == nullptr)
            return;

        juce::MemoryInputStream stream (data, static_cast<size_t> (sizeInBytes), false);
        clap_istream_t in {};
        in.ctx = &stream;
        in.read = [] (const clap_istream_t* self, void* buffer, uint64_t size) -> int64_t
        { return static_cast<juce::MemoryInputStream*> (self->ctx)->read (buffer, static_cast<int> (size)); };

        const auto* context = extension<clap_plugin_state_context_t> (CLAP_EXT_STATE_CONTEXT);
        std::ignore = context != nullptr ? context->load (m_plugin, &in, CLAP_STATE_CONTEXT_FOR_PROJECT)
                                         : state->load (m_plugin, &in);

        // A state can bring other parameters, which is read before the first prepare whether or not the plugin says so.
        if (! m_listFixed)
            refreshParameterList();

        refreshParameterValues();
    }

    // A parameter moved in the host: it reaches the plugin in the next block, or at once while none runs.
    void parameterChanged (const ClapParameter& parameter, float value) noexcept
    {
        int start = 0;
        int size = 0;
        int start2 = 0;
        int size2 = 0;
        m_fifo.prepareToWrite (1, start, size, start2, size2);

        if (size > 0)
        {
            m_changes[static_cast<size_t> (start)] = { &parameter, value };
            m_fifo.finishedWrite (1);
        }

        if (! m_active)
        {
            m_wantsFlush.store (true);
            triggerAsyncUpdate();
        }
    }

    [[nodiscard]] juce::String parameterText (const clap_param_info_t& info, double plain) const
    {
        if (const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS))
        {
            std::array<char, 256> out {};

            if (params->value_to_text (m_plugin, info.id, plain, out.data(), static_cast<uint32_t> (out.size())))
                return juce::String::fromUTF8 (out.data());
        }

        return juce::String (plain);
    }

    [[nodiscard]] std::optional<double> parameterFromText (const clap_param_info_t& info,
                                                           const juce::String& value) const
    {
        double plain = 0.0;

        if (const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);
            params != nullptr && params->text_to_value (m_plugin, info.id, value.toRawUTF8(), &plain))
            return plain;

        return std::nullopt;
    }

    // The plugin's requests, which may come from any thread (C6).
    void requestCallback() noexcept
    {
        m_wantsCallback.store (true);
        triggerAsyncUpdate();
    }

    void requestRestart() noexcept
    {
        m_wantsRestart.store (true);
        triggerAsyncUpdate();
    }

    void requestFlush() noexcept
    {
        m_wantsFlush.store (true);
        triggerAsyncUpdate();
    }

    void latencyChanged() noexcept
    {
        m_wantsLatency.store (true);
        triggerAsyncUpdate();
    }

    void rescanValues() noexcept
    {
        m_wantsValues.store (true);
        triggerAsyncUpdate();
    }

    // Parameters added or removed (ParameterListSource): read again before the first prepare, else the host makes a new
    // instance.
    void parameterListChanged()
    {
        if (! m_listFixed)
            refreshParameterList();
        else if (m_parameterList != nullptr)
            m_parameterList();
    }

    void setParameterListCallback (std::function<void()> callback) override { m_parameterList = std::move (callback); }

private:
    struct Change
    {
        const ClapParameter* parameter = nullptr;
        float value = 0.0f;
    };

    static constexpr int maxChanges = 4096;

    ClapPluginInstance (const juce::PluginDescription& description, std::shared_ptr<ClapLibrary> library,
                        std::unique_ptr<ClapHost> host, const clap_plugin_t* plugin, Ports ports)
        : juce::AudioPluginInstance (busesOf (ports)),
          m_description (description),
          m_library (std::move (library)),
          m_host (std::move (host)),
          m_plugin (plugin),
          m_ports (std::move (ports))
    {
        m_host->instance.store (this);
        m_changes.resize (static_cast<size_t> (maxChanges));

        // What the plugin asked for while it initialised.
        m_nextTimer = m_host->nextEarlyTimer;

        for (const auto& [id, period] : m_host->earlyTimers)
            startTimer (static_cast<int> (id), static_cast<int> (std::max<uint32_t> (1, period)));

#if JUCE_LINUX || JUCE_BSD
        for (const auto& [fd, flags] : m_host->earlyFds)
            std::ignore = registerFd (fd, flags);
#endif
        m_host->earlyTimers.clear();
        m_host->earlyFds.clear();

        m_outEvents.ctx = this;
        m_outEvents.try_push = [] (const clap_output_events_t* list, const clap_event_header_t* event) -> bool
        {
            static_cast<ClapPluginInstance*> (list->ctx)->handleOutput (*event);
            return true;
        };

        if (const auto* notes = extension<clap_plugin_note_ports_t> (CLAP_EXT_NOTE_PORTS))
        {
            m_notePorts = notes->count (m_plugin, true);
            clap_note_port_info_t info {};

            if (m_notePorts > 0 && notes->get (m_plugin, 0, true, &info))
            {
                m_acceptsMidi = (info.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
                m_acceptsClapNotes = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
            }
        }

        if (const auto* voices = extension<clap_plugin_voice_info_t> (CLAP_EXT_VOICE_INFO))
        {
            clap_voice_info_t info {};
            m_overlappingNotes =
                voices->get (m_plugin, &info) && (info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0;
        }

        addParameters();
    }

    void sendNote (uint16_t type, uint32_t time, int32_t noteId, int key, double velocity) noexcept
    {
        auto event = eventOf<clap_event_note_t> (type, time);
        event.note_id = noteId;
        event.port_index = noteId < 0 ? -1 : 0;
        event.channel = noteId < 0 ? -1 : 0;
        event.key = static_cast<int16_t> (key);
        event.velocity = velocity;
        m_events.add (event);
    }

    [[nodiscard]] const ClapParameter* parameterAt (int index) const noexcept
    {
        const auto& parameters = getParameters();
        return index >= 0 && index < parameters.size()
                   ? static_cast<const ClapParameter*> (parameters.getUnchecked (index))
                   : nullptr;
    }

    template <typename Extension>
    [[nodiscard]] const Extension* extension (const char* id) const noexcept
    {
        return static_cast<const Extension*> (m_plugin->get_extension (m_plugin, id));
    }

    [[nodiscard]] static Ports readPorts (const clap_plugin_t& plugin)
    {
        Ports ports;
        const auto* audio =
            static_cast<const clap_plugin_audio_ports_t*> (plugin.get_extension (&plugin, CLAP_EXT_AUDIO_PORTS));

        if (audio == nullptr)
            return ports;

        for (const auto input : { true, false })
        {
            for (uint32_t i = 0; i < audio->count (&plugin, input); ++i)
            {
                clap_audio_port_info_t info {};

                if (audio->get (&plugin, i, input, &info))
                    (input ? ports.inputs : ports.outputs).push_back (info);
            }
        }

        return ports;
    }

    [[nodiscard]] static juce::AudioChannelSet channelSet (uint32_t channels)
    {
        if (channels == 1)
            return juce::AudioChannelSet::mono();

        if (channels == 2)
            return juce::AudioChannelSet::stereo();

        return juce::AudioChannelSet::discreteChannels (static_cast<int> (channels));
    }

    [[nodiscard]] static BusesProperties busesOf (const Ports& ports)
    {
        BusesProperties buses;

        for (size_t i = 0; i < ports.inputs.size(); ++i)
            buses = buses.withInput (i == 0 ? "Input" : "Sidechain " + juce::String (i),
                                     channelSet (ports.inputs[i].channel_count), i == 0);

        for (size_t i = 0; i < ports.outputs.size(); ++i)
            buses = buses.withOutput (juce::String (ports.outputs[i].name).isNotEmpty()
                                          ? juce::String (ports.outputs[i].name)
                                          : juce::String ("Output ") + juce::String (i + 1),
                                      channelSet (ports.outputs[i].channel_count), i == 0);

        return buses;
    }

    static void fillHost (ClapHost& owner)
    {
        const auto instanceOf = [] (const clap_host_t* host)
        { return static_cast<ClapHost*> (host->host_data)->instance.load(); };
        static const auto instanceFrom = instanceOf;

        auto& host = owner.host;
        host.clap_version = CLAP_VERSION;
        host.host_data = &owner;
        host.name = "DAWg";
        host.vendor = "DAWg";
        host.url = "";
        host.version = "0.1.0";
        host.get_extension = [] (const clap_host_t*, const char* id) -> const void*
        {
            static const clap_host_thread_check_t threadCheck {
                [] (const clap_host_t*) -> bool { return juce::MessageManager::existsAndIsCurrentThread(); },
                [] (const clap_host_t*) -> bool { return isClapAudioThread; },
            };
            static const clap_host_log_t log { [] (const clap_host_t*, clap_log_severity, const char* message)
                                               {
                                                   juce::ignoreUnused (message);
                                                   DBG ("CLAP: " << text (message));
                                               } };
            static const clap_host_latency_t latency { [] (const clap_host_t* self)
                                                       {
                                                           if (auto* instance = instanceFrom (self))
                                                               instance->latencyChanged();
                                                       } };
            static const clap_host_state_t state { [] (const clap_host_t*) {} };
            static const clap_host_gui_t gui {
                [] (const clap_host_t*) {},
                [] (const clap_host_t* self, uint32_t width, uint32_t height) -> bool
                {
                    auto* instance = instanceFrom (self);
                    return instance != nullptr && instance->requestEditorResize (width, height);
                },
                [] (const clap_host_t*) -> bool { return false; },
                [] (const clap_host_t*) -> bool { return false; },
                [] (const clap_host_t*, bool) {},
            };
            static const clap_host_timer_support_t timers {
                [] (const clap_host_t* self, uint32_t period, clap_id* timerId) -> bool
                {
                    if (auto* instance = instanceFrom (self))
                        return instance->registerTimer (period, *timerId);

                    auto& early = *static_cast<ClapHost*> (self->host_data);
                    *timerId = early.nextEarlyTimer++;
                    early.earlyTimers.emplace_back (*timerId, period);
                    return true;
                },
                [] (const clap_host_t* self, clap_id timerId) -> bool
                {
                    if (auto* instance = instanceFrom (self))
                        return instance->unregisterTimer (timerId);

                    auto& early = static_cast<ClapHost*> (self->host_data)->earlyTimers;
                    return std::erase_if (early, [timerId] (const auto& timer) { return timer.first == timerId; }) > 0;
                },
            };
            static const clap_host_params_t params {
                [] (const clap_host_t* self, clap_param_rescan_flags flags)
                {
                    auto* instance = instanceFrom (self);

                    if (instance == nullptr)
                        return;

                    if ((flags & CLAP_PARAM_RESCAN_ALL) != 0)
                        instance->parameterListChanged();
                    else if ((flags & CLAP_PARAM_RESCAN_VALUES) != 0)
                        instance->rescanValues();
                },
                [] (const clap_host_t*, clap_id, clap_param_clear_flags) {},
                [] (const clap_host_t* self)
                {
                    if (auto* instance = instanceFrom (self))
                        instance->requestFlush();
                },
            };

            if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0)
                return &threadCheck;
            if (std::strcmp (id, CLAP_EXT_LOG) == 0)
                return &log;
            if (std::strcmp (id, CLAP_EXT_LATENCY) == 0)
                return &latency;
            if (std::strcmp (id, CLAP_EXT_STATE) == 0)
                return &state;
            if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
                return &params;
            if (std::strcmp (id, CLAP_EXT_GUI) == 0)
                return &gui;
            if (std::strcmp (id, CLAP_EXT_TIMER_SUPPORT) == 0)
                return &timers;
#if JUCE_LINUX || JUCE_BSD
            static const clap_host_posix_fd_support_t posixFds {
                [] (const clap_host_t* self, int fd, clap_posix_fd_flags_t flags) -> bool
                {
                    if (auto* instance = instanceFrom (self))
                        return instance->registerFd (fd, flags);

                    return static_cast<ClapHost*> (self->host_data)->earlyFds.try_emplace (fd, flags).second;
                },
                [] (const clap_host_t* self, int fd, clap_posix_fd_flags_t flags) -> bool
                {
                    if (auto* instance = instanceFrom (self))
                        return instance->modifyFd (fd, flags);

                    auto& early = static_cast<ClapHost*> (self->host_data)->earlyFds;
                    const auto found = early.find (fd);

                    if (found == early.end())
                        return false;

                    found->second = flags;
                    return true;
                },
                [] (const clap_host_t* self, int fd) -> bool
                {
                    if (auto* instance = instanceFrom (self))
                        return instance->unregisterFd (fd);

                    return static_cast<ClapHost*> (self->host_data)->earlyFds.erase (fd) > 0;
                },
            };

            if (std::strcmp (id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)
                return &posixFds;
#endif

            return nullptr;
        };
        host.request_restart = [] (const clap_host_t* self)
        {
            if (auto* instance = instanceFrom (self))
                instance->requestRestart();
        };
        host.request_process = [] (const clap_host_t*) {}; // every plugin runs every block
        host.request_callback = [] (const clap_host_t* self)
        {
            if (auto* instance = instanceFrom (self))
                instance->requestCallback();
        };
    }

    void addParameters()
    {
        const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);

        if (params == nullptr)
            return;

        for (uint32_t i = 0; i < params->count (m_plugin); ++i)
        {
            clap_param_info_t info {};

            if (! params->get_info (m_plugin, i, &info))
                continue;

            auto value = info.default_value;
            std::ignore = params->get_value (m_plugin, info.id, &value);
            auto parameter = std::make_unique<ClapParameter> (*this, info, value);
            m_parametersById[info.id] = parameter.get();
            addHostedParameter (std::move (parameter));
        }
    }

    // Reads the parameter list again where it changed: only while nothing uses it, before the first prepare.
    void refreshParameterList()
    {
        const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);

        if (params == nullptr)
            return;

        auto same = std::cmp_equal (params->count (m_plugin), getParameters().size());

        for (uint32_t i = 0; same && i < params->count (m_plugin); ++i)
        {
            clap_param_info_t info {};
            same = params->get_info (m_plugin, i, &info)
                   && static_cast<const ClapParameter*> (getParameters()[static_cast<int> (i)])->info().id == info.id;
        }

        if (same)
            return;

        m_parametersById.clear();
        setHostedParameterTree ({});
        addParameters();
    }

    void refreshParameterValues()
    {
        const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);

        if (params == nullptr)
            return;

        for (const auto& [id, parameter] : m_parametersById)
        {
            double value = 0.0;

            if (params->get_value (m_plugin, id, &value))
                parameter->setFromPlugin (value);
        }
    }

    void allocatePorts()
    {
        const auto channels = [] (const std::vector<clap_audio_port_info_t>& ports)
        {
            int total = 0;

            for (const auto& port : ports)
                total += static_cast<int> (port.channel_count);

            return total;
        };

        m_inputAudio.setSize (std::max (1, channels (m_ports.inputs)), m_maxBlock);
        m_outputAudio.setSize (std::max (1, channels (m_ports.outputs)), m_maxBlock);
        m_inputAudio.clear();
        m_outputAudio.clear();
        const auto fill = [] (juce::AudioBuffer<float>& audio, const std::vector<clap_audio_port_info_t>& ports,
                              std::vector<clap_audio_buffer_t>& buffers)
        {
            buffers.clear();
            int next = 0;

            for (const auto& port : ports)
            {
                clap_audio_buffer_t buffer {};
                buffer.data32 = const_cast<float**> (audio.getArrayOfWritePointers()) + next;
                buffer.channel_count = port.channel_count;
                buffers.push_back (buffer);
                next += static_cast<int> (port.channel_count);
            }
        };

        fill (m_inputAudio, m_ports.inputs, m_inputBuffers);
        fill (m_outputAudio, m_ports.outputs, m_outputBuffers);
    }

    void activate()
    {
        if (m_active || m_sampleRate <= 0.0)
            return;

        // Waiting changes go through flush, since no block runs yet.
        flushParameters();
        m_active = m_plugin->activate (m_plugin, m_sampleRate, 1, static_cast<uint32_t> (m_maxBlock));

        if (const auto* latency = extension<clap_plugin_latency_t> (CLAP_EXT_LATENCY); latency != nullptr && m_active)
            setLatencySamples (static_cast<int> (latency->get (m_plugin)));
    }

    void deactivate()
    {
        if (! m_active)
            return;

        if (m_processing)
        {
            const ScopedAudioThread audioThread; // no block runs: this thread has the audio thread's role
            m_plugin->stop_processing (m_plugin);
            m_processing = false;
        }

        m_plugin->deactivate (m_plugin);
        m_active = false;
    }

    // Parameter changes while no block runs, through params.flush on the main thread (C4).
    void flushParameters()
    {
        const auto* params = extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);

        if (params == nullptr)
            return;

        m_events.clear();
        queueParameterChanges();
        params->flush (m_plugin, m_events.list(), &m_outEvents);
        m_events.clear();
    }

    void queueParameterChanges() noexcept
    {
        int start = 0;
        int size = 0;
        int start2 = 0;
        int size2 = 0;
        m_fifo.prepareToRead (m_fifo.getNumReady(), start, size, start2, size2);
        const auto send = [this] (int from, int count)
        {
            for (int i = from; i < from + count; ++i)
            {
                const auto& change = m_changes[static_cast<size_t> (i)];
                auto event = eventOf<clap_event_param_value_t> (CLAP_EVENT_PARAM_VALUE, 0);
                event.param_id = change.parameter->info().id;
                event.cookie = change.parameter->info().cookie;
                event.note_id = -1;
                event.port_index = -1;
                event.channel = -1;
                event.key = -1;
                event.value = change.parameter->plainFrom (change.value);
                m_events.add (event);
            }
        };
        send (start, size);
        send (start2, size2);
        m_fifo.finishedRead (size + size2);
    }

    void queueMidi (const juce::MidiBuffer& midi, int numSamples) noexcept
    {
        for (const auto metadata : midi)
        {
            const auto time = static_cast<uint32_t> (std::clamp (metadata.samplePosition, 0, numSamples - 1));

            if (metadata.numBytes > 3)
                continue;

            if (m_acceptsMidi)
            {
                auto event = eventOf<clap_event_midi_t> (CLAP_EVENT_MIDI, time);
                event.port_index = 0;
                std::memcpy (event.data, metadata.data, static_cast<size_t> (metadata.numBytes));
                m_events.add (event);
                continue;
            }

            // A plugin that only takes CLAP's own notes: the notes go through; expression needs the native path.
            const auto message = metadata.getMessage();

            if (! message.isNoteOnOrOff())
                continue;

            auto event = eventOf<clap_event_note_t> (
                static_cast<uint16_t> (message.isNoteOn() ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), time);
            event.note_id = -1;
            event.port_index = 0;
            event.channel = static_cast<int16_t> (message.getChannel() - 1);
            event.key = static_cast<int16_t> (message.getNoteNumber());
            event.velocity = static_cast<double> (message.getFloatVelocity());
            m_events.add (event);
        }
    }

    void copyInputs (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        int port = 0;
        int next = 0;

        for (const auto& info : m_ports.inputs)
        {
            const auto available = getChannelCountOfBus (true, port);

            for (int ch = 0; ch < static_cast<int> (info.channel_count); ++ch)
            {
                if (ch < available)
                    m_inputAudio.copyFrom (next + ch, 0, buffer, getChannelIndexInProcessBlockBuffer (true, port, ch),
                                           0, numSamples);
                else
                    m_inputAudio.clear (next + ch, 0, numSamples);
            }

            next += static_cast<int> (info.channel_count);
            ++port;
        }
    }

    void copyOutputs (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        int port = 0;
        int next = 0;

        for (const auto& info : m_ports.outputs)
        {
            const auto available = getChannelCountOfBus (false, port);

            for (int ch = 0; ch < std::min (available, static_cast<int> (info.channel_count)); ++ch)
                buffer.copyFrom (getChannelIndexInProcessBlockBuffer (false, port, ch), 0, m_outputAudio, next + ch, 0,
                                 numSamples);

            next += static_cast<int> (info.channel_count);
            ++port;
        }
    }

    void fillTransport (clap_event_transport_t& transport) const noexcept
    {
        transport.header.size = sizeof (transport);
        transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        transport.header.type = CLAP_EVENT_TRANSPORT;
        const auto* head = getPlayHead();

        if (head == nullptr)
            return;

        const auto position = head->getPosition();

        if (! position.hasValue())
            return;

        if (position->getIsPlaying())
            transport.flags |= CLAP_TRANSPORT_IS_PLAYING;

        if (const auto bpm = position->getBpm())
        {
            transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
            transport.tempo = *bpm;
        }

        if (const auto beats = position->getPpqPosition())
        {
            transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
            transport.song_pos_beats = static_cast<clap_beattime> (std::llround (*beats * CLAP_BEATTIME_FACTOR));

            if (const auto bar = position->getPpqPositionOfLastBarStart())
                transport.bar_start = static_cast<clap_beattime> (std::llround (*bar * CLAP_BEATTIME_FACTOR));
        }

        if (const auto seconds = position->getTimeInSeconds())
        {
            transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
            transport.song_pos_seconds = static_cast<clap_sectime> (std::llround (*seconds * CLAP_SECTIME_FACTOR));
        }

        if (const auto signature = position->getTimeSignature())
        {
            transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
            transport.tsig_num = static_cast<uint16_t> (signature->numerator);
            transport.tsig_denom = static_cast<uint16_t> (signature->denominator);
        }
    }

    // What the plugin says back: its editor's changes, which are the "touch" of automation writing (C4).
    void handleOutput (const clap_event_header_t& header) noexcept
    {
        if (header.space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;

        const auto parameterOf = [this] (clap_id id) -> ClapParameter*
        {
            const auto found = m_parametersById.find (id);
            return found != m_parametersById.end() ? found->second : nullptr;
        };

        if (header.type == CLAP_EVENT_NOTE_END)
        {
            if (m_noteEnds != nullptr)
                m_noteEnds->noteEnded (reinterpret_cast<const clap_event_note_t&> (header).note_id);
        }
        else if (header.type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto& event = reinterpret_cast<const clap_event_param_value_t&> (header);

            if (auto* parameter = parameterOf (event.param_id))
            {
                parameter->setFromPlugin (event.value);
                parameter->sendValueChangedMessageToListeners (parameter->getValue());
            }
        }
        else if (header.type == CLAP_EVENT_PARAM_GESTURE_BEGIN || header.type == CLAP_EVENT_PARAM_GESTURE_END)
        {
            const auto& event = reinterpret_cast<const clap_event_param_gesture_t&> (header);

            if (auto* parameter = parameterOf (event.param_id))
            {
                if (header.type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
                    parameter->beginChangeGesture();
                else
                    parameter->endChangeGesture();
            }
        }
    }

    void timerCallback (int id) override
    {
        if (const auto* support = extension<clap_plugin_timer_support_t> (CLAP_EXT_TIMER_SUPPORT))
            support->on_timer (m_plugin, static_cast<clap_id> (id));
    }

    void handleAsyncUpdate() override
    {
        if (m_wantsCallback.exchange (false))
            m_plugin->on_main_thread (m_plugin);

        if (m_wantsRestart.exchange (false) && m_active)
        {
            suspendProcessing (true);
            deactivate();
            activate();
            suspendProcessing (false);
        }

        if (m_wantsLatency.exchange (false))
            if (const auto* latency = extension<clap_plugin_latency_t> (CLAP_EXT_LATENCY))
                setLatencySamples (static_cast<int> (latency->get (m_plugin)));

        if (m_wantsValues.exchange (false))
            refreshParameterValues();

        if (m_wantsFlush.exchange (false) && ! m_active)
            flushParameters();
    }

    juce::PluginDescription m_description;
    std::shared_ptr<ClapLibrary> m_library; // last to go, after the plugin
    std::unique_ptr<ClapHost> m_host;
    const clap_plugin_t* m_plugin;
    Ports m_ports;
    uint32_t m_notePorts = 0;
    bool m_acceptsMidi = false;
    bool m_acceptsClapNotes = false;
    bool m_overlappingNotes = false;
    ClapNoteEndListener* m_noteEnds = nullptr;
    double m_sampleRate = 0.0;
    int m_maxBlock = 0;
    bool m_active = false;
    bool m_processing = false;
    int64_t m_steadyTime = 0;
    bool m_guiCreated = false;
    clap_id m_nextTimer = 1;
    std::function<void (uint32_t, uint32_t)> m_editorResize;
    InputEvents m_events;
    clap_output_events_t m_outEvents {};
    juce::AudioBuffer<float> m_inputAudio;
    juce::AudioBuffer<float> m_outputAudio;
    std::vector<clap_audio_buffer_t> m_inputBuffers;
    std::vector<clap_audio_buffer_t> m_outputBuffers;
    std::map<clap_id, ClapParameter*> m_parametersById;
#if JUCE_LINUX || JUCE_BSD
    std::map<int, clap_posix_fd_flags_t> m_fds; // message thread
#endif
    juce::AbstractFifo m_fifo { maxChanges };
    std::vector<Change> m_changes;
    std::atomic<bool> m_wantsCallback { false };
    std::atomic<bool> m_wantsRestart { false };
    std::atomic<bool> m_wantsFlush { false };
    std::atomic<bool> m_wantsLatency { false };
    std::atomic<bool> m_wantsValues { false };
    std::function<void()> m_parameterList;
    bool m_listFixed = false; // from the first prepare on
};

void ClapParameter::setValue (float newValue)
{
    m_value.store (newValue);
    m_owner.parameterChanged (*this, newValue);
}

juce::String ClapParameter::getText (float normalisedValue, int maximumStringLength) const
{
    return m_owner.parameterText (m_info, plainFrom (normalisedValue)).substring (0, maximumStringLength);
}

float ClapParameter::getValueForText (const juce::String& text) const
{
    return normalised (m_owner.parameterFromText (m_info, text).value_or (text.getDoubleValue()));
}
} // namespace

std::optional<juce::PluginDescription> findMovedClapPlugin (const juce::Array<juce::PluginDescription>& known,
                                                            const juce::String& identifier)
{
    const auto prefix = juce::String (ClapPluginFormat::formatName) + "-";

    if (! identifier.startsWith (prefix))
        return std::nullopt;

    const auto id = identifier.fromLastOccurrenceOf ("-", false, false);

    for (const auto& description : known)
        if (description.pluginFormatName == ClapPluginFormat::formatName
            && juce::String::toHexString (description.uniqueId) == id
            && juce::File (description.fileOrIdentifier).exists())
            return description;

    return std::nullopt;
}

void ClapPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                                            const juce::String& fileOrIdentifier)
{
    const juce::File file (fileOrIdentifier);
    const auto library = ClapLibrary::open (file);

    if (library == nullptr)
        return;

    const auto& factory = library->factory();

    for (uint32_t i = 0; i < factory.get_plugin_count (&factory); ++i)
    {
        if (const auto* descriptor = factory.get_plugin_descriptor (&factory, i);
            descriptor != nullptr && descriptor->id != nullptr)
        {
            auto description = std::make_unique<juce::PluginDescription>();
            describe (*description, *descriptor, file);
            results.add (description.release());
        }
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const juce::String& fileOrIdentifier)
{
    return juce::File::createFileWithoutCheckingPath (fileOrIdentifier).hasFileExtension ("clap");
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier)
{
    return juce::File::createFileWithoutCheckingPath (fileOrIdentifier).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const juce::PluginDescription& description)
{
    return juce::File (description.fileOrIdentifier).getLastModificationTime() != description.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const juce::PluginDescription& description)
{
    return juce::File (description.fileOrIdentifier).exists();
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins (const juce::FileSearchPath& directoriesToSearch,
                                                           bool recursive, bool /*allowAsync*/)
{
    juce::StringArray found;

    for (int i = 0; i < directoriesToSearch.getNumPaths(); ++i)
    {
        // A bundle on macOS is a folder.
        const auto what = juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles;

        for (const auto& file : directoriesToSearch[i].findChildFiles (what, recursive, "*.clap"))
            found.add (file.getFullPathName());
    }

    return found;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    juce::FileSearchPath paths;

#if JUCE_WINDOWS
    paths.add (
        juce::File (juce::SystemStats::getEnvironmentVariable ("COMMONPROGRAMFILES", "C:\\Program Files\\Common Files"))
            .getChildFile ("CLAP"));
    paths.add (juce::File (juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}))
                   .getChildFile ("Programs\\Common\\CLAP"));
#elif JUCE_MAC
    paths.add (juce::File ("/Library/Audio/Plug-Ins/CLAP"));
    paths.add (juce::File ("~/Library/Audio/Plug-Ins/CLAP"));
#else
    paths.add (juce::File ("~/.clap"));
    paths.add (juce::File ("/usr/lib/clap"));
#endif

    const auto separator = juce::String (juce::File::getSeparatorChar() == '\\' ? ";" : ":");

    for (const auto& path :
         juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("CLAP_PATH", {}), separator, {}))
        if (path.isNotEmpty())
            paths.add (juce::File (path));

    return paths;
}

void ClapPluginFormat::createPluginInstance (const juce::PluginDescription& description, double /*initialSampleRate*/,
                                             int /*initialBufferSize*/, PluginCreationCallback callback)
{
    juce::String error;
    auto instance = ClapPluginInstance::create (description, error);
    callback (std::move (instance), error);
}

} // namespace dawg
