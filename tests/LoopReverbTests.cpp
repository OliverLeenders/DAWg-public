#include "../engine/src/loopreverb/Reverb.h"
#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>
#include <string_view>

using namespace dawg;
using namespace dawg::test;

// The Loop Reverb's adapter (engine/src/LoopReverb.cpp): DAWg's node must be exactly the clean-room reverb
// (engine/src/loopreverb/). The reverb's own behaviour is tested by its clean-room tests (tests/loopreverb/).

namespace
{
using Settings = std::map<juce::String, float>;

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

[[nodiscard]] Stereo noise (size_t length)
{
    std::mt19937 random (20261027); // NOLINT(bugprone-random-generator-seed): the same noise in every run
    std::uniform_real_distribution<float> uniform (-0.5f, 0.5f);
    Stereo result { std::vector<float> (length), std::vector<float> (length) };

    for (size_t i = 0; i < length; ++i)
    {
        result.left[i] = uniform (random);
        result.right[i] = i < length / 2 ? uniform (random) : 0.0f;
    }

    return result;
}

[[nodiscard]] std::unique_ptr<Node> makeReverb (const Settings& settings)
{
    auto node = NodeRegistry::builtin().create (nodetype::loopReverb);
    REQUIRE (node != nullptr);

    for (const auto& [id, value] : settings)
        node->params().set (static_cast<size_t> (node->descriptor().indexOfParam (id)), value);

    return node;
}

// Block sizes that vary, as the engine's may.
constexpr std::array<int, 5> blockSizes { 512, 100, 1, 37, 256 };

// Runs DAWg's node over the input, in blocks of varying size.
[[nodiscard]] Stereo run (Node& node, const Stereo& input)
{
    Stereo output { std::vector<float> (input.left.size()), std::vector<float> (input.left.size()) };
    Transport transport;
    size_t block = 0;

    for (size_t offset = 0; offset < input.left.size(); ++block)
    {
        const auto n =
            std::min (static_cast<size_t> (blockSizes[block % blockSizes.size()]), input.left.size() - offset);
        const std::array<AudioInput, 1> ins { { { input.left.data() + offset, input.right.data() + offset } } };
        const std::array<AudioOutput, 1> outs { { { output.left.data() + offset, output.right.data() + offset } } };
        ProcessContext context;
        context.numSamples = static_cast<int> (n);
        context.audioInputs = ins;
        context.audioOutputs = outs;
        context.transport = &transport;
        node.process (context);
        offset += n;
    }

    return output;
}

// Runs the clean-room reverb itself over the input, in the same blocks.
[[nodiscard]] Stereo run (reverb::ReverbNode& node, const Stereo& input)
{
    Stereo output { std::vector<float> (input.left.size()), std::vector<float> (input.left.size()) };
    reverb::Transport transport;
    size_t block = 0;

    for (size_t offset = 0; offset < input.left.size(); ++block)
    {
        const auto n =
            std::min (static_cast<size_t> (blockSizes[block % blockSizes.size()]), input.left.size() - offset);
        const std::array<reverb::AudioInput, 1> ins { { { input.left.data() + offset, input.right.data() + offset } } };
        const std::array<reverb::AudioOutput, 1> outs { { { output.left.data() + offset,
                                                            output.right.data() + offset } } };
        node.process ({ static_cast<int> (n), ins, outs, &transport });
        offset += n;
    }

    return output;
}
} // namespace

