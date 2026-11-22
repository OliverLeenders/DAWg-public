#include "TestUtils.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// The Delay and the Limiter (docs/design/sampler-midi-delay-limiter.md).

namespace
{
struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

// One effect instance, run over whole buffers in blocks.
struct Effect
{
    Effect (const char* type, const std::map<juce::String, float>& values)
        : node (NodeRegistry::builtin().create (type))
    {
        REQUIRE (node != nullptr);
        set (values);
        GraphNode model;
        model.type = type;
        node->configure (model);
        node->prepare (spec);
    }

    void set (const std::map<juce::String, float>& values)
    {
        for (const auto& [id, value] : values)
        {
            const auto index = node->descriptor().indexOfParam (id);
            REQUIRE (index >= 0);
            node->params().set (static_cast<size_t> (index), value);
        }
    }

    [[nodiscard]] Stereo run (const std::vector<float>& inputLeft, const std::vector<float>& inputRight)
    {
        Stereo out { std::vector<float> (inputLeft.size()), std::vector<float> (inputLeft.size()) };
        const std::vector<float> silence (static_cast<size_t> (blockSize));

        for (size_t offset = 0; offset < inputLeft.size(); offset += blockSize)
        {
            const auto count = std::min (static_cast<size_t> (blockSize), inputLeft.size() - offset);
            std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                            AudioInput { silence.data(), silence.data() });
            inputs[0] = { inputLeft.data() + offset, inputRight.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { out.left.data() + offset,
                                                                     out.right.data() + offset } };

            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            node->process (context);
        }

        return out;
    }

    [[nodiscard]] Stereo run (const std::vector<float>& input) { return run (input, input); }

    std::unique_ptr<Node> node;
};

[[nodiscard]] std::vector<float> impulse (int length, float level = 1.0f)
{
    std::vector<float> result (static_cast<size_t> (length));
    result[0] = level;
    return result;
}

[[nodiscard]] std::vector<float> sine (double frequency, double amplitude, int length)
{
    std::vector<float> result (static_cast<size_t> (length));

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<float> (
            amplitude
            * std::sin (juce::MathConstants<double>::twoPi * frequency * static_cast<double> (i) / sampleRate));

    return result;
}

[[nodiscard]] float peak (const std::vector<float>& signal, size_t from, size_t to)
{
    float largest = 0.0f;

    for (auto i = from; i < std::min (to, signal.size()); ++i)
        largest = std::max (largest, std::abs (signal[i]));

    return largest;
}

// A wide-open delay: only the echoes, none of the filters' colour.
const std::map<juce::String, float> plainDelay { { "sync", 0.0f },     { "timeL", 100.0f }, { "timeR", 100.0f },
                                                 { "mix", 100.0f },    { "lowCut", 20.0f }, { "highCut", 20000.0f },
                                                 { "feedback", 50.0f } };
} // namespace

TEST_CASE ("the Delay repeats at its time, each echo scaled by the feedback", "[delay][dsp]")
{
    Effect delay (nodetype::delay, plainDelay);
    const auto out = delay.run (impulse (20000)).left;

    // The dry impulse at full, then the echoes.
    CHECK_THAT (peak (out, 0, 10), WithinAbs (1.0, 1.0e-4));
    CHECK (peak (out, 10, 4700) < 1.0e-3f);
    CHECK_THAT (peak (out, 4700, 4900), WithinAbs (1.0, 0.1));
    CHECK_THAT (peak (out, 9500, 9700), WithinAbs (0.5, 0.1));
    CHECK_THAT (peak (out, 14300, 14500), WithinAbs (0.25, 0.08));
}

TEST_CASE ("Mix 0 % is the dry path", "[delay][dsp]")
{
    auto values = plainDelay;
    values["mix"] = 0.0f;
    Effect delay (nodetype::delay, values);
    const auto input = sine (440.0, 0.5, 8000);
    const auto out = delay.run (input).left;

    // The mix starts at its target, so nothing has to settle.
    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE_THAT (out[i], WithinAbs (input[i], 1.0e-5));
}

