#include "dawg/AudioSource.h"

#include <algorithm>

namespace dawg
{

std::shared_ptr<AudioSource> AudioSource::open (const juce::File& file, juce::AudioFormatManager& formats,
                                                juce::TimeSliceThread& readAhead)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels < 1)
        return nullptr;

    std::shared_ptr<AudioSource> source (
        new AudioSource()); // NOLINT(cppcoreguidelines-owning-memory): private constructor
    source->m_sampleRate = reader->sampleRate;
    source->m_channels = static_cast<int> (reader->numChannels);
    source->m_frames = reader->lengthInSamples;

    if (static_cast<double> (reader->lengthInSamples) <= inMemorySeconds * reader->sampleRate)
    {
        source->m_memory.setSize (source->m_channels, static_cast<int> (reader->lengthInSamples));

        if (! reader->read (&source->m_memory, 0, static_cast<int> (reader->lengthInSamples), 0, true, true))
            return nullptr;
    }
    else
    {
        auto buffering = std::make_unique<juce::BufferingAudioReader> (reader.release(), readAhead, readAheadFrames);
        buffering->setReadTimeout (0); // never wait on the audio thread
        source->m_stream = std::move (buffering);
        source->m_direct.reset (formats.createReaderFor (file));
    }

    return source;
}

std::shared_ptr<AudioSource> AudioSource::fromBuffer (juce::AudioBuffer<float> buffer, double sampleRate)
{
    std::shared_ptr<AudioSource> source (
        new AudioSource()); // NOLINT(cppcoreguidelines-owning-memory): private constructor
    source->m_sampleRate = sampleRate;
    source->m_channels = buffer.getNumChannels();
    source->m_frames = buffer.getNumSamples();
    source->m_memory = std::move (buffer);
    return source;
}

AudioSource::~AudioSource() = default;

std::shared_ptr<AudioSource> AudioSource::withOverlays (std::shared_ptr<const AudioSource> base,
                                                        std::vector<Overlay> overlays)
{
    std::shared_ptr<AudioSource> source (new AudioSource());
    source->m_sampleRate = base->m_sampleRate;
    source->m_channels = base->m_channels;
    source->m_frames = base->m_frames;
    source->m_base = std::move (base);
    source->m_overlays = std::move (overlays);
    return source;
}

// The base's frames, then each overlay's over the frames it covers.
template <typename Read>
bool AudioSource::readOverlays (const std::array<float*, 2>& destination, juce::int64 start, int numFrames,
                                Read&& read) const
{
    auto ok = read (*m_base, destination, start, numFrames);

    for (const auto& overlay : m_overlays)
    {
        const auto first = std::max (start, overlay.start);
        const auto last = std::min (start + numFrames, overlay.end);

        if (first >= last || overlay.source == nullptr)
            continue;

        const auto offset = static_cast<size_t> (first - start);
        ok = read (*overlay.source, { destination[0] + offset, destination[1] + offset }, first - overlay.sourceStart,
                   static_cast<int> (last - first))
             && ok;
    }

    return ok;
}

bool AudioSource::read (const std::array<float*, 2>& destination, juce::int64 start, int numFrames) const noexcept
{
    if (m_base != nullptr)
        return readOverlays (destination, start, numFrames,
                             [] (const AudioSource& source, const std::array<float*, 2>& into, juce::int64 at,
                                 int count) { return source.read (into, at, count); });

    juce::FloatVectorOperations::clear (destination[0], numFrames);
    juce::FloatVectorOperations::clear (destination[1], numFrames);

    // Only the part inside the file is read; the rest stays silent.
    const auto first = std::max<juce::int64> (start, 0);
    const auto last = std::min<juce::int64> (start + numFrames, m_frames);

    if (first >= last)
        return true;

    const auto offset = static_cast<int> (first - start);
    const auto count = static_cast<int> (last - first);
    std::array<float*, 2> dest { destination[0] + offset, destination[1] + offset };

    if (m_stream != nullptr)
    {
        const auto ok = m_stream->read (dest.data(), std::min (2, m_channels), first, count);

        if (m_channels == 1)
            juce::FloatVectorOperations::copy (dest[1], dest[0], count);

        return ok;
    }

    const auto from = static_cast<int> (first);
    juce::FloatVectorOperations::copy (dest[0], m_memory.getReadPointer (0, from), count);
    juce::FloatVectorOperations::copy (dest[1], m_memory.getReadPointer (std::min (1, m_channels - 1), from), count);
    return true;
}

bool AudioSource::readBlocking (const std::array<float*, 2>& destination, juce::int64 start, int numFrames) const
{
    if (m_base != nullptr)
        return readOverlays (destination, start, numFrames,
                             [] (const AudioSource& source, const std::array<float*, 2>& into, juce::int64 at,
                                 int count) { return source.readBlocking (into, at, count); });

    if (m_stream == nullptr)
        return read (destination, start, numFrames);

    juce::FloatVectorOperations::clear (destination[0], numFrames);
    juce::FloatVectorOperations::clear (destination[1], numFrames);
    const auto first = std::max<juce::int64> (start, 0);
    const auto last = std::min<juce::int64> (start + numFrames, m_frames);

    if (first >= last || m_direct == nullptr)
        return first >= last;

    const auto offset = static_cast<int> (first - start);
    const auto count = static_cast<int> (last - first);
    std::array<float*, 2> dest { destination[0] + offset, destination[1] + offset };
    const std::scoped_lock lock (m_directLock);
    const auto ok = m_direct->read (dest.data(), std::min (2, m_channels), first, count);

    if (m_channels == 1)
        juce::FloatVectorOperations::copy (dest[1], dest[0], count);

    return ok;
}

juce::AudioBuffer<float> AudioSource::readAllChannels (juce::int64 start, int numFrames) const
{
    if (m_base != nullptr)
        return m_base->readAllChannels (start, numFrames);

    const auto first = std::clamp<juce::int64> (start, 0, m_frames);
    const auto count = static_cast<int> (std::clamp<juce::int64> (numFrames, 0, m_frames - first));
    juce::AudioBuffer<float> result (m_channels, count);

    if (count == 0)
        return result;

    if (m_stream == nullptr)
    {
        for (int ch = 0; ch < m_channels; ++ch)
            result.copyFrom (ch, 0, m_memory, ch, static_cast<int> (first), count);

        return result;
    }

    if (m_direct == nullptr)
        return {};

    const std::scoped_lock lock (m_directLock);

    if (! m_direct->read (&result, 0, count, first, true, true))
        return {};

    return result;
}

} // namespace dawg
