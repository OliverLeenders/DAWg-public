#include "dawg/Curve.h"
#include "dawg/EnvelopeHandle.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace dawg;
using Catch::Matchers::WithinAbs;

TEST_CASE ("an empty curve returns the lane default", "[curve]")
{
    CHECK_THAT (evaluateCurve ({}, 100.0, 0.5), WithinAbs (0.5, 1.0e-12));
}

TEST_CASE ("curves hold their first and last values outside the points", "[curve]")
{
    const Curve curve { { 100, 1.0 }, { 200, 3.0 } };

    CHECK_THAT (evaluateCurve (curve, 0.0, 0.0), WithinAbs (1.0, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 1000.0, 0.0), WithinAbs (3.0, 1.0e-12));
}

TEST_CASE ("linear segments interpolate", "[curve]")
{
    const Curve curve { { 0, 0.0 }, { 100, 10.0 } };

    CHECK_THAT (evaluateCurve (curve, 25.0, 0.0), WithinAbs (2.5, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 50.0, 0.0), WithinAbs (5.0, 1.0e-12));
}

TEST_CASE ("hold segments step at the next point", "[curve]")
{
    const Curve curve { { 0, 1.0, CurveShape::hold }, { 100, 2.0 } };

    CHECK_THAT (evaluateCurve (curve, 99.0, 0.0), WithinAbs (1.0, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 100.0, 0.0), WithinAbs (2.0, 1.0e-12));
}

TEST_CASE ("power segments ease by tension", "[curve]")
{
    const Curve slowStart { { 0, 0.0, CurveShape::power, 1.0 }, { 100, 1.0 } };
    const Curve fastStart { { 0, 0.0, CurveShape::power, -1.0 }, { 100, 1.0 } };
    const Curve neutral { { 0, 0.0, CurveShape::power, 0.0 }, { 100, 1.0 } };

    CHECK_THAT (evaluateCurve (slowStart, 50.0, 0.0), WithinAbs (std::pow (0.5, 8.0), 1.0e-12));
    CHECK_THAT (evaluateCurve (fastStart, 50.0, 0.0), WithinAbs (std::pow (0.5, 1.0 / 8.0), 1.0e-12));
    CHECK_THAT (evaluateCurve (neutral, 50.0, 0.0), WithinAbs (0.5, 1.0e-12));
}

TEST_CASE ("power segments use the wider tension range for free handles", "[curve]")
{
    const Curve deep { { 0, 0.0, CurveShape::power, 2.0 }, { 100, 1.0 } };

    CHECK_THAT (evaluateCurve (deep, 50.0, 0.0), WithinAbs (std::pow (0.5, 64.0), 1.0e-12));

    const Curve clamped { { 0, 0.0, CurveShape::power, 10.0 }, { 100, 1.0 } };

    CHECK_THAT (evaluateCurve (clamped, 50.0, 0.0),
                WithinAbs (std::pow (0.5, std::pow (2.0, maxTension * 3.0)), 1.0e-12));
}

// Expected values from the handle rule (freeSegment() in Curve.h) with the Bezier sampled finely, not bisected.
TEST_CASE ("free segments point their handles at the tension point", "[curve]")
{
    const Curve curve { { 0, 0.0, CurveShape::free, 0.0, 0.5, 0.99 }, { 100, 1.0 } };

    CHECK_THAT (evaluateCurve (curve, 0.0, 0.0), WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 50.0, 0.0), WithinAbs (0.780442, 1.0e-4));
    CHECK_THAT (evaluateCurve (curve, 100.0, 0.0), WithinAbs (1.0, 1.0e-12));

    // In the middle of the rectangle the handles are half as long as the way to it: a straight line.
    for (const auto x : { 0.1, 0.37, 0.5, 0.9 })
        CHECK_THAT (freeSegment (x, 0.5, 0.5), WithinAbs (x, 1.0e-9));
}

TEST_CASE ("a tension point can move inside the segment rectangle", "[curve]")
{
    const Curve curve { { 0, 0.0, CurveShape::free, 0.0, 0.01, 0.99 }, { 100, 1.0 } };

    CHECK_THAT (evaluateCurve (curve, 1.0, 0.0), WithinAbs (0.423637, 1.0e-4));
    CHECK_THAT (evaluateCurve (curve, 10.0, 0.0), WithinAbs (0.819489, 1.0e-4));
}

