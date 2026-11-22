#include "dawg/Convolver.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

// The Convolution Reverb's engine (engine/src/Convolver.cpp; docs/design/convolution-reverb.md, section 5): the
// convolver against a direct convolution, block-size independence, program swaps, and the IR's preparation.

using namespace dawg;
using namespace dawg::convolution;

namespace
{
constexpr double rate = 48000.0;

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

[[nodiscard]] juce::AudioBuffer<float> randomImpulse (int channels, int length, unsigned seed)
{
    juce::AudioBuffer<float> ir (channels, length);

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto values = noise (static_cast<size_t> (length), seed + static_cast<unsigned> (ch));

        for (int i = 0; i < length; ++i)
            ir.setSample (ch, i,
                          values[static_cast<size_t> (i)]
                              * std::exp (-3.0f * static_cast<float> (i) / static_cast<float> (length)));
    }

    return ir;
}

// The convolution worked out directly, in double.
[[nodiscard]] Channels reference (const juce::AudioBuffer<float>& ir, Layout layout, const Channels& input)
{
    const auto length = input[0].size();
    std::array<std::vector<double>, 2> sum { std::vector<double> (length), std::vector<double> (length) };

    for (const auto& route : routesOf (layout))
    {
        const auto* h = ir.getReadPointer (route.channel);
        const auto& x = input[static_cast<size_t> (route.input)];
        auto& y = sum[static_cast<size_t> (route.output)];

        // Scattered from each non-zero input sample, so silent stretches cost nothing.
        for (size_t m = 0; m < length; ++m)
        {
            if (x[m] == 0.0f)
                continue;

            for (size_t t = 0; t < static_cast<size_t> (ir.getNumSamples()) && m + t < length; ++t)
                y[m + t] += static_cast<double> (h[t]) * x[m];
        }
    }

    Channels result;

    for (size_t ch = 0; ch < 2; ++ch)
        for (const auto value : sum[ch])
            result[ch].push_back (static_cast<float> (value));

    return result;
}

// A convolver with its exchange, run over whole buffers in blocks of the given sizes, in turn.
struct Rig
{
    Rig() { convolver.prepare(); }

    void publish (const std::shared_ptr<const Kernel>& kernel, const std::shared_ptr<History>& history)
    {
        exchange.publish (std::make_shared<Program> (Program { kernel, history }));
    }

    [[nodiscard]] Channels run (const Channels& input, const std::vector<int>& blocks = { 256 })
    {
        const auto length = input[0].size();
        Channels output { std::vector<float> (length), std::vector<float> (length) };
        size_t offset = 0;
        size_t index = 0;

        while (offset < length)
        {
            const auto count = std::min (static_cast<size_t> (blocks[index++ % blocks.size()]), length - offset);
            convolver.process (exchange, { input[0].data() + offset, input[1].data() + offset },
                               { output[0].data() + offset, output[1].data() + offset }, static_cast<int> (count),
                               rate);
            offset += count;
        }

        return output;
    }

    ProgramExchange exchange;
    Convolver convolver;
};

[[nodiscard]] float peakOf (const Channels& signal)
{
    auto peak = 0.0f;

    for (const auto& channel : signal)
        for (const auto sample : channel)
            peak = std::max (peak, std::abs (sample));

    return peak;
}

[[nodiscard]] float maxError (const Channels& a, const Channels& b, size_t from = 0)
{
    auto error = 0.0f;

    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = from; i < a[ch].size(); ++i)
            error = std::max (error, std::abs (a[ch][i] - b[ch][i]));

    return error;
}

[[nodiscard]] std::shared_ptr<History> historyFor (const std::shared_ptr<const Kernel>& kernel)
{
    return std::make_shared<History> (level3Partitions (kernel->length));
}
} // namespace

