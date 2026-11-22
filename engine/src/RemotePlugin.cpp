#ifdef _WIN32
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#elif defined(__linux__)
 #include <climits>
 #include <csignal>
 #include <ctime>
 #include <fcntl.h>
 #include <linux/futex.h>
 #include <poll.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

#include "dawg/ClapPlugin.h"
#include "dawg/RemotePlugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Plugins in helper processes (docs/design/out-of-process-plugins.md). A stand-in processor stands where the plugin would,
// and hands each block over through shared memory, with the notes and parameter changes for that block; everything else goes
// as messages through JUCE's child process pipe. A helper is watched by its process handle and for going quiet; what happens then
// is in O4 and O8. Editor windows are the helper's own (O7). Windows and Linux differ only in the few platform pieces below: shared
// memory, waking the other side, the process's id and watching it.

namespace dawg
{

#if defined(_WIN32) || defined(__linux__)

namespace
{
constexpr int spinMicros = 30;
constexpr int midiCapacity = 64 * 1024;
constexpr int launchTimeoutMs = 10000;
constexpr int requestTimeoutMs = 30000;
constexpr int quickTimeoutMs = 2000;
constexpr int maxBlockEvents = 4096;
constexpr int maxNoteEnds = 256;
constexpr int watchIntervalMs = 100;
constexpr double hungAfterSeconds = 3.0;   // of blocks all coming back late
constexpr double quietAfterSeconds = 20.0; // of the helper's message thread not answering a ping
constexpr double pingEverySeconds = 2.0;
std::atomic<double> snapshotEverySeconds { 30.0 }; // for a plugin whose parameters moved

// What a block carries for the plugin besides its MIDI: ClapNativeInput's calls, and parameter changes.
enum class EventKind : int
{
    noteOn,
    noteOff,
    noteChoke,
    chokeAll,
    expression,
    controller,
    modulate,
    modulateNote,
    automate,
    setValue,
    midi,
};

struct BlockEvent
{
    EventKind kind = EventKind::noteOn;
    int time = 0;
    int noteId = 0;
    int key = 0;
    int a = 0; // the parameter, the expression, or a controller's or MIDI message's status
    int b = 0; // the first data byte
    int c = 0; // the second data byte
    double value = 0.0;
};

// What one block carries, at the start of an instance's shared memory; the audio follows it.
struct SharedHeader
{
    // Each side writes its own cache line.
    std::atomic<uint32_t> request { 0 };
    std::array<std::byte, 60> padding {};
    std::atomic<uint32_t> done { 0 };
    std::atomic<uint32_t> waiting { 0 };
    int numSamples = 0;
    int midiBytes = 0;
    int nonRealtime = 0;
    double bpm = 120.0;
    double ppq = 0.0;
    double barPpq = 0.0;
    double seconds = 0.0;
    juce::int64 samples = 0;
    int numerator = 4;
    int denominator = 4;
    int playing = 0;
    int eventCount = 0;
    int noteEndCount = 0;
    std::array<BlockEvent, maxBlockEvents> events;
    std::array<int32_t, maxNoteEnds> noteEnds;
    std::array<std::byte, midiCapacity> midi;
};

[[nodiscard]] int currentProcessId() noexcept
{
 #ifdef _WIN32
    return static_cast<int> (GetCurrentProcessId());
 #else
    return static_cast<int> (getpid());
 #endif
}

// A breath while spinning on the other side's answer.
void spinPause() noexcept
{
 #ifdef _WIN32
    YieldProcessor();
 #elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
 #else
    std::this_thread::yield();
 #endif
}

 #ifdef _WIN32

[[nodiscard]] std::wstring wide (const juce::String& text)
{
    return text.toWideCharPointer();
}

// An instance's shared memory and its two events: one wakes the helper for a block, one wakes the app when it's done.
class SharedBlock final
{
public:
    SharedBlock (const juce::String& name, size_t audioBytes, bool create)
    {
        const auto size = sizeof (SharedHeader) + audioBytes;
        const auto mapName = wide ("Local\\" + name);
        m_mapping = create ? CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                 static_cast<DWORD> (size), mapName.c_str())
                           : OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());

        if (m_mapping == nullptr)
            return;

        // Whoever opens it maps all of it, whatever size its creator chose.
        m_memory = MapViewOfFile (m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, create ? size : 0);

        if (m_memory == nullptr)
            return;

        if (create)
            new (m_memory) SharedHeader();

        const auto requestName = wide ("Local\\" + name + "_req");
        const auto doneName = wide ("Local\\" + name + "_don");
        m_requested = create ? CreateEventW (nullptr, FALSE, FALSE, requestName.c_str())
                             : OpenEventW (EVENT_ALL_ACCESS, FALSE, requestName.c_str());
        m_finished = create ? CreateEventW (nullptr, FALSE, FALSE, doneName.c_str())
                            : OpenEventW (EVENT_ALL_ACCESS, FALSE, doneName.c_str());
    }

    ~SharedBlock()
    {
        if (m_requested != nullptr)
            CloseHandle (m_requested);

        if (m_finished != nullptr)
            CloseHandle (m_finished);

        if (m_memory != nullptr)
            UnmapViewOfFile (m_memory);

        if (m_mapping != nullptr)
            CloseHandle (m_mapping);
    }

    SharedBlock (const SharedBlock&) = delete;
    SharedBlock& operator= (const SharedBlock&) = delete;
    SharedBlock (SharedBlock&&) = delete;
    SharedBlock& operator= (SharedBlock&&) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_memory != nullptr && m_requested != nullptr && m_finished != nullptr;
    }

    [[nodiscard]] SharedHeader& header() const noexcept { return *static_cast<SharedHeader*> (m_memory); }

    [[nodiscard]] float* audio() const noexcept
    {
        return reinterpret_cast<float*> (static_cast<std::byte*> (m_memory) + sizeof (SharedHeader));
    }

    void wakeHelper() const noexcept { SetEvent (m_requested); }

    void waitForRequest (uint32_t /*seen*/, int ms) const noexcept
    {
        WaitForSingleObject (m_requested, static_cast<DWORD> (ms));
    }

    void wakeApp() const noexcept { SetEvent (m_finished); }

    void waitForDone (uint32_t /*current*/, int ms) const noexcept
    {
        WaitForSingleObject (m_finished, static_cast<DWORD> (ms));
    }

private:
    HANDLE m_mapping = nullptr;
    void* m_memory = nullptr;
    HANDLE m_requested = nullptr;
    HANDLE m_finished = nullptr;
};

// A helper's process, watched for its end (O4).
class ProcessWatch final
{
public:
    ProcessWatch() = default;

    ~ProcessWatch()
    {
        if (const auto handle = m_handle.load(); handle != nullptr)
            CloseHandle (handle);
    }

    ProcessWatch (const ProcessWatch&) = delete;
    ProcessWatch& operator= (const ProcessWatch&) = delete;
    ProcessWatch (ProcessWatch&&) = delete;
    ProcessWatch& operator= (ProcessWatch&&) = delete;

    void open (int pid) noexcept
    {
        m_handle.store (OpenProcess (SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD> (pid)));
    }

    // Whether the process ended, waiting up to `ms` milliseconds for it.
    [[nodiscard]] bool ended (int ms) const noexcept
    {
        return WaitForSingleObject (m_handle.load(), static_cast<DWORD> (ms)) == WAIT_OBJECT_0;
    }

    void terminate() const noexcept { TerminateProcess (m_handle.load(), 1); }

private:
    std::atomic<HANDLE> m_handle { nullptr };
};

 #else

// Sleeps while a word both processes map still holds `expected`, at most `ms` milliseconds. The block's own request and done
// counters are the words, so nothing else needs to be shared to wake the other side.
void futexWait (std::atomic<uint32_t>& word, uint32_t expected, int ms) noexcept
{
    static_assert (sizeof (std::atomic<uint32_t>) == sizeof (uint32_t) && std::atomic<uint32_t>::is_always_lock_free);
    const timespec timeout { ms / 1000, static_cast<long> (ms % 1000) * 1000000L };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-type-vararg): the futex call
    syscall (SYS_futex, reinterpret_cast<uint32_t*> (&word), FUTEX_WAIT, expected, &timeout, nullptr, 0);
}

