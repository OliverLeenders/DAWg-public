#pragma once

#include "dawg/Curve.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace dawg
{

/**
    One note of a pool file's pitch edits (docs/design/pitch-correction.md, section 4.1, PC5). Frames are the file's
    own; shapes are curves of file frames from the note's start, in semitones relative to their centre.
*/
struct PitchNote
{
    /**
        Unique within the file.
    */
    juce::int64 id = 0;

    /**
        Where its unvoiced lead-in begins (never shifted); equal to start without one.
    */
    juce::int64 leadStart = 0;

    /**
        The voiced part.
    */
    juce::int64 start = 0;

    /**
        The end of the voiced part (exclusive).
    */
    juce::int64 end = 0;

    /**
        The centre detection found, kept so an edit means the same after a new analysis.
    */
    double detectedCentre = 60.0;

    /**
        The centre the user wants; detectedCentre when untouched.
    */
    double targetCentre = 60.0;

    /**
        The shape detection fitted, frozen (relative to detectedCentre).
    */
    Curve detectedShape;

    /**
        The user's shape (relative to targetCentre); detectedShape when untouched.
    */
    Curve shape;

    /**
        Tilt pivots, in frames from the note's start (section 4.5).
    */
    std::vector<juce::int64> pivots;

    /**
        The vibrato's amount: 0 removes it, 1 keeps it, 2 doubles it.
    */
    double vibrato = 1.0;

    /**
        How long the step to a neighbouring note's correction is eased over, in milliseconds.
    */
    double transitionMs = 20.0;

    /**
        Returns whether the note sounds as sung: nothing that changes the pitch was edited.
    */
    [[nodiscard]] bool untouched() const noexcept;

    /**
        Moves the centre to where the edited shape puts the note's perceived pitch, and the shape the other way, so
        what plays doesn't change (section 4.4). The shift is the mean of shape - detectedShape over the middle 70 %
        of the note, the part the analyser measures its centre on; an untouched shape gives 0.
    */
    void recentre();
};

/**
    A pool file's pitch edits (AudioFile::pitch). Empty notes: the file plays as recorded.
*/
struct FilePitch
{
    /**
        The analyser the notes were seeded from (PitchAnalysis::analyser), for information.
    */
    juce::String analyser;

    /**
        The notes, sorted by start, not overlapping.
    */
    std::vector<PitchNote> notes;

    /**
        Returns whether any note is edited.
    */
    [[nodiscard]] bool edited() const noexcept;

    /**
        Returns the note with an id, or null.
    */
    [[nodiscard]] const PitchNote* find (juce::int64 id) const noexcept;

    /**
        Returns whether two files' edits sound the same: the same notes with the same frames, centres, shapes,
        vibrato and transitions (pivots and the analyser don't change the sound).
    */
    [[nodiscard]] bool soundsLike (const FilePitch& other) const noexcept;
};

struct PitchTrack;

/**
    A stretch of a file whose pitch is corrected, rendered as one (section 5.2): notes chained by gaps of under 100 ms
    that change the pitch somewhere.
*/
struct PitchPhrase
{
    /**
        The first frame (the first note's lead-in).
    */
    juce::int64 start = 0;

    /**
        The end, exclusive (the last note's end).
    */
    juce::int64 end = 0;
};

/**
    The correction of a file's pitch in semitones (section 4.3, PC6): per note the change of centre and of shape, and
    the vibrato scaled, with steps between neighbouring notes eased over their transitionMs and 10 ms ramps at each
    note's voiced edges. Zero outside notes and in lead-ins.

    @param pitch       The edits.
    @param sampleRate  The file's sample rate.
    @param start       The first frame wanted.
    @param count       How many values.
    @param hop         Frames between values.
    @param track       The file's pitch, to scale vibrato; null keeps vibrato as sung.
    @return            count values: the correction at start, start + hop, ...
*/
[[nodiscard]] std::vector<float> pitchCorrection (const FilePitch& pitch, double sampleRate, juce::int64 start,
                                                  size_t count, int hop, const PitchTrack* track);

/**
    The phrases of a file that need rendering (section 5.2): edited notes, with their neighbours closer than 100 ms
    chained in, as stretches whose correction exceeds half a cent somewhere.

    @param pitch       The edits.
    @param sampleRate  The file's sample rate.
    @param track       As for pitchCorrection.
    @return            The phrases, in order.
*/
[[nodiscard]] std::vector<PitchPhrase> pitchPhrases (const FilePitch& pitch, double sampleRate,
                                                     const PitchTrack* track);

} // namespace dawg
