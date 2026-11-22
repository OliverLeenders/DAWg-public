#pragma once

#include "dawg/Time.h"

#include <vector>

namespace dawg
{

/**
    Whether a pool file follows the tempo (docs/design/time-stretching.md, TS2).
*/
enum class TimingMode
{
    oneShot, ///< Plays at its own speed and keeps its length in seconds (the default).
    musical  ///< Its content ticks are musical ticks: its clips stay on their bars when the tempo changes.
};

/**
    How a musical file changes speed.
*/
enum class MusicalStyle
{
    stretch, ///< Keeps the pitch (rendered offline, TS7).
    repitch  ///< Speed and pitch change together, like tape.
};

/**
    A timing marker: pins a file frame to a point of the file's content time (TS2). A musical file's content time is
    in ticks, so it follows the tempo; a one-shot file's is in seconds, so its markers stay put in real time.
*/
struct TimingMarker
{
    /**
        The content position from the start of the file's content time: ticks for musical files, seconds for
        one-shot files.
    */
    double tick = 0.0;

    /**
        The file frame that sounds there.
    */
    double frame = 0.0;

    /**
        Returns whether two markers are the same.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    bool operator== (const TimingMarker&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    A pool file's timing settings (TS2).
*/
struct FileTiming
{
    /**
        One-shot or musical.
    */
    TimingMode mode = TimingMode::oneShot;

    /**
        Stretch or repitch, for musical files and for one-shot files with markers.
    */
    MusicalStyle style = MusicalStyle::stretch;

    /**
        The file's own tempo, which sets the content-to-file rate past the last marker.
    */
    double sourceBpm = 120.0;

    /**
        Sorted by position; positions and frames both strictly increasing and positive. Switching the mode converts
        them, so the audio stays where it sounds.
    */
    std::vector<TimingMarker> markers;

    /**
        Returns whether the file follows the tempo.
    */
    [[nodiscard]] bool musical() const noexcept { return mode == TimingMode::musical; }

    /**
        Returns whether the file plays through its timing map: musical, or one-shot with markers.
    */
    [[nodiscard]] bool retimed() const noexcept { return musical() || ! markers.empty(); }

    /**
        Returns whether two settings are the same.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    bool operator== (const FileTiming&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    Sorts markers by tick and drops those that would break the strict increase of ticks or frames (keeping the
    earlier one), and those at or before tick 0 or frame 0 (the implicit first point).

    @param markers  The markers to repair.
*/
void normaliseTimingMarkers (std::vector<TimingMarker>& markers);

/**
    A file's content-to-file mapping (docs/design/time-stretching.md, TS3): piecewise linear through its markers,
    from the implicit point (0, frame 0), and on at the file's natural rate past the last marker (and before 0).

    A musical file's content is in ticks and its natural rate is its source tempo's; a one-shot file's content is in
    seconds and its natural rate is its sample rate, so without markers frame f sounds at f / sampleRate. The methods
    say "tick" for either unit.

    Immutable; lookups are a binary search. The TypeScript twin is ui/src/time/timingMap.ts.
*/
class TimingMap final
{
public:
    /**
        Creates the map of a file at 120 BPM and 48 kHz, without markers.
    */
    TimingMap();

    /**
        Creates the map of a file.

        @param timing    The file's timing (its markers must be normalised, see normaliseTimingMarkers()).
        @param fileRate  The file's sample rate in Hz.
    */
    TimingMap (const FileTiming& timing, double fileRate);

    /**
        Returns the file frame at a content tick.
    */
    [[nodiscard]] double frameAt (double tick) const noexcept;

    /**
        Returns the content tick at a file frame, the inverse of frameAt().
    */
    [[nodiscard]] double tickAt (double frame) const noexcept;

    /**
        Returns the file frames per content tick at a tick (the slope of the segment it lies in).
    */
    [[nodiscard]] double rateAt (double tick) const noexcept;

    /**
        Returns the frames per content unit past the last marker: at the source tempo (musical), or the sample rate
        (one-shot).
    */
    [[nodiscard]] double sourceRate() const noexcept { return m_sourceRate; }

    /**
        Returns the points the map runs through: (0, 0) and the markers.
    */
    [[nodiscard]] const std::vector<TimingMarker>& points() const noexcept { return m_points; }

    /**
        Returns the index of the segment a tick lies in: 0 before the second point, points().size() - 1 past the
        last one.
    */
    [[nodiscard]] size_t segmentAt (double tick) const noexcept;

