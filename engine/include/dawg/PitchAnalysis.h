#pragma once

#include "dawg/Curve.h"
#include "dawg/Onsets.h"
#include "dawg/PitchModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <functional>
#include <vector>

namespace dawg
{

/**
    The analyser's version (docs/design/pitch-correction.md, PC1). Raised whenever detection changes, so cached
    analyses are redone; the model's identity is part of the version too (PitchAnalysis::analyser).
*/
inline constexpr int pitchAnalysisVersion = 1;

/**
    The pitch of monophonic audio, frame by frame, at the pitch model's hop.
*/
struct PitchTrack
{
    /**
        The audio's sample rate, to turn frames into the audio's own frames.
    */
    double sampleRate = 44100.0;

    /**
        The middle of the first frame, in seconds from the audio's start.
    */
    double startSeconds = 0.0;

    /**
        Seconds between frames.
    */
    double hopSeconds = 0.01;

    /**
        MIDI semitones per frame; NaN where unvoiced.
    */
    std::vector<float> pitch;

    /**
        How likely each frame is voiced, 0..1.
    */
    std::vector<float> voicedProb;

    /**
        The RMS level per frame, in dBFS.
    */
    std::vector<float> levelDb;

    /**
        The number of frames.
    */
    [[nodiscard]] size_t size() const noexcept { return pitch.size(); }

    /**
        The audio frame at the middle of a track frame.
    */
    [[nodiscard]] juce::int64 audioFrameOf (double index) const noexcept;

    /**
        The track frame (fractional) at an audio frame.
    */
    [[nodiscard]] double indexOf (juce::int64 audioFrame) const noexcept;

    /**
        Whether a frame is voiced.
    */
    [[nodiscard]] bool voiced (size_t index) const noexcept { return ! std::isnan (pitch[index]); }
};

/**
    A note found in monophonic audio (section 3.4).
*/
struct DetectedPitchNote
{
    /**
        Where the unvoiced lead-in (a consonant, a breath) begins, in audio frames; equal to start without one. It
        moves with the note in time, but is never pitch-shifted.
    */
    juce::int64 leadStart = 0;

    /**
        The voiced part, in audio frames.
    */
    juce::int64 start = 0;

    /**
        The end of the voiced part (exclusive), in audio frames.
    */
    juce::int64 end = 0;

    /**
        The perceived pitch, in MIDI semitones.
    */
    double centre = 60.0;

    /**
        The slow pitch movement relative to the centre (section 3.5): time in audio frames from start, values in
        semitones. One run of smooth segments (CurveShape::smooth).
    */
    Curve shape;

    /**
        The vibrato's rate in Hz and depth in cents (peak); both 0 without vibrato.
    */
    double vibratoRate = 0.0;

    /**
        @copydoc vibratoRate
    */
    double vibratoDepth = 0.0;
};

/**
    A full analysis: the track and its notes.
*/
struct PitchAnalysis
{
    /**
        The analyser: pitchAnalysisVersion and the model's identity. Another analyser means a cached analysis is
        stale.
    */
    juce::String analyser;

    /**
        The pitch per frame.
    */
    PitchTrack track;

    /**
        The notes, in time order, not overlapping.
    */
    std::vector<DetectedPitchNote> notes;
};

/**
    Prepares audio for a pitch model (section 3.1): the mono sum of the first two channels, a 40 Hz high-pass, and
    resampling to the model's rate.

    @param audio       The audio.
    @param sampleRate  Its sample rate.
    @param modelRate   The rate the model reads.
    @return            Mono audio at modelRate.
*/
[[nodiscard]] std::vector<float> preparePitchAudio (const juce::AudioBuffer<float>& audio, double sampleRate,
                                                    double modelRate);

/**
    Turns a model's salience into a pitch track (section 3.3): Viterbi decoding over the pitch bins and an unvoiced
    state, then per frame the salience-weighted pitch around the best path's bin.

    @param salience    The model's output.
    @param threshold   The model's voicing threshold (PitchModel::voicingThreshold).
    @param levelDb     The level per salience frame, in dBFS; frames below -60 dBFS are unvoiced. May be empty.
    @param sampleRate  The audio's sample rate, stored in the track.
    @return            The track.
*/
[[nodiscard]] PitchTrack decodePitch (const PitchSalience& salience, float threshold, const std::vector<float>& levelDb,
                                      double sampleRate);

/**
    Finds the notes in a pitch track (sections 3.4 and 3.5): segmentation, centres, vibrato and fitted shapes.

    @param track   The track.
    @param onsets  The audio's onsets (Onsets.h), which split repeated notes.
    @return        The notes, in time order.
*/
[[nodiscard]] std::vector<DetectedPitchNote> findPitchNotes (const PitchTrack& track, const std::vector<Onset>& onsets);

/**
    Analyses monophonic audio: preparation, the model, decoding, onsets and notes.

    @param audio       The audio.
    @param sampleRate  Its sample rate.
    @param model       The pitch model.
    @param onsets      The audio's onsets if they are known already (the audio library caches them); null finds them.
    @param cancelled   Asked now and then; true stops early with an empty analysis. May be empty.
    @return            The analysis; its track is empty if cancelled or the model failed.
*/
[[nodiscard]] PitchAnalysis analysePitch (const juce::AudioBuffer<float>& audio, double sampleRate,
                                          const PitchModel& model, const std::vector<Onset>* onsets = nullptr,
                                          const std::function<bool()>& cancelled = {});

/**
    An analysis as the bridge sends it and the cache stores it (section 3.6).

    @param analysis  The analysis.
    @return          { analyser, sampleRate, start, hop, pitch, notes }: start is the first pitch value's time in
                     seconds; pitch holds every second frame of the track (so hop is twice the track's), in semitones
                     rounded to a cent, null where unvoiced; each note is { leadStart, start, end, centre, vibratoRate,
                     vibratoDepth, shape } with frames in the audio's own frames and shape as [[time, value, shape]].
*/
[[nodiscard]] juce::var pitchAnalysisToVar (const PitchAnalysis& analysis);

/**
    The perceived pitch of a stretch of a track (section 3.4, step 5): the mean pitch weighted by level over the
    middle 70 % of its voiced frames.

    @param track  The track.
    @param start  The first audio frame.
    @param end    The end, exclusive, in audio frames.
    @return       The pitch in semitones, or NaN if nothing in the stretch is voiced.
*/
[[nodiscard]] double measureCentre (const PitchTrack& track, juce::int64 start, juce::int64 end);

/**
    Fits a curve of smooth segments to the slow pitch movement of a stretch of a track (section 3.5, PC4).

    @param track   The track.
    @param start   The first audio frame.
    @param end     The end, exclusive, in audio frames.
    @param centre  The pitch the shape is relative to.
    @return        The shape: time in audio frames from start, values in semitones; empty if nothing is voiced.
*/
[[nodiscard]] Curve fitShape (const PitchTrack& track, juce::int64 start, juce::int64 end, double centre);

} // namespace dawg
