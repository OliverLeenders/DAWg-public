#include "dawg/AudioEngine.h"

#include "dawg/AllocationGuard.h"
#include "dawg/Modulation.h"
#include "dawg/ParamLinks.h"
#include "dawg/PluginNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace dawg
{

namespace
{
// Highest absolute sample of a buffer.
[[nodiscard]] float peakOf (const float* data, int numSamples) noexcept
{
    const auto range = juce::FloatVectorOperations::findMinAndMax (data, numSamples);
    return std::max (std::abs (range.getStart()), std::abs (range.getEnd()));
}

// Samples between values for a plugin that takes them with sample offsets, as for per-note values.
constexpr int modulationGridSamples = 32;

// A lane's values across a chunk, with sample offsets, for a plugin that takes them (docs/design/clap-hosting.md, C4).
void automateByEvents (const CompiledLane& lane, const Transport& transport) noexcept
{
    const auto& map = transport.timeMap.tempoMap();
    const auto samples = static_cast<int> (std::lround (
        (map.secondsAt (transport.endTick) - map.secondsAt (transport.startTick)) * transport.timeMap.sampleRate()));

    for (int s = 0; s < std::max (1, samples); s += modulationGridSamples)
    {
        const auto tick = transport.startTick
                          + (transport.endTick - transport.startTick) * static_cast<double> (s) / std::max (1, samples);
        lane.plugin->automateParameter (lane.pluginParam, static_cast<float> (evaluateCurve (lane.points, tick, 0.0)),
                                        static_cast<uint32_t> (s));
    }
}

// Evaluates a track's automation for a chunk: node and plugin parameters are set, fader and pan returned.
[[nodiscard]] TrackMix automate (const CompiledTrack& track, const Transport& transport) noexcept
{
    TrackMix mix;
    mix.gainStart = mix.gainEnd = track.state->gain.load();
    mix.panStart = mix.panEnd = track.state->pan.load();

    for (size_t i = 0; i < track.sends.size() && i < limits::maxSends; ++i)
        mix.sends[i].fill (track.sends[i].state->gain.load());

    const auto off = track.state->automationMode.load() == static_cast<int> (AutomationMode::off);

    for (const auto& lane : track.lanes)
    {
        if (off || lane.state->overridden.load())
        {
            lane.state->value.store (-1.0f);
            continue;
        }

        const auto start = evaluateCurve (lane.points, transport.startTick, 0.0);
        const auto end = evaluateCurve (lane.points, transport.endTick, 0.0);
        lane.state->value.store (static_cast<float> (end));

        switch (lane.kind)
        {
            case AutomationTargetKind::gain:
                mix.gainStart = faderGain (start);
                mix.gainEnd = faderGain (end);
                break;
            case AutomationTargetKind::pan:
                mix.panStart = static_cast<float> (2.0 * start - 1.0);
                mix.panEnd = static_cast<float> (2.0 * end - 1.0);
                break;
            case AutomationTargetKind::nodeParam:
            {
                const auto value = paramFromNormalised (lane.info, start);
                lane.node->params().set (lane.param, value);

                for (auto* voice : lane.voiceNodes)
                    voice->params().set (lane.param, value);

                break;
            }
            case AutomationTargetKind::pluginParam:
                if (lane.plugin->takesParameterEvents())
                    automateByEvents (lane, transport);
                else
                    lane.plugin->setAutomatedParameter (lane.pluginParam, static_cast<float> (start));
                break;
            case AutomationTargetKind::send:
                if (lane.send >= 0 && static_cast<size_t> (lane.send) < limits::maxSends)
                    mix.sends[static_cast<size_t> (lane.send)] = { faderGain (start), faderGain (end) };
                break;
        }
    }

    return mix;
}

// Adds a track's output to a bus with its fader and pan, and raises its meters (mixing-basics.md, M1, M2).
// A mono bus (both channels the same buffer) gets the left channel only, without pan. Automated fader and pan
// ramp across the chunk.
// Adds a signal to a bus with per-channel gains that ramp from `from` to `to` across the chunk.
void addRamped (std::array<float*, 2> bus, AudioInput source, std::array<float, 2> from, std::array<float, 2> to,
                size_t channels, int numSamples) noexcept
{
    for (size_t ch = 0; ch < channels; ++ch)
    {
        if (std::abs (to[ch] - from[ch]) < 1.0e-7f)
        {
            juce::FloatVectorOperations::addWithMultiply (bus[ch], source[ch], to[ch], numSamples);
            continue;
        }

        const auto step = (to[ch] - from[ch]) / static_cast<float> (numSamples);

        for (int s = 0; s < numSamples; ++s)
            bus[ch][s] += source[ch][s] * (from[ch] + step * static_cast<float> (s + 1));
    }
}

// A track's fader and pan as per-channel gains, at the start and the end of the chunk.
[[nodiscard]] std::array<std::array<float, 2>, 2> faderGains (const TrackMix& mix, bool stereo) noexcept
{
    const auto gainsAt = [stereo] (float gain, float pan)
    {
        const auto p = stereo ? panGains (pan) : std::array { 1.0f, 1.0f };
        return std::array { gain * p[0], gain * p[1] };
    };

    return { gainsAt (mix.gainStart, mix.panStart), gainsAt (mix.gainEnd, mix.panEnd) };
}

// Adds a track's output to a bus with its fader and pan, and raises its meters (mixing-basics.md, M1, M2).
// A mono bus (both channels the same buffer) gets the left channel only, without pan.
void mixTrack (const CompiledTrack& track, const TrackMix& mix, std::array<float*, 2> bus, int numSamples) noexcept
{
    const auto stereo = bus[0] != bus[1];
    const auto [from, to] = faderGains (mix, stereo);
    addRamped (bus, track.output, from, to, stereo ? size_t { 2 } : size_t { 1 }, numSamples);

    raisePeak (track.state->peakLeft, peakOf (track.output[0], numSamples) * std::max (from[0], to[0]));
    raisePeak (track.state->peakRight, peakOf (track.output[1], numSamples) * std::max (from[1], to[1]));
}

// Adds a track's sends to their buses (docs/design/groups-and-sends.md, R4): before the fader at the send level, or
// after the fader and pan scaled by it. Either the sends to returns (keys false) or the key sends (keys true;
// docs/design/sidechain.md, K2).
template <typename BusOf, typename Wanted>
void mixSends (const CompiledTrack& track, const TrackMix& mix, const BusOf& busOf, const Wanted& wanted,
               int numSamples, bool keys) noexcept
{
    const auto [faderFrom, faderTo] = faderGains (mix, true);

    for (size_t i = 0; i < track.sends.size() && i < limits::maxSends; ++i)
    {
        const auto& send = track.sends[i];

        if (send.key != keys || send.bus < 0 || ! wanted (send.bus) || ! send.state->active.load())
            continue;

        const auto [levelFrom, levelTo] = mix.sends[i];
        const auto pre = send.state->preFader.load();
        const std::array from { levelFrom * (pre ? 1.0f : faderFrom[0]), levelFrom * (pre ? 1.0f : faderFrom[1]) };
        const std::array to { levelTo * (pre ? 1.0f : faderTo[0]), levelTo * (pre ? 1.0f : faderTo[1]) };
        addRamped (busOf (send.bus), send.source, from, to, 2, numSamples);
    }
}

// A chunk of transport to render, split at the loop end when a track runs far enough ahead to cross it before the
// playhead does.
//
// Tracks (and the master) see the transport early so latent chains line up with the playhead. Without an early wrap
// that lead would skip the first samples of the new iteration: an audio clip starting at the loop start would begin
// `ahead` samples in. A lead shorter than the loop is split at the loop end; the second segment starts at the loop
// start and is rendered `split` samples into the track's output buffer. A lead longer than the loop first maps its
// start back into the current iteration with a modulo, then splits the same way when it crosses the loop end.
struct AheadBlock
{
    Transport first;
    Transport second;
    int split = 0;
    bool hasSecond = false;
};

[[nodiscard]] AheadBlock aheadOf (const Transport& transport, const TimeMap& timeMap, juce::int64 position,
                                  int numSamples, int samples, bool looping, const LoopRange& loop) noexcept
{
    AheadBlock block;
    block.first = transport;

    if (samples <= 0)
        return block;

    const auto start = position + samples;
    const auto from = static_cast<double> (start);
    block.first.startSample = start;
    block.first.startTick = timeMap.sampleToTick (from);
    block.first.endTick = transport.playing ? timeMap.sampleToTick (from + numSamples) : block.first.startTick;

    if (! looping || numSamples <= 0)
        return block;

    // A track ahead of the playhead wraps on its own, earlier than the playhead: `looped` means this track's content
    // just went back to the loop start, once per iteration. Repeating it would restart the notes under the track.
    block.first.looped = false;

    const auto loopEndSample =
        static_cast<juce::int64> (std::llround (timeMap.tickToSample (static_cast<double> (loop.end))));
    const auto loopStartSample =
        static_cast<juce::int64> (std::llround (timeMap.tickToSample (static_cast<double> (loop.start))));
    const auto loopLength = loopEndSample - loopStartSample;

    if (loopLength <= 0)
        return block;

    auto wrapped = timeMap;
    wrapped.anchor (static_cast<double> (loop.start), loopEndSample);

    // A lead longer than the loop starts several iterations in. Map the start back into the current iteration with a
    // modulo, so an offset of, say, two loop lengths starts at the loop start rather than two loops past its end.
    auto contentStart = start;

    if (start >= loopEndSample)
    {
        contentStart = loopStartSample + (start - loopStartSample) % loopLength;
        block.first.startSample = contentStart;
        block.first.startTick = timeMap.sampleToTick (static_cast<double> (contentStart));
        block.first.endTick = transport.playing ? timeMap.sampleToTick (static_cast<double> (contentStart + numSamples))
                                                : block.first.startTick;
        // The previous block ended exactly at the loop end, so it did not split. Any later block is past the wrap.
        block.first.looped = contentStart == loopStartSample;
    }

    if (contentStart + numSamples <= loopEndSample)
        return block;

    // The track crosses the loop end inside this chunk. Render the two halves separately so the second half starts
    // exactly at the loop start but lands `split` samples into the output.
    block.split = static_cast<int> (loopEndSample - contentStart);
    block.first.endTick = static_cast<double> (loop.end);
    block.second = transport;
    block.second.startSample = loopEndSample;
    block.second.startTick = static_cast<double> (loop.start);
    block.second.endTick = transport.playing
                               ? wrapped.sampleToTick (static_cast<double> (loopEndSample + numSamples - block.split))
                               : block.second.startTick;
    block.second.timeMap = wrapped;
    block.second.started = false;
    block.second.stopped = false;
    block.second.jumped = false;
    block.second.looped = true;
    block.hasSecond = true;

    return block;
}
} // namespace

namespace
{
constexpr double clickGain = 0.25;
constexpr double downbeatFrequencyHz = 1320.0;
constexpr double beatFrequencyHz = 880.0;
constexpr double clickDecaySeconds = 0.04;
constexpr double clickSilenceThreshold = 1.0e-4;
constexpr double subdivisionLevel = 0.35;
// The stereo output, then the taps of a freeze render (AudioEngine::setExportMode).
constexpr int maxOutputChannels = 2 + 2 * static_cast<int> (AudioEngine::maxExportTaps);

// Exact comparison for change detection (not arithmetic equality), without tripping -Wfloat-equal.
[[nodiscard]] bool differs (double a, double b) noexcept
{
    return std::less<> {}(a, b) || std::less<> {}(b, a);
}
} // namespace

// Renders the tracks on a fixed set of threads. The audio thread wakes the workers, takes work itself, and spins
// until every worker has finished; only one block is ever in flight, so the workers need no locks. Each worker has its
// own live-event scratch, and the tracks it renders write only their own compiled arenas.
//
// A track is ready once every track that writes a bus it reads has rendered (CompiledTrack::fedBy); then whoever takes
// it sums those tracks' contributions into its buses, in track order, and renders it. Tracks are taken in the order
// they became ready, so a group starts as soon as its members are done, whatever else is still rendering.
//
// A thread with no track to take helps the tracks still rendering: a node in one of them (a Voice Instrument) hands
// its independent pieces to forEach(), and every idle thread takes some (docs/design/phase2-engine.md, 5.6).
//
// Real-time safety (CODING_GUIDELINES.md, section 4): the audio thread never takes a lock or blocks. It wakes the
// workers through an atomic wait/notify (a futex or WaitOnAddress, no mutex) and waits for them by spinning, which
// is bounded by the tracks' own render time. The workers run at real-time priority, so the callback never waits on a
// thread the scheduler has put behind ordinary work.
class AudioEngine::WorkerPool final : public Parallel
{
public:
    explicit WorkerPool (AudioEngine& engine)
        : m_engine (engine)
    {
        const auto cores = static_cast<int> (std::thread::hardware_concurrency());
        const auto count = std::clamp (cores - 1, 0, maxWorkers);
        m_live.resize (static_cast<size_t> (count) + 1);

        for (int i = 0; i < count; ++i)
        {
            auto worker = std::make_unique<Worker> (*this, i + 1);

            if (! worker->startRealtimeThread (juce::Thread::RealtimeOptions {}))
                std::ignore = worker->startThread (juce::Thread::Priority::highest);

            m_workers.push_back (std::move (worker));
        }
    }

    ~WorkerPool()
    {
        for (auto& worker : m_workers)
            worker->signalThreadShouldExit();

        // Wake every worker so it sees the exit flag.
        m_generation.fetch_add (1);
        m_generation.notify_all();

        for (auto& worker : m_workers)
            std::ignore = worker->stopThread (-1);
    }

    WorkerPool (const WorkerPool&) = delete;
    WorkerPool& operator= (const WorkerPool&) = delete;
    WorkerPool (WorkerPool&&) = delete;
    WorkerPool& operator= (WorkerPool&&) = delete;

    // The chunk being rendered: the compiled engine, its transport and the block's range in the output.
    struct Job
    {
        const CompiledEngine* engine = nullptr;
        const Transport* transport = nullptr;
        int offset = 0;
        int numSamples = 0;
        bool loopActive = false;
        bool stems = false;
        bool anySolo = false;
    };

    // Returns how many worker threads the pool has (the audio thread helps on top of those).
    [[nodiscard]] int workers() const noexcept { return static_cast<int> (m_workers.size()); }

    // Renders every track, on the workers and the calling thread.
    void render (const Job& job) noexcept
    {
        const auto& engine = *job.engine;
        const auto count = engine.tracks.size();

        for (size_t i = 0; i < count; ++i)
        {
            engine.waiting[i].store (static_cast<int> (engine.tracks[i].fedBy.size()));
            engine.readyOrder[i].store (-1);
        }

        m_ready.store (0);

        for (size_t i = 0; i < count; ++i)
            if (engine.tracks[i].fedBy.empty())
                engine.readyOrder[m_ready.fetch_add (1)].store (static_cast<int> (i));

        m_job.store (&job);
        m_next.store (0);
        m_tracksLeft.store (count);
        m_remaining.store (workers());

        // A new generation releases every worker once; the stores above are visible to them before it.
        m_generation.fetch_add (1);
        m_generation.notify_all();

        work (job, 0, false);

        // The workers are finishing: spin until they are done rather than block.
        while (m_remaining.load() != 0)
            std::this_thread::yield();
    }

    // Runs the pieces on this thread and on every thread that has no track left to render.
    void forEach (size_t count, Task task, void* context) noexcept override
    {
        Assist assist;
        assist.task = task;
        assist.context = context;
        assist.count = count;
        auto slot = m_assists.size();

        for (size_t i = 0; i < m_assists.size() && slot == m_assists.size(); ++i)
        {
            Assist* expected = nullptr;

            if (m_assists[i].compare_exchange_strong (expected, &assist))
                slot = i;
        }

        if (slot == m_assists.size())
        {
            std::ignore = assist.run(); // every slot is taken: this thread does it all
            return;
        }

        wakeHelpers();

        std::ignore = assist.run();

        // No one joins from now on: wait for the pieces others took, and for them to let go of the assist.
        m_assists[slot].store (nullptr);

        while (assist.done.load() != count || m_assistUsers[slot].load() != 0)
            std::this_thread::yield();
    }

private:
    static constexpr int maxWorkers = 8;

    // At most one assist per track rendering at once, and at most one track per thread.
    static constexpr size_t maxAssists = static_cast<size_t> (maxWorkers) + 1;

    // The pieces a node handed to forEach(), taken one at a time by whoever runs them.
    struct Assist
    {
        Task task = nullptr;
        void* context = nullptr;
        size_t count = 0;
        std::atomic<size_t> next { 0 };
        std::atomic<size_t> done { 0 };

        // Runs pieces until none are left. Returns true if it ran any.
        [[nodiscard]] bool run() noexcept
        {
            auto ran = false;

            for (auto index = next.fetch_add (1); index < count; index = next.fetch_add (1))
            {
                task (context, index);
                done.fetch_add (1);
                ran = true;
            }

            return ran;
        }
    };

    class Worker final : public juce::Thread
    {
    public:
        Worker (WorkerPool& pool, int scratch)
            : juce::Thread ("DAWg track renderer"),
              m_pool (pool),
              m_scratch (scratch)
        {
        }

        void run() override { m_pool.runWorker (*this, m_scratch); }

    private:
        WorkerPool& m_pool;
        int m_scratch = 0;
    };

    void runWorker (const juce::Thread& thread, int scratch) noexcept
    {
        const ScopedNoAllocation noAllocation;
        const juce::ScopedNoDenormals noDenormals;

        // Starts at the pool's initial generation, not at the value read here: a block can be rendered before this
        // thread first runs, and it must not be missed.
        std::uint32_t seen = 0;

        for (;;)
        {
            m_generation.wait (seen);
            seen = m_generation.load();

            if (thread.threadShouldExit())
                return;

            work (*m_job.load(), scratch, true);
            m_remaining.fetch_sub (1);
        }
    }

    // Wakes the threads waiting for a track or for pieces to help with.
    void wakeHelpers() noexcept
    {
        m_events.fetch_add (1);
        m_events.notify_all();
    }

    // Waits, helping meanwhile, until `done` is true. A worker may sleep while there is nothing to help with; the
    // audio thread spins.
    template <typename Done>
    void waitUntil (const Done& done, bool mayBlock) noexcept
    {
        for (;;)
        {
            if (done())
                return;

            const auto seen = m_events.load();

            if (help())
                continue;

            if (done())
                return;

            if (mayBlock)
                m_events.wait (seen);
            else
                std::this_thread::yield();
        }
    }

    // Takes tracks in the order they became ready until none are left, then helps the ones still rendering until
    // they are all done.
    void work (const Job& job, int scratch, bool mayBlock) noexcept
    {
        const auto& engine = *job.engine;
        const auto liveScratch = std::span<NoteEvent> (m_live[static_cast<size_t> (scratch)]);

        for (;;)
        {
            const auto ticket = m_next.fetch_add (1);

            if (ticket >= engine.tracks.size())
                break;

            // The ticket's track may not be ready yet: a group whose members are still rendering.
            auto index = -1;
            waitUntil ([&] { return (index = engine.readyOrder[ticket].load()) >= 0; }, mayBlock);
            render (job, static_cast<size_t> (index), liveScratch);
        }

        waitUntil ([this] { return m_tracksLeft.load() == 0; }, mayBlock);
    }

    // Sums what the track's sources send to its buses, renders it, and makes the tracks waiting for it ready.
    void render (const Job& job, size_t index, std::span<NoteEvent> liveScratch) noexcept
    {
        const auto& engine = *job.engine;
        const auto& track = engine.tracks[index];

        for (const auto source : track.fedBy)
        {
            const auto from = static_cast<size_t> (source);
            m_engine.mixRenderedTrack (engine, engine.tracks[from], engine.trackMix[from], { nullptr, nullptr }, {},
                                       job.stems, job.anySolo, job.numSamples, static_cast<int> (index));
        }

        m_engine.renderTrackSteps (engine, track, index, *job.transport, job.offset, job.numSamples, job.loopActive,
                                   liveScratch, this);

        auto woken = false;

        for (const auto reader : track.feeds)
            if (engine.waiting[static_cast<size_t> (reader)].fetch_sub (1) == 1)
            {
                engine.readyOrder[m_ready.fetch_add (1)].store (reader);
                woken = true;
            }

        // The last track lets the helpers go.
        if (m_tracksLeft.fetch_sub (1) == 1 || woken)
            wakeHelpers();
    }

    // Runs pieces of any published assist. Returns true if it ran any.
    //
    // A helper counts itself on the slot before it reads the slot's assist, and the owner clears the slot before it
    // waits for that count to reach zero, so no one reads an assist after its owner has returned.
    [[nodiscard]] bool help() noexcept
    {
        for (size_t slot = 0; slot < m_assists.size(); ++slot)
        {
            if (m_assists[slot].load() == nullptr)
                continue;

            m_assistUsers[slot].fetch_add (1);
            auto* assist = m_assists[slot].load();
            const auto ran = assist != nullptr && assist->run();
            m_assistUsers[slot].fetch_sub (1);

            if (ran)
                return true;
        }

        return false;
    }

    AudioEngine& m_engine;
    std::vector<std::unique_ptr<Worker>> m_workers;
    std::vector<std::array<NoteEvent, AudioEngine::maxLiveEvents>> m_live;
    std::atomic<const Job*> m_job { nullptr };
    std::atomic<size_t> m_next { 0 };  // the next ticket: a place in CompiledEngine::readyOrder
    std::atomic<size_t> m_ready { 0 }; // how many places in readyOrder have been given a track
    std::atomic<size_t> m_tracksLeft { 0 };
    std::atomic<int> m_remaining { 0 };
    std::atomic<std::uint32_t> m_generation { 0 };
    std::array<std::atomic<Assist*>, maxAssists> m_assists {};
    std::array<std::atomic<int>, maxAssists> m_assistUsers {};
    std::atomic<std::uint32_t> m_events { 0 }; // a track became ready, pieces were handed out, or the tracks are done
};

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() = default;

bool AudioEngine::defaultParallelTracks() noexcept
{
    return juce::SystemStats::getEnvironmentVariable ("DAWG_PARALLEL_TRACKS", "1") != "0";
}

void AudioEngine::setEngine (std::shared_ptr<const CompiledEngine> engine)
{
    jassert (engine != nullptr);

    // Created here, on the message thread, before the audio thread can run an engine.
    if (m_workerPool == nullptr)
        m_workerPool = std::make_unique<WorkerPool> (*this);

    m_latencySamplesForUi.store (engine->latencySamples);
    m_engines.publish (std::move (engine));
}

void AudioEngine::setParallelTracks (bool enabled) noexcept
{
    m_parallelTracks.store (enabled);
}

void AudioEngine::collectGarbage()
{
    m_engines.collectGarbage();
    m_previews.collectGarbage();
}

void AudioEngine::preview (std::shared_ptr<const AudioSource> source)
{
    // Scratch for one block of file frames at up to 8x the device rate (allocated here, never on the audio thread).
    constexpr size_t maxFrames = 8 * 8192 + 4;
    for (auto& channel : m_previewScratch)
        if (channel.size() < maxFrames)
            channel.assign (maxFrames, 0.0f);

    m_previews.publish (std::make_shared<const PreviewVoice> (PreviewVoice { std::move (source) }));
}

void AudioEngine::renderPreview (std::span<float* const> outputs, int numSamples) noexcept
{
    if (const auto* next = m_previews.acquire(); next != nullptr && next != m_preview)
    {
        m_preview = next;
        m_previewFrame = 0.0;
    }

    if (m_preview == nullptr || m_preview->source == nullptr)
    {
        m_previewSecondsForUi.store (-1.0);
        return;
    }

    const auto& source = *m_preview->source;
    const auto step = source.sampleRate() / m_deviceSampleRate;
    const auto first = static_cast<juce::int64> (std::floor (m_previewFrame));
    const auto count =
        std::min (static_cast<int> (std::ceil (numSamples * step)) + 2, static_cast<int> (m_previewScratch[0].size()));

    if (m_previewFrame >= static_cast<double> (source.frames()))
    {
        m_previewSecondsForUi.store (-1.0); // finished
        return;
    }

    std::array<float*, 2> scratch { m_previewScratch[0].data(), m_previewScratch[1].data() };
    std::ignore = source.read (scratch, first, count);

    // Linear interpolation is enough for auditioning.
    const auto gain = m_previewGain.load();
    const auto stereo = outputs[0] != outputs[1];

    for (int s = 0; s < numSamples; ++s)
    {
        const auto position = m_previewFrame + s * step - static_cast<double> (first);
        const auto index = static_cast<int> (position);

        if (index + 1 >= count)
            break;

        const auto frac = static_cast<float> (position - index);
        const auto i = static_cast<size_t> (index);
        outputs[0][s] += (scratch[0][i] + (scratch[0][i + 1] - scratch[0][i]) * frac) * gain;

        if (stereo)
            outputs[1][s] += (scratch[1][i] + (scratch[1][i + 1] - scratch[1][i]) * frac) * gain;
    }

    m_previewFrame += numSamples * step;
    m_previewSecondsForUi.store (m_previewFrame / source.sampleRate());
}

void AudioEngine::play() noexcept
{
    m_playRequested.store (true);
}

void AudioEngine::stop() noexcept
{
    m_stopRequested.store (true);
}

void AudioEngine::seek (double beats) noexcept
{
    m_seekRequest.store (std::max (0.0, beats));
}

bool AudioEngine::isPlaying() const noexcept
{
    return m_playing.load();
}

ObjectId AudioEngine::auditionNoteOn (ObjectId trackId, double pitch, float velocity) noexcept
{
    LiveInput input;
    input.trackId = trackId;
    input.noteId = nextLiveNoteId();
    input.pitch = pitch;
    input.velocity = std::clamp (velocity, 0.0f, 1.0f);
    return m_liveRequests.push (input) ? input.noteId : 0;
}

void AudioEngine::auditionNoteOff (ObjectId trackId, ObjectId noteId) noexcept
{
    LiveInput input;
    input.trackId = trackId;
    input.noteId = noteId;
    input.type = NoteEventType::noteOff;
    input.velocity = 0.5f;
    std::ignore = m_liveRequests.push (input);
}

bool AudioEngine::sendLiveInput (const LiveInput& input) noexcept
{
    return m_inputRequests.push (input);
}

void AudioEngine::takeLiveRequests (int numSamples) noexcept
{
    while (const auto request = m_liveRequests.pop())
        takeLiveRequest (*request, numSamples);

    while (const auto request = m_inputRequests.pop())
        takeLiveRequest (*request, numSamples);

    // Recording taps the coalesced events, with the input latency taken off their positions (M5).
    if (m_recording.load() && m_isPlaying)
    {
        for (size_t i = 0; i < m_pendingLiveCount; ++i)
        {
            const auto& pending = m_pendingLive[i];
            auto input = pending.input;
            input.value = pending.event.value;
            const auto sample = m_samplePosition + pending.event.sampleOffset - m_inputLatency.load();
            std::ignore = m_recordedInputs.push (
                { input, m_timeMap.sampleToTick (static_cast<double> (sample)), m_recordLoopWraps.load() });
        }
    }
}

void AudioEngine::takeLiveRequest (const LiveInput& request, int numSamples) noexcept
{
    NoteEvent event;
    event.noteId = request.noteId;
    event.type = request.type;
    event.lane = request.lane;
    event.value = request.value;

    // Timed events land where they happened within the previous block's duration (M3).
    const auto callback = m_callbackSeconds.load();

    if (request.time > 0.0 && callback > 0.0 && numSamples > 0)
    {
        const auto blockStart = callback - numSamples / m_deviceSampleRate;
        event.sampleOffset =
            std::clamp (static_cast<int> ((request.time - blockStart) * m_deviceSampleRate), 0, numSamples - 1);
    }

    auto& note = m_liveNotes[static_cast<size_t> (-request.noteId) % maxLiveNotes];

    if (request.type == NoteEventType::noteOn)
    {
        // Slots are reused round-robin; a slot is only overwritten after maxLiveNotes newer notes. The initial
        // expression is a one-point curve, so the note starts with it.
        auto& curves = m_liveCurves[static_cast<size_t> (-request.noteId) % maxLiveNotes];
        note = {};
        note.id = request.noteId;
        note.start = std::llround (m_timeMap.sampleToTick (static_cast<double> (m_samplePosition)));
        note.end = std::numeric_limits<Ticks>::max() / 2;
        note.pitch = request.pitch;
        note.velocity = request.velocity;

        for (size_t lane = 0; lane < curves.size(); ++lane)
        {
            curves[lane] = {};
            curves[lane].value = static_cast<double> (request.initial[lane]);
            note.lanes[lane] = { &curves[lane], 1 };
        }

        event.note = &note;
    }
    else if (request.type == NoteEventType::noteOff && request.noteId < 0 && note.id == request.noteId)
    {
        note.releaseVelocity = request.velocity;
    }

    // Expression and controllers are coalesced: the latest value per note, lane and track wins.
    if (request.type == NoteEventType::expression || request.type == NoteEventType::controller)
    {
        for (size_t i = 0; i < m_pendingLiveCount; ++i)
        {
            auto& pending = m_pendingLive[i];

            if (pending.trackId == request.trackId && pending.event.type == event.type
                && pending.event.noteId == event.noteId && pending.event.lane == event.lane)
            {
                pending.event.value = event.value;
                pending.event.sampleOffset = std::max (pending.event.sampleOffset, event.sampleOffset);
                return;
            }
        }
    }

    if (m_pendingLiveCount < m_pendingLive.size())
        m_pendingLive[m_pendingLiveCount++] = { request.trackId, event, request };
}

std::span<const NoteEvent> AudioEngine::liveEventsFor (ObjectId trackId, int numSamples,
                                                       std::span<NoteEvent> scratch) noexcept
{
    if (! m_deliverLive)
        return {};

    size_t count = 0;

    for (size_t i = 0; i < m_pendingLiveCount; ++i)
    {
        if (m_pendingLive[i].trackId != trackId)
            continue;

        // The first chunk of a block can be shorter than the block (a loop wrap).
        auto event = m_pendingLive[i].event;
        event.sampleOffset = std::min (event.sampleOffset, std::max (0, numSamples - 1));
        scratch[count++] = event;
    }

    // Nodes add these to their event buffers, which keep them sorted by offset.
    return { scratch.data(), count };
}

Telemetry AudioEngine::getTelemetry() noexcept
{
    Telemetry t;
    t.playing = m_playing.load();
    t.positionBeats = m_positionForUi.load();
    t.peakLeft = m_peakLeft.exchange (0.0f);
    t.peakRight = m_peakRight.exchange (0.0f);
    t.sampleRate = m_deviceSampleRateForUi.load();
    t.latencySamples = m_latencySamplesForUi.load();
    t.recording = m_recording.load();
    t.previewSeconds = m_previewSecondsForUi.load();
    t.inputPeak = m_inputPeak.exchange (0.0f);
    t.outputMute = m_outputMute.load();
    return t;
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    m_deviceSampleRate = device->getCurrentSampleRate();
    m_deviceSampleRateForUi.store (m_deviceSampleRate);
}

void AudioEngine::audioDeviceStopped()
{
    m_deviceSampleRateForUi.store (0.0);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& /*context*/) noexcept
{
    m_numInputs = std::min (numInputChannels, maxInputs);

    for (int ch = 0; ch < m_numInputs; ++ch)
        m_inputs[static_cast<size_t> (ch)] = inputChannelData[ch];

    // Input level for the UI (peak-hold until polled).
    float inputPeak = 0.0f;

    for (int ch = 0; ch < m_numInputs; ++ch)
        if (inputChannelData[ch] != nullptr)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (inputChannelData[ch], numSamples);
            inputPeak = std::max ({ inputPeak, std::abs (range.getStart()), std::abs (range.getEnd()) });
        }

    if (inputPeak > m_inputPeak.load())
        m_inputPeak.store (inputPeak);

    m_callbackSeconds.store (juce::Time::getMillisecondCounterHiRes() * 0.001);
    process (outputChannelData, numOutputChannels, numSamples);
    protectOutput (outputChannelData, numOutputChannels, numSamples);
}

