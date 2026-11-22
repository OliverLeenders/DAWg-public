#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/ModulationEffects.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <random>
#include <tuple>

// The Chorus, Flanger and Phaser (docs/design/modulation-effects.md, section 5).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::array modulationTypes { nodetype::chorus, nodetype::flanger, nodetype::phaser };
constexpr int largestBlock = 4096;
constexpr ProcessSpec largeSpec { sampleRate, largestBlock };

// Ticks per sample at the test tempo map's 120 bpm.
constexpr double ticksPerSample = 2.0 * static_cast<double> (ticksPerBeat) / sampleRate;

[[nodiscard]] std::vector<float> noise (int length, unsigned seed = 1, float amplitude = 0.5f)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> uniform (-amplitude, amplitude);
    std::vector<float> result (static_cast<size_t> (length));

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

[[nodiscard]] std::vector<float> tone (double frequency, int length, double amplitude = 0.5)
{
    std::vector<float> result (static_cast<size_t> (length));

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<float> (
            amplitude * std::sin (2.0 * std::numbers::pi * frequency * static_cast<double> (i) / sampleRate));

    return result;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

// One node, its parameters set before prepare(), run over whole buffers in blocks; optionally with a playing transport
// that starts at a tick.
struct Runner
{
    Runner (const char* type, const std::map<juce::String, float>& values, ProcessSpec runSpec = largeSpec)
        : node (NodeRegistry::builtin().create (type))
    {
        REQUIRE (node != nullptr);
        set (values);
        GraphNode model;
        model.type = type;
        node->configure (model);
        node->prepare (runSpec);
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

    [[nodiscard]] Stereo run (const std::vector<float>& left, const std::vector<float>& right, int block = 512,
                              std::optional<double> startTick = std::nullopt, bool guarded = false)
    {
        Stereo result { std::vector<float> (left.size()), std::vector<float> (left.size()) };
        const std::vector<float> silence (static_cast<size_t> (largestBlock));
        std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                        AudioInput { silence.data(), silence.data() });

        for (size_t offset = 0; offset < left.size(); offset += static_cast<size_t> (block))
        {
            const auto count = std::min (static_cast<size_t> (block), left.size() - offset);
            inputs[0] = { left.data() + offset, right.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { result.left.data() + offset,
                                                                     result.right.data() + offset } };
            Transport transport;

            if (startTick)
            {
                transport.playing = true;
                transport.startTick = *startTick + static_cast<double> (offset) * ticksPerSample;
                transport.endTick = transport.startTick + static_cast<double> (count) * ticksPerSample;
                transport.timeMap = TimeMap (constantTempo(), sampleRate);
            }

            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            context.transport = startTick ? &transport : nullptr;

            if (guarded)
            {
                const ScopedNoAllocation guard;
                node->process (context);
            }
            else
            {
                node->process (context);
            }
        }

        return result;
    }

    [[nodiscard]] Stereo run (const std::vector<float>& mono, int block = 512) { return run (mono, mono, block); }

    std::unique_ptr<Node> node;
};

[[nodiscard]] float peak (const std::vector<float>& signal, size_t from = 0)
{
    auto result = 0.0f;

    for (auto i = from; i < signal.size(); ++i)
        result = std::max (result, std::abs (signal[i]));

    return result;
}

[[nodiscard]] double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from)
{
    auto ab = 0.0;
    auto aa = 0.0;
    auto bb = 0.0;

    for (auto i = from; i < a.size(); ++i)
    {
        ab += static_cast<double> (a[i]) * b[i];
        aa += static_cast<double> (a[i]) * a[i];
        bb += static_cast<double> (b[i]) * b[i];
    }

    return ab / std::sqrt (std::max (aa * bb, 1.0e-30));
}

[[nodiscard]] double rmsDb (const std::vector<float>& signal, size_t from)
{
    auto sum = 0.0;

    for (auto i = from; i < signal.size(); ++i)
        sum += static_cast<double> (signal[i]) * signal[i];

    return 10.0 * std::log10 (std::max (sum / static_cast<double> (signal.size() - from), 1.0e-30));
}

// The largest sample-to-sample change.
[[nodiscard]] float largestStep (const std::vector<float>& signal, size_t from = 1)
{
    auto result = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < signal.size(); ++i)
        result = std::max (result, std::abs (signal[i] - signal[i - 1]));

    return result;
}
} // namespace