TEST_CASE ("Wet sets the echoes' level and never turns the dry sound down", "[delay][dsp]")
{
    const auto input = sine (440.0, 0.5, 4000); // shorter than the first echo
    auto values = plainDelay;
    values["feedback"] = 0.0f;

    for (const auto wet : { 25.0f, 100.0f })
    {
        values["mix"] = wet;
        Effect delay (nodetype::delay, values);
        auto padded = input;
        padded.resize (12000, 0.0f);
        const auto out = delay.run (padded).left;

        for (size_t i = 0; i < input.size(); ++i)
            REQUIRE_THAT (out[i], WithinAbs (input[i], 1.0e-5));

        // The echo, 100 ms on, at the wet level.
        CHECK_THAT (peak (out, 4800, 8800), WithinAbs (0.5 * wet / 100.0, 0.02));
    }
}

TEST_CASE ("the Delay follows the tempo when synced", "[delay][dsp]")
{
    // No transport in this test: 120 bpm, so the default dotted eighth is 0.375 s.
    Effect delay (nodetype::delay, { { "sync", 1.0f }, { "mix", 100.0f }, { "highCut", 20000.0f } });
    const auto out = delay.run (impulse (20000)).left;
    CHECK (peak (out, 10, 17900) < 1.0e-3f);
    CHECK (peak (out, 17900, 18100) > 0.5f);
}

TEST_CASE ("left and right have their own times unless linked", "[delay][dsp]")
{
    auto values = plainDelay;
    values["timeR"] = 200.0f;
    values["feedback"] = 0.0f;
    values["link"] = 0.0f;
    Effect separate (nodetype::delay, values);
    const auto out = separate.run (impulse (12000));
    CHECK (peak (out.left, 4700, 4900) > 0.5f);
    CHECK (peak (out.right, 4700, 4900) < 1.0e-3f);
    CHECK (peak (out.right, 9500, 9700) > 0.5f);

    values["link"] = 1.0f;
    Effect linked (nodetype::delay, values);
    CHECK (peak (linked.run (impulse (12000)).right, 4700, 4900) > 0.5f);
}

TEST_CASE ("ping-pong enters left and crosses sides", "[delay][dsp]")
{
    auto values = plainDelay;
    values["pingPong"] = 1.0f;
    Effect delay (nodetype::delay, values);
    const std::vector<float> silent (15000);
    const auto out = delay.run (impulse (15000), silent);

    CHECK (peak (out.left, 4700, 4900) > 0.3f); // half the left-only impulse, as the input goes mono
    CHECK (peak (out.right, 4700, 4900) < 1.0e-3f);
    CHECK (peak (out.right, 9500, 9700) > 0.1f);
    CHECK (peak (out.left, 9500, 9700) < 1.0e-3f);
    CHECK (peak (out.left, 14300, 14500) > 0.05f);
}

TEST_CASE ("the filters thin the echoes", "[delay][dsp]")
{
    const auto level = [] (float lowCut, float highCut, double frequency)
    {
        auto values = plainDelay;
        values["feedback"] = 0.0f;
        values["lowCut"] = lowCut;
        values["highCut"] = highCut;
        Effect delay (nodetype::delay, values);
        // Eight thousand frames of sine, then silence: from 8100 on only its echo, 100 ms later, is heard.
        auto input = sine (frequency, 0.5, 8000);
        input.resize (16000, 0.0f);
        return peak (delay.run (input).left, 8100, 12700);
    };

    CHECK (level (20.0f, 500.0f, 8000.0) < 0.1f);
    CHECK (level (2000.0f, 20000.0f, 60.0) < 0.05f);
    CHECK (level (20.0f, 20000.0f, 440.0) > 0.45f);
}