// Guards ears and speakers (docs/design/output-protection.md, S2, S3): the device output mutes when it runs away, and
// is clipped to full scale. Offline rendering doesn't come here (S4).
void AudioEngine::protectOutput (float* const* outputs, int numChannels, int numSamples) noexcept
{
    if (m_unmuteRequested.exchange (false))
    {
        m_outputMute.store (OutputMute::none);
        m_overloadSamples = 0.0;
    }

    if (m_outputMute.load() == OutputMute::none)
        detectRunaway (outputs, numChannels, numSamples);

    // The fade towards silence (muted) or back to unity, then the ceiling. Invalid samples become silence first, so
    // the gain never multiplies a NaN.
    const auto target = m_outputMute.load() == OutputMute::none ? 1.0f : 0.0f;
    const auto step = static_cast<float> (1.0 / std::max (1.0, protectionFadeSeconds * m_deviceSampleRate));
    const auto startGain = m_protectionGain;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* channel = outputs[ch];

        if (channel == nullptr)
            continue;

        auto gain = startGain;

        for (int s = 0; s < numSamples; ++s)
        {
            gain = target > gain ? std::min (target, gain + step) : std::max (target, gain - step);
            const auto x = std::isfinite (channel[s]) ? channel[s] : 0.0f;
            channel[s] = std::clamp (x * gain, -1.0f, 1.0f);
        }

        m_protectionGain = gain;
    }
}