TEST_CASE ("the modulation effects are stereo effects with the LFO's parameters first", "[modfx]")
{
    const auto& registry = NodeRegistry::builtin();

    for (const auto* type : modulationTypes)
    {
        INFO (type);
        const auto* descriptor = registry.find (type);
        REQUIRE (descriptor != nullptr);
        CHECK (descriptor->category == NodeCategory::effect);
        CHECK (descriptor->outputs.size() == 1);
        CHECK (descriptor->inputs[0].type == PortType::audio);
        CHECK (nodetype::allowedInVoiceGraph (type));
        REQUIRE (descriptor->params.size() == 10);

        const std::array<const char*, 5> lfo { "sync", "rate", "cycle", "shape", "spread" };

        for (size_t i = 0; i < lfo.size(); ++i)
            CHECK (juce::String (descriptor->params[i].id) == lfo[i]);

        CHECK (juce::String (descriptor->params[9].id) == "mix");
        CHECK (descriptor->params[4].acceptsModulation());

        const auto described = toVar (*descriptor);
        CHECK (described["params"][4]["unit"].toString() == juce::String::fromUTF8 ("\xc2\xb0"));
    }

    const auto* flanger = registry.find (nodetype::flanger);
    const auto throughZero = flanger->indexOfParam ("throughZero");
    REQUIRE (throughZero >= 0);
    CHECK (flanger->params[static_cast<size_t> (throughZero)].changesLatency);
    CHECK_FALSE (flanger->params[static_cast<size_t> (throughZero)].acceptsAutomation());
}

TEST_CASE ("the LFO's shapes, its spread and its slew to the song position", "[modfx]")
{
    using modfx::LfoShape;
    CHECK_THAT (modfx::lfoValue (LfoShape::sine, 0.25), WithinAbs (1.0, 1e-12));
    CHECK_THAT (modfx::lfoValue (LfoShape::triangle, 0.0), WithinAbs (0.0, 1e-12));
    CHECK_THAT (modfx::lfoValue (LfoShape::triangle, 0.25), WithinAbs (1.0, 1e-12));
    CHECK_THAT (modfx::lfoValue (LfoShape::triangle, 0.5), WithinAbs (0.0, 1e-12));
    CHECK_THAT (modfx::lfoValue (LfoShape::triangle, 0.75), WithinAbs (-1.0, 1e-12));
    CHECK_THAT (modfx::lfoValue (LfoShape::triangle, 0.125), WithinAbs (0.5, 1e-12));

    modfx::StereoLfo lfo;
    lfo.prepare (sampleRate);
    lfo.setShapeNow (LfoShape::sine);

    for (int i = 0; i < 1000; ++i)
        lfo.advance (1.0 / 4800.0, -1.0);

    // The right channel reads the LFO Spread / 360 of a cycle further on.
    CHECK_THAT (lfo.phase(), WithinAbs (1000.0 / 4800.0, 1e-9));
    CHECK_THAT (lfo.value (0.25), WithinAbs (modfx::lfoValue (LfoShape::sine, lfo.phase() + 0.25), 1e-12));

    // A song position half a cycle away appears: the phase slews, never jumps, and has followed within 0.4 s.
    auto target = 0.7;
    auto previous = lfo.phase();
    auto largest = 0.0;

    for (int i = 0; i < 19200; ++i)
    {
        target += 1.0 / 4800.0;
        lfo.advance (1.0 / 4800.0, target - std::floor (target));
        largest = std::max (largest, std::abs (modfx::phaseDifference (lfo.phase(), previous)));
        previous = lfo.phase();
    }

    CHECK (largest < 0.02);
    CHECK (std::abs (modfx::phaseDifference (target, lfo.phase())) < 1.0e-3);

    // After a reset, a song position is taken at once.
    lfo.reset();
    lfo.advance (1.0 / 4800.0, 0.6);
    CHECK_THAT (lfo.phase(), WithinAbs (0.6, 1e-12));
}

TEST_CASE ("each channel goes through its own effect", "[modfx]")
{
    const auto input = noise (48000);
    const std::vector<float> silence (input.size());

    const std::map<const char*, std::map<juce::String, float>> settings {
        { nodetype::chorus, { { "feedback", 60.0f }, { "voices", 3.0f } } },
        { nodetype::flanger, { { "feedback", 80.0f } } },
        { nodetype::phaser, { { "feedback", 80.0f } } },
    };

    for (const auto& [type, values] : settings)
    {
        INFO (type);
        Runner runner (type, values);
        const auto out = runner.run (input, silence);
        CHECK (peak (out.left) > 0.1f);
        CHECK (peak (out.right) == 0.0f);
    }

    // And with throughZero on.
    Runner throughZero (nodetype::flanger, { { "throughZero", 1.0f } });
    const auto out = throughZero.run (silence, input);
    CHECK (peak (out.left) == 0.0f);
    CHECK (peak (out.right) > 0.1f);
}

