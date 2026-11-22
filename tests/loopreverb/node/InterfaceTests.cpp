#include "../harness/Check.h"
#include "../harness/Render.h"
#include "Nodes.h"
#include "Reverb.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

// Checks of SPEC.md's interface (section 4) and engineering rules (section 7). They say nothing about how the reverb
// sounds; the acceptance tests (section 8) are run separately by the DAWg side.

namespace
{
using harness::Audio;
using harness::RenderOptions;

struct Expected
{
    std::string_view id;
    std::string_view name;
    float min;
    float max;
    dawg::ParamKind kind;
    std::string_view unit;
};

// The nine controls whose ids and names are fixed (4.1).
constexpr std::array<Expected, 9> interface { {
    { "feedback", "Feedback", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "diffusion", "Diffusion", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "size", "Size", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "predelay", "Pre-delay", 0.0f, 250.0f, dawg::ParamKind::continuous, "ms" },
    { "damp", "Damp highs", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "thin", "Thin lows", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "mute", "Mute", 0.0f, 1.0f, dawg::ParamKind::toggle, "" },
    { "mix", "Mix", 0.0f, 100.0f, dawg::ParamKind::continuous, "%" },
    { "seed", "Seed", 1.0f, 999999.0f, dawg::ParamKind::integer, "" },
} };

[[nodiscard]] bool finite (const Audio& audio)
{
    for (const auto* channel : { &audio.left, &audio.right })
        for (const auto sample : *channel)
            if (! std::isfinite (sample))
                return false;

    return true;
}

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from = 0)
{
    float result = 0.0f;
    for (auto i = from; i < samples.size(); ++i)
        result = std::max (result, std::abs (samples[i]));
    return result;
}

// Every control at its minimum, its maximum, or a mixed setting.
[[nodiscard]] std::map<std::string, float> extremes (int which)
{
    std::map<std::string, float> params;
    const auto& all = dawg::reverb::reverbDescriptor.params;

    for (size_t i = 0; i < all.size(); ++i)
    {
        const auto& p = all[i];
        const auto value = which == 0 ? p.minValue : which == 1 ? p.maxValue : (i % 2 == 0 ? p.maxValue : p.minValue);
        params[std::string (p.id)] = value;
    }

    params["mute"] = 0.0f;
    return params;
}
} // namespace

TEST ("the descriptor has the specified type, name and controls (4)")
{
    const auto& d = dawg::reverb::reverbDescriptor;
    CHECK (std::string_view (d.type) == "dawg.loopReverb");
    CHECK (std::string_view (d.displayName) == "Loop Reverb");
    CHECK (d.category == dawg::NodeCategory::effect);
    CHECK (d.inputs.size() == 1 && d.outputs.size() == 1);
    // The implementer's controls (SPEC 1.6, 4.1): two early-reflection controls, the modulation control and, optionally,
    // a second width control.
    CHECK (d.params.size() == interface.size() + 3 || d.params.size() == interface.size() + 4);

    for (const auto& expected : interface)
    {
        const auto index = d.indexOfParam (expected.id);
        CHECK (index >= 0);

        if (index < 0)
            continue;

        const auto& p = d.params[static_cast<size_t> (index)];
        CHECK (std::string_view (p.name) == expected.name);
        CHECK (p.minValue == expected.min);
        CHECK (p.maxValue == expected.max);
        CHECK (p.kind == expected.kind);
        CHECK (std::string_view (p.unit) == expected.unit);
        CHECK (p.defaultValue >= p.minValue && p.defaultValue <= p.maxValue);
    }

    // The implementer's controls (5.5, 5.10, 5.12): ids and names are free, range, unit and kind are not.
    int early = 0;

    for (const auto& p : d.params)
    {
        const auto id = std::string_view (p.id);
        const auto fixed =
            std::any_of (interface.begin(), interface.end(), [id] (const auto& e) { return e.id == id; });

        if (fixed)
            continue;

        ++early;
        CHECK (! id.empty() && id.front() >= 'a' && id.front() <= 'z');
        CHECK (d.indexOfParam (id) == &p - d.params.data());
        CHECK (! std::string_view (p.name).empty() && std::string_view (p.name).size() <= 12);
        CHECK (p.minValue == 0.0f && p.maxValue == 100.0f);
        CHECK (p.kind == dawg::ParamKind::continuous);
        CHECK (std::string_view (p.unit) == "%");
        CHECK (p.defaultValue >= p.minValue && p.defaultValue <= p.maxValue);
    }

    CHECK (early == 3 || early == 4);
}

