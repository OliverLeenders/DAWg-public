#include "dawg/Vibrato.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
/** One cycle of the waveform for a phase in [0, 1), starting at 0 and rising first. */
[[nodiscard]] double waveform (VibratoShape shape, double phase) noexcept
{
    switch (shape)
    {
        case VibratoShape::triangle:
            if (phase < 0.25)
                return 4.0 * phase;
            if (phase < 0.75)
                return 2.0 - 4.0 * phase;
            return 4.0 * phase - 4.0;

        case VibratoShape::square:
            return phase < 0.5 ? 1.0 : -1.0;

        case VibratoShape::sawUp:
            return phase < 0.5 ? 2.0 * phase : 2.0 * phase - 2.0;

        case VibratoShape::sawDown:
            return phase < 0.5 ? -2.0 * phase : 2.0 - 2.0 * phase;

        case VibratoShape::sine:
            break;
    }

    return std::sin (juce::MathConstants<double>::twoPi * phase);
}
} // namespace

double vibratoOffset (const Vibrato& vibrato, double ticksFromStart, double secondsPerTick, double depthScale) noexcept
{
    const auto elapsed = ticksFromStart - static_cast<double> (vibrato.delay);

    if (! vibrato.active() || depthScale <= 0.0 || elapsed < 0.0)
        return 0.0;

    const auto fade = vibrato.fadeIn > 0 ? std::min (1.0, elapsed / static_cast<double> (vibrato.fadeIn)) : 1.0;
    const auto cycles = vibrato.syncBeats > 0.0 ? elapsed / (vibrato.syncBeats * static_cast<double> (ticksPerBeat))
                                                : elapsed * secondsPerTick * vibrato.rateHz;

    return vibrato.depth * fade * depthScale * waveform (vibrato.shape, cycles - std::floor (cycles));
}

} // namespace dawg
