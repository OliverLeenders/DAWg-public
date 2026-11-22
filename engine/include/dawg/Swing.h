#pragma once

#include <algorithm>
#include <cmath>

namespace dawg
{

/**
    Swing for the arpeggiator and note repeat (docs/design/midi-effects.md, ME9). The piano roll's quantise does the
    same in ui/src/time/swing.ts; both are tested against one table.
*/
namespace swing
{

/**
    Straight: the second cell of each pair starts halfway through the pair.
*/
inline constexpr double straight = 0.5;

/**
    The most swing allowed.
*/
inline constexpr double maximum = 0.75;

/**
    Clamps a swing amount to straight..maximum.

    @param amount  The swing, 0.5..0.75.
    @return        The clamped swing.
*/
[[nodiscard]] constexpr double clamp (double amount) noexcept
{
    return std::clamp (amount, straight, maximum);
}

/**
    Returns where grid line `index` lies, counted from a line that starts a pair (a bar start): in each pair of cells
    the second line moves from 50 % of the pair to `amount` of it.

    @param index   The grid line, 0 for the start.
    @param step    The grid cell length, in ticks.
    @param amount  The swing, 0.5..0.75.
    @return        The line's offset from the start, in ticks.
*/
[[nodiscard]] constexpr double line (long long index, double step, double amount) noexcept
{
    const auto pair = index / 2;
    return index % 2 == 0 ? static_cast<double> (index) * step
                          : (2.0 * static_cast<double> (pair) + 2.0 * clamp (amount)) * step;
}

} // namespace swing

} // namespace dawg
