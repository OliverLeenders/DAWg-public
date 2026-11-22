#pragma once

#include "dawg/Curve.h"

namespace dawg
{

/**
    An envelope segment's tension point, as fractions of the rectangle between the segment's ends: a free curve segment
    (a cubic Bezier whose handles point at it, freeSegment()). 0.5 and 0.5 is a straight line. Shared by the ADSR node
    and the Sampler's zones.
*/
struct EnvelopeHandle
{
    /// The handle's position across the segment's time, 0..1.
    float time = 0.5f;
    /// The handle's position across the segment's level, 0..1.
    float level = 0.5f;

    /**
        How far the segment has travelled at a progress through it, both 0..1. A handle on the diagonal is a straight
        line, which skips the Bezier.
    */
    [[nodiscard]] float shape (float progress) const noexcept
    {
        if (juce::exactlyEqual (time, level))
            return progress;

        return static_cast<float> (freeSegment (progress, time, level));
    }

    /**
        shape() for an envelope that moves through the segment sample by sample: `hint` carries the Bezier's solution
        from one sample to the next (freeSegment()), which makes each a step or two. Any start value works; the
        result is the same as shape()'s to within rounding.
    */
    [[nodiscard]] float shape (float progress, double& hint) const noexcept
    {
        if (juce::exactlyEqual (time, level))
            return progress;

        return static_cast<float> (freeSegment (progress, time, level, &hint));
    }

    /**
        The progress at which the segment has travelled `travel` (the inverse of shape()).
    */
    [[nodiscard]] float progressFor (float travel) const noexcept
    {
        auto low = 0.0f;
        auto high = 1.0f;

        for (int i = 0; i < 24; ++i)
        {
            const auto middle = (low + high) * 0.5f;

            if (shape (middle) < travel)
                low = middle;
            else
                high = middle;
        }

        return (low + high) * 0.5f;
    }
};

} // namespace dawg
