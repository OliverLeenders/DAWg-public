#include "dawg/Export.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <lame.h>
#include <limits>
#include <random>

namespace dawg
{

namespace
{
constexpr double absoluteGateLufs = -70.0;
constexpr double relativeGateLu = -10.0;
constexpr double loudnessOffset = -0.691;
constexpr double silenceThreshold = 3.1622776e-5; // −90 dBFS
constexpr double quietSecondsToEnd = 1.0;

[[nodiscard]] double energyToLufs (double energy) noexcept
{
    return energy > 0.0 ? loudnessOffset + 10.0 * std::log10 (energy) : -std::numeric_limits<double>::infinity();
}
} // namespace

// ---------------------------------------------------------------------------------------------------------
// LoudnessMeter

double LoudnessMeter::Biquad::process (double x, size_t ch) noexcept
{
    const auto y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
}

void LoudnessMeter::prepare (double sampleRate)
{
    m_sampleRate = sampleRate;

    // K-weighting at any sample rate (the BS.1770 pre-filter and RLB high-pass, as in libebur128).
    {
        constexpr double f0 = 1681.974450955533;
        constexpr double gainDb = 3.999843853973347;
        constexpr double q = 0.7071752369554196;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const auto vh = std::pow (10.0, gainDb / 20.0);
        const auto vb = std::pow (vh, 0.4996667741545416);
        const auto a0 = 1.0 + k / q + k * k;
        m_shelf = {};
        m_shelf.b0 = (vh + vb * k / q + k * k) / a0;
        m_shelf.b1 = 2.0 * (k * k - vh) / a0;
        m_shelf.b2 = (vh - vb * k / q + k * k) / a0;
        m_shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        m_shelf.a2 = (1.0 - k / q + k * k) / a0;
    }

    {
        constexpr double f0 = 38.13547087602444;
        constexpr double q = 0.5003270373238773;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const auto a0 = 1.0 + k / q + k * k;
        m_highPass = {};
        m_highPass.b0 = 1.0;
        m_highPass.b1 = -2.0;
        m_highPass.b2 = 1.0;
        m_highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        m_highPass.a2 = (1.0 - k / q + k * k) / a0;
    }

    m_blockStep = std::max (1, static_cast<int> (std::lround (sampleRate * 0.1)));
    m_stepEnergy = 0.0;
    m_stepCount = 0;
    m_steps = {};
    m_stepsSeen = 0;
    m_blocks.clear();
    m_peak = 0.0f;
}

void LoudnessMeter::process (std::array<const float*, 2> channels, int numSamples)
{
    for (int s = 0; s < numSamples; ++s)
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto x = channels[ch][s];
            m_peak = std::max (m_peak, std::abs (x));
            const auto weighted = m_highPass.process (m_shelf.process (static_cast<double> (x), ch), ch);
            m_stepEnergy += weighted * weighted;
        }

        if (++m_stepCount < m_blockStep)
            continue;

        // A 100 ms step is complete: each 400 ms block is the last four steps.
        std::rotate (m_steps.begin(), m_steps.begin() + 1, m_steps.end());
        m_steps.back() = m_stepEnergy;
        m_stepEnergy = 0.0;
        m_stepCount = 0;

        if (++m_stepsSeen >= static_cast<int> (m_steps.size()))
        {
            const auto sum = m_steps[0] + m_steps[1] + m_steps[2] + m_steps[3];
            m_blocks.push_back (sum / (4.0 * m_blockStep));
        }
    }
}

double LoudnessMeter::integratedLufs() const
{
    const auto meanOf = [this] (double threshold)
    {
        double sum = 0.0;
        int count = 0;

        for (const auto energy : m_blocks)
        {
            if (energyToLufs (energy) > threshold)
            {
                sum += energy;
                ++count;
            }
        }

        return count > 0 ? sum / count : 0.0;
    };

    const auto ungated = meanOf (absoluteGateLufs);

    if (ungated <= 0.0)
        return -std::numeric_limits<double>::infinity();

    return energyToLufs (meanOf (std::max (absoluteGateLufs, energyToLufs (ungated) + relativeGateLu)));
}

// ---------------------------------------------------------------------------------------------------------
// Rendering