    /**
        Returns the file frame at a content tick, starting the search at a hint (the segment of the last lookup,
        updated), for monotonic lookups on the audio thread.

        @param tick  The content tick.
        @param hint  The segment to start from; updated to the tick's segment.
    */
    [[nodiscard]] double frameAt (double tick, size_t& hint) const noexcept;

private:
    [[nodiscard]] double slope (size_t segment) const noexcept;

    std::vector<TimingMarker> m_points;
    double m_sourceRate = 0.0;
};

/**
    The frames per content tick of a file at a tempo.

    @param fileRate  The file's sample rate.
    @param bpm       The tempo.
*/
[[nodiscard]] constexpr double framesPerTick (double fileRate, double bpm) noexcept
{
    return fileRate * 60.0 / (bpm * static_cast<double> (ticksPerBeat));
}

/**
    A file's markers converted to the other timing mode, so that a clip whose content starts at anchorTick plays every
    frame at the same time as before (TS4).

    A musical file's content tick c sounds at timeline tick anchorTick + c; a one-shot file's content second s at
    timeline second map.secondsAt (anchorTick) + s. The result pins the frames at the old markers, at each tempo point
    the file passes through and along tempo ramps (finely enough that no point is off by more than about 10 µs), and
    at the file's end, then drops the markers that the natural rate past them would give anyway.

    @param map          The tempo map.
    @param anchorTick   The timeline tick where the file's frame 0 sounds.
    @param from         The file's timing now (its mode says which way to convert).
    @param fileFrames   The file's length in frames.
    @param fileRate     The file's sample rate.
    @param sourceBpm    The source tempo after the switch, for a switch to musical; ignored otherwise.
    @return             The markers for the other mode, normalised.
*/
[[nodiscard]] std::vector<TimingMarker> convertTimingMarkers (const TempoMap& map, double anchorTick,
                                                              const FileTiming& from, juce::int64 fileFrames,
                                                              double fileRate, double sourceBpm);

/**
    The markers that make a musical file play its frames at the times a one-shot file without markers would (TS4):
    convertTimingMarkers() from plain one-shot timing.

    A one-shot file plays frame f at timeline second anchorSeconds + f / fileRate. As a musical file, content tick c
    plays at timeline tick anchorTick + c. The markers pin the frames at each tempo point the file passes through,
    sample tempo ramps finely enough that no point is off by more than about 10 µs, and pin the file's end where the
    tempo there differs from sourceBpm.

    @param map          The tempo map.
    @param anchorTick   The timeline tick where frame 0 sounds (map.tickAt (anchorSeconds)).
    @param fileFrames   The file's length in frames.
    @param fileRate     The file's sample rate.
    @param sourceBpm    The source tempo (the rate past the last marker).
    @return             The markers, normalised.
*/
[[nodiscard]] std::vector<TimingMarker> markersFromTempo (const TempoMap& map, double anchorTick,
                                                          juce::int64 fileFrames, double fileRate, double sourceBpm);

/**
    A source tempo suggested by a file's name (docs/design/time-stretching.md, TS11): a number from 60 to 200 followed
    by "bpm" ("Drums 128bpm", "128_BPM").

    @param name  The file name.
    @return      The tempo, or 0 for none.
*/
[[nodiscard]] double tempoFromName (const juce::String& name);

/**
    A source tempo guessed from a short file's length (TS11): the tempo from 80 to 160 BPM at which the file lasts 1,
    2, 4 or 8 whole bars; of several, the one nearest the project's tempo.

    @param seconds      The file's length.
    @param barQuarters  Quarter notes per bar in the project's signature.
    @param projectBpm   The project's tempo.
    @return             The tempo, or 0 for none (or for files of 30 seconds or more).
*/
[[nodiscard]] double tempoFromLength (double seconds, double barQuarters, double projectBpm);

/**
    The source tempo to suggest for a file (TS11), trying in order its metadata, its name and its length.

    @param metadataBpm  A tempo from the file's metadata (a WAV acid chunk, Apple Loops), or 0.
    @param name         The file name.
    @param seconds      Its length.
    @param barQuarters  Quarter notes per bar in the project's signature.
    @param projectBpm   The project's tempo.
    @return             The tempo, or 0 for none. Never applied by itself.
*/
[[nodiscard]] double suggestTempo (double metadataBpm, const juce::String& name, double seconds, double barQuarters,
                                   double projectBpm);

} // namespace dawg
