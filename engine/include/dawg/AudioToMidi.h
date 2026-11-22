#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <utility>
#include <vector>

namespace dawg
{
/**
    The pitch of a short stretch of audio (docs/design/audio-features.md, AU4).
*/
struct PitchFrame
{
    /**
        The middle of the stretch, in seconds from the audio's start.
    */
    double seconds = 0.0;

    /**
        The pitch in MIDI semitones (fractional), or 0 where the audio has no clear pitch.
    */
    double pitch = 0.0;

    /**
        The RMS level, in dBFS.
    */
    double levelDb = -120.0;
};

/**
    Finds the pitch over time of monophonic audio: YIN (de Cheveigné and Kawahara, 2002) on the mono sum, brought
    down to about 12 kHz, every 5.3 ms, from 50 Hz to 1.5 kHz, with parabolic interpolation of the dip. A stretch has a
    pitch where YIN's normalised difference dips below 0.15 and the level is above -50 dBFS.

    @param audio       The audio.
    @param sampleRate  Its sample rate.
    @return            The frames, in time order.
*/
[[nodiscard]] std::vector<PitchFrame> detectPitch (const juce::AudioBuffer<float>& audio, double sampleRate);

/**
    A note found in audio, in seconds from the audio's start.
*/
struct DetectedNote
{
    /**
        Start in seconds.
    */
    double start = 0.0;

    /**
        End in seconds.
    */
    double end = 0.0;

    /**
        The MIDI key.
    */
    double pitch = 60.0;

    /**
        Velocity, 0..1.
    */
    double velocity = 0.8;

    /**
        The pitch curve: (seconds from the note's start, semitones from its key), thinned. Empty for drums.
    */
    std::vector<std::pair<double, double>> bend;

    /**
        The level curve as pressure: (seconds from the start, 0..1 of the note's peak). Empty for drums.
    */
    std::vector<std::pair<double, double>> level;
};

/**
    Turns pitch frames into notes (AU4): runs of pitched frames, split where the pitch moves more than 0.6 semitone
    from the note's key (its median so far, rounded) for 40 ms, notes shorter than 40 ms dropped. Each note's key is its median pitch rounded,
    the deviation from it is its bend, and its velocity follows its peak level (-40 dBFS to 1, 0 dBFS to 127).

    @param frames  From detectPitch().
    @return        The notes, in time order.
*/
[[nodiscard]] std::vector<DetectedNote> pitchNotes (const std::vector<PitchFrame>& frames);

/**
    Turns drum hits into notes (AU4): one per onset (Onsets.h) at least 1 - sensitivity strong, as long as the gap to the
    next (at most 125 ms), with its velocity from the peak in its first 10 ms. Without `split`, every note is key 36
    (C1); with it, the spectral centroid (by power) of the first 30 ms picks 36 (kick, below 400 Hz), 38 (snare, below 3 kHz) or
    42 (hats).

    @param audio        The audio.
    @param sampleRate   Its sample rate.
    @param sensitivity  0..1.
    @param split        Whether to split kick, snare and hats.
    @return             The notes, in time order.
*/
[[nodiscard]] std::vector<DetectedNote> drumNotes (const juce::AudioBuffer<float>& audio, double sampleRate,
                                                   double sensitivity, bool split);

/**
    The velocity (0..1) of a peak level: -40 dBFS and below is 1/127, 0 dBFS is 1, linear in dB.

    @param peakDb  The peak in dBFS.
    @return        The velocity.
*/
[[nodiscard]] double velocityOfPeak (double peakDb) noexcept;
} // namespace dawg