TEST_CASE ("free segments bend falling curves too", "[curve]")
{
    const Curve curve { { 0, 1.0, CurveShape::free, 0.0, 0.5, 0.75 }, { 100, 0.0 } };

    CHECK_THAT (evaluateCurve (curve, 50.0, 0.0), WithinAbs (0.381668, 1.0e-4));
}

TEST_CASE ("multi-segment curves pick the right segment", "[curve]")
{
    const Curve curve { { 0, 0.0 }, { 100, 10.0 }, { 200, 0.0 } };

    CHECK_THAT (evaluateCurve (curve, 150.0, 0.0), WithinAbs (5.0, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 100.0, 0.0), WithinAbs (10.0, 1.0e-12));
}

TEST_CASE ("lanes are found by id", "[curve]")
{
    Lane lane {};
    REQUIRE (laneFromId ("timbre", lane));
    CHECK (lane == Lane::timbre);
    CHECK_FALSE (laneFromId ("brightness", lane));
}

namespace
{
// The free segment by plain bisection, carried far past double precision's need: the reference for the Newton solve.
double bisectedFreeSegment (double x, double hx, double hy)
{
    const auto start = std::max (hx, hy);
    const auto end = std::max (1.0 - hx, 1.0 - hy);
    const auto bezier = [] (double u, double c1, double c2)
    {
        const auto v = 1.0 - u;
        return 3.0 * v * v * u * c1 + 3.0 * v * u * u * c2 + u * u * u;
    };
    auto low = 0.0;
    auto high = 1.0;

    for (int i = 0; i < 80; ++i)
    {
        const auto u = (low + high) * 0.5;
        (bezier (u, hx * start, 1.0 + (hx - 1.0) * end) < x ? low : high) = u;
    }

    return bezier ((low + high) * 0.5, hy * start, 1.0 + (hy - 1.0) * end);
}

constexpr std::array<double, 9> handlePositions { 0.0, 0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99, 1.0 };
} // namespace

TEST_CASE ("the free segment's solve matches bisection at every tension point, from any start", "[curve]")
{
    auto worst = 0.0;

    for (const auto hx : handlePositions)
        for (const auto hy : handlePositions)
            for (int i = 1; i < 1000; ++i)
            {
                const auto x = i / 1000.0;
                const auto expected = bisectedFreeSegment (x, hx, hy);
                worst = std::max (worst, std::abs (freeSegment (x, hx, hy) - expected));

                for (const auto start : { 0.0, 0.5, 1.0 })
                {
                    auto hint = start;
                    worst = std::max (worst, std::abs (freeSegment (x, hx, hy, &hint) - expected));
                }
            }

    // Near a corner the curve is steep, so y magnifies the solve's last bits.
    CHECK (worst < 1.0e-9);

    // The ends are exact. (Bisection isn't there: where x is flat in u, as at the end of a segment whose tension point
    // sits in a corner, it stops up to 1e-5 short of 1.)
    for (const auto hx : handlePositions)
        for (const auto hy : handlePositions)
        {
            CHECK (freeSegment (0.0, hx, hy) == 0.0);
            CHECK (freeSegment (1.0, hx, hy) == 1.0);
        }
}

TEST_CASE ("an envelope stepping through a segment with a hint gets shape()'s float values", "[curve]")
{
    // A 480-sample segment, as the ADSR node and the Sampler step through one: the warm-started solve lands on the
    // same float as a cold one, at most one rounding step apart.
    for (const auto time : { 0.0f, 0.2f, 0.8f, 1.0f })
        for (const auto level : { 0.0f, 0.3f, 0.9f, 1.0f })
        {
            const EnvelopeHandle handle { time, level };
            auto hint = 0.0;
            auto progress = 0.0f;

            for (int s = 0; s < 480; ++s)
            {
                progress += 1.0f / 480.0f;
                const auto cold = handle.shape (progress);
                const auto warm = handle.shape (progress, hint);
                CHECK (std::abs (warm - cold) <= std::nextafter (std::max (cold, 1.0e-30f), 2.0f) - cold);
            }
        }
}