// Mutes the device output (S3) on a sample beyond extremeLevel or not a number, at once, or on a block RMS above full
// scale for overloadSeconds.
void AudioEngine::detectRunaway (const float* const* outputs, int numChannels, int numSamples) noexcept
{
    auto extreme = false;
    float meanSquare = 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto* channel = outputs[ch];

        if (channel == nullptr)
            continue;

        float sum = 0.0f;

        for (int s = 0; s < numSamples; ++s)
        {
            const auto x = channel[s];
            extreme = extreme || ! std::isfinite (x) || std::abs (x) > extremeLevel;
            sum += x * x;
        }

        meanSquare = std::max (meanSquare, numSamples > 0 ? sum / static_cast<float> (numSamples) : 0.0f);
    }

    m_overloadSamples = meanSquare > 1.0f ? m_overloadSamples + numSamples : 0.0;

    if (extreme)
        m_outputMute.store (OutputMute::extreme);
    else if (m_overloadSamples >= overloadSeconds * m_deviceSampleRate)
        m_outputMute.store (OutputMute::overload);
}

void AudioEngine::renderOffline (float* const* outputs, int numChannels, int numSamples, double sampleRate,
                                 const float* const* inputs, int numInputs) noexcept
{
    m_deviceSampleRate = sampleRate;
    m_callbackSeconds.store (0.0); // offline: timed live events land at the block start
    m_numInputs = inputs != nullptr ? std::min (numInputs, maxInputs) : 0;

    for (int ch = 0; ch < m_numInputs; ++ch)
        m_inputs[static_cast<size_t> (ch)] = inputs[ch];

    process (outputs, numChannels, numSamples);
}

