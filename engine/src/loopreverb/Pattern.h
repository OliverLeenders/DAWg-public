#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The seeded part of the reverb: early-reflection patterns and the tail's texture (SPEC.md 5.6; DESIGN.md 1.3, 1.8).

namespace dawg::reverb
{

/**
    Taps in each early-reflection pattern.
*/
inline constexpr size_t tapsPerPattern = 16;

/**
    Delay lines in the feedback delay network.
*/
inline constexpr size_t numLines = 16;

/**
    One early-reflection pattern, independent of Size: times as fractions of the early span.
*/
struct EarlyPattern
{
    /**
        Tap times as fractions of the early span, ascending, in (0, 1].
    */
    std::array<float, tapsPerPattern> position {};

    /**
        Tap gains, signed, at most 1 in magnitude; the first tap is +1.
    */
    std::array<float, tapsPerPattern> gain {};

    /**
        Tap directions for the side patterns, 0 (on the source's side) to 1 (furthest across), stratified: one tap per
        sixteenth of the range, in a seeded order.
    */
    std::array<float, tapsPerPattern> pan {};
};

/**
    Everything the seed chooses.
*/
struct Pattern
{
    /**
        The early patterns: shared (M), and the left and right inputs' own (A, B) (DESIGN.md 2.1).
    */
    std::array<EarlyPattern, 3> early {};

    /**
        Each line's length as a multiple of the size length, in [0.3, 1.35].
    */
    std::array<float, numLines> lineLength {};

    /**
        Each line's allpass length as a multiple of the size length, in [0.1, 0.2].
    */
    std::array<float, numLines> allpassLength {};

    /**
        Each line's allpass modulation rate in Hz.
    */
    std::array<float, numLines> modulationRate {};

    /**
        Each line's allpass modulation phase at prepare(), in cycles.
    */
    std::array<float, numLines> modulationPhase {};

    /**
        Each line's delay modulation rate in Hz (the chorus above the Modulation control's default, revision 1.6).
    */
    std::array<float, numLines> chorusRate {};

    /**
        Each line's delay modulation phase at prepare(), in cycles.
    */
    std::array<float, numLines> chorusPhase {};
};

/**
    A SplitMix64 pseudo-random generator [TXT]: tiny, fast, and the same sequence on every platform.
*/
class Random final
{
public:
    /**
        Creates the generator.

        @param seed  The seed.
    */
    explicit Random (uint64_t seed) noexcept
        : m_state (seed)
    {
    }

    /**
        Returns the next 64 random bits.
    */
    [[nodiscard]] uint64_t next() noexcept;

    /**
        Returns a uniform value in [0, 1).
    */
    [[nodiscard]] float uniform() noexcept;

private:
    uint64_t m_state;
};

/**
    Draws the pattern for a seed.

    @param seed  The Seed control's value, 1 to 999,999.
    @return      The pattern; the same for the same seed.
    @note        Any thread; bounded work, no allocation.
*/
[[nodiscard]] Pattern makePattern (int seed) noexcept;

} // namespace dawg::reverb
