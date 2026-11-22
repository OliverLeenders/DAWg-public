#pragma once

#include "../harness/Render.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

// Measurements for the clean side's own acceptance tests (SPEC.md, section 8). They follow the spec's wording; where
// it leaves a method open, the choice is stated here and in DESIGN.md, section 3.

namespace measure
{

/**
    Parameter values by id.
*/
using Params = std::map<std::string, float>;

/**
    Renders an impulse response: a unit impulse at sample 0 on both channels.

    @param params      Parameters (others at their defaults).
    @param seconds     Length of the response.
    @param sampleRate  Sample rate.
    @return            The response.
*/
[[nodiscard]] harness::Audio impulseResponse (const Params& params, double seconds, double sampleRate = 48000.0);

/**
    Renders audio while a function changes parameters before every block.

    @param input       The input.
    @param params      Parameters set before prepare().
    @param blockSize   Samples per block.
    @param change      Called before each block with the node and the block's first sample index.
    @return            The output.
*/
[[nodiscard]] harness::Audio renderChanging (const harness::Audio& input, const Params& params, int blockSize,
                                             const std::function<void (dawg::Node&, size_t)>& change);

/**
    Returns an octave band of a signal: two cascaded second-order bandpasses (RBJ cookbook, constant 0 dB peak) at
    the centre frequency with a one-octave bandwidth.

    @param signal      The signal.
    @param centreHz    The band's centre.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] std::vector<float> octaveBand (const std::vector<float>& signal, double centreHz, double sampleRate);

/**
    Returns the reverberation time of an impulse response's energy, from Schroeder's backward integration with a
    least-squares line through the decay curve between -5 dB and -25 dB (T20), extrapolated to 60 dB.

    @param energy      Squared samples (e.g. left^2 + right^2), from the impulse onwards.
    @param sampleRate  Sample rate.
    @return            T60 in seconds, or 0 if the decay never reaches -25 dB.
*/
[[nodiscard]] double reverberationTime (const std::vector<double>& energy, double sampleRate);

/**
    Returns the RT60 of a stereo impulse response as SPEC.md 8 measures it: the mean of the octave bands at 500 Hz,
    1 kHz and 2 kHz, each from both channels' energy.

    @param response  The impulse response.
*/
[[nodiscard]] double rt60 (const harness::Audio& response);

/**
    Returns the RT60 of each octave band (500 Hz, 1 kHz, 2 kHz).

    @param response  The impulse response.
*/
[[nodiscard]] std::vector<double> rt60Bands (const harness::Audio& response);

/**
    Returns the RT60 of one octave band of a stereo impulse response, from both channels' energy.

    @param response  The impulse response.
    @param centreHz  The octave's centre.
*/
[[nodiscard]] double rt60At (const harness::Audio& response, double centreHz);

/**
    Returns the energy of a band of a stereo signal over a time range, in dB: above lowHz (two cascaded RBJ
    highpasses) and, if highHz is above 0, below highHz (two cascaded RBJ lowpasses).

    @param signal    The signal.
    @param lowHz     The lower edge.
    @param highHz    The upper edge, or 0 for none.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double bandEnergyDb (const harness::Audio& signal, double lowHz, double highHz, double fromSecs,
                                   double toSecs);

/**
    Returns the normalised correlation (sum ab / sqrt (sum a^2 sum b^2)) of two signals over a range of samples.

    @param a     One signal.
    @param b     The other.
    @param from  First sample.
    @param to    One past the last sample.
*/
[[nodiscard]] double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to);

/**
    Returns the correlation of two stereo signals over a time range, both channels together.

    @param a         One signal.
    @param b         The other.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double correlation (const harness::Audio& a, const harness::Audio& b, double fromSecs, double toSecs);

/**
    Returns the largest normalised cross-correlation between a stereo signal's channels over a range of samples, at
    lags up to a maximum in either direction: sum l[n] r[n + lag] / sqrt (sum l^2 sum r^2).

    @param signal      The signal.
    @param from        First sample.
    @param to          One past the last sample.
    @param maxLagSecs  The largest lag, in seconds.
*/
[[nodiscard]] double largestCrossCorrelation (const harness::Audio& signal, size_t from, size_t to, double maxLagSecs);