void AudioEngine::startRecording (std::span<const RecordTarget> targets, double fromBeats, double untilBeats) noexcept
{
    const auto count = std::min (targets.size(), maxRecordTargets);
    m_recording.store (false);

    for (size_t i = 0; i < count; ++i)
    {
        m_recordTargets[i] = targets[i];
        m_recordedFrames[i].store (0);
    }

    m_numRecordTargets.store (count);
    m_recordFromBeats.store (fromBeats);
    m_recordUntilBeats.store (untilBeats);
    m_recordStartBeats.store (-1.0);
    m_recordLoopWraps.store (0);
    m_recording.store (true);
}

void AudioEngine::stopRecording() noexcept
{
    m_recording.store (false);
}

juce::int64 AudioEngine::recordedFrames (size_t index) const noexcept
{
    return index < maxRecordTargets ? m_recordedFrames[index].load() : 0;
}

std::array<const float*, 2> AudioEngine::inputFor (const TrackState& state, int offset) const noexcept
{
    const auto channel = state.inputChannel.load();

    if (channel < 0 || channel >= m_numInputs || m_inputs[static_cast<size_t> (channel)] == nullptr)
        return { nullptr, nullptr };

    // Monitoring: always, or automatically while armed and not playing back (stopped, or recording).
    const auto mode = static_cast<MonitorMode> (state.monitor.load());
    const auto active =
        mode == MonitorMode::on
        || (mode == MonitorMode::automatic && state.armed.load() && (! m_isPlaying || m_recording.load()));

    if (! active)
        return { nullptr, nullptr };

    const auto* left = m_inputs[static_cast<size_t> (channel)] + offset;
    const auto right = state.inputStereo.load() && channel + 1 < m_numInputs
                           ? m_inputs[static_cast<size_t> (channel) + 1] + offset
                           : left;
    return { left, right };
}

void AudioEngine::recordChunk (const Transport& transport, int offset, int numSamples) noexcept
{
    if (! m_recording.load() || ! m_isPlaying || numSamples <= 0)
        return;

    // Recording begins at the record position (after a count-in): skip the samples before it.
    const auto fromTick = m_recordFromBeats.load() * ticksPerBeat;

    if (transport.endTick <= fromTick)
        return;

    const auto skip = transport.startTick >= fromTick
                          ? 0
                          : std::clamp (static_cast<int> (std::ceil (transport.timeMap.tickToSample (fromTick)
                                                                     - static_cast<double> (transport.startSample))),
                                        0, numSamples);
    // A punch stops writing a little after the punch-out (docs/design/audio-features.md, AU11).
    const auto untilTick = m_recordUntilBeats.load() * ticksPerBeat;
    const auto stopAt = untilTick < 0.0 || transport.endTick <= untilTick
                            ? numSamples
                            : std::clamp (static_cast<int> (std::ceil (transport.timeMap.tickToSample (untilTick)
                                                                       - static_cast<double> (transport.startSample))),
                                          0, numSamples);
    const auto count = stopAt - skip;

    if (count <= 0)
        return;

    if (m_recordStartBeats.load() < 0.0)
        m_recordStartBeats.store (
            ticksToBeats (transport.timeMap.sampleToTick (static_cast<double> (transport.startSample + skip))));

    for (size_t i = 0; i < m_numRecordTargets.load(); ++i)
    {
        const auto& target = m_recordTargets[i];

        if (target.writer == nullptr || target.channel < 0 || target.channel >= m_numInputs)
            continue;

        const auto right = target.stereo && target.channel + 1 < m_numInputs ? target.channel + 1 : target.channel;
        const std::array<const float*, 2> channels { m_inputs[static_cast<size_t> (target.channel)] + offset + skip,
                                                     m_inputs[static_cast<size_t> (right)] + offset + skip };

        // The FIFO is sized generously; if the disk can't keep up, samples are dropped rather than waited for.
        if (target.writer->write (channels.data(), count))
            m_recordedFrames[i].fetch_add (count);
    }
}

// Takes over a new snapshot. The time map moves to the snapshot's tempo map (and sample rate), anchored at the
// current tick, so the playhead stays where it was whatever changed (docs/design/tempo-map.md, TM7). The previous
// snapshot may already be retired, so the current tick was read before acquiring this one.
void AudioEngine::adopt (const CompiledEngine& engine, double currentTick, bool hadEngine) noexcept
{
    m_timeMap = TimeMap (engine.tempoMap, engine.spec.sampleRate);
    m_timeMap.anchor (hadEngine ? currentTick : 0.0, m_samplePosition);
    m_latencySamplesForUi.store (engine.latencySamples);
    clearLoopSeam();
    m_current = &engine;
}

