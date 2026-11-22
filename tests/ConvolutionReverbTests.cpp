#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/AudioSource.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/Convolver.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <random>

// The Convolution Reverb node (docs/design/convolution-reverb.md, section 5): its response, Mix and bypass, the
// shaping controls, pre-delay, swaps, resampling, normalising, missing files, the default IR, the model and saving.

using namespace dawg;
using namespace dawg::test;

namespace
{
using Channels = std::array<std::vector<float>, 2>;

[[nodiscard]] std::vector<float> noise (size_t length, unsigned seed, float amplitude = 0.5f)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> uniform (-amplitude, amplitude);
    std::vector<float> result (length);

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

// A decaying noise IR.
[[nodiscard]] juce::AudioBuffer<float> impulse (int channels, int length, unsigned seed, float level = 1.0f)
{
    juce::AudioBuffer<float> ir (channels, length);

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto values = noise (static_cast<size_t> (length), seed + static_cast<unsigned> (ch), level);

        for (int i = 0; i < length; ++i)
            ir.setSample (ch, i,
                          values[static_cast<size_t> (i)]
                              * std::exp (-4.0f * static_cast<float> (i) / static_cast<float> (length)));
    }

    return ir;
}

[[nodiscard]] std::shared_ptr<const AudioSource> sourceOf (const juce::AudioBuffer<float>& ir, double rate = sampleRate)
{
    return AudioSource::fromBuffer (ir, rate);
}

// A Convolution Reverb node with its parameters, a file (or none), prepared and built.
struct Reverb
{
    explicit Reverb (const std::map<juce::String, float>& values, std::shared_ptr<const AudioSource> source = nullptr,
                     ObjectId fileId = 0)
        : node (NodeRegistry::builtin().create (nodetype::convolutionReverb))
    {
        REQUIRE (node != nullptr);
        set (values);
        node->prepare (spec);
        model.type = nodetype::convolutionReverb;
        model.fileId = fileId;

        if (fileId == 0)
            model.fileId = source != nullptr ? 1 : 0;
        node->configure (model);
        node->setAudioFile (std::move (source));
        waitForConvolutionBuilds();
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

    // Changes parameters as the panel does: set, then configure().
    void change (const std::map<juce::String, float>& values)
    {
        set (values);
        node->configure (model);
    }

    [[nodiscard]] Channels run (const Channels& input, int block = blockSize)
    {
        const auto length = input[0].size();
        Channels output { std::vector<float> (length), std::vector<float> (length) };
        const std::vector<float> silence (static_cast<size_t> (block));

        for (size_t offset = 0; offset < length; offset += static_cast<size_t> (block))
        {
            const auto count = std::min (static_cast<size_t> (block), length - offset);
            std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                            AudioInput { silence.data(), silence.data() });
            inputs[0] = { input[0].data() + offset, input[1].data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { output[0].data() + offset,
                                                                     output[1].data() + offset } };
            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            node->process (context);
        }

        return output;
    }

    std::unique_ptr<Node> node;
    GraphNode model;
};

[[nodiscard]] Channels impulseInput (size_t length, float level = 1.0f)
{
    Channels input { std::vector<float> (length), std::vector<float> (length) };
    input[0][0] = level;
    input[1][0] = level;
    return input;
}

// The wet path alone, as the file is: Mix 100 %, Normalise off, filters at the ends of their ranges.
const std::map<juce::String, float> plainWet { { "mix", 100.0f }, { "normalise", 0.0f } };

[[nodiscard]] double rms (const std::vector<float>& signal, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (signal[i]) * signal[i];

    return std::sqrt (sum / static_cast<double> (to - from));
}

[[nodiscard]] float largestStep (const std::vector<float>& signal, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < to; ++i)
        largest = std::max (largest, std::abs (signal[i] - signal[i - 1]));

    return largest;
}
} // namespace