void futexWake (std::atomic<uint32_t>& word) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-type-vararg): the futex call
    syscall (SYS_futex, reinterpret_cast<uint32_t*> (&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

// An instance's shared memory, in /dev/shm. The two sides wake each other with futexes on its request and done counters.
class SharedBlock final
{
public:
    SharedBlock (const juce::String& name, size_t audioBytes, bool create)
    {
        const auto path = ("/" + name).toStdString();

        if (create)
        {
            shm_unlink (path.c_str()); // left by a run that crashed, with the same process id
            m_fd = shm_open (path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        else
        {
            m_fd = shm_open (path.c_str(), O_RDWR, 0);
        }

        if (m_fd < 0)
            return;

        if (create)
        {
            m_path = path;

            if (ftruncate (m_fd, static_cast<off_t> (sizeof (SharedHeader) + audioBytes)) != 0)
                return;
        }

        // Whoever opens it maps all of it, whatever size its creator chose.
        struct stat info {};

        if (fstat (m_fd, &info) != 0 || info.st_size <= 0)
            return;

        m_size = static_cast<size_t> (info.st_size);
        auto* memory = mmap (nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);

        if (memory
            == MAP_FAILED) // NOLINT(cppcoreguidelines-pro-type-cstyle-cast, performance-no-int-to-ptr): the macro
            return;

        m_memory = memory;

        if (create)
            new (m_memory) SharedHeader();
    }

    ~SharedBlock()
    {
        if (m_memory != nullptr)
            munmap (m_memory, m_size);

        if (m_fd >= 0)
            close (m_fd);

        if (! m_path.empty())
            shm_unlink (m_path.c_str());
    }

    SharedBlock (const SharedBlock&) = delete;
    SharedBlock& operator= (const SharedBlock&) = delete;
    SharedBlock (SharedBlock&&) = delete;
    SharedBlock& operator= (SharedBlock&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_memory != nullptr; }

    [[nodiscard]] SharedHeader& header() const noexcept { return *static_cast<SharedHeader*> (m_memory); }

    [[nodiscard]] float* audio() const noexcept
    {
        return reinterpret_cast<float*> (static_cast<std::byte*> (m_memory) + sizeof (SharedHeader));
    }

    void wakeHelper() const noexcept { futexWake (header().request); }

    void waitForRequest (uint32_t seen, int ms) const noexcept { futexWait (header().request, seen, ms); }

    void wakeApp() const noexcept { futexWake (header().done); }

    void waitForDone (uint32_t current, int ms) const noexcept { futexWait (header().done, current, ms); }

private:
    int m_fd = -1;
    void* m_memory = nullptr;
    size_t m_size = 0;
    std::string m_path; // set by the creator, which removes the name again
};

// A helper's process, watched for its end through a pidfd (O4).
class ProcessWatch final
{
public:
    ProcessWatch() = default;

    ~ProcessWatch()
    {
        if (const auto fd = m_fd.load(); fd >= 0)
            close (fd);
    }

    ProcessWatch (const ProcessWatch&) = delete;
    ProcessWatch& operator= (const ProcessWatch&) = delete;
    ProcessWatch (ProcessWatch&&) = delete;
    ProcessWatch& operator= (ProcessWatch&&) = delete;

    void open (int pid) noexcept
    {
        m_pid.store (pid);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): the pidfd_open call
        m_fd.store (static_cast<int> (syscall (SYS_pidfd_open, pid, 0)));
    }

    // Whether the process ended, waiting up to `ms` milliseconds for it. Without a pidfd (a kernel before 5.3), only the ping
    // notices an end.
    [[nodiscard]] bool ended (int ms) const noexcept
    {
        const auto fd = m_fd.load();

        if (fd < 0)
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (ms));
            return false;
        }

        pollfd watched { fd, POLLIN, 0 };
        return poll (&watched, 1, ms) > 0;
    }

    void terminate() const noexcept
    {
        if (const auto pid = m_pid.load(); pid > 0)
            kill (pid, SIGKILL);
    }

private:
    std::atomic<int> m_fd { -1 };
    std::atomic<int> m_pid { 0 };
};

 #endif

[[nodiscard]] juce::MemoryBlock encode (const juce::var& message)
{
    const auto text = juce::JSON::toString (message, true);
    return { text.toRawUTF8(), text.getNumBytesAsUTF8() };
}

[[nodiscard]] juce::var decode (const juce::MemoryBlock& block)
{
    return juce::JSON::parse (block.toString());
}

[[nodiscard]] juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    juce::var result (new juce::DynamicObject());

    for (const auto& [name, value] : fields)
        result.getDynamicObject()->setProperty (name, value);

    return result;
}

[[nodiscard]] juce::var busesOf (const juce::AudioProcessor& processor, bool input)
{
    juce::Array<juce::var> buses;

    for (int i = 0; i < processor.getBusCount (input); ++i)
    {
        const auto* bus = processor.getBus (input, i);
        buses.add (object ({ { "name", bus->getName() },
                             { "channels", bus->getNumberOfChannels() },
                             { "defaultChannels", bus->getDefaultLayout().size() },
                             { "enabled", bus->isEnabled() } }));
    }

    return buses;
}

[[nodiscard]] juce::AudioChannelSet channelSet (int channels)
{
    switch (channels)
    {
        case 0:
            return juce::AudioChannelSet::disabled();
        case 1:
            return juce::AudioChannelSet::mono();
        case 2:
            return juce::AudioChannelSet::stereo();
        default:
            return juce::AudioChannelSet::discreteChannels (channels);
    }
}

[[nodiscard]] juce::var channelsOf (const juce::Array<juce::AudioChannelSet>& buses)
{
    juce::Array<juce::var> channels;

    for (const auto& bus : buses)
        channels.add (bus.size());

    return channels;
}

[[nodiscard]] juce::AudioProcessor::BusesLayout layoutFrom (const juce::var& inputs, const juce::var& outputs)
{
    juce::AudioProcessor::BusesLayout layout;

    if (const auto* list = inputs.getArray())
        for (const auto& channels : *list)
            layout.inputBuses.add (channelSet (static_cast<int> (channels)));

    if (const auto* list = outputs.getArray())
        for (const auto& channels : *list)
            layout.outputBuses.add (channelSet (static_cast<int> (channels)));

    return layout;
}

// ---------------------------------------------------------------------------------------------------------
// The helper's side.

class PlayHead final : public juce::AudioPlayHead
{
public:
    [[nodiscard]] juce::Optional<PositionInfo> getPosition() const override { return info; }

    PositionInfo info;
};

 #ifdef _WIN32

// The running thread as a pro audio thread, which measuring showed is needed (O3, section 4.1). Taken from the DLL at run
// time, so this file doesn't depend on the order of Windows headers.
class ProAudioPriority final
{
public:
    ProAudioPriority()
    {
        m_library = LoadLibraryW (L"avrt.dll");

        if (m_library == nullptr)
            return;

        using Set = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
        const auto set = reinterpret_cast<Set> (GetProcAddress (m_library, "AvSetMmThreadCharacteristicsW"));
        DWORD task = 0;

        if (set != nullptr)
            m_task = set (L"Pro Audio", &task);
    }

    ~ProAudioPriority()
    {
        if (m_library == nullptr)
            return;

        using Revert = BOOL (WINAPI*) (HANDLE);
        const auto revert = reinterpret_cast<Revert> (GetProcAddress (m_library, "AvRevertMmThreadCharacteristics"));

        if (revert != nullptr && m_task != nullptr)
            revert (m_task);

        FreeLibrary (m_library);
    }

    ProAudioPriority (const ProAudioPriority&) = delete;
    ProAudioPriority& operator= (const ProAudioPriority&) = delete;
    ProAudioPriority (ProAudioPriority&&) = delete;
    ProAudioPriority& operator= (ProAudioPriority&&) = delete;

private:
    HMODULE m_library = nullptr;
    HANDLE m_task = nullptr;
};

 #else

// On Linux, JUCE's real-time thread already asks for real-time scheduling where the system allows it.
class ProAudioPriority final
{};

 #endif