/**
    Returns the correlation of the two channels' energy envelopes over a range of samples (SPEC 1.4, A9): each
    channel's squared signal averaged over a sliding window, then the Pearson correlation (means removed) of the two.

    @param signal      The signal.
    @param from        First sample.
    @param to          One past the last sample.
    @param smoothSecs  The averaging window, in seconds.
*/
[[nodiscard]] double envelopeCorrelation (const harness::Audio& signal, size_t from, size_t to, double smoothSecs);

/**
    Returns one channel's energy over a time range, in dB.

    @param signal    The signal.
    @param channel   0 for left, 1 for right.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double channelEnergyDb (const harness::Audio& signal, size_t channel, double fromSecs, double toSecs);

/**
    Returns the echo density: local maxima of |h| above -40 dB re the response's peak, per 10 ms, between two times,
    averaged over both channels.

    @param response  The impulse response.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double echoDensity (const harness::Audio& response, double fromSecs, double toSecs);

/**
    Returns the energy of an octave band of a stereo signal over a time range, in dB.

    @param signal    The signal.
    @param centreHz  The band's centre.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double bandLevelDb (const harness::Audio& signal, double centreHz, double fromSecs, double toSecs);

/**
    Returns the RMS of both channels together over a time range, in dB.

    @param signal    The signal.
    @param fromSecs  Start, in seconds.
    @param toSecs    End, in seconds.
*/
[[nodiscard]] double rmsDb (const harness::Audio& signal, double fromSecs, double toSecs);

/**
    Returns seeded pink noise (Voss-McCartney: 16 octave-spaced random sources summed), independent per channel,
    scaled to -12 dBFS RMS.

    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] harness::Audio pinkNoise (double seconds, double sampleRate = 48000.0);

/**
    Returns a sine on both channels.

    @param hz          Frequency.
    @param amplitude   Peak amplitude.
    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] harness::Audio sine (double hz, double amplitude, double seconds, double sampleRate = 48000.0);

/**
    How smooth a signal's high frequencies are (A14; DESIGN.md, section 4.3).
*/
struct Smoothness
{
    double medianExcessKurtosis = 0.0; ///< Median over 10 ms windows of kurtosis - 3: 0 for Gaussian noise.
    double worstExcessKurtosis = 0.0;  ///< 90th percentile of the same: the grainiest stretches.
    double medianDensity = 0.0;        ///< Median of P(|x| > sigma) / 0.3173: 1 for Gaussian noise, lower if sparse.
    double worstDensity = 0.0;         ///< 10th percentile of the same.
    int windows = 0;                   ///< Number of windows measured.
};

/**
    Returns the high-band echo density of a response over time: for the band above 4 kHz, in consecutive 5 ms windows
    per channel, the fraction of samples beyond one standard deviation of the window, divided by its value for
    Gaussian noise (0.3173), averaged over both channels. 1 where the response is as dense and smooth as noise; lower
    where separate echoes or clicks stand out.

    @param response  The response.
    @param fromSecs  Start.
    @param toSecs    End.
    @return          One value per window.
*/
[[nodiscard]] std::vector<double> densityProfile (const harness::Audio& response, double fromSecs, double toSecs);

/**
    Returns a high-passed copy of a signal: two cascaded second-order RBJ highpasses (Q 0.707).

    @param signal      The signal.
    @param cornerHz    The corner.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] std::vector<float> highpass (const std::vector<float>& signal, double cornerHz, double sampleRate);

/**
    Measures the smoothness of a tail's high frequencies: the band above 4 kHz, in 10 ms windows per channel, from a
    time until the band has decayed 40 dB below its loudest window (or the end).

    @param signal    The output.
    @param fromSecs  Where the tail starts (after the input has stopped).
    @return          Kurtosis and density statistics.
*/
[[nodiscard]] Smoothness smoothness (const harness::Audio& signal, double fromSecs);

/**
    Returns how much the high band's envelope fluctuates beyond what its spectrum explains: the coefficient of
    variation of 2 ms frame energies above 4 kHz, divided by the same for a phase-randomised copy (the same spectrum,
    stationary). 1 for a time-invariant reverb fed with steady noise; higher when the reverb adds flutter, crackle or
    sizzle in the highs. Averaged over both channels.

    @param output    The output, fed with steady noise.
    @param fromSecs  Start of the steady part.
    @param toSecs    End (at least 2^16 samples after the start is best).
*/
[[nodiscard]] double highFluctuation (const harness::Audio& output, double fromSecs, double toSecs);