TEST_CASE ("the convolver matches a direct convolution", "[convolution][dsp]")
{
    const Channels input { noise (16000, 1), noise (16000, 2) };

    for (const auto layout : { Layout::mono, Layout::stereo, Layout::trueStereo })
    {
        for (const auto length : { 1, 63, 64, 65, 511, 512, 513, 4096, 4097, 9000 })
        {
            INFO ("layout " << static_cast<int> (layout) << ", length " << length);
            const auto ir = randomImpulse (channelsOf (layout), length, static_cast<unsigned> (length));
            const auto kernel = makeKernel (ir, layout, rate);
            Rig rig;
            rig.publish (kernel, historyFor (kernel));
            const auto output = rig.run (input);
            const auto expected = reference (ir, layout, input);
            CHECK (maxError (output, expected) <= 2.0e-5f * std::max (1.0f, peakOf (expected)));
        }
    }
}

TEST_CASE ("a unit impulse through the convolver returns the IR, with no latency", "[convolution][dsp]")
{
    const auto ir = randomImpulse (2, 10000, 7);
    const auto kernel = makeKernel (ir, Layout::stereo, rate);
    Rig rig;
    rig.publish (kernel, historyFor (kernel));
    Channels input { std::vector<float> (12000), std::vector<float> (12000) };
    input[0][0] = 1.0f;
    input[1][0] = 1.0f;
    const auto output = rig.run (input, { 100 });

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < ir.getNumSamples(); ++i)
            REQUIRE (output[static_cast<size_t> (ch)][static_cast<size_t> (i)]
                     == Catch::Approx (ir.getSample (ch, i)).margin (1.0e-5));

        for (size_t i = static_cast<size_t> (ir.getNumSamples()); i < input[0].size(); ++i)
            REQUIRE (std::abs (output[static_cast<size_t> (ch)][i]) < 1.0e-5f);
    }
}

TEST_CASE ("the convolver's output doesn't depend on the block sizes", "[convolution][dsp]")
{
    const auto ir = randomImpulse (4, 20000, 3);
    const auto kernel = makeKernel (ir, Layout::trueStereo, rate);
    const Channels input { noise (40000, 4), noise (40000, 5) };
    Channels first;

    for (const auto& blocks :
         std::vector<std::vector<int>> { { 4096 }, { 1 }, { 17 }, { 64 }, { 333 }, { 1, 17, 64, 333, 4096 } })
    {
        Rig rig;
        rig.publish (kernel, historyFor (kernel));
        const auto output = rig.run (input, blocks);

        if (first[0].empty())
            first = output;
        else
            REQUIRE (output == first);
    }
}

TEST_CASE ("a new IR prepares for a cycle, fades in over the next and keeps the tail of what was played",
           "[convolution][dsp]")
{
    // IR B is A with a different start: the edit a trim control makes.
    const auto a = randomImpulse (2, 20000, 11);
    juce::AudioBuffer<float> b (2, 16000);

    for (int ch = 0; ch < 2; ++ch)
        b.copyFrom (ch, 0, a, ch, 4000, 16000);

    const auto kernelA = makeKernel (a, Layout::stereo, rate);
    const auto kernelB = makeKernel (b, Layout::stereo, rate);
    const auto history = historyFor (kernelA);

    // A burst, then silence: the swap happens while only the tail rings.
    Channels input { noise (34000, 12), noise (34000, 13) };

    for (auto& channel : input)
        std::fill (channel.begin() + 6000, channel.end(), 0.0f);

    Rig rig;
    rig.publish (kernelA, history);
    Channels output { std::vector<float> (34000), std::vector<float> (34000) };

    const auto runPart = [&] (size_t from, size_t to)
    {
        for (size_t offset = from; offset < to; offset += 300)
        {
            const auto count = std::min<size_t> (300, to - offset);
            rig.convolver.process (rig.exchange, { input[0].data() + offset, input[1].data() + offset },
                                   { output[0].data() + offset, output[1].data() + offset }, static_cast<int> (count),
                                   rate);
        }
    };

    runPart (0, 10000);
    // Taken at 10 000, prepares from the boundary at 12 288 and fades in from 16 384.
    rig.publish (kernelB, history);
    runPart (10000, 34000);

    const auto withA = reference (a, Layout::stereo, input);
    const auto withB = reference (b, Layout::stereo, input);
    constexpr size_t fadeStart = 4 * cycleLength;
    constexpr size_t fadeEnd = fadeStart + cycleLength;

    for (size_t ch = 0; ch < 2; ++ch)
    {
        for (size_t i = 0; i < fadeStart; ++i)
            REQUIRE (output[ch][i] == Catch::Approx (withA[ch][i]).margin (1.0e-4));

        // During the fade, a linear crossfade of the two; after it, B as if it had always been there.
        for (size_t i = fadeStart; i < fadeEnd; ++i)
        {
            const auto g = static_cast<double> (i - fadeStart) / cycleLength;
            REQUIRE (output[ch][i] == Catch::Approx ((1.0 - g) * withA[ch][i] + g * withB[ch][i]).margin (1.0e-4));
        }

        for (size_t i = fadeEnd; i < output[ch].size(); ++i)
            REQUIRE (output[ch][i] == Catch::Approx (withB[ch][i]).margin (1.0e-4));
    }

    // B's tail carries on after the fade, from input played before B arrived (it ends at 6000 + 16 000).
    auto energy = 0.0;

    for (size_t i = fadeEnd; i < 21900; ++i)
        energy += static_cast<double> (output[0][i]) * output[0][i];

    CHECK (energy > 1.0e-3);
}

