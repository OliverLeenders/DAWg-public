#pragma once

#include "dawg/AudioToMidi.h"
#include "dawg/Model.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <utility>
#include <vector>

namespace dawg
{
/**
    The length of a pool file in a clip's content ticks: through its timing map for musical files, else at the tempo
    where the clip starts (docs/design/tempo-map.md, TM8).

    @param file       The pool file.
    @param map        The project's tempo map.
    @param clipStart  The clip's start on the timeline.
    @return           The length in content ticks.
*/
[[nodiscard]] double fileContentTicks (const AudioFile& file, const TempoMap& map, Ticks clipStart);

/**
    The timing of a reversed copy of a file (docs/design/audio-features.md, AU1): every marker mirrored, plus one
    where the original's start lands, so the copy plays the original's timing backwards.

    @param timing    The original's timing (normalised markers).
    @param fileRate  The file's sample rate.
    @param frames    The file's length in frames.
    @return          The mirrored timing.
*/
[[nodiscard]] FileTiming reversedTiming (const FileTiming& timing, double fileRate, juce::int64 frames);

/**
    Mirrors a clip's content around its file (AU1): the same audio, played backwards in the same place. The content
    range and loop range mirror, and the fades swap.

    @param clip          The clip; its file is replaced by the caller.
    @param contentTicks  The file's length in the clip's content ticks (fileContentTicks()).
*/
void mirrorClip (NoteClip& clip, double contentTicks);

/**
    The file frames a clip plays, as sorted, disjoint ranges [first, second) inside the file (AU2).

    @param clip  An audio clip.
    @param file  Its pool file.
    @param map   The project's tempo map.
    @return      The ranges; empty if the clip plays nothing of its file.
*/
[[nodiscard]] std::vector<std::pair<juce::int64, juce::int64>>
playedFrameRanges (const NoteClip& clip, const AudioFile& file, const TempoMap& map);

/**
    The gain changes that normalise clips to a peak target (AU2).

    @param peaks     Each clip's sample peak (linear); 0 for silence.
    @param targetDb  The target peak in dBFS.
    @param together  One change for all, set by the loudest; otherwise one per clip.
    @return          The change in dB for each clip, 0 for silent ones. The caller clamps the resulting gain to the
                     clip gain's range (limits::maxGainDb).
*/
[[nodiscard]] std::vector<double> normaliseChanges (const std::vector<float>& peaks, double targetDb, bool together);

/**
    Writes a file's audio backwards, as a 32-bit float WAV (AU1).

    @param reader  The source.
    @param target  The file to write (replaced).
    @return        True on success.
*/
[[nodiscard]] bool writeReversed (juce::AudioFormatReader& reader, const juce::File& target);

/**
    Which edges of an audio clip lie over audio of a clip below it on its track (AU5), so that a fade there is a
    crossfade.
*/
struct CoveredEdges
{
    /**
        The left edge: a clip below starts before it and ends after it.
    */
    bool start = false;