void AudioEngine::process (float* const* outputs, int numChannels, int numSamples) noexcept
{
    const ScopedNoAllocation noAllocation;

    // The device's thread doesn't flush denormals: without this, every node but the effects (which flush their own)
    // ran decaying tails into them on this thread. The pool's workers set the same mode.
    const juce::ScopedNoDenormals noDenormals;

    for (int ch = 0; ch < numChannels; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto currentTick = m_timeMap.sampleToTick (static_cast<double> (m_samplePosition));
    const auto hadEngine = m_current != nullptr;

    if (const auto* engine = m_engines.acquire(); engine != nullptr && engine != m_current)
        adopt (*engine, currentTick, hadEngine);

    if (m_stopRequested.exchange (false) && m_isPlaying)
    {
        // Return to where playback started (Cubase's "return to start position on stop").
        m_isPlaying = false;
        m_justStopped = true;
        m_timeMap.anchor (m_playStartTick, m_samplePosition);
        m_clickEnvelope = 0.0;
        clearLoopSeam();
    }

    if (const auto beats = m_seekRequest.exchange (-1.0); beats >= 0.0)
    {
        // Keep the sample counter running and re-anchor the tick mapping at the new position.
        m_timeMap.anchor (beats * static_cast<double> (ticksPerBeat), m_samplePosition);
        m_justJumped = true;
        m_clickEnvelope = 0.0;
        clearLoopSeam();
    }

    if (m_playRequested.exchange (false) && ! m_isPlaying)
    {
        m_isPlaying = true;
        m_justStarted = true;
        m_playStartTick = m_timeMap.sampleToTick (static_cast<double> (m_samplePosition));
    }

    m_playing.store (m_isPlaying);
    takeLiveRequests (numSamples);

    // Wrong sample rate (the device changed and the engine is being recompiled): stay silent until it's ready.
    if (m_current == nullptr || differs (m_current->spec.sampleRate, m_deviceSampleRate))
    {
        m_pendingLiveCount = 0;
        m_positionForUi.store (ticksToBeats (m_timeMap.sampleToTick (static_cast<double> (m_samplePosition))));
        return;
    }

    std::array<float*, maxOutputChannels> channels {};
    for (int ch = 0; ch < std::min (numChannels, maxOutputChannels); ++ch)
        channels[static_cast<size_t> (ch)] = outputs[ch];

    if (channels[0] == nullptr)
        return;

    if (channels[1] == nullptr)
        channels[1] = channels[0];

    renderBlock (*m_current, channels, numSamples);

    if (auto* capture = m_outputCapture.load(); capture != nullptr && m_isPlaying)
    {
        const std::array<const float*, 2> captured { channels[0], channels[1] };
        std::ignore = capture->write (captured.data(), numSamples);
    }

    if (! m_exporting.load())
        renderPreview (channels, numSamples);
    m_pendingLiveCount = 0;

    float peakLeft = 0.0f;
    float peakRight = 0.0f;

    for (int s = 0; s < numSamples; ++s)
    {
        peakLeft = std::max (peakLeft, std::abs (channels[0][s]));
        peakRight = std::max (peakRight, std::abs (channels[1][s]));
    }

    publishPeak (peakLeft, peakRight);
    m_positionForUi.store (ticksToBeats (m_timeMap.sampleToTick (static_cast<double> (m_samplePosition))));
}

void AudioEngine::renderBlock (const CompiledEngine& engine, std::span<float* const> channels, int numSamples) noexcept
{
    const auto maxBlock = engine.spec.maxBlockSize;
    const auto& loop = engine.loop;

    if (! m_isPlaying)
        clearLoopSeam();

    for (int offset = 0; offset < numSamples;)
    {
        auto count = std::min (maxBlock, numSamples - offset);
        const auto tickBefore = m_timeMap.sampleToTick (static_cast<double> (m_samplePosition));
        const auto looping =
            m_isPlaying && loop.enabled && ! m_exporting.load() && tickBefore < static_cast<double> (loop.end);

        const auto cutAt = [&] (Ticks tick)
        {
            const auto toTick = static_cast<int> (std::llround (m_timeMap.tickToSample (static_cast<double> (tick)))
                                                  - m_samplePosition);

            if (toTick > 0)
                count = std::min (count, toTick);
        };

        // Stop the chunk exactly at the loop end, so the wrap is sample-accurate (A9).
        if (looping)
            cutAt (loop.end);

        // And at tempo points, so ticks are linear in samples within a chunk (exactly at steps, closely along
        // ramps): nodes interpolate ticks across a chunk (docs/design/tempo-map.md, TM7).
        if (m_isPlaying)
        {
            const auto& points = engine.tempoMap.points();
            const auto next = std::ranges::upper_bound (points, tickBefore, {}, [] (const TempoPoint& p)
                                                        { return static_cast<double> (p.tick); });

            if (next != points.end())
                cutAt (next->tick);
        }

        m_deliverLive = offset == 0; // live events go out once, at the start of the block
        renderChunk (engine, channels, offset, count);

        declickLoopSeam (channels, offset, count);
        offset += count;

        if (looping
            && m_timeMap.sampleToTick (static_cast<double> (m_samplePosition)) >= static_cast<double> (loop.end) - 0.5)
        {
            // The next sample starts the loop again: remember where this iteration ended (declickLoopSeam).
            for (size_t ch = 0; ch < 2; ++ch)
                m_seamLast[ch] = channels[ch][offset - 1];

            m_seamPending = true;

            m_timeMap.anchor (static_cast<double> (loop.start), m_samplePosition);
            m_justLooped = true;

            if (m_recording.load() && m_recordStartBeats.load() >= 0.0)
                m_recordLoopWraps.fetch_add (1);
        }
    }
}

void AudioEngine::declickLoopSeam (std::span<float* const> channels, int offset, int numSamples) noexcept
{
    // A loop wrap joins the end of one iteration to the start of the next, and the step between the two samples
    // clicks. The step is measured at the first sample after the seam and faded out over loopDeclickSamples, so
    // nothing is repeated or dropped. The seam can fall at the end of a callback, so this carries across callbacks.
    if (m_seamPending && numSamples > 0)
    {
        m_seamPending = false;
        m_seamRemaining = loopDeclickSamples;

        for (size_t ch = 0; ch < 2; ++ch)
            m_seamStep[ch] = m_seamLast[ch] - channels[ch][offset];
    }

    const auto count = std::min (numSamples, m_seamRemaining);

    for (int i = 0; i < count; ++i)
    {
        const auto weight = static_cast<float> (m_seamRemaining - i) / static_cast<float> (loopDeclickSamples);

        for (size_t ch = 0; ch < 2; ++ch)
            channels[ch][offset + i] += m_seamStep[ch] * weight;
    }

    m_seamRemaining -= count;
}

void AudioEngine::clearLoopSeam() noexcept
{
    m_seamPending = false;
    m_seamRemaining = 0;
}

namespace
{
// The mix buffer of bus `index`: the groups and returns (docs/design/groups-and-sends.md, R6).
[[nodiscard]] std::array<float*, 2> busChannelsOf (const CompiledEngine& engine, int index) noexcept
{
    auto* first =
        engine.busMemory.get() + static_cast<size_t> (index) * 2 * static_cast<size_t> (engine.spec.maxBlockSize);
    return { first, first + engine.spec.maxBlockSize };
}

[[nodiscard]] bool soloed (const CompiledEngine& engine, int index) noexcept
{
    return engine.tracks[static_cast<size_t> (index)].state->solo.load();
}

[[nodiscard]] bool anySoloed (const CompiledEngine& engine) noexcept
{
    return std::ranges::any_of (engine.tracks, [] (const CompiledTrack& track) { return track.state->solo.load(); });
}

// While any track is soloed, only soloed tracks are heard, along with the groups they are in, the tracks inside
// soloed groups and the returns they send to (R5). Muted tracks still run (tails, plugin state); a muted group
// silences its members.
[[nodiscard]] bool isAudible (const CompiledEngine& engine, const CompiledTrack& track, bool anySolo) noexcept
{
    if (track.state->mute.load())
        return false;

    if (! anySolo)
        return true;

    // Returns are solo-safe, and a soloed return keeps the tracks that send to it audible (R5).
    const auto sendsToSoloed =
        std::ranges::any_of (track.sends, [&] (const CompiledSend& send)
                             { return ! send.key && send.target >= 0 && soloed (engine, send.target); });

    // An output track is heard with its instrument (docs/design/multi-output-instruments.md, MO6).
    const auto sourceSoloed = track.source >= 0 && soloed (engine, track.source);

    return track.isReturn || sourceSoloed || track.state->solo.load()
           || std::ranges::any_of (track.enclosing, [&] (int index) { return soloed (engine, index); })
           || std::ranges::any_of (track.inside, [&] (int index) { return soloed (engine, index); }) || sendsToSoloed;
}

// A port channel advanced by `sampleOffset`, for the second half of an early loop wrap. Null stays null.
template <typename Sample>
[[nodiscard]] Sample* shiftedChannel (Sample* data, int sampleOffset) noexcept
{
    return data != nullptr ? data + sampleOffset : nullptr;
}

template <typename Port>
[[nodiscard]] Port shiftedPort (const Port& port, int sampleOffset) noexcept
{
    return { shiftedChannel (port[0], sampleOffset), shiftedChannel (port[1], sampleOffset) };
}

// Port views for a track half. The returned views point into `scratch`, which must outlive them. Nothing if the
// node has more ports than `scratch` holds: running it with some of its ports missing would read past them.
template <typename Port>
[[nodiscard]] std::optional<std::span<const Port>> shiftedPorts (const std::vector<Port>& ports, int sampleOffset,
                                                                 std::span<Port> scratch) noexcept
{
    if (sampleOffset == 0)
        return std::span<const Port> (ports);

    jassert (ports.size() <= scratch.size());

    if (ports.size() > scratch.size())
        return std::nullopt;

    for (size_t i = 0; i < ports.size(); ++i)
        scratch[i] = shiftedPort (ports[i], sampleOffset);

    return std::span<const Port> (scratch.data(), ports.size());
}

// Silences a step's outputs for a track half it can't run (see shiftedPorts).
void silenceOutputs (const CompiledStep& step, int sampleOffset, int numSamples) noexcept
{
    for (const auto& output : step.audioOutputs)
        for (auto* channel : output)
            if (channel != nullptr)
                juce::FloatVectorOperations::clear (channel + sampleOffset, numSamples);
}

// A parameter that differs from what modulation last wrote was changed by hand or by automation.
constexpr float modulationChangeTolerance = 1.0e-5f;

// Hands a removed slot's plugin parameter back at its base (CompiledModulation::restore).
void restoreModulationBase (const CompiledModulation& modulation) noexcept
{
    if (modulation.plugin == nullptr || modulation.pluginParam < 0 || modulation.state->restored.exchange (true))
        return;

    // Modulation apart from the value: nothing to give back but the amount (docs/design/clap-hosting.md, C9).
    if (modulation.plugin->isModulatable (modulation.pluginParam))
    {
        modulation.plugin->modulateParameter (modulation.pluginParam, 0.0f);
        return;
    }

    const auto last = modulation.state->lastWritten.load();
    const auto current = modulation.plugin->parameterValue (modulation.pluginParam);

    if (std::isfinite (last) && std::abs (current - last) <= modulationChangeTolerance)
        modulation.plugin->setModulatedParameter (modulation.pluginParam, modulation.state->base.load());
}

// A slot's offset at one sample, in units of its target's range: its source there, the target's polarity applied,
// times the depth. Returns nothing when the slot has no source.
[[nodiscard]] std::optional<float> modulationAmount (const CompiledModulation& modulation,
                                                     std::span<const AudioInput> inputs, int sample) noexcept
{
    if (modulation.port < 0 || static_cast<size_t> (modulation.port) >= inputs.size())
        return std::nullopt;

    const auto* source = inputs[static_cast<size_t> (modulation.port)][0];

    if (source == nullptr)
        return std::nullopt;

    const auto sourceValue = source[sample];

    // Plugin parameters are always bipolar; a built-in one may accept only one side, at a positive depth.
    const auto depth = modulation.currentDepth();

    if (modulation.node == nullptr)
        return sourceValue * depth;

    switch (modulation.info.modulationPolarity)
    {
        case ModulationPolarity::positive:
            return std::max (sourceValue, 0.0f) * std::abs (depth);
        case ModulationPolarity::negative:
            return std::min (sourceValue, 0.0f) * std::abs (depth);
        case ModulationPolarity::bipolar:
            break;
    }

    return sourceValue * depth;
}

// The parameter value modulation is applied around: the last base, unless the parameter moved since modulation
// last wrote it (a manual or automated change), which becomes the new base.
[[nodiscard]] float modulationBase (ModulationState& state, float current) noexcept
{
    const auto last = state.lastWritten.load();

    if (! std::isfinite (last) || std::abs (current - last) > modulationChangeTolerance)
        state.base.store (current);

    return state.base.load();
}

// A built-in parameter's value for a base and a summed amount, in its modulation scale (MC1 to MC3).
[[nodiscard]] float modulatedValue (const ParamInfo& info, float base, float amount) noexcept
{
    return modulatedParamValue (info, base, amount);
}

// Writes one target's modulated value: its base plus the summed amount of every slot that targets it. Returns the
// base.
float writeModulation (const CompiledModulation& modulation, float amount) noexcept
{
    if (modulation.plugin != nullptr)
    {
        if (modulation.pluginParam < 0)
            return 0.0f;

        const auto base =
            modulationBase (*modulation.state, modulation.plugin->parameterValue (modulation.pluginParam));
        const auto value = std::clamp (base + amount, 0.0f, 1.0f);
        modulation.plugin->setModulatedParameter (modulation.pluginParam, value);
        modulation.state->lastWritten.store (value);
        return base;
    }

    if (modulation.node == nullptr)
        return 0.0f;

    const auto base = modulationBase (*modulation.state, modulation.node->params().get (modulation.param));
    const auto value = modulatedValue (modulation.info, base, amount);
    modulation.node->params().set (modulation.param, value);
    modulation.state->lastWritten.store (value);
    return base;
}

// Parameter links (docs/design/parameter-linking.md, L6 to L10).

// A link's source value now: built-in in its units, a plugin's normalised; nothing while a cross-track source hasn't
// published yet.
[[nodiscard]] std::optional<float> linkSourceValue (const CompiledLink& link) noexcept
{
    if (link.crossTrack)
    {
        const auto published = link.state->published.load();
        return std::isfinite (published) ? std::optional (published) : std::nullopt;
    }

    if (link.sourceNode != nullptr)
        return link.sourceNode->params().get (link.sourceParam);

    if (link.sourcePlugin != nullptr && link.sourcePluginParam >= 0)
        return link.sourcePlugin->parameterValue (link.sourcePluginParam);

    return std::nullopt;
}

// The source's value delaySamples ago on the link's own clock (L8): the newest value at least that old, or the
// oldest there is.
[[nodiscard]] float delayedLinkValue (LinkState& state, float value, int delaySamples, int numSamples) noexcept
{
    state.history[state.next] = value;
    state.times[state.next] = state.clock;
    state.next = (state.next + 1) % LinkState::historySize;
    state.count = std::min (state.count + 1, LinkState::historySize);

    const auto wanted = state.clock - delaySamples;
    state.clock += numSamples;
    auto result = value;
    auto best = std::numeric_limits<juce::int64>::min();
    auto oldest = std::numeric_limits<juce::int64>::max();
    auto oldestValue = value;

    for (size_t i = 0; i < state.count; ++i)
    {
        const auto time = state.times[i];

        if (time <= wanted && time > best)
        {
            best = time;
            result = state.history[i];
        }

        if (time < oldest)
        {
            oldest = time;
            oldestValue = state.history[i];
        }
    }

    return best == std::numeric_limits<juce::int64>::min() ? oldestValue : result;
}

// Writes each link's mapped source value into its target, before the target's modulation adds to it (L6).
void applyLinks (std::span<const CompiledLink> links, int numSamples) noexcept
{
    for (const auto& link : links)
    {
        auto value = linkSourceValue (link);

        if (! value.has_value())
            continue;

        if (link.delaySamples > 0)
            value = delayedLinkValue (*link.state, *value, link.delaySamples, numSamples);

        const auto* sourceInfo = link.sourceNode != nullptr ? &link.sourceInfo : nullptr;

        // A cross-track source publishes in the same terms (built-in units or normalised).
        if (link.crossTrack && link.sourcePlugin != nullptr)
            sourceInfo = nullptr;

        if (link.targetNode != nullptr)
        {
            const auto mapped = mapLinkValue (link.mapping, sourceInfo, &link.targetInfo, *value);
            link.targetNode->params().set (link.targetParam, mapped);

            for (auto* voice : link.targetVoices)
                voice->params().set (link.targetParam, mapped);
        }
        else if (link.targetPlugin != nullptr && link.targetPluginParam >= 0)
        {
            const auto mapped = mapLinkValue (link.mapping, sourceInfo, nullptr, *value);

            // A plugin hears a value only when it changes.
            if (std::abs (link.targetPlugin->parameterValue (link.targetPluginParam) - mapped) <= 1.0e-6f)
                continue;

            if (link.targetPlugin->takesParameterEvents())
                link.targetPlugin->automateParameter (link.targetPluginParam, mapped, 0);
            else
                link.targetPlugin->setAutomatedParameter (link.targetPluginParam, mapped);
        }
    }
}

// Publishes the sources of links that cross tracks, once every track has run (L10).
void publishLinks (const CompiledEngine& engine) noexcept
{
    for (const auto& source : engine.linkSources)
    {
        if (source.node != nullptr)
            source.state->published.store (source.node->params().get (source.param));
        else if (source.plugin != nullptr && source.pluginParam >= 0)
            source.state->published.store (source.plugin->parameterValue (source.pluginParam));
    }
}

[[nodiscard]] bool sameTarget (const CompiledModulation& a, const CompiledModulation& b) noexcept
{
    return a.plugin == b.plugin && a.pluginParam == b.pluginParam && a.node == b.node && a.param == b.param;
}

// One modulated parameter in a block: the slots aimed at it and their summed amount at the middle of the block.
struct ModulationTarget
{
    std::array<const CompiledModulation*, maxModulationInputs> slots {};
    size_t slotCount = 0;
    float amount = 0.0f;
};

// Fills a built-in target's value at every sample (ModulatedParam), around the base its block-rate write used.
void fillModulatedValues (const ModulationTarget& target, float base, std::span<const AudioInput> inputs, float* values,
                          int numSamples) noexcept
{
    const auto slots = std::span (target.slots).first (target.slotCount);
    const auto& info = slots.front()->info;

    for (int s = 0; s < numSamples; ++s)
    {
        float amount = 0.0f;

        for (const auto* slot : slots)
            amount += modulationAmount (*slot, inputs, s).value_or (0.0f);

        values[s] = modulatedValue (info, base, amount);
    }
}

// Applies a step's modulation slots (docs/design/modulation.md, M5). Every parameter is written once per block,
// like automation, from the source at the middle of the block, so the result does not depend on whether the block
// begins just before or after a fast LFO's zero crossing; plugins and most built-in nodes follow only that.
// Built-in targets also get their value at every sample in `modulated` (one block of `blockSize` each in the step's
// modulationValues), for nodes that follow modulation faster. Returns how many were filled.
size_t applyModulations (const CompiledStep& step, std::span<const AudioInput> inputs, int numSamples, size_t blockSize,
                         std::span<ModulatedParam, maxModulationInputs> modulated) noexcept
{
    if (numSamples <= 0)
        return 0;

    // Several slots may target the same parameter. Sum them first so each parameter is written once per block
    // around one base; otherwise the second slot would mistake the first slot's write for a manual change.
    std::array<ModulationTarget, maxModulationInputs> targets {};
    size_t count = 0;

    for (const auto& modulation : step.modulations)
    {
        if (modulation.restore)
        {
            restoreModulationBase (modulation);
            continue;
        }

        // The plugin node works these out per note instead.
        if (modulation.noteSource >= 0 || ! modulation.noteNodes.empty())
            continue;

        // A slot at no depth still runs: it writes the base, so turning the depth down puts the parameter back.
        const auto amount = modulationAmount (modulation, inputs, numSamples / 2);

        if (! amount.has_value())
            continue;

        const auto used = std::span (targets).first (count);
        auto index =
            static_cast<size_t> (std::ranges::find_if (used, [&] (const ModulationTarget& item)
                                                       { return sameTarget (*item.slots.front(), modulation); })
                                 - used.begin());

        if (index == count)
        {
            if (count == targets.size())
                continue;

            ++count;
        }

        auto& target = targets[index];

        if (target.slotCount < target.slots.size())
            target.slots[target.slotCount++] = &modulation;

        target.amount += *amount;
    }

    size_t filled = 0;
    const auto perSample = step.modulationValues != nullptr && std::cmp_less_equal (numSamples, blockSize);

    for (const auto& target : std::span (targets).first (count))
    {
        // A plugin that keeps modulation apart from the value takes it with sample offsets, so it needn't step by block
        // (docs/design/clap-hosting.md, C9).
        if (const auto& first = *target.slots.front();
            first.plugin != nullptr && first.pluginParam >= 0 && first.plugin->isModulatable (first.pluginParam))
        {
            for (int s = 0; s < numSamples; s += modulationGridSamples)
            {
                float amount = 0.0f;

                for (const auto* slot : std::span (target.slots).first (target.slotCount))
                    amount += modulationAmount (*slot, inputs, s).value_or (0.0f);

                first.plugin->modulateParameter (first.pluginParam, amount, static_cast<uint32_t> (s));
            }

            continue;
        }

        const auto base = writeModulation (*target.slots.front(), target.amount);

        if (perSample && target.slots.front()->node != nullptr)
        {
            auto* values = step.modulationValues.get() + (filled * blockSize);
            fillModulatedValues (target, base, inputs, values, numSamples);
            modulated[filled++] = { target.slots.front()->param, values };
        }
    }

    return filled;
}

// A step's slots that its plugin works out per note (docs/design/clap-hosting.md, C10), each with what the parameter's
// other slots add at the middle of the block.
[[nodiscard]] size_t collectNoteModulations (const CompiledStep& step, std::span<const AudioInput> inputs,
                                             int numSamples,
                                             std::span<NoteModulation, maxModulationInputs> out) noexcept
{
    size_t count = 0;

    for (const auto& modulation : step.modulations)
    {
        if ((modulation.noteSource < 0 && modulation.noteNodes.empty()) || modulation.restore || count == out.size())
            continue;

        float shared = 0.0f;

        for (const auto& other : step.modulations)
            if (&other != &modulation && ! other.restore && other.noteSource < 0 && other.noteNodes.empty()
                && sameTarget (other, modulation))
                shared += modulationAmount (other, inputs, numSamples / 2).value_or (0.0f);

        out[count++] = { modulation.pluginParam, modulation.noteSource, modulation.currentDepth(), shared,
                         modulation.noteNodes,   modulation.notePort };
    }

    return count;
}

// Output protection (docs/design/output-protection.md, S1): whether a track's output has NaN or infinity in this chunk.
// If so, its nodes are reset, so one whose state went invalid can recover, and the block is counted for the UI.
[[nodiscard]] bool silenceIfInvalid (const CompiledTrack& track, int numSamples) noexcept
{
    const auto invalid = std::ranges::any_of (track.output,
                                              [numSamples] (const float* channel)
                                              {
                                                  return channel != nullptr
                                                         && ! std::all_of (channel, channel + numSamples,
                                                                           [] (float x) { return std::isfinite (x); });
                                              });

    if (! invalid)
        return false;

    for (const auto& step : track.steps)
        if (step.node != nullptr)
            step.node->reset();

    if (track.state != nullptr)
        track.state->invalidBlocks.fetch_add (1);

    return true;
}

// Sums a compiled mix step's sources into its output, shifted for a loop-wrap half. Into a mono output, each source
// counts as (L + R) / 2, which for a mono source (the same channel twice) is the source itself
// (docs/design/mono-ports.md, MP2).
void mixStep (const CompiledStep& step, int sampleOffset, int numSamples) noexcept
{
    const auto& out = step.audioOutputs[0];

    if (step.channels == 1)
    {
        auto* destination = shiftedChannel (out[0], sampleOffset);
        juce::FloatVectorOperations::clear (destination, numSamples);

        for (const auto& source : step.mixSources)
            for (const auto* channel : source)
                juce::FloatVectorOperations::addWithMultiply (destination, shiftedChannel (channel, sampleOffset), 0.5f,
                                                              numSamples);

        return;
    }

    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto* destination = shiftedChannel (out[ch], sampleOffset);
        juce::FloatVectorOperations::copy (destination, shiftedChannel (step.mixSources[0][ch], sampleOffset),
                                           numSamples);

        for (size_t i = 1; i < step.mixSources.size(); ++i)
            juce::FloatVectorOperations::add (destination, shiftedChannel (step.mixSources[i][ch], sampleOffset),
                                              numSamples);
    }
}
} // namespace