TEST_CASE ("a longer IR with a larger history takes over the old history", "[convolution][dsp]")
{
    const auto a = randomImpulse (1, 4500, 21);
    const auto b = randomImpulse (1, 20000, 22);
    const auto kernelA = makeKernel (a, Layout::mono, rate);
    const auto kernelB = makeKernel (b, Layout::mono, rate);
    const auto smallHistory = historyFor (kernelA);
    const auto largeHistory = historyFor (kernelB);
    REQUIRE (smallHistory->levels[2].capacity < kernelB->levels[2].partitions);

    const Channels input { noise (50000, 23), noise (50000, 24) };
    Rig rig;
    rig.publish (kernelA, smallHistory);
    Channels output { std::vector<float> (50000), std::vector<float> (50000) };

    for (size_t offset = 0; offset < 50000; offset += 500)
    {
        if (offset == 20000)
            rig.publish (kernelB, largeHistory); // prepares from 20 480, fades in from 24 576

        rig.convolver.process (rig.exchange, { input[0].data() + offset, input[1].data() + offset },
                               { output[0].data() + offset, output[1].data() + offset }, 500, rate);
    }

    // After the fade the old history's spectra are there, but the input before them isn't: B is exact once its
    // partitions only reach back as far as the copied history (a.capacity level-3 blocks before the swap).
    const auto withB = reference (b, Layout::mono, input);
    const auto exactFrom = static_cast<size_t> (24576 + cycleLength + 20000);

    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = exactFrom; i < 50000; ++i)
            REQUIRE (output[ch][i] == Catch::Approx (withB[ch][i]).margin (1.0e-4));

    // And no click at the swap: the output never jumps further than it does steadily with A before or B after.
    const auto steps = [&output] (size_t from, size_t to)
    {
        auto largest = 0.0f;

        for (size_t i = from; i < to; ++i)
            largest = std::max (largest, std::abs (output[0][i] - output[0][i - 1]));

        return largest;
    };

    CHECK (steps (20000, 29000) < 1.5f * std::max (steps (10000, 20000), steps (30000, 50000)));
}

TEST_CASE ("a program for another rate is silent", "[convolution][dsp]")
{
    const auto ir = randomImpulse (2, 1000, 31);
    const auto kernel = makeKernel (ir, Layout::stereo, 44100.0);
    Rig rig;
    rig.publish (kernel, historyFor (kernel));
    const Channels input { noise (5000, 32), noise (5000, 33) };
    CHECK (peakOf (rig.run (input)) == 0.0f);
}