TEST_CASE ("the Convolution Reverb is a built-in effect for track graphs only", "[convolution]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::convolutionReverb);
    REQUIRE (descriptor != nullptr);
    CHECK (descriptor->category == NodeCategory::effect);
    CHECK_FALSE (nodetype::allowedInVoiceGraph (nodetype::convolutionReverb));
    CHECK (nodetype::usesAudioFile (nodetype::convolutionReverb));

    // The shaping controls rebuild the IR: neither modulated nor automated (I14).
    for (const auto* id : { "start", "length", "decay", "reverse", "stretch", "normalise" })
    {
        const auto& info = descriptor->params[static_cast<size_t> (descriptor->indexOfParam (id))];
        CHECK_FALSE (info.acceptsModulation());
        CHECK_FALSE (info.acceptsAutomation());
    }

    for (const auto* id : { "lowCut", "highCut", "width", "gain", "mix" })
    {
        const auto& info = descriptor->params[static_cast<size_t> (descriptor->indexOfParam (id))];
        CHECK (info.acceptsModulation());
        CHECK (info.acceptsAutomation());
    }

    const auto& predelay = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("predelay"))];
    CHECK_FALSE (predelay.acceptsModulation());
    CHECK (predelay.acceptsAutomation());
}

TEST_CASE ("a unit impulse through the Convolution Reverb returns the IR, with no latency", "[convolution][dsp]")
{
    const auto ir = impulse (2, 20000, 1);
    Reverb reverb (plainWet, sourceOf (ir));
    CHECK (reverb.node->latencySamples() == 0);
    const auto output = reverb.run (impulseInput (24000));

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < ir.getNumSamples(); ++i)
            REQUIRE (output[static_cast<size_t> (ch)][static_cast<size_t> (i)]
                     == Catch::Approx (ir.getSample (ch, i)).margin (2.0e-5));
}

TEST_CASE ("the Convolution Reverb's Mix and bypass", "[convolution][dsp]")
{
    const auto ir = impulse (2, 5000, 2);
    const Channels input { noise (8000, 3), noise (8000, 4) };

    Reverb dry ({ { "mix", 0.0f } }, sourceOf (ir));
    CHECK (dry.run (input) == input);

    Reverb bypassed ({ { "mix", 100.0f } }, sourceOf (ir));
    bypassed.model.bypassed = true;
    bypassed.node->configure (bypassed.model);
    CHECK (bypassed.run (input) == input);

    // Half way: dry and wet at equal power.
    Reverb wet (plainWet, sourceOf (ir));
    const auto wetOnly = wet.run (input);
    Reverb half ({ { "mix", 50.0f }, { "normalise", 0.0f } }, sourceOf (ir));
    const auto mixed = half.run (input);
    const auto g = std::sqrt (0.5f);

    for (size_t i = 0; i < input[0].size(); ++i)
        REQUIRE (mixed[0][i] == Catch::Approx (g * input[0][i] + g * wetOnly[0][i]).margin (1.0e-5));
}

TEST_CASE ("the Convolution Reverb's response is the shaped IR", "[convolution][dsp]")
{
    const auto ir = impulse (2, 30000, 5);
    const std::vector<std::map<juce::String, float>> shapes {
        { { "start", 50.0f } },    { { "length", 40.0f } },
        { { "decay", 50.0f } },    { { "reverse", 1.0f }, { "length", 60.0f } },
        { { "stretch", 150.0f } },
    };

    for (const auto& shape : shapes)
    {
        auto values = plainWet;
        values.insert (shape.begin(), shape.end());
        Reverb reverb (values, sourceOf (ir));
        const auto output = reverb.run (impulseInput (50000));

        convolution::Shape expectedShape;
        expectedShape.normalise = false;
        expectedShape.startMs = values.contains ("start") ? values["start"] : 0.0;
        expectedShape.lengthPercent = values.contains ("length") ? values["length"] : 100.0;
        expectedShape.decayPercent = values.contains ("decay") ? values["decay"] : 100.0;
        expectedShape.reverse = values.contains ("reverse");
        expectedShape.stretchPercent = values.contains ("stretch") ? values["stretch"] : 100.0;
        const auto expected = convolution::shapeImpulse (ir, sampleRate, convolution::Layout::stereo, sampleRate,
                                                         expectedShape, convolution::estimateDecay (ir, sampleRate));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < expected.ir.getNumSamples(); ++i)
                REQUIRE (output[static_cast<size_t> (ch)][static_cast<size_t> (i)]
                         == Catch::Approx (expected.ir.getSample (ch, i)).margin (2.0e-5));
    }
}