namespace
{
// A node whose event source changed hears sourceChanged before its events (docs/design/midi-effects.md, ME6), from a
// copy of them.
void processAfterSourceChange (Node& node, const ProcessContext& context) noexcept
{
    EventBuffer events;
    std::ignore = events.add ({ 0, NoteEventType::sourceChanged, 0, nullptr });

    if (context.eventsIn != nullptr)
        for (const auto& event : context.eventsIn->events())
            std::ignore = events.add (event);

    auto changed = context;
    changed.eventsIn = &events;
    node.process (changed);
}

void runStep (const CompiledStep& step, EventBuffer* eventBuffers, const EventBuffer* eventsIn,
              const ProcessContext& base, int sampleOffset, int maxBlockSize) noexcept
{
    // A track rendered in two halves (an early loop wrap) writes its second half into the same compiled arenas,
    // offset by `sampleOffset`. Node port views are only pointers, so shift their channels for that half.
    // Enough for any built-in node and a Faust script's channels plus the modulation pool.
    constexpr size_t maxShiftedPorts = 32;
    std::array<AudioInput, maxShiftedPorts> shiftedInputs {};
    std::array<AudioOutput, maxShiftedPorts> shiftedOutputs {};
    const auto numSamples = base.numSamples;

    if (step.node == nullptr)
    {
        mixStep (step, sampleOffset, numSamples);
        return;
    }

    const auto shiftedIn = shiftedPorts<AudioInput> (step.audioInputs, sampleOffset, shiftedInputs);
    const auto shiftedOut = shiftedPorts<AudioOutput> (step.audioOutputs, sampleOffset, shiftedOutputs);

    if (! shiftedIn.has_value() || ! shiftedOut.has_value())
    {
        silenceOutputs (step, sampleOffset, numSamples);
        return;
    }

    const auto inputs = *shiftedIn;
    const auto outputs = *shiftedOut;

    applyLinks (step.links, numSamples);

    std::array<ModulatedParam, maxModulationInputs> modulated {};
    const auto modulatedCount =
        applyModulations (step, inputs, numSamples, static_cast<size_t> (maxBlockSize), modulated);

    EventBuffer* eventsOut = nullptr;

    if (step.eventOutput >= 0)
    {
        eventsOut = &eventBuffers[static_cast<size_t> (step.eventOutput)];
        eventsOut->clear();
    }

    auto context = base;
    context.audioInputs = inputs;
    context.audioOutputs = outputs;
    context.eventsIn = eventsIn;

    if (context.eventsIn == nullptr && step.eventInput >= 0)
        context.eventsIn = &eventBuffers[static_cast<size_t> (step.eventInput)];
    context.eventsOut = eventsOut;
    context.input = shiftedPort (base.input, sampleOffset);
    context.modulatedParams = std::span (modulated).first (modulatedCount);
    std::array<NoteModulation, maxModulationInputs> noteModulations {};
    context.noteModulations =
        std::span (noteModulations).first (collectNoteModulations (step, inputs, numSamples, noteModulations));
    context.voice = step.voice.get();
    context.samples = step.samples.get();
    context.wiredOutputs = step.wiredOutputs;
    context.loopPart = step.loopPart;
    context.channels = step.channels;

    if (step.takesEvents && step.node->takeEventSource (step.eventSource))
        processAfterSourceChange (*step.node, context);
    else
        step.node->process (context);

    // The device panel's analysers (docs/design/phase11-faust-panels.md, P2).
    if (auto* tap = step.node->activeTap(); tap != nullptr && ! step.audioOutputs.empty())
    {
        auto output = step.audioOutputs[0];

        if ((step.monoOutputs & 1u) != 0)
            output[1] = output[0];

        tap->push (shiftedPort (output, sampleOffset), numSamples);
    }
}

// A loop in the graph (docs/design/feedback-routing.md, R2): its steps again and again over small pieces of the block, each piece
// in its place in the same buffers. Each Feedback node reads at the start of a piece and writes at the end, and what it reads
// is at least a piece old, so how the block divides up doesn't change the sound.
void runRegion (std::span<const CompiledStep> steps, EventBuffer* eventBuffers, const ProcessContext& base,
                int sampleOffset, int maxBlockSize) noexcept
{
    const auto hasEvents = std::ranges::any_of (steps, [] (const CompiledStep& step) { return step.eventInput >= 0; });
    EventBuffer pieceEvents;

    for (int done = 0; done < base.numSamples; done += loopChunkSamples)
    {
        const auto length = std::min (loopChunkSamples, base.numSamples - done);
        auto transport = *base.transport;
        const auto tickAt = [&] (int sample)
        {
            return base.transport->startTick
                   + (base.transport->endTick - base.transport->startTick) * static_cast<double> (sample)
                         / base.numSamples;
        };

        transport.startTick = tickAt (done);
        transport.endTick = tickAt (done + length);
        transport.startSample += done;

        auto piece = base;
        piece.numSamples = length;
        piece.transport = &transport;

        if (done > 0)
        {
            transport.started = transport.stopped = transport.jumped = transport.looped = false;
            piece.liveEvents = {};
        }

        for (const auto& step : steps)
        {
            const EventBuffer* eventsIn = nullptr;

            if (hasEvents && step.eventInput >= 0)
            {
                pieceEvents.clear();

                for (auto event : eventBuffers[static_cast<size_t> (step.eventInput)].events())
                {
                    if (event.sampleOffset >= done && event.sampleOffset < done + length)
                    {
                        event.sampleOffset -= done;
                        std::ignore = pieceEvents.add (event);
                    }
                }

                eventsIn = &pieceEvents;
            }

            runStep (step, eventBuffers, eventsIn, piece, sampleOffset + done, maxBlockSize);
        }
    }
}
} // namespace