// One plugin in the helper, with the thread that runs its blocks.
class HostedInstance final : private juce::Thread,
                             private juce::AudioProcessorListener,
                             private ClapNoteEndListener
{
public:
    // What the plugin says of itself, for the app: its latency, a parameter's value, a gesture.
    struct Callbacks
    {
        std::function<void (int)> latency;
        std::function<void (int, float)> value;
        std::function<void (int, bool)> gesture;
        std::function<void()> parameterList;
    };

    HostedInstance (std::unique_ptr<juce::AudioPluginInstance> plugin, Callbacks callbacks)
        : juce::Thread ("DAWg plugin"),
          m_plugin (std::move (plugin)),
          m_callbacks (std::move (callbacks)),
          m_native (dynamic_cast<ClapNativeInput*> (m_plugin.get()))
    {
        m_plugin->setPlayHead (&m_playHead);
        m_plugin->addListener (this);

        if (m_native != nullptr)
            m_native->setNoteEndListener (this);

        if (auto* source = dynamic_cast<ParameterListSource*> (m_plugin.get()))
            source->setParameterListCallback (
                [this]
                {
                    if (m_callbacks.parameterList != nullptr)
                        m_callbacks.parameterList();
                });
    }

    ~HostedInstance() override
    {
        stop();
        m_plugin->removeListener (this);

        if (auto* source = dynamic_cast<ParameterListSource*> (m_plugin.get()))
            source->setParameterListCallback (nullptr);

        if (m_native != nullptr)
            m_native->setNoteEndListener (nullptr);
    }

    HostedInstance (const HostedInstance&) = delete;
    HostedInstance& operator= (const HostedInstance&) = delete;
    HostedInstance (HostedInstance&&) = delete;
    HostedInstance& operator= (HostedInstance&&) = delete;

    [[nodiscard]] juce::AudioPluginInstance& plugin() noexcept { return *m_plugin; }

    [[nodiscard]] ClapNativeInput* native() const noexcept { return m_native; }

    void prepare (const juce::String& shared, double sampleRate, int blockSize, const juce::var& inputs,
                  const juce::var& outputs)
    {
        stop();
        std::ignore = m_plugin->setBusesLayout (layoutFrom (inputs, outputs));
        m_plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        m_plugin->prepareToPlay (sampleRate, blockSize);
        m_maxBlock = blockSize;
        m_inputs = m_plugin->getTotalNumInputChannels();
        m_outputs = m_plugin->getTotalNumOutputChannels();
        m_buffer.setSize (std::max ({ 1, m_inputs, m_outputs }), blockSize);
        m_midi.ensureSize (midiCapacity);
        m_block = std::make_unique<SharedBlock> (shared, 0, false);

        if (m_block->valid())
        {
            m_started.reset();
            std::ignore = startRealtimeThread (RealtimeOptions {});
            std::ignore = m_started.wait (2000); // the first block mustn't pay for the thread's start
        }
    }

    void release()
    {
        stop();
        m_plugin->releaseResources();
    }

private:
    void stop() { std::ignore = stopThread (2000); }

    void run() override
    {
        [[maybe_unused]] const ProAudioPriority audioPriority;
        uint32_t seen = 0; // a new block's first request is 1, and may be there already
        m_started.signal();

        while (! threadShouldExit())
        {
            m_block->waitForRequest (seen, 100);
            auto& header = m_block->header();
            const auto request = header.request.load (std::memory_order_acquire);

            if (request == seen)
                continue;

            seen = request;
            processBlock (header);
            header.done.store (seen, std::memory_order_release);

            if (header.waiting.exchange (0) != 0)
                m_block->wakeApp();
        }
    }

    void processBlock (SharedHeader& header)
    {
        const auto numSamples = std::min (header.numSamples, m_maxBlock);
        const auto* audio = m_block->audio();
        m_buffer.setSize (m_buffer.getNumChannels(), numSamples, true, false, true);

        for (int ch = 0; ch < m_inputs; ++ch)
            m_buffer.copyFrom (ch, 0, audio + static_cast<std::ptrdiff_t> (ch) * m_maxBlock, numSamples);

        for (int ch = m_inputs; ch < m_buffer.getNumChannels(); ++ch)
            m_buffer.clear (ch, 0, numSamples);

        m_midi.clear();

        for (int at = 0; at < header.midiBytes;)
        {
            int offset = 0;
            int size = 0;
            std::memcpy (&offset, header.midi.data() + at, sizeof (int));
            std::memcpy (&size, header.midi.data() + at + sizeof (int), sizeof (int));
            at += 2 * static_cast<int> (sizeof (int));
            m_midi.addEvent (header.midi.data() + at, size, offset);
            at += size;
        }

        header.noteEndCount = 0;
        m_header = &header;
        applyEvents (header, numSamples);

        auto& info = m_playHead.info;
        info.setBpm (header.bpm);
        info.setPpqPosition (header.ppq);
        info.setPpqPositionOfLastBarStart (header.barPpq);
        info.setTimeInSeconds (header.seconds);
        info.setTimeInSamples (header.samples);
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { header.numerator, header.denominator });
        info.setIsPlaying (header.playing != 0);

        {
            const juce::ScopedNoDenormals noDenormals;
            m_plugin->processBlock (m_buffer, m_midi);
        }

        auto* out = m_block->audio() + static_cast<std::ptrdiff_t> (m_inputs) * m_maxBlock;

        for (int ch = 0; ch < m_outputs; ++ch)
            std::memcpy (out + static_cast<std::ptrdiff_t> (ch) * m_maxBlock, m_buffer.getReadPointer (ch),
                         sizeof (float) * static_cast<size_t> (numSamples));
    }

    void applyEvents (const SharedHeader& header, int numSamples) noexcept
    {
        const auto& parameters = m_plugin->getParameters();

        for (int i = 0; i < std::min (header.eventCount, maxBlockEvents); ++i)
        {
            const auto& event = header.events[static_cast<size_t> (i)];
            const auto time = static_cast<uint32_t> (std::clamp (event.time, 0, std::max (0, numSamples - 1)));

            if (event.kind == EventKind::midi)
            {
                const auto data = std::array { static_cast<juce::uint8> (event.a), static_cast<juce::uint8> (event.b),
                                               static_cast<juce::uint8> (event.c) };
                m_midi.addEvent (data.data(), event.noteId, static_cast<int> (time));
                continue;
            }

            if (event.kind == EventKind::setValue || (event.kind == EventKind::automate && m_native == nullptr))
            {
                if (event.a >= 0 && event.a < parameters.size())
                {
                    m_applying = true;
                    parameters.getUnchecked (event.a)->setValue (static_cast<float> (event.value));
                    m_applying = false;
                }

                continue;
            }

            if (m_native == nullptr)
                continue;

            switch (event.kind)
            {
                case EventKind::noteOn:
                    m_native->noteOn (time, event.noteId, event.key, event.value);
                    break;
                case EventKind::noteOff:
                    m_native->noteOff (time, event.noteId, event.key, event.value);
                    break;
                case EventKind::noteChoke:
                    m_native->noteChoke (time, event.noteId, event.key);
                    break;
                case EventKind::chokeAll:
                    m_native->chokeAll (time);
                    break;
                case EventKind::expression:
                    m_native->noteExpression (time, event.noteId, event.key,
                                              static_cast<ClapNativeInput::Expression> (event.a), event.value);
                    break;
                case EventKind::controller:
                    m_native->controller (time, static_cast<uint8_t> (event.a), static_cast<uint8_t> (event.b),
                                          static_cast<uint8_t> (event.c));
                    break;
                case EventKind::modulate:
                    m_native->modulateParameter (event.a, event.value, time);
                    break;
                case EventKind::modulateNote:
                    m_native->modulateNoteParameter (time, event.noteId, event.key, event.a, event.value);
                    break;
                case EventKind::automate:
                    m_native->automateParameter (event.a, static_cast<float> (event.value), time);
                    break;
                case EventKind::setValue:
                case EventKind::midi:
                    break;
            }
        }
    }

    void noteEnded (int32_t noteId) noexcept override
    {
        if (m_header != nullptr && m_header->noteEndCount < maxNoteEnds)
            m_header->noteEnds[static_cast<size_t> (m_header->noteEndCount++)] = noteId;
    }

    void audioProcessorParameterChanged (juce::AudioProcessor* /*processor*/, int index, float value) override
    {
        if (! m_applying)
            m_callbacks.value (index, value);
    }

    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* /*processor*/, int index) override
    {
        m_callbacks.gesture (index, true);
    }

    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* /*processor*/, int index) override
    {
        m_callbacks.gesture (index, false);
    }

    void audioProcessorChanged (juce::AudioProcessor* processor, const ChangeDetails& details) override
    {
        if (details.latencyChanged)
            m_callbacks.latency (processor->getLatencySamples());
    }

    std::unique_ptr<juce::AudioPluginInstance> m_plugin;
    Callbacks m_callbacks;
    ClapNativeInput* m_native;
    SharedHeader* m_header = nullptr;
    bool m_applying = false; // on the plugin's thread: its own changes are the only ones to report
    std::unique_ptr<SharedBlock> m_block;
    juce::WaitableEvent m_started { true };
    juce::AudioBuffer<float> m_buffer;
    juce::MidiBuffer m_midi;
    PlayHead m_playHead;
    int m_maxBlock = 0;
    int m_inputs = 0;
    int m_outputs = 0;
};

