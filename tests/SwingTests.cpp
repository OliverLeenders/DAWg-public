#include "dawg/Swing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <tuple>

using namespace dawg;

// The same table as ui/src/time/swing.test.ts, so the arpeggiator and the piano roll's quantise agree.
TEST_CASE ("swing moves every second grid line", "[swing]")
{
    constexpr double sixteenth = 3840.0;
    constexpr std::array<std::tuple<long long, double, double>, 9> table { {
        { 0, 0.5, 0.0 },
        { 1, 0.5, 3840.0 },
        { 1, 0.6, 4608.0 },
        { 1, 2.0 / 3.0, 5120.0 },
        { 1, 0.75, 5760.0 },
        { 2, 0.75, 7680.0 },
        { 3, 0.75, 13440.0 },
        { 3, 0.9, 13440.0 }, // clamped to 75 %
        { 1, 0.3, 3840.0 },  // clamped to 50 %
    } };

    for (const auto& [index, amount, expected] : table)
        CHECK (swing::line (index, sixteenth, amount) == Catch::Approx (expected));
}
