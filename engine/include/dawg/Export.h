#pragma once

#include "dawg/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace dawg
{

/**
    Integrated loudness and sample peak of a rendered signal (docs/design/phase9-export-midi-input.md, X5):
    ITU-R BS.1770 / EBU R128, with K-weighting, 400 ms blocks overlapping by 75 %, and absolute (−70 LUFS) and
    relative (−10 LU) gating.
*/
class LoudnessMeter final
{
public:
    /**
        Starts a new measurement.

        @param sampleRate  The signal's sample rate.
    */
    void prepare (double sampleRate);

    /**
        Measures a stereo block.

        @param channels    Left and right samples.
        @param numSamples  Number of samples.
    */
    void process (std::array<const float*, 2> channels, int numSamples);

    /**
        Returns the integrated loudness in LUFS, or −infinity for silence.
    */
    [[nodiscard]] double integratedLufs() const;

    /**
        Returns the highest absolute sample.
    */
    [[nodiscard]] float samplePeak() const noexcept { return m_peak; }

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // NOLINT(readability-isolate-declaration)
        std::array<double, 2> z1 {};
        std::array<double, 2> z2 {};

        [[nodiscard]] double process (double x, size_t ch) noexcept;
    };

    double m_sampleRate = 48000.0;
    Biquad m_shelf;
    Biquad m_highPass;
    int m_blockStep = 0; // samples between block starts (100 ms)
    double m_stepEnergy = 0.0;
    int m_stepCount = 0;
    std::array<double, 4> m_steps {}; // the last four 100 ms energies
    int m_stepsSeen = 0;
    std::vector<double> m_blocks; // mean square per 400 ms block
    float m_peak = 0.0f;
};

/**
    What an export renders (docs/design/phase9-export-midi-input.md, X2, X3).
*/
struct ExportRange
{
    /**
        Start in beats.
    */
    double startBeats = 0.0;

    /**
        End in beats (before the tail).
    */
    double endBeats = 4.0;

    /**
        Whether the tail ends automatically once the output stays below −90 dBFS for a second.
    */
    bool autoTail = true;

    /**
        A fixed tail in seconds (when autoTail is off), or the longest automatic tail.
    */
    double tailSeconds = 30.0;

    /**
        The tracks of a stem (summed), or none for the mix.
    */
    std::vector<ObjectId> tracks;

    /**
        Whether the stem's tracks are taken before their fader and pan (docs/design/audio-features.md, AU14).
    */
    bool preFader = false;

    /**
        Output to track nodes of the stem's tracks whose input is rendered too, each to a pair of channels of its own
        (a freeze, docs/design/audio-features.md, AU15). At most AudioEngine::maxExportTaps.
    */
    std::vector<ObjectId> taps;
};

/**
    Renders a range offline through the engine. The device must not be running the engine meanwhile.

    @param engine      The engine, with the project's compiled engine set.
    @param range       What to render.
    @param sampleRate  The sample rate (the compiled engine's).
    @param blockSize   Samples per block (at most the compiled engine's block size).
    @param sink        Receives each block (left and right); returns false to cancel.
    @param tapSink     Receives each block of the taps (left and right of each, in the order of range.taps); returns
                       false to cancel. The tail lasts until they are quiet too.
    @return            The number of frames rendered, or −1 if cancelled.
*/
[[nodiscard]] juce::int64
renderRange (AudioEngine& engine, const ExportRange& range, double sampleRate, int blockSize,
             const std::function<bool (std::array<const float*, 2>, int)>& sink,
             const std::function<bool (std::span<const float* const>, int)>& tapSink = nullptr);

/**
    File formats for export (X4).
*/
enum class ExportFormat
{
    wav16,
    wav24,
    wav32,
    flac16,
    flac24,
    mp3,
};

/**
    An MP3's ID3v2 tag (X6). Empty fields are left out; the title falls back to the file's name.
*/
struct Mp3Tags
{
    /// The title; empty for the file name.
    juce::String title;
    /// The artist.
    juce::String artist;
    /// The album.
    juce::String album;
    /// The year.
    juce::String year;
    /// The track number, as text ("3" or "3/12").
    juce::String track;
    /// The genre.
    juce::String genre;
    /// A comment.
    juce::String comment;
};

/**
    How an MP3 is encoded (X6): a constant bitrate, or LAME's variable bitrate at a quality.
*/
struct Mp3Settings
{
    /** The constant bitrate in kbps (320, 256, 192 or 128), used unless vbrQuality is set. */
    int bitrateKbps = 320;

    /** LAME's variable bitrate quality, 0 (best) to 9; −1 for a constant bitrate. */
    int vbrQuality = -1;

    /// The ID3v2 tag.
    Mp3Tags tags;
};

/**
    The sample rate an MP3 of a render at `renderRate` is written at (X6): the render's own up to 48 kHz, otherwise
    44.1 or 48 kHz, whichever it is a multiple of (48 kHz for any other rate).
*/
[[nodiscard]] double mp3SampleRate (double renderRate) noexcept;

/**
    Peak and integrated loudness of an audio file DAWg can read (an MP3 after encoding, X6).
*/
struct FileMeasurement
{
    /// The sample peak, linear.
    double peak = 0.0;
    /// The integrated loudness in LUFS.
    double lufs = 0.0;
};

/**
    Measures a written file's sample peak and loudness.

    @param file  A WAV, FLAC or MP3 file.
    @return      The measurement, or nothing if the file can't be read.
*/
[[nodiscard]] std::optional<FileMeasurement> measureFile (const juce::File& file);

/**
    Writes a rendered 32-bit float WAV file in the final format, with gain (normalisation) and, for 16-bit
    formats, TPDF dither.

    @param source  The rendered file (32-bit float WAV).
    @param target  The file to write. Replaced if it exists.
    @param format  The format.
    @param gain    Linear gain applied before conversion.
    @param mp3     How an MP3 is encoded and tagged; only for ExportFormat::mp3.
    @return        An error message, or empty on success.
*/
[[nodiscard]] juce::String writeExport (const juce::File& source, const juce::File& target, ExportFormat format,
                                        float gain, const Mp3Settings& mp3 = {});

} // namespace dawg