// Plugin code runs on the helper's message thread, as it would in the app; messages arrive on the pipe's thread.
class HelperWorker final : public juce::ChildProcessWorker,
                           public RemotePluginHelper
{
public:
    HelperWorker (const std::function<void (juce::AudioPluginFormatManager&)>& addFormats,
                  std::function<void()> onDisconnect, RemoteEditorFactory makeEditor)
        : m_onDisconnect (std::move (onDisconnect)),
          m_makeEditor (std::move (makeEditor))
    {
        addFormats (m_formats);
    }

    ~HelperWorker() override { *m_alive = false; }

    HelperWorker (const HelperWorker&) = delete;
    HelperWorker& operator= (const HelperWorker&) = delete;
    HelperWorker (HelperWorker&&) = delete;
    HelperWorker& operator= (HelperWorker&&) = delete;

    void handleMessageFromCoordinator (const juce::MemoryBlock& block) override
    {
        const std::weak_ptr<bool> alive = m_alive;
        juce::MessageManager::callAsync (
            [this, alive, message = decode (block)]
            {
                if (alive.lock() != nullptr && *m_alive)
                    handle (message);
            });
    }

    void handleConnectionMade() override
    {
        std::ignore =
            sendMessageToCoordinator (encode (object ({ { "event", "hello" }, { "pid", currentProcessId() } })));
    }

    void handleConnectionLost() override
    {
        if (m_onDisconnect != nullptr)
            m_onDisconnect();
    }

private:
    [[nodiscard]] static juce::var valuesOf (const juce::AudioProcessor& plugin)
    {
        juce::Array<juce::var> values;

        for (const auto* parameter : plugin.getParameters())
            values.add (parameter->getValue());

        return values;
    }

    [[nodiscard]] static juce::var parametersOf (const juce::AudioProcessor& plugin, const ClapNativeInput* native)
    {
        juce::Array<juce::var> list;
        const auto& parameters = plugin.getParameters();

        for (int i = 0; i < parameters.size(); ++i)
        {
            const auto* parameter = parameters.getUnchecked (i);
            const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameter);
            list.add (object ({ { "id", hosted != nullptr ? hosted->getParameterID() : juce::String (i) },
                                { "name", parameter->getName (256) },
                                { "label", parameter->getLabel() },
                                { "default", parameter->getDefaultValue() },
                                { "value", parameter->getValue() },
                                { "steps", parameter->getNumSteps() },
                                { "discrete", parameter->isDiscrete() },
                                { "automatable", parameter->isAutomatable() },
                                { "modulatable", native != nullptr && native->isModulatable (i) },
                                { "perNote", native != nullptr && native->isModulatablePerNote (i) } }));
        }

        return list;
    }

    void openEditor (const juce::var& message, int id, juce::AudioPluginInstance& plugin)
    {
        if (const auto editor = m_editors.find (id); editor != m_editors.end())
        {
            editor->second->toFront();
            reply (message, {});
            return;
        }

        if (m_makeEditor == nullptr)
        {
            reply (message, object ({ { "error", "No editor windows here" } }));
            return;
        }

        std::optional<RemoteEditorPosition> position;

        if (message.hasProperty ("x"))
            position = RemoteEditorPosition { static_cast<int> (message["x"]), static_cast<int> (message["y"]) };

        const std::weak_ptr<bool> alive = m_alive;
        m_editors[id] = m_makeEditor (
            plugin, message["title"].toString(), position, static_cast<bool> (message["visible"]),
            [this, alive, id]
            {
                if (alive.lock() == nullptr || ! *m_alive)
                    return;

                const auto editor = m_editors.find (id);

                if (editor == m_editors.end())
                    return;

                const auto where = editor->second->position();
                m_editors.erase (editor);
                sendEvent (object ({ { "event", "editorClosed" }, { "id", id }, { "x", where.x }, { "y", where.y } }));
            });
        reply (message, {});
    }

    void sendEvent (juce::var event)
    {
        // Reports may come on the plugin's audio thread: the pipe is for the message thread.
        const std::weak_ptr<bool> alive = m_alive;
        juce::MessageManager::callAsync (
            [this, alive, message = std::move (event)]
            {
                if (alive.lock() != nullptr && *m_alive)
                    std::ignore = sendMessageToCoordinator (encode (message));
            });
    }

    void reply (const juce::var& message, const juce::var& fields)
    {
        auto answer = fields.isObject() ? fields : object ({});
        answer.getDynamicObject()->setProperty ("reply", message["seq"]);
        std::ignore = sendMessageToCoordinator (encode (answer));
    }

    void handle (const juce::var& message)
    {
        const auto kind = message["kind"].toString();

        if (kind == "ping")
        {
            reply (message, {});
            return;
        }

        if (kind == "load")
        {
            load (message);
            return;
        }

        const auto found = m_instances.find (static_cast<int> (message["id"]));

        if (found == m_instances.end())
        {
            reply (message, object ({ { "error", "No such plugin" } }));
            return;
        }

        auto& instance = *found->second;
        auto& plugin = instance.plugin();

        const auto id = found->first;

        if (kind == "openEditor")
        {
            openEditor (message, id, plugin);
        }
        else if (kind == "closeEditor")
        {
            auto answer = object ({});

            if (const auto editor = m_editors.find (id); editor != m_editors.end())
            {
                const auto position = editor->second->position();
                answer = object ({ { "x", position.x }, { "y", position.y } });
                m_editors.erase (editor);
            }

            reply (message, answer);
        }
        else if (kind == "showEditor")
        {
            if (const auto editor = m_editors.find (id); editor != m_editors.end())
                editor->second->setShown (static_cast<bool> (message["onTop"]), static_cast<bool> (message["visible"]));
        }
        else if (kind == "unload")
        {
            m_editors.erase (id);
            m_instances.erase (found);
        }
        else if (kind == "prepare")
        {
            instance.prepare (message["shared"].toString(), static_cast<double> (message["sampleRate"]),
                              static_cast<int> (message["blockSize"]), message["inputs"], message["outputs"]);
            reply (message, object ({ { "latency", plugin.getLatencySamples() } }));
        }
        else if (kind == "release")
        {
            instance.release();
            reply (message, {});
        }
        else if (kind == "supports")
        {
            reply (message, object ({ { "ok", plugin.checkBusesLayoutSupported (
                                                  layoutFrom (message["inputs"], message["outputs"])) } }));
        }
        else if (kind == "getState")
        {
            juce::MemoryBlock state;
            auto* forCopy = dynamic_cast<DuplicateStateSource*> (&plugin);

            if (static_cast<bool> (message["duplicate"]) && forCopy != nullptr)
                forCopy->getStateForDuplicate (state);
            else
                plugin.getStateInformation (state);

            reply (message, object ({ { "state", state.toBase64Encoding() } }));
        }
        else if (kind == "setState")
        {
            juce::MemoryBlock state;
            state.fromBase64Encoding (message["state"].toString());
            plugin.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            reply (message, object ({ { "values", valuesOf (plugin) },
                                      { "parameters", parametersOf (plugin, instance.native()) } }));
        }
        else if (kind == "parameterText")
        {
            const auto index = static_cast<int> (message["index"]);
            const auto& parameters = plugin.getParameters();

            if (index >= 0 && index < parameters.size())
            {
                auto* parameter = parameters.getUnchecked (index);

                if (message.hasProperty ("text"))
                    reply (message, object ({ { "value", parameter->getValueForText (message["text"].toString()) } }));
                else
                    reply (message,
                           object ({ { "text", parameter->getText (static_cast<float> (message["value"]), 1024) } }));
            }
            else
            {
                reply (message, {});
            }
        }
        else if (kind == "nonRealtime")
        {
            plugin.setNonRealtime (static_cast<bool> (message["value"]));
        }
    }

    void load (const juce::var& message)
    {
        juce::PluginDescription description;
        const auto xml = juce::parseXML (message["description"].toString());

        if (xml == nullptr || ! description.loadFromXml (*xml))
        {
            reply (message, object ({ { "error", "Bad plugin description" } }));
            return;
        }

        const auto id = static_cast<int> (message["id"]);
        const std::weak_ptr<bool> alive = m_alive;
        m_formats.createPluginInstanceAsync (
            description, static_cast<double> (message["sampleRate"]), static_cast<int> (message["blockSize"]),
            [this, alive, message, id] (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& error)
            {
                if (alive.lock() == nullptr || ! *m_alive)
                    return;

                if (plugin == nullptr)
                {
                    reply (message,
                           object ({ { "error",
                                       error.isNotEmpty() ? error : juce::String ("Could not load the plugin") } }));
                    return;
                }

                HostedInstance::Callbacks callbacks;
                callbacks.latency = [this, id] (int latency)
                { sendEvent (object ({ { "event", "latency" }, { "id", id }, { "latency", latency } })); };
                callbacks.value = [this, id] (int index, float value)
                {
                    sendEvent (object ({ { "event", "value" }, { "id", id }, { "index", index }, { "value", value } }));
                };
                callbacks.gesture = [this, id] (int index, bool begin)
                {
                    sendEvent (
                        object ({ { "event", "gesture" }, { "id", id }, { "index", index }, { "begin", begin } }));
                };
                callbacks.parameterList = [this, id]
                { sendEvent (object ({ { "event", "parameterList" }, { "id", id } })); };
                auto instance = std::make_unique<HostedInstance> (std::move (plugin), std::move (callbacks));
                const auto& hosted = instance->plugin();
                reply (message, object ({ { "inputs", busesOf (hosted, true) },
                                          { "outputs", busesOf (hosted, false) },
                                          { "latency", hosted.getLatencySamples() },
                                          { "acceptsMidi", hosted.acceptsMidi() },
                                          { "parameters", parametersOf (hosted, instance->native()) },
                                          { "nativeNotes",
                                            instance->native() != nullptr && instance->native()->takesNativeNotes() },
                                          { "overlapping", instance->native() != nullptr
                                                               && instance->native()->playsOverlappingNotes() },
                                          { "tail", hosted.getTailLengthSeconds() } }));
                m_instances[id] = std::move (instance);
            });
    }

    juce::AudioPluginFormatManager m_formats;
    std::map<int, std::unique_ptr<HostedInstance>> m_instances;
    std::map<int, std::unique_ptr<RemoteEditorWindow>> m_editors; // after the plugins, so that they go first
    std::function<void()> m_onDisconnect;
    RemoteEditorFactory m_makeEditor;
    std::shared_ptr<bool> m_alive = std::make_shared<bool> (true);
};

// ---------------------------------------------------------------------------------------------------------
// The app's side.

class RemoteProcessor;

// A helper process, for all the instances of one plugin file.
class Helper final : public juce::ChildProcessCoordinator,
                     public std::enable_shared_from_this<Helper>,
                     private juce::Thread
{
public:
    Helper()
        : juce::Thread ("DAWg plugin watch")
    {
    }

    Helper (const Helper&) = delete;
    Helper& operator= (const Helper&) = delete;
    Helper (Helper&&) = delete;
    Helper& operator= (Helper&&) = delete;

    ~Helper() override
    {
        std::ignore = stopThread (2000);
        killWorkerProcess();
    }

    using Reply = std::function<void (const juce::var&)>;

    [[nodiscard]] bool launch (const juce::File& executable)
    {
        return launchWorkerProcess (executable, remotePluginHelperId, launchTimeoutMs, 0);
    }

    void send (const juce::var& message, Reply reply = nullptr)
    {
        if (reply != nullptr)
        {
            const std::scoped_lock lock (m_mutex);

            if (m_lost)
            {
                reply (object ({ { "error", remotePluginProcessGone } }));
                return;
            }

            const auto seq = ++m_sequence;
            m_replies[seq] = std::move (reply);
            message.getDynamicObject()->setProperty ("seq", seq);
        }

        std::ignore = sendMessageToWorker (encode (message));
    }

    // Waits for the answer: for what JUCE wants right now, within a limit.
    [[nodiscard]] juce::var request (const juce::var& message, int timeoutMs)
    {
        auto done = std::make_shared<juce::WaitableEvent>();
        auto answer = std::make_shared<juce::var>();
        send (message,
              [done, answer] (const juce::var& reply)
              {
                  *answer = reply;
                  done->signal();
              });
        std::ignore = done->wait (timeoutMs);
        return *answer;
    }

    void addInstance (int id, RemoteProcessor* instance)
    {
        const std::scoped_lock lock (m_mutex);
        m_instances[id] = instance;
    }

    void removeInstance (int id)
    {
        const std::scoped_lock lock (m_mutex);
        m_instances.erase (id);
    }

    [[nodiscard]] bool lost() const noexcept { return m_lostFlag.load(); }

    [[nodiscard]] int processId() const noexcept { return m_processId.load(); }

    // A long load or prepare holds the helper's message thread, and doesn't count as going quiet.
    void beginLongRequest() noexcept { m_longRequests.fetch_add (1); }

    void endLongRequest() noexcept
    {
        m_longRequests.fetch_sub (1);
        m_lastAnswer.store (juce::Time::getMillisecondCounterHiRes());
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& block) override;
    void handleConnectionLost() override;
    void run() override;
    void declareLost (const juce::String& reason);
    void watchProcess (int pid);
    void closeHung (const juce::String& reason);

    ProcessWatch m_process;
    std::atomic<int> m_processId { 0 };
    std::atomic<int> m_longRequests { 0 };
    std::atomic<double> m_lastAnswer { 0.0 };

    std::mutex m_mutex;
    int m_sequence = 0;
    bool m_lost = false;
    std::atomic<bool> m_lostFlag { false };
    std::map<int, Reply> m_replies;
    std::map<int, RemoteProcessor*> m_instances;
};