juce::int64 renderRange (AudioEngine& engine, const ExportRange& range, double sampleRate, int blockSize,
                         const std::function<bool (std::array<const float*, 2>, int)>& sink,
                         const std::function<bool (std::span<const float* const>, int)>& tapSink)
{
    // Left and right, then two channels per tap.
    const auto taps = tapSink != nullptr ? std::min (range.taps.size(), AudioEngine::maxExportTaps) : size_t { 0 };
    std::vector<std::vector<float>> buffers (2 + 2 * taps, std::vector<float> (static_cast<size_t> (blockSize)));
    std::vector<float*> outputs;

    for (auto& buffer : buffers)
        outputs.push_back (buffer.data());

    const auto& left = buffers[0];
    const auto& right = buffers[1];
    const std::vector<const float*> tapChannels (outputs.begin() + 2, outputs.end());
    const auto numChannels = static_cast<int> (outputs.size());

    engine.setExportMode (true, range.tracks, range.preFader, std::span (range.taps.data(), taps));
    engine.seek (range.startBeats);
    engine.play();

    juce::int64 frames = 0;
    juce::int64 tailFrames = 0;
    juce::int64 quietFrames = 0;
    const auto maxTail = static_cast<juce::int64> (range.tailSeconds * sampleRate);
    const auto quietToEnd = static_cast<juce::int64> (quietSecondsToEnd * sampleRate);
    auto cancelled = false;

    for (;;)
    {
        // The first block starts the transport (requests are taken at the start of a block).
        const auto inRange = frames == 0 || engine.positionBeats() < range.endBeats;
        engine.renderOffline (outputs.data(), numChannels, blockSize, sampleRate);

        if (! sink ({ left.data(), right.data() }, blockSize) || (taps > 0 && ! tapSink (tapChannels, blockSize)))
        {
            cancelled = true;
            break;
        }

        frames += blockSize;

        if (inRange)
            continue;

        // The tail: fixed, or until the output stays quiet for a second.
        tailFrames += blockSize;
        auto peak = 0.0f;

        for (const auto& buffer : buffers)
            peak = std::max ({ peak, juce::FloatVectorOperations::findMaximum (buffer.data(), blockSize),
                               -juce::FloatVectorOperations::findMinimum (buffer.data(), blockSize) });

        quietFrames = peak < silenceThreshold ? quietFrames + blockSize : 0;

        if (tailFrames >= maxTail || (range.autoTail && quietFrames >= quietToEnd))
            break;
    }

    engine.stop();
    engine.renderOffline (outputs.data(), numChannels, blockSize, sampleRate); // let the stop happen
    engine.setExportMode (false);
    return cancelled ? -1 : frames;
}

// ---------------------------------------------------------------------------------------------------------
// MP3 (X6)

namespace
{
constexpr std::array<double, 9> mp3Rates { 8000.0,  11025.0, 12000.0, 16000.0, 22050.0,
                                           24000.0, 32000.0, 44100.0, 48000.0 };
constexpr double decimationPassband = 20000.0;
constexpr double decimationStopbandDb = 80.0;

/** The modified Bessel function of the first kind, order 0, for the Kaiser window (std::cyl_bessel_i isn't in every
    standard library). */
[[nodiscard]] double besselI0 (double x) noexcept
{
    auto sum = 1.0;
    auto term = 1.0;

    for (int k = 1; k < 64 && term > 1.0e-12 * sum; ++k)
    {
        const auto half = x / (2.0 * k);
        term *= half * half;
        sum += term;
    }

    return sum;
}

/** A linear-phase low-pass that keeps every `factor`th sample: a render above 48 kHz on its way to an MP3. The filter
    is centred on each output sample, so the output starts where the input does and nothing is shifted. */
class Decimator final
{
public:
    Decimator (int factor, double inputRate)
        : m_factor (factor)
    {
        const auto stop = inputRate / factor / 2.0;
        const auto pass = std::min (decimationPassband, stop * 0.9);
        const auto transition = (stop - pass) / inputRate;
        auto taps = static_cast<int> (std::ceil ((decimationStopbandDb - 7.95) / (14.36 * transition))) + 1;
        taps += 1 - (taps % 2); // odd, so the filter has a centre tap
        m_half = taps / 2;

        const auto cutoff = (pass + stop) / 2.0 / inputRate;
        const auto beta = 0.1102 * (decimationStopbandDb - 8.7);
        const auto norm = besselI0 (beta);
        std::vector<double> kernel (static_cast<size_t> (taps));
        auto sum = 0.0;

        for (int k = 0; k < taps; ++k)
        {
            const auto n = static_cast<double> (k - m_half);
            const auto sinc = k == m_half ? 2.0 * cutoff
                                          : std::sin (2.0 * juce::MathConstants<double>::pi * cutoff * n)
                                                / (juce::MathConstants<double>::pi * n);
            const auto ratio = n / m_half;
            kernel[static_cast<size_t> (k)] =
                sinc * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - ratio * ratio))) / norm;
            sum += kernel[static_cast<size_t> (k)];
        }

        for (const auto value : kernel)
            m_kernel.push_back (static_cast<float> (value / sum));
    }

    /** Takes input samples and appends the output samples they complete. */
    void push (const float* left, const float* right, int count, std::array<std::vector<float>, 2>& out)
    {
        m_input[0].insert (m_input[0].end(), left, left + count);
        m_input[1].insert (m_input[1].end(), right, right + count);
        m_length += count;
        produce (false, out);
    }

    /** Appends the last output samples, the input after the end being silence. */
    void finish (std::array<std::vector<float>, 2>& out) { produce (true, out); }

