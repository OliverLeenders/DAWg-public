#pragma once

#include "TestUtils.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

// Helpers for testing the waveshaping effects (the Saturator, the Clipper and the Waveshaper): one node run over whole
// buffers, sines that fit FFT bins, the spectrum measures the aliasing tests use, and the transparent ADAA written out
// again as a reference.

namespace dawg::test
{
inline constexpr int fftOrder = 14;
inline constexpr int fftSize = 1 << fftOrder;

// A frequency that fits a whole number of cycles into fftSize samples, so its harmonics fall on bins.
[[nodiscard]] inline double binFrequency (int bin)
{
    return static_cast<double> (bin) * sampleRate / fftSize;
}

[[nodiscard]] inline std::vector<float> sine (double frequency, double amplitude, int length)
{
    std::vector<float> result (static_cast<size_t> (length));

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<float> (
            amplitude
            * std::sin (juce::MathConstants<double>::twoPi * frequency * static_cast<double> (i) / sampleRate));

    return result;
}

[[nodiscard]] inline float dbToGain (float db)
{
    return std::pow (10.0f, db / 20.0f);
}

// One effect node instance, its parameters set before prepare(), run over whole buffers in blocks with the same
// signal on both channels.
struct EffectRunner
{
    EffectRunner (const char* type, const std::map<juce::String, float>& values)
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

    [[nodiscard]] std::vector<float> run (const std::vector<float>& input)
    {
        std::vector<float> left (input.size());
        std::vector<float> right (input.size());
        const std::vector<float> silence (static_cast<size_t> (blockSize));

        for (size_t offset = 0; offset < input.size(); offset += blockSize)
        {
            const auto count = std::min (static_cast<size_t> (blockSize), input.size() - offset);
            std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                            AudioInput { silence.data(), silence.data() });
            inputs[0] = { input.data() + offset, input.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { left.data() + offset, right.data() + offset } };

            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            node->process (context);
        }

        for (size_t i = 0; i < left.size(); ++i)
            REQUIRE (left[i] == right[i]);

        return left;
    }

    std::unique_ptr<Node> node;
};

// The magnitude spectrum of the last fftSize samples, Hann-windowed.
[[nodiscard]] inline std::vector<double> spectrum (const std::vector<float>& signal)
{
    REQUIRE (signal.size() >= fftSize);
    std::vector<float> data (2 * fftSize, 0.0f);
    const auto start = signal.size() - fftSize;

    for (size_t i = 0; i < fftSize; ++i)
    {
        const auto window =
            0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * static_cast<double> (i) / fftSize);
        data[i] = static_cast<float> (window * static_cast<double> (signal[start + i]));
    }

    juce::dsp::FFT fft (fftOrder);
    fft.performFrequencyOnlyForwardTransform (data.data());
    return { data.begin(), data.begin() + fftSize / 2 + 1 };
}

// The level in dB of the peak near a bin (the window spreads it over three bins).
[[nodiscard]] inline double levelDb (const std::vector<double>& magnitudes, int bin)
{
    auto power = 0.0;

    for (int b = std::max (0, bin - 1); b <= std::min (fftSize / 2, bin + 1); ++b)
        power += magnitudes[static_cast<size_t> (b)] * magnitudes[static_cast<size_t> (b)];

    return 10.0 * std::log10 (std::max (power, 1.0e-30));
}

// The power of everything that isn't DC or a harmonic of the fundamental below Nyquist (that is, the folded-back
// harmonics and noise), in dB relative to the fundamental.
[[nodiscard]] inline double aliasDb (const std::vector<double>& magnitudes, int fundamental)
{
    auto alias = 0.0;

    for (int b = 4; b <= fftSize / 2; ++b)
    {
        const auto nearest = static_cast<int> (std::lround (static_cast<double> (b) / fundamental)) * fundamental;

        if (nearest > 0 && std::abs (b - nearest) <= 3)
            continue;

        alias += magnitudes[static_cast<size_t> (b)] * magnitudes[static_cast<size_t> (b)];
    }

    return 10.0 * std::log10 (std::max (alias, 1.0e-30)) - levelDb (magnitudes, fundamental);
}

// Transparent ADAA (docs/design/vicanek-adaa.md), written out again here rather than taken from the engine: a one-pole
// y = (1 + a) x - a y1 before and after the shaper, a = 1 / (3 + sqrt 8), and between them half the curve's mean over
// each half of the last two input intervals. mean (from, to) is the curve's mean over an interval. The history starts
// where a constant input start has left it, with fStart the curve's value there.
template <typename Mean>
[[nodiscard]] std::vector<double> referenceTransparent (const std::vector<double>& inputs, const Mean& mean,
                                                        double start = 0.0, double fStart = 0.0)
{
    const auto a = 1.0 / (3.0 + std::sqrt (8.0));
    auto pre = start;
    auto post = fStart;
    auto v1 = start;
    auto m1 = start;
    std::vector<double> result;
    result.reserve (inputs.size());

    for (const auto u : inputs)
    {
        pre = (1.0 + a) * u - a * pre;
        const auto v = pre;
        const auto m = 0.5 * (v + v1);
        const auto shaped = 0.5 * mean (v1, m) + 0.5 * mean (m1, v1);
        post = (1.0 + a) * shaped - a * post;
        result.push_back (post);
        m1 = m;
        v1 = v;
    }

    return result;
}

// The transparent ADAA's response where the curve is the line f(u) = u: what a node's dry path goes through.
[[nodiscard]] inline std::vector<double> referenceLinear (const std::vector<float>& signal)
{
    const std::vector<double> inputs (signal.begin(), signal.end());
    return referenceTransparent (inputs, [] (double from, double to) { return 0.5 * (from + to); });
}

// The level of a sine at a bin relative to the input's, in dB: a node's gain at that frequency.
[[nodiscard]] inline double gainAtDb (const std::vector<float>& output, const std::vector<float>& input, int bin)
{
    return levelDb (spectrum (output), bin) - levelDb (spectrum (input), bin);
}
} // namespace dawg::test
