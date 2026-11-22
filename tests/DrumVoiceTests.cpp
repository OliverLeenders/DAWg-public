#include "EffectTestUtils.h"
#include "TestUtils.h"
#include "dawg/DrumVoices.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The Drum Synth's voice models (docs/design/drum-synth.md, DR5-DR7, step 2): one hit of each rendered and measured.

namespace
{
struct Render
{
    std::vector<float> left;
    std::vector<float> right;
};

[[nodiscard]] Render render (const DrumHit& hit, double seconds, double rate = sampleRate)
{
    const auto count = static_cast<int> (seconds * rate);
    Render out { std::vector<float> (static_cast<size_t> (count)), std::vector<float> (static_cast<size_t> (count)) };
    DrumVoice voice;
    voice.prepare (rate);
    voice.trigger (hit);

    for (int offset = 0; offset < count; offset += blockSize)
    {
        const auto length = std::min (blockSize, count - offset);
        voice.render (out.left.data() + offset, out.right.data() + offset, length);
    }

    return out;
}

[[nodiscard]] size_t at (double seconds, double rate = sampleRate)
{
    return static_cast<size_t> (seconds * rate);
}

// Rising zero crossings in [from, to), in samples, placed between samples by linear interpolation.
[[nodiscard]] std::vector<double> risingCrossings (const std::vector<float>& x, size_t from, size_t to)
{
    std::vector<double> result;

    for (size_t i = std::max<size_t> (from, 1); i < std::min (to, x.size()); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            result.push_back (static_cast<double> (i - 1) + static_cast<double> (x[i - 1] / (x[i - 1] - x[i])));

    return result;
}

// The mean frequency over whole cycles between the first and last rising crossings in [from, to).
[[nodiscard]] double meanFrequency (const std::vector<float>& x, size_t from, size_t to)
{
    const auto crossings = risingCrossings (x, from, to);
    REQUIRE (crossings.size() >= 3);
    return static_cast<double> (crossings.size() - 1) * sampleRate / (crossings.back() - crossings.front());
}

[[nodiscard]] double peak (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    double result = 0.0;

    for (size_t i = from; i < std::min (to, x.size()); ++i)
        result = std::max (result, static_cast<double> (std::abs (x[i])));

    return result;
}

[[nodiscard]] double energy (const std::vector<float>& x, size_t from, size_t to)
{
    double result = 0.0;

    for (size_t i = from; i < std::min (to, x.size()); ++i)
        result += static_cast<double> (x[i]) * static_cast<double> (x[i]);

    return result;
}

// The time in seconds after which every 20 ms window stays below a level relative to the whole render's peak.
[[nodiscard]] double timeBelow (const std::vector<float>& x, double relative)
{
    const auto threshold = peak (x) * relative;
    const auto window = at (0.02);
    size_t last = 0;

    for (size_t from = 0; from + window <= x.size(); from += window / 4)
        if (peak (x, from, from + window) >= threshold)
            last = from + window;

    return static_cast<double> (last) / sampleRate;
}

// A signal through two cascaded SVF stages of one mode at a cutoff.
[[nodiscard]] std::vector<float> filtered (const std::vector<float>& x, SvfMode mode, double cutoff,
                                           double rate = sampleRate)
{
    const auto svf = SvfCoefficients::bilinear (mode, cutoff, std::numbers::sqrt2, rate);
    SvfState a;
    SvfState b;
    std::vector<float> result (x.size());

    for (size_t i = 0; i < x.size(); ++i)
        result[i] = b.process (svf, a.process (svf, x[i]));

    return result;
}

// The magnitude spectrum of the first fftSize samples, Hann-windowed (zero-padded if shorter).
[[nodiscard]] std::vector<double> spectrumFromStart (const std::vector<float>& x)
{
    std::vector<float> padded (static_cast<size_t> (fftSize), 0.0f);
    std::copy_n (x.begin(), std::min (x.size(), padded.size()), padded.begin());
    return spectrum (padded);
}

[[nodiscard]] int binOf (double frequency)
{
    return static_cast<int> (std::lround (frequency * fftSize / sampleRate));
}

// The frequency of the largest bin within ±10 % of a frequency.
[[nodiscard]] double peakNear (const std::vector<double>& magnitudes, double frequency)
{
    const auto lo = binOf (0.9 * frequency);
    const auto hi = binOf (1.1 * frequency);
    auto best = lo;

    for (auto b = lo; b <= hi; ++b)
        if (magnitudes[static_cast<size_t> (b)] > magnitudes[static_cast<size_t> (best)])
            best = b;

    return static_cast<double> (best) * sampleRate / fftSize;
}

[[nodiscard]] DrumHit pureKick()
{
    return DrumHit::defaults (DrumModel::kick).set ("click", 0.0f).set ("drive", 0.0f);
}
} // namespace

TEST_CASE ("Drum voices: every model has its parameters and defaults", "[drums][voices]")
{
    CHECK (drumCommonParams().size() == drumCommonCount);
    CHECK (drumModelParams (DrumModel::off).empty());

    for (size_t m = 1; m < drumModelCount; ++m)
    {
        const auto model = static_cast<DrumModel> (m);
        const auto params = drumModelParams (model);
        INFO (drumModelName (model));
        CHECK (! params.empty());
        CHECK (params.size() <= maxDrumModelParams);

        for (const auto& p : params)
        {
            CHECK (p.defaultValue >= p.minValue);
            CHECK (p.defaultValue <= p.maxValue);
        }

        const auto hit = DrumHit::defaults (model);

        for (size_t i = 0; i < params.size(); ++i)
            CHECK (hit.values[i] == params[i].defaultValue);
    }

    CHECK (std::string (drumModelId (DrumModel::cymbal)) == "cymbal");
    CHECK (std::string (drumModelName (DrumModel::hat)) == "Hat");
}

TEST_CASE ("Drum voices: each model at its defaults peaks between -12 and 0 dBFS and ends", "[drums][voices]")
{
    for (size_t m = 1; m < drumModelCount; ++m)
    {
        const auto model = static_cast<DrumModel> (m);
        INFO (drumModelName (model));

        DrumVoice voice;
        voice.prepare (sampleRate);
        voice.trigger (DrumHit::defaults (model));
        CHECK (voice.active());

        std::vector<float> left (at (5.0));
        std::vector<float> right (left.size());

        for (size_t offset = 0; offset < left.size(); offset += blockSize)
            voice.render (left.data() + offset, right.data() + offset,
                          static_cast<int> (std::min<size_t> (blockSize, left.size() - offset)));

        const auto loudest = std::max (peak (left), peak (right));
        CHECK (loudest <= 1.0);
        CHECK (loudest >= 0.25);
        CHECK_FALSE (voice.active());
        CHECK (peak (left, left.size() - at (0.1)) == 0.0);
    }
}

TEST_CASE ("Kick: the pitch sweeps from Pitch x 2^(Sweep/12) down to Pitch", "[drums][voices][kick]")
{
    constexpr double pitch = 50.0;
    constexpr double sweep = 24.0;
    constexpr double tau = 0.2;
    const auto hit =
        pureKick().set ("pitch", 50.0f).set ("sweep", 24.0f).set ("sweepTime", 200.0f).set ("decay", 400.0f);
    const auto out = render (hit, 1.5);

    // The first whole cycle, against the sweep's frequency at its middle.
    const auto crossings = risingCrossings (out.left, 0, at (0.05));
    REQUIRE (crossings.size() >= 2);
    const auto middle = 0.5 * (crossings[0] + crossings[1]) / sampleRate;
    const auto expected = pitch * std::exp2 (sweep * std::exp (-middle / tau) / 12.0);
    CHECK_THAT (sampleRate / (crossings[1] - crossings[0]), WithinRel (expected, 0.03));

    // A short sweep has landed after five time constants.
    const auto settled = render (pureKick().set ("pitch", 50.0f).set ("sweepTime", 40.0f).set ("decay", 400.0f), 1.0);
    CHECK_THAT (meanFrequency (settled.left, at (0.25), at (0.8)), WithinRel (pitch, 0.01));

    // Tune moves it by semitones.
    const auto tuned = render (pureKick().set ("pitch", 50.0f).set ("sweepTime", 40.0f).set ("tune", 12.0f), 1.0);
    CHECK_THAT (meanFrequency (tuned.left, at (0.25), at (0.5)), WithinRel (2.0 * pitch, 0.01));
}

TEST_CASE ("Kick: decays to -60 dB at its decay time, scaled by Decay", "[drums][voices][kick]")
{
    for (const auto decay : { 100.0f, 200.0f })
    {
        const auto out = render (pureKick().set ("sweep", 0.0f).set ("decay", decay), 2.0);
        CHECK_THAT (timeBelow (out.left, 1.0e-3), WithinRel (0.45 * decay / 100.0f, 0.1));
    }

    // Hold keeps it at full level first.
    const auto held = render (pureKick().set ("sweep", 0.0f).set ("hold", 100.0f), 2.0);
    CHECK_THAT (timeBelow (held.left, 1.0e-3), WithinRel (0.55, 0.1));
}

TEST_CASE ("Kick: the click is high and early", "[drums][voices][kick]")
{
    const auto withClick = filtered (render (pureKick().set ("click", 100.0f), 0.1).left, SvfMode::highPass, 2000.0);
    const auto without = filtered (render (pureKick(), 0.1).left, SvfMode::highPass, 2000.0);

    const auto early = energy (withClick, 0, at (0.005));
    CHECK (early > 10.0 * energy (without, 0, at (0.005)));
    CHECK (energy (withClick, at (0.02), at (0.04)) < 1.0e-3 * early);
}

TEST_CASE ("Snare: body modes at Pitch and 1.75 x Pitch; Snappy is the noise", "[drums][voices][snare]")
{
    const auto body = DrumHit::defaults (DrumModel::snare)
                          .set ("snappy", 0.0f)
                          .set ("tone", 0.0f)
                          .set ("sweep", 0.0f)
                          .set ("pitch", 200.0f);
    const auto out = render (body, 1.0);
    const auto magnitudes = spectrumFromStart (out.left);

    CHECK_THAT (peakNear (magnitudes, 200.0), WithinRel (200.0, 0.02));
    CHECK_THAT (peakNear (magnitudes, 350.0), WithinRel (350.0, 0.02));

    // Without the snares there's nothing above 4 kHz.
    CHECK (energy (filtered (out.left, SvfMode::highPass, 4000.0), 0, out.left.size())
           < 1.0e-4 * energy (out.left, 0, out.left.size()));

    // Noise tone moves the snares' brightness.
    const auto noisy = DrumHit::defaults (DrumModel::snare).set ("snappy", 100.0f).set ("tone", 100.0f);
    const auto lowShare = [] (const std::vector<float>& x)
    {
        const auto band = filtered (filtered (x, SvfMode::highPass, 2000.0), SvfMode::lowPass, 5000.0);
        return energy (band, 0, band.size()) / energy (x, 0, x.size());
    };
    const auto dark = lowShare (render (DrumHit (noisy).set ("noiseTone", 2000.0f), 0.5).left);
    const auto bright = lowShare (render (DrumHit (noisy).set ("noiseTone", 8000.0f), 0.5).left);
    CHECK (dark > 3.0 * bright);
}

TEST_CASE ("Tom: settles on Pitch, with the 1.59 mode as an overtone", "[drums][voices][tom]")
{
    const auto plain = DrumHit::defaults (DrumModel::tom)
                           .set ("overtones", 0.0f)
                           .set ("stick", 0.0f)
                           .set ("pitch", 110.0f)
                           .set ("sweepTime", 50.0f)
                           .set ("decay", 300.0f);
    CHECK_THAT (meanFrequency (render (plain, 1.0).left, at (0.3), at (0.7)), WithinRel (110.0, 0.01));

    // The sweep drops onto the pitch; with no sweep the start is the pitch already.
    const auto swept = render (plain, 1.0);
    CHECK (meanFrequency (swept.left, 0, at (0.03)) > 1.05 * 110.0);
    const auto flat = render (DrumHit (plain).set ("sweep", 0.0f), 1.0);
    CHECK_THAT (meanFrequency (flat.left, 0, at (0.05)), WithinRel (110.0, 0.01));

    // Overtones bring in the mode at 1.59.
    const auto bare = spectrumFromStart (render (DrumHit (plain).set ("sweep", 0.0f), 1.0).left);
    const auto rich =
        spectrumFromStart (render (DrumHit (plain).set ("sweep", 0.0f).set ("overtones", 100.0f), 1.0).left);
    const auto mode = binOf (1.59 * 110.0);
    CHECK (levelDb (rich, mode) - levelDb (rich, binOf (110.0)) > -20.0);
    CHECK (levelDb (bare, mode) - levelDb (bare, binOf (110.0)) < -40.0);
}

TEST_CASE ("Hat: metal above 1 kHz only, closed shorter than open, the same at 96 kHz", "[drums][voices][hat]")
{
    const auto hat = DrumHit::defaults (DrumModel::hat);
    const auto magnitudes = spectrumFromStart (render (hat, 0.4).left);
    const auto loudest = *std::max_element (magnitudes.begin(), magnitudes.end());
    const auto lowest = *std::max_element (magnitudes.begin() + 1, magnitudes.begin() + binOf (1000.0));
    CHECK (20.0 * std::log10 (lowest / loudest) < -40.0);

    const auto closed = timeBelow (render (DrumHit (hat).set ("decay", 40.0f), 2.0).left, 1.0e-3);
    const auto open = timeBelow (render (DrumHit (hat).set ("decay", 300.0f), 2.0).left, 1.0e-3);
    CHECK (open > 5.0 * closed);
}

TEST_CASE ("Hat: the metal's squares are band-limited", "[drums][voices][hat]")
{
    // A steady bank, at Tune 0 and an octave up. Everything that isn't an odd harmonic of one of the six squares is
    // folded back: its share of the energy is the aliasing. Two-point polyBLEP keeps it near -39 dB at Tune 0 and -35 dB
    // an octave up, 15 dB under naive squares'; the high-passes after it take most of what folds below their cutoff.
    const auto ratio = GENERATE (1.0, 2.0);
    const auto aliasDbOf = [ratio] (const std::vector<float>& x)
    {
        const auto magnitudes = spectrum (x);
        std::vector<bool> harmonic (magnitudes.size(), false);

        for (const auto f : DrumMetal::frequencies)
            for (auto k = 1.0; k * f * ratio < 0.5 * sampleRate; k += 2.0)
            {
                const auto centre = binOf (k * f * ratio);

                for (auto b = std::max (0, centre - 4); b <= std::min (fftSize / 2, centre + 4); ++b)
                    harmonic[static_cast<size_t> (b)] = true;
            }

        double alias = 0.0;
        double total = 0.0;

        for (size_t b = 2; b < magnitudes.size(); ++b)
        {
            const auto power = magnitudes[b] * magnitudes[b];
            total += power;
            alias += harmonic[b] ? 0.0 : power;
        }

        return 10.0 * std::log10 (alias / total);
    };

    DrumMetal metal;
    metal.start (ratio, sampleRate);
    std::vector<float> limited (static_cast<size_t> (2 * fftSize));

    for (auto& v : limited)
        v = static_cast<float> (metal.next());

    // The same squares without polyBLEP, to show the measure sees aliasing.
    std::array<double, 6> phases {};
    std::vector<float> naive (limited.size());

    for (auto& v : naive)
    {
        double sum = 0.0;

        for (size_t i = 0; i < phases.size(); ++i)
        {
            sum += phases[i] < 0.5 ? 1.0 : -1.0;
            phases[i] += DrumMetal::frequencies[i] * ratio / sampleRate;
            phases[i] -= std::floor (phases[i]);
        }

        v = static_cast<float> (sum / 6.0);
    }

    const auto limitedDb = aliasDbOf (limited);
    const auto naiveDb = aliasDbOf (naive);
    INFO (ratio << ": " << limitedDb << " dB band-limited, " << naiveDb << " dB naive");
    CHECK (limitedDb < (ratio == 1.0 ? -37.0 : -33.0));
    CHECK (naiveDb - limitedDb > 12.0);
}

TEST_CASE ("Cymbal: Width spreads it", "[drums][voices][cymbal]")
{
    const auto mono = render (DrumHit::defaults (DrumModel::cymbal).set ("width", 0.0f), 0.5);
    CHECK (mono.left == mono.right);

    const auto wide = render (DrumHit::defaults (DrumModel::cymbal).set ("width", 100.0f), 0.5);
    const auto dot = std::inner_product (wide.left.begin(), wide.left.end(), wide.right.begin(), 0.0);
    const auto norm = std::sqrt (energy (wide.left, 0, wide.left.size()) * energy (wide.right, 0, wide.right.size()));
    CHECK (dot / norm < 0.9);
}

TEST_CASE ("Cymbal: many modes and a wash, not a bell", "[drums][voices][cymbal]")
{
    // 0.3 s in, from 500 Hz to 12 kHz: the loudest bin over the median one. The cymbal of six squares and four bell
    // partials measured 59 dB as a crash, 46 dB as a ride (section 9.2); a crash in a real kit is far smoother.
    const auto tonality = [] (const DrumHit& hit)
    {
        const auto out = render (hit, 1.0).left;
        const auto magnitudes = spectrumFromStart ({ out.begin() + static_cast<std::ptrdiff_t> (at (0.3)), out.end() });
        std::vector<double> band (magnitudes.begin() + binOf (500.0), magnitudes.begin() + binOf (12000.0));
        std::ranges::sort (band);
        return 20.0 * std::log10 (band.back() / band[band.size() / 2]);
    };

    for (const auto seed : { 1U, 2U, 3U, 4U })
    {
        auto crash = DrumHit::defaults (DrumModel::cymbal);
        crash.seed = seed;
        auto ride = DrumHit (crash).set ("ride", 100.0f).set ("wash", 25.0f).set ("sizzle", 12.0f);
        INFO (seed);
        CHECK (tonality (crash) < 42.0);
        CHECK (tonality (ride) < 38.0);

        // The wash makes it smoother.
        CHECK (tonality (DrumHit (crash).set ("wash", 0.0f).set ("sizzle", 0.0f)) > tonality (crash) + 6.0);
    }
}

TEST_CASE ("Cymbal: a crash's coupling never rings on by itself", "[drums][voices][cymbal]")
{
    // Too much coupling makes the modes drive themselves: the cymbal grows louder after the hit instead of decaying.
    // Across the extremes that make the modes loudest, nothing after the first 50 ms is louder than the hit.
    for (const auto tone : { 2000.0f, 12000.0f })
        for (const auto colour : { 0.0f, 100.0f })
            for (const auto tune : { -24.0f, 0.0f, 24.0f })
                for (const auto decay : { 10.0f, 400.0f })
                {
                    auto hit = DrumHit::defaults (DrumModel::cymbal)
                                   .set ("tone", tone)
                                   .set ("colour", colour)
                                   .set ("tune", tune)
                                   .set ("decay", decay)
                                   .set ("level", 0.0f)
                                   .set ("wash", 0.0f)
                                   .set ("sizzle", 0.0f); // the modes alone: the wash swells by design
                    hit.seed = static_cast<std::uint32_t> (tone + colour + tune + decay);
                    const auto out = render (hit, 1.0).left;
                    INFO (tone << " Hz, colour " << colour << ", tune " << tune << ", decay " << decay);
                    REQUIRE (std::ranges::all_of (out, [] (float x) { return std::isfinite (x); }));
                    CHECK (peak (out, at (0.05)) < peak (out, 0, at (0.05)));
                }
    // Nor does a roll on one voice, each hit adding to the last: it dies away once the roll stops.
    const auto crash =
        DrumHit::defaults (DrumModel::cymbal).set ("wash", 0.0f).set ("sizzle", 0.0f).set ("level", 0.0f);
    const auto step = static_cast<int> (at (0.03));
    std::vector<float> l (at (2.0)), r (l.size());
    DrumVoice voice;
    voice.prepare (sampleRate);
    voice.trigger (crash);

    for (int k = 0; k < 20; ++k)
    {
        voice.render (l.data() + k * step, r.data() + k * step, step);
        REQUIRE (voice.restrike (crash));
    }

    const auto rolled = static_cast<size_t> (20 * step);
    voice.render (l.data() + rolled, r.data() + rolled, static_cast<int> (l.size() - rolled));
    REQUIRE (std::ranges::all_of (l, [] (float x) { return std::isfinite (x); }));
    CHECK (peak (l, rolled + at (0.5), rolled + at (0.6)) < 0.5 * peak (l, rolled, rolled + at (0.1)));
}

TEST_CASE ("Cymbal: Ride rings longer, Colour and Tone brighten", "[drums][voices][cymbal]")
{
    const auto crash = DrumHit::defaults (DrumModel::cymbal).set ("wash", 0.0f).set ("sizzle", 0.0f);
    const auto ride = DrumHit (crash).set ("ride", 100.0f);
    CHECK (timeBelow (render (ride, 6.0).left, 1.0e-2) > timeBelow (render (crash, 6.0).left, 1.0e-2) + 0.3);

    const auto brightness = [] (const DrumHit& hit)
    {
        const auto out = render (hit, 0.5).left;
        return energy (filtered (out, SvfMode::highPass, 6000.0), 0, out.size()) / energy (out, 0, out.size());
    };

    CHECK (brightness (DrumHit (crash).set ("colour", 100.0f))
           > 1.5 * brightness (DrumHit (crash).set ("colour", 0.0f)));
    CHECK (brightness (DrumHit (crash).set ("tone", 12000.0f))
           > 1.5 * brightness (DrumHit (crash).set ("tone", 2000.0f)));
}

TEST_CASE ("Cymbal: a restrike rings the same cymbal on", "[drums][voices][cymbal]")
{
    // Two hits on one voice: a ride's modes are linear (a crash's couple), so the first keeps ringing under the
    // second. A new voice for the second would cut nothing either, but it costs a second bank of modes.
    const auto hit = DrumHit::defaults (DrumModel::cymbal).set ("ride", 100.0f).set ("wash", 0.0f).set ("sizzle", 0.0f);
    const auto half = static_cast<int> (at (0.5));
    std::vector<float> l (2 * static_cast<size_t> (half)), r (l.size());
    DrumVoice voice;
    voice.prepare (sampleRate);
    voice.trigger (hit);
    voice.render (l.data(), r.data(), half);
    REQUIRE (voice.restrike (hit));
    voice.render (l.data() + half, r.data() + half, half);

    // The second half is the first hit's tail plus the same hit again, to within the stick's noise.
    const auto once = render (hit, 1.0).left;
    std::vector<float> expected (once.size());

    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = once[i] + (i >= static_cast<size_t> (half) ? once[i - static_cast<size_t> (half)] : 0.0f);

    std::vector<float> error (expected.size());

    for (size_t i = 0; i < error.size(); ++i)
        error[i] = l[i] - expected[i];

    CHECK (energy (error, static_cast<size_t> (half), error.size())
           < 0.2 * energy (expected, static_cast<size_t> (half), expected.size()));

    // Only a sounding cymbal restrikes.
    DrumVoice kick;
    kick.prepare (sampleRate);
    kick.trigger (DrumHit::defaults (DrumModel::kick));
    CHECK_FALSE (kick.restrike (DrumHit::defaults (DrumModel::cymbal)));
    CHECK_FALSE (kick.restrike (DrumHit::defaults (DrumModel::kick)));
    DrumVoice silent;
    silent.prepare (sampleRate);
    CHECK_FALSE (silent.restrike (hit));
    voice.choke (0.005);
    CHECK_FALSE (voice.restrike (hit));
}

TEST_CASE ("Clap: its bursts, then the tail", "[drums][voices][clap]")
{
    const auto clap = DrumHit::defaults (DrumModel::clap).set ("bursts", 4.0f).set ("spread", 10.0f);
    const auto out = render (DrumHit (clap).set ("tail", 0.0f), 0.2).left;

    // Each burst starts loud after a quiet gap.
    for (int k = 1; k < 4; ++k)
    {
        const auto start = at (0.01 * k);
        INFO (k);
        CHECK (energy (out, start, start + at (0.002)) > 20.0 * energy (out, start - at (0.002), start));
    }

    // Nothing after the last burst without a tail; something with one.
    CHECK (peak (out, at (0.06)) < 1.0e-3 * peak (out));
    const auto tailed = render (DrumHit (clap).set ("tail", 100.0f), 0.2).left;
    CHECK (peak (tailed, at (0.06), at (0.08)) > 0.05 * peak (tailed));
}

TEST_CASE ("Rim: short", "[drums][voices][rim]")
{
    const auto out = render (DrumHit::defaults (DrumModel::rim), 0.3).left;
    CHECK (timeBelow (out, 1.0e-3) < 0.08);
}

TEST_CASE ("Drum voices: velocity sets the level by the pad's Velocity amount (DR7)", "[drums][voices]")
{
    const auto level = [] (float amount, float velocity)
    {
        auto hit = pureKick().set ("velocity", amount);
        hit.velocity = velocity;
        return peak (render (hit, 0.2).left);
    };

    const auto soft = 32.0 / 127.0;
    CHECK_THAT (level (0.0f, 32.0f) / level (0.0f, 127.0f), WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (level (100.0f, 32.0f) / level (100.0f, 127.0f), WithinRel (soft * soft, 0.01));
    CHECK_THAT (level (50.0f, 32.0f) / level (50.0f, 127.0f), WithinRel (0.5 + 0.5 * soft * soft, 0.01));

    // The accent: a hard hit's click is brighter.
    auto hard = DrumHit::defaults (DrumModel::kick).set ("velocity", 100.0f);
    const auto click = [] (const DrumHit& hit)
    {
        const auto out = render (hit, 0.01).left;
        return energy (filtered (out, SvfMode::highPass, 6000.0), 0, out.size()) / energy (out, 0, out.size());
    };
    auto softer = hard;
    softer.velocity = 127.0f;
    hard.velocity = 127.0f;
    softer.set ("velocity", 0.0f);
    CHECK (click (hard) > click (softer));
}

TEST_CASE ("Drum voices: level, pan and determinism", "[drums][voices]")
{
    const auto left = render (DrumHit::defaults (DrumModel::snare).set ("pan", -100.0f), 0.2);
    CHECK (peak (left.right) == 0.0);
    CHECK (peak (left.left) > 0.0);

    const auto quiet = render (pureKick().set ("level", -6.0f), 0.2);
    CHECK_THAT (peak (quiet.left) / peak (render (pureKick(), 0.2).left), WithinRel (0.5012, 0.001));

    // The same seed sounds the same; another seed doesn't (DR11).
    auto hit = DrumHit::defaults (DrumModel::snare);
    hit.seed = 7;
    CHECK (render (hit, 0.2).left == render (hit, 0.2).left);
    auto other = hit;
    other.seed = 8;
    CHECK (render (hit, 0.2).left != render (other, 0.2).left);
}

TEST_CASE ("Drum voices: a choke fades out in its time and frees the voice", "[drums][voices]")
{
    DrumVoice voice;
    voice.prepare (sampleRate);
    voice.trigger (DrumHit::defaults (DrumModel::cymbal));

    std::vector<float> l (blockSize);
    std::vector<float> r (blockSize);
    voice.render (l.data(), r.data(), blockSize);
    voice.choke (0.005);

    std::vector<float> fl (at (0.02));
    std::vector<float> fr (fl.size());
    voice.render (fl.data(), fr.data(), static_cast<int> (fl.size()));

    CHECK_FALSE (voice.active());
    CHECK (peak (fl, at (0.005) + 1) == 0.0);
    CHECK (peak (fl, 0, at (0.001)) > 0.0);

    // A silent voice ignores a choke, and a new trigger plays again.
    voice.choke (0.005);
    CHECK_FALSE (voice.active());
    voice.trigger (DrumHit::defaults (DrumModel::kick));
    CHECK (voice.active());
}

// Writes one hit of each model, at its defaults, to a WAV in DAWG_DRUM_WAVS, for listening. Hidden.
TEST_CASE ("Drum voices: write a WAV of each model", "[.drum-wavs]")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("DAWG_DRUM_WAVS", {});
    REQUIRE (folder.isNotEmpty());
    const juce::File directory (folder);
    REQUIRE (directory.createDirectory());

    for (size_t m = 1; m < drumModelCount; ++m)
    {
        const auto model = static_cast<DrumModel> (m);
        const auto out = render (DrumHit::defaults (model), 2.0);
        const auto file = directory.getChildFile (juce::String (drumModelId (model)) + ".wav");
        file.deleteFile();

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        const std::array<const float*, 2> channels { out.left.data(), out.right.data() };
        REQUIRE (writer->writeFromFloatArrays (channels.data(), 2, static_cast<int> (out.left.size())));
    }
}
