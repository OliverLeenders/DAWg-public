#pragma once

#include "dawg/Curve.h"
#include "dawg/PitchNotes.h"
#include "dawg/Time.h"
#include "dawg/TimingMap.h"
#include "dawg/Vibrato.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace dawg
{

/**
    Stable identifier for model objects (tracks, clips, notes, graph nodes).

    Unique within a project and never reused.
*/
using ObjectId = juce::int64;

/**
    Fade curve of an audio clip fade (docs/design/phase7-audio-tracks.md).
*/
enum class FadeShape
{
    linear,      ///< Straight line: constant gain when two fades of correlated audio cross.
    equalPower,  ///< Quarter sine: constant power when two fades cross.
    exponential, ///< Slow start, fast end (fade in), mirrored for fade out.
    sCurve       ///< Half a cosine: constant gain when two cross, smooth at both ends (audio-features.md, AU6).
};

/**
    A link from a note to the later note that continues its voice (docs/design/linked-notes.md).
*/
struct NoteLink
{
    /**
        The later note in the same clip that continues this note's voice.
    */
    ObjectId to = 0;

    /**
        How long the pitch, pressure, timbre and velocity take to reach the later note's, in milliseconds.
    */
    double glideMs = 100.0;

    /**
        Compares all fields.

        @param other  The link to compare with.
        @return       True if both links are equal.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const NoteLink& other) const noexcept = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    A note with per-note expression.
*/
struct Note
{
    /**
        Unique id of the note within the project.
    */
    ObjectId id = 0;

    /**
        Start relative to the clip start. At least 0.
    */
    Ticks start = 0;

    /**
        Length. Greater than 0.
    */
    Ticks length = ticksPerBeat;

    /**
        Base pitch in MIDI semitones (60 = middle C). May be fractional.
    */
    double pitch = 60.0;

    /**
        Note-on strike velocity, 0..1.
    */
    double velocity = 0.8;

    /**
        Note-off lift velocity, 0..1.
    */
    double releaseVelocity = 0.5;

    /**
        Expression curves, indexed by Lane. Point times are relative to the note start.
    */
    Expression expression;

    /**
        Parametric vibrato added on top of the pitch curve (inactive by default).
    */
    Vibrato vibrato;

    /**
        The note that continues this note's voice, if any (docs/design/linked-notes.md).
    */
    std::optional<NoteLink> link;
};

/**
    Shared handle to an immutable note.
*/
using NotePtr = std::shared_ptr<const Note>;

/**
    A clip of notes on a track.
*/
struct NoteClip
{
    /**
        Unique id of the clip within the project.
    */
    ObjectId id = 0;

    /**
        Start on the timeline. At least 0.
    */
    Ticks start = 0;

    /**
        Length. Notes starting at or after the length do not play; notes reaching past it are cut.
    */
    Ticks length = 4 * ticksPerBeat;

    /**
        The content position shown at the clip's left edge (docs/design/phase6-arrangement.md, A2). A note
        sounds at start + note.start - offset. Trimming the left edge changes start and offset together, so
        the notes stay in place. At least 0.
    */
    Ticks offset = 0;

    /**
        Where the looped content range begins (content position). Only used when loopLength > 0.
    */
    Ticks loopStart = 0;

    /**
        Length of the looped content range, or 0 for no loop (A3). While looping, the clip plays from offset to
        the loop end, then repeats [loopStart, loopStart + loopLength) until it ends.
    */
    Ticks loopLength = 0;

    /**
        Display name; empty shows the track name.
    */
    juce::String name;

    /**
        Display colour as a CSS hex string; empty uses the track colour.
    */
    juce::String colour;

    /**
        For audio clips: the pool file (Project::audioFiles) the clip plays; 0 for note clips. Content position
        0 is the start of the file; offset and the loop range are in ticks of content time, which setTempo
        rescales so the audio keeps its duration (docs/design/phase7-audio-tracks.md, T3).
    */
    ObjectId fileId = 0;

    /**
        Audio clips: gain in decibels.
    */
    double gainDb = 0.0;

    /**
        Audio clips: fade-in length in ticks (0 = none).
    */
    Ticks fadeIn = 0;

    /**
        Audio clips: fade-out length in ticks (0 = none).
    */
    Ticks fadeOut = 0;

    /**
        Audio clips: fade-in curve.
    */
    FadeShape fadeInShape = FadeShape::linear;

    /**
        Audio clips: fade-out curve.
    */
    FadeShape fadeOutShape = FadeShape::linear;

    /**
        Audio clips of musical files in the stretch style: the pitch shift in semitones, cents as the fraction
        (docs/design/time-stretching.md, TS2).
    */
    double transpose = 0.0;

    /**
        Linked clips (shared copies) share a non-zero link id and always hold the same notes, with the same note
        ids: every note edit to one is applied to all of them. Their placement (start, length, offset, loop),
        name and colour stay their own. 0 means the clip is not linked.
    */
    ObjectId linkId = 0;

    /**
        A sequencer clip (docs/design/step-sequencer.md, SS2): the pattern of the track's step sequencer it plays, or
        -1 for a clip of notes. A sequencer clip holds no notes; the sequencer plays only inside it.
    */
    int sequencerPattern = -1;

    /**
        Sequencer clips: the step the pattern starts on at the clip's content origin, 0 for the first (SS3).
    */
    int sequencerStartStep = 0;

    /**
        Whether this is a sequencer clip.
    */
    [[nodiscard]] bool isSequencer() const noexcept { return sequencerPattern >= 0; }

    /**
        The takes lane the clip sits in on its track, 0 for the top row (docs/design/takes-lanes.md, T2). Clips in one
        lane don't overlap.
    */
    int lane = 0;

    /**
        Its place in the track's stack: where clips overlap, the one with the highest order plays (T1). A clip placed
        later gets a higher order.
    */
    std::int64_t order = 0;

    /**
        MIDI controller curves (docs/design/phase10-automation.md, A2), keyed by controller: 0–127 are CCs (64 is
        the sustain pedal), controllerKeyPitchBend and controllerKeyPressure. Times are content positions (they
        move and loop with the clip); values are 0..1 (pitch bend: 0.5 is the centre).
    */
    std::map<int, Curve> controllers;

    /**
        Notes sorted by start, then id.
    */
    std::vector<NotePtr> notes;

    /**
        Finds a note by id.

        @param noteId  The id to look for.
        @return        The index into notes, or -1 if the clip has no such note.
    */
    [[nodiscard]] int indexOfNote (ObjectId noteId) const noexcept;
};

/**
    Shared handle to an immutable clip.
*/
using ClipPtr = std::shared_ptr<const NoteClip>;

/**
    The part of a clip between two timeline positions inside it, as a copy with the same id: the left edge trimmed
    as a split leaves it (the content stays where it is, a looped clip keeps its loop), the right edge cut.

    @param clip  The clip.
    @param from  The part's start, at least clip.start.
    @param to    The part's end, at most the clip's end, after from.
    @return      The part.
*/
[[nodiscard]] NoteClip clipPart (const NoteClip& clip, Ticks from, Ticks to);

/**
    What a track's clips play (docs/design/takes-lanes.md, T1 and T8): where clips overlap, only the one with the
    highest order, the others cut around it with clipPart(). Playback, export and MIDI export go through this, so a
    stack plays as if the covered parts had been cut away, without changing the project.

    @param clips  A track's clips, sorted by start.
    @return       The parts that play, sorted by start: the same clips, unchanged, when none overlap.
*/
[[nodiscard]] std::vector<ClipPtr> playedClips (const std::vector<ClipPtr>& clips);

/**
    Calls fn for every note a clip actually plays, with its timeline position, taking the content offset and
    looping into account (docs/design/phase6-arrangement.md, A2 and A3). Notes are cut at the clip end and, in
    looped clips, at the end of each repeat.

    @param clip  The clip.
    @param fn    Called as fn (const Note& note, Ticks start, Ticks end, int repetition), in content order per
                 repetition; repetition is 0 for clips without a loop.
*/
template <typename Fn>
void forEachAudibleNote (const NoteClip& clip, Fn&& fn)
{
    const auto clipEnd = clip.start + clip.length;

    if (clip.loopLength <= 0)
    {
        for (const auto& note : clip.notes)
        {
            if (note->start < clip.offset || note->start >= clip.offset + clip.length)
                continue;

            const auto start = clip.start + note->start - clip.offset;
            fn (*note, start, std::min (start + note->length, clipEnd), 0);
        }

        return;
    }

    // The first pass plays from the offset to the loop end, every further pass the whole loop range.
    const auto loopEnd = clip.loopStart + clip.loopLength;
    auto contentFrom = clip.offset < loopEnd ? clip.offset : clip.loopStart;
    auto passStart = clip.start;

    for (int repetition = 0; passStart < clipEnd; ++repetition)
    {
        const auto passEnd = std::min (clipEnd, passStart + (loopEnd - contentFrom));

        for (const auto& note : clip.notes)
        {
            if (note->start < contentFrom || note->start >= loopEnd)
                continue;

            const auto start = passStart + note->start - contentFrom;

            if (start < passEnd)
                fn (*note, start, std::min (start + note->length, passEnd), repetition);
        }

        passStart = passEnd;
        contentFrom = clip.loopStart;
    }
}

/**
    Calls fn for every pass of a clip over the timeline: once for a clip without a loop, once per repetition of a
    looped one (docs/design/phase10-automation.md, A5).

    @param clip  The clip.
    @param fn    Called as fn (Ticks start, Ticks end, Ticks contentAt), where contentAt is the content position at
                 start.
*/
template <typename Fn>
void forEachPass (const NoteClip& clip, Fn&& fn)
{
    const auto clipEnd = clip.start + clip.length;

    if (clip.loopLength <= 0)
    {
        fn (clip.start, clipEnd, clip.offset);
        return;
    }

    const auto loopEnd = clip.loopStart + clip.loopLength;
    auto contentFrom = clip.offset < loopEnd ? clip.offset : clip.loopStart;

    for (auto passStart = clip.start; passStart < clipEnd;)
    {
        const auto passEnd = std::min (clipEnd, passStart + (loopEnd - contentFrom));
        fn (passStart, passEnd, contentFrom);
        passStart = passEnd;
        contentFrom = clip.loopStart;
    }
}

/**
    Identifies a hosted plugin (VST3, VST, AU, LV2). See docs/design/phase4-plugin-hosting.md, section 2.
*/
struct PluginRef
{
    /**
        juce::PluginDescription::createIdentifierString(): stable across rescans.
    */
    juce::String identifier;

    /**
        Display name, shown even while the plugin is missing.
    */
    juce::String name;

    /**
        Manufacturer name, for display.
    */
    juce::String manufacturer;

    /**
        Plugin format name, e.g. "VST3".
    */
    juce::String format;

    /**
        Compares all fields.

        @param other  The reference to compare with.
        @return       True if both refer to the same plugin with the same display data.
    */
    [[nodiscard]] bool operator== (const PluginRef& other) const noexcept = default;
};

/**
    How an instrument plugin receives notes. See docs/design/phase4-plugin-hosting.md, section 5.
*/
struct MidiOutputSettings
{
    /**
        The usual pitch-bend range of plain MIDI synths, in semitones.
    */
    static constexpr int plainBendRange = 2;

    /**
        The MPE specification's default per-note pitch-bend range, in semitones.
    */
    static constexpr int mpeBendRange = 48;

    /**
        Returns the default pitch-bend range of a mode.

        @param mpe  True for MPE, false for plain MIDI.
    */
    [[nodiscard]] static constexpr int defaultBendRange (bool mpe) noexcept
    {
        return mpe ? mpeBendRange : plainBendRange;
    }

    /**
        True: MPE (lower zone, one member channel per note). False: plain MIDI on channel 1 (the default for a new
        node; docs/design/phase12-voice-scope.md, V5).
    */
    bool mpe = false;

    /**
        Pitch-bend range in semitones: per note in MPE mode, the channel's range in plain MIDI.
    */
    int bendRange = plainBendRange;

    /**
        Compares all fields.

        @param other  The settings to compare with.
        @return       True if both settings are equal.
    */
    [[nodiscard]] bool operator== (const MidiOutputSettings& other) const noexcept = default;
};

/**
    One modulation input slot on an effect or instrument (docs/design/modulation.md, M2).

    The slot is keyed by the graph node's modulation input port. The source is the connection feeding that port;
    several sources may feed one slot and sum. A macro slot takes its source from a knob of the track's Macros device
    instead, without a cable (docs/design/macros-and-modulation-column.md, MC7). The target is a built-in parameter id
    or a plugin parameter id.
*/
struct ModulationSlot
{
    /**
        The target parameter: a built-in node parameter id, or a plugin parameter id. Empty means unassigned.
    */
    juce::String param;

    /**
        Depth, -1..1. Positive moves the parameter up for a positive source value, negative down. For targets with
        a unipolar modulation polarity, the depth sign is ignored. The offset is scaled by the target parameter's
        modulation range (ParamInfo::modulationRange(): octaves for a frequency, knob travel for another skewed
        parameter, else its range or declared span), so a depth of 1 covers it at a source value of 1. New
        connections start at 0: assigning a target does not change the sound until the depth is raised.
    */
    double depth = 0.0;

    /**
        Whether the slot is active. An inactive slot keeps its target but applies nothing.
    */
    bool active = true;

    /**
        The knob of the track's Macros device that feeds the slot, 0..maxMacros - 1, or -1 for a slot fed by the
        connections to its port (docs/design/macros-and-modulation-column.md, MC7).
    */
    int macro = -1;

    /**
        Compares all fields.

        @param other  The slot to compare with.
        @return       True if both slots are equal.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const ModulationSlot& other) const noexcept = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    How many knobs a Macros device has (docs/design/macros-and-modulation-column.md, MC5).
*/
inline constexpr int maxMacros = 8;

/**
    Ticks in one cycle of an LFO shape curve. Shape point times are 0..this, so the editor can use a normal
    curve with one beat as its cycle.
*/
inline constexpr Ticks modulationShapeTicks = ticksPerBeat;

struct TrackGraph;

/**
    How a sample zone follows its note.
*/
enum class SampleMode
{
    oneShot, ///< Plays to its end whatever the note's length.
    gate,    ///< Plays while the note is held; its envelope releases when the note ends.
    loop,    ///< Loops while the note is held; its envelope releases when the note ends.
};

/**
    Returns a sample mode's name in the project file: "oneShot", "gate" or "loop".
*/
[[nodiscard]] const char* sampleModeName (SampleMode mode) noexcept;

/**
    One step of a step sequencer's pattern row (docs/design/step-sequencer.md, SS6).
*/
struct StepCell
{
    /// Whether the step plays.
    bool on = false;
    /// The step's velocity, 0..1.
    float velocity = 0.8f;

    /**
        The note's length in steps, 0.1 to 16: above 1 it ties over the following steps.
    */
    float length = 1.0f;

    /**
        How likely the step plays, 0..1 (SS7).
    */
    float chance = 1.0f;

    /**
        Hits in the step, 1 to 4.
    */
    int ratchet = 1;

    /// Whether two are the same, member by member.
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const StepCell&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    A step sequencer's row (SS5): the pitch it plays, shared by every pattern.
*/
struct StepRow
{
    /// The MIDI pitch the row plays.
    int pitch = 36;
    /// The row's name, shown in the grid.
    juce::String name;

    /// Whether two are the same, member by member.
    [[nodiscard]] bool operator== (const StepRow&) const = default;
};

/**
    One pattern of a step sequencer (SS4, SS6).
*/
struct StepPattern
{
    /**
        The step size: an index into stepRates() (1/16 by default).
    */
    int rate = 2;

    /**
        Swing, 0.5 (straight) to 0.75.
    */
    float swing = 0.5f;

    /**
        Per row: its length in steps (1 to maxSteps) and its steps (always maxSteps of them).
    */
    struct Row
    {
        /// The row's length in steps, 1 to maxSteps.
        int length = 16;
        /// Its steps: always maxSteps of them.
        std::vector<StepCell> cells;

        /// Whether two are the same, member by member.
        [[nodiscard]] bool operator== (const Row&) const = default;
    };

    /// One Row per row of the sequence.
    std::vector<Row> rows;

    /// Whether two are the same, member by member.
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const StepPattern&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    What a step sequencer plays (nodetype::stepSequencer): its rows and its patterns, each pattern with one Row per row.
*/
struct StepSequence
{
    /// The longest a row can be, in steps.
    static constexpr int maxSteps = 64;
    /// The most rows a sequence holds.
    static constexpr int maxRows = 32;
    /// The most patterns a sequence holds (A to P).
    static constexpr int maxPatterns = 16;

    /// The rows, shared by every pattern.
    std::vector<StepRow> rows;
    /// The patterns, each with one Row per row.
    std::vector<StepPattern> patterns;

    /// Whether two are the same, member by member.
    [[nodiscard]] bool operator== (const StepSequence&) const = default;
};

/**
    The step sizes a pattern can use, in ticks, with their names: 1/4, 1/8, 1/16, 1/32, 1/8T, 1/16T, 1/32T.
*/
struct StepRate
{
    /// The name shown, such as "1/16".
    const char* name;
    /// The step length in ticks.
    Ticks ticks;
};

/// The step sizes a pattern can use, by StepPattern::rate.
inline constexpr std::array<StepRate, 7> stepRates { {
    { "1/4", ticksPerBeat },
    { "1/8", ticksPerBeat / 2 },
    { "1/16", ticksPerBeat / 4 },
    { "1/32", ticksPerBeat / 8 },
    { "1/8T", ticksPerBeat / 3 },
    { "1/16T", ticksPerBeat / 6 },
    { "1/32T", ticksPerBeat / 12 },
} };

/**
    What a Drum Synth pad plays (docs/design/drum-synth.md, DR2, DR6).
*/
enum class DrumModel
{
    off,    ///< Nothing.
    kick,   ///< A sine with a pitch sweep, a click and drive.
    snare,  ///< Two tuned modes and high-passed noise.
    tom,    ///< Three membrane modes with a pitch drop and a stick noise.
    hat,    ///< Six squares through band-passes and a high-pass, with noise.
    cymbal, ///< The hat's metal in three bands, with a bell and stereo width.
    clap,   ///< Band-passed noise bursts and a tail.
    rim,    ///< Two short high modes and a click.
};

/**
    The number of Drum Synth pads, on keys firstDrumKey and up (DR1).
*/
inline constexpr int drumPadCount = 16;

/**
    The key of a Drum Synth's first pad; pad i plays key firstDrumKey + i (General MIDI's kick to ride).
*/
inline constexpr int firstDrumKey = 36;

/**
    One pad of a Drum Synth: what shapes the node, as opposed to its sound, which is in the node's parameters (DR2).
*/
struct DrumPad
{
    /**
        The pad's name, shown on the pad, in the roll and the Step Sequencer.
    */
    juce::String name;

    /**
        What the pad plays.
    */
    DrumModel model = DrumModel::off;

    /**
        The output the pad plays from, 1 (Main) to 16, as SampleZone::output (MO1, MO2).
    */
    int output = 1;

    /**
        Choke group, 1..8, or 0 for none: a hit in a group fades the group's other pads (DR8).
    */
    int chokeGroup = 0;

    /**
        A new hit on the pad fades its last one (DR8).
    */
    bool mono = false;

    /**
        A kit-wide randomise leaves the pad alone (DR14).
    */
    bool locked = false;

    /**
        Compares every field.
    */
    [[nodiscard]] bool operator== (const DrumPad& other) const noexcept = default;
};

/**
    One sample of a Sampler and the notes it plays (docs/design/sampler-midi-delay-limiter.md, section 4).
*/
struct SampleZone
{
    /**
        Unique id of the zone within its node.
    */
    ObjectId id = 0;

    /**
        The sample, in Project::audioFiles.
    */
    ObjectId fileId = 0;

    /**
        Lowest key played, as a MIDI note.
    */
    int lowKey = 0;

    /**
        Highest key played.
    */
    int highKey = 127;

    /**
        The key at which the sample plays at its own pitch.
    */
    int rootKey = 60;

    /**
        Lowest note-on velocity played, 0..127.
    */
    int lowVelocity = 0;

    /**
        Highest note-on velocity played, 0..127.
    */
    int highVelocity = 127;

    /**
        First frame played.
    */
    juce::int64 start = 0;

    /**
        The frame after the last played; 0 for the end of the file.
    */
    juce::int64 end = 0;

    /**
        Tuning in semitones.
    */
    double tune = 0.0;

    /**
        Gain in dB.
    */
    double gainDb = 0.0;

    /**
        Pan, -1..1.
    */
    double pan = 0.0;

    /**
        How the zone follows its note.
    */
    SampleMode mode = SampleMode::oneShot;

    /**
        First looped frame.
    */
    juce::int64 loopStart = 0;

    /**
        The frame the loop jumps back from; 0 for the zone's end.
    */
    juce::int64 loopEnd = 0;

    /**
        Frames of crossfade at the end of the loop.
    */
    juce::int64 crossfade = 0;

    /**
        Choke group, 1..8, or 0 for none: a note starting in a group stops the group's other notes.
    */
    int chokeGroup = 0;

    /**
        Layer, 1..8: a note plays a zone from each layer that holds it (docs/design/sampler-mapping.md, TL1).
    */
    int layer = 1;

    /**
        The Sampler output the zone plays from, 1 (Main) to maxSampleOutputs (docs/design/multi-output-instruments.md,
        MO1). An output with nothing wired to it plays in Main (MO2).
    */
    int output = 1;

    /**
        Velocity crossfade in (docs/design/sampler-mapping.md, SF7): silent at or below this velocity, rising to full level at
        fadeInHighVelocity. Equal to it for none.
    */
    int fadeInLowVelocity = 0;

    /**
        The velocity the fade in reaches full level at.
    */
    int fadeInHighVelocity = 0;

    /**
        Velocity crossfade out: full level at or below this velocity, falling to silence at fadeOutHighVelocity. Equal
        to it for none.
    */
    int fadeOutLowVelocity = 127;

    /**
        The velocity the fade out reaches silence at.
    */
    int fadeOutHighVelocity = 127;

    /**
        Envelope attack in milliseconds.
    */
    double attackMs = 0.0;

    /**
        Envelope decay in milliseconds.
    */
    double decayMs = 0.0;

    /**
        Envelope sustain level, 0..1.
    */
    double sustain = 1.0;

    /**
        Envelope release in milliseconds. A one-shot doesn't release.
    */
    double releaseMs = 50.0;

    /**
        The attack's tension point, as fractions of the segment's rectangle (time, level): 0.5 and 0.5 is a straight
        line (dawg/EnvelopeHandle.h).
    */
    double attackHandleTime = 0.5;

    /**
        See attackHandleTime.
    */
    double attackHandleLevel = 0.5;

    /**
        The decay's tension point, as for the attack.
    */
    double decayHandleTime = 0.5;

    /**
        See decayHandleTime.
    */
    double decayHandleLevel = 0.5;

    /**
        The release's tension point, as for the attack.
    */
    double releaseHandleTime = 0.5;

    /**
        See releaseHandleTime.
    */
    double releaseHandleLevel = 0.5;

    /**
        Compares all fields.

        @param other  The zone to compare with.
        @return       True if both zones are equal.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const SampleZone& other) const noexcept = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

struct ComponentGraph;

/**
    What a Stereo Split or Stereo Merge splits into or joins (docs/design/split-nodes.md, SP4, SP12).
*/
enum class StereoMode : uint8_t
{
    midSide,   ///< M = (L + R) / 2 and S = (L - R) / 2.
    leftRight, ///< The left and the right channel.
};

/**
    The main bus layouts an effect plugin may take, tried in order when the graph compiles
    (docs/design/mono-ports.md, MP4).
*/
enum class PluginChannels : uint8_t
{
    automatic,    ///< Fed mono: mono → mono, mono → stereo, stereo → stereo. Fed stereo: stereo first.
    monoToStereo, ///< Mono → stereo (a stereo input folded), else stereo → stereo.
    stereo,       ///< Stereo → stereo only, as before mono ports.
};

/**
    Returns a PluginChannels value's name in the project file and the bridge: "auto", "monoToStereo" or "stereo".
*/
[[nodiscard]] const char* pluginChannelsName (PluginChannels channels) noexcept;

/**
    Reads a PluginChannels name; anything else is PluginChannels::automatic.
*/
[[nodiscard]] PluginChannels pluginChannelsFromName (const juce::String& name) noexcept;

/**
    A processing node in a track graph.
*/
struct GraphNode
{
    /**
        Unique id of the node within the project.
    */
    ObjectId id = 0;

    /**
        Node type in the NodeRegistry, e.g. "dawg.mpeSynth".
    */
    juce::String type;

    /**
        Parameter values by parameter id. Missing parameters use their default.
    */
    std::map<juce::String, double> params;

    /**
        Horizontal position in the node editor (layout only).
    */
    float x = 0.0f;

    /**
        Vertical position in the node editor (layout only).
    */
    float y = 0.0f;

    /**
        Whether the node is bypassed (effects pass audio through, instruments are muted).
    */
    bool bypassed = false;

    /**
        The hosted plugin, for plugin nodes (nodetype::pluginInstrument and nodetype::pluginEffect).
    */
    std::optional<PluginRef> plugin;

    /**
        The Faust source, for Faust nodes (nodetype::faust; docs/design/phase11-faust-panels.md, F2).
    */
    juce::String code;

    /**
        The plugin's saved state (getStateInformation()), or null. Refreshed from the live instance at
        defined moments; the live instance is the source of truth while it exists (P3). Opaque, and not
        sent to the UI.
    */
    std::shared_ptr<const juce::MemoryBlock> pluginState;

    /**
        How the node's plugin receives notes (instrument plugins only).
    */
    MidiOutputSettings midi;

    /**
        Modulation inputs, keyed by input port index (docs/design/modulation.md, M2). Only ports of type modulation
        use entries; the port's connection(s) supply the source signal.
    */
    std::map<int, ModulationSlot> modulations;

    /**
        The shape of an LFO source's cycle (nodetype::lfo): breakpoints over 0..modulationShapeTicks, values -1..1
        (docs/design/modulation.md, M4). Empty uses a sine. A Waveshaper's drawn curve (nodetype::waveshaper) uses it
        too, its times mapped to the shaper's input from -1 to 1 (docs/design/waveshaper.md, S6); empty is the
        identity.
    */
    Curve modulationShape;

    /**
        The voice graph of a Voice Instrument (nodetype::voiceInstrument; docs/design/phase12-voice-scope.md, V1): the
        nodes that run once per sounding note, with its fixed Voice Input and Voice Output. Null for other nodes.
    */
    std::shared_ptr<const TrackGraph> voiceGraph;

    /**
        What's inside a component (nodetype::component; docs/design/component-nodes.md, N1). Null for other nodes.
    */
    std::shared_ptr<const ComponentGraph> component;

    /**
        The names of a Macros device's knobs (nodetype::macros; docs/design/macros-and-modulation-column.md, MC5), by
        knob; a missing or empty one shows as "Macro N".
    */
    std::vector<juce::String> macroNames;

    /**
        What a Sampler plays (nodetype::sampler and nodetype::sampleVoice).
    */
    std::vector<SampleZone> sampleZones;

    /**
        A Drum Synth's pads (nodetype::drumSynth; docs/design/drum-synth.md, DR2): drumPadCount of them, or empty for
        the default kit.
    */
    std::vector<DrumPad> drumPads;

    /**
        The pool file a node reads (nodetype::usesAudioFile): a Convolution Reverb's impulse response
        (docs/design/convolution-reverb.md, I10). 0 for none.
    */
    ObjectId fileId = 0;

    /**
        An Output to track node's track (nodetype::busOutput; docs/design/multi-output-instruments.md, MO3): an output
        track of the node's own track. 0 for none.
    */
    ObjectId target = 0;

    /**
        Instrument plugins: whether every output bus of the plugin is on, each playing from a port of its own
        (docs/design/multi-output-instruments.md, MO1, "Use all outputs"). Off leaves the buses as the plugin starts.
    */
    bool allOutputs = false;

    /**
        Effect plugins: the layouts its main buses may take (docs/design/mono-ports.md, MP4).
    */
    PluginChannels pluginChannels = PluginChannels::automatic;

    /**
        A Band Split's number of bands, 2 to 4 (nodetype::bandSplit; docs/design/split-nodes.md, SP1).
    */
    int bands = 3;

    /**
        A Stereo Split's or Stereo Merge's parts: mid and side, or left and right (SP4, SP5).
    */
    StereoMode stereoMode = StereoMode::midSide;

    /**
        A step sequencer's rows and patterns (nodetype::stepSequencer; docs/design/step-sequencer.md). Null for other
        nodes.
    */
    std::shared_ptr<const StepSequence> steps;
};

/**
    Kinds of node ports.
*/
enum class PortType
{
    audio,      ///< Audio, mono or stereo (PortInfo::channels; docs/design/mono-ports.md).
    events,     ///< Note events (see NoteEvent).
    modulation, ///< A mono control signal, -1..1 (docs/design/modulation.md, M1).
};

/**
    Returns a port type's name in the project file and the bridge: "audio", "events" or "modulation".
*/
[[nodiscard]] const char* portTypeName (PortType type) noexcept;

/**
    Returns the port type with a name portTypeName() gives; anything else is a modulation port.
*/
[[nodiscard]] PortType portTypeNamed (const juce::String& name) noexcept;

/**
    A bend in a cable on the graph canvas, in the same coordinates as node positions: x along the signal's flow,
    y across it (docs/design/graph-grid-and-cables.md, G5).
*/
struct CablePoint
{
    /**
        Along the flow.
    */
    float x = 0.0f;

    /**
        Across the flow.
    */
    float y = 0.0f;

    /**
        Compares both coordinates.

        @param other  The point to compare with.
        @return       True if both are at the same place.
    */
    [[nodiscard]] bool operator== (const CablePoint& other) const noexcept = default;
};

/**
    A connection from an output port to an input port.
*/
struct Connection
{
    /**
        An unconnected cable, from port 0 of node 0 to port 0 of node 0.
    */
    Connection() = default;

    /**
        A cable between two ports. A constructor rather than aggregate initialisation, so that `{ from, 0, to, 0 }`
        leaves the bends empty without GCC's -Wmissing-field-initializers.

        @param from      Source node id.
        @param output    Output port index on the source node.
        @param to        Destination node id.
        @param input     Input port index on the destination node.
        @param points    Where the cable bends on the canvas; empty for the usual curve.
    */
    Connection (ObjectId from, int output, ObjectId to, int input, std::vector<CablePoint> points = {})
        : fromNode (from),
          fromPort (output),
          toNode (to),
          toPort (input),
          bends (std::move (points))
    {
    }

    /**
        Source node id.
    */
    ObjectId fromNode = 0;

    /**
        Output port index on the source node.
    */
    int fromPort = 0;

    /**
        Destination node id.
    */
    ObjectId toNode = 0;

    /**
        Input port index on the destination node.
    */
    int toPort = 0;

    /**
        Where the cable bends on the canvas, from its output to its input; empty for the usual curve (G5). Only
        the view uses them.
    */
    std::vector<CablePoint> bends;

    /**
        Compares the ports, not the bends: two connections are the same cable if they link the same ports, and
        moving a bend changes nothing the engine plays.

        @param other  The connection to compare with.
        @return       True if both connections link the same ports.
    */
    [[nodiscard]] bool operator== (const Connection& other) const noexcept
    {
        return fromNode == other.fromNode && fromPort == other.fromPort && toNode == other.toNode
               && toPort == other.toPort;
    }
};

/**
    The processing graph of a track. See docs/design/phase2-engine.md, section 5.
*/
struct TrackGraph
{
    /**
        Nodes in creation order.
    */
    std::vector<GraphNode> nodes;

    /**
        Connections between node ports.
    */
    std::vector<Connection> connections;

    /**
        Finds a node by id.

        @param nodeId  The id to look for.
        @return        The index into nodes, or -1 if the graph has no such node.
    */
    [[nodiscard]] int indexOfNode (ObjectId nodeId) const noexcept;
};

/**
    A port of a component (docs/design/component-nodes.md, N2).
*/
struct ComponentPort
{
    /**
        What the port carries.
    */
    PortType type = PortType::audio;

    /**
        The name the user gave it.
    */
    juce::String name;

    /**
        Audio ports: 2 stereo, 1 mono, 0 adaptive, the width of what feeds it (docs/design/mono-ports.md, MP11).
    */
    int channels = 2;

    /**
        Compares all fields.

        @param other  The port to compare with.
        @return       True if both are the same.
    */
    [[nodiscard]] bool operator== (const ComponentPort& other) const = default;
};

/**
    A parameter of a node in a component, shown on the component under a name of its own (docs/design/component-nodes.md, N6). It is the inner
    parameter, not a copy, so undo, automation and modulation all act on the inner node.
*/
struct ExposedParam
{
    /**
        The node, anywhere inside.
    */
    ObjectId nodeId = 0;

    /**
        The parameter's id.
    */
    juce::String param;

    /**
        What the component calls it.
    */
    juce::String name;

    /**
        Compares all fields.

        @param other  The parameter to compare with.
        @return       True if both are the same.
    */
    [[nodiscard]] bool operator== (const ExposedParam& other) const = default;
};

/**
    What's inside a component (docs/design/component-nodes.md, N1): a graph with one Component Input and one Component Output, whose
    ports match the component's own by position. An input port's cables go on from the same port of the Component Input, and what
    reaches a port of the Component Output goes on through the same output port.
*/
struct ComponentGraph
{
    /**
        The name the user gave it; empty for none.
    */
    juce::String name;

    /**
        The nodes inside, and their cables.
    */
    TrackGraph graph;

    /**
        The component's input ports.
    */
    std::vector<ComponentPort> inputs;

    /**
        The component's output ports.
    */
    std::vector<ComponentPort> outputs;

    /**
        The parameters shown on it, in order.
    */
    std::vector<ExposedParam> exposed;
};

/**
    Calls `visit (node, inVoice)` for every node of a graph, at any depth: those in Voice Instruments' voice graphs and in components too
    (docs/design/component-nodes.md, N10). What walks a track's nodes walks them with this.

    @param graph    The graph.
    @param visit    Called with each node, and whether it's in a voice graph, so that it runs per voice.
    @param inVoice  Whether the graph itself is in a voice graph.
*/
template <typename Visit>
void forEachNode (const TrackGraph& graph, Visit&& visit, bool inVoice = false)
{
    for (const auto& node : graph.nodes)
    {
        visit (node, inVoice);

        if (node.voiceGraph != nullptr)
            forEachNode (*node.voiceGraph, visit, true);

        if (node.component != nullptr)
            forEachNode (node.component->graph, visit, inVoice);
    }
}

/**
    The render of one Output to track node of a frozen track (docs/design/audio-features.md, AU15): what it sent to its
    output track, which plays it on while the track is frozen.
*/
struct FrozenOutput
{
    /**
        The Output to track node.
    */
    ObjectId node = 0;

    /**
        Its render in Project::audioFiles.
    */
    ObjectId fileId = 0;
};

/**
    A frozen track's render (docs/design/audio-features.md, AU15).
*/
struct TrackFreeze
{
    /**
        The render in Project::audioFiles, or 0 when the track isn't frozen.
    */
    ObjectId fileId = 0;

    /**
        Where the render starts on the timeline.
    */
    Ticks start = 0;

    /**
        freezeKey() of the track when it was frozen: when the key changes, the render is stale.
    */
    juce::String key;

    /**
        The renders of its Output to track nodes, which start where the main render does.
    */
    std::vector<FrozenOutput> outputs;

    /**
        Returns whether the track is frozen.
    */
    [[nodiscard]] bool active() const noexcept { return fileId != 0; }
};

/**
    What a track plays (docs/design/phase7-audio-tracks.md, T1).
*/
enum class TrackKind
{
    instrument,  ///< Note clips through an instrument.
    audio,       ///< Audio clips through an audio player.
    master,      ///< The master track: the mix through master effects (docs/design/mixing-basics.md, M4).
    group,       ///< A group: its member tracks summed through its own chain (docs/design/groups-and-sends.md, R1).
    returnTrack, ///< A return: the sends to it summed through its own chain (R1, R4).
    output,      ///< An instrument's extra output, fed by its Output to track nodes (multi-output-instruments.md, MO4).
};

/**
    NoteClip::controllers key of channel pitch bend.
*/
inline constexpr int controllerKeyPitchBend = 128;

/**
    NoteClip::controllers key of channel pressure.
*/
inline constexpr int controllerKeyPressure = 129;

/**
    What an automation lane controls (docs/design/phase10-automation.md, A1).
*/
enum class AutomationTargetKind
{
    gain,        ///< The track fader.
    pan,         ///< The track pan.
    nodeParam,   ///< A parameter of a graph node.
    pluginParam, ///< A parameter of a hosted plugin.
    send,        ///< The level of one of the track's sends (docs/design/groups-and-sends.md, R8).
};

/**
    The parameter an automation lane controls.
*/
struct AutomationTarget
{
    /**
        What kind of parameter.
    */
    AutomationTargetKind kind = AutomationTargetKind::gain;

    /**
        The graph node (node and plugin parameters), else 0.
    */
    ObjectId nodeId = 0;

    /**
        The node parameter id, or the plugin's parameter id (stable across plugin versions).
    */
    juce::String param;

    /**
        Display name of a plugin parameter (cached; plugins aren't loaded when the UI lists lanes).
    */
    juce::String name;

    /**
        Whether two targets are the same parameter.

        @param other  The target to compare with.
        @return       True for the same kind, node and parameter.
    */
    [[nodiscard]] bool sameAs (const AutomationTarget& other) const noexcept
    {
        return kind == other.kind && nodeId == other.nodeId && param == other.param;
    }
};

/**
    A track's automation lane: one parameter's values over the timeline.
*/
struct AutomationLane
{
    /**
        Unique id within the project.
    */
    ObjectId id = 0;

    /**
        The parameter.
    */
    AutomationTarget target;

    /**
        Points at absolute timeline ticks, values normalised 0..1 across the parameter's range.
    */
    Curve points;

    /**
        Whether the lane plays (off keeps the points but leaves the parameter alone).
    */
    bool active = true;

    /**
        Lane height in the arrangement, in pixels (layout only).
    */
    int height = 48;
};

/**
    How a track's automation plays and records (docs/design/phase10-automation.md, A6).
*/
enum class AutomationMode
{
    off,   ///< Lanes don't play.
    read,  ///< Lanes play.
    touch, ///< Lanes play; a control that is held writes until it is released.
    latch, ///< Like touch, but writing continues after release until the transport stops.
    write, ///< While playing, every active lane is overwritten with the controls' values.
};

/**
    When an audio track passes its input through (docs/design/phase7-audio-tracks.md, T9).
*/
enum class MonitorMode
{
    off,       ///< Never.
    automatic, ///< While armed, except while playing back.
    on,        ///< Always.
};

/**
    A send from a track to a return track (docs/design/groups-and-sends.md, R4).
*/
struct Send
{
    /**
        Unique id within the project.
    */
    ObjectId id = 0;

    /**
        The return track the send feeds.
    */
    ObjectId target = 0;

    /**
        The node on the target track it feeds: 0 for the track's Mix Input. Reserved for sidechain inputs (R13).
    */
    ObjectId targetNode = 0;

    /**
        Send level in decibels.
    */
    double levelDb = 0.0;

    /**
        Whether the send taps the signal before the track's fader and pan (otherwise after them).
    */
    bool preFader = false;

    /**
        Whether the send is on.
    */
    bool active = true;
};

/**
    A track in the project.

    Immutable once shared: edits create a new Track.
*/
struct Track
{
    /**
        Unique id of the track within the project.
    */
    ObjectId id = 0;

    /**
        Display name shown in the UI.
    */
    juce::String name;

    /**
        Display colour as a CSS hex string, e.g. "#7c8cff".
    */
    juce::String colour = "#7c8cff";

    /**
        Track fader gain in decibels.
    */
    double gainDb = 0.0;

    /**
        Pan position from -1 (left) to +1 (right); 0 is centre (docs/design/mixing-basics.md, M1).
    */
    double pan = 0.0;

    /**
        Timing offset in milliseconds (docs/PLAN.md, phase 13). Positive delays the track; negative pulls it
        earlier. Sends, key sends and the track's group follow it.
    */
    double offsetMs = 0.0;

    /**
        Whether the piano roll treats the track as drums: "auto" (by its name), "drums" or "melodic".
    */
    juce::String drumMode { "auto" };

    /**
        Lane height in the arrangement, in pixels (layout only).
    */
    int height = 64;

    /**
        Instrument, audio, master or group track.
    */
    TrackKind kind = TrackKind::instrument;

    /**
        The group the track belongs to, or 0 at the top level (docs/design/groups-and-sends.md, R2). A group's
        members follow it directly in Project::tracks.
    */
    ObjectId parent = 0;

    /**
        Output tracks: the track whose output it is (docs/design/multi-output-instruments.md, MO4). It starts right
        after that track, in its group, and can be moved and grouped like any track. 0 for other tracks.
    */
    ObjectId source = 0;

    /**
        Output tracks: whether this is the track's Main, which plays what reaches its Track Output. A track with output
        tracks is only a header: its own strip plays nothing, so its Main has a line of its own like its other outputs
        (docs/design/multi-output-instruments.md, MO12).
    */
    bool mainOutput = false;

    /**
        Groups: whether the arrangement folds the members away (layout only).
    */
    bool folded = false;

    /**
        Sends to return tracks, in the order they were added (docs/design/groups-and-sends.md, R4).
    */
    std::vector<Send> sends;

    /**
        Whether the track is muted.
    */
    bool mute = false;

    /**
        Whether the track is soloed (while any track is soloed, only soloed tracks play).
    */
    bool solo = false;

    /**
        Audio tracks: the first input channel recorded and monitored (0-based), or -1 for none.
    */
    int inputChannel = -1;

    /**
        Audio tracks: whether the input is the stereo pair starting at inputChannel.
    */
    bool inputStereo = false;

    /**
        Audio tracks: input monitoring.
    */
    MonitorMode monitor = MonitorMode::automatic;

    /**
        Automation lanes, in display order (docs/design/phase10-automation.md).
    */
    std::vector<AutomationLane> automation;

    /**
        How the automation plays and records.
    */
    AutomationMode automationMode = AutomationMode::read;

    /**
        Whether the arrangement shows the automation lanes under the track (layout only).
    */
    bool automationExpanded = false;

    /**
        How many takes lanes the track has, at least 1 (docs/design/takes-lanes.md, T2); clips sit in lane 0 to
        lanes - 1, the top row first.
    */
    int lanes = 1;

    /**
        Whether the arrangement shows the takes lanes under the track (layout only, T5).
    */
    bool lanesExpanded = false;

    /**
        Note clips sorted by start, then id. They may overlap across lanes; playedClips() gives what plays.
    */
    std::vector<ClipPtr> clips;

    /**
        The track's processing graph.
    */
    TrackGraph graph;

    /**
        Set while the track is frozen (docs/design/audio-features.md, AU15): it plays this render instead of its clips
        and graph.
    */
    TrackFreeze freeze;

    /**
        Finds a clip by id.

        @param clipId  The id to look for.
        @return        The index into clips, or -1 if the track has no such clip.
    */
    [[nodiscard]] int indexOfClip (ObjectId clipId) const noexcept;

    /**
        Finds a node in the track's graph, its Voice Instruments' voice graphs or its components (node ids are unique in the
        project; docs/design/phase12-voice-scope.md, V1).

        @param nodeId  The id to look for.
        @return        The node, or null.
    */
    [[nodiscard]] const GraphNode* findNode (ObjectId nodeId) const noexcept;

    /**
        Finds a node that runs once on the track: one in a component too, but not in a voice graph.

        @param nodeId  The id to look for.
        @return        The node, or nullptr.
    */
    [[nodiscard]] const GraphNode* findTrackNode (ObjectId nodeId) const noexcept;

    /**
        Returns the Voice Instrument or component whose graph directly holds a node.

        @param nodeId  The node.
        @return        Its id, or 0 when the node is in the track's graph or nowhere.
    */
    [[nodiscard]] ObjectId graphOwnerOf (ObjectId nodeId) const noexcept;
};

/**
    Shared handle to an immutable track.
*/
using TrackPtr = std::shared_ptr<const Track>;

/**
    An audio file in the project's pool (docs/design/phase7-audio-tracks.md, T4).
*/
struct AudioFile
{
    /**
        Unique id within the project; clips refer to it.
    */
    ObjectId id = 0;

    /**
        Path relative to the project folder (normally "Audio/<name>.wav"), or absolute while unsaved.
    */
    juce::String path;

    /**
        Display name.
    */
    juce::String name;

    /**
        The file's sample rate in Hz.
    */
    double sampleRate = 48000.0;

    /**
        Number of channels.
    */
    int channels = 2;

    /**
        Length in sample frames.
    */
    juce::int64 frames = 0;

    /**
        Whether the file follows the tempo, and how (docs/design/time-stretching.md, TS2). Shared by every clip of
        the file.
    */
    FileTiming timing;

    /**
        A suggested source tempo from the file's metadata, name or length (TS11), or 0 for none. Never applied by
        itself.
    */
    double suggestedBpm = 0.0;

    /**
        For a reversed copy made by reverseClips (docs/design/audio-features.md, AU1): the pool file it is the reverse
        of, or 0. Reversing a clip of this file points it back at that one.
    */
    ObjectId reverseOf = 0;

    /**
        Its pitch edits (docs/design/pitch-correction.md, PC5). Shared by every clip of the file, like its timing.
    */
    FilePitch pitch;
};

/**
    Returns a pool entry with its basic facts; the rest keep their defaults.
*/
[[nodiscard]] inline AudioFile audioFile (ObjectId id, const juce::String& path, const juce::String& name,
                                          double sampleRate, int channels, juce::int64 frames)
{
    AudioFile file;
    file.id = id;
    file.path = path;
    file.name = name;
    file.sampleRate = sampleRate;
    file.channels = channels;
    file.frames = frames;
    return file;
}

/**
    A named position on the timeline.
*/
struct Marker
{
    /**
        Unique id of the marker within the project.
    */
    ObjectId id = 0;

    /**
        Position on the timeline.
    */
    Ticks position = 0;

    /**
        Display name.
    */
    juce::String name;
};

/**
    Maps a MIDI controller to a DAWg parameter (docs/design/phase9-export-midi-input.md, M8).
*/
struct MidiMapping
{
    /**
        The input device's name; empty for any device.
    */
    juce::String device;

    /**
        MIDI channel, 1..16.
    */
    int channel = 1;

    /**
        Controller number, 0..127.
    */
    int controller = 0;

    /**
        The track (or master track) the parameter belongs to.
    */
    ObjectId trackId = 0;

    /**
        The graph node, or 0 for a track parameter.
    */
    ObjectId nodeId = 0;

    /**
        The parameter: "gain" or "pan" for a track, a parameter id for a node.
    */
    juce::String param;
};

/**
    The transport loop range (docs/design/phase6-arrangement.md, A8).
*/
struct LoopRange
{
    /**
        Whether playback loops.
    */
    bool enabled = false;

    /**
        Loop start.
    */
    Ticks start = 0;

    /**
        Loop end, greater than start.
    */
    Ticks end = 16 * ticksPerBeat;
};

/**
    The punch range (docs/design/audio-features.md, AU10): recordings keep only what was recorded inside it.
*/
struct PunchRange
{
    /**
        Whether recording punches in and out.
    */
    bool enabled = false;

    /**
        The punch-in.
    */
    Ticks start = 4 * ticksPerBeat;

    /**
        The punch-out, after start.
    */
    Ticks end = 8 * ticksPerBeat;
};

/**
    The project's key (docs/design/scale-highlighting.md, SH1): the piano roll highlights and folds to it, and Scale
    Lock can follow it.
*/
struct ProjectKey
{
    /**
        Whether a key is set. Without one the piano roll shows no scale and Scale Lock uses its own.
    */
    bool enabled = false;

    /**
        The root's pitch class, 0 (C) to 11 (B).
    */
    int root = 0;

    /**
        The scale, an index into dawg::scales (Scales.h).
    */
    int scale = 0;

    /// Whether two are the same, member by member.
    [[nodiscard]] bool operator== (const ProjectKey&) const = default;
};

/**
    A key from a bar on (docs/design/scale-highlighting.md, SH9): the project's keys are a list of these, the first on
    bar 0.
*/
struct KeyChange
{
    /**
        The 0-based bar the key starts on.
    */
    int bar = 0;

    /**
        The key from that bar on.
    */
    ProjectKey key;

    /// Whether two are the same, member by member.
    [[nodiscard]] bool operator== (const KeyChange&) const = default;
};

/**
    The project's keys on the timeline in ticks, for the audio thread (SH9): the tempo map has turned their bars into
    ticks.
*/
struct KeyTimeline
{
    /**
        Each key and the tick it starts at, ascending; the first at tick 0.
    */
    std::vector<std::pair<double, ProjectKey>> changes;

    /**
        Returns the key at a tick, or no key when there is none.

        @param tick  The timeline position.
    */
    [[nodiscard]] ProjectKey at (double tick) const noexcept
    {
        ProjectKey key;

        for (const auto& [start, change] : changes)
        {
            if (start > tick)
                break;

            key = change;
        }

        return key;
    }
};

/**
    A parameter anywhere in the project: a built-in node parameter or a plugin parameter
    (docs/design/parameter-linking.md, L1).
*/
struct ParamAddress
{
    /**
        The track (or master track) that holds the node.
    */
    ObjectId trackId = 0;

    /**
        The node, at any depth of components and voice graphs.
    */
    ObjectId nodeId = 0;

    /**
        A built-in parameter id, or a plugin's stable parameter id.
    */
    juce::String param;

    /**
        Compares all fields.

        @param other  The address to compare with.
        @return       True for the same parameter.
    */
    [[nodiscard]] bool operator== (const ParamAddress& other) const = default;
};

/**
    How a link maps its source's value (docs/design/parameter-linking.md, L2).
*/
enum class LinkMappingKind
{
    identity, ///< The same value.
    inverted, ///< Mirrored around a centre: 2c - x.
    offset,   ///< Moved by an amount: x + a.
    scaled,   ///< Scaled around a centre: c + a (x - c).
    range,    ///< The normalised source from inMin..inMax to outMin..outMax, along a curve.
};

/**
    A link's mapping (docs/design/parameter-linking.md, L2). Inverted, offset and scaled work in the parameters' units
    when both ends are built-in parameters with the same unit and modulation scale (octaves for a frequency), and in
    normalised terms otherwise; range always works in normalised terms.
*/
struct LinkMapping
{
    /**
        The kind of mapping.
    */
    LinkMappingKind kind = LinkMappingKind::identity;

    /**
        Offset: what is added (octaves for a frequency). Scaled: the factor.
    */
    double amount = 0.0;

    /**
        Inverted and scaled: the value they turn around, in the units the mapping works in; none for the middle of the
        source's range (its geometric middle for a frequency, 0.5 in normalised terms).
    */
    std::optional<double> centre;

    /**
        Range: the part of the source's normalised range that is mapped.
    */
    double inMin = 0.0;

    /**
        Range: the end of that part.
    */
    double inMax = 1.0;

    /**
        Range: the target's normalised value at inMin.
    */
    double outMin = 0.0;

    /**
        Range: the target's normalised value at inMax (below outMin inverts).
    */
    double outMax = 1.0;

    /**
        Range: a shape from inMin to inMax, times over 0..modulationShapeTicks and values 0..1, or empty for a straight
        line.
    */
    Curve curve;

    /**
        Compares all fields.

        @param other  The mapping to compare with.
        @return       True if both are the same.
    */
    // Exact comparison of the floating-point members is intended (an edit changed them or not).
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    [[nodiscard]] bool operator== (const LinkMapping& other) const noexcept
    {
        const auto samePoint = [] (const CurvePoint& a, const CurvePoint& b)
        {
            return a.time == b.time && a.value == b.value && a.shape == b.shape && a.tension == b.tension
                   && a.handleTime == b.handleTime && a.handleValue == b.handleValue;
        };

        return kind == other.kind && amount == other.amount && centre == other.centre && inMin == other.inMin
               && inMax == other.inMax && outMin == other.outMin && outMax == other.outMax
               && std::ranges::equal (curve, other.curve, samePoint);
    }
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE
};

/**
    A parameter that follows another through a mapping (docs/design/parameter-linking.md, L1). A parameter has at most
    one link driving it, and links never form a cycle (L3).
*/
struct ParamLink
{
    /**
        Unique id within the project.
    */
    ObjectId id = 0;

    /**
        The parameter that drives.
    */
    ParamAddress source;

    /**
        The parameter that follows.
    */
    ParamAddress target;

    /**
        How the source's value becomes the target's.
    */
    LinkMapping mapping;

    /**
        Whether the link applies (off keeps it but leaves the target alone).
    */
    bool active = true;

    /**
        Compares all fields.

        @param other  The link to compare with.
        @return       True if both are the same.
    */
    [[nodiscard]] bool operator== (const ParamLink& other) const = default;
};

/**
    The whole project document.

    The model is an immutable value: commands never mutate a Project in place, they build a new one.
    Unchanged children are shared between versions through shared_ptr<const T>, so snapshots are cheap and
    undo/redo is just swapping the current snapshot.
*/
struct Project
{
    /**
        The tempo points and time signatures (docs/design/tempo-map.md).
    */
    TempoMap tempoMap;

    /**
        Whether the metronome clicks during playback.
    */
    bool metronome = false;

    /**
        Markers sorted by position, then id.
    */
    std::vector<Marker> markers;

    /**
        The transport loop range.
    */
    LoopRange loop;

    /**
        The punch range.
    */
    PunchRange punch;

    /**
        The audio pool, by creation.
    */
    std::vector<AudioFile> audioFiles;

    /**
        The length of the crossfades made where an audio clip is placed over another, in milliseconds, or 0 for none
        (docs/design/audio-features.md, AU7).
    */
    double autoCrossfadeMs = 10.0;

    /**
        The keys the piano roll shows and Scale Lock can follow, sorted by bar, the first on bar 0
        (docs/design/scale-highlighting.md, SH1, SH9).
    */
    std::vector<KeyChange> keys { KeyChange {} };

    /**
        Returns the key in effect on a bar.

        @param bar  A 0-based bar.
    */
    [[nodiscard]] ProjectKey keyAtBar (int bar) const noexcept
    {
        ProjectKey key;

        for (const auto& change : keys)
        {
            if (change.bar > bar)
                break;

            key = change.key;
        }

        return key;
    }

    /**
        Returns the keys with their bars turned into ticks by the tempo map.
    */
    [[nodiscard]] KeyTimeline keyTimeline() const;

    /**
        MIDI learn mappings, by creation.
    */
    std::vector<MidiMapping> midiMappings;

    /**
        Parameter links, by creation (docs/design/parameter-linking.md, L1).
    */
    std::vector<ParamLink> links;

    /**
        Finds the link driving a parameter.

        @param target  The parameter.
        @return        The link, or nullptr when nothing drives it.
    */
    [[nodiscard]] const ParamLink* linkDriving (const ParamAddress& target) const noexcept
    {
        const auto it = std::ranges::find_if (links, [&] (const ParamLink& l) { return l.target == target; });
        return it != links.end() ? &*it : nullptr;
    }

    /**
        Finds an audio file by id.

        @param id  The id to look for.
        @return    The file, or nullptr.
    */
    [[nodiscard]] const AudioFile* findAudioFile (ObjectId id) const noexcept;

    /**
        Tracks in display order.
    */
    std::vector<TrackPtr> tracks;

    /**
        The master track (kind master: no clips, chain Mix Input -> [effects...] -> Gain -> Track Output), or
        nullptr for a project without one, which mixes the tracks straight to the output. See withMaster().
    */
    TrackPtr master;

    /**
        The id the next created object receives.
    */
    ObjectId nextId = 1;

    /**
        Finds a track by id. The master track is not found here; see findTrackOrMaster().

        @param id  The id to look for.
        @return    The track, or nullptr if no track has this id.
    */
    [[nodiscard]] const Track* findTrack (ObjectId id) const noexcept;

    /**
        Finds a track by id, including the master track.

        @param id  The id to look for.
        @return    The track, or nullptr if neither a track nor the master track has this id.
    */
    [[nodiscard]] const Track* findTrackOrMaster (ObjectId id) const noexcept;

    /**
        Finds the position of a track by id.

        @param id  The id to look for.
        @return    The index into tracks, or -1 if no track has this id.
    */
    [[nodiscard]] int indexOfTrack (ObjectId id) const noexcept;
};

/**
    Shared handle to an immutable project snapshot.
*/
using ProjectPtr = std::shared_ptr<const Project>;

/**
    Restores the track list invariants (docs/design/groups-and-sends.md, R2, R10): every parent is an existing group
    that the track doesn't contain (others become 0, top level), each group's members follow it directly, as one
    contiguous block, return tracks sit at the top level after every other track, and sends go to existing return
    tracks other than their own. Tracks keep their relative order otherwise.

    @param project  The project to repair.
*/
void normaliseGroups (Project& project);

/**
    Returns the end of a track's block in a normalised track list: the index after its last member, nested groups
    included. For a track that isn't a group, that is index + 1.

    @param tracks  A normalised track list (see normaliseGroups()).
    @param index   The track's index.
    @return        The index past the track and everything inside it.
*/
[[nodiscard]] size_t subtreeEnd (const std::vector<TrackPtr>& tracks, size_t index);

/**
    Serialises a project to the JSON shape the UI mirrors.

    Collections that are edited element by element (tracks, clips, notes, graph nodes, connections) are
    objects keyed by id, so adding or removing one element produces a single patch operation
    (docs/design/phase2-engine.md, D3). Track order is kept in "trackOrder".

    @param project  The project to serialise.
    @return         { tempoMap, signatures, metronome, trackOrder: [ids], tracks: { id: track }, master: track | null }
*/
[[nodiscard]] juce::var toVar (const Project& project);

/**
    What a track's freeze depends on (docs/design/audio-features.md, AU15): its clips, graph, node automation and
    timing offset, and the tempo map, as a hash. Its fader, pan, sends, mute, name and layout are left out: they stay
    live while the track is frozen.

    @param project  The project.
    @param track    The track.
    @return         The key.
*/
[[nodiscard]] juce::String freezeKey (const Project& project, const Track& track);

/**
    The JSON of what a frozen track may not change while frozen: its clips and graph.

    @param track  The track.
    @return       The JSON text.
*/
[[nodiscard]] juce::String frozenContent (const Track& track);

/**
    How a frozen track compiles (AU15): an audio track playing its render through an Audio Player into its Track
    Output, without input. Its own nodes stay in the graph, cut off from the output, so they keep their instances (a
    plugin stays loaded) but don't run. A track that isn't frozen, or whose render is missing from the pool, comes back
    as it is.

    @param track    The track.
    @param project  The project (the render's pool entry and the tempo map).
    @return         The track to compile.
*/
[[nodiscard]] Track frozenView (const Track& track, const Project& project);

/**
    Whether a track has output tracks (docs/design/multi-output-instruments.md, MO4). Such a track is only a header: its
    own strip plays nothing, and what reaches its Track Output plays on its Main output track (MO12).
*/
[[nodiscard]] bool hasOutputTracks (const Project& project, ObjectId trackId);

/**
    A track's Main output track (MO12), or nullptr if it has none.
*/
[[nodiscard]] const Track* mainOutputOf (const Project& project, ObjectId trackId);

/**
    Computes the patch operations that turn one project's JSON (see toVar()) into another's, as diff() of the two
    serialised projects would.

    Tracks, clips and notes that both snapshots share are unchanged and are skipped without being serialised, so
    the cost follows the size of the change rather than the size of the project.

    @param before  The old project.
    @param after   The new project.
    @return        The operations, in the order they must be applied.
*/
[[nodiscard]] juce::Array<juce::var> diff (const Project& before, const Project& after);

/**
    Serialises a track to JSON.

    @param track  The track to serialise.
    @return       { id, name, colour, gainDb, pan, kind, clips: { id: clip }, graph: { nodes, connections } }
*/
[[nodiscard]] juce::var toVar (const Track& track);

/**
    What's in a component, in the model's JSON form.

    @param node  A component node.
    @return      Its "component" field.
*/
[[nodiscard]] juce::var componentToVar (const GraphNode& node);

/**
    Serialises a clip to JSON.

    @param clip  The clip to serialise.
    @return      { id, start, length, notes: { id: note } }
*/
[[nodiscard]] juce::var toVar (const NoteClip& clip);

/**
    Serialises a note to JSON.

    @param note  The note to serialise.
    @return      { id, start, length, pitch, velocity, releaseVelocity, expression: { lane: [points] } }
*/
[[nodiscard]] juce::var toVar (const Note& note);

/**
    Bounds enforced by commands and by deserialisation.
*/
namespace limits
{
/**
    Most bends a cable can have (docs/design/graph-grid-and-cables.md, G9).
*/
constexpr size_t maxCableBends = 64;

/**
    Lowest allowed track gain in decibels.
*/
constexpr double minGainDb = -96.0;

/**
    Highest allowed track gain in decibels.
*/
constexpr double maxGainDb = 12.0;

/**
    Longest automatic crossfade, in milliseconds (docs/design/audio-features.md, AU7).
*/
constexpr double maxCrossfadeMs = 1000.0;

/**
    Lowest clip transpose in semitones (docs/design/time-stretching.md, TS2).
*/
constexpr double minTranspose = -48.0;

/**
    Highest clip transpose in semitones.
*/
constexpr double maxTranspose = 48.0;

/**
    Lowest source tempo of a musical file.
*/
constexpr double minSourceBpm = 5.0;

/**
    Highest source tempo of a musical file.
*/
constexpr double maxSourceBpm = 2000.0;

/**
    Leftmost pan position.
*/
constexpr double minPan = -1.0;

/**
    Rightmost pan position.
*/
constexpr double maxPan = 1.0;

/**
    Earliest track timing offset in milliseconds.
*/
constexpr double minOffsetMs = -1000.0;

/**
    Latest track timing offset in milliseconds.
*/
constexpr double maxOffsetMs = 1000.0;

/**
    Lowest allowed note pitch (MIDI semitones).
*/
constexpr double minPitch = 0.0;

/**
    Highest allowed note pitch (MIDI semitones).
*/
constexpr double maxPitch = 127.0;

/**
    Latest allowed position on the timeline: 10000 bars of 4/4.
*/
constexpr Ticks maxPosition = 40000 * ticksPerBeat;

/**
    Highest bar a time signature may start on (0-based).
*/
constexpr int maxBar = 9999;

/**
    Most points allowed in one expression curve.
*/
constexpr size_t maxCurvePoints = 4096;

/**
    Most sends one track may have.
*/
constexpr size_t maxSends = 16;

/**
    Shortest clip loop (a 1/16 note), which bounds the number of repeats a looped clip expands to.
*/
constexpr Ticks minLoopLength = ticksPerBeat / 4;

/**
    Lowest lane height in the arrangement, in pixels.
*/
constexpr int minTrackHeight = 24;

/**
    Highest lane height in the arrangement, in pixels.
*/
constexpr int maxTrackHeight = 400;

/**
    Most takes lanes a track holds (docs/design/takes-lanes.md, T2).
*/
constexpr int maxLanes = 64;

/**
    Smallest pitch-bend range for plugin MIDI output, in semitones.
*/
constexpr int minBendRange = 1;

/**
    Largest pitch-bend range for plugin MIDI output, in semitones (the MPE specification's maximum).
*/
constexpr int maxBendRange = 96;

/**
    How many levels of components can nest (docs/design/component-nodes.md, N3).
*/
inline constexpr int maxComponentDepth = 8;
} // namespace limits

} // namespace dawg