TEST_CASE ("resampling keeps a tone's frequency and length and removes what is above the new Nyquist",
           "[convolution][dsp]")
{
    // A decaying 1 kHz sine at 96 kHz, brought to 48 kHz.
    juce::AudioBuffer<float> tone (1, 96000);

    for (int i = 0; i < 96000; ++i)
        tone.setSample (0, i,
                        static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 96000.0)
                                            * std::exp (-2.0 * i / 96000.0)));

    const auto down = resample (tone, 0.5);
    REQUIRE (down.getNumSamples() == 48000);

    // Zero crossings over the first half second: 1000 Hz gives 1000.
    int crossings = 0;

    for (int i = 1; i < 24000; ++i)
        if ((down.getSample (0, i - 1) < 0.0f) != (down.getSample (0, i) < 0.0f))
            ++crossings;

    CHECK (std::abs (crossings - 1000) <= 2);

    // The amplitude is kept (away from the ends).
    CHECK (down.getMagnitude (0, 1000, 200) == Catch::Approx (tone.getMagnitude (0, 2000, 400)).epsilon (0.02));

    // 30 kHz at 96 kHz is above 24 kHz: almost nothing is left.
    juce::AudioBuffer<float> high (1, 96000);

    for (int i = 0; i < 96000; ++i)
        high.setSample (0, i,
                        static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 30000.0 * i / 96000.0)));

    const auto removed = resample (high, 0.5);
    CHECK (removed.getRMSLevel (0, 1000, 46000) < 1.0e-3f);

    // 44.1 kHz up to 48 kHz keeps 1 kHz at 1 kHz.
    juce::AudioBuffer<float> cd (1, 44100);

    for (int i = 0; i < 44100; ++i)
        cd.setSample (0, i,
                      static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0)));

    const auto up = resample (cd, 48000.0 / 44100.0);
    REQUIRE (up.getNumSamples() == 48000);
    crossings = 0;

    for (int i = 1; i < 48000; ++i)
        if ((up.getSample (0, i - 1) < 0.0f) != (up.getSample (0, i) < 0.0f))
            ++crossings;

    CHECK (std::abs (crossings - 2000) <= 2);
}

TEST_CASE ("the decay estimate measures an exponential decay", "[convolution][dsp]")
{
    for (const auto seconds : { 0.5, 2.0, 5.0 })
    {
        INFO ("decay " << seconds << " s");
        const auto length = static_cast<int> (seconds * 1.2 * rate);
        juce::AudioBuffer<float> ir (2, length);
        const auto values = noise (static_cast<size_t> (2 * length), 41);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                ir.setSample (ch, i,
                              values[static_cast<size_t> (ch * length + i)]
                                  * static_cast<float> (std::pow (10.0, -3.0 * i / (seconds * rate))));

        CHECK (estimateDecay (ir, rate) == Catch::Approx (seconds).epsilon (0.05));
    }

    juce::AudioBuffer<float> silence (1, 100);
    silence.clear();
    CHECK (estimateDecay (silence, rate) == 0.0);
}

