#include "TestUtils.h"
#include "dawg/AudioEngine.h"
#include "dawg/AudioSource.h"
#include "dawg/CompiledEngine.h"
#include "dawg/GraphCompiler.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <typeinfo>
#include <vector>

// What a saved project costs to render. Hidden: set DAWG_BENCH_PROJECT to a .dawg file and run with [.project-bench].
// It loads the project, plays its loop (or from the start) and reports:
//   1. the whole engine, block by block, with the track pool on and off: the share of the block's deadline;
//   2. each track's steps run one at a time on this thread (bus inputs read silence), summed over the run.
// DAWG_BENCH_RATE (default 48000), DAWG_BENCH_BLOCK (default 480) and DAWG_BENCH_SECONDS (default 20) set the run.

using namespace dawg;

namespace
{
[[nodiscard]] double envOr (const char* name, double fallback)
{
    const auto value = juce::SystemStats::getEnvironmentVariable (name, {});
    return value.isEmpty() ? fallback : value.getDoubleValue();
}

[[nodiscard]] double ticksToMs (juce::int64 ticks)
{
    return 1000.0 * static_cast<double> (ticks) / static_cast<double> (juce::Time::getHighResolutionTicksPerSecond());
}

[[nodiscard]] std::string nameOf (const CompiledStep& step)
{
    if (step.node == nullptr)
        return "(mix)";

    std::string name = typeid (*step.node).name();

    for (const auto* prefix : { "class ", "struct ", "dawg::", "`anonymous namespace'::" })
        for (auto at = name.find (prefix); at != std::string::npos; at = name.find (prefix))
            name.erase (at, std::char_traits<char>::length (prefix));

    return name;
}

struct Loaded
{
    Project project;
    juce::File folder;
};

[[nodiscard]] Loaded load()
{
    const auto path = juce::SystemStats::getEnvironmentVariable ("DAWG_BENCH_PROJECT", {});
    REQUIRE (path.isNotEmpty());
    const juce::File file (path);
    REQUIRE (file.existsAsFile());
    return { projectfile::read (file).project, file.getParentDirectory() };
}

struct Rig
{
    juce::AudioFormatManager formats;
    juce::TimeSliceThread readAhead { "bench read-ahead" };
    std::map<juce::String, std::shared_ptr<const AudioSource>> sources;
    GraphCompiler compiler;

    explicit Rig (const juce::File& folder)
    {
        formats.registerBasicFormats();
        readAhead.startThread();
        compiler.setAudioSourceResolver (
            [this, folder] (const AudioFile& audio) -> std::shared_ptr<const AudioSource>
            {
                const auto location = juce::File::isAbsolutePath (audio.path) ? juce::File (audio.path)
                                                                              : folder.getChildFile (audio.path);
                auto& source = sources[location.getFullPathName()];

                if (source == nullptr && location.existsAsFile())
                    source = AudioSource::open (location, formats, readAhead);

                return source;
            });
    }

    ~Rig() { readAhead.stopThread (1000); }

