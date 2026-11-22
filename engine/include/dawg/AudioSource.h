#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <mutex>
#include <vector>

namespace dawg
{

/**
    An audio file prepared for reading on the audio thread (docs/design/phase7-audio-tracks.md, T6).

    Short files are decoded into memory; longer files are streamed through a juce::BufferingAudioReader on a
    shared read-ahead thread and read with a zero timeout, so the audio thread never waits for the disk.

    @note  open(): message thread (or any non-audio thread). read(): audio thread.
*/
class AudioSource final
{
public:
    /**
        Files up to this many seconds are held in memory.
    */
    static constexpr double inMemorySeconds = 30.0;

    /**
        Frames of read-ahead for streamed files (at the file's rate: about 2 seconds at 48 kHz).
    */
    static constexpr int readAheadFrames = 96000;

    /**
        Opens a file.

        @param file      The audio file.
        @param formats   Formats to decode with. Must outlive the source only during this call.
        @param readAhead The shared read-ahead thread for streamed files. Must outlive the source.
        @return          The source, or nullptr if the file can't be read.
    */
    [[nodiscard]] static std::shared_ptr<AudioSource> open (const juce::File& file, juce::AudioFormatManager& formats,
                                                            juce::TimeSliceThread& readAhead);

    /**
        Wraps decoded audio (tests, and recordings still being written).

        @param buffer      The audio.
        @param sampleRate  Its sample rate.
        @return            The source.
    */
    [[nodiscard]] static std::shared_ptr<AudioSource> fromBuffer (juce::AudioBuffer<float> buffer, double sampleRate);

    /**
        A stretch of another source that plays in place of a file's own frames (a pitch-corrected phrase,
        docs/design/pitch-correction.md, section 5.2).
    */
    struct Overlay
    {
        /**
            The file frames it covers, end exclusive.
        */
        juce::int64 start = 0;

        /**
            @copydoc start
        */
        juce::int64 end = 0;

        /**
            The file frame of the overlay's frame 0.
        */
        juce::int64 sourceStart = 0;

        /**
            What plays there.
        */
        std::shared_ptr<const AudioSource> source;
    };

    /**
        A file with stretches of it replaced: reads come from `base`, except inside an overlay. The overlays' own
        edges must match the file there (a render that changes nothing at its ends), as nothing is crossfaded.
        readAllChannels() reads the base only.

        @param base      The file.
        @param overlays  Sorted by start, not overlapping.
        @return          The source.
    */
    [[nodiscard]] static std::shared_ptr<AudioSource> withOverlays (std::shared_ptr<const AudioSource> base,
                                                                    std::vector<Overlay> overlays);

    ~AudioSource();

    AudioSource (const AudioSource&) = delete;
    AudioSource& operator= (const AudioSource&) = delete;
    AudioSource (AudioSource&&) = delete;
    AudioSource& operator= (AudioSource&&) = delete;

    /**
        Returns the file's sample rate in Hz.
    */
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }

    /**
        Returns the number of channels.
    */
    [[nodiscard]] int channels() const noexcept { return m_channels; }

    /**
        Returns the length in frames.
    */
    [[nodiscard]] juce::int64 frames() const noexcept { return m_frames; }

    /**
        Reads frames into two channels (mono files fill both). Frames outside the file read as silence.

        @param destination  Two channel pointers with room for numFrames each.
        @param start        The first frame to read (may be negative or past the end).
        @param numFrames    How many frames to read.
        @return             False if streamed data wasn't ready (a dropout: silence was written instead).
        @note  Audio thread. Never allocates or blocks.
    */
    [[nodiscard]] bool read (const std::array<float*, 2>& destination, juce::int64 start, int numFrames) const noexcept;

    /**
        Reads frames like read(), but waits for the disk where the file is streamed. For other threads than the audio
        thread (lining up joins between stretched pieces, docs/design/time-stretching.md, TS9).

        @param destination  Two channel pointers with room for numFrames each.
        @param start        The first frame to read (may be negative or past the end).
        @param numFrames    How many frames to read.
        @return             False if the file couldn't be read (silence was written instead).
    */
    [[nodiscard]] bool readBlocking (const std::array<float*, 2>& destination, juce::int64 start, int numFrames) const;

    /**
        Reads frames with every channel of the file, waiting for the disk where the file is streamed. For other threads
        than the audio thread (building a Convolution Reverb's impulse response, docs/design/convolution-reverb.md, I3).

        @param start      The first frame to read (from 0).
        @param numFrames  How many frames; fewer are read where the file ends.
        @return           The frames, one buffer channel per file channel; empty if the file couldn't be read.
    */
    [[nodiscard]] juce::AudioBuffer<float> readAllChannels (juce::int64 start, int numFrames) const;

private:
    AudioSource() = default;

    double m_sampleRate = 48000.0;
    int m_channels = 0;
    juce::int64 m_frames = 0;
    juce::AudioBuffer<float> m_memory;                 // in-memory files
    std::unique_ptr<juce::AudioFormatReader> m_stream; // streamed files (a BufferingAudioReader)
    std::unique_ptr<juce::AudioFormatReader> m_direct; // streamed files, for readBlocking()
    mutable std::mutex m_directLock;
    std::shared_ptr<const AudioSource> m_base; // withOverlays()
    std::vector<Overlay> m_overlays;

    template <typename Read>
    [[nodiscard]] bool readOverlays (const std::array<float*, 2>& destination, juce::int64 start, int numFrames,
                                     Read&& read) const;
};

} // namespace dawg