private:
    void produce (bool finishing, std::array<std::vector<float>, 2>& out)
    {
        const auto available = m_base + static_cast<juce::int64> (m_input[0].size());
        const auto taps = static_cast<juce::int64> (m_kernel.size());

        for (;; ++m_next)
        {
            const auto centre = m_next * m_factor;

            if (finishing ? centre >= m_length : centre + m_half >= available)
                break;

            // Input index centre + m_half − k for tap k, limited to what is held (outside it is silence).
            const auto first = std::max<juce::int64> (0, centre + m_half - (available - 1));
            const auto last = std::min<juce::int64> (taps - 1, centre + m_half - m_base);

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto value = 0.0f;

                for (auto k = first; k <= last; ++k)
                    value += m_kernel[static_cast<size_t> (k)]
                             * m_input[ch][static_cast<size_t> (centre + m_half - k - m_base)];

                out[ch].push_back (value);
            }
        }

        // Keep what the next output sample reaches back to.
        const auto keepFrom = std::clamp (m_next * m_factor - m_half, m_base, available);
        const auto drop = static_cast<std::ptrdiff_t> (keepFrom - m_base);

        for (auto& channel : m_input)
            channel.erase (channel.begin(), channel.begin() + drop);

        m_base = keepFrom;
    }

    int m_factor;
    int m_half = 0;
    std::vector<float> m_kernel;
    std::array<std::vector<float>, 2> m_input;
    juce::int64 m_base = 0;   // the input index of m_input's first sample
    juce::int64 m_length = 0; // input samples pushed
    juce::int64 m_next = 0;   // the next output sample
};

/** UTF-16 with a byte order mark and a terminating zero, as LAME's ID3v2 functions take text. */
[[nodiscard]] std::vector<unsigned short> utf16 (const juce::String& text)
{
    std::vector<unsigned short> result { 0xFEFF };

    for (auto p = text.toUTF16(); ! p.isEmpty(); ++p)
    {
        const auto c = *p;

        if (c >= 0x10000)
        {
            const auto v = static_cast<unsigned> (c) - 0x10000u;
            result.push_back (static_cast<unsigned short> (0xD800u + (v >> 10)));
            result.push_back (static_cast<unsigned short> (0xDC00u + (v & 0x3FFu)));
        }
        else
        {
            result.push_back (static_cast<unsigned short> (c));
        }
    }

    result.push_back (0);
    return result;
}

void setTag (lame_t lame, const char* frame, const juce::String& value)
{
    if (value.trim().isNotEmpty())
        std::ignore = id3tag_set_textinfo_utf16 (lame, frame, utf16 (value.trim()).data());
}

/** Owns a LAME encoder. */
struct LameEncoder
{
    LameEncoder() = default;
    LameEncoder (const LameEncoder&) = delete;
    LameEncoder& operator= (const LameEncoder&) = delete;
    LameEncoder (LameEncoder&&) = delete;
    LameEncoder& operator= (LameEncoder&&) = delete;

    ~LameEncoder()
    {
        if (lame != nullptr)
            lame_close (lame);
    }

    lame_t lame = lame_init();
};