TEST_CASE ("a mono input comes out wide, unless Spread is 0", "[modfx]")
{
    const auto input = noise (96000);

    for (const auto* type : modulationTypes)
    {
        INFO (type);
        Runner wide (type, { { "spread", 90.0f }, { "mix", 100.0f }, { "rate", 2.0f } });
        const auto out = wide.run (input);
        CHECK (correlation (out.left, out.right, 4800) < 0.9);

        Runner mono (type, { { "spread", 0.0f }, { "mix", 100.0f }, { "rate", 2.0f } });
        const auto same = mono.run (input);
        CHECK (same.left == same.right);
    }
}

TEST_CASE ("the right channel is the left one Spread later in the LFO's cycle", "[modfx]")
{
    // Synced to a quarter note at 120 bpm, the LFO follows the song position, so a second node started Spread / 360 of
    // a cycle later in the song has its left channel where the first one has its right.
    constexpr float quarterNote = 13.0f;
    REQUIRE (juce::String (lfoCycleChoices[13]) == "1/4");
    const auto input = noise (24000);

    for (const auto* type : modulationTypes)
    {
        for (const auto spread : { 45.0f, 90.0f, 180.0f })
        {
            INFO (type << " at " << spread << " degrees");
            const std::map<juce::String, float> values {
                { "sync", 1.0f }, { "cycle", quarterNote }, { "spread", spread }, { "depth", 80.0f }, { "mix", 100.0f }
            };
            Runner first (type, values);
            Runner second (type, values);
            const auto out = first.run (input, input, 512, 0.0);
            // The Chorus's right voices sit Spread / 360 of the way to the next left voice (two voices by default).
            const auto voices = juce::String (type) == nodetype::chorus ? 2.0 : 1.0;
            const auto shift = static_cast<double> (spread) / 360.0 / voices * static_cast<double> (ticksPerBeat);
            const auto later = second.run (input, input, 512, shift);
            auto difference = 0.0f;

            for (size_t i = 0; i < input.size(); ++i)
                difference = std::max (difference, std::abs (out.right[i] - later.left[i]));

            CHECK (difference < 1.0e-4f);
            CHECK (correlation (out.left, out.right, 2400) < 0.999);
        }
    }
}

TEST_CASE ("the modulation effects' output doesn't depend on the block size", "[modfx]")
{
    const auto left = noise (20000, 3);
    const auto right = noise (20000, 4);

    const std::map<const char*, std::map<juce::String, float>> settings {
        { nodetype::chorus, { { "feedback", 50.0f }, { "voices", 4.0f }, { "rate", 3.0f }, { "shape", 1.0f } } },
        { nodetype::flanger, { { "feedback", -70.0f }, { "rate", 2.0f } } },
        { nodetype::phaser, { { "feedback", 70.0f }, { "stages", 5.0f }, { "rate", 2.0f } } },
    };

    for (const auto& [type, values] : settings)
    {
        for (const auto synced : { false, true })
        {
            INFO (type << (synced ? " synced" : " free"));
            auto all = values;
            all["sync"] = synced ? 1.0f : 0.0f;
            const auto start = synced ? std::optional<double> (1234.0) : std::nullopt;
            Runner reference (type, all);
            const auto expected = reference.run (left, right, 512, start);

            for (const auto block : { 1, 17, 64, 333, 4096 })
            {
                INFO ("block " << block);
                Runner runner (type, all);
                const auto out = runner.run (left, right, block, start);

                if (synced)
                {
                    auto difference = 0.0f;

                    for (size_t i = 0; i < left.size(); ++i)
                        difference = std::max ({ difference, std::abs (out.left[i] - expected.left[i]),
                                                 std::abs (out.right[i] - expected.right[i]) });

                    CHECK (difference < 1.0e-5f);
                }
                else
                {
                    CHECK (out.left == expected.left);
                    CHECK (out.right == expected.right);
                }
            }
        }
    }
}