TEST_CASE ("changing the time glides without a click", "[delay][dsp]")
{
    auto values = plainDelay;
    values["feedback"] = 0.0f;
    Effect delay (nodetype::delay, values);
    const auto input = sine (100.0, 0.5, 40000);
    const std::vector<float> first (input.begin(), input.begin() + 20000);
    const std::vector<float> second (input.begin() + 20000, input.end());
    std::ignore = delay.run (first);
    delay.set ({ { "timeL", 400.0f } });
    const auto out = delay.run (second).left;

    float largest = 0.0f;

    for (size_t i = 1; i < out.size(); ++i)
        largest = std::max (largest, std::abs (out[i] - out[i - 1]));

    // A 100 Hz sine at 0.5 steps by 0.0065 per sample. The glide reads it at up to four times the speed, an octave or two
    // of bent pitch, and that is all: a jump in the signal would step by far more.
    CHECK (largest < 0.04f);
}

TEST_CASE ("full feedback stays bounded", "[delay][dsp]")
{
    auto values = plainDelay;
    values["feedback"] = 100.0f;
    values["timeL"] = 1.0f;
    Effect delay (nodetype::delay, values);
    const auto out = delay.run (sine (3000.0, 1.0, 200000)).left;
    CHECK (std::ranges::all_of (out, [] (float s) { return std::isfinite (s) && std::abs (s) < 8.0f; }));
}

TEST_CASE ("resetting the Delay clears its echoes", "[delay][dsp]")
{
    Effect delay (nodetype::delay, plainDelay);
    std::ignore = delay.run (impulse (2000));
    delay.node->reset();
    CHECK (peak (delay.run (std::vector<float> (20000)).left, 0, 20000) < 1.0e-5f);
}

TEST_CASE ("the Limiter never passes its ceiling", "[limiter][dsp]")
{
    for (const auto ceiling : { -0.3f, -6.0f, -24.0f })
    {
        Effect limiter (nodetype::limiter, { { "gain", 24.0f }, { "ceiling", ceiling } });
        const auto limit = std::pow (10.0f, ceiling / 20.0f);
        const auto out = limiter.run (sine (997.0, 1.0, 48000)).left;
        CHECK (peak (out, 0, out.size()) <= limit + 1.0e-5f);
        CHECK (peak (out, 24000, out.size()) > 0.9f * limit); // and it gets there
    }

    // A lone spike at the very start of a block, and one straddling two blocks.
    Effect limiter (nodetype::limiter, { { "ceiling", -6.0f } });
    auto spikes = std::vector<float> (4096);
    spikes[static_cast<size_t> (blockSize)] = 4.0f;
    spikes[static_cast<size_t> (2 * blockSize - 1)] = -4.0f;
    const auto out = limiter.run (spikes).left;
    CHECK (peak (out, 0, out.size()) <= std::pow (10.0f, -6.0f / 20.0f) + 1.0e-5f);
}

TEST_CASE ("a quiet signal passes the Limiter unchanged but for its latency", "[limiter][dsp]")
{
    Effect limiter (nodetype::limiter, {});
    const auto latency = limiter.node->latencySamples();
    CHECK (latency == 240); // 5 ms at 48 kHz

    const auto input = sine (440.0, 0.25, 8000);
    const auto out = limiter.run (input).left;

    for (size_t i = static_cast<size_t> (latency); i < input.size(); ++i)
        REQUIRE_THAT (out[i], WithinAbs (input[i - static_cast<size_t> (latency)], 1.0e-5));

    CHECK (peak (out, 0, static_cast<size_t> (latency)) == 0.0f);
}

