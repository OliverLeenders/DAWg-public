#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <numbers>
#include <random>

// The Gate and the Expander (docs/design/gate-expander.md, section 5).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int largestBlock = 4096;
constexpr ProcessSpec largeSpec { sampleRate, largestBlock };
constexpr int perMs = 48; // samples per millisecond at the test rate

[[nodiscard]] float db (double value)
{
    return static_cast<float> (std::pow (10.0, value / 20.0));
}

[[nodiscard]] double toDb (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

// A constant signal (DC) whose level changes at the given sample positions: { from, level dB }, in order.
[[nodiscard]] std::vector<float> steps (int length, const std::vector<std::pair<int, double>>& levels)
{
    std::vector<float> result (static_cast<size_t> (length), 0.0f);

    for (size_t i = 0; i < levels.size(); ++i)
    {
        const auto end = i + 1 < levels.size() ? levels[i + 1].first : length;

        for (auto s = levels[i].first; s < end; ++s)
            result[static_cast<size_t> (s)] = levels[i].second <= -200.0 ? 0.0f : db (levels[i].second);
    }

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

// Noise in bursts of different levels, for the comparisons that need the gate to open and close often.
[[nodiscard]] std::vector<float> bursts (int length, unsigned seed = 1)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    std::vector<float> result (static_cast<size_t> (length));
    auto level = 0.0f;

    for (size_t i = 0; i < result.size(); ++i)
    {
        if (i % 1500 == 0)
            level = db (-70.0 + 65.0 * (0.5 + 0.5 * static_cast<double> (uniform (random))));

        result[i] = level * uniform (random);
    }

    return result;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

// One node, its parameters set before prepare(), run over whole buffers in blocks, with an optional key.
struct Runner
{
    Runner (const char* type, const std::map<juce::String, float>& values)
        : node (NodeRegistry::builtin().create (type))
    {
        REQUIRE (node != nullptr);
        set (values);
        GraphNode model;
        model.type = type;
        node->configure (model);
        node->prepare (largeSpec);
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

    [[nodiscard]] Stereo run (const std::vector<float>& left, const std::vector<float>& right,
                              const std::vector<float>* keyLeft = nullptr, const std::vector<float>* keyRight = nullptr,
                              int block = 512, bool guarded = false)
    {
        Stereo result { std::vector<float> (left.size()), std::vector<float> (left.size()) };
        const std::vector<float> silence (static_cast<size_t> (largestBlock));
        std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                        AudioInput { silence.data(), silence.data() });

        for (size_t offset = 0; offset < left.size(); offset += static_cast<size_t> (block))
        {
            const auto count = std::min (static_cast<size_t> (block), left.size() - offset);
            inputs[0] = { left.data() + offset, right.data() + offset };

            if (keyLeft != nullptr)
                inputs[1] = { keyLeft->data() + offset, keyRight->data() + offset };

            const std::array<AudioOutput, 1> outputs { AudioOutput { result.left.data() + offset,
                                                                     result.right.data() + offset } };
            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;

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

    [[nodiscard]] Stereo run (const std::vector<float>& mono, int block = 512)
    {
        return run (mono, mono, nullptr, nullptr, block);
    }

    std::unique_ptr<Node> node;
};

// The gain a node applied at a sample of a DC input: output over input.
[[nodiscard]] double gainAt (const std::vector<float>& in, const std::vector<float>& out, int sample)
{
    const auto s = static_cast<size_t> (sample);
    return static_cast<double> (out[s]) / static_cast<double> (in[s]);
}

[[nodiscard]] double rmsDb (const std::vector<float>& signal, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (signal[i]) * signal[i];

    return 10.0 * std::log10 (std::max (sum / static_cast<double> (to - from), 1.0e-30));
}

// The first sample at or after from where the gain is below 1.
[[nodiscard]] int firstClosing (const std::vector<float>& in, const std::vector<float>& out, int from)
{
    for (auto s = from; s < static_cast<int> (in.size()); ++s)
        if (gainAt (in, out, s) < 1.0 - 1.0e-6)
            return s;

    return -1;
}
} // namespace

TEST_CASE ("the gate and expander describe themselves", "[gate][dsp]")
{
    for (const auto* type : { nodetype::gate, nodetype::expander })
    {
        INFO (type);
        const auto* descriptor = NodeRegistry::builtin().find (type);
        REQUIRE (descriptor != nullptr);
        CHECK (descriptor->category == NodeCategory::effect);
        REQUIRE (descriptor->inputs.size() >= 2);
        CHECK (descriptor->inputs[1].type == PortType::audio);

        const std::vector<juce::String> shared { "range",    "attack", "hold",      "release", "lookahead", "detection",
                                                 "detector", "keyHp",  "keyHpFreq", "keyLp",   "keyLpFreq", "listen" };

        for (size_t i = 0; i < shared.size(); ++i)
            CHECK (descriptor->indexOfParam (shared[i]) == static_cast<int> (i) + 2);

        const auto& lookahead = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("lookahead"))];
        CHECK (lookahead.changesLatency);
        CHECK_FALSE (lookahead.acceptsAutomation());
    }

    const auto* gate = NodeRegistry::builtin().find (nodetype::gate);
    CHECK (gate->indexOfParam ("hysteresis") == 1);
    CHECK (gate->params[2].defaultValue == -80.0f);
    const auto* expander = NodeRegistry::builtin().find (nodetype::expander);
    CHECK (expander->indexOfParam ("ratio") == 1);
    CHECK (expander->params[7].defaultValue == 1.0f); // RMS
}

TEST_CASE ("the gate opens at the threshold and closes below the hysteresis", "[gate][dsp]")
{
    constexpr int segment = 100 * perMs;
    const std::map<juce::String, float> fast {
        { "threshold", -40.0f }, { "attack", 0.01f }, { "hold", 0.0f }, { "release", 1.0f }
    };

    SECTION ("with 6 dB of hysteresis, a level between the thresholds keeps the state")
    {
        Runner gate (nodetype::gate, fast);
        const auto in = steps (5 * segment, { { 0, -50.0 },
                                              { segment, -35.0 },
                                              { 2 * segment, -43.0 },
                                              { 3 * segment, -50.0 },
                                              { 4 * segment, -43.0 } });
        const auto out = gate.run (in).left;
        const auto end = [&] (int i) { return gainAt (in, out, (i + 1) * segment - 1); };

        CHECK (end (0) == 0.0);                      // below: closed
        CHECK_THAT (end (1), WithinAbs (1.0, 1e-6)); // above the threshold: open
        CHECK_THAT (end (2), WithinAbs (1.0, 1e-6)); // between: stays open
        CHECK (end (3) == 0.0);                      // below the close threshold: closed
        CHECK (end (4) == 0.0);                      // between: stays closed
    }

    SECTION ("without hysteresis both thresholds are the same")
    {
        auto values = fast;
        values["hysteresis"] = 0.0f;
        Runner gate (nodetype::gate, values);
        const auto in = steps (3 * segment, { { 0, -39.0 }, { segment, -41.0 }, { 2 * segment, -39.0 } });
        const auto out = gate.run (in).left;

        CHECK_THAT (gainAt (in, out, segment - 1), WithinAbs (1.0, 1e-6));
        CHECK (gainAt (in, out, 2 * segment - 1) == 0.0);
        CHECK_THAT (gainAt (in, out, 3 * segment - 1), WithinAbs (1.0, 1e-6));
    }
}

TEST_CASE ("the gate holds before it releases", "[gate][dsp]")
{
    constexpr int hold = 50 * perMs;
    const std::map<juce::String, float> values {
        { "threshold", -40.0f }, { "attack", 0.01f }, { "hold", 50.0f }, { "release", 1.0f }
    };

    // When the peak follower (instant attack, 10 ms decay) falls below the close threshold, -46 dB.
    const auto decay = std::exp (-1.0 / (0.01 * sampleRate));
    const auto below = static_cast<int> (std::ceil (std::log (db (-46.0) / db (-20.0)) / std::log (decay)));

    SECTION ("for the hold time after the level falls below the close threshold")
    {
        Runner gate (nodetype::gate, values);
        const auto drop = 100 * perMs;
        const auto in = steps (400 * perMs, { { 0, -20.0 }, { drop, -60.0 } });
        const auto out = gate.run (in).left;
        const auto closing = firstClosing (in, out, drop);

        CHECK (std::abs (closing - (drop + below + hold)) <= 2);
    }

    SECTION ("and a level above the close threshold during the hold restarts it")
    {
        Runner gate (nodetype::gate, values);
        const auto drop = 100 * perMs;
        const auto burst = drop + below + hold / 2;
        const auto in =
            steps (400 * perMs, { { 0, -20.0 }, { drop, -60.0 }, { burst, -20.0 }, { burst + perMs, -60.0 } });
        const auto out = gate.run (in).left;
        const auto closing = firstClosing (in, out, drop);

        CHECK (std::abs (closing - (burst + perMs + below + hold)) <= 2);
    }
}

TEST_CASE ("the range is the gate's floor", "[gate][dsp]")
{
    const auto in = steps (200 * perMs, { { 0, -60.0 } });

    SECTION ("a range of -20 dB lets a tenth through")
    {
        Runner gate (nodetype::gate, { { "range", -20.0f } });
        const auto out = gate.run (in).left;
        CHECK_THAT (gainAt (in, out, 200 * perMs - 1), WithinAbs (0.1, 1e-5));
    }

    SECTION ("at -80 dB the gate is silent")
    {
        Runner gate (nodetype::gate, {});
        const auto out = gate.run (in).left;
        CHECK (out.back() == 0.0f);
    }

    SECTION ("at 0 dB it does nothing")
    {
        Runner gate (nodetype::gate, { { "range", 0.0f } });
        const auto out = gate.run (in).left;
        CHECK (out == in);
    }
}

TEST_CASE ("the gate's attack and release are the times to open and close fully", "[gate][dsp]")
{
    constexpr int start = 100 * perMs;

    SECTION ("attack: linear in amplitude from the floor to 1")
    {
        Runner gate (nodetype::gate, { { "attack", 10.0f } });
        const auto in = steps (200 * perMs, { { 0, -200.0 }, { start, -20.0 } });
        const auto out = gate.run (in).left;

        CHECK_THAT (gainAt (in, out, start), WithinAbs (1.0 / 480.0, 1e-5));
        CHECK_THAT (gainAt (in, out, start + 239), WithinAbs (0.5, 1e-4));
        CHECK_THAT (gainAt (in, out, start + 479), WithinAbs (1.0, 1e-5));
        CHECK_THAT (gainAt (in, out, start + 600), WithinAbs (1.0, 1e-6));
    }

    SECTION ("release: linear in dB from 0 dB to the range")
    {
        Runner gate (nodetype::gate,
                     { { "attack", 0.01f }, { "hold", 0.0f }, { "release", 100.0f }, { "range", -40.0f } });
        const auto in = steps (500 * perMs, { { 0, -20.0 }, { start, -70.0 } });
        const auto out = gate.run (in).left;
        const auto closing = firstClosing (in, out, start);
        REQUIRE (closing > start);

        CHECK_THAT (toDb (gainAt (in, out, closing + 50 * perMs - 1)), WithinAbs (-20.0, 0.1));
        CHECK_THAT (toDb (gainAt (in, out, closing + 100 * perMs - 1)), WithinAbs (-40.0, 0.1));
        CHECK_THAT (toDb (gainAt (in, out, closing + 150 * perMs)), WithinAbs (-40.0, 1e-3));
    }
}

TEST_CASE ("the expander's ratio and range", "[expander][dsp]")
{
    SECTION ("the curve")
    {
        CHECK (expanderGainChangeDb (-30.0f, -40.0f, 2.0f, -80.0f) == 0.0f);
        CHECK_THAT (expanderGainChangeDb (-50.0f, -40.0f, 2.0f, -80.0f), WithinAbs (-10.0, 1e-5));
        CHECK_THAT (expanderGainChangeDb (-60.0f, -40.0f, 3.0f, -80.0f), WithinAbs (-40.0, 1e-5));
        CHECK_THAT (expanderGainChangeDb (-60.0f, -40.0f, 4.0f, -30.0f), WithinAbs (-30.0, 1e-5));
        CHECK (expanderGainChangeDb (-60.0f, -40.0f, 1.0f, -80.0f) == 0.0f);

        // The knee: continuous at its edges, -(ratio - 1) * 6 / 8 dB at the threshold.
        CHECK_THAT (expanderGainChangeDb (-40.0f, -40.0f, 2.0f, -80.0f), WithinAbs (-0.75, 1e-5));
        CHECK_THAT (expanderGainChangeDb (-37.0f, -40.0f, 2.0f, -80.0f), WithinAbs (0.0, 1e-5));
        CHECK_THAT (expanderGainChangeDb (-43.0f, -40.0f, 2.0f, -80.0f), WithinAbs (-3.0, 1e-5));
    }

    SECTION ("steady levels below the threshold")
    {
        for (const auto& [ratio, level, range, expected] :
             { std::tuple { 2.0f, -50.0, -80.0f, -10.0 }, std::tuple { 2.0f, -60.0, -80.0f, -20.0 },
               std::tuple { 3.0f, -55.0, -80.0f, -30.0 }, std::tuple { 4.0f, -60.0, -30.0f, -30.0 },
               std::tuple { 2.0f, -30.0, -80.0f, 0.0 } })
        {
            INFO ("ratio " << ratio << ", level " << level);
            Runner expander (nodetype::expander,
                             { { "ratio", ratio }, { "range", range }, { "attack", 0.1f }, { "release", 5.0f } });
            const auto in = steps (300 * perMs, { { 0, level } });
            const auto out = expander.run (in).left;
            CHECK_THAT (toDb (gainAt (in, out, 300 * perMs - 1)), WithinAbs (expected, 0.01));
        }
    }
}

TEST_CASE ("the expander's attack and release are time constants", "[expander][dsp]")
{
    constexpr int start = 100 * perMs;
    Runner expander (nodetype::expander, { { "detection", 0.0f },
                                           { "ratio", 2.0f },
                                           { "range", -40.0f },
                                           { "attack", 10.0f },
                                           { "hold", 0.0f },
                                           { "release", 500.0f } });

    // From the resting gain (the range, for silence) towards 0 dB.
    const auto in = steps (2000 * perMs, { { 0, -200.0 }, { start, -20.0 }, { 1000 * perMs, -70.0 } });
    const auto out = expander.run (in).left;
    CHECK_THAT (toDb (gainAt (in, out, start + 10 * perMs - 1)), WithinAbs (-40.0 * std::exp (-1.0), 0.1));

    // Towards (2 - 1) * (-70 + 40) = -30 dB; the detector's own decay delays it a little.
    CHECK_THAT (toDb (gainAt (in, out, 1500 * perMs)), WithinAbs (-30.0 * (1.0 - std::exp (-1.0)), 1.5));
}

TEST_CASE ("the gate's hold applies to the expander too", "[expander][dsp]")
{
    Runner expander (nodetype::expander, { { "detection", 0.0f }, { "hold", 100.0f }, { "release", 1.0f } });
    const auto in = steps (400 * perMs, { { 0, -20.0 }, { 100 * perMs, -70.0 } });
    const auto out = expander.run (in).left;

    // The level falls at once (the peak follower takes a few ms), but the gain waits for the hold.
    CHECK_THAT (gainAt (in, out, 150 * perMs), WithinAbs (1.0, 1e-3));
    CHECK (toDb (gainAt (in, out, 300 * perMs)) < -20.0);
}

TEST_CASE ("lookahead delays the audio, so the gate is open for the first sample", "[gate][dsp]")
{
    Runner gate (nodetype::gate, { { "attack", 1.0f }, { "lookahead", 2.0f } }); // 1 ms
    CHECK (gate.node->latencySamples() == perMs);

    constexpr int start = 100 * perMs;
    const auto in = steps (200 * perMs, { { 0, -200.0 }, { start, -20.0 } });
    const auto out = gate.run (in).left;

    CHECK (out[static_cast<size_t> (start + perMs - 1)] == 0.0f);
    CHECK_THAT (out[static_cast<size_t> (start + perMs)], WithinAbs (db (-20.0), 1e-6));

    Runner none (nodetype::gate, {});
    CHECK (none.node->latencySamples() == 0);
}

TEST_CASE ("the gate and expander link the channels", "[gate][expander][dsp]")
{
    for (const auto* type : { nodetype::gate, nodetype::expander })
    {
        INFO (type);

        SECTION (std::string ("a loud left opens the right: ") + type)
        {
            Runner node (type, { { "detection", 0.0f } });
            const auto left = steps (200 * perMs, { { 0, -20.0 } });
            const auto right = steps (200 * perMs, { { 0, -70.0 } });
            const auto out = node.run (left, right);
            CHECK_THAT (gainAt (right, out.right, 200 * perMs - 1), WithinAbs (1.0, 1e-4));
        }

        SECTION (std::string ("and both channels get the same gain: ") + type)
        {
            Runner node (type, { { "range", -20.0f }, { "detection", 0.0f } });
            const auto left = steps (300 * perMs, { { 0, -60.0 } });
            const auto right = steps (300 * perMs, { { 0, -65.0 } });
            const auto out = node.run (left, right);
            const auto at = 300 * perMs - 1;
            CHECK_THAT (gainAt (left, out.left, at), WithinAbs (gainAt (right, out.right, at), 1e-6));
            CHECK (gainAt (left, out.left, at) < 0.2);
        }
    }
}

TEST_CASE ("the gate's sidechain", "[gate][dsp][sidechain]")
{
    constexpr int length = 300 * perMs;
    const auto audio = steps (length, { { 0, -60.0 } });
    const auto loud = steps (length, { { 0, -20.0 } });
    const std::vector<float> silent (static_cast<size_t> (length), 0.0f);

    SECTION ("keyed, the key opens and closes the gate")
    {
        Runner gate (nodetype::gate, { { "detector", 1.0f } });
        CHECK_THAT (gainAt (audio, gate.run (audio, audio, &loud, &loud).left, length - 1), WithinAbs (1.0, 1e-6));

        Runner shut (nodetype::gate, { { "detector", 1.0f } });
        const auto louder = steps (length, { { 0, -10.0 } });
        CHECK (shut.run (louder, louder, &silent, &silent).left.back() == 0.0f);
    }

    SECTION ("the key filters take out what is outside their band")
    {
        const auto low = tone (50.0, length, db (-20.0));
        const auto high = tone (5000.0, length, db (-20.0));

        Runner open (nodetype::gate, { { "detector", 1.0f } });
        CHECK_THAT (gainAt (audio, open.run (audio, audio, &low, &low).left, length - 1), WithinAbs (1.0, 1e-6));

        Runner hp (nodetype::gate, { { "detector", 1.0f }, { "keyHp", 1.0f }, { "keyHpFreq", 2000.0f } });
        CHECK (hp.run (audio, audio, &low, &low).left.back() == 0.0f);

        Runner lp (nodetype::gate, { { "detector", 1.0f }, { "keyLp", 1.0f }, { "keyLpFreq", 200.0f } });
        CHECK (lp.run (audio, audio, &high, &high).left.back() == 0.0f);
    }

    SECTION ("Listen plays the key")
    {
        const auto key = tone (440.0, length, 0.3);
        Runner gate (nodetype::gate, { { "detector", 1.0f }, { "listen", 1.0f } });
        CHECK (gate.run (audio, audio, &key, &key).left == key);
    }
}

TEST_CASE ("the compressor's key low-pass", "[sidechain][dsp]")
{
    const auto high = tone (5000.0, 100 * perMs, 0.5);
    Runner compressor (nodetype::compressor, { { "listen", 1.0f }, { "keyLp", 1.0f }, { "keyLpFreq", 200.0f } });
    const auto out = compressor.run (high).left;
    CHECK (rmsDb (out, 50 * perMs, out.size()) < rmsDb (high, 50 * perMs, high.size()) - 50.0);
}

TEST_CASE ("the gate and expander don't depend on the block size", "[gate][expander][dsp]")
{
    const auto left = bursts (40000, 1);
    const auto right = bursts (40000, 2);

    for (const auto* type : { nodetype::gate, nodetype::expander })
        for (const auto lookahead : { 0.0f, 3.0f })
        {
            INFO (type << ", lookahead " << lookahead);
            const std::map<juce::String, float> values {
                { "threshold", -30.0f }, { "hold", 5.0f }, { "release", 40.0f }, { "lookahead", lookahead }
            };
            Runner reference (type, values);
            const auto expected = reference.run (left, right, nullptr, nullptr, 4096);

            for (const auto block : { 1, 17, 64, 333 })
            {
                INFO ("block " << block);
                Runner runner (type, values);
                const auto result = runner.run (left, right, nullptr, nullptr, block);
                CHECK (result.left == expected.left);
                CHECK (result.right == expected.right);
            }
        }
}

TEST_CASE ("the gate and expander don't allocate while processing", "[gate][expander][dsp][realtime]")
{
    const auto in = bursts (20000);

    for (const auto* type : { nodetype::gate, nodetype::expander, nodetype::compressor })
    {
        INFO (type);
        Runner runner (type, { { "keyHp", 1.0f }, { "keyLp", 1.0f } });
        std::ignore = runner.run (in, in, &in, &in, 512, true);
        runner.set (
            { { "keyHpFreq", 300.0f }, { "keyLpFreq", 3000.0f }, { "detector", 1.0f }, { "threshold", -20.0f } });
        std::ignore = runner.run (in, in, &in, &in, 512, true);
    }
}

TEST_CASE ("the gate doesn't click", "[gate][dsp]")
{
    // A 1 kHz tone, gated by a key that comes and goes; the default attack and release.
    constexpr int length = 1000 * perMs;
    const auto in = tone (1000.0, length, 0.5);
    std::vector<float> key (static_cast<size_t> (length), 0.0f);

    for (size_t i = 0; i < key.size(); ++i)
        key[i] = (i / (100 * perMs)) % 2 == 0 ? 0.5f : 0.0f;

    Runner gate (nodetype::gate, { { "detector", 1.0f } });
    const auto out = gate.run (in, in, &key, &key).left;
    auto largestStep = 0.0f;

    for (size_t i = 1; i < out.size(); ++i)
        largestStep = std::max (largestStep, std::abs (out[i] - out[i - 1]));

    // The tone's own largest step is 2 pi 1000 / 48000 * 0.5 = 0.065; the 0.5 ms attack adds at most 0.5 / 24.
    CHECK (largestStep < 0.09f);
}
