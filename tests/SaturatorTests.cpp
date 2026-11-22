#include "EffectTestUtils.h"
#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <complex>
#include <map>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The Saturator (docs/design/saturator.md).

namespace
{
constexpr int settle = 48000; // one second before measuring, for the DC blocker and the oversampling filters

struct Saturator : EffectRunner
{
    explicit Saturator (const std::map<juce::String, float>& values)
        : EffectRunner (nodetype::saturator, values)
    {
    }
};

// tanh without anti-aliasing, with the Saturator's gain staging, for comparison.
[[nodiscard]] std::vector<float> naiveTanh (const std::vector<float>& input, float driveDb)
{
    const auto g = 0.1 * std::pow (10.0, static_cast<double> (driveDb) / 20.0);
    std::vector<float> result (input.size());

    for (size_t i = 0; i < input.size(); ++i)
        result[i] = static_cast<float> (std::tanh (g * static_cast<double> (input[i])) / g);

    return result;
}
} // namespace

TEST_CASE ("the Saturator aliases less than a naive tanh, and less again oversampled", "[saturator][dsp]")
{
    constexpr int bin = 2389; // about 7 kHz
    const auto input = sine (binFrequency (bin), 0.5, settle + fftSize);

    const auto naive = aliasDb (spectrum (naiveTanh (input, 40.0f)), bin);
    const auto adaa =
        aliasDb (spectrum (Saturator ({ { "drive", 40.0f }, { "oversampling", 0.0f } }).run (input)), bin);
    const auto twice =
        aliasDb (spectrum (Saturator ({ { "drive", 40.0f }, { "oversampling", 1.0f } }).run (input)), bin);
    const auto four =
        aliasDb (spectrum (Saturator ({ { "drive", 40.0f }, { "oversampling", 2.0f } }).run (input)), bin);

    INFO ("naive " << naive << " dB, ADAA " << adaa << " dB, 2x " << twice << " dB, 4x " << four << " dB");
    // Measured with transparent ADAA (docs/design/vicanek-adaa.md, section 10): naive -15.8 dB, ADAA -24.6 dB,
    // 2x -45.0 dB, 4x -90.6 dB (plain ADAA gave -28.0, -47.4 and -92.8 dB).
    CHECK (adaa < naive - 8.0);
    CHECK (twice < adaa - 15.0);
    CHECK (four < twice - 30.0);
}

TEST_CASE ("full bias puts the second harmonic level with the third around -20 dBFS", "[saturator][dsp]")
{
    // The measurements of docs/design/saturator.md, W3: H2 relative to H3 at full bias, which the drive barely moves.
    struct Case
    {
        float level;
        float driveDb;
        double expected;
    };

    constexpr int bin = 341; // about 1 kHz

    for (const auto oversampling : { 0.0f, 2.0f })
    {
        for (const auto& [level, driveDb, expected] :
             { Case { 0.0f, 0.0f, -24.4 }, Case { 0.0f, 20.0f, -25.5 }, Case { -20.0f, 0.0f, -4.4 },
               Case { -20.0f, 40.0f, -5.4 }, Case { -30.0f, 20.0f, 5.6 } })
        {
            INFO ("oversampling " << oversampling << ", " << level << " dBFS, drive " << driveDb << " dB");
            const auto input = sine (binFrequency (bin), static_cast<double> (dbToGain (level)), settle + fftSize);
            const auto magnitudes = spectrum (
                Saturator ({ { "drive", driveDb }, { "bias", 100.0f }, { "oversampling", oversampling } }).run (input));
            CHECK_THAT (levelDb (magnitudes, 2 * bin) - levelDb (magnitudes, 3 * bin), WithinAbs (expected, 1.0));
        }
    }
}

TEST_CASE ("the Saturator leaves no DC with bias", "[saturator][dsp]")
{
    constexpr int bin = 341;

    for (const auto level : { -20.0f, 0.0f })
    {
        for (const auto oversampling : { 0.0f, 1.0f })
        {
            INFO (level << " dBFS, oversampling " << oversampling);
            const auto input = sine (binFrequency (bin), static_cast<double> (dbToGain (level)), settle + fftSize);
            const auto output =
                Saturator ({ { "drive", 40.0f }, { "bias", 100.0f }, { "oversampling", oversampling } }).run (input);

            // A whole number of cycles, so the sine itself averages to zero.
            auto sum = 0.0;

            for (auto i = output.size() - fftSize; i < output.size(); ++i)
                sum += static_cast<double> (output[i]);

            CHECK (std::abs (sum / fftSize) < 1.0e-4); // -80 dBFS
        }
    }
}

