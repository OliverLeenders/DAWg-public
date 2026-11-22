#include "../harness/Check.h"
#include "Measure.h"
#include "Nodes.h"
#include "Reverb.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

// The clean side's checks of SPEC.md's behaviour (sections 5 to 7) beyond the acceptance table: sample-rate
// independence, block independence while controls move, seed changes, progressive controls, mix loudness and the
// panel's data. Measurements are printed for DESIGN.md, section 3.

namespace
{
using measure::Params;

constexpr double rate = 48000.0;

void report (const std::string& line)
{
    std::cout << "      " << line << "\n";
}

std::string fixed (double value, int precision = 2)
{
    auto text = std::to_string (value);
    const auto dot = text.find ('.');
    return dot == std::string::npos ? text : text.substr (0, dot + 1 + static_cast<size_t> (precision));
}
} // namespace

TEST ("7.4: the reverb sounds the same from 44.1 to 192 kHz (RT60, tone, early timing)")
{
    std::string line;
    double reference = 0.0, reference8k = 0.0, reference100 = 0.0;
    size_t first48 = 0;

    for (const auto sampleRate : { 48000.0, 44100.0, 96000.0, 192000.0 })
    {
        const auto response = measure::impulseResponse ({ { "mix", 100.0f } }, 3.0, sampleRate);
        const auto t60 = measure::rt60 (response);
        const auto level8k = measure::bandLevelDb (response, 8000.0, 0.0, 3.0) + 10.0 * std::log10 (sampleRate);
        const auto level100 = measure::bandLevelDb (response, 100.0, 0.0, 3.0) + 10.0 * std::log10 (sampleRate);
        const auto first = static_cast<double> (measure::firstAbove (response, 0.5f)) / sampleRate;
        line += fixed (sampleRate / 1000.0, 1) + " kHz: RT60 " + fixed (t60) + " s, first reflection "
                + fixed (first * 1000.0) + " ms, 8 kHz " + fixed (level8k, 1) + " dB, 100 Hz " + fixed (level100, 1)
                + " dB; ";

        if (sampleRate == 48000.0)
        {
            reference = t60;
            reference8k = level8k;
            reference100 = level100;
            first48 = static_cast<size_t> (first * 1.0e6);
            continue;
        }

        CHECK (std::abs (t60 / reference - 1.0) < 0.05);
        CHECK (std::abs (level8k - reference8k) < 1.5);
        CHECK (std::abs (level100 - reference100) < 1.0);
        CHECK (std::abs (static_cast<double> (first48) / 1.0e6 - first) < 0.0001);
    }

    report (line);
}

TEST ("7.3: with controls moving, the output still doesn't depend on the block sizes")
{
    // Changes happen at multiples of 512 samples, which are block boundaries for every sequence below.
    const auto input = harness::noise (2.0, rate);
    const auto change = [] (dawg::Node& node, size_t position)
    {
        if (position % 512 != 0)
            return;

        const auto step = static_cast<float> (position / 512);
        node.params().set (dawg::reverb::param::size, 30.0f + std::fmod (step * 7.0f, 50.0f));
        node.params().set (dawg::reverb::param::feedback, 20.0f + std::fmod (step * 3.0f, 70.0f));
        node.params().set (dawg::reverb::param::predelay, std::fmod (step * 11.0f, 200.0f));
        node.params().set (dawg::reverb::param::earlyTiming, std::fmod (step * 13.0f, 100.0f));
        node.params().set (dawg::reverb::param::mix, 50.0f);
    };

    const auto a = measure::renderChanging (input, {}, 512, change);
    const auto b = measure::renderChanging (input, {}, 64, change);
    const auto c = measure::renderChanging (input, {}, 128, change);
    CHECK (a.left == b.left && a.right == b.right);
    CHECK (a.left == c.left && a.right == c.right);
}

TEST ("5.6, 5.11: changing Seed restarts the pattern without a loud click; going back gives the same output")
{
    constexpr int block = 64;
    const auto input = measure::sine (1000.0, 0.5, 2.0);
    const auto seedIndex = static_cast<size_t> (dawg::reverb::param::seed);
    const Params settings { { "mix", 100.0f } };
    const auto still = measure::renderChanging (input, settings, block, [] (dawg::Node&, size_t) {});
    const auto changed = measure::renderChanging (input, settings, block,
                                                  [seedIndex] (dawg::Node& node, size_t position)
                                                  {
                                                      if (position == 48000)
                                                          node.params().set (seedIndex, 4711.0f);
                                                  });
    const auto ratio = measure::largestStep (changed, 4800) / measure::largestStep (still, 4800);
    report ("Seed change at Mix 100 %: largest step " + fixed (ratio) + " x unchanged");
    CHECK (ratio <= 4.0f);

    // A seed typed in again gives exactly the pattern it gave before.
    const auto first = measure::impulseResponse ({ { "seed", 4711.0f }, { "mix", 100.0f } }, 1.0);
    const auto again = measure::impulseResponse ({ { "seed", 4711.0f }, { "mix", 100.0f } }, 1.0);
    CHECK (first.left == again.left);
}

