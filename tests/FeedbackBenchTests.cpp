#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/GraphCompiler.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

// What a feedback loop's chunk size costs (docs/design/feedback-routing.md, R3). Hidden: run with [.feedback-bench]. It runs a
// voice graph of real nodes in every voice slot, the way a region will, with the copy through a delay line a Feedback node
// makes per chunk. The signal comes from an oscillator rather than around a loop, which costs the same.

using namespace dawg;
using namespace dawg::test;

namespace
{
[[nodiscard]] GraphNode benchNode (ObjectId id, const char* type)
{
    GraphNode result;
    result.id = id;
    result.type = type;
    return result;
}

[[nodiscard]] Project benchProject()
{
    TrackGraph voice;
    voice.nodes = { benchNode (10, nodetype::voiceInput), benchNode (11, nodetype::oscillator),
                    benchNode (12, nodetype::mix),        benchNode (13, nodetype::filter),
                    benchNode (14, nodetype::saturator),  benchNode (15, nodetype::gain),
                    benchNode (16, nodetype::voiceOutput) };
    voice.connections = { { 11, 0, 12, 0 }, { 12, 0, 13, 0 }, { 13, 0, 14, 0 }, { 14, 0, 15, 0 }, { 15, 0, 16, 0 } };

    TrackGraph graph;
    graph.nodes = { benchNode (1, nodetype::sequencer), benchNode (2, nodetype::voiceInstrument),
                    benchNode (3, nodetype::trackOutput) };
    graph.nodes[1].voiceGraph = std::make_shared<const TrackGraph> (voice);
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

    Track track;
    track.id = 1;
    track.graph = std::move (graph);

    Project project;
    project.tracks.push_back (std::make_shared<const Track> (std::move (track)));
    return project;
}

struct Result
{
    double loadPercent = 0.0;
    double nanosecondsPerCall = 0.0;
    float peak = 0.0f;
};

[[nodiscard]] Result measure (const CompiledVoice& voice, int blockLength, int chunk, int voices)
{
    constexpr double rate = 48000.0;
    constexpr double seconds = 3.0;
    const auto steps = voice.slots[0].steps.size();
    const auto blocks = static_cast<int> (seconds * rate / blockLength);

    // Per voice, two channels: the delay line a Feedback node would keep.
    constexpr size_t ringLength = 4096;
    std::vector<std::array<std::array<float, ringLength>, 2>> rings (static_cast<size_t> (voices));
    size_t ringPosition = 0;

    Transport transport;
    const TrackSequence sequence { std::span<const ClipPtr> {} };
    Result result;
    juce::int64 calls = 0;
    const auto start = juce::Time::getHighResolutionTicks();

    for (int block = 0; block < blocks; ++block)
    {
        for (int v = 0; v < voices; ++v)
        {
            const auto& slot = voice.slots[static_cast<size_t> (v)];
            auto position = ringPosition;

            for (int offset = 0; offset < blockLength; offset += chunk)
            {
                const auto length = std::min (chunk, blockLength - offset);
                ProcessContext base;
                base.numSamples = length;
                base.transport = &transport;
                base.sequence = &sequence;
                runSteps (slot.steps, slot.eventBuffers.get(), base, offset, blockLength);
                calls += static_cast<juce::int64> (steps);

                const auto& out = slot.steps.back().audioOutputs[0];
                const auto count = std::min<size_t> (static_cast<size_t> (length), ringLength - position);

                for (size_t ch = 0; ch < 2; ++ch)
                    std::memcpy (rings[static_cast<size_t> (v)][ch].data() + position, out[ch] + offset,
                                 count * sizeof (float));

                result.peak = std::max (result.peak, std::abs (out[0][offset]));
                position = (position + count) % ringLength;
            }
        }

        ringPosition = (ringPosition + static_cast<size_t> (blockLength)) % ringLength;
    }

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    result.loadPercent = 100.0 * elapsed / (blocks * blockLength / rate);
    result.nanosecondsPerCall = elapsed * 1e9 / static_cast<double> (calls);
    return result;
}
} // namespace

TEST_CASE ("what a feedback loop's chunk size costs", "[.feedback-bench]")
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    GraphCompiler compiler (registry);

    for (const auto blockLength : { 128, 512 })
    {
        const auto engine = compiler.compile (benchProject(), { 48000.0, blockLength });
        const auto step =
            std::ranges::find_if (engine->tracks[0].steps, [] (const CompiledStep& s) { return s.voice != nullptr; });
        REQUIRE (step != engine->tracks[0].steps.end());

        std::printf ("block %d, %d steps per voice\n", blockLength,
                     static_cast<int> (step->voice->slots[0].steps.size()));

        for (const auto voices : { 1, 16 })
            for (const auto chunk : { blockLength, 64, 32, 16, 8 })
            {
                const auto result = measure (*step->voice, blockLength, chunk, voices);
                std::printf ("  %2d voices, chunk %3d: %6.1f %% of one core, %5.0f ns per node call, peak %.2f\n",
                             voices, chunk, result.loadPercent, result.nanosecondsPerCall,
                             static_cast<double> (result.peak));
                std::fflush (stdout);
            }
    }
}