/**
    Returns a synthetic snare-like hit: a 3 ms attack of white noise and a 180 Hz tone, both decaying (noise with a
    40 ms, tone with a 25 ms time constant), peak about 0.7, the same on both channels, then silence.

    @param seconds     Total length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] harness::Audio snare (double seconds, double sampleRate = 48000.0);

/**
    Returns the largest step between consecutive samples, both channels, from a sample on.

    @param signal  The signal.
    @param from    First sample.
*/
[[nodiscard]] float largestStep (const harness::Audio& signal, size_t from = 0);

/**
    Returns the index of the first sample, either channel, whose magnitude reaches a fraction of the peak.

    @param signal    The signal.
    @param fraction  Of the peak magnitude.
*/
[[nodiscard]] size_t firstAbove (const harness::Audio& signal, float fraction);

// SPEC 1.5, section 8: the definitions used by A14, A18 and A19 (filters Butterworth, applied forwards and backwards).

/**
    Returns a signal filtered forwards and backwards by a fourth-order Butterworth highpass or lowpass (two biquads,
    Q 0.5412 and 1.3066, bilinear transform with prewarping) [TXT].

    @param signal      The signal.
    @param cornerHz    The corner.
    @param high        True for a highpass, false for a lowpass.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] std::vector<double> butterworth (const std::vector<double>& signal, double cornerHz, bool high,
                                               double sampleRate);

/**
    A14's roughness of a response's band above 8 kHz (SPEC 1.5), and that of its reference.
*/
struct Roughness
{
    double response = 0.0;  ///< Mean over both channels.
    double reference = 0.0; ///< Mean over both channels and four random-phase draws.

    [[nodiscard]] double ratio() const { return reference > 0.0 ? response / reference : 0.0; }
};

/**
    Measures A14's roughness (SPEC 1.5): the band above 8 kHz (fourth-order highpass), the segment from 0.2 to 1.0 s on
    each channel, the standard deviation of its 2 ms frame energies in dB minus their 25-frame centred moving average.
    The reference: a phase-randomised copy of the segment, scaled sample by sample so its 50 ms moving RMS equals the
    segment's.

    @param response  An impulse response (Mix 100 %, Pre-delay 0).
*/
[[nodiscard]] Roughness highRoughness (const harness::Audio& response);

/**
    Returns A18's held note (SPEC 1.5): a sine of amplitude 0.25 for 8 s, ending with a 5 ms linear fade (from 7.995 s),
    then 10 s of silence, the same on both channels.

    @param hz          The note.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] harness::Audio heldNote (double hz, double sampleRate = 48000.0);

/**
    Returns SPEC 1.5's ideal reverb of an input: per channel, independent Gaussian white noise times 10^(-3t/T), of
    length 3T but at most 18 s, convolved with the input (its left channel, for both). The output has the input's
    length.

    @param input  The input.
    @param t60    T, in seconds.
    @param seed   The noise draw.
*/
[[nodiscard]] harness::Audio idealReverb (const harness::Audio& input, double t60, unsigned seed);

/**
    A18's measures of a held note's reverb on one channel (SPEC 1.5).
*/
struct NoteMotion
{
    double swingDb = 0.0;    ///< Peak-to-peak range of the smoothed amplitude, in dB.
    double driftCents = 0.0; ///< Standard deviation of the smoothed instantaneous frequency, in cents.
};

/**
    Measures A18 on one channel (SPEC 1.5): from 5.0 to 7.9 s after the note starts, the output band-passed from 0.8 f
    to 1.25 f (fourth-order highpass and lowpass), its analytic signal, its amplitude in dB and instantaneous frequency
    in cents re f, each smoothed by a 20 ms moving average.

    @param channel     The output.
    @param hz          The note, f.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] NoteMotion noteMotion (const std::vector<float>& channel, double hz, double sampleRate);

/**
    Measures A19 on one channel (SPEC 1.5): from 50 ms after the held note ends (8.0 s), energies of 100 ms frames in
    dB up to the first frame 30 dB below the first; the standard deviation of these around a cubic fit over time.

    @param channel     The output.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] double releaseUnevenness (const std::vector<float>& channel, double sampleRate);

} // namespace measure