TEST_CASE ("the Convolution Reverb's pre-delay delays the response, and moves without a click", "[convolution][dsp]")
{
    const auto ir = impulse (2, 4000, 6);
    auto values = plainWet;
    values["predelay"] = 10.0f;
    Reverb reverb (values, sourceOf (ir));
    const auto output = reverb.run (impulseInput (10000));

    // Nothing before it, but for the dry impulse at Mix 100 % (cos 90°, a float's −4e−8).
    for (size_t i = 0; i < 480; ++i)
        REQUIRE (std::abs (output[0][i]) < 1.0e-6f);

    CHECK (output[0][480] == Catch::Approx (ir.getSample (0, 0)).margin (1.0e-5));
    CHECK (output[0][1480] == Catch::Approx (ir.getSample (0, 1000)).margin (1.0e-5));

    // A sine while Pre-delay jumps from 10 to 200 ms: no step larger than the steady output's.
    Reverb moving (values, sourceOf (impulse (2, 2000, 7)));
    Channels sine { std::vector<float> (40000), std::vector<float> (40000) };

    for (size_t i = 0; i < sine[0].size(); ++i)
        sine[0][i] = sine[1][i] = static_cast<float> (
            0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * static_cast<double> (i) / sampleRate));

    const auto first = moving.run ({ std::vector<float> (sine[0].begin(), sine[0].begin() + 20000),
                                     std::vector<float> (sine[1].begin(), sine[1].begin() + 20000) });
    moving.set ({ { "predelay", 200.0f } });
    const auto second = moving.run ({ std::vector<float> (sine[0].begin() + 20000, sine[0].end()),
                                      std::vector<float> (sine[1].begin() + 20000, sine[1].end()) });
    std::vector<float> joined (first[0]);
    joined.insert (joined.end(), second[0].begin(), second[0].end());
    CHECK (largestStep (joined, 20000, 30000) < 1.5f * largestStep (joined, 10000, 20000));
}

TEST_CASE ("changing the Convolution Reverb's IR doesn't click", "[convolution][dsp]")
{
    const auto ir = impulse (2, 30000, 8);
    Reverb reverb (plainWet, sourceOf (ir));
    Channels sine { std::vector<float> (30000), std::vector<float> (30000) };

    for (size_t i = 0; i < sine[0].size(); ++i)
        sine[0][i] = sine[1][i] = static_cast<float> (
            0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * static_cast<double> (i) / sampleRate));

    std::vector<float> output;

    for (const auto& edit : std::vector<std::map<juce::String, float>> {
             {}, { { "start", 200.0f } }, { { "length", 20.0f } }, { { "reverse", 1.0f } } })
    {
        if (! edit.empty())
        {
            reverb.change (edit);
            waitForConvolutionBuilds();
        }

        const auto part = reverb.run (sine);
        output.insert (output.end(), part[0].begin(), part[0].end());
    }

    // The steady output's largest step, against the largest anywhere after the first IR settled.
    const auto steady = largestStep (output, 20000, 30000);
    CHECK (largestStep (output, 30000, output.size()) < 1.5f * steady);

    // A new file swaps the same way.
    reverb.model.fileId = 2;
    reverb.node->configure (reverb.model);
    reverb.node->setAudioFile (sourceOf (impulse (2, 15000, 9)));
    waitForConvolutionBuilds();
    const auto swapped = reverb.run (sine);
    std::vector<float> joined (output.end() - 10000, output.end());
    joined.insert (joined.end(), swapped[0].begin(), swapped[0].end());
    CHECK (largestStep (joined, 0, joined.size()) < 1.5f * steady);
}

TEST_CASE ("the Convolution Reverb resamples an IR from another rate", "[convolution][dsp]")
{
    // A 96 kHz IR at 48 kHz: with Normalise off, a unit impulse returns the IR at 48 kHz, twice as loud per sample.
    const auto ir = impulse (2, 40000, 10);
    Reverb reverb (plainWet, sourceOf (ir, 96000.0));
    const auto output = reverb.run (impulseInput (24000));
    auto expected = convolution::resample (ir, 0.5);
    expected.applyGain (2.0f);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < expected.getNumSamples(); ++i)
            REQUIRE (output[static_cast<size_t> (ch)][static_cast<size_t> (i)]
                     == Catch::Approx (expected.getSample (ch, i)).margin (2.0e-5));
}