TEST_CASE ("small signals pass the Saturator at unity gain, and Mix 0 % is the dry path", "[saturator][dsp]")
{
    const auto quiet = sine (1000.0, 0.001, 8192);

    for (const auto driveDb : { 0.0f, 12.0f, 40.0f })
    {
        INFO ("drive " << driveDb << " dB");
        const auto output = Saturator ({ { "drive", driveDb }, { "bias", 50.0f } }).run (quiet);
        auto peak = 0.0f;

        for (auto i = output.size() / 2; i < output.size(); ++i)
            peak = std::max (peak, std::abs (output[i]));

        CHECK_THAT (peak, WithinRel (0.001f, 0.02f));
    }

    // Mix 0 % is the dry path: the shaped path's linear response (docs/design/vicanek-adaa.md, V7).
    const auto loud = sine (1000.0, 0.8, 2048);
    const auto dry = Saturator ({ { "drive", 40.0f }, { "mix", 0.0f }, { "oversampling", 0.0f } }).run (loud);
    const auto expected = referenceLinear (loud);

    for (size_t i = 0; i < loud.size(); ++i)
        REQUIRE_THAT (dry[i], WithinAbs (static_cast<float> (expected[i]), 1.0e-6));
}

TEST_CASE ("moving drive and bias doesn't click", "[saturator][dsp]")
{
    const auto input = sine (100.0, 0.5, 4 * blockSize);
    Saturator saturator ({ { "drive", 0.0f }, { "bias", 0.0f }, { "oversampling", 0.0f } });

    std::vector<float> output;

    for (int block = 0; block < 4; ++block)
    {
        if (block == 2)
            saturator.set ({ { "drive", 40.0f }, { "bias", 100.0f } });

        const std::vector<float> slice (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
        const auto rendered = saturator.run (slice);
        output.insert (output.end(), rendered.begin(), rendered.end());
    }

    // The input moves by at most 2 pi 100 / 48000 * 0.5 = 0.0065 per sample; the shaper's slope is at most 1.
    auto step = 0.0f;

    for (size_t i = 1; i < output.size(); ++i)
        step = std::max (step, std::abs (output[i] - output[i - 1]));

    CHECK (step < 0.012f);
}

TEST_CASE ("the Saturator stays finite and clean for extreme input", "[saturator][dsp]")
{
    std::vector<float> extreme (4096);

    for (size_t i = 0; i < extreme.size(); ++i)
        extreme[i] = (i / 37) % 2 == 0 ? 1000.0f : -1000.0f;

    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        INFO ("oversampling " << oversampling);

        for (const auto sample :
             Saturator ({ { "drive", 40.0f }, { "bias", 100.0f }, { "oversampling", oversampling } }).run (extreme))
            REQUIRE (std::isfinite (sample));
    }

    const std::vector<float> tiny (4096, 1.0e-40f);

    for (const auto sample : Saturator ({ { "drive", 40.0f } }).run (tiny))
        REQUIRE (std::isfinite (sample));
}

TEST_CASE ("resetting the Saturator clears its history", "[saturator][dsp]")
{
    Saturator saturator ({ { "drive", 40.0f }, { "oversampling", 2.0f } });
    std::ignore = saturator.run (sine (3000.0, 0.9, 2048));
    saturator.node->reset();

    for (const auto sample : saturator.run (std::vector<float> (2048, 0.0f)))
        REQUIRE (sample == 0.0f);
}

TEST_CASE ("the Saturator reports the oversampling's latency", "[saturator][dsp]")
{
    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        INFO ("oversampling " << oversampling);
        Saturator saturator ({ { "drive", 0.0f }, { "oversampling", oversampling } });
        const auto latency = saturator.node->latencySamples();

        if (oversampling < 0.5f)
            CHECK (latency == 0);
        else
            CHECK (latency > 0);

        // A small impulse: ADAA spreads it over two samples, so the peak is at the latency or one after.
        std::vector<float> impulse (2048, 0.0f);
        impulse[100] = 0.001f;
        const auto output = saturator.run (impulse);
        const auto peak = std::ranges::max_element (output, {}, [] (float s) { return std::abs (s); }) - output.begin();
        CHECK (peak >= 100 + latency);
        CHECK (peak <= 101 + latency);
    }
}

TEST_CASE ("Saturator instances keep their own state", "[saturator][dsp]")
{
    // The plugin it comes from shared its history between instances; two nodes here must not.
    Saturator loud ({ { "drive", 40.0f } });
    Saturator silent ({ { "drive", 40.0f } });
    const auto tone = sine (440.0, 0.9, 1024);
    const std::vector<float> silence (1024, 0.0f);

    for (int round = 0; round < 4; ++round)
    {
        std::ignore = loud.run (tone);

        for (const auto sample : silent.run (silence))
            REQUIRE (sample == 0.0f);
    }
}

