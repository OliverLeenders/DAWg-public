#include "dawg/Recording.h"

#include <algorithm>

namespace dawg
{
std::optional<PunchRange> punchInLoop (const PunchRange& punch, const LoopRange& loop)
{
    PunchRange clipped = punch;
    clipped.start = std::max (punch.start, loop.start);
    clipped.end = std::min (punch.end, loop.end);

    if (clipped.end <= clipped.start)
        return std::nullopt;

    return clipped;
}

std::vector<TakeSpan> recordedTakes (Ticks recordStart, Ticks recordedTicks, Ticks latency,
                                     std::optional<LoopRange> loop, std::optional<PunchRange> punch)
{
    std::vector<TakeSpan> takes;

    if (recordedTicks <= 0)
        return takes;

    // The content tick heard at a timeline tick, on the first pass.
    const auto contentAt = [&] (Ticks tick) { return tick - recordStart + latency; };

    if (punch.has_value() && loop.has_value())
    {
        const auto range = punchInLoop (*punch, *loop);

        if (! range.has_value())
            return takes;

        const auto loopLength = loop->end - loop->start;
        const auto length = range->end - range->start;
        // Ticks of recording until the punch-in first comes round: straight there, or through the loop's end.
        const auto first = recordStart <= range->start ? range->start - recordStart
                                                       : (loop->end - recordStart) + (range->start - loop->start);

        for (auto elapsed = first; elapsed + latency + length <= recordedTicks; elapsed += loopLength)
            takes.push_back ({ range->start, elapsed + latency, length });

        return takes;
    }

    if (punch.has_value())
    {
        // The earliest tick the file holds, and the end of what it holds.
        const auto from = std::max (punch->start, recordStart - latency);
        const auto to = std::min (punch->end, recordStart - latency + recordedTicks);

        if (from < to && from >= 0)
            takes.push_back ({ from, contentAt (from), to - from });

        return takes;
    }

    if (loop.has_value() && recordStart >= loop->start && recordStart < loop->end)
    {
        const auto loopLength = loop->end - loop->start;
        const auto firstPass = recordStart == loop->start ? Ticks { 0 } : loop->end - recordStart;
        const auto passes = (recordedTicks - firstPass - latency) / loopLength;

        for (Ticks pass = 0; pass < passes; ++pass)
            takes.push_back ({ loop->start, firstPass + latency + pass * loopLength, loopLength });

        if (! takes.empty())
            return takes;
    }

    // The whole file, shifted earlier by the latency (but not before the song's start).
    const auto start = recordStart - latency;
    takes.push_back ({ std::max (Ticks { 0 }, start), std::max (Ticks { 0 }, -start), 0 });
    return takes;
}
} // namespace dawg
