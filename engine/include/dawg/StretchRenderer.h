#pragma once

#include "dawg/AudioSource.h"
#include "dawg/Model.h"
#include "dawg/PitchAnalysis.h"
#include "dawg/Stretcher.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace dawg
{

/**
    One render (docs/design/time-stretching.md, TS7): a stretch of a pool file's frames, in timeline time.
*/
struct StretchJob
{
    /**
        The pool file to read.
    */
    AudioFile file;

    /**
        The first input frame (may be negative: frames outside the file are silence).
    */
    juce::int64 inputStart = 0;

    /**
        How many input frames to stretch.
    */
    juce::int64 inputFrames = 0;

    /**
        The output's length, key frames (relative to inputStart and the output's start) and pitch.
    */
    StretchSpec spec;

    /**
        Where the render plays, in timeline seconds: renders nearest the playhead go first (TS9).
    */
    double seconds = 0.0;

    /**
        A pitch render instead of a stretch (docs/design/pitch-correction.md, section 5.2): the input range (a phrase
        and some padding) shifted by file.pitch's correction, as long as it is. spec is unused.
    */
    bool pitch = false;
};

/**
    A Sampler zone's render at another pitch, as long as its frames, its formants moving with the pitch as on tape
    (docs/design/sampler-mapping.md, PM2).
*/
[[nodiscard]] inline StretchJob samplerRenderJob (const AudioFile& file, juce::int64 start, juce::int64 frames,
                                                  double pitchScale)
{
    StretchJob job;
    job.file = file;
    job.inputStart = start;
    job.inputFrames = frames;
    job.spec.sampleRate = file.sampleRate;
    job.spec.outputFrames = frames;
    job.spec.pitchScale = pitchScale;
    job.spec.formants = false;
    return job;
}

/**
    Renders stretched audio offline on background threads and keeps the renders in a cache folder (TS7–TS9).

    A render is keyed by everything it depends on (TS8): the file's identity (path, size and modification time),
    the input range, the key frames, the output length, the pitch and the stretcher's identity. Undo therefore finds
    earlier renders still there. The folder is kept under a size limit by removing the least recently used renders.

    @note  find(), beginRequests() and endRequests() are for the message thread (the graph compiler). Workers call
           the ready callback on their own thread.
*/
class StretchRenderer final
{
public:
    /**
        Reads input frames: fills `into` from frame `start` of the file (silence outside it). Returns false if the
        file can't be read. Called on worker threads.
    */
    using Input = std::function<bool (const AudioFile& file, juce::int64 start, juce::AudioBuffer<float>& into)>;

    /**
        Returns what identifies a file's content (path, size and modification time), and may make the file's path
        absolute for Input. Called where jobs come in: find(), lookup(), keyOf() and renderNow().
    */
    using Identify = std::function<juce::String (AudioFile& file)>;

    /**
        Called on a worker thread when renders are ready, with the clips that asked for them.
    */
    using Ready = std::function<void (const std::vector<ObjectId>& clipIds)>;

    /**
        Returns a file's pitch track (docs/design/pitch-correction.md, section 3), or null if it hasn't been analysed.
        Pitch renders need it for their pitch marks and the vibrato. Called on any thread, with the file's path made
        absolute (see Identify).
    */
    using PitchTrackOf = std::function<std::shared_ptr<const PitchTrack> (const AudioFile& file)>;

    /**
        The default cache size limit: 10 GB.
    */
    static constexpr juce::int64 defaultSizeLimit = juce::int64 { 10 } * 1024 * 1024 * 1024;

    /**
        Creates the renderer and starts its workers.

        @param folder     The cache folder (created when needed).
        @param stretcher  The library.
        @param input      Reads the source audio.
        @param identify   Identifies file contents.
        @param threads    Worker threads (at least 1).
    */
    StretchRenderer (juce::File folder, std::shared_ptr<const Stretcher> stretcher, Input input, Identify identify,
                     int threads);

    /**
        Stops the workers (abandoning running renders).
    */
    ~StretchRenderer();

    StretchRenderer (const StretchRenderer&) = delete;
    StretchRenderer& operator= (const StretchRenderer&) = delete;
    StretchRenderer (StretchRenderer&&) = delete;
    StretchRenderer& operator= (StretchRenderer&&) = delete;

    /**
        Sets what is called when renders are ready.
    */
    void setReadyCallback (Ready ready);

    /**
        Sets where pitch renders find the files' pitch tracks. Without it, files play uncorrected.
    */
    void setPitchTracks (PitchTrackOf tracks);

    /**
        Returns the overlays that play a file's pitch corrections (docs/design/pitch-correction.md, section 5.2): one
        per phrase whose render is ready. The others are queued, like find(), and play uncorrected meanwhile.

        @param file     The pool file (with its pitch edits).
        @param clipId   The clip that wants them.
        @param pending  Increased by the number of renders still being made.
        @return         The overlays, in order.
    */
    [[nodiscard]] std::vector<AudioSource::Overlay> pitchOverlays (const AudioFile& file, ObjectId clipId,
                                                                   int& pending);

    /**
        Returns the pitch renders a project's edited files need, for export (they go before its stretch jobs).
    */
    [[nodiscard]] std::vector<StretchJob> pitchJobs (const Project& project);

    /**
        Returns the cache key of a job.
    */
    [[nodiscard]] juce::String keyOf (const StretchJob& job) const;

    /**
        Returns the cache key of a job, and makes its file's path what Input expects (see Identify).
    */
    [[nodiscard]] juce::String prepare (StretchJob& job) const;

    /**
        Returns a job's render if it is in the cache; otherwise queues it (once) and returns nullptr.

        @param request The render.
        @param clipId  The clip that wants it (told when it is ready).
        @param key     Receives the job's key, if not null.
        @return        The render, or nullptr while it is pending.
    */
    [[nodiscard]] std::shared_ptr<const AudioSource> find (const StretchJob& request, ObjectId clipId,
                                                           juce::String* key = nullptr);

    /**
        Returns a value cached under a key, computing it the first time (the alignment of joins between renders).

        @param key      The key.
        @param compute  Computes the value.
    */
    [[nodiscard]] double cached (const juce::String& key, const std::function<double()>& compute);

    /**
        Returns a job's render if it is in the cache, without queueing it.
    */
    [[nodiscard]] std::shared_ptr<const AudioSource> lookup (const StretchJob& job);

    /**
        Starts a round of requests (a compilation). Queued jobs that endRequests() finds unrequested are dropped, and
        running ones are cancelled. Renders wait quietMilliseconds after it before they start.
    */
    void beginRequests();

    /**
        Ends a round of requests: drops the queued jobs nobody asked for since beginRequests().
    */
    void endRequests();

    /**
        Renders jobs now on the calling thread (with the workers' help), for export (TS10).

        @param jobs      The renders.
        @param progress  Called with 0..1 as renders finish; returning false cancels.
        @return          False if cancelled or a render failed.
    */
    bool renderNow (const std::vector<StretchJob>& jobs, const std::function<bool (double)>& progress);

    /**
        Holds back new renders while the user drags something that changes them (a timing marker, the transpose),
        so that only the result of the whole drag is rendered (the preview plays meanwhile). A hold lapses by itself
        after maxHoldSeconds, should its release never come.

        @param held  True when a drag starts, false when it ends.
    */
    void setHeld (bool held);

    /**
        How long a hold lasts at most.
    */
    static constexpr int maxHoldSeconds = 20;

    /**
        After each round of requests, renders wait this long for the next one before they start: a string of quick
        edits (wheel nudges, typing) renders only its last state.
    */
    static constexpr int quietMilliseconds = 300;

    /**
        Sets where the playhead is, in timeline seconds: workers take the pending render nearest to it first.
    */
    void setPlayhead (double seconds);

    /**
        Sets the cache size limit in bytes, and trims the cache to it.
    */
    void setSizeLimit (juce::int64 bytes);

    /**
        Removes every render from the cache (renders in use stay until they are released).
    */
    void clear();

    /**
        Returns how many renders are queued or running.
    */
    [[nodiscard]] int pendingCount() const;

    /**
        Returns the clips waiting for renders.
    */
    [[nodiscard]] std::vector<ObjectId> pendingClips() const;

    /**
        Waits until no render is queued or running (tests), up to a timeout.

        @param milliseconds  The timeout.
        @return              False on timeout.
    */
    bool waitUntilIdle (int milliseconds);

    /**
        Returns how many renders were made (not found in the cache) since creation (tests).
    */
    [[nodiscard]] int renderedCount() const { return m_rendered.load(); }

    /**
        Returns the total size of the cache folder's renders in bytes.
    */
    [[nodiscard]] juce::int64 cacheSize() const;

private:
    struct Pending
    {
        StretchJob job;
        juce::String key;
        std::set<ObjectId> clips;
        bool requested = true;
        bool running = false;
    };

    void work();
    [[nodiscard]] bool renderOne (const StretchJob& job, const juce::String& key);
    [[nodiscard]] juce::AudioBuffer<float> renderPitch (const StretchJob& job, const juce::AudioBuffer<float>& input,
                                                        const std::function<bool()>& cancelled);
    [[nodiscard]] bool applyPitch (const StretchJob& job, juce::AudioBuffer<float>& input);
    [[nodiscard]] std::vector<StretchJob> pitchJobsOf (const AudioFile& file) const;
    [[nodiscard]] std::shared_ptr<const PitchTrack> trackOf (const AudioFile& file) const;
    [[nodiscard]] std::shared_ptr<const AudioSource> openCached (const juce::String& key);
    [[nodiscard]] juce::File fileFor (const juce::String& key) const;
    void trim();

    juce::File m_folder;
    std::shared_ptr<const Stretcher> m_stretcher;
    Input m_input;
    Identify m_identify;
    Ready m_ready;
    PitchTrackOf m_pitchTracks;

    juce::AudioFormatManager m_formats;
    juce::TimeSliceThread m_readAhead { "DAWg stretch read-ahead" };

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::vector<Pending> m_pending;
    std::map<juce::String, std::weak_ptr<const AudioSource>> m_open;
    std::map<juce::String, double> m_values;
    std::set<juce::String> m_cancelled; // running renders nobody wants any more
    std::chrono::steady_clock::time_point m_heldUntil;
    std::chrono::steady_clock::time_point m_quietUntil;
    double m_playhead = 0.0;
    juce::int64 m_sizeLimit = defaultSizeLimit;
    bool m_stopping = false;
    std::atomic<int> m_rendered { 0 };
    std::vector<std::unique_ptr<juce::Thread>> m_workers;
};

} // namespace dawg