TEST ("every output sample is written (7.1)")
{
    auto node = nodes::make ("dawg.loopReverb");
    node->prepare ({ 48000.0, 512 });

    std::vector<float> inL (512, 0.1f), inR (512, -0.1f);
    std::vector<float> outL (512, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> outR (512, std::numeric_limits<float>::quiet_NaN());
    const std::array<dawg::AudioInput, 1> inputs { { { inL.data(), inR.data() } } };
    const std::array<dawg::AudioOutput, 1> outputs { { { outL.data(), outR.data() } } };
    dawg::Transport transport;
    dawg::ProcessContext context;
    context.numSamples = 512;
    context.audioInputs = inputs;
    context.audioOutputs = outputs;
    context.transport = &transport;
    node->process (context);

    CHECK (finite ({ 48000.0, outL, outR }));
}

TEST ("the output doesn't depend on how the audio is cut into blocks (7.3)")
{
    for (int setting = 0; setting < 3; ++setting)
    {
        auto a = nodes::make ("dawg.loopReverb");
        auto b = nodes::make ("dawg.loopReverb");
        const auto input = harness::noise (0.5, 48000.0);

        RenderOptions fixed;
        fixed.params = extremes (setting);
        fixed.tailSamples = 24000;
        RenderOptions varied = fixed;
        varied.blockSizes = { 1, 7, 512, 64, 2048, 33, 300 };

        const auto x = harness::render (*a, input, fixed);
        const auto y = harness::render (*b, input, varied);
        CHECK (x.left == y.left);
        CHECK (x.right == y.right);
    }
}

TEST ("the same settings and seed always give the same output (5.6)")
{
    auto a = nodes::make ("dawg.loopReverb");
    auto b = nodes::make ("dawg.loopReverb");
    RenderOptions options;
    options.params = extremes (2);
    options.tailSamples = 48000;
    const auto input = harness::impulse (0.01, 48000.0);
    CHECK (harness::render (*a, input, options).left == harness::render (*b, input, options).left);
}

TEST ("the output stays finite and bounded at every extreme, at every sample rate (7.2, 7.4)")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int setting = 0; setting < 3; ++setting)
        {
            auto node = nodes::make ("dawg.loopReverb");
            RenderOptions options;
            options.sampleRate = rate;
            options.params = extremes (setting);
            options.tailSamples = static_cast<int> (rate * 10.0);
            const auto output = harness::render (*node, harness::noise (1.0, rate), options);
            CHECK (finite (output));
            CHECK (peak (output.left) < 8.0f);
            CHECK (peak (output.right) < 8.0f);
        }
}

TEST ("Mix 0 % is the input unchanged (5.9)")
{
    auto node = nodes::make ("dawg.loopReverb");
    RenderOptions options;
    options.params = extremes (2);
    options.params["mix"] = 0.0f;
    const auto input = harness::noise (0.5, 48000.0);
    const auto output = harness::render (*node, input, options);

    for (size_t i = 0; i < input.left.size(); i += 97)
        CHECK_NEAR (output.left[i], input.left[i], 1.0e-6);
}

TEST ("a prepared node starts silent: no tail is left from before (4, prepare)")
{
    auto node = nodes::make ("dawg.loopReverb");
    RenderOptions options;
    options.params = extremes (1);
    std::ignore = harness::render (*node, harness::noise (0.5, 48000.0), options);
    const auto afterwards = harness::render (*node, harness::silence (0.5, 48000.0), options);
    CHECK (peak (afterwards.left) == 0.0f);
}
