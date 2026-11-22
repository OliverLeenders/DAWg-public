#pragma once

#include "dawg/AudioSource.h"
#include "dawg/Node.h"
#include "dawg/TimingMap.h"

#include <functional>
#include <memory>
#include <vector>

namespace dawg
{

class StretchRenderer;
struct StretchJob;

/**
    A stretch of a musical clip's playback (docs/design/time-stretching.md, TS7): the file played straight where the
    ratio is 1, a render where it is stretched, or a repitch preview of a render that isn't ready yet (and everywhere
    in the repitch style). Pieces meet at the markers, where they crossfade over joinSeconds.
*/
struct CompiledAudioPiece
{
    /**
        What the piece plays.
    */
    enum class Kind
    {
        file,   ///< The file at its own speed: frame = (seconds - anchorSeconds) * rate.
        render, ///< A render, in timeline time: frame = (seconds - anchorSeconds) * rate.
        repitch ///< The file through the timing map: content = timeline tick (musical) or second (one-shot) - delta.
    };

    /**
        What the piece plays.
    */
    Kind kind = Kind::file;

    /**
        The timeline seconds the piece covers (before the crossfades at its joins).
    */
    double startSeconds = 0.0;

    /**
        The end of the piece.
    */
    double endSeconds = 0.0;

    /**
        Whether the piece crossfades with the previous one at its start.
    */
    bool joinIn = false;

    /**
        Whether the piece crossfades with the next one at its end.
    */
    bool joinOut = false;

    /**
        The file, or the render.
    */
    std::shared_ptr<const AudioSource> source;

    /**
        File and render pieces: the timeline second of the source's frame 0.
    */
    double anchorSeconds = 0.0;

    /**
        Repitch pieces: the timeline tick of content tick 0 (musical files), or the timeline second of content second
        0 (one-shot files).
    */
    double delta = 0.0;

    /**
        File and render pieces after a join with a render: how far the piece is offset, in seconds (it plays its
        source at seconds + shift), to line it up with the piece before over their crossfade. Renders don't keep the
        source's phase, so unaligned joins of tonal audio would cancel. The offset is constant over the piece, so it
        never changes the pitch, and at most maxAlignSeconds either way, so it never adds up along a clip.
    */
    double shift = 0.0;
};

/**
    Half the crossfade between neighbouring pieces, in seconds (10 ms in all, TS9).
*/
inline constexpr double joinSeconds = 0.005;

/**
    An audio clip flattened for playback (docs/design/phase7-audio-tracks.md, T8). Positions are timeline
    ticks; content positions are ticks of content time from the start of the file.
*/
struct CompiledAudioClip
{
    /**
        The clip id (for dropout reports).
    */
    ObjectId id = 0;

    /**
        The pool file the clip plays.
    */
    ObjectId fileId = 0;

    /**
        Timeline start and end.
    */
    Ticks start = 0;

    /**
        Timeline end, greater than start.
    */
    Ticks end = 0;

    /**
        Content position at the clip start, and the loop range (loopLength 0 = no loop), as in NoteClip. Content
        positions, fades and localLength are ticks at the tempo where the clip starts (docs/design/tempo-map.md,
        TM8), which run at a constant rate in real time.
    */
    Ticks offset = 0;

    /**
        Start of the looped content range.
    */
    Ticks loopStart = 0;

    /**
        Length of the looped content range, or 0.
    */
    Ticks loopLength = 0;

    /**
        Linear clip gain.
    */
    float gain = 1.0f;

    /**
        Fade lengths in content ticks.
    */
    Ticks fadeIn = 0;

    /**
        Fade-out length in ticks.
    */
    Ticks fadeOut = 0;

    /**
        Fade curves.
    */
    FadeShape fadeInShape = FadeShape::linear;

    /**
        Fade-out curve.
    */
    FadeShape fadeOutShape = FadeShape::linear;