void runSteps (std::span<const CompiledStep> steps, EventBuffer* eventBuffers, const ProcessContext& base,
               int sampleOffset, int maxBlockSize) noexcept
{
    for (size_t index = 0; index < steps.size(); ++index)
    {
        const auto regionSteps = steps[index].regionSteps;

        if (regionSteps > 0)
        {
            runRegion (steps.subspan (index, regionSteps), eventBuffers, base, sampleOffset, maxBlockSize);
            index += regionSteps - 1;
        }
        else
        {
            runStep (steps[index], eventBuffers, nullptr, base, sampleOffset, maxBlockSize);
        }
    }
}

void AudioEngine::renderChunk (const CompiledEngine& engine, std::span<float* const> outputs, int offset,
                               int numSamples) noexcept
{
    Transport transport;
    transport.playing = m_isPlaying;
    transport.started = m_justStarted;
    transport.stopped = m_justStopped;
    transport.jumped = m_justJumped;
    transport.looped = m_justLooped;
    transport.startSample = m_samplePosition;
    transport.timeMap = m_timeMap;
    transport.startTick = m_timeMap.sampleToTick (static_cast<double> (m_samplePosition));
    transport.endTick = m_isPlaying ? m_timeMap.sampleToTick (static_cast<double> (m_samplePosition + numSamples))
                                    : transport.startTick;

    m_justStarted = false;
    m_justStopped = false;
    m_justJumped = false;
    m_justLooped = false;

    const std::array<float*, 2> device { outputs[0] + offset, outputs[1] + offset };

    const auto loopActive =
        m_isPlaying && engine.loop.enabled && ! m_exporting.load()
        && m_timeMap.sampleToTick (static_cast<double> (m_samplePosition)) < static_cast<double> (engine.loop.end);

    // With a master track, the tracks are summed into the mix buffer, which feeds the master chain. A stem
    // export renders one track straight to the output.
    const auto stem = m_exporting.load() && m_numExportTracks.load() > 0;
    auto bus = device;

    // A freeze render's taps: the inputs of the track's Output to track nodes, each to a pair of output channels.
    std::array<std::array<float*, 2>, maxExportTaps> tapChannels {};
    size_t numTaps = 0;

    if (stem)
        for (size_t k = 0; k < m_numExportTaps.load() && 3 + 2 * k < outputs.size(); ++k, ++numTaps)
        {
            if (outputs[2 + 2 * k] == nullptr || outputs[3 + 2 * k] == nullptr)
                break;

            tapChannels[k] = { outputs[2 + 2 * k] + offset, outputs[3 + 2 * k] + offset };
        }

    if (engine.master.has_value() && ! stem)
    {
        bus = { engine.mixMemory.get(), engine.mixMemory.get() + engine.spec.maxBlockSize };
        juce::FloatVectorOperations::clear (bus[0], numSamples);
        juce::FloatVectorOperations::clear (bus[1], numSamples);
    }

    // Group buses (docs/design/groups-and-sends.md, R6): members are summed in before their group runs.
    for (int i = 0; i < engine.numBuses; ++i)
        for (auto* channel : busChannelsOf (engine, i))
            juce::FloatVectorOperations::clear (channel, numSamples);

    // While any track is soloed, only soloed tracks are heard, along with the groups they are in and the tracks inside
    // soloed groups (R5). Muted tracks still run (tails, plugin state); a muted group silences its members.
    const auto anySolo = anySoloed (engine);

    // Tracks run ahead by the engine latency, so their (delay-aligned) output lines up with the playhead after
    // the master track (renderTrackSteps).
    //
    // On the pool, each track renders once the tracks that fill its buses have, and every bus and the master mix sum
    // their inputs in track order, so the result is identical to rendering one track at a time
    // (docs/design/phase2-engine.md, 5.6).
    const auto parallel =
        m_parallelTracks.load() && m_workerPool != nullptr && m_workerPool->workers() > 0 && engine.tracks.size() > 1;
    const auto taps = std::span<const std::array<float*, 2>> (tapChannels.data(), numTaps);

    if (parallel)
    {
        // Every track renders on the pool once the tracks it reads have; each bus is summed by its reader's thread
        // in track order. What goes to the master mix is summed here, in track order too.
        const WorkerPool::Job job { &engine, &transport, offset, numSamples, loopActive, stem, anySolo };
        m_workerPool->render (job);

        for (size_t i = 0; i < engine.tracks.size(); ++i)
            mixRenderedTrack (engine, engine.tracks[i], engine.trackMix[i], bus, taps, stem, anySolo, numSamples,
                              mixToMain);
    }
    else
    {
        for (size_t i = 0; i < engine.tracks.size(); ++i)
        {
            const auto& track = engine.tracks[i];
            renderTrackSteps (engine, track, i, transport, offset, numSamples, loopActive, m_trackLive);
            mixRenderedTrack (engine, track, engine.trackMix[i], bus, taps, stem, anySolo, numSamples);
        }
    }

    if (engine.master.has_value() && ! stem)
    {
        const auto& master = *engine.master;
        // The master track runs ahead by its own latency, wrapping early at the loop end like the tracks.
        const auto masterBlock = aheadOf (transport, m_timeMap, m_samplePosition, numSamples,
                                          engine.masterLatencySamples, loopActive, engine.loop);
        const auto masterMix = automate (master, masterBlock.first);
        applyLinks (master.links, numSamples);

        if (masterBlock.hasSecond)
        {
            runTrack (master, engine, masterBlock.first, { bus[0], bus[1] }, masterBlock.split, 0, m_trackLive);
            runTrack (master, engine, masterBlock.second, { bus[0], bus[1] }, numSamples - masterBlock.split,
                      masterBlock.split, m_trackLive);
        }
        else
        {
            runTrack (master, engine, masterBlock.first, { bus[0], bus[1] }, numSamples, 0, m_trackLive);
        }

        if (! silenceIfInvalid (master, numSamples))
            mixTrack (master, masterMix, device, numSamples);
    }

    publishLinks (engine);

    if (engine.metronome && m_isPlaying && ! m_exporting.load())
        renderMetronome (engine, transport, outputs, offset, numSamples);

    recordChunk (transport, offset, numSamples);

    if (m_isPlaying)
        m_samplePosition += numSamples;
}

