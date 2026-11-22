#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <tuple>

// The Bit Crusher (docs/design/bit-crusher.md, section 5).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int largestBlock = 4096;
constexpr ProcessSpec largeSpec { sampleRate, largestBlock };

// The settings at which every section is off (B12).
const std::map<juce::String, float> transparent { { "bits", bitcrusher::maxBits }, { "rate", 192000.0f } };

[[nodiscard]] std::vector<float> noise (int length, unsigned seed = 1, float amplitude = 0.5f)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> uniform (-amplitude, amplitude);
    std::vector<float> result (static_cast<size_t> (length));

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

// A signal whose every sample differs from its neighbours, so a held value is recognisable: the output equals the
// input exactly where the hold took a sample.
[[nodiscard]] std::vector<float> distinct (int length, int from = 0)
{
    std::vector<float> result (static_cast<size_t> (length));

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<float> ((static_cast<int> (i) + from) % 500000) * 1.0e-6f - 0.25f;

    return result;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

// One Bit Crusher, its parameters set before prepare(), run over whole buffers in blocks.
struct Runner
{
    explicit Runner (const std::map<juce::String, float>& values)
        : node (NodeRegistry::builtin().create (nodetype::bitcrusher))
    {
        REQUIRE (node != nullptr);
        set (values);
        GraphNode model;
        model.type = nodetype::bitcrusher;
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

    // Runs both channels in blocks; modulation, when given, is the per-sample value of one parameter.
    [[nodiscard]] Stereo run (const std::vector<float>& left, const std::vector<float>& right, int block = 512,
                              bool guarded = false,
                              std::optional<std::pair<juce::String, std::vector<float>>> modulation = std::nullopt)
    {
        Stereo result { std::vector<float> (left.size()), std::vector<float> (left.size()) };

        for (size_t offset = 0; offset < left.size(); offset += static_cast<size_t> (block))
        {
            const auto count = std::min (static_cast<size_t> (block), left.size() - offset);
            std::vector<AudioInput> inputs (node->descriptor().inputs.size(), AudioInput { nullptr, nullptr });
            inputs[0] = { left.data() + offset, right.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { result.left.data() + offset,
                                                                     result.right.data() + offset } };
            std::vector<ModulatedParam> modulated;

            if (modulation)
                modulated.push_back ({ static_cast<size_t> (node->descriptor().indexOfParam (modulation->first)),
                                       modulation->second.data() + offset });

            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            context.modulatedParams = modulated;

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

    [[nodiscard]] std::vector<float> run (const std::vector<float>& mono, int block = 512)
    {
        return run (mono, mono, block).left;
    }

    std::unique_ptr<Node> node;
};

// The samples at which the hold took a new value, for a distinct() input with the quantiser off.
[[nodiscard]] std::vector<size_t> takes (const std::vector<float>& input, const std::vector<float>& output)
{
    std::vector<size_t> result;

    for (size_t i = 0; i < input.size(); ++i)
        if (output[i] == input[i])
            result.push_back (i);

    return result;
}

[[nodiscard]] std::set<size_t> holdLengths (const std::vector<size_t>& at)
{
    std::set<size_t> result;

    for (size_t i = 1; i < at.size(); ++i)
        result.insert (at[i] - at[i - 1]);

    return result;
}

// The quantiser's output before the DC blocker, recovered from the node's output: the blocker is y = q - q1 + c y1,
// so q = y - c y1 + q1, from the silence a fresh node starts in.
[[nodiscard]] std::vector<double> unblock (const std::vector<float>& output)
{
    const auto c = std::exp (-2.0 * juce::MathConstants<double>::pi * 5.0 / sampleRate);
    std::vector<double> result (output.size());
    auto q1 = 0.0;
    auto y1 = 0.0;

    for (size_t i = 0; i < output.size(); ++i)
    {
        const auto y = static_cast<double> (output[i]);
        result[i] = y - c * y1 + q1;
        q1 = result[i];
        y1 = y;
    }

    return result;
}

// The distinct values of a signal, to a tolerance.
[[nodiscard]] std::vector<double> levels (const std::vector<double>& values, double tolerance)
{
    auto sorted = values;
    std::ranges::sort (sorted);
    std::vector<double> result;

    for (const auto v : sorted)
        if (result.empty() || v - result.back() > tolerance)
            result.push_back (v);

    return result;
}
} // namespace

TEST_CASE ("the Bit Crusher describes itself", "[bitcrusher]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::bitcrusher);
    REQUIRE (descriptor != nullptr);
    CHECK (descriptor->category == NodeCategory::effect);
    CHECK (juce::String (descriptor->displayName) == "Bit Crusher");
    CHECK (descriptor->inputs[0].type == PortType::audio);
    CHECK (descriptor->outputs.size() == 1);
    CHECK (nodetype::allowedInVoiceGraph (nodetype::bitcrusher));

    const std::vector<juce::String> ids { "bits",   "quantiser", "rateMode", "rate", "note", "jitter", "antiAlias",
                                          "smooth", "dither",    "drive",    "clip", "bias", "mix",    "output" };
    REQUIRE (descriptor->params.size() == ids.size());

    for (size_t i = 0; i < ids.size(); ++i)
        CHECK (descriptor->indexOfParam (ids[i]) == static_cast<int> (i));

    CHECK (descriptor->params[0].defaultValue == 8.0f);
    CHECK (descriptor->params[0].maxValue == bitcrusher::maxBits);
    CHECK (descriptor->params[3].defaultValue == 11000.0f);
    CHECK (descriptor->params[4].maxValue - descriptor->params[4].minValue == 240.0f); // tracks Pitch at 100 %

    const Runner runner ({});
    CHECK (runner.node->latencySamples() == 0);
}

TEST_CASE ("the quantiser has 2^bits steps of 2^(1 - bits)", "[bitcrusher]")
{
    // A full-scale ramp, beyond +-1 at both ends.
    std::vector<double> ramp;

    for (int i = -120000; i <= 120000; ++i)
        ramp.push_back (static_cast<double> (i) / 100000.0);

    const auto count = [&] (double bits, bool midRise)
    {
        std::vector<double> out;

        for (const auto x : ramp)
            out.push_back (bitcrusher::quantise (x, bits, midRise));

        return levels (out, 1.0e-9);
    };

    for (const auto bits : { 1, 2, 4, 8 })
    {
        INFO (bits);
        const auto step = std::exp2 (1.0 - bits);
        const auto tread = count (bits, false);
        const auto rise = count (bits, true);
        CHECK (tread.size() == (size_t { 1 } << bits) + 1);
        CHECK (rise.size() == (size_t { 1 } << bits));
        CHECK_THAT (tread.front(), WithinAbs (-1.0, 1.0e-12));
        CHECK_THAT (tread.back(), WithinAbs (1.0, 1.0e-12));
        CHECK_THAT (rise.back(), WithinAbs (1.0 - step / 2.0, 1.0e-12));

        for (size_t i = 1; i < tread.size(); ++i)
            CHECK_THAT (tread[i] - tread[i - 1], WithinAbs (step, 1.0e-12));

        for (size_t i = 1; i < rise.size(); ++i)
            CHECK_THAT (rise[i] - rise[i - 1], WithinAbs (step, 1.0e-12));
    }

    // Mid-tread keeps silence silent; mid-rise never outputs 0.
    CHECK (bitcrusher::quantise (0.0, 4.0, false) == 0.0);
    CHECK (bitcrusher::quantise (0.0, 4.0, true) == 0.0625);
    CHECK (bitcrusher::quantise (-1.0e-9, 1.0, true) == -0.5);

    // A fractional depth lies between its neighbours: its step is 2^(1 - bits), its level count between theirs.
    const auto between = count (4.5, false);
    CHECK (between.size() > count (4.0, false).size());
    CHECK (between.size() < count (5.0, false).size());
    CHECK_THAT (between[between.size() / 2 + 1] - between[between.size() / 2], WithinAbs (std::exp2 (-3.5), 1.0e-12));

    // At the top of the range the input passes unchanged.
    CHECK (bitcrusher::quantise (0.123456789, bitcrusher::maxBits, false) == 0.123456789);
    CHECK (bitcrusher::quantise (1.7, bitcrusher::maxBits, true) == 1.7);
}

TEST_CASE ("the node quantises to the depth's levels", "[bitcrusher]")
{
    // At the full rate, a slow ramp through the node, with the DC blocker undone.
    std::vector<float> ramp (96000);

    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = -1.0f + 2.0f * static_cast<float> (i) / static_cast<float> (ramp.size() - 1);

    for (const auto bits : { 2.0f, 3.0f, 5.0f })
        for (const auto midRise : { 0.0f, 1.0f })
        {
            INFO ("bits " << bits << ", mid-rise " << midRise);
            Runner runner ({ { "bits", bits }, { "rate", 192000.0f }, { "quantiser", midRise } });
            const auto quantised = unblock (runner.run (ramp));

            for (size_t i = 0; i < ramp.size(); i += 97)
                REQUIRE_THAT (
                    quantised[i],
                    WithinAbs (bitcrusher::quantise (static_cast<double> (ramp[i]), bits, midRise > 0.5f), 1.0e-4));

            const auto expected = (1u << static_cast<unsigned> (bits)) + (midRise > 0.5f ? 0u : 1u);
            CHECK (levels (quantised, std::exp2 (-static_cast<double> (bits))).size() == expected);
        }
}

TEST_CASE ("the hold lasts sampleRate / rate samples", "[bitcrusher]")
{
    const auto input = distinct (48000);

    SECTION ("a whole ratio, also where rate / sampleRate isn't exact in binary")
    {
        for (const auto ratio : { 4, 3, 10, 7 })
        {
            INFO (ratio);
            Runner runner ({ { "bits", bitcrusher::maxBits }, { "rate", 48000.0f / static_cast<float> (ratio) } });
            const auto at = takes (input, runner.run (input));
            REQUIRE (! at.empty());
            CHECK (at.front() == 0);
            CHECK (holdLengths (at) == std::set<size_t> { static_cast<size_t> (ratio) });
        }
    }

    SECTION ("a fractional ratio alternates its neighbours in a fixed pattern")
    {
        Runner runner ({ { "bits", bitcrusher::maxBits }, { "rate", 19200.0f } }); // 2.5
        const auto at = takes (input, runner.run (input));
        CHECK (holdLengths (at) == std::set<size_t> { 2, 3 });

        for (size_t i = 2; i < at.size(); ++i)
            REQUIRE (at[i] - at[i - 2] == 5);
    }

    SECTION ("the Note mode holds at the note's frequency")
    {
        for (const auto note : { 0.0f, 12.0f, 31.0f })
        {
            INFO (note);
            Runner runner ({ { "bits", bitcrusher::maxBits }, { "rateMode", 1.0f }, { "note", note } });
            const auto at = takes (input, runner.run (input));
            const auto expected = bitcrusher::noteRate (static_cast<double> (note));
            CHECK (std::abs (static_cast<double> (at.size()) - expected) <= 1.0);
        }
    }

    SECTION ("at or above the sample rate every sample is taken")
    {
        Runner runner ({ { "bits", bitcrusher::maxBits }, { "rate", 48000.0f } });
        CHECK (takes (input, runner.run (input)).size() == input.size());
    }
}

TEST_CASE ("a fractional ratio stays exact over a long run", "[bitcrusher]")
{
    // 11025 Hz at 48 kHz: a ratio of 4.3537..., over 20 million samples (seven minutes), streamed in blocks.
    constexpr int64_t length = 20'000'000;
    constexpr int block = largestBlock;
    Runner runner ({ { "bits", bitcrusher::maxBits }, { "rate", 11025.0f } });

    int64_t count = 0;
    int64_t last = -1;
    std::set<int64_t> lengths;

    for (int64_t offset = 0; offset < length; offset += block)
    {
        const auto input = distinct (block, static_cast<int> (offset % 500000));
        const auto output = runner.run (input, block);

        for (size_t i = 0; i < input.size(); ++i)
            if (output[i] == input[i])
            {
                const auto at = offset + static_cast<int64_t> (i);

                if (last >= 0)
                    lengths.insert (at - last);

                last = at;
                ++count;
            }
    }

    const auto samples = static_cast<double> ((length + block - 1) / block * block);
    CHECK (std::abs (static_cast<double> (count) - samples * 11025.0 / sampleRate) <= 1.0);
    CHECK (lengths == std::set<int64_t> { 4, 5 });
}

TEST_CASE ("with every section off the input passes unchanged", "[bitcrusher]")
{
    // Peaks beyond full scale too: nothing clips while the quantiser is off.
    const auto left = noise (20000, 1, 1.5f);
    const auto right = noise (20000, 2, 1.5f);
    Runner runner (transparent);
    const auto result = runner.run (left, right, 333);
    CHECK (result.left == left);
    CHECK (result.right == right);

    // Mix 0 is the dry signal at any setting.
    Runner dry ({ { "bits", 3.0f }, { "rate", 3000.0f }, { "mix", 0.0f }, { "smooth", 1.0f } });
    CHECK (dry.run (left, right).left == left);
}

TEST_CASE ("the channels are crushed separately at the same instants", "[bitcrusher]")
{
    const auto left = noise (24000, 3);
    const auto right = noise (24000, 4, 0.2f);
    const std::map<juce::String, float> values {
        { "bits", 5.0f }, { "rate", 7000.0f }, { "smooth", 1.0f }, { "antiAlias", 1.0f }, { "bias", 30.0f }
    };

    Runner stereo (values);
    const auto both = stereo.run (left, right);
    Runner leftOnly (values);
    Runner rightOnly (values);
    CHECK (both.left == leftOnly.run (left));
    CHECK (both.right == rightOnly.run (right));

    // The same hold instants for both channels.
    const auto a = distinct (24000);
    std::vector<float> b (a.size());

    for (size_t i = 0; i < a.size(); ++i)
        b[i] = -a[i];

    Runner held ({ { "bits", bitcrusher::maxBits }, { "rate", 7000.0f } });
    const auto heldBoth = held.run (a, b);
    CHECK (takes (a, heldBoth.left) == takes (b, heldBoth.right));
}

TEST_CASE ("jitter varies the holds by up to half a period, repeatably", "[bitcrusher]")
{
    const auto input = distinct (48000);
    const std::map<juce::String, float> values { { "bits", bitcrusher::maxBits },
                                                 { "rate", 4800.0f },
                                                 { "jitter", 100.0f } }; // a period of 10 samples

    Runner first (values);
    const auto output = first.run (input);
    const auto lengths = holdLengths (takes (input, output));
    CHECK (lengths.size() > 5);
    CHECK (*lengths.begin() >= 5);
    CHECK (*lengths.rbegin() <= 16);

    Runner second (values);
    CHECK (second.run (input) == output);
}

TEST_CASE ("the Bit Crusher leaves no DC", "[bitcrusher]")
{
    // Mid-rise at 2 bits turns silence into a constant quarter; bias moves it further. The blocker takes both away.
    const std::vector<float> silence (96000, 0.0f);
    Runner runner ({ { "bits", 2.0f }, { "quantiser", 1.0f }, { "bias", 60.0f } });
    const auto output = runner.run (silence);
    CHECK (std::abs (output.back()) < 1.0e-3f);
    CHECK (std::abs (output.front()) > 0.1f);
}

TEST_CASE ("the Bit Crusher's output doesn't depend on the block size", "[bitcrusher]")
{
    const auto left = noise (30000, 5);
    const auto right = noise (30000, 6);
    const std::map<juce::String, float> values { { "bits", 4.3f },   { "jitter", 30.0f }, { "antiAlias", 1.0f },
                                                 { "smooth", 1.0f }, { "dither", 1.0f },  { "clip", 1.0f },
                                                 { "drive", 6.0f },  { "bias", -40.0f },  { "mix", 70.0f } };

    // The rate sweeps from 2 kHz to 30 kHz and back through modulation, so the filters follow it.
    std::vector<float> rate (left.size());

    for (size_t i = 0; i < rate.size(); ++i)
        rate[i] =
            2000.0f
            + 28000.0f * std::sin (static_cast<float> (i) * 0.0002f) * std::sin (static_cast<float> (i) * 0.0002f);

    std::optional<Stereo> reference;

    for (const auto block : { 1, 17, 64, 333, 4096 })
    {
        INFO (block);
        Runner runner (values);
        const auto result = runner.run (left, right, block, false, std::pair { juce::String ("rate"), rate });

        if (! reference)
        {
            reference = result;
            continue;
        }

        CHECK (result.left == reference->left);
        CHECK (result.right == reference->right);
    }
}

TEST_CASE ("parameter moves don't click", "[bitcrusher]")
{
    // A quiet, slow sine at the full rate and 16 bits: a sweep of the depth to 6 bits and back over the 20 ms ramp
    // never jumps by more than a few of the coarsest steps.
    std::vector<float> sine (48000);

    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * std::sin (static_cast<float> (i) * 0.01f);

    Runner runner ({ { "bits", 16.0f }, { "rate", 192000.0f } });
    auto output = runner.run (std::vector<float> (sine.begin(), sine.begin() + 12000));
    runner.set ({ { "bits", 6.0f } });
    const auto after = runner.run (std::vector<float> (sine.begin() + 12000, sine.end()));
    output.insert (output.end(), after.begin(), after.end());

    auto largest = 0.0f;

    for (size_t i = 1; i < output.size(); ++i)
        largest = std::max (largest, std::abs (output[i] - output[i - 1]));

    CHECK (largest < 3.0f * std::exp2 (-5.0f));
}

TEST_CASE ("the Bit Crusher doesn't allocate while processing", "[bitcrusher][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    const auto input = noise (8192, 7);
    std::vector<float> note (input.size(), 12.0f);

    for (const auto& values : { std::map<juce::String, float> { { "bits", 3.5f },
                                                                { "rate", 5000.0f },
                                                                { "jitter", 50.0f },
                                                                { "antiAlias", 1.0f },
                                                                { "smooth", 1.0f },
                                                                { "dither", 1.0f },
                                                                { "clip", 1.0f } },
                                std::map<juce::String, float> { { "rateMode", 1.0f } } })
    {
        Runner runner (values);
        const auto before = allocationViolationCount();
        std::ignore = runner.run (input, input, 512, true);
        std::ignore = runner.run (input, input, 512, true, std::pair { juce::String ("note"), note });
        CHECK (allocationViolationCount() == before);
    }
}