TEST ("5.7: Damp highs and Thin lows act progressively")
{
    std::vector<double> damp, thin;

    for (int v = 0; v <= 100; v += 20)
    {
        damp.push_back (measure::bandLevelDb (
            measure::impulseResponse ({ { "damp", static_cast<float> (v) }, { "mix", 100.0f } }, 2.0), 8000.0, 0.05,
            2.0));
        thin.push_back (measure::bandLevelDb (
            measure::impulseResponse ({ { "thin", static_cast<float> (v) }, { "mix", 100.0f } }, 2.0), 100.0, 0.05,
            2.0));
    }

    std::string d, t;
    for (size_t i = 0; i < damp.size(); ++i)
    {
        d += fixed (damp[i] - damp[0], 1) + " ";
        t += fixed (thin[i] - thin[0], 1) + " ";
    }

    report ("tail at 8 kHz re Damp 0 %, Damp 0..100 % in 20 % steps: " + d);
    report ("tail at 100 Hz re Thin 0 %, Thin 0..100 %: " + t);

    for (size_t i = 1; i < damp.size(); ++i)
    {
        CHECK (damp[i] < damp[i - 1]);
        CHECK (thin[i] < thin[i - 1]);
    }
}

TEST ("5.4: echo density rises steadily with Diffusion")
{
    std::vector<double> density;
    std::string line;

    for (int v = 0; v <= 100; v += 20)
    {
        density.push_back (measure::echoDensity (
            measure::impulseResponse ({ { "diffusion", static_cast<float> (v) }, { "mix", 100.0f } }, 0.3), 0.05,
            0.15));
        line += fixed (density.back(), 1) + " ";
    }

    report ("peaks per 10 ms, Diffusion 0..100 % in 20 % steps: " + line);

    // The peak count saturates near 150-160 per 10 ms at 48 kHz (a dense signal has a local peak every few
    // samples), so above that only "no fall" can be checked.
    for (size_t i = 1; i < density.size(); ++i)
        CHECK (density[i] >= 0.97 * density[i - 1]);

    CHECK (density.back() > 10.0 * density.front());
}

TEST ("5.9: the perceived loudness stays roughly constant across Mix (pink noise)")
{
    const auto input = measure::pinkNoise (6.0);
    const auto inputDb = measure::rmsDb (input, 3.0, 6.0);
    std::string line;

    for (int m = 0; m <= 100; m += 25)
    {
        auto node = nodes::make ("dawg.loopReverb");
        harness::RenderOptions options;
        options.params = { { "mix", static_cast<float> (m) } };
        const auto level = measure::rmsDb (harness::render (*node, input, options), 3.0, 6.0) - inputDb;
        line += fixed (level, 1) + " ";
        CHECK (std::abs (level) <= 3.0);
    }

    report ("output re input (dB), Mix 0..100 % in 25 % steps: " + line);
}

TEST ("5.2: the decay time grows steadily with Feedback (no jumps)")
{
    std::vector<double> times;

    for (int f = 0; f <= 100; f += 5)
    {
        const auto expected = 0.25 * std::pow (64.0, f / 100.0);
        times.push_back (measure::rt60 (measure::impulseResponse (
            { { "feedback", static_cast<float> (f) }, { "mix", 100.0f } }, 1.5 + 0.9 * expected)));
    }

    // Exponential by design: every 5 % step multiplies the T60 by 64^0.05 = 1.23. Allow some spread from the
    // measurement, but no jumps.
    for (size_t i = 1; i < times.size(); ++i)
    {
        const auto ratio = times[i] / times[i - 1];
        CHECK (ratio > 1.05 && ratio < 1.5);
    }
}