void AudioEngine::renderTrackSteps (const CompiledEngine& engine, const CompiledTrack& track, size_t index,
                                    const Transport& transport, int offset, int numSamples, bool loopActive,
                                    std::span<NoteEvent> liveScratch, Parallel* parallel) noexcept
{
    const auto block =
        aheadOf (transport, m_timeMap, m_samplePosition, numSamples, track.aheadSamples, loopActive, engine.loop);
    engine.trackMix[index] = automate (track, block.first);
    applyLinks (track.links, numSamples);

    const auto input = [&]() -> std::array<const float*, 2>
    {
        if (track.bus < 0)
            return inputFor (*track.state, offset);

        const auto channels = busChannelsOf (engine, track.bus);
        return { channels[0], channels[1] };
    }();

    if (block.hasSecond)
    {
        runTrack (track, engine, block.first, input, block.split, 0, liveScratch, parallel);
        runTrack (track, engine, block.second, input, numSamples - block.split, block.split, liveScratch, parallel);
        meterInput (*track.state, offset, block.split);
        meterInput (*track.state, offset + block.split, numSamples - block.split);
    }
    else
    {
        runTrack (track, engine, block.first, input, numSamples, 0, liveScratch, parallel);
        meterInput (*track.state, offset, numSamples);
    }

    engine.trackMix[index].invalid = silenceIfInvalid (track, numSamples);
}

void AudioEngine::mixRenderedTrack (const CompiledEngine& engine, const CompiledTrack& track, const TrackMix& mix,
                                    std::array<float*, 2> bus, std::span<const std::array<float*, 2>> taps, bool stems,
                                    bool anySolo, int numSamples, int into) noexcept
{
    // Invalid audio plays nothing, not even into sends (docs/design/output-protection.md, S1).
    if (mix.invalid)
        return;

    const auto busChannels = [&] (int index) { return busChannelsOf (engine, index); };

    // Each destination is mixed by exactly one of the calls a track gets when its buses are mixed by their readers.
    const auto toMain = into == mixEverything || into == mixToMain;
    const auto wanted = [&] (int index)
    {
        return into == mixEverything
               || (into >= 0 && index >= 0 && engine.busReaders[static_cast<size_t> (index)] == into);
    };

    // Output to track (docs/design/multi-output-instruments.md, MO6, MO7): whatever the track's fader and solo, but
    // muting the track mutes its outputs. A stem export feeds them, so each output track is a stem of its own.
    if (stems || ! track.state->mute.load())
        for (const auto& output : track.busOutputs)
            if (wanted (output.bus))
                addRamped (busChannels (output.bus), output.source, { 1.0f, 1.0f }, { 1.0f, 1.0f }, 2, numSamples);

    // A stem export renders its tracks (with what is inside them, for groups) straight to the output: a stem whose
    // group is a stem too goes into the group as usual.
    const auto inside =
        stems
        && std::ranges::any_of (track.enclosing, [&] (int index)
                                { return isExportTrack (engine.tracks[static_cast<size_t> (index)].id); });
    const auto root = stems && ! inside && isExportTrack (track.id);
    const auto audible = stems ? root || inside : isAudible (engine, track, anySolo);

    if (audible)
    {
        // Track renders for freeze and bounce take the track before its fader and pan (audio-features.md, AU14). A
        // header plays nothing itself: its Main is an output track (multi-output-instruments.md, MO12).
        const auto toGroup = track.outputBus >= 0 && ! root;

        if (root && m_exportPreFader.load())
        {
            if (toMain)
                addRamped (bus, track.output, { 1.0f, 1.0f }, { 1.0f, 1.0f }, 2, numSamples);
        }
        else if (! track.header && (toGroup ? wanted (track.outputBus) : toMain))
        {
            mixTrack (track, mix, toGroup ? busChannels (track.outputBus) : bus, numSamples);
        }

        // A freeze renders what each Output to track sends to its output track too (AU15).
        if (root && toMain)
            for (size_t k = 0; k < taps.size(); ++k)
                for (const auto& output : track.busOutputs)
                    if (output.node == m_exportTaps[k].load())
                        addRamped (taps[k], output.source, { 1.0f, 1.0f }, { 1.0f, 1.0f }, 2, numSamples);

        // A stem export renders its tracks dry.
        if (! stems && ! track.header)
            mixSends (track, mix, busChannels, wanted, numSamples, false);
    }

    // Keys make no sound: they run whatever the track's mute, solo or a stem export (K2).
    mixSends (track, mix, busChannels, wanted, numSamples, true);
}

bool AudioEngine::isExportTrack (ObjectId id) const noexcept
{
    const auto count = m_numExportTracks.load();

    for (size_t i = 0; i < count; ++i)
        if (m_exportTracks[i].load() == id)
            return true;

    return false;
}

void AudioEngine::meterInput (TrackState& state, int offset, int numSamples) const noexcept
{
    const auto channel = state.inputChannel.load();

    if (channel < 0 || channel >= m_numInputs)
        return;

    const auto last = state.inputStereo.load() ? std::min (channel + 1, m_numInputs - 1) : channel;
    float peak = 0.0f;

    for (auto ch = channel; ch <= last; ++ch)
        if (const auto* data = m_inputs[static_cast<size_t> (ch)]; data != nullptr)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (data + offset, numSamples);
            peak = std::max ({ peak, std::abs (range.getStart()), std::abs (range.getEnd()) });
        }

    raisePeak (state.inputPeak, peak);
}

void AudioEngine::runTrack (const CompiledTrack& track, const CompiledEngine& engine, const Transport& transport,
                            std::array<const float*, 2> input, int numSamples, int sampleOffset,
                            std::span<NoteEvent> liveScratch, Parallel* parallel) noexcept
{
    ProcessContext base;
    base.numSamples = numSamples;
    base.transport = &transport;
    base.sequence = track.sequence.get();
    base.liveEvents =
        sampleOffset == 0 ? liveEventsFor (track.id, numSamples, liveScratch) : std::span<const NoteEvent> {};
    base.audio = track.audio.get();
    base.input = input;
    base.parallel = parallel;

    runSteps (track.steps, engine.eventBuffers.get(), base, sampleOffset, engine.spec.maxBlockSize);
}

// Clicks on each beat of the time signature, the bar's first accented. Compound meters (6/8, 9/8, 12/8) click on
// dotted quarters, with the eighths between quieter (docs/design/tempo-map.md, TM9).
void AudioEngine::renderMetronome (const CompiledEngine& engine, const Transport& transport,
                                   std::span<float* const> outputs, int offset, int numSamples) noexcept
{
    const auto sampleRate = engine.spec.sampleRate;
    const auto ticksPerSample = (transport.endTick - transport.startTick) / numSamples;
    const auto decay = std::exp (-1.0 / (clickDecaySeconds * sampleRate));
    const auto stereo = outputs[0] != outputs[1];

    for (int s = 0; s < numSamples; ++s)
    {
        const auto tick = transport.startTick + ticksPerSample * s;
        const auto position = engine.tempoMap.barAt (tick);

        // The first sample at or after a beat line.
        if (ticksPerSample > 0.0 && position.tickInBeat < ticksPerSample)
        {
            const auto& signature = engine.tempoMap.signatureAt (tick);
            const auto compound = signature.denominator == 8 && signature.numerator % 3 == 0 && signature.numerator > 3;
            m_clickFrequency = position.beat == 0 ? downbeatFrequencyHz : beatFrequencyHz;
            m_clickLevel = compound && position.beat % 3 != 0 ? subdivisionLevel : 1.0;
            m_clickPhase = 0.0;
            m_clickEnvelope = 1.0;
        }

        if (m_clickEnvelope > clickSilenceThreshold)
        {
            const auto sample =
                static_cast<float> (clickGain * m_clickLevel * m_clickEnvelope * std::sin (m_clickPhase));
            outputs[0][offset + s] += sample;

            if (stereo)
                outputs[1][offset + s] += sample;

            m_clickPhase += juce::MathConstants<double>::twoPi * m_clickFrequency / sampleRate;
            m_clickEnvelope *= decay;
        }
    }
}

void AudioEngine::publishPeak (float left, float right) noexcept
{
    // Peak-hold until the UI polls: only ever raise the stored value.
    raisePeak (m_peakLeft, left);
    raisePeak (m_peakRight, right);
}

} // namespace dawg