TEST_CASE ("the modulation effects stay bounded under feedback", "[modfx]")
{
    const auto seconds = 10 * static_cast<int> (sampleRate);
    const auto hot = noise (seconds, 7, 1.0f);
    const auto sine = tone (440.0, seconds, 1.0);

    std::vector<std::pair<const char*, std::map<juce::String, float>>> cases {
        { nodetype::chorus, { { "feedback", 80.0f }, { "voices", 1.0f }, { "depth", 100.0f }, { "rate", 10.0f } } },
        { nodetype::chorus, { { "feedback", 80.0f }, { "voices", 4.0f }, { "depth", 0.0f }, { "delay", 5.0f } } },
        { nodetype::flanger, { { "feedback", 95.0f }, { "depth", 100.0f }, { "rate", 10.0f } } },
        { nodetype::flanger, { { "feedback", -95.0f }, { "depth", 100.0f }, { "delay", 0.1f } } },
        { nodetype::flanger, { { "feedback", 95.0f }, { "throughZero", 1.0f }, { "depth", 100.0f } } },
    };

    for (const auto stages : { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f })
        for (const auto feedback : { -95.0f, 95.0f })
            cases.push_back (
                { nodetype::phaser,
                  { { "feedback", feedback }, { "stages", stages }, { "depth", 100.0f }, { "rate", 10.0f } } });

    for (const auto& [type, values] : cases)
    {
        for (const auto* input : { &hot, &sine })
        {
            INFO (type << " feedback " << values.at ("feedback"));
            Runner runner (type, values);
            const auto out = runner.run (*input);
            const auto loudest = std::max (peak (out.left), peak (out.right));
            CHECK (std::isfinite (loudest));
            CHECK (loudest < 6.0f);
        }
    }
}

TEST_CASE ("the modulation effects don't allocate while processing", "[modfx][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    const auto input = noise (16 * 512);

    for (const auto* type : modulationTypes)
    {
        INFO (type);
        Runner runner (type, { { "feedback", 50.0f } });
        const auto before = allocationViolationCount();
        (void) runner.run (input, input, 512, std::nullopt, true);
        runner.set ({ { "shape", 1.0f }, { "spread", 180.0f }, { "sync", 1.0f } });
        (void) runner.run (input, input, 512, 0.0, true);
        CHECK (allocationViolationCount() == before);
    }

    Runner chorus (nodetype::chorus, {});
    Runner phaser (nodetype::phaser, {});
    const auto before = allocationViolationCount();
    chorus.set ({ { "voices", 4.0f } });
    phaser.set ({ { "stages", 5.0f } });
    (void) chorus.run (input, input, 512, std::nullopt, true);
    (void) phaser.run (input, input, 512, std::nullopt, true);
    CHECK (allocationViolationCount() == before);
}

TEST_CASE ("at Depth 0 the Chorus and the Flanger are delays", "[modfx]")
{
    std::vector<float> impulse (4800);
    impulse[100] = 1.0f;

    Runner chorus (nodetype::chorus, { { "depth", 0.0f }, { "voices", 1.0f }, { "delay", 10.0f }, { "mix", 100.0f } });
    const auto chorused = chorus.run (impulse);
    CHECK_THAT (chorused.left[580], WithinAbs (1.0, 1e-6));
    CHECK_THAT (peak (chorused.left), WithinAbs (1.0, 1e-6));

    Runner flanger (nodetype::flanger,
                    { { "depth", 0.0f }, { "delay", 5.0f }, { "feedback", 50.0f }, { "mix", 100.0f } });
    const auto flanged = flanger.run (impulse);
    CHECK_THAT (flanged.left[340], WithinAbs (1.0, 1e-6));
    CHECK_THAT (flanged.left[580], WithinAbs (0.5, 1e-6));
    CHECK_THAT (flanged.left[820], WithinAbs (0.25, 1e-6));

    // The comb's first notch, at 1 / (2 d), with Mix 50 % and no feedback.
    Runner comb (nodetype::flanger, { { "depth", 0.0f }, { "delay", 2.0f }, { "feedback", 0.0f } });
    const auto notch = comb.run (tone (250.0, 9600));
    const auto open = Runner (nodetype::flanger, { { "depth", 0.0f }, { "delay", 2.0f }, { "feedback", 0.0f } })
                          .run (tone (500.0, 9600));
    CHECK (rmsDb (notch.left, 4800) < -60.0);
    CHECK (rmsDb (open.left, 4800) > -10.0);
}

TEST_CASE ("through-zero flanging reports its reference as latency", "[modfx]")
{
    Runner plain (nodetype::flanger, {});
    CHECK (plain.node->latencySamples() == 0);

    Runner throughZero (nodetype::flanger, { { "throughZero", 1.0f }, { "depth", 0.0f }, { "feedback", 0.0f } });
    CHECK (throughZero.node->latencySamples() == 240);

    // At the crossing (Depth 0 keeps it there), the swept tap is the reference: the output is the input 5 ms later.
    const auto input = noise (4800);
    const auto out = throughZero.run (input);

    for (size_t i = 240; i < input.size(); ++i)
        REQUIRE (out.left[i] == input[i - 240]);

    // Turned on later, through configure() as the compiler does.
    GraphNode model;
    model.type = nodetype::flanger;
    plain.set ({ { "throughZero", 1.0f } });
    plain.node->configure (model);
    CHECK (plain.node->latencySamples() == 240);
}