    Rig (const Rig&) = delete;
    Rig& operator= (const Rig&) = delete;
};

struct BlockStats
{
    double meanMs = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

[[nodiscard]] BlockStats statsOf (std::vector<double> times)
{
    BlockStats stats;

    if (times.empty())
        return stats;

    double sum = 0.0;

    for (const auto t : times)
        sum += t;

    std::ranges::sort (times);
    stats.meanMs = sum / static_cast<double> (times.size());
    stats.p99Ms = times[static_cast<size_t> (0.99 * static_cast<double> (times.size() - 1))];
    stats.maxMs = times.back();
    return stats;
}

[[nodiscard]] double startBeats (const Project& project)
{
    return project.loop.enabled ? static_cast<double> (project.loop.start) / static_cast<double> (ticksPerBeat) : 0.0;
}

[[nodiscard]] BlockStats runEngine (const Project& project, Rig& rig, const ProcessSpec& spec, int block, int blocks,
                                    bool parallel)
{
    AudioEngine engine;
    engine.setParallelTracks (parallel);
    engine.setEngine (rig.compiler.compile (project, spec));
    engine.seek (startBeats (project));
    engine.play();

    std::vector<float> left (static_cast<size_t> (block));
    std::vector<float> right (static_cast<size_t> (block));
    std::array<float*, 2> outputs { left.data(), right.data() };
    std::vector<double> times;
    times.reserve (static_cast<size_t> (blocks));

    // Warm up (first touches, voice allocation), then measure.
    for (int i = 0; i < 50; ++i)
        engine.renderOffline (outputs.data(), 2, block, spec.sampleRate);

    for (int i = 0; i < blocks; ++i)
    {
        const auto start = juce::Time::getHighResolutionTicks();
        engine.renderOffline (outputs.data(), 2, block, spec.sampleRate);
        times.push_back (ticksToMs (juce::Time::getHighResolutionTicks() - start));
    }

    return statsOf (std::move (times));
}

void runSteps (const Project& project, Rig& rig, const ProcessSpec& spec, int block, int blocks)
{
    const auto compiled = rig.compiler.compile (project, spec);
    const auto& engine = *compiled;
    const auto deadlineMs = 1000.0 * block / spec.sampleRate;

    std::vector<float> silence (static_cast<size_t> (spec.maxBlockSize) * 2, 0.0f);
    const AudioInput silent { silence.data(), silence.data() + spec.maxBlockSize };

    TimeMap timeMap (engine.tempoMap, spec.sampleRate);
    const auto loopStart = static_cast<double> (project.loop.start);
    const auto loopEnd = static_cast<double> (project.loop.end);
    timeMap.anchor (project.loop.enabled ? loopStart : 0.0, 0);
    juce::int64 position = 0;

    // Index of a step within its track (master last), total ticks spent in it, and the most voices seen.
    struct StepCost
    {
        std::string name;
        juce::int64 ticks = 0;
        int voices = 0;
    };

    std::vector<const CompiledTrack*> tracks;

    for (const auto& track : engine.tracks)
        tracks.push_back (&track);

    if (engine.master.has_value())
        tracks.push_back (&*engine.master);

    std::vector<std::vector<StepCost>> costs (tracks.size());

    for (size_t t = 0; t < tracks.size(); ++t)
        for (const auto& step : tracks[t]->steps)
            costs[t].push_back ({ nameOf (step) });

    bool started = true;
    bool looped = false;

    for (int b = 0; b < blocks + 50; ++b)
    {
        Transport transport;
        transport.playing = true;
        transport.started = started;
        transport.looped = looped;
        transport.startSample = position;
        transport.timeMap = timeMap;
        transport.startTick = timeMap.sampleToTick (static_cast<double> (position));
        transport.endTick = timeMap.sampleToTick (static_cast<double> (position + block));
        started = false;
        looped = false;

        for (size_t t = 0; t < tracks.size(); ++t)
        {
            const auto& track = *tracks[t];
            ProcessContext base;
            base.numSamples = block;
            base.transport = &transport;
            base.sequence = track.sequence.get();
            base.audio = track.audio.get();
            base.input = silent;

            for (size_t s = 0; s < track.steps.size(); ++s)
            {
                const auto region = track.steps[s].regionSteps;
                const auto count = region > 0 ? region : size_t { 1 };
                const auto start = juce::Time::getHighResolutionTicks();
                dawg::runSteps (std::span (track.steps).subspan (s, count), engine.eventBuffers.get(), base, 0,
                                spec.maxBlockSize);
                const auto spent = juce::Time::getHighResolutionTicks() - start;

                if (b >= 50)
                {
                    costs[t][s].ticks += spent;

                    if (auto* node = track.steps[s].node; node != nullptr)
                        if (const auto meters = node->meters(); meters.hasProperty ("voices"))
                            costs[t][s].voices = std::max (costs[t][s].voices, static_cast<int> (meters["voices"]));
                }

                if (region > 0)
                {
                    costs[t][s].name = "loop region of " + std::to_string (region) + " steps from " + costs[t][s].name;
                    s += region - 1;
                }
            }
        }

        position += block;

        if (project.loop.enabled && timeMap.sampleToTick (static_cast<double> (position)) >= loopEnd)
        {
            timeMap.anchor (loopStart, position);
            looped = true;
        }
    }

    std::printf ("\nPer step, run one at a time on one thread (share of a %.2f ms block):\n", deadlineMs);
    double totalMs = 0.0;

    struct Row
    {
        double ms;
        std::string text;
    };

    std::vector<Row> rows;

    for (size_t t = 0; t < tracks.size(); ++t)
    {
        double trackMs = 0.0;
        const auto isMaster = engine.master.has_value() && t + 1 == tracks.size();
        const Track* model = isMaster ? project.master.get() : nullptr;

        for (const auto& track : project.tracks)
            if (! isMaster && track->id == tracks[t]->id)
                model = track.get();

        const auto name = model != nullptr ? model->name.toStdString() : std::string ("?");
        std::printf ("  track %lld \"%s\"%s%s\n", static_cast<long long> (tracks[t]->id), name.c_str(),
                     tracks[t]->bus >= 0 ? " [reads a bus: serial pass]" : "", tracks[t]->keyed ? " [keyed]" : "");

        for (const auto& cost : costs[t])
        {
            const auto ms = ticksToMs (cost.ticks) / blocks;
            trackMs += ms;

            if (ms >= 0.001)
            {
                std::printf ("      %7.3f ms  %5.1f %%  %s", ms, 100.0 * ms / deadlineMs, cost.name.c_str());

                if (cost.voices > 0)
                    std::printf ("  (up to %d voices)", cost.voices);

                std::printf ("\n");
                rows.push_back ({ ms, name + " (" + std::to_string (tracks[t]->id) + ") / " + cost.name });
            }
        }

        std::printf ("    = %7.3f ms  %5.1f %%\n", trackMs, 100.0 * trackMs / deadlineMs);
        totalMs += trackMs;
    }

    std::printf ("  all steps: %.3f ms per block, %.1f %% of one core's deadline\n", totalMs,
                 100.0 * totalMs / deadlineMs);

    std::ranges::sort (rows, [] (const Row& a, const Row& b) { return a.ms > b.ms; });
    std::printf ("\nCostliest steps:\n");

    for (size_t i = 0; i < std::min<size_t> (rows.size(), 15); ++i)
        std::printf ("  %7.3f ms  %5.1f %%  %s\n", rows[i].ms, 100.0 * rows[i].ms / deadlineMs, rows[i].text.c_str());
}
} // namespace

TEST_CASE ("What a saved project costs to render", "[.project-bench]")
{
    // The device's audio thread doesn't flush denormals (only the pool's workers and EffectNode::process do), so
    // by default neither does this one. DAWG_BENCH_FTZ=1 flushes them here too.
    std::optional<juce::ScopedNoDenormals> noDenormals;

    if (envOr ("DAWG_BENCH_FTZ", 0.0) > 0.5)
        noDenormals.emplace();

    const auto loaded = load();
    Rig rig (loaded.folder);

    const auto rate = envOr ("DAWG_BENCH_RATE", 48000.0);
    const auto block = static_cast<int> (envOr ("DAWG_BENCH_BLOCK", 480.0));
    const auto seconds = envOr ("DAWG_BENCH_SECONDS", 20.0);
    const ProcessSpec spec { rate, 512 };
    const auto blocks = static_cast<int> (seconds * rate / block);
    const auto deadlineMs = 1000.0 * block / rate;

    std::printf ("Project: %d tracks, %.0f Hz, %d-sample blocks (%.2f ms), %d blocks\n",
                 static_cast<int> (loaded.project.tracks.size()), rate, block, deadlineMs, blocks);

    for (const auto parallel : { true, false })
    {
        const auto stats = runEngine (loaded.project, rig, spec, block, blocks, parallel);
        std::printf ("Engine, pool %s: mean %.3f ms (%.1f %%), p99 %.3f ms (%.1f %%), max %.3f ms (%.1f %%)\n",
                     parallel ? "on " : "off", stats.meanMs, 100.0 * stats.meanMs / deadlineMs, stats.p99Ms,
                     100.0 * stats.p99Ms / deadlineMs, stats.maxMs, 100.0 * stats.maxMs / deadlineMs);
    }

    runSteps (loaded.project, rig, spec, block, blocks);
}