TEST_CASE ("Normalise brings IRs of any level to the same loudness", "[convolution][dsp]")
{
    const Channels input { noise (48000, 11), noise (48000, 12) };
    std::vector<double> levels;

    for (const auto level : { 1.0f, 0.01f })
    {
        Reverb reverb ({ { "mix", 100.0f } }, sourceOf (impulse (2, 24000, 13, level)));
        const auto output = reverb.run (input);
        levels.push_back (rms (output[0], 24000, 48000));
    }

    CHECK (20.0 * std::log10 (levels[0] / levels[1]) == Catch::Approx (0.0).margin (1.0));

    // Unity: white noise comes out at the dry level, within a dB.
    CHECK (20.0 * std::log10 (levels[0] / rms (input[0], 24000, 48000)) == Catch::Approx (0.0).margin (1.0));
}

TEST_CASE ("a missing IR file leaves the wet path silent, and no file plays the default", "[convolution][dsp]")
{
    const Channels input { noise (20000, 14), noise (20000, 15) };

    Reverb missing ({ { "mix", 100.0f } }, nullptr, 7);
    const auto silent = missing.run (input);
    CHECK (rms (silent[0], 0, 20000) < 1.0e-6); // the dry path's float cos 90°
    CHECK (static_cast<bool> (missing.node->panelMeters()["missing"]));

    Reverb half ({ { "mix", 50.0f } }, nullptr, 7);
    const auto dry = half.run (input);
    CHECK (dry[0][100] == Catch::Approx (std::sqrt (0.5f) * input[0][100]));

    Reverb fallback ({ { "mix", 100.0f } });
    const auto wet = fallback.run (input);
    CHECK (rms (wet[0], 5000, 20000) > 0.05);
    const auto meters = fallback.node->panelMeters();
    CHECK_FALSE (static_cast<bool> (meters["missing"]));
    CHECK_FALSE (static_cast<bool> (meters["pending"]));
    CHECK (static_cast<double> (meters["decay"]) == Catch::Approx (2.0).epsilon (0.1));
    CHECK (meters["envelope"].size() > 100);
}

TEST_CASE ("resetting the Convolution Reverb silences its tail", "[convolution][dsp]")
{
    Reverb reverb ({ { "mix", 100.0f } });
    std::ignore = reverb.run ({ noise (10000, 16), noise (10000, 17) });
    reverb.node->reset();
    const auto after = reverb.run ({ std::vector<float> (5000), std::vector<float> (5000) });
    CHECK (rms (after[0], 0, 5000) == 0.0);
}