    /**
        The right edge.
    */
    bool end = false;
};

/**
    Finds which edges of an audio clip lie over audio of a clip lower in the stack (AU5).

    @param clips  The track's clips.
    @param clip   One of them.
    @return       The covered edges.
*/
[[nodiscard]] CoveredEdges coveredEdges (const std::vector<ClipPtr>& clips, const NoteClip& clip);

/**
    The fade length, in an audio clip's content ticks, that lasts a time at one of its edges: content ticks run at the
    start tempo for one-shot files, and are timeline ticks for musical files.

    @param clip     The clip.
    @param atStart  The fade-in (true) or the fade-out.
    @param seconds  The time.
    @param musical  Whether the clip's file is musical.
    @param map      The tempo map.
    @return         The length in content ticks, at least 1, at most half the clip.
*/
[[nodiscard]] Ticks fadeTicksFor (const NoteClip& clip, bool atStart, double seconds, bool musical,
                                  const TempoMap& map);

/**
    What a track's audio clips play (AU6): playedClips(), with every part that a clip above cuts at a fade of its own
    reaching on under that fade with the mirrored fade, so the two crossfade. Note clips are left out.

    @param clips    The track's clips.
    @param project  The project (the audio pool and the tempo map).
    @return         The parts, sorted by start.
*/
[[nodiscard]] std::vector<ClipPtr> crossfadedParts (const std::vector<ClipPtr>& clips, const Project& project);

/**
    The project a render of some of a track's clips runs (docs/design/audio-features.md, AU16): the track keeps only
    those clips. Dry (Consolidate), its Audio Player feeds its Track Output straight: the inserts stay in the graph,
    cut off, so they keep their instances but don't run.

    @param project  The project.
    @param trackId  The track.
    @param clipIds  The clips to keep; all of them if empty.
    @param dry      Whether to leave the inserts out.
    @return         The project to compile for the render.
*/
[[nodiscard]] Project clipsRenderProject (const Project& project, ObjectId trackId,
                                          const std::vector<ObjectId>& clipIds, bool dry);

/**
    Maps between an audio clip's timeline positions and its file's frames, on its first pass (docs/design/audio-features.md,
    AU3): through the timing map for musical files, at the start tempo for one-shot files.
*/
class ClipFrames final
{
public:
    /**
        Creates the mapping.

        @param clip  An audio clip.
        @param file  Its pool file.
        @param map   The project's tempo map.
    */
    ClipFrames (const NoteClip& clip, const AudioFile& file, const TempoMap& map);

    /**
        Returns the file frame playing at a timeline tick.
    */
    [[nodiscard]] double frameAt (double tick) const;

    /**
        Returns the timeline tick where a file frame plays.
    */
    [[nodiscard]] double tickAt (double frame) const;

    /**
        Returns the frames the clip plays from its start to its end (not looped), cut to the file.
    */
    [[nodiscard]] std::pair<juce::int64, juce::int64> range() const;

private:
    NoteClip m_clip;
    AudioFile m_file;
    const TempoMap* m_map;
};

/**
    Where a grid cuts a clip into slices (AU3): the file frames at every multiple of `division` on the timeline strictly
    inside the clip.

    @param frames    The clip's mapping.
    @param clip      The clip.
    @param division  The grid in ticks (a 1/16 is ticksPerBeat / 4).
    @return          The cut points, sorted.
*/
[[nodiscard]] std::vector<juce::int64> gridSlices (const ClipFrames& frames, const NoteClip& clip, Ticks division);

/**
    Where transients cut a range into slices (AU3): the onsets strictly inside it whose strength is at least
    1 - sensitivity, at least `minFrames` after the range start and the previous cut.

    @param onsets       Frame and strength (0..1) of each onset, by frame.
    @param range        The frames to slice.
    @param sensitivity  0 keeps only the strongest onset, 1 keeps every one.
    @param minFrames    The shortest slice.
    @return             The cut points, sorted.
*/
[[nodiscard]] std::vector<juce::int64> onsetSlices (const std::vector<std::pair<juce::int64, float>>& onsets,
                                                    std::pair<juce::int64, juce::int64> range, double sensitivity,
                                                    juce::int64 minFrames);

/**
    Most slices a Sampler gets: one per MIDI key.
*/
inline constexpr int maxSlices = 128;

/**
    Notes found in a clip's audio as addClip notes (docs/design/audio-features.md, AU4), placed where the audio plays.

    @param notes       The notes, in seconds from `rangeStart`.
    @param frames      The clip's mapping.
    @param clipStart   The new clip's start: note starts are relative to it.
    @param rangeStart  The file frame the analysed audio began at.
    @param sampleRate  The file's sample rate.
    @param curves      Whether to keep the pitch curves (and the level as pressure).
    @return            [{ start, length, pitch, velocity, expression? }]
*/
[[nodiscard]] juce::var notesVar (const std::vector<DetectedNote>& notes, const ClipFrames& frames, Ticks clipStart,
                                  juce::int64 rangeStart, double sampleRate, bool curves);
} // namespace dawg