std::mutex poolMutex;
// A function's static, so building it happens on first use, not at load time.
std::map<juce::String, std::weak_ptr<Helper>>& helpers()
{
    static std::map<juce::String, std::weak_ptr<Helper>> map;
    return map;
}
juce::File helperExecutable;
std::atomic<int> nextInstance { 1 };

// JUCE takes ten seconds to clean up after a process that died, which the message thread shouldn't sit through.
void releaseHelper (std::shared_ptr<Helper> helper)
{
    if (helper != nullptr && helper->lost())
        juce::Thread::launch ([released = std::move (helper)]() mutable { released.reset(); });
}

 #ifdef _WIN32
[[nodiscard]] bool helperHasProcess (int process)
{
    const std::scoped_lock lock (poolMutex);

    return std::ranges::any_of (helpers(),
                                [process] (const auto& entry)
                                {
                                    const auto helper = entry.second.lock();
                                    return helper != nullptr && helper->processId() == process;
                                });
}
 #endif

[[nodiscard]] std::shared_ptr<Helper> helperFor (const juce::String& file)
{
    const std::scoped_lock lock (poolMutex);

    if (auto existing = helpers()[file].lock(); existing != nullptr && ! existing->lost())
        return existing;

    auto helper = std::make_shared<Helper>();
    const auto executable = helperExecutable != juce::File()
                                ? helperExecutable
                                : juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    if (! helper->launch (executable))
        return nullptr;

    helpers()[file] = helper;
    return helper;
}

// A parameter of the plugin in the helper. Its value is known here, so reading it costs nothing.
class RemoteParameter final : public juce::HostedAudioProcessorParameter
{
public:
    RemoteParameter (RemoteProcessor& owner, int index, const juce::var& info)
        : m_owner (owner),
          m_index (index),
          m_id (info["id"].toString()),
          m_name (info["name"].toString()),
          m_label (info["label"].toString()),
          m_default (static_cast<float> (info["default"])),
          m_steps (static_cast<int> (info["steps"])),
          m_discrete (static_cast<bool> (info["discrete"])),
          m_automatable (static_cast<bool> (info["automatable"])),
          modulatable (static_cast<bool> (info["modulatable"])),
          perNote (static_cast<bool> (info["perNote"])),
          m_value (static_cast<float> (info["value"]))
    {
    }

    [[nodiscard]] float getValue() const override { return m_value.load(); }

    void setValue (float newValue) override;

    // What the plugin has: nothing to send back.
    void store (float value) noexcept { m_value.store (value); }

    [[nodiscard]] float getDefaultValue() const override { return m_default; }

    [[nodiscard]] juce::String getName (int maximumStringLength) const override
    {
        return m_name.substring (0, maximumStringLength);
    }

    [[nodiscard]] juce::String getLabel() const override { return m_label; }

    [[nodiscard]] juce::String getText (float normalisedValue, int maximumStringLength) const override;

    [[nodiscard]] float getValueForText (const juce::String& text) const override;

    [[nodiscard]] int getNumSteps() const override { return m_steps; }

    [[nodiscard]] bool isDiscrete() const override { return m_discrete; }

    [[nodiscard]] bool isAutomatable() const override { return m_automatable; }

    [[nodiscard]] juce::String getParameterID() const override { return m_id; }

private:
    RemoteProcessor& m_owner;
    int m_index;
    juce::String m_id;
    juce::String m_name;
    juce::String m_label;
    float m_default;
    int m_steps;
    bool m_discrete;
    bool m_automatable;

public:
    const bool modulatable;
    const bool perNote;

private:
    std::atomic<float> m_value;
};