TEST_CASE ("the Phaser's notches sit where its stages turn the phase by half a cycle", "[modfx]")
{
    // N stages at f0 turn a frequency f by 2N atan(tan(pi f / fs) / tan(pi f0 / fs)); a notch where that is 180 degrees.
    constexpr double centre = 1000.0;
    const auto t = std::tan (std::numbers::pi * centre / sampleRate);

    for (const auto& [stages, choice] : { std::pair { 2, 0.0f }, std::pair { 4, 1.0f }, std::pair { 8, 3.0f } })
    {
        INFO (stages << " stages");
        const auto perStage = std::numbers::pi / static_cast<double> (stages);
        const auto notch = sampleRate / std::numbers::pi * std::atan (t * std::tan (perStage / 2.0));
        const std::map<juce::String, float> values {
            { "depth", 0.0f }, { "feedback", 0.0f }, { "centre", static_cast<float> (centre) }, { "stages", choice }
        };
        const auto atNotch = Runner (nodetype::phaser, values).run (tone (notch, 24000));
        const auto between = Runner (nodetype::phaser, values).run (tone (notch * 0.25, 24000));
        CHECK (rmsDb (atNotch.left, 12000) < -50.0);
        CHECK (rmsDb (between.left, 12000) > -12.0);
    }
}

TEST_CASE ("the modulation effects' controls move without clicks", "[modfx]")
{
    // A 200 Hz sine changes by at most 0.013 per sample; a control that jumped would step far more than the output
    // does when settled.
    const auto input = tone (200.0, 48000);
    const auto quiet = largestStep (input);

    const std::vector<std::tuple<const char*, const char*, float, float>> jumps {
        { nodetype::chorus, "delay", 5.0f, 40.0f },      { nodetype::chorus, "depth", 0.0f, 100.0f },
        { nodetype::chorus, "voices", 1.0f, 4.0f },      { nodetype::chorus, "mix", 0.0f, 100.0f },
        { nodetype::chorus, "shape", 0.0f, 1.0f },       { nodetype::chorus, "spread", 0.0f, 180.0f },
        { nodetype::flanger, "delay", 0.5f, 10.0f },     { nodetype::flanger, "feedback", -90.0f, 90.0f },
        { nodetype::flanger, "rate", 0.1f, 10.0f },      { nodetype::phaser, "stages", 0.0f, 5.0f },
        { nodetype::phaser, "centre", 100.0f, 5000.0f }, { nodetype::phaser, "depth", 0.0f, 100.0f },
    };

    for (const auto& [type, id, from, to] : jumps)
    {
        INFO (type << " " << id);
        Runner runner (type, { { id, from } });
        const std::vector<float> first (input.begin(), input.begin() + 24000);
        const std::vector<float> second (input.begin() + 24000, input.end());
        const auto before = runner.run (first);
        runner.set ({ { id, to } });
        const auto after = runner.run (second);
        std::vector<float> joined (before.left);
        joined.insert (joined.end(), after.left.begin(), after.left.end());
        // Measured against the steadiest of the settled output before and after, which feedback may make louder.
        const std::vector<float> settledBefore (joined.begin() + 12000, joined.begin() + 23800);
        const std::vector<float> settledAfter (joined.begin() + 36000, joined.end());
        const auto steady = std::max ({ quiet, largestStep (settledBefore), largestStep (settledAfter) });
        CHECK (largestStep (joined, 24000 - 200) < 1.5f * steady);
    }
}

TEST_CASE ("Mix 0 is the dry signal and bypass passes the input", "[modfx]")
{
    const auto input = noise (4800);

    for (const auto* type : modulationTypes)
    {
        INFO (type);
        Runner dry (type, { { "mix", 0.0f }, { "feedback", 50.0f } });
        CHECK (dry.run (input).left == input);

        Runner bypassed (type, { { "feedback", 50.0f } });
        GraphNode model;
        model.type = type;
        model.bypassed = true;
        bypassed.node->configure (model);
        const auto out = bypassed.run (input);
        CHECK (out.left == input);
        CHECK (out.right == input);
    }
}

TEST_CASE ("the modulation effects show the LFO's phase", "[modfx]")
{
    Runner chorus (nodetype::chorus, { { "rate", 1.0f } });
    (void) chorus.run (std::vector<float> (12000));
    CHECK_THAT (static_cast<double> (chorus.node->meters()["phase"]), WithinAbs (0.25, 1e-3));
}