    /**
        The file's audio, or null if it is missing (the clip then plays silence).
    */
    std::shared_ptr<const AudioSource> source;

    /**
        Seconds of the file per content tick: the tick length at the tempo where the clip starts.
    */
    double secondsPerTick = 0.0;

    /**
        Seconds per content tick at the clip's end, for its declick: secondsPerTick for one-shot files, the tick
        length at the tempo there for musical ones.
    */
    double endSecondsPerTick = 0.0;

    /**
        The real time of the clip start, through the tempo map.
    */
    double startSeconds = 0.0;

    /**
        The clip's duration in content ticks (its real time over secondsPerTick). For musical files, its length in
        timeline ticks.
    */
    double localLength = 0.0;

    /**
        Whether the file is musical (docs/design/time-stretching.md, TS2): content positions, fades and localLength
        are then musical ticks, which run with the timeline's ticks.
    */
    bool musical = false;

    /**
        For clips of musical files and one-shot files with markers: the file's timing map, in content ticks or
        content seconds. The file frame of a content position comes from it (repitch, TS7). Null otherwise.
    */
    std::shared_ptr<const TimingMap> timing;

    /**
        For clips with a timing map: what plays where, sorted by start. Such clips play their pieces instead of the
        source.
    */
    std::vector<CompiledAudioPiece> pieces;

    /**
        How many of the pieces preview a render that is still being made.
    */
    int pendingRenders = 0;
};

/**
    A track's audio clips compiled for playback. Immutable; shared by compiled engines.
*/
struct AudioSequence
{
    /**
        The clips sorted by start.
    */
    std::vector<CompiledAudioClip> clips;
};

/**
    Opens the audio of a pool file, or returns nullptr if it can't be read (missing file).
*/
using AudioSourceResolver = std::function<std::shared_ptr<const AudioSource> (const AudioFile& file)>;

/**
    Compiles a track's audio clips.

    @param track    The track.
    @param project  The project (audio pool and tempo).
    @param resolve  Opens audio files.
    @param stretch  Makes and caches renders of stretched musical files; without one, they play the repitch preview.
    @return         The compiled clips.
*/
[[nodiscard]] AudioSequence compileAudio (const Track& track, const Project& project,
                                          const AudioSourceResolver& resolve, StretchRenderer* stretch = nullptr);

/**
    Returns the renders the project's musical clips need (docs/design/time-stretching.md, TS10), for export.

    @param project  The project.
    @return         The jobs, one per stretched marker segment and pass (duplicates share a key).
*/
[[nodiscard]] std::vector<StretchJob> stretchJobs (const Project& project);

/**
    The width of an Audio Player's output (docs/design/mono-ports.md, MP15): mono when every clip it plays is from a
    mono file and the track's input, if it has one, is mono; stereo otherwise, and when there is neither.

    @param track     The player's track.
    @param project   The project, for the pool's files.
    @param onlyFile  The one file the player plays (GraphNode::fileId), or 0 for all of the track's clips.
    @return          1 or 2.
*/
[[nodiscard]] int playerWidth (const Track& track, const Project& project, ObjectId onlyFile) noexcept;

/**
    Description of nodetype::audioPlayer.
*/
[[nodiscard]] const NodeDescriptor& audioPlayerDescriptor() noexcept;

/**
    Creates the audio player node: renders the track's audio clips (ProcessContext::audio) while the transport
    runs, resampled to the device rate, with clip gain, fades and short declick fades. Its output is adaptive: the
    compiler gives it the width playerWidth() answers, and a mono player writes one channel.

    @return  The node.
*/
[[nodiscard]] std::unique_ptr<Node> makeAudioPlayerNode();

/**
    The fade gain at a position x in [0, 1] of a fade (0 = silent end, 1 = full level).

    @param shape  The fade curve.
    @param x      Position within the fade.
    @return       The gain.
*/
[[nodiscard]] float fadeGain (FadeShape shape, double x) noexcept;

} // namespace dawg
