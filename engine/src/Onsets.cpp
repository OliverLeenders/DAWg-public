#include "dawg/Onsets.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
constexpr int fftOrder = 10; // 1024 frames
constexpr int fftSize = 1 << fftOrder;
constexpr int hop = 128;
constexpr double compression = 100.0;    // log (1 + c |X|): louder and quieter onsets count alike
constexpr double thresholdSeconds = 0.1; // the adaptive threshold's reach on each side
constexpr double minGapSeconds = 0.03;   // onsets closer than this are one
constexpr double thresholdOffset = 0.05; // of the largest flux
constexpr int envelopeBlock = 16;

// The mono sum.
[[nodiscard]] std::vector<float> monoOf (const juce::AudioBuffer<float>& audio)
{
    std::vector<float> mono (static_cast<size_t> (audio.getNumSamples()), 0.0f);
    const auto gain = 1.0f / static_cast<float> (std::max (1, audio.getNumChannels()));

    for (int c = 0; c < audio.getNumChannels(); ++c)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (c), gain,
                                                      audio.getNumSamples());

    return mono;
}

// Spectral flux per hop: the summed rise of the compressed magnitudes.
[[nodiscard]] std::vector<double> fluxOf (const std::vector<float>& mono)
{
    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window (fftSize, juce::dsp::WindowingFunction<float>::hann, false);
    const auto frames = mono.size() < static_cast<size_t> (fftSize) ? size_t { 1 } : (mono.size() - fftSize) / hop + 1;
    std::vector<double> flux (frames, 0.0);
    std::vector<float> data (2 * static_cast<size_t> (fftSize));
    std::vector<double> previous (fftSize / 2 + 1, 0.0);

    for (size_t f = 0; f < frames; ++f)
    {
        std::ranges::fill (data, 0.0f);
        const auto start = f * hop;
        const auto count = std::min<size_t> (fftSize, mono.size() - std::min (mono.size(), start));
        std::copy_n (mono.begin() + static_cast<std::ptrdiff_t> (start), count, data.begin());
        window.multiplyWithWindowingTable (data.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);
        auto sum = 0.0;

        for (size_t k = 0; k < previous.size(); ++k)
        {
            const auto magnitude = std::log1p (compression * data[k]);
            sum += std::max (0.0, magnitude - previous[k]);
            previous[k] = magnitude;
        }

        flux[f] = sum;
    }

    return flux;
}

// The attack of an onset whose flux peaks at analysis frame f: where the waveform's envelope, from its quietest
// point before its peak in the window, has risen a third of the way to that peak.
[[nodiscard]] juce::int64 refine (const std::vector<float>& mono, size_t f)
{
    // The attack lies in the analysis window, which starts at f * hop.
    const auto centre = static_cast<juce::int64> (f) * hop + fftSize / 2;
    const auto from = std::max<juce::int64> (0, centre - fftSize / 2 - hop);
    const auto to = std::min<juce::int64> (static_cast<juce::int64> (mono.size()), centre + fftSize / 2 + hop);

    if (to - from < 2 * juce::int64 { envelopeBlock })
        return std::clamp<juce::int64> (centre, 0, static_cast<juce::int64> (mono.size()));

    std::vector<float> envelope;

    for (auto n = from; n + envelopeBlock <= to; n += envelopeBlock)
    {
        auto peak = 0.0f;

        for (int i = 0; i < envelopeBlock; ++i)
            peak = std::max (peak, std::abs (mono[static_cast<size_t> (n + i)]));

        envelope.push_back (peak);
    }

    const auto top = std::ranges::max_element (envelope);
    const auto floor = *std::min_element (envelope.begin(), top + 1);
    const auto level = floor + (*top - floor) / 3.0f;
    const auto quietest = std::min_element (envelope.begin(), top + 1);

    for (auto it = quietest; it <= top; ++it)
    {
        if (*it < level)
            continue;

        // Within the block: the first sample past the level.
        const auto block = from + static_cast<juce::int64> (it - envelope.begin()) * envelopeBlock;

        for (int i = 0; i < envelopeBlock; ++i)
            if (std::abs (mono[static_cast<size_t> (block + i)]) >= level)
                return block + i;

        return block;
    }

    return centre;
}
} // namespace

std::vector<Onset> detectOnsets (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto mono = monoOf (audio);

    if (mono.size() < static_cast<size_t> (fftSize))
        return {};

    const auto flux = fluxOf (mono);
    const auto largest = *std::ranges::max_element (flux);

    if (largest <= 0.0)
        return {};

    const auto reach = static_cast<size_t> (std::max (1.0, thresholdSeconds * sampleRate / hop));
    const auto minGap = static_cast<juce::int64> (minGapSeconds * sampleRate);
    std::vector<Onset> onsets;

    for (size_t f = 1; f + 1 < flux.size(); ++f)
    {
        if (flux[f] < flux[f - 1] || flux[f] < flux[f + 1])
            continue;

        // Above the mean around it by a margin.
        const auto lo = f > reach ? f - reach : 0;
        const auto hi = std::min (flux.size(), f + reach + 1);
        auto mean = 0.0;

        for (auto i = lo; i < hi; ++i)
            mean += flux[i];

        mean /= static_cast<double> (hi - lo);

        if (flux[f] < mean * 1.5 + thresholdOffset * largest)
            continue;

        const auto frame = refine (mono, f);
        const auto strength = static_cast<float> (flux[f] / largest);

        // One attack can peak twice: it keeps its first frame and the larger strength.
        if (! onsets.empty() && frame - onsets.back().frame < minGap)
        {
            onsets.back().strength = std::max (onsets.back().strength, strength);
            continue;
        }

        onsets.push_back ({ frame, strength });
    }

    return onsets;
}

} // namespace dawg