TEST_CASE ("the Loop Reverb is a built-in effect with the clean-room reverb's controls", "[reverb]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::loopReverb);
    REQUIRE (descriptor != nullptr);
    CHECK (std::string_view (descriptor->displayName) == "Loop Reverb");
    CHECK (descriptor->category == NodeCategory::effect);
    CHECK (descriptor->userEditable);

    // One stereo input, DAWg's modulation inputs, one stereo output.
    REQUIRE (descriptor->inputs.size() == 1 + maxModulationInputs);
    CHECK (descriptor->inputs[0].type == PortType::audio);
    CHECK (descriptor->inputs[1].type == PortType::modulation);
    REQUIRE (descriptor->outputs.size() == 1);
    CHECK (descriptor->outputs[0].type == PortType::audio);

    REQUIRE (descriptor->params.size() == reverb::params.size());

    for (size_t i = 0; i < reverb::params.size(); ++i)
    {
        const auto& ours = descriptor->params[i];
        const auto& theirs = reverb::params[i];
        INFO (theirs.id);
        CHECK (std::string_view (ours.id) == theirs.id);
        CHECK (std::string_view (ours.name) == theirs.name);
        CHECK (std::string_view (ours.unit) == theirs.unit);
        CHECK (ours.minValue == theirs.minValue);
        CHECK (ours.maxValue == theirs.maxValue);
        CHECK (ours.defaultValue == theirs.defaultValue);
        CHECK (static_cast<int> (ours.kind) == static_cast<int> (theirs.kind));
    }

    for (const auto* id : { "feedback", "diffusion", "size", "predelay", "earlyTiming", "width", "damp", "thin",
                            "modulation", "mute", "mix", "seed" })
        CHECK (descriptor->indexOfParam (id) >= 0);
}

TEST_CASE ("the Loop Reverb renders exactly what the clean-room reverb renders", "[reverb]")
{
    const auto input = noise (static_cast<size_t> (sampleRate)); // the right input stops halfway
    const std::vector<Settings> settings {
        {},
        { { "feedback", 90.0f },
          { "size", 80.0f },
          { "diffusion", 100.0f },
          { "predelay", 40.0f },
          { "earlyTiming", 75.0f },
          { "width", 60.0f },
          { "damp", 30.0f },
          { "thin", 40.0f },
          { "mix", 60.0f },
          { "seed", 7.0f } },
        // Width beyond the natural widest and the chorus on (SPEC 1.6).
        { { "feedback", 80.0f }, { "width", 95.0f }, { "modulation", 100.0f }, { "mix", 100.0f } },
    };

    for (const auto& setting : settings)
    {
        auto node = makeReverb (setting);
        node->prepare (spec);

        reverb::ReverbNode reference;

        for (const auto& [id, value] : setting)
            reference.params().set (reverb::paramIndex (id.toStdString()), value);

        reference.prepare ({ sampleRate, blockSize });

        const auto ours = run (*node, input);
        const auto theirs = run (reference, input);
        CHECK (ours.left == theirs.left);
        CHECK (ours.right == theirs.right);
    }
}

TEST_CASE ("the Loop Reverb follows parameter changes between blocks", "[reverb]")
{
    const auto input = noise (static_cast<size_t> (sampleRate / 2));
    auto node = makeReverb ({});
    node->prepare (spec);
    reverb::ReverbNode reference;
    reference.prepare ({ sampleRate, blockSize });

    // The engine writes automation and modulation into the node's parameters before each block.
    const auto first = Stereo { { input.left.begin(), input.left.begin() + 6000 },
                                { input.right.begin(), input.right.begin() + 6000 } };
    const auto second =
        Stereo { { input.left.begin() + 6000, input.left.end() }, { input.right.begin() + 6000, input.right.end() } };
    CHECK (run (*node, first).left == run (reference, first).left);

    node->params().set (static_cast<size_t> (node->descriptor().indexOfParam ("size")), 90.0f);
    node->params().set (static_cast<size_t> (node->descriptor().indexOfParam ("mix")), 80.0f);
    reference.params().set (reverb::param::size, 90.0f);
    reference.params().set (reverb::param::mix, 80.0f);
    CHECK (run (*node, second).left == run (reference, second).left);
}