TEST_CASE ("the Limiter recovers with its release and shows its reduction", "[limiter][dsp]")
{
    Effect limiter (nodetype::limiter, { { "release", 50.0f }, { "ceiling", -6.0f } });
    auto input = sine (440.0, 1.0, 24000);
    const auto quiet = sine (440.0, 0.1, 24000);
    input.insert (input.end(), quiet.begin(), quiet.end());

    const auto loud = limiter.run (std::vector<float> (input.begin(), input.begin() + 24000)).left;
    CHECK (static_cast<float> (limiter.node->meters()["reduction"]) > 4.0f);
    CHECK (peak (loud, 0, loud.size()) <= std::pow (10.0f, -6.0f / 20.0f) + 1.0e-5f);

    const auto after = limiter.run (std::vector<float> (input.begin() + 24000, input.end())).left;
    CHECK_THAT (peak (after, 20000, 24000), WithinAbs (0.1, 0.002)); // recovered: the quiet part comes through whole
    CHECK (static_cast<float> (limiter.node->meters()["reduction"]) < 0.1f);
}

TEST_CASE ("a bypassed node adds no latency, and the project's latency follows", "[limiter][bypass]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::limiter }, { "index", 0 } });
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::utility }, { "index", 1 } });

    const auto idOf = [&] (const char* type)
    {
        const auto& nodes = project.tracks[0]->graph.nodes;
        return std::ranges::find_if (nodes, [type] (const GraphNode& n) { return n.type == type; })->id;
    };
    const auto bypass = [&] (const char* type, bool bypassed)
    {
        project = apply (project, "setNodeBypass",
                         { { "trackId", trackId }, { "nodeId", idOf (type) }, { "bypassed", bypassed } });
    };

    GraphCompiler compiler;
    const auto first = compiler.update (project, spec);
    REQUIRE (first != nullptr);
    CHECK (first->latencySamples == 240);

    bypass (nodetype::limiter, true);
    const auto bypassed = compiler.update (project, spec);
    REQUIRE (bypassed != nullptr); // the delays changed, so it compiled
    CHECK (bypassed->latencySamples == 0);

    // A node without latency needs no compilation.
    bypass (nodetype::utility, true);
    CHECK (compiler.update (project, spec) == nullptr);

    bypass (nodetype::limiter, false);
    const auto back = compiler.update (project, spec);
    REQUIRE (back != nullptr);
    CHECK (back->latencySamples == 240);
}

TEST_CASE ("a bypassed latent effect plays at once, and comes back without stale audio", "[limiter][bypass]")
{
    Effect limiter (nodetype::limiter, {});
    GraphNode model;
    model.type = nodetype::limiter;

    // Audio goes into its delay line, then it is bypassed.
    std::ignore = limiter.run (sine (440.0, 0.5, 4096));
    model.bypassed = true;
    limiter.node->configure (model);

    const auto input = sine (440.0, 0.25, 4096);
    const auto through = limiter.run (input).left;

    for (size_t i = 0; i < input.size(); ++i)
        REQUIRE (through[i] == input[i]);

    // Back on, the line holds nothing from before.
    model.bypassed = false;
    limiter.node->configure (model);
    const auto after = limiter.run (std::vector<float> (2048)).left;
    CHECK (peak (after, 0, after.size()) == 0.0f);
}

TEST_CASE ("the Limiter's gain has no steps", "[limiter][dsp]")
{
    Effect limiter (nodetype::limiter, { { "ceiling", -6.0f } });
    auto input = sine (100.0, 0.25, 12000);

    // The level doubles and more at once.
    for (size_t i = 6000; i < input.size(); ++i)
        input[i] *= 3.0f;

    const auto out = limiter.run (input).left;
    float largest = 0.0f;

    for (size_t i = 1; i < out.size(); ++i)
        largest = std::max (largest, std::abs (out[i] - out[i - 1]));

    CHECK (largest < 0.05f);
}

TEST_CASE ("the Limiter's channels share one gain", "[limiter][dsp]")
{
    Effect limiter (nodetype::limiter, { { "ceiling", -6.0f } });
    const auto loud = sine (440.0, 1.0, 12000);
    auto quiet = loud;

    for (auto& s : quiet)
        s *= 0.25f;

    const auto out = limiter.run (loud, quiet);

    for (size_t i = 6000; i < out.left.size(); ++i)
        REQUIRE_THAT (out.right[i], WithinAbs (out.left[i] * 0.25f, 1.0e-5));
}