// The stand-in: what the app holds in place of the plugin. The native calls queue up on the audio thread and go with the next
// block that can be sent, so a late block loses no notes (O4).
class RemoteProcessor final : public juce::AudioPluginInstance,
                              public ClapNativeInput,
                              public RemoteInstance,
                              public ParameterListSource,
                              public DuplicateStateSource
{
public:
    RemoteProcessor (const juce::PluginDescription& description, std::shared_ptr<Helper> helper, int id,
                     const juce::var& loaded)
        : juce::AudioPluginInstance (busesFrom (loaded["inputs"], loaded["outputs"])),
          m_description (description),
          m_helper (std::move (helper)),
          m_id (id),
          m_acceptsMidi (static_cast<bool> (loaded["acceptsMidi"])),
          m_tail (static_cast<double> (loaded["tail"]))
    {
        setLatencySamples (static_cast<int> (loaded["latency"]));
        m_nativeNotes = static_cast<bool> (loaded["nativeNotes"]);
        m_overlapping = static_cast<bool> (loaded["overlapping"]);

        if (const auto* list = loaded["parameters"].getArray())
            addParameters (*list);

        m_helper->addInstance (m_id, this);
    }

    ~RemoteProcessor() override
    {
        m_helper->removeInstance (m_id);
        m_helper->send (object ({ { "kind", "unload" }, { "id", m_id } }));
        releaseHelper (std::move (m_helper));
    }

    RemoteProcessor (const RemoteProcessor&) = delete;
    RemoteProcessor& operator= (const RemoteProcessor&) = delete;
    RemoteProcessor (RemoteProcessor&&) = delete;
    RemoteProcessor& operator= (RemoteProcessor&&) = delete;

    // The helper went away. Runs under the helper's lock, on a thread of its own.
    void connectionLost (const juce::String& reason)
    {
        m_ready.store (false);
        {
            const std::scoped_lock lock (m_stateMutex);
            m_crashReason = reason;
        }
        m_crashed.store (true);
        setLatencySamples (0);

        if (m_crashCallback != nullptr)
            m_crashCallback();

        editorGone (std::nullopt);
    }

    void editorGone (std::optional<RemoteEditorPosition> position)
    {
        std::function<void (std::optional<RemoteEditorPosition>)> onGone;
        {
            const std::scoped_lock lock (m_stateMutex);
            onGone.swap (m_editorGone);
        }

        // What the user did in there may not have moved a parameter.
        m_editorOpen.store (false);
        m_stateMoved.store (true);

        if (onGone != nullptr)
            onGone (position);
    }

    void openEditor (const juce::String& title, std::optional<RemoteEditorPosition> position, bool visible,
                     std::function<void (std::optional<RemoteEditorPosition>)> onGone) override
    {
        if (m_crashed.load())
            return;

        {
            const std::scoped_lock lock (m_stateMutex);
            m_editorGone = std::move (onGone);
        }

        m_editorOpen.store (true);
        auto request =
            object ({ { "kind", "openEditor" }, { "id", m_id }, { "title", title }, { "visible", visible } });

        if (position.has_value())
        {
            request.getDynamicObject()->setProperty ("x", position->x);
            request.getDynamicObject()->setProperty ("y", position->y);
        }

 #ifdef _WIN32
        // A process in the background may not bring its windows to the front unless it's let.
        AllowSetForegroundWindow (static_cast<DWORD> (m_helper->processId()));
 #endif
        m_helper->beginLongRequest();
        m_helper->send (request, [helper = m_helper.get()] (const juce::var&) { helper->endLongRequest(); });
    }

    std::optional<RemoteEditorPosition> closeEditor() override
    {
        {
            const std::scoped_lock lock (m_stateMutex);
            m_editorGone = nullptr;
        }

        m_editorOpen.store (false);
        m_stateMoved.store (true);

        if (m_crashed.load())
            return std::nullopt;

        const auto answer = m_helper->request (object ({ { "kind", "closeEditor" }, { "id", m_id } }), quickTimeoutMs);

        if (! answer.hasProperty ("x"))
            return std::nullopt;

        return RemoteEditorPosition { static_cast<int> (answer["x"]), static_cast<int> (answer["y"]) };
    }

    void showEditor (bool onTop, bool visible) override
    {
        if (! m_crashed.load())
            m_helper->send (
                object ({ { "kind", "showEditor" }, { "id", m_id }, { "onTop", onTop }, { "visible", visible } }));
    }

    [[nodiscard]] bool crashed() const noexcept override { return m_crashed.load(); }

    [[nodiscard]] juce::String crashReason() const override
    {
        const std::scoped_lock lock (m_stateMutex);
        return m_crashReason;
    }

    void setCrashCallback (std::function<void()> callback) override { m_crashCallback = std::move (callback); }

    [[nodiscard]] int lateBlocks() const noexcept override { return m_lateBlocks.load(); }

    [[nodiscard]] bool reload (juce::String& error) override
    {
        auto helper = helperFor (m_description.fileOrIdentifier);

        if (helper == nullptr)
        {
            error = "Could not start a process for " + m_description.name;
            return false;
        }

        const auto id = nextInstance.fetch_add (1);
        helper->beginLongRequest();
        const auto loaded = helper->request (object ({ { "kind", "load" },
                                                       { "id", id },
                                                       { "description", m_description.createXml()->toString() },
                                                       { "sampleRate", m_sampleRate },
                                                       { "blockSize", std::max (1, m_maxBlock) } }),
                                             requestTimeoutMs);
        helper->endLongRequest();

        if (! loaded.hasProperty ("parameters"))
        {
            error = loaded.hasProperty ("error") ? loaded["error"].toString()
                                                 : juce::String ("The plugin didn't load in time");
            return false;
        }

        const auto* parameters = loaded["parameters"].getArray();

        if (parameters == nullptr || std::cmp_not_equal (parameters->size(), m_parameters.size()))
        {
            error = m_description.name + " came back with different parameters";
            return false;
        }

        // The audio thread stays away, and nothing it uses is touched until prepareToPlay() makes it ready again.
        suspendProcessing (true);
        m_helper->removeInstance (m_id);
        releaseHelper (std::exchange (m_helper, std::move (helper)));
        m_id = id;
        m_helper->addInstance (m_id, this);

        juce::MemoryBlock state;
        {
            const std::scoped_lock lock (m_stateMutex);
            state = m_lastState;
        }

        m_crashed.store (false);

        if (state.getSize() > 0)
            setStateInformation (state.getData(), static_cast<int> (state.getSize()));

        if (m_maxBlock > 0)
            prepareToPlay (m_sampleRate, m_maxBlock);

        m_queued = 0;
        suspendProcessing (false);
        {
            const std::scoped_lock lock (m_stateMutex);
            m_crashReason.clear();
        }
        return true;
    }

    // Whether there is reason to think the plugin's state changed since this was last asked.
    [[nodiscard]] bool takeStateMoved() noexcept
    {
        return ! m_crashed.load() && (m_stateMoved.exchange (false) || m_editorOpen.load());
    }

    void keepState (const juce::String& base64)
    {
        juce::MemoryBlock state;
        state.fromBase64Encoding (base64);
        const std::scoped_lock lock (m_stateMutex);
        m_lastState = std::move (state);
    }

    // How long this plugin's blocks have all come back late, in seconds; 0 when it's doing fine.
    [[nodiscard]] double lateForSeconds() const noexcept
    {
        const auto since = m_lateSince.load();
        return since > 0.0 && ! isNonRealtime() ? (juce::Time::getMillisecondCounterHiRes() - since) / 1000.0 : 0.0;
    }

    // A value changed on this side, for the next block.
    void parameterSet (int index) noexcept
    {
        m_dirty[static_cast<size_t> (index)].store (true);
        m_anyDirty.store (true);
        m_stateMoved.store (true);
    }

    [[nodiscard]] Helper& helper() const noexcept { return *m_helper; }

    [[nodiscard]] int id() const noexcept { return m_id; }

    // What the plugin itself did, as the helper reports it.
    void valueFromPlugin (int index, float value)
    {
        if (index >= 0 && std::cmp_less (index, m_parameters.size()))
        {
            m_parameters[static_cast<size_t> (index)]->store (value);
            m_parameters[static_cast<size_t> (index)]->sendValueChangedMessageToListeners (value);
            m_stateMoved.store (true);
        }
    }

    void gestureFromPlugin (int index, bool begin)
    {
        if (index < 0 || std::cmp_greater_equal (index, m_parameters.size()))
            return;

        if (begin)
            m_parameters[static_cast<size_t> (index)]->beginChangeGesture();
        else
            m_parameters[static_cast<size_t> (index)]->endChangeGesture();
    }

    [[nodiscard]] bool takesNativeNotes() const noexcept override { return m_nativeNotes; }

    [[nodiscard]] bool playsOverlappingNotes() const noexcept override { return m_overlapping; }

    void noteOn (uint32_t time, int32_t noteId, int key, double velocity) noexcept override
    {
        queue ({ EventKind::noteOn, static_cast<int> (time), noteId, key, 0, 0, 0, velocity });
    }

    void noteOff (uint32_t time, int32_t noteId, int key, double velocity) noexcept override
    {
        queue ({ EventKind::noteOff, static_cast<int> (time), noteId, key, 0, 0, 0, velocity });
    }

    void noteChoke (uint32_t time, int32_t noteId, int key) noexcept override
    {
        queue ({ EventKind::noteChoke, static_cast<int> (time), noteId, key, 0, 0, 0, 0.0 });
    }

    void chokeAll (uint32_t time) noexcept override
    {
        queue ({ EventKind::chokeAll, static_cast<int> (time), 0, 0, 0, 0, 0, 0.0 });
    }

    void noteExpression (uint32_t time, int32_t noteId, int key, Expression which, double value) noexcept override
    {
        queue ({ EventKind::expression, static_cast<int> (time), noteId, key, static_cast<int> (which), 0, 0, value });
    }

    void controller (uint32_t time, uint8_t status, uint8_t first, uint8_t second) noexcept override
    {
        queue ({ EventKind::controller, static_cast<int> (time), 0, 0, status, first, second, 0.0 });
    }

    [[nodiscard]] bool isModulatable (int parameter) const noexcept override
    {
        return parameter >= 0 && std::cmp_less (parameter, m_parameters.size())
               && m_parameters[static_cast<size_t> (parameter)]->modulatable;
    }

    void modulateParameter (int parameter, double amount, uint32_t time) noexcept override
    {
        queue ({ EventKind::modulate, static_cast<int> (time), 0, 0, parameter, 0, 0, amount });
    }

    void automateParameter (int parameter, float value, uint32_t time) noexcept override
    {
        if (parameter < 0 || std::cmp_greater_equal (parameter, m_parameters.size()))
            return;

        m_parameters[static_cast<size_t> (parameter)]->store (value);
        m_stateMoved.store (true);
        queue ({ EventKind::automate, static_cast<int> (time), 0, 0, parameter, 0, 0, static_cast<double> (value) });
    }

    [[nodiscard]] bool isModulatablePerNote (int parameter) const noexcept override
    {
        return parameter >= 0 && std::cmp_less (parameter, m_parameters.size())
               && m_parameters[static_cast<size_t> (parameter)]->perNote;
    }

    void modulateNoteParameter (uint32_t time, int32_t noteId, int key, int parameter, double amount) noexcept override
    {
        queue ({ EventKind::modulateNote, static_cast<int> (time), noteId, key, parameter, 0, 0, amount });
    }

    void setNoteEndListener (ClapNoteEndListener* listener) noexcept override { m_noteEnds = listener; }

    void latencyChanged (int latency) { setLatencySamples (latency); }

    // The plugin's parameters were added to or removed after it was prepared (on the helper's pipe thread).
    void parameterListChanged()
    {
        if (m_parameterList != nullptr)
            m_parameterList();
    }

    void setParameterListCallback (std::function<void()> callback) override { m_parameterList = std::move (callback); }

    void fillInPluginDescription (juce::PluginDescription& description) const override { description = m_description; }

    [[nodiscard]] const juce::String getName() const override { return m_description.name; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        m_listFixed = true;
        m_ready.store (false);
        m_block.reset();
        m_maxBlock = std::max (1, samplesPerBlock);
        m_sampleRate = sampleRate;
        m_inputs = getTotalNumInputChannels();
        m_outputs = getTotalNumOutputChannels();

        const auto shared = "dawgplug_" + juce::String (currentProcessId()) + "_" + juce::String (m_id) + "_"
                            + juce::String (++m_generation);
        const auto audioBytes = sizeof (float) * static_cast<size_t> ((m_inputs + m_outputs) * m_maxBlock);
        m_block = std::make_unique<SharedBlock> (shared, audioBytes, true);

        if (! m_block->valid())
            return;

        const auto layout = getBusesLayout();
        m_helper->beginLongRequest();
        const auto answer = m_helper->request (object ({ { "kind", "prepare" },
                                                         { "id", m_id },
                                                         { "shared", shared },
                                                         { "sampleRate", sampleRate },
                                                         { "blockSize", m_maxBlock },
                                                         { "inputs", channelsOf (layout.inputBuses) },
                                                         { "outputs", channelsOf (layout.outputBuses) } }),
                                               requestTimeoutMs);

        m_helper->endLongRequest();

        if (! answer.hasProperty ("latency"))
            return;

        setLatencySamples (static_cast<int> (answer["latency"]));
        m_busy = false;
        m_sequence = 0;
        m_lateSince.store (0.0);
        m_ready.store (true);
    }

    void releaseResources() override
    {
        m_ready.store (false);
        std::ignore = m_helper->request (object ({ { "kind", "release" }, { "id", m_id } }), quickTimeoutMs);
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto answer = m_helper->request (object ({ { "kind", "supports" },
                                                         { "id", m_id },
                                                         { "inputs", channelsOf (layouts.inputBuses) },
                                                         { "outputs", channelsOf (layouts.outputBuses) } }),
                                               quickTimeoutMs);
        return static_cast<bool> (answer["ok"]);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const auto numSamples = buffer.getNumSamples();

        // Not there, or still on an earlier block: the sound of this block is lost and the input goes through, but its notes
        // wait for the next one.
        if (! m_ready.load() || numSamples > m_maxBlock || ! exchange (buffer, midi, numSamples))
        {
            for (int ch = m_inputs; ch < buffer.getNumChannels(); ++ch)
                buffer.clear (ch, 0, numSamples);

            midi.clear();
            return;
        }

        const auto* out = m_block->audio() + static_cast<std::ptrdiff_t> (m_inputs) * m_maxBlock;

        for (int ch = 0; ch < m_outputs; ++ch)
            buffer.copyFrom (ch, 0, out + static_cast<std::ptrdiff_t> (ch) * m_maxBlock, numSamples);

        midi.clear();
    }

    using juce::AudioPluginInstance::processBlock;

    [[nodiscard]] double getTailLengthSeconds() const override { return m_tail; }

    [[nodiscard]] bool acceptsMidi() const override { return m_acceptsMidi; }

    [[nodiscard]] bool producesMidi() const override { return false; }

    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    [[nodiscard]] bool hasEditor() const override { return false; }

    [[nodiscard]] int getNumPrograms() override { return 1; }

    [[nodiscard]] int getCurrentProgram() override { return 0; }

    void setCurrentProgram (int /*index*/) override {}

    [[nodiscard]] const juce::String getProgramName (int /*index*/) override { return {}; }

    void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        destData.reset();

        if (! m_crashed.load())
        {
            const auto answer =
                m_helper->request (object ({ { "kind", "getState" }, { "id", m_id } }), requestTimeoutMs);

            if (answer["state"].isString())
            {
                destData.fromBase64Encoding (answer["state"].toString());
                const std::scoped_lock lock (m_stateMutex);
                m_lastState = destData;
                return;
            }
        }

        // A plugin that's gone keeps the state it last had: saving now mustn't lose it.
        const std::scoped_lock lock (m_stateMutex);
        destData = m_lastState;
    }

    // A copy's state, from the helper; the project state where it can't be had.
    void getStateForDuplicate (juce::MemoryBlock& destData) override
    {
        if (! m_crashed.load())
        {
            const auto answer = m_helper->request (
                object ({ { "kind", "getState" }, { "id", m_id }, { "duplicate", true } }), requestTimeoutMs);

            if (answer["state"].isString())
            {
                destData.reset();
                destData.fromBase64Encoding (answer["state"].toString());
                return;
            }
        }

        getStateInformation (destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        const juce::MemoryBlock state (data, static_cast<size_t> (sizeInBytes));
        {
            const std::scoped_lock lock (m_stateMutex);
            m_lastState = state;
        }
        const auto answer = m_helper->request (
            object ({ { "kind", "setState" }, { "id", m_id }, { "state", state.toBase64Encoding() } }),
            requestTimeoutMs);

        // Before the first prepare, a state can bring other parameters (ParameterListSource).
        if (const auto* list = answer["parameters"].getArray();
            list != nullptr && ! m_listFixed && ! sameParameters (*list))
        {
            m_parameters.clear();
            setHostedParameterTree ({});
            addParameters (*list);
        }

        // The state set the plugin's parameters.
        if (const auto* values = answer["values"].getArray())
            for (int i = 0; i < values->size() && std::cmp_less (i, m_parameters.size()); ++i)
                m_parameters[static_cast<size_t> (i)]->store (static_cast<float> (values->getReference (i)));
    }

    void setNonRealtime (bool isNonRealtime) noexcept override
    {
        juce::AudioPluginInstance::setNonRealtime (isNonRealtime);
        m_helper->send (object ({ { "kind", "nonRealtime" }, { "id", m_id }, { "value", isNonRealtime } }));
    }

private:
    void addParameters (const juce::Array<juce::var>& list)
    {
        // NOLINTNEXTLINE(modernize-avoid-c-arrays): atomics cannot live in a vector
        m_dirty = std::make_unique<std::atomic<bool>[]> (static_cast<size_t> (list.size()));

        for (int i = 0; i < list.size(); ++i)
        {
            auto parameter = std::make_unique<RemoteParameter> (*this, i, list.getReference (i));
            m_parameters.push_back (parameter.get());
            addHostedParameter (std::move (parameter));
        }
    }

    [[nodiscard]] bool sameParameters (const juce::Array<juce::var>& list) const
    {
        if (std::cmp_not_equal (list.size(), m_parameters.size()))
            return false;

        for (int i = 0; i < list.size(); ++i)
            if (list.getReference (i)["id"].toString() != m_parameters[static_cast<size_t> (i)]->getParameterID())
                return false;

        return true;
    }

    std::function<void()> m_parameterList;
    bool m_listFixed = false; // from the first prepare on
    [[nodiscard]] static BusesProperties busesFrom (const juce::var& inputs, const juce::var& outputs)
    {
        BusesProperties buses;

        if (const auto* list = inputs.getArray())
            for (const auto& bus : *list)
                buses = buses.withInput (bus["name"].toString(), channelSet (static_cast<int> (bus["defaultChannels"])),
                                         static_cast<bool> (bus["enabled"]));

        if (const auto* list = outputs.getArray())
            for (const auto& bus : *list)
                buses =
                    buses.withOutput (bus["name"].toString(), channelSet (static_cast<int> (bus["defaultChannels"])),
                                      static_cast<bool> (bus["enabled"]));

        return buses;
    }

    // Hands a block over and waits for it, within its own length unless rendering offline. Returns whether it came back.
    [[nodiscard]] bool exchange (const juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi,
                                 int numSamples) noexcept
    {
        auto& header = m_block->header();

        if (m_busy)
        {
            if (header.done.load (std::memory_order_acquire) != m_sequence)
            {
                m_lateBlocks.fetch_add (1);
                keepForLater (midi);
                return false;
            }

            m_busy = false;
            m_lateSince.store (0.0);
            reportNoteEnds (header);
        }

        auto* audio = m_block->audio();

        for (int ch = 0; ch < m_inputs; ++ch)
            std::memcpy (audio + static_cast<std::ptrdiff_t> (ch) * m_maxBlock, buffer.getReadPointer (ch),
                         sizeof (float) * static_cast<size_t> (numSamples));

        flushParameters();
        const auto events = std::min (m_queued, maxBlockEvents);
        std::copy_n (m_queue.data(), events, header.events.data());
        header.eventCount = events;
        m_queued = 0;

        int bytes = 0;

        for (const auto metadata : midi)
        {
            if (bytes + 2 * static_cast<int> (sizeof (int)) + metadata.numBytes > midiCapacity)
                break;

            std::memcpy (header.midi.data() + bytes, &metadata.samplePosition, sizeof (int));
            std::memcpy (header.midi.data() + bytes + sizeof (int), &metadata.numBytes, sizeof (int));
            bytes += 2 * static_cast<int> (sizeof (int));
            std::memcpy (header.midi.data() + bytes, metadata.data, static_cast<size_t> (metadata.numBytes));
            bytes += metadata.numBytes;
        }

        header.midiBytes = bytes;
        header.numSamples = numSamples;
        fillTransport (header);

        const auto sequence = ++m_sequence;
        header.waiting.store (0);
        header.request.store (sequence, std::memory_order_release);
        m_block->wakeHelper();

        const auto start = juce::Time::getHighResolutionTicks();
        const auto ticksPerSecond = static_cast<double> (juce::Time::getHighResolutionTicksPerSecond());
        const auto elapsedSeconds = [&]
        { return static_cast<double> (juce::Time::getHighResolutionTicks() - start) / ticksPerSecond; };
        const auto limit = isNonRealtime() ? 30.0 : static_cast<double> (numSamples) / m_sampleRate;

        while (header.done.load (std::memory_order_acquire) != sequence
               && elapsedSeconds() < static_cast<double> (spinMicros) * 1.0e-6)
            spinPause();

        if (header.done.load (std::memory_order_acquire) == sequence)
        {
            reportNoteEnds (header);
            return true;
        }

        header.waiting.store (1);

        while (header.done.load (std::memory_order_acquire) != sequence)
        {
            if (elapsedSeconds() >= limit)
            {
                m_busy = true;
                m_lateBlocks.fetch_add (1);

                if (m_lateSince.load() <= 0.0)
                    m_lateSince.store (juce::Time::getMillisecondCounterHiRes());

                return false;
            }

            m_block->waitForDone (header.done.load (std::memory_order_acquire), 1);
        }

        reportNoteEnds (header);
        return true;
    }

    void queue (const BlockEvent& event) noexcept
    {
        if (m_queued < maxBlockEvents)
            m_queue[static_cast<size_t> (m_queued++)] = event;
    }

    // A block that couldn't go: what it carried waits, at the start of the next one.
    void keepForLater (const juce::MidiBuffer& midi) noexcept
    {
        for (int i = 0; i < m_queued; ++i)
            m_queue[static_cast<size_t> (i)].time = 0;

        for (const auto metadata : midi)
            if (metadata.numBytes <= 3)
                queue ({ EventKind::midi, 0, metadata.numBytes, 0, metadata.data[0],
                         metadata.numBytes > 1 ? metadata.data[1] : 0, metadata.numBytes > 2 ? metadata.data[2] : 0,
                         0.0 });
    }

    void flushParameters() noexcept
    {
        if (! m_anyDirty.exchange (false))
            return;

        for (size_t i = 0; i < m_parameters.size(); ++i)
            if (m_dirty[i].exchange (false))
                queue ({ EventKind::setValue, 0, 0, 0, static_cast<int> (i), 0, 0,
                         static_cast<double> (m_parameters[i]->getValue()) });
    }

    void reportNoteEnds (const SharedHeader& header) const noexcept
    {
        if (m_noteEnds != nullptr)
            for (int i = 0; i < std::min (header.noteEndCount, maxNoteEnds); ++i)
                m_noteEnds->noteEnded (header.noteEnds[static_cast<size_t> (i)]);
    }

    void fillTransport (SharedHeader& header) const noexcept
    {
        const auto* head = getPlayHead();

        if (head == nullptr)
            return;

        const auto position = head->getPosition();

        if (! position.hasValue())
            return;

        header.bpm = position->getBpm().orFallback (120.0);
        header.ppq = position->getPpqPosition().orFallback (0.0);
        header.barPpq = position->getPpqPositionOfLastBarStart().orFallback (0.0);
        header.seconds = position->getTimeInSeconds().orFallback (0.0);
        header.samples = position->getTimeInSamples().orFallback (0);
        header.playing = position->getIsPlaying() ? 1 : 0;

        if (const auto signature = position->getTimeSignature())
        {
            header.numerator = signature->numerator;
            header.denominator = signature->denominator;
        }
    }

    juce::PluginDescription m_description;
    std::shared_ptr<Helper> m_helper;
    int m_id;
    bool m_acceptsMidi;
    double m_tail;
    std::unique_ptr<SharedBlock> m_block;
    std::atomic<bool> m_ready { false };
    bool m_busy = false;
    uint32_t m_sequence = 0;
    int m_generation = 0;
    int m_maxBlock = 0;
    int m_inputs = 0;
    int m_outputs = 0;
    double m_sampleRate = 48000.0;
    bool m_nativeNotes = false;
    bool m_overlapping = false;
    std::vector<RemoteParameter*> m_parameters;
    std::unique_ptr<std::atomic<bool>[]> m_dirty; // NOLINT(modernize-avoid-c-arrays): atomics cannot live in a vector
    std::atomic<bool> m_anyDirty { false };
    std::vector<BlockEvent> m_queue = std::vector<BlockEvent> (static_cast<size_t> (maxBlockEvents));
    int m_queued = 0;
    ClapNoteEndListener* m_noteEnds = nullptr;
    std::atomic<bool> m_crashed { false };
    std::atomic<int> m_lateBlocks { 0 };
    std::atomic<double> m_lateSince { 0.0 };
    std::atomic<bool> m_stateMoved { false };
    std::function<void()> m_crashCallback;
    std::function<void (std::optional<RemoteEditorPosition>)> m_editorGone;
    std::atomic<bool> m_editorOpen { false };
    mutable std::mutex m_stateMutex;
    juce::String m_crashReason;
    juce::MemoryBlock m_lastState;
};