TEST_CASE ("the Loop Reverb's bypass passes the input through", "[reverb]")
{
    const auto input = noise (4096);
    auto node = makeReverb ({ { "mix", 100.0f } });
    node->prepare (spec);

    GraphNode model;
    model.type = nodetype::loopReverb;
    model.bypassed = true;
    node->configure (model);
    const auto bypassed = run (*node, input);
    CHECK (bypassed.left == input.left);
    CHECK (bypassed.right == input.right);

    model.bypassed = false;
    node->configure (model);
    CHECK (run (*node, input).left != input.left);
}

TEST_CASE ("the Loop Reverb's meters are arrays of numbers for the panel", "[reverb]")
{
    auto node = makeReverb ({ { "predelay", 20.0f } });
    node->prepare (spec);
    std::ignore = run (*node, noise (4096));

    const auto meters = node->meters();
    REQUIRE (meters.isObject());

    for (const auto* key : { "arrivalsLeft", "arrivalsRight" })
    {
        INFO (key);
        const auto* arrivals = meters[key].getArray();
        REQUIRE (arrivals != nullptr);
        REQUIRE (! arrivals->isEmpty());
        CHECK (arrivals->size() <= 32);
        CHECK (static_cast<double> (arrivals->getFirst()) >= 20.0); // after the pre-delay, in ms

        for (int i = 1; i < arrivals->size(); ++i)
            CHECK (static_cast<double> ((*arrivals)[i - 1]) <= static_cast<double> ((*arrivals)[i]));
    }

    const auto* level = meters["level"].getArray();
    REQUIRE (level != nullptr);
    REQUIRE (level->size() == 2);
    CHECK (static_cast<double> ((*level)[0]) > 0.0);
}

TEST_CASE ("the Loop Reverb's arrivals follow its controls without processing", "[reverb]")
{
    // A node that doesn't reach the output is never processed, yet its panel shows its settings.
    auto node = makeReverb ({});
    node->prepare (spec);
    node->params().set (static_cast<size_t> (node->descriptor().indexOfParam ("predelay")), 80.0f);

    reverb::ParamValues expected {};

    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = node->params().get (i);

    const auto times = reverb::arrivals (expected, sampleRate);
    const auto meters = node->meters();

    for (const auto& [key, side] : { std::pair { "arrivalsLeft", 0 }, std::pair { "arrivalsRight", 1 } })
    {
        INFO (key);
        const auto* arrivals = meters[key].getArray();
        REQUIRE (arrivals != nullptr);
        REQUIRE (static_cast<size_t> (arrivals->size()) == times[static_cast<size_t> (side)].size());
        CHECK (static_cast<double> (arrivals->getFirst()) >= 80.0);
    }
}

TEST_CASE ("the Loop Reverb never allocates while processing", "[reverb][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    const auto input = noise (static_cast<size_t> (sampleRate / 4));
    auto node = makeReverb ({ { "mix", 50.0f } });
    node->prepare (spec);
    Stereo output { std::vector<float> (input.left.size()), std::vector<float> (input.left.size()) };
    Transport transport;
    const auto before = allocationViolationCount();

    for (size_t offset = 0; offset + blockSize <= input.left.size(); offset += blockSize)
    {
        const std::array<AudioInput, 1> ins { { { input.left.data() + offset, input.right.data() + offset } } };
        const std::array<AudioOutput, 1> outs { { { output.left.data() + offset, output.right.data() + offset } } };
        ProcessContext context;
        context.numSamples = blockSize;
        context.audioInputs = ins;
        context.audioOutputs = outs;
        context.transport = &transport;

        // Every control moves, as automation would.
        node->params().set (0, static_cast<float> (offset % 100));
        const auto block = offset / blockSize;
        node->params().set (static_cast<size_t> (node->descriptor().indexOfParam ("seed")),
                            static_cast<float> (1 + block));

        const ScopedNoAllocation guard;
        node->process (context);
    }

    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("a track can hold the Loop Reverb", "[reverb][commands]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::loopReverb } });

    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto reverbNode =
        std::ranges::find_if (nodes, [] (const GraphNode& node) { return node.type == nodetype::loopReverb; });
    REQUIRE (reverbNode != nodes.end());
}