TEST_CASE ("shaping cuts, fades, decays and reverses the IR", "[convolution][dsp]")
{
    juce::AudioBuffer<float> file (1, 48000);
    file.clear();

    for (int i = 0; i < 48000; ++i)
        file.setSample (0, i, 0.5f);

    Shape plain;
    plain.normalise = false;

    SECTION ("untouched")
    {
        const auto shaped = shapeImpulse (file, rate, Layout::mono, rate, plain, 0.0);
        REQUIRE (shaped.ir.getNumSamples() == 48000);
        CHECK (shaped.ir.getSample (0, 0) == 0.5f);
        CHECK (shaped.ir.getSample (0, 47999) == 0.5f);
        CHECK_FALSE (shaped.truncated);
    }

    SECTION ("start and length")
    {
        auto shape = plain;
        shape.startMs = 100.0;
        shape.lengthPercent = 50.0;
        const auto shaped = shapeImpulse (file, rate, Layout::mono, rate, shape, 0.0);
        REQUIRE (shaped.ir.getNumSamples() == 21600); // half of the 43 200 after 100 ms
        CHECK (shaped.ir.getSample (0, 0) < 0.01f);   // the fade-in after the cut
        CHECK (shaped.ir.getSample (0, 100) == 0.5f);
        CHECK (shaped.ir.getSample (0, 21599) < 0.01f); // the fade-out at the end
        CHECK (shaped.ir.getSample (0, 21600 - 2160 - 10) == 0.5f);
    }

    SECTION ("reverse")
    {
        auto shape = plain;
        shape.lengthPercent = 50.0;
        shape.reverse = true;
        const auto shaped = shapeImpulse (file, rate, Layout::mono, rate, shape, 0.0);
        CHECK (shaped.ir.getSample (0, 0) < 0.01f);
        CHECK (shaped.ir.getSample (0, 23999) == 0.5f);
    }

    SECTION ("decay")
    {
        auto shape = plain;
        shape.decayPercent = 50.0;
        const auto shaped = shapeImpulse (file, rate, Layout::mono, rate, shape, 2.0);
        // Falling 60 dB in 1 s instead of 2 s adds 30 dB per second: −30 dB after one second.
        CHECK (shaped.ir.getSample (0, 47999) / 0.5f == Catch::Approx (std::pow (10.0, -30.0 / 20.0)).epsilon (0.01));
    }

    SECTION ("normalise")
    {
        auto shape = plain;
        shape.normalise = true;
        const auto shaped = shapeImpulse (file, rate, Layout::mono, rate, shape, 0.0);
        auto energy = 0.0;

        for (int i = 0; i < shaped.ir.getNumSamples(); ++i)
            energy += static_cast<double> (shaped.ir.getSample (0, i)) * shaped.ir.getSample (0, i);

        CHECK (energy == Catch::Approx (1.0).epsilon (1.0e-3));
    }

    SECTION ("stretch and the length cap")
    {
        juce::AudioBuffer<float> longFile (1, static_cast<int> (15.0 * rate));
        longFile.clear();
        auto shape = plain;
        shape.stretchPercent = 200.0;
        const auto shaped = shapeImpulse (longFile, rate, Layout::mono, rate, shape, 0.0);
        CHECK (shaped.truncated);
        CHECK (std::abs (shaped.ir.getNumSamples() - static_cast<int> (maxSeconds * rate)) <= 2);
    }
}

TEST_CASE ("the default IR decays in about two seconds", "[convolution][dsp]")
{
    const auto ir = defaultImpulse (rate);
    CHECK (ir.getNumChannels() == 2);
    CHECK (estimateDecay (ir, rate) == Catch::Approx (2.0).epsilon (0.1));
    CHECK (ir.getSample (0, 0) == 0.0f);

    const auto analysis = analyse (ir, rate);
    CHECK (analysis.envelope.size() <= 256);
    CHECK (analysis.seconds == Catch::Approx (2.5));
    CHECK (analysis.envelope.front() > analysis.envelope.back() + 40.0f);
}

TEST_CASE ("convolver cost for a long IR", "[.][bench][convolution]")
{
    // Run by hand: dawg_tests "[bench][convolution]". A 10 s IR, 20 s of input in 256-sample blocks.
    for (const auto layout : { Layout::stereo, Layout::trueStereo })
    {
        const auto ir = randomImpulse (channelsOf (layout), static_cast<int> (10.0 * rate), 51);
        const auto kernel = makeKernel (ir, layout, rate);
        Rig rig;
        rig.publish (kernel, historyFor (kernel));
        const Channels input { noise (static_cast<size_t> (20.0 * rate), 52),
                               noise (static_cast<size_t> (20.0 * rate), 53) };
        Channels output { std::vector<float> (input[0].size()), std::vector<float> (input[0].size()) };
        auto slowest = 0.0;
        const auto start = juce::Time::getMillisecondCounterHiRes();

        for (size_t offset = 0; offset < input[0].size(); offset += 256)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            rig.convolver.process (rig.exchange, { input[0].data() + offset, input[1].data() + offset },
                                   { output[0].data() + offset, output[1].data() + offset }, 256, rate);
            const auto took = juce::Time::getMillisecondCounterHiRes() - t0;

            if (offset >= static_cast<size_t> (rate)) // after the first second, once caches are warm
                slowest = std::max (slowest, took);
        }

        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        WARN ((layout == Layout::stereo ? "stereo" : "true stereo")
              << ": " << 100.0 * seconds / 20.0 << " % of one core, slowest 256-sample block " << slowest
              << " ms (the block lasts 5.33 ms)");
    }
}