TEST_CASE ("the Convolution Reverb never allocates while processing", "[convolution][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    Reverb reverb ({ { "mix", 60.0f } }, sourceOf (impulse (4, 20000, 18)));
    const auto input = noise (40 * blockSize, 19);
    std::vector<float> left (input.size());
    std::vector<float> right (input.size());
    std::vector<float> modulation (static_cast<size_t> (blockSize), 300.0f);
    const std::array<ModulatedParam, 1> modulated { { { 7, modulation.data() } } };
    const std::vector<float> silence (static_cast<size_t> (blockSize));
    std::vector<AudioInput> inputs (reverb.node->descriptor().inputs.size(),
                                    AudioInput { silence.data(), silence.data() });
    const auto before = allocationViolationCount();

    for (size_t offset = 0; offset < input.size(); offset += blockSize)
    {
        // A new IR half way, so a swap and a larger history happen while processing.
        if (offset == 20 * blockSize)
        {
            reverb.model.fileId = 2;
            reverb.node->configure (reverb.model);
            reverb.node->setAudioFile (sourceOf (impulse (2, 60000, 20)));
            waitForConvolutionBuilds();
        }

        inputs[0] = { input.data() + offset, input.data() + offset };
        const std::array<AudioOutput, 1> outputs { AudioOutput { left.data() + offset, right.data() + offset } };
        ProcessContext context;
        context.numSamples = blockSize;
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        context.modulatedParams = modulated;
        reverb.node->params().set (0, static_cast<float> (offset % 100)); // pre-delay

        const ScopedNoAllocation guard;
        reverb.node->process (context);
    }

    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("an impulse response is loaded into the pool, compiled, and saved with the project",
           "[convolution][commands]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::convolutionReverb } });
    const auto& graph = project.tracks[0]->graph;
    const auto node =
        std::ranges::find_if (graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::convolutionReverb; });
    REQUIRE (node != graph.nodes.end());
    const auto nodeId = node->id;
    CHECK (node->fileId == 0);

    const auto file = makeObject ({ { "path", "Audio/hall.wav" },
                                    { "name", "hall" },
                                    { "sampleRate", 48000.0 },
                                    { "channels", 4 },
                                    { "frames", 96000 } });
    project = apply (project, "setNodeFile", { { "trackId", trackId }, { "nodeId", nodeId }, { "file", file } });
    REQUIRE (project.audioFiles.size() == 1);
    const auto fileId = project.audioFiles[0].id;
    const auto findNode = [nodeId] (const Project& p) -> const GraphNode&
    {
        const auto& g = p.tracks[0]->graph;
        return g.nodes[static_cast<size_t> (g.indexOfNode (nodeId))];
    };
    CHECK (findNode (project).fileId == fileId);

    // Loading the same file again pools it once.
    project = apply (project, "setNodeFile", { { "trackId", trackId }, { "nodeId", nodeId }, { "file", file } });
    CHECK (project.audioFiles.size() == 1);

    // Saved and loaded with the project.
    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto saved = projectfile::toVar (project, { "abc-id", "0.1.0" }, states);
    const auto loaded = projectfile::fromVar (saved, [] (const juce::String&) { return nullptr; });
    CHECK (findNode (loaded.project).fileId == fileId);
    CHECK (loaded.project.audioFiles.size() == 1);

    // Removed, and refused on other nodes or for a file that isn't in the pool.
    const auto removed =
        apply (project, "setNodeFile", { { "trackId", trackId }, { "nodeId", nodeId }, { "fileId", 0 } });
    CHECK (findNode (removed).fileId == 0);
    CHECK_THROWS_AS (applyCommand (project, "setNodeFile",
                                   makeObject ({ { "trackId", trackId }, { "nodeId", nodeId }, { "fileId", 999 } })),
                     CommandError);

    for (const auto& other : project.tracks[0]->graph.nodes)
        if (other.type == nodetype::trackOutput)
            CHECK_THROWS_AS (
                applyCommand (project, "setNodeFile",
                              makeObject ({ { "trackId", trackId }, { "nodeId", other.id }, { "fileId", 0 } })),
                CommandError);

    // The compiler hands the node its file, and a new file without recompiling.
    GraphCompiler compiler;
    std::vector<std::shared_ptr<const AudioSource>> opened;
    compiler.setAudioSourceResolver (
        [&opened] (const AudioFile& f)
        {
            auto source = AudioSource::fromBuffer (impulse (f.channels, 1000, 21), f.sampleRate);
            opened.push_back (source);
            return source;
        });
    REQUIRE (compiler.compile (project, spec) != nullptr);
    CHECK (opened.size() == 1);

    auto relinked = apply (project, "setNodeFile",
                           { { "trackId", trackId },
                             { "nodeId", nodeId },
                             { "file", makeObject ({ { "path", "Audio/plate.wav" },
                                                     { "name", "plate" },
                                                     { "sampleRate", 44100.0 },
                                                     { "channels", 2 },
                                                     { "frames", 44100 } }) } });
    // A new pool file changes the pool, which recompiles; picking a file already in the pool doesn't.
    REQUIRE (compiler.compile (relinked, spec) != nullptr);
    CHECK (opened.size() == 2);
    const auto back =
        apply (relinked, "setNodeFile", { { "trackId", trackId }, { "nodeId", nodeId }, { "fileId", fileId } });
    CHECK (compiler.update (back, spec) == nullptr);
    CHECK (opened.size() == 3);
    waitForConvolutionBuilds();
}