void RemoteParameter::setValue (float newValue)
{
    m_value.store (newValue);
    m_owner.parameterSet (m_index);
}

juce::String RemoteParameter::getText (float normalisedValue, int maximumStringLength) const
{
    const auto answer = m_owner.helper().request (object ({ { "kind", "parameterText" },
                                                            { "id", m_owner.id() },
                                                            { "index", m_index },
                                                            { "value", normalisedValue } }),
                                                  quickTimeoutMs);
    return answer["text"].toString().substring (0, maximumStringLength);
}

float RemoteParameter::getValueForText (const juce::String& text) const
{
    const auto answer = m_owner.helper().request (
        object ({ { "kind", "parameterText" }, { "id", m_owner.id() }, { "index", m_index }, { "text", text } }),
        quickTimeoutMs);
    return answer.hasProperty ("value") ? static_cast<float> (answer["value"]) : getValue();
}

void Helper::handleMessageFromWorker (const juce::MemoryBlock& block)
{
    const auto message = decode (block);

    if (message.hasProperty ("reply"))
    {
        Reply reply;
        {
            const std::scoped_lock lock (m_mutex);
            const auto found = m_replies.find (static_cast<int> (message["reply"]));

            if (found != m_replies.end())
            {
                reply = std::move (found->second);
                m_replies.erase (found);
            }
        }

        if (reply != nullptr)
            reply (message);

        return;
    }

    const auto event = message["event"].toString();

    if (event == "hello")
    {
        watchProcess (static_cast<int> (message["pid"]));
        return;
    }

    const std::scoped_lock lock (m_mutex);
    const auto found = m_instances.find (static_cast<int> (message["id"]));

    if (found == m_instances.end())
        return;

    if (event == "latency")
        found->second->latencyChanged (static_cast<int> (message["latency"]));
    else if (event == "value")
        found->second->valueFromPlugin (static_cast<int> (message["index"]), static_cast<float> (message["value"]));
    else if (event == "gesture")
        found->second->gestureFromPlugin (static_cast<int> (message["index"]), static_cast<bool> (message["begin"]));
    else if (event == "parameterList")
        found->second->parameterListChanged();
    else if (event == "editorClosed")
        found->second->editorGone (
            RemoteEditorPosition { static_cast<int> (message["x"]), static_cast<int> (message["y"]) });
}