[[nodiscard]] juce::String writeMp3 (juce::AudioFormatReader& reader, const juce::File& target, float gain,
                                     const Mp3Settings& settings)
{
    const auto outputRate = mp3SampleRate (reader.sampleRate);
    const auto ratio = reader.sampleRate / outputRate;
    const auto factor = std::abs (ratio - std::round (ratio)) < 1.0e-9 ? static_cast<int> (std::round (ratio)) : 1;
    const auto encoderInputRate = reader.sampleRate / factor;

    LameEncoder encoder;
    auto* lame = encoder.lame;

    if (lame == nullptr)
        return "The MP3 encoder could not start";

    lame_set_num_channels (lame, 2);
    lame_set_in_samplerate (lame, static_cast<int> (std::lround (encoderInputRate)));
    lame_set_out_samplerate (lame, static_cast<int> (std::lround (outputRate))); // LAME converts an odd rate itself
    lame_set_mode (lame, JOINT_STEREO);
    lame_set_quality (lame, 2);
    lame_set_bWriteVbrTag (lame, 1); // the Xing/LAME frame: length, encoder delay and padding for gapless players
    lame_set_write_id3tag_automatic (lame, 0);

    if (settings.vbrQuality >= 0)
    {
        lame_set_VBR (lame, vbr_default);
        lame_set_VBR_quality (lame, static_cast<float> (std::clamp (settings.vbrQuality, 0, 9)));
    }
    else
    {
        lame_set_VBR (lame, vbr_off);
        lame_set_brate (lame, settings.bitrateKbps);
    }

    id3tag_init (lame);
    id3tag_v2_only (lame);
    const auto& tags = settings.tags;
    setTag (lame, "TIT2", tags.title.trim().isNotEmpty() ? tags.title : target.getFileNameWithoutExtension());
    setTag (lame, "TPE1", tags.artist);
    setTag (lame, "TALB", tags.album);
    setTag (lame, "TYER", tags.year);
    setTag (lame, "TRCK", tags.track);
    setTag (lame, "TCON", tags.genre);
    setTag (lame, "TSSE", "DAWg");

    if (tags.comment.trim().isNotEmpty())
    {
        const auto description = utf16 ({});
        std::ignore = id3tag_set_comment_utf16 (lame, "eng", description.data(), utf16 (tags.comment.trim()).data());
    }

    if (lame_init_params (lame) < 0)
        return "The MP3 encoder doesn't take these settings";

    std::ignore = target.deleteFile();
    juce::FileOutputStream stream (target);

    // A FileOutputStream appends: should the old file still be there, it is emptied rather than added to.
    if (stream.failedToOpen() || ! stream.setPosition (0) || stream.truncate().failed())
        return "Cannot write " + target.getFullPathName();

    std::vector<unsigned char> tag (lame_get_id3v2_tag (lame, nullptr, 0));

    if (! tag.empty())
    {
        std::ignore = lame_get_id3v2_tag (lame, tag.data(), tag.size());
        std::ignore = stream.write (tag.data(), tag.size());
    }

    const auto lametagPosition = stream.getPosition();
    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (2, chunk);
    std::array<std::vector<float>, 2> decimated;
    std::optional<Decimator> decimator;

    if (factor > 1)
        decimator.emplace (factor, reader.sampleRate);

    std::vector<unsigned char> encoded (static_cast<size_t> (chunk + chunk / 4 + 7200));

    const auto encode = [&] (const float* left, const float* right, int count)
    {
        const auto bytes =
            lame_encode_buffer_ieee_float (lame, left, right, count, encoded.data(), static_cast<int> (encoded.size()));
        return bytes >= 0 && stream.write (encoded.data(), static_cast<size_t> (bytes));
    };

    const auto encodeDecimated = [&]
    {
        const auto ok = decimated[0].empty()
                        || encode (decimated[0].data(), decimated[1].data(), static_cast<int> (decimated[0].size()));
        decimated[0].clear();
        decimated[1].clear();
        return ok;
    };

    const auto failed = "Writing " + target.getFileName() + " failed";

    for (juce::int64 position = 0; position < reader.lengthInSamples; position += chunk)
    {
        const auto count = static_cast<int> (std::min<juce::int64> (chunk, reader.lengthInSamples - position));

        if (! reader.read (&buffer, 0, count, position, true, true))
            return "Could not read the rendered audio";

        buffer.applyGain (0, count, gain);
        auto ok = true;

        if (decimator.has_value())
        {
            decimator->push (buffer.getReadPointer (0), buffer.getReadPointer (1), count, decimated);
            ok = encodeDecimated();
        }
        else
        {
            ok = encode (buffer.getReadPointer (0), buffer.getReadPointer (1), count);
        }

        if (! ok)
            return failed;
    }

    if (decimator.has_value())
    {
        decimator->finish (decimated);

        if (! encodeDecimated())
            return failed;
    }

    const auto last = lame_encode_flush (lame, encoded.data(), static_cast<int> (encoded.size()));

    if (last < 0 || ! stream.write (encoded.data(), static_cast<size_t> (last)))
        return failed;

    // The Xing/LAME frame LAME left room for at the start, now that the length is known.
    std::array<unsigned char, 2880> lametag {};

    if (const auto size = lame_get_lametag_frame (lame, lametag.data(), lametag.size());
        size > 0 && size <= lametag.size())
        if (! stream.setPosition (lametagPosition) || ! stream.write (lametag.data(), size))
            return failed;

    stream.flush();
    return stream.getStatus().wasOk() ? juce::String() : failed;
}
} // namespace

