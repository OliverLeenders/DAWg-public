#pragma once

#include "dawg/Model.h"

#include <optional>
#include <vector>

namespace dawg
{
/**
    Where a recorded audio file goes on its track (docs/design/takes-lanes.md, T4; docs/design/audio-features.md,
    AU11 and AU13): one clip of the file.
*/
struct TakeSpan
{
    /**
        The clip's start on the timeline.
    */
    Ticks start = 0;

    /**
        The content position at its start: ticks into the file, at the tempo where recording started.
    */
    Ticks offset = 0;

    /**
        Its length, or 0 for the rest of the file.
    */
    Ticks length = 0;

    /**
        Compares all fields.
    */
    bool operator== (const TakeSpan&) const = default;
};

/**
    Turns a recording into clips. The audio heard at timeline tick t is at content tick t - recordStart + latency in
    the file, which shifts the take earlier by the round-trip latency (phase7-audio-tracks.md, T9).

    * Without a loop or a punch range: one clip of the whole file.
    * Loop recording (the loop wrapped while recording), from inside the loop: one clip per full pass over the loop
      range (T4). Started outside the loop, the whole file.
    * Punch, without a loop: one clip over the punch range, cut to what was recorded (AU11).
    * Punch inside a loop: one clip per full pass over the punch range, clipped to the loop (AU13).

    @param recordStart    The timeline tick of the file's first frame.
    @param recordedTicks  The file's length in content ticks.
    @param latency        The round-trip latency in content ticks.
    @param loop           The loop range, if the loop wrapped while recording.
    @param punch          The punch range, if punching.
    @return               The clips, in recording order (the last pass last); empty if nothing usable was recorded.
*/
[[nodiscard]] std::vector<TakeSpan> recordedTakes (Ticks recordStart, Ticks recordedTicks, Ticks latency,
                                                   std::optional<LoopRange> loop, std::optional<PunchRange> punch);

/**
    The punch range clipped to the loop range (AU13), or nullopt if they don't overlap.

    @param punch  The punch range.
    @param loop   The loop range.
    @return       The part of the punch range inside the loop.
*/
[[nodiscard]] std::optional<PunchRange> punchInLoop (const PunchRange& punch, const LoopRange& loop);
} // namespace dawg