TEST_CASE ("smooth segments are a monotone cubic through their nodes", "[curve]")
{
    const auto smooth = [] (Ticks time, double value) { return CurvePoint { time, value, CurveShape::smooth }; };
    const Curve curve { smooth (0, 0.0), smooth (100, 1.0), smooth (200, 3.0), smooth (300, 2.0), smooth (400, 2.0) };

    // The same values as the UI's twin (ui/src/pianoroll/geometry.test.ts).
    CHECK_THAT (evaluateCurve (curve, 50.0, 0.0), WithinAbs (0.3958333333333333, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 150.0, 0.0), WithinAbs (2.166666666666667, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 250.0, 0.0), WithinAbs (2.5, 1.0e-12));
    CHECK_THAT (evaluateCurve (curve, 350.0, 0.0), WithinAbs (2.0, 1.0e-12));

    // Through every node, never past the peak at 200 or below the trough-free flat end, and without corners: the
    // slope just before and after each inner node agrees.
    for (const auto& p : curve)
        CHECK_THAT (evaluateCurve (curve, static_cast<double> (p.time), 0.0), WithinAbs (p.value, 1.0e-12));

    for (auto t = 0.0; t <= 400.0; t += 1.0)
    {
        const auto v = evaluateCurve (curve, t, 0.0);
        CHECK (v <= 3.0 + 1.0e-12);
        CHECK (v >= -1.0e-12);
    }

    for (const auto node : { 100.0, 200.0, 300.0 })
    {
        const auto before = (evaluateCurve (curve, node, 0.0) - evaluateCurve (curve, node - 0.01, 0.0)) / 0.01;
        const auto after = (evaluateCurve (curve, node + 0.01, 0.0) - evaluateCurve (curve, node, 0.0)) / 0.01;
        CHECK_THAT (before, WithinAbs (after, 1.0e-3));
    }

    CHECK (smoothSlope (curve, 200) == 0.0); // the peak is flat
}

TEST_CASE ("a smooth run ends where another shape begins", "[curve]")
{
    // A lone smooth segment is a straight line; a smooth run next to a linear segment ignores the linear one's node.
    const Curve lone { { 0, 0.0, CurveShape::smooth }, { 100, 2.0, CurveShape::linear }, { 200, 0.0 } };
    CHECK_THAT (evaluateCurve (lone, 25.0, 0.0), WithinAbs (0.5, 1.0e-12));

    const Curve mixed {
        { 0, 0.0, CurveShape::linear }, { 100, 1.0, CurveShape::smooth }, { 200, 3.0, CurveShape::smooth }, { 300, 2.0 }
    };
    const Curve alone { { 100, 1.0, CurveShape::smooth }, { 200, 3.0, CurveShape::smooth }, { 300, 2.0 } };

    for (auto t = 100.0; t <= 300.0; t += 10.0)
        CHECK_THAT (evaluateCurve (mixed, t, 0.0), WithinAbs (evaluateCurve (alone, t, 0.0), 1.0e-12));
}

TEST_CASE ("rounded curves round a knee that smooth ones keep", "[curve]")
{
    // A steep rise into a long flat stretch: the monotone curve stops dead at the node, the rounded one carries on a
    // little past it and comes back.
    const auto knee = [] (CurveShape shape)
    { return Curve { { 0, 0.0, shape }, { 100, 1.0, shape }, { 1000, 1.0, shape } }; };
    const auto smooth = knee (CurveShape::smooth);
    const auto rounded = knee (CurveShape::rounded);

    CHECK (smoothSlope (smooth, 1) == 0.0);
    CHECK_THAT (smoothSlope (rounded, 1), Catch::Matchers::WithinAbs (0.001, 1e-12));
    CHECK (evaluateCurve (smooth, 250.0, 0.0) <= 1.0);
    CHECK (evaluateCurve (rounded, 250.0, 0.0) > 1.0);
    CHECK (evaluateCurve (rounded, 250.0, 0.0) < 1.15);
    CHECK_THAT (evaluateCurve (rounded, 100.0, 0.0), Catch::Matchers::WithinAbs (1.0, 1e-12)); // through its nodes

    // Its ends are level, meeting the values held beyond them: a rise that ends at the last node has no corner there.
    const Curve rise { { 0, 0.0, CurveShape::rounded },
                       { 100, 0.0, CurveShape::rounded },
                       { 200, 1.0, CurveShape::rounded } };
    CHECK (smoothSlope (rise, 0) == 0.0);
    CHECK (smoothSlope (rise, 2) == 0.0);
    CHECK_THAT (evaluateCurve (rise, 199.0, 0.0), Catch::Matchers::WithinAbs (1.0, 0.001));
}
