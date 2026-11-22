#include "Pattern.h"

#include <cmath>
#include <utility>

namespace dawg::reverb
{

namespace
{
// Early taps (DESIGN.md 1.3): the first between 4 and 8 % of the span, the last between 97 and 100 %, the others
// stratified in between and denser towards the end.
constexpr float firstTapMin = 0.04f;
constexpr float firstTapRange = 0.04f;
constexpr float lastTapMin = 0.97f;
constexpr float lastTapRange = 0.03f;
constexpr float tapSpacingPower = 0.7f;
constexpr float minTapGain = 0.4f;

// FDN line and in-loop allpass lengths as multiples of the size length (DESIGN.md 1.1, 2.3).
constexpr float shortestLine = 0.3f;
constexpr float longestLine = 1.35f;
constexpr float shortestAllpass = 0.1f;
constexpr float longestAllpass = 0.2f;

// Modulation rates (DESIGN.md 2.6).
constexpr float slowestModulationHz = 0.1f;
constexpr float fastestModulationHz = 0.4f;
constexpr float slowestChorusHz = 1.0f;
constexpr float fastestChorusHz = 2.0f;

// Distinct streams per seed, so that early patterns and the tail don't share random numbers.
constexpr uint64_t streamEarly = 0x6a09e667f3bcc909ull;
constexpr uint64_t streamTail = 0xbb67ae8584caa73bull;
constexpr uint64_t streamPan = 0x3c6ef372fe94f82bull;
constexpr uint64_t streamChorus = 0xa54ff53a5f1d36f1ull;

EarlyPattern makeEarly (Random& random) noexcept
{
    EarlyPattern pattern;
    constexpr auto last = tapsPerPattern - 1;
    pattern.position[0] = firstTapMin + firstTapRange * random.uniform();
    pattern.position[last] = lastTapMin + lastTapRange * random.uniform();
    pattern.gain[0] = 1.0f;

    const auto first = pattern.position[0];
    const auto span = pattern.position[last] - first;

    for (size_t i = 1; i < last; ++i)
    {
        // Stratified: tap i draws within its own slice, so taps are ordered and never coincide.
        const auto slice = (static_cast<float> (i - 1) + random.uniform()) / static_cast<float> (last - 1);
        pattern.position[i] = first + span * (0.02f + 0.96f * std::pow (slice, tapSpacingPower));
        const auto magnitude = minTapGain + (1.0f - minTapGain) * random.uniform();
        pattern.gain[i] = random.uniform() < 0.5f ? -magnitude : magnitude;
    }

    const auto lastMagnitude = minTapGain + (1.0f - minTapGain) * random.uniform();
    pattern.gain[last] = random.uniform() < 0.5f ? -lastMagnitude : lastMagnitude;
    return pattern;
}
} // namespace

uint64_t Random::next() noexcept
{
    m_state += 0x9e3779b97f4a7c15ull;
    auto z = m_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

float Random::uniform() noexcept
{
    // The top 24 bits give every float in [0, 1) with spacing 2^-24.
    return static_cast<float> (next() >> 40) * (1.0f / 16777216.0f);
}

Pattern makePattern (int seed) noexcept
{
    Pattern pattern;
    const auto seedBits = static_cast<uint64_t> (seed);

    Random early (seedBits ^ streamEarly);
    for (auto& p : pattern.early)
        p = makeEarly (early);

    // Directions: a separate stream, so adding them (revision 1.3) left every seed's times and gains as they were.
    Random pan (seedBits ^ streamPan);

    for (auto& p : pattern.early)
    {
        std::array<size_t, tapsPerPattern> slot {};
        for (size_t k = 0; k < tapsPerPattern; ++k)
            slot[k] = k;

        for (size_t k = tapsPerPattern - 1; k > 0; --k)
            std::swap (slot[k], slot[static_cast<size_t> (pan.next() % (k + 1))]);

        for (size_t k = 0; k < tapsPerPattern; ++k)
            p.pan[k] = (static_cast<float> (slot[k]) + pan.uniform()) / static_cast<float> (tapsPerPattern);
    }

    Random tail (seedBits ^ streamTail);

    // Line lengths: one log-spaced stratum per line, assigned to lines in a shuffled order so the left (0-7) and
    // right (8-15) halves both get short and long lines.
    std::array<size_t, numLines> order {};
    for (size_t i = 0; i < numLines; ++i)
        order[i] = i;

    for (size_t i = numLines - 1; i > 0; --i)
        std::swap (order[i], order[static_cast<size_t> (tail.next() % (i + 1))]);

    const auto logRange = std::log (longestLine / shortestLine);

    for (size_t i = 0; i < numLines; ++i)
    {
        const auto stratum = (static_cast<float> (order[i]) + tail.uniform()) / static_cast<float> (numLines);
        pattern.lineLength[i] = shortestLine * std::exp (logRange * stratum);
        pattern.allpassLength[i] = shortestAllpass + (longestAllpass - shortestAllpass) * tail.uniform();
        pattern.modulationRate[i] = slowestModulationHz + (fastestModulationHz - slowestModulationHz) * tail.uniform();
        pattern.modulationPhase[i] = tail.uniform();
    }

    // The chorus (revision 1.6): its own stream, so every seed's earlier choices stay as they were.
    Random chorus (seedBits ^ streamChorus);

    for (size_t i = 0; i < numLines; ++i)
    {
        pattern.chorusRate[i] = slowestChorusHz + (fastestChorusHz - slowestChorusHz) * chorus.uniform();
        pattern.chorusPhase[i] = chorus.uniform();
    }

    return pattern;
}

} // namespace dawg::reverb
