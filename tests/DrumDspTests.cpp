#include "TestUtils.h"
#include "dawg/DrumEnvelopes.h"
#include "dawg/PolyBlep.h"
#include "dawg/Svf.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The shared DSP blocks of the Drum Synth (docs/design/drum-synth.md, DR5, step 1): the SVF the Filter now shares,
// the polyBLEP residuals the Oscillator now shares, and the voices' envelopes.

namespace
{
// The steady-state peak of an SVF's response to a unit sine, over the last of two seconds.
[[nodiscard]] double sineGain (const SvfCoefficients& svf, double frequency)
{
    SvfState state;
    const auto total = static_cast<int> (2.0 * sampleRate);
    double peak = 0.0;

    for (int n = 0; n < total; ++n)
    {
        const auto x = static_cast<float> (std::sin (2.0 * std::numbers::pi * frequency * n / sampleRate));
        const auto y = state.process (svf, x);

        if (n >= total / 2)
            peak = std::max (peak, static_cast<double> (std::abs (y)));
    }

    return peak;
}

// The level of an envelope at each sample of a length.
[[nodiscard]] std::vector<float> levels (DrumEnvelope& envelope, int count)
{
    std::vector<float> result;
    result.reserve (static_cast<size_t> (count));

    for (int n = 0; n < count; ++n)
        result.push_back (envelope.next());

    return result;
}
} // namespace

TEST_CASE ("SVF: the bilinear modes have their prototypes' gains", "[drums][svf]")
{
    constexpr double cutoff = 1000.0;
    const auto butterworth = std::numbers::sqrt2;

    // A Butterworth low-pass is -3 dB at its cutoff, and passes low frequencies.
    CHECK_THAT (sineGain (SvfCoefficients::bilinear (SvfMode::lowPass, cutoff, butterworth, sampleRate), cutoff),
                WithinAbs (std::numbers::sqrt2 / 2.0, 0.005));
    CHECK_THAT (sineGain (SvfCoefficients::bilinear (SvfMode::lowPass, cutoff, butterworth, sampleRate), 50.0),
                WithinAbs (1.0, 0.005));

    // A high-pass mirrors it.
    CHECK_THAT (sineGain (SvfCoefficients::bilinear (SvfMode::highPass, cutoff, butterworth, sampleRate), cutoff),
                WithinAbs (std::numbers::sqrt2 / 2.0, 0.005));
    CHECK (sineGain (SvfCoefficients::bilinear (SvfMode::highPass, cutoff, butterworth, sampleRate), 50.0) < 0.01);

    // The band-pass peaks at 1 / k at the cutoff; the notch is silent there.
    CHECK_THAT (sineGain (SvfCoefficients::bilinear (SvfMode::bandPass, cutoff, 0.5, sampleRate), cutoff),
                WithinAbs (2.0, 0.02));
    CHECK (sineGain (SvfCoefficients::bilinear (SvfMode::notch, cutoff, butterworth, sampleRate), cutoff) < 0.01);
}

TEST_CASE ("SVF: fromBiquad realises the biquad it is given", "[drums][svf]")
{
    // A cookbook low-pass at 2 kHz, Q 2, normalised to a0 = 1.
    const auto w0 = 2.0 * std::numbers::pi * 2000.0 / sampleRate;
    const auto alpha = std::sin (w0) / (2.0 * 2.0);
    const auto a0 = 1.0 + alpha;
    const auto b0 = (1.0 - std::cos (w0)) / 2.0 / a0;
    const auto b1 = (1.0 - std::cos (w0)) / a0;
    const auto b2 = b0;
    const auto a1 = -2.0 * std::cos (w0) / a0;
    const auto a2 = (1.0 - alpha) / a0;

    const auto svf = SvfCoefficients::fromBiquad (b0, b1, b2, a1, a2);
    SvfState state;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    for (int n = 0; n < 2000; ++n)
    {
        const auto x = n == 0 ? 1.0 : 0.0;
        const auto expected = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = expected;

        REQUIRE_THAT (static_cast<double> (state.process (svf, static_cast<float> (x))), WithinAbs (expected, 1.0e-5));
    }

    state.reset();
    CHECK (state.ic1 == 0.0f);
    CHECK (state.ic2 == 0.0f);
}

