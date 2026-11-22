#pragma once

#include "dawg/Model.h"
#include "dawg/Swing.h"

#include <cmath>
#include <cstdint>

namespace dawg
{

/**
    The notes a step sequencer plays (docs/design/step-sequencer.md). The engine's node and Render steps to MIDI both
    go through forEachStepHit(), so a rendered clip holds exactly what played.
*/
namespace stepseq
{

/**
    Where a sequencer clip's steps count from, on the timeline.
*/
struct Span
{
    /**
        The clip's start and end: the sequencer plays only between them (SS2).
    */
    Ticks start = 0;
    /// The end of the clip.
    Ticks end = 0;

    /**
        The clip's content origin (its start minus its offset): step `startStep` falls here (SS3).
    */
    Ticks origin = 0;

    /// The pattern the clip plays.
    int pattern = 0;
    /// The step the clip starts on (0 is step 1).
    int startStep = 0;

    /**
        The clip, for repeatable chance (SS7).
    */
    ObjectId clipId = 0;
};

/**
    One note the sequencer plays.
*/
struct Hit
{
    /// Where the note starts, in ticks.
    double start = 0.0;
    /// Where it ends, in ticks.
    double end = 0.0;
    /// Its MIDI pitch.
    int pitch = 60;
    /// Its velocity, 0..1.
    float velocity = 0.8f;
    /// The row that plays it.
    int row = 0;
};

/**
    Whether a hit with a chance below 1 plays: a hash of the clip, the step count, the row and the hit (SS7), so the same
    song plays the same notes every time, and different ones on every pass through the pattern.
*/
[[nodiscard]] constexpr bool plays (float chance, ObjectId clipId, long long step, int row, int hit) noexcept
{
    if (chance >= 1.0f)
        return true;

    if (chance <= 0.0f)
        return false;

    auto x = static_cast<std::uint64_t> (clipId) * UINT64_C (0x9E3779B97F4A7C15);
    x ^= static_cast<std::uint64_t> (step) + UINT64_C (0x632BE59BD9B4E019) + (x << 6) + (x >> 2);
    x ^= static_cast<std::uint64_t> (row) * UINT64_C (0xC2B2AE3D27D4EB4F) + (x << 6) + (x >> 2);
    x ^= static_cast<std::uint64_t> (hit) * UINT64_C (0x165667B19E3779F9) + (x << 6) + (x >> 2);
    x ^= x >> 33;
    x *= UINT64_C (0xFF51AFD7ED558CCD);
    x ^= x >> 33;
    const auto unit = static_cast<double> (x >> 11) * (1.0 / 9007199254740992.0);
    return unit < static_cast<double> (chance);
}

/**
    Calls fn (const Hit&) for every note a sequencer clip starts in [from, to), in time order per step, rows in order.
    Notes are cut at the clip's end.

    @param sequence  The sequencer's rows and patterns.
    @param span      The clip.
    @param from      The first tick to look at.
    @param to        The tick after the last.
    @param fn        Called for each hit. Never allocates.
*/
template <typename Fn>
void forEachStepHit (const StepSequence& sequence, const Span& span, double from, double to, Fn&& fn)
{
    if (span.pattern < 0 || span.pattern >= static_cast<int> (sequence.patterns.size()))
        return;

    const auto& pattern = sequence.patterns[static_cast<size_t> (span.pattern)];
    const auto stepTicks = static_cast<double> (
        stepRates[static_cast<size_t> (std::clamp (pattern.rate, 0, static_cast<int> (stepRates.size()) - 1))].ticks);
    const auto origin = static_cast<double> (span.origin);
    const auto lineAt = [&] (long long k) { return origin + swing::line (k, stepTicks, pattern.swing); };

    from = std::max (from, static_cast<double> (span.start));
    to = std::min (to, static_cast<double> (span.end));

    if (to <= from || stepTicks <= 0.0)
        return;

    // The first step whose line could start a hit at or after `from`: the steps before it may still ratchet into it.
    auto k = std::max (0ll, static_cast<long long> (std::floor ((from - origin) / stepTicks)) - 1);

    for (;; ++k)
    {
        const auto line = lineAt (k);

        if (line >= to)
            break;

        const auto next = lineAt (k + 1);
        const auto rows = std::min (sequence.rows.size(), pattern.rows.size());

        for (size_t r = 0; r < rows; ++r)
        {
            const auto& row = pattern.rows[r];

            if (row.length <= 0 || row.cells.empty())
                continue;

            const auto index = static_cast<size_t> ((k + span.startStep) % row.length);

            if (index >= row.cells.size())
                continue;

            const auto& cell = row.cells[index];

            if (! cell.on)
                continue;

            const auto hits = std::clamp (cell.ratchet, 1, 4);
            const auto each = (next - line) / hits;

            for (int h = 0; h < hits; ++h)
            {
                const auto start = line + each * h;

                if (start < from || start >= to)
                    continue;

                if (! plays (cell.chance, span.clipId, k, static_cast<int> (r), h))
                    continue;

                const auto length = hits > 1 ? each * std::min (1.0f, cell.length) : stepTicks * cell.length;
                Hit hit;
                hit.start = start;
                hit.end = std::min (start + std::max (1.0, length), static_cast<double> (span.end));
                hit.pitch = sequence.rows[r].pitch;
                hit.velocity = cell.velocity;
                hit.row = static_cast<int> (r);
                fn (hit);
            }
        }
    }
}

} // namespace stepseq

} // namespace dawg