TEST ("6: the panel gets arrivals (ascending, at most 32, including the pre-delay) and levels")
{
    auto node = nodes::make ("dawg.loopReverb");
    harness::RenderOptions options;
    options.params = { { "predelay", 50.0f } };
    std::ignore = harness::render (*node, harness::noise (0.5, rate), options);
    const auto meters = node->meters();

    for (const auto* key : { "arrivalsLeft", "arrivalsRight" })
    {
        CHECK (meters.contains (key));
        if (! meters.contains (key))
            continue;

        const auto& times = meters.at (key);
        CHECK (! times.empty() && times.size() <= 32);
        CHECK (std::is_sorted (times.begin(), times.end()));
        CHECK (! times.empty() && times.front() >= 50.0);
    }

    CHECK (meters.contains ("level") && meters.at ("level").size() == 2);
    CHECK (meters.at ("level")[0] > 0.0);
    CHECK (node->meters().at ("level")[0] == 0.0); // reset by the previous call
}

TEST ("7.2: with -12 dBFS noise in, no sample exceeds 8.0 at any corner of the controls")
{
    const auto input = harness::noise (2.0, rate);
    float worst = 0.0f;
    int corners = 0;

    for (int bits = 0; bits < 128; ++bits)
    {
        const auto corner = [bits] (int bit) { return (bits >> bit & 1) != 0 ? 100.0f : 0.0f; };
        auto node = nodes::make ("dawg.loopReverb");
        harness::RenderOptions options;
        options.params = { { "feedback", corner (0) },
                           { "diffusion", corner (1) },
                           { "size", corner (2) },
                           { "earlyTiming", corner (3) },
                           { "width", corner (4) },
                           { "thin", corner (5) },
                           { "modulation", corner (6) },
                           { "damp", 0.0f },
                           { "mix", 100.0f } };
        options.tailSamples = static_cast<int> (rate);
        const auto output = harness::render (*node, input, options);

        for (const auto* channel : { &output.left, &output.right })
            for (const auto sample : *channel)
            {
                CHECK (std::isfinite (sample));
                worst = std::max (worst, std::abs (sample));
            }

        ++corners;
    }

    report ("largest sample over " + std::to_string (corners) + " corners: " + fixed (worst));
    CHECK (worst < 8.0f);
}

TEST ("5.11: after a change, every delay and gain reaches the same state as a node prepared with the new settings")
{
    // Diffusion 0 has no modulation, so once everything has settled the responses must be identical.
    const Params before { { "diffusion", 0.0f }, { "mix", 100.0f } };
    const Params after { { "diffusion", 0.0f },    { "mix", 100.0f }, { "size", 90.0f },     { "feedback", 70.0f },
                         { "earlyTiming", 80.0f }, { "damp", 60.0f }, { "predelay", 100.0f } };
    const auto impulseAt = static_cast<size_t> (2.0 * rate); // well after every glide, ramp and fade has ended
    auto input = harness::silence (4.0, rate);
    input.left[impulseAt] = input.right[impulseAt] = 1.0f;

    const auto changed = measure::renderChanging (
        input, before, 64,
        [&after] (dawg::Node& node, size_t position)
        {
            if (position != 0)
                return;
            for (const auto& [id, value] : after)
                node.params().set (static_cast<size_t> (node.descriptor().indexOfParam (id)), value);
        });
    const auto fresh = measure::impulseResponse (after, 2.0);
    bool same = true;
    for (size_t i = 0; i < fresh.left.size(); ++i)
        same = same && changed.left[impulseAt + i] == fresh.left[i] && changed.right[impulseAt + i] == fresh.right[i];
    CHECK (same);
}

TEST ("5.10: a centred source's reverb stays centred, early part and tail")
{
    double worst = 0.0;

    // Width 75 % is 1.5's widest; beyond it (revision 1.6) a centred source must still stay centred.
    for (const auto width : { 0.0f, 52.5f, 75.0f, 100.0f })
        for (const auto timing : { 0.0f, 40.0f, 100.0f })
            for (const auto seed : { 1.0f, 2.0f, 3.0f })
            {
                const auto response = measure::impulseResponse (
                    { { "mix", 100.0f }, { "width", width }, { "earlyTiming", timing }, { "seed", seed } }, 2.0);
                const auto earlyEnd = 0.01 + 0.005 * std::pow (20.0, 0.55);

                for (const auto& [from, to] : { std::pair { 0.0, earlyEnd }, std::pair { 0.1, 2.0 } })
                    worst = std::max (worst, std::abs (measure::channelEnergyDb (response, 0, from, to)
                                                       - measure::channelEnergyDb (response, 1, from, to)));
            }

    report ("largest left/right energy difference for a mono impulse (early part, tail): " + fixed (worst) + " dB");
    // The early taps are balanced exactly; what remains is the tail's first few passes inside the short early window,
    // a handful of echoes whose left and right energies differ by chance (DESIGN.md 1.9). 1.5 dB is about the
    // smallest level difference between the ears that moves an image noticeably.
    CHECK (worst <= 1.5);
}