TEST_CASE ("the Saturator's panel curve is its static transfer curve", "[saturator][panel]")
{
    const auto curve = effectResponse (nodetype::saturator, { { "drive", 24.0 } }, sampleRate);
    const auto* x = curve["x"].getArray();
    const auto* y = curve["y"].getArray();
    REQUIRE (x != nullptr);
    REQUIRE (y != nullptr);
    REQUIRE (x->size() == y->size());
    CHECK_THAT (static_cast<double> ((*x)[0]), WithinAbs (-1.0, 1.0e-9));
    CHECK_THAT (static_cast<double> ((*x)[x->size() - 1]), WithinAbs (1.0, 1.0e-9));

    // Without bias the curve is odd and passes through 0; with it, it still passes through 0 but isn't odd.
    const auto middle = x->size() / 2;
    CHECK_THAT (static_cast<double> ((*y)[middle]), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (static_cast<double> ((*y)[0]), WithinAbs (-static_cast<double> ((*y)[y->size() - 1]), 1.0e-9));

    const auto biased = effectResponse (nodetype::saturator, { { "drive", 24.0 }, { "bias", 100.0 } }, sampleRate);
    CHECK_THAT (static_cast<double> (biased["y"][middle]), WithinAbs (0.0, 1.0e-9));
    CHECK (std::abs (static_cast<double> (biased["y"][0]) + static_cast<double> (biased["y"][y->size() - 1])) > 1.0e-3);

    // Mix 0 % is the identity, scaled by Output.
    const auto dry = effectResponse (nodetype::saturator, { { "mix", 0.0 }, { "output", -6.0 } }, sampleRate);
    CHECK_THAT (static_cast<double> (dry["y"][0]), WithinAbs (-std::pow (10.0, -6.0 / 20.0), 1.0e-4));
}

TEST_CASE ("changing the Saturator's oversampling recompiles with its new latency", "[saturator][compiler][latency]")
{
    // docs/design/saturator.md, W7: the compiler reads latencySamples() only when it compiles.
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::saturator } });
    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto nodeId =
        std::ranges::find_if (nodes, [] (const GraphNode& n) { return n.type == nodetype::saturator; })->id;
    const auto set = [&] (const char* param, double value)
    {
        project = apply (project, "setNodeParam",
                         { { "trackId", trackId }, { "nodeId", nodeId }, { "param", param }, { "value", value } });
    };

    GraphCompiler compiler (NodeRegistry::builtin());
    const auto first = compiler.compile (project, spec);
    const auto twice = first->tracks[0].latencySamples;
    CHECK (twice > 0);

    set ("oversampling", 0.0);
    const auto off = compiler.update (project, spec);
    REQUIRE (off != nullptr);
    CHECK (off->tracks[0].latencySamples == 0);

    set ("oversampling", 2.0);
    const auto four = compiler.update (project, spec);
    REQUIRE (four != nullptr);
    CHECK (four->tracks[0].latencySamples > twice);

    // Other parameters don't recompile.
    set ("drive", 30.0);
    CHECK (compiler.update (project, spec) == nullptr);
}

TEST_CASE ("the Saturator's oversampling can't be automated or modulated", "[saturator][automation]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::saturator);
    REQUIRE (descriptor != nullptr);
    const auto& oversampling = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("oversampling"))];
    CHECK (oversampling.changesLatency);
    CHECK_FALSE (oversampling.acceptsAutomation());
    CHECK_FALSE (oversampling.acceptsModulation());
    CHECK (descriptor->params[0].acceptsAutomation());

    const auto described = toVar (*descriptor);
    CHECK_FALSE (static_cast<bool> (described["params"][4]["automatable"]));
    CHECK (static_cast<bool> (described["params"][0]["automatable"]));

    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::saturator } });
    const auto nodeId = std::ranges::find_if (project.tracks[0]->graph.nodes,
                                              [] (const GraphNode& n) { return n.type == nodetype::saturator; })
                            ->id;
    const auto target = [nodeId] (const char* param)
    { return makeObject ({ { "kind", "node" }, { "nodeId", nodeId }, { "param", param } }); };

    CHECK_THROWS_AS (
        apply (project, "addAutomationLane", { { "trackId", trackId }, { "target", target ("oversampling") } }),
        CommandError);
    CHECK_NOTHROW (apply (project, "addAutomationLane", { { "trackId", trackId }, { "target", target ("drive") } }));
}

TEST_CASE ("the Saturator's meter reads the input level for the panel's dot", "[saturator][panel]")
{
    // The input's peak before Drive, which the curve is drawn against, at any drive and oversampling.
    for (const auto oversampling : { 0.0f, 1.0f })
    {
        INFO ("oversampling " << oversampling);
        Saturator saturator ({ { "drive", 30.0f }, { "oversampling", oversampling } });
        CHECK (static_cast<double> (saturator.node->meters()["input"]) == 0.0);
        std::ignore = saturator.run (sine (1000.0, 0.5, 4096));
        CHECK_THAT (static_cast<double> (saturator.node->meters()["input"]), WithinAbs (0.5, 1.0e-3));

        // It falls once the input stops, and reset clears it.
        std::ignore = saturator.run (std::vector<float> (24000, 0.0f));
        CHECK (static_cast<double> (saturator.node->meters()["input"]) < 0.001);
        std::ignore = saturator.run (sine (1000.0, 0.5, 4096));
        saturator.node->reset();
        CHECK (static_cast<double> (saturator.node->meters()["input"]) == 0.0);
    }
}
