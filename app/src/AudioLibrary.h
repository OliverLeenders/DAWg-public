#pragma once

#include "dawg/AudioPlayer.h"
#include "dawg/PitchAnalysis.h"
#include "dawg/PitchModel.h"
#include "dawg/SampleMapping.h"
#include "dawg/StretchRenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace dawg
{

/**
    The app's audio file services (docs/design/phase7-audio-tracks.md): opening pool files for playback,
    importing files into the project's Audio folder, waveform peaks, and keeping project audio together when a
    project is saved somewhere new.

    @note  Message thread, except the background jobs it runs itself.
*/
class AudioLibrary final
{
public:
    /**
        A file copied into the project, ready for an addAudioClip command.
    */
    struct Imported
    {
        /**
            Path relative to the project folder ("Audio/<name>.wav").
        */
        juce::String path;

        /**
            Display name.
        */
        juce::String name;

        /**
            The file it was copied from, as given.
        */
        juce::String source;

        /**
            Sample rate in Hz.
        */
        double sampleRate = 0.0;

        /**
            Channel count.
        */
        int channels = 0;

        /**
            Length in frames.
        */
        juce::int64 frames = 0;

        /**
            The tempo in the file's metadata (a WAV acid chunk, or Apple Loops' beats in an AIFF file), or 0
            (docs/design/time-stretching.md, TS11).
        */
        double metadataBpm = 0.0;

        /**
            Its sounds, cut into slots and with their pitches, when the import was asked to analyse it
            (docs/design/sampler-mapping.md, SM2); empty otherwise.
        */
        std::vector<SampleSlice> slices;

        /**
            The root key the file stores (a WAV file's smpl chunk), or -1 (docs/design/sampler-mapping.md, SM5a).
        */
        int storedKey = -1;

        /**
            The title the file stores (a WAV file's INAM), or empty.
        */
        juce::String title;

        /**
            The first frame of the loop the file stores, or -1.
        */
        juce::int64 loopStart = -1;

        /**
            The frame after that loop's last, or -1.
        */
        juce::int64 loopEnd = -1;
    };

    /**
        Creates the library.

        @param projectFolder  Returns the open project's folder (where "Audio/" lives).
        @param cacheFolder    Where peak files are cached.
        @param peaksReady     Called on the message thread when peaks for a file finish (with its absolute path).
    */
    AudioLibrary (std::function<juce::File()> projectFolder, juce::File cacheFolder,
                  std::function<void (const juce::String&)> peaksReady);

    /**
        Waits for background jobs.
    */
    ~AudioLibrary();

    AudioLibrary (const AudioLibrary&) = delete;
    AudioLibrary& operator= (const AudioLibrary&) = delete;
    AudioLibrary (AudioLibrary&&) = delete;
    AudioLibrary& operator= (AudioLibrary&&) = delete;

    /**
        Returns the audio formats DAWg reads (WAV, AIFF, FLAC, Ogg Vorbis, MP3).
    */
    [[nodiscard]] juce::AudioFormatManager& formats() noexcept { return m_formats; }

    /**
        Returns the absolute location of a pool file.

        @param file  The pool entry.
        @return      The file (it may not exist).
    */
    [[nodiscard]] juce::File locate (const AudioFile& file) const;

    /**
        Returns the resolver for the graph compiler: opens pool files, sharing open sources by path.
    */
    [[nodiscard]] AudioSourceResolver resolver();

    /**
        Returns what reads source audio for stretch renders (docs/design/time-stretching.md, TS7): any thread, from
        files whose paths identify() made absolute.
    */
    [[nodiscard]] StretchRenderer::Input stretchInput();

    /**
        Makes a pool file's path absolute and returns what identifies its content: path, size and modification
        time, as for peaks (TS8).

        @param file  The pool entry; its path becomes absolute.
        @return      The identity.
    */
    [[nodiscard]] juce::String identify (AudioFile& file) const;

    /**
        Receives the imported files and the names of the files that could not be imported.
    */
    using ImportDone = std::function<void (std::vector<Imported>, juce::StringArray)>;

    /**
        Imports files into the project's Audio folder on a background thread. WAV, AIFF and FLAC are copied;
        other formats are decoded to WAV. Unreadable files are skipped.

        @param files    The files to import.
        @param done     Called on the message thread with what was imported and the files that failed.
        @param analyse  Whether to cut each file into its sounds and find their pitches, for mapping them in a Sampler.
    */
    void importFiles (std::vector<juce::File> files, ImportDone done, bool analyse = false);

    /**
        Receives the reversed copies, by the id of the pool file each reverses, and the names of the files that
        failed.
    */
    using ReverseDone = std::function<void (std::map<ObjectId, Imported>, juce::StringArray)>;

    /**
        Writes reversed copies of files into the project's Audio folder on a background thread, as
        "<name> (reversed).wav" (docs/design/audio-features.md, AU1).

        @param sources  The pool files to reverse.
        @param done     Called on the message thread.
    */
    void reverseFiles (const std::vector<AudioFile>& sources, ReverseDone done);

    /**
        What a peak measurement reads: a file and the frame ranges in it.
    */
    struct PeakRequest
    {
        /**
            The file.
        */
        juce::File file;

        /**
            Sorted, disjoint frame ranges.
        */
        std::vector<std::pair<juce::int64, juce::int64>> ranges;
    };

    /**
        Finds the sample peak (linear, over all channels) of each request on a background thread (AU2). Unreadable
        files give 0.

        @param requests  What to measure.
        @param done      Called on the message thread with one peak per request.
    */
    void peaks (std::vector<PeakRequest> requests, std::function<void (std::vector<float>)> done);

    /**
        Reads frames of a file and runs an analysis of them on a background thread (docs/design/audio-features.md,
        AU4).

        @param file   The audio file.
        @param start  The first frame.
        @param count  How many frames.
        @param work   Runs on the background thread with the audio and its sample rate.
        @param done   Called on the message thread with what work returned (void if the file couldn't be read).
    */
    void analyse (const juce::File& file, juce::int64 start, juce::int64 count,
                  std::function<juce::var (const juce::AudioBuffer<float>&, double)> work,
                  std::function<void (juce::var)> done);

    /**
        Returns cached peaks for a file, or starts computing them (peaksReady is called when done).

        @param file   The audio file.
        @param level  Mip level: 0 = 256 frames per point, each level 4x coarser.
        @return       { ready, samplesPerPoint, channels, points, frames, sampleRate, data } where data is base64
                      int8 pairs (min, max) per point and channel, interleaved by channel; or { ready: false }.
    */
    [[nodiscard]] juce::var peaks (const juce::File& file, int level);

    /**
        Returns a file's onsets (docs/design/time-stretching.md, TS11), cached next to the peaks, or starts finding
        them (onsetsReady is called when done).

        @param file  The audio file.
        @return      { ready: true, onsets: [[frame, strength], ...] }, or { ready: false }.
    */
    [[nodiscard]] juce::var onsets (const juce::File& file);

    /**
        Sets what is called on the message thread when a file's onsets are found (with its absolute path).
    */
    void setOnsetsReady (std::function<void (const juce::String&)> ready) { m_onsetsReady = std::move (ready); }

    /**
        Returns a file's pitch analysis (docs/design/pitch-correction.md, section 3.6), cached by file identity and
        analyser, or starts it (pitchReady is called when done).

        @param file  The audio file.
        @return      { ready: true, analyser, sampleRate, start, hop, pitch, notes } as pitchAnalysisToVar gives it,
                     { ready: true, error } if the model could not run, or { ready: false }.
    */
    [[nodiscard]] juce::var pitchAnalysis (const juce::File& file);

    /**
        Sets what is called on the message thread when a file's pitch analysis is done (with its absolute path).
    */
    void setPitchReady (std::function<void (const juce::String&)> ready) { m_pitchReady = std::move (ready); }

    /**
        Returns a file's pitch track from the cache, at full resolution, for pitch renders (any thread).

        @param file  The audio file.
        @return      The track, or null if the file hasn't been analysed.
    */
    [[nodiscard]] std::shared_ptr<const PitchTrack> pitchTrack (const juce::File& file);

    /**
        Reads a range of a file's frames for drawing the wave when zoomed in past the peaks: the mono mid
        ((left + right) / 2) as 32-bit floats.

        @param file   The audio file.
        @param start  The first frame.
        @param count  How many frames (at most 65536).
        @return       { start, count, data } with data base64 little-endian floats, or void if unreadable.
    */
    [[nodiscard]] juce::var samples (const juce::File& file, juce::int64 start, int count);

    /**
        The pitch of the sound in part of a file, for a zone's Detect pitch (docs/design/sampler-mapping.md, SM4).

        @param file   The file.
        @param start  The sound's first frame.
        @param end    The frame after its last, or 0 for the file's end.
        @return       Its pitch in MIDI semitones (fractional), or 0 if none was found.
    */
    [[nodiscard]] double pitchOf (const juce::File& file, juce::int64 start, juce::int64 end);

    /**
        Opens a file for previewing (not a pool file; sources are not shared).

        @param file  The file.
        @return      The source, or nullptr if it can't be read.
    */
    [[nodiscard]] std::shared_ptr<const AudioSource> openForPreview (const juce::File& file);

    /**
        Reads the durations of audio files in the background (headers only).

        @param files  The files.
        @param done   Called on the message thread with { path: seconds } for the readable ones.
    */
    void readDurations (std::vector<juce::File> files, std::function<void (juce::var)> done);

    /**
        Returns true if DAWg can read files with this name's extension.

        @param file  The file.
    */
    [[nodiscard]] static bool isAudioFile (const juce::File& file);

    /**
        Copies the project's audio files from one project folder to another (first save, Save as).

        @param project  The project.
        @param from     The folder the pool paths resolve against now.
        @param to       The new project folder.
    */
    void copyProjectAudio (const Project& project, const juce::File& from, const juce::File& to);

    /**
        Returns the pool files that can't be found.

        @param project  The project.
        @return         The missing entries.
    */
    [[nodiscard]] std::vector<AudioFile> missing (const Project& project) const;

private:
    struct Peaks
    {
        int channels = 0;
        juce::int64 frames = 0;
        double sampleRate = 0.0;
        std::vector<std::vector<juce::int8>> levels; // per level: points * channels * 2
    };

    [[nodiscard]] std::shared_ptr<const Peaks> computePeaks (const juce::File& file);
    [[nodiscard]] std::optional<Imported> importOne (const juce::File& source, const juce::File& audioFolder,
                                                     bool analyse);
    [[nodiscard]] juce::File peakCacheFile (const juce::File& file) const;
    [[nodiscard]] juce::var computeOnsets (const juce::File& file);
    [[nodiscard]] juce::var computePitch (const juce::File& file, const std::weak_ptr<bool>& alive);
    [[nodiscard]] juce::File pitchCacheFile (const juce::File& file, const char* extension) const;

    std::function<juce::File()> m_projectFolder;
    juce::File m_cacheFolder;
    std::function<void (const juce::String&)> m_peaksReady;
    juce::AudioFormatManager m_formats;
    juce::TimeSliceThread m_readAhead { "DAWg audio read-ahead" };
    juce::ThreadPool m_jobs { juce::ThreadPoolOptions {}.withNumberOfThreads (2).withThreadName ("DAWg audio jobs") };
    std::map<juce::String, std::weak_ptr<const AudioSource>> m_sources;
    std::map<juce::String, std::shared_ptr<const Peaks>> m_peaks;
    std::map<juce::String, bool> m_peaksPending;
    std::function<void (const juce::String&)> m_onsetsReady;
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> m_sampleReaders; // samples(), message thread
    std::map<juce::String, juce::var> m_onsets;
    std::map<juce::String, bool> m_onsetsPending;
    std::function<void (const juce::String&)> m_pitchReady;
    std::map<juce::String, juce::var> m_pitch;
    std::map<juce::String, bool> m_pitchPending;
    std::mutex m_pitchModelLock; // the model is loaded once, on the first job that needs it
    std::unique_ptr<PitchModel> m_pitchModel;
    juce::String m_pitchModelError;
    std::mutex m_tracksLock;
    std::map<juce::String, std::shared_ptr<const PitchTrack>> m_tracks; // by cache file
    std::shared_ptr<bool> m_alive = std::make_shared<bool> (true);
};

} // namespace dawg