TEST_CASE ("PolyBLEP: residuals are zero away from the step and continuous across it", "[drums][polyblep]")
{
    constexpr double dt = 0.01;

    CHECK (polyblep::blep (0.5, dt) == 0.0);
    CHECK (polyblep::blamp (0.5, dt) == 0.0);
    // The step's residual runs from -1 just after it to +1 just before it, and is 0 at the window's edges.
    CHECK_THAT (polyblep::blep (0.0, dt), WithinAbs (-1.0, 1.0e-12));
    CHECK_THAT (polyblep::blep (1.0 - 1.0e-12, dt), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (polyblep::blep (dt - 1.0e-12, dt), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (polyblep::blamp (0.0, dt), WithinAbs (1.0 / 6.0, 1.0e-12));
}

TEST_CASE ("Drum envelope: decays to -60 dB at its decay time", "[drums][envelope]")
{
    for (const auto decaySeconds : { 0.05, 0.3, 1.5 })
    {
        DrumEnvelope envelope;
        envelope.start (sampleRate, 0.0, 0.0, decaySeconds);
        const auto samples = levels (envelope, static_cast<int> (3.0 * decaySeconds * sampleRate));

        const auto crossing = std::find_if (samples.begin(), samples.end(), [] (float v) { return v < 1.0e-3f; });
        REQUIRE (crossing != samples.end());
        const auto seconds = static_cast<double> (crossing - samples.begin()) / sampleRate;
        CHECK_THAT (seconds, WithinRel (decaySeconds, 0.02));

        CHECK (samples.front() == 1.0f);
        CHECK (envelope.finished());
    }
}

TEST_CASE ("Drum envelope: attack and hold come before the decay", "[drums][envelope]")
{
    constexpr double attack = 0.01;
    constexpr double hold = 0.02;
    const auto attackSamples = static_cast<int> (attack * sampleRate);
    const auto holdSamples = static_cast<int> (hold * sampleRate);

    DrumEnvelope envelope;
    envelope.start (sampleRate, attack, hold, 0.2);
    const auto samples = levels (envelope, static_cast<int> (sampleRate));

    CHECK (samples[0] == 0.0f);
    CHECK_THAT (samples[static_cast<size_t> (attackSamples / 2)], WithinAbs (0.5, 0.001));
    CHECK_THAT (samples[static_cast<size_t> (attackSamples)], WithinAbs (1.0, 1.0e-6));

    // Full level through the hold, then falling.
    for (int n = attackSamples; n <= attackSamples + holdSamples; ++n)
        REQUIRE (samples[static_cast<size_t> (n)] == 1.0f);

    CHECK (samples[static_cast<size_t> (attackSamples + holdSamples + 1)] < 1.0f);

    // -60 dB at the decay time after the hold.
    const auto crossing =
        std::find_if (samples.begin(), samples.end(), [] (float v) { return v > 0.0f && v < 1.0e-3f; });
    REQUIRE (crossing != samples.end());
    const auto decayStart = attackSamples + holdSamples;
    CHECK_THAT (static_cast<double> ((crossing - samples.begin()) - decayStart) / sampleRate, WithinRel (0.2, 0.02));
}

TEST_CASE ("Drum envelope: a choke fades out over its time, from wherever it is", "[drums][envelope]")
{
    constexpr double fade = 0.005;
    const auto fadeSamples = static_cast<int> (fade * sampleRate);

    DrumEnvelope envelope;
    envelope.start (sampleRate, 0.0, 0.0, 2.0);
    std::ignore = levels (envelope, 1000);
    const auto before = envelope.next();

    envelope.choke (sampleRate, fade);
    CHECK (envelope.choking());
    const auto samples = levels (envelope, fadeSamples + 1);

    CHECK (samples.front() <= before);
    CHECK (std::is_sorted (samples.rbegin(), samples.rend())); // never rises
    CHECK (samples.back() == 0.0f);
    CHECK (envelope.finished());
    CHECK_FALSE (envelope.choking());

    // A finished envelope stays silent, choked or not.
    envelope.choke (sampleRate, fade);
    CHECK (envelope.next() == 0.0f);
    CHECK (envelope.finished());
}

TEST_CASE ("Pitch sweep: falls by e each time constant, in semitones", "[drums][envelope]")
{
    constexpr double sweep = 24.0;
    constexpr double seconds = 0.04;

    PitchSweep pitch;
    pitch.start (sampleRate, sweep, seconds);
    CHECK_THAT (pitch.nextRatio(), WithinAbs (4.0, 1.0e-9)); // two octaves up at the start

    PitchSweep again;
    again.start (sampleRate, sweep, seconds);
    const auto tau = static_cast<int> (seconds * sampleRate);

    for (int n = 0; n < tau; ++n)
        std::ignore = again.next();

    CHECK_THAT (again.next(), WithinRel (sweep / std::numbers::e, 1.0e-3));

    // After five time constants it is within 1 % of the note in frequency.
    for (int n = 0; n < 4 * tau; ++n)
        std::ignore = again.next();

    CHECK (std::exp2 (again.next() / 12.0) < 1.01);
}