double mp3SampleRate (double renderRate) noexcept
{
    if (std::find (mp3Rates.begin(), mp3Rates.end(), renderRate) != mp3Rates.end())
        return renderRate;

    const auto ratio = renderRate / 11025.0;
    return std::abs (ratio - std::round (ratio)) < 1.0e-9 ? 44100.0 : 48000.0;
}

std::optional<FileMeasurement> measureFile (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return std::nullopt;

    LoudnessMeter meter;
    meter.prepare (reader->sampleRate);
    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (2, chunk);

    for (juce::int64 position = 0; position < reader->lengthInSamples; position += chunk)
    {
        const auto count = static_cast<int> (std::min<juce::int64> (chunk, reader->lengthInSamples - position));

        if (! reader->read (&buffer, 0, count, position, true, true))
            return std::nullopt;

        meter.process ({ buffer.getReadPointer (0), buffer.getReadPointer (1) }, count);
    }

    return FileMeasurement { static_cast<double> (meter.samplePeak()), meter.integratedLufs() };
}

// ---------------------------------------------------------------------------------------------------------
// Writing

juce::String writeExport (const juce::File& source, const juce::File& target, ExportFormat format, float gain,
                          const Mp3Settings& mp3)
{
    juce::WavAudioFormat wav;
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (source.createInputStream().release(), true));

    if (reader == nullptr)
        return "Could not read the rendered audio";

    if (format == ExportFormat::mp3)
        return writeMp3 (*reader, target, gain, mp3);

    auto bits = 24;

    if (format == ExportFormat::wav16 || format == ExportFormat::flac16)
        bits = 16;
    else if (format == ExportFormat::wav32)
        bits = 32;

    const auto isFlac = format == ExportFormat::flac16 || format == ExportFormat::flac24;

    std::ignore = target.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());

    if (stream == nullptr)
        return "Cannot write " + target.getFullPathName();

    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate (reader->sampleRate)
                             .withNumChannels (static_cast<int> (reader->numChannels))
                             .withBitsPerSample (bits)
                             .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                           : juce::AudioFormatWriterOptions::SampleFormat::integral);
    auto writer = isFlac ? flac.createWriterFor (stream, options) : wav.createWriterFor (stream, options);

    if (writer == nullptr)
        return "This format can't be written";

    // TPDF dither of ±1 LSB for 16-bit output.
    const auto dither = bits == 16;
    const auto lsb = 1.0f / 32768.0f;
    std::minstd_rand random (std::random_device {}());
    std::uniform_real_distribution<float> uniform (-0.5f, 0.5f);

    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), chunk);

    for (juce::int64 position = 0; position < reader->lengthInSamples; position += chunk)
    {
        const auto count = static_cast<int> (std::min<juce::int64> (chunk, reader->lengthInSamples - position));

        if (! reader->read (&buffer, 0, count, position, true, true))
            return "Could not read the rendered audio";

        buffer.applyGain (0, count, gain);

        if (dither)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int s = 0; s < count; ++s)
                    buffer.setSample (ch, s, buffer.getSample (ch, s) + (uniform (random) + uniform (random)) * lsb);

        if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
            return "Writing " + target.getFileName() + " failed";
    }

    return {};
}

} // namespace dawg
