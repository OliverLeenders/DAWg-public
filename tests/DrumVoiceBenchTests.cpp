#include "TestUtils.h"
#include "dawg/DrumVoices.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_basics/juce_audio_basics.h>

#include <chrono>
#include <cstdio>
#include <vector>

// What one voice of each Drum Synth model costs per sample (docs/design/drum-synth.md, step 7), with and without
// denormals flushed. Hidden: run with [.drum-voice-bench], in a Release build.

using namespace dawg;
using namespace dawg::test;

namespace
{
[[nodiscard]] double nanosecondsPerSample (DrumModel model, bool flush)
{
    constexpr int block = 480;
    constexpr int blocks = 400; // 4 s, into the tails
    std::vector<float> left (block);
    std::vector<float> right (block);
    double total = 0.0;
    int samples = 0;

    for (int round = 0; round < 8; ++round)
    {
        DrumVoice voice;
        voice.prepare (sampleRate);
        auto hit = DrumHit::defaults (model);
        hit.common[drumDecay] = 400.0f;
        voice.trigger (hit);

        for (int b = 0; b < blocks && voice.active(); ++b)
        {
            const auto start = std::chrono::steady_clock::now();

            if (flush)
            {
                const juce::ScopedNoDenormals noDenormals;
                voice.render (left.data(), right.data(), block);
            }
            else
            {
                voice.render (left.data(), right.data(), block);
            }

            total += std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start).count();
            samples += block;
        }
    }

    return total / samples;
}
} // namespace

TEST_CASE ("Drum voices: cost per sample of each model", "[.drum-voice-bench]")
{
    for (size_t m = 1; m < drumModelCount; ++m)
    {
        const auto model = static_cast<DrumModel> (m);
        std::printf ("  %-7s %7.1f ns/sample   %7.1f with denormals flushed\n", drumModelName (model),
                     nanosecondsPerSample (model, false), nanosecondsPerSample (model, true));
    }
}