void Helper::handleConnectionLost()
{
    declareLost ("Its process crashed");
}

// However it was found out, this happens once.
void Helper::declareLost (const juce::String& reason)
{
    std::map<int, Reply> replies;
    {
        const std::scoped_lock lock (m_mutex);

        if (m_lost)
            return;

        m_lost = true;
        m_lostFlag.store (true);

        for (const auto& [id, instance] : m_instances)
            instance->connectionLost (reason);

        replies.swap (m_replies);
    }

    for (const auto& [seq, reply] : replies)
        reply (object ({ { "error", remotePluginProcessGone } }));
}

void Helper::watchProcess (int pid)
{
    m_process.open (pid);
    m_processId.store (pid);
    m_lastAnswer.store (juce::Time::getMillisecondCounterHiRes());
    std::ignore = startThread();
}

void Helper::closeHung (const juce::String& reason)
{
    // Said first, so that the process's end doesn't read as a crash.
    declareLost (reason);
    m_process.terminate();
}

// A process's end is known at once (JUCE would take until its ping runs out). A hang shows as blocks all coming back late, or
// the helper's message thread not answering.
void Helper::run()
{
    auto lastPing = juce::Time::getMillisecondCounterHiRes();
    auto lastSnapshot = lastPing;

    while (! threadShouldExit())
    {
        if (m_process.ended (watchIntervalMs))
        {
            declareLost ("Its process crashed");
            return;
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();
        auto hung = false;
        auto snapshot = false;
        std::vector<int> moved;

        if (now - lastSnapshot > snapshotEverySeconds.load() * 1000.0)
        {
            lastSnapshot = now;
            snapshot = true;
        }

        {
            const std::scoped_lock lock (m_mutex);

            for (const auto& [id, instance] : m_instances)
            {
                hung = hung || instance->lateForSeconds() > hungAfterSeconds;

                if (snapshot && instance->takeStateMoved())
                    moved.push_back (id);
            }
        }

        // The instance may be gone by the time the answer comes, so it's looked up again then.
        for (const auto id : moved)
            send (object ({ { "kind", "getState" }, { "id", id } }),
                  [this, id] (const juce::var& answer)
                  {
                      if (! answer["state"].isString())
                          return;

                      const std::scoped_lock lock (m_mutex);
                      const auto found = m_instances.find (id);

                      if (found != m_instances.end())
                          found->second->keepState (answer["state"].toString());
                  });

        if (hung)
        {
            closeHung ("It stopped responding and was closed");
            return;
        }

        if (now - lastPing > pingEverySeconds * 1000.0)
        {
            lastPing = now;
            send (object ({ { "kind", "ping" } }),
                  [this] (const juce::var&) { m_lastAnswer.store (juce::Time::getMillisecondCounterHiRes()); });
        }

        if (m_longRequests.load() == 0 && now - m_lastAnswer.load() > quietAfterSeconds * 1000.0)
        {
            closeHung ("It stopped responding and was closed");
            return;
        }
    }
}
} // namespace

bool remotePluginsAvailable() noexcept
{
    return true;
}

bool remoteHelperIsForeground()
{
 #ifdef _WIN32
    DWORD process = 0;
    GetWindowThreadProcessId (GetForegroundWindow(), &process);
    const auto id = static_cast<int> (process);
    return id != 0 && id != currentProcessId() && helperHasProcess (id);
 #else
    return false; // which window has the focus isn't asked on Linux yet
 #endif
}

void setRemotePluginSnapshotInterval (double seconds)
{
    snapshotEverySeconds.store (seconds);
}

void setRemotePluginExecutable (const juce::File& executable)
{
    const std::scoped_lock lock (poolMutex);
    helperExecutable = executable;
}

void createRemotePlugin (
    const juce::PluginDescription& description, double sampleRate, int blockSize,
    const std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)>& callback)
{
    const auto helper = helperFor (description.fileOrIdentifier);

    if (helper == nullptr)
    {
        callback (nullptr, "Could not start a process for " + description.name);
        return;
    }

    const auto id = nextInstance.fetch_add (1);
    const auto xml = description.createXml()->toString();
    helper->beginLongRequest();
    helper->send (object ({ { "kind", "load" },
                            { "id", id },
                            { "description", xml },
                            { "sampleRate", sampleRate },
                            { "blockSize", blockSize } }),
                  [helper, description, id, callback] (const juce::var& answer)
                  {
                      helper->endLongRequest();

                      if (answer.hasProperty ("error"))
                      {
                          callback (nullptr, answer["error"].toString());
                          return;
                      }

                      callback (std::make_unique<RemoteProcessor> (description, helper, id, answer), {});
                  });
}

std::unique_ptr<RemotePluginHelper>
startRemotePluginHelper (const juce::String& commandLine,
                         const std::function<void (juce::AudioPluginFormatManager&)>& addFormats,
                         std::function<void()> onDisconnect, RemoteEditorFactory makeEditor)
{
    if (! commandLine.contains (juce::String (remotePluginHelperId) + ":"))
        return nullptr;

 #ifdef _WIN32
    // A plugin that crashes here is the app's to report, without Windows asking the user about it.
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
 #endif

    auto helper = std::make_unique<HelperWorker> (addFormats, std::move (onDisconnect), std::move (makeEditor));

    if (! helper->initialiseFromCommandLine (commandLine, remotePluginHelperId, launchTimeoutMs))
        return nullptr;

    return helper;
}

#else

bool remotePluginsAvailable() noexcept
{
    return false;
}

bool remoteHelperIsForeground()
{
    return false;
}

void setRemotePluginSnapshotInterval (double /*seconds*/) {}

void setRemotePluginExecutable (const juce::File& /*executable*/) {}

void createRemotePlugin (
    const juce::PluginDescription& /*description*/, double /*sampleRate*/, int /*blockSize*/,
    const std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)>& callback)
{
    callback (nullptr, "Running plugins in separate processes isn't available on this platform yet");
}

// NOLINTBEGIN(performance-unnecessary-value-param): the signature the Windows and Linux builds have
std::unique_ptr<RemotePluginHelper>
startRemotePluginHelper (const juce::String& /*commandLine*/,
                         const std::function<void (juce::AudioPluginFormatManager&)>& /*addFormats*/,
                         std::function<void()> /*onDisconnect*/, RemoteEditorFactory /*makeEditor*/)
{
    return nullptr;
}
// NOLINTEND(performance-unnecessary-value-param)

#endif

} // namespace dawg
