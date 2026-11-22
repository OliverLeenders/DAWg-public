#pragma once

#include "dawg/AudioSource.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Realtime.h"
#include "dawg/Time.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>

namespace dawg
{

/**
    Why the device output is muted (docs/design/output-protection.md, S3).
*/
enum class OutputMute
{
    none,     ///< Not muted.
    overload, ///< The level stayed above full scale for overloadSeconds.
    extreme,  ///< A sample went beyond extremeLevel, or was NaN or infinite.
};

/**
    Values the UI polls for meters and the playhead.
*/
struct Telemetry
{
    /**
        Whether the transport is running.
    */
    bool playing = false;

    /**
        Transport position in beats.
    */
    double positionBeats = 0.0;

    /**
        Highest absolute left-channel sample since the previous poll.
    */
    float peakLeft = 0.0f;

    /**
        Highest absolute right-channel sample since the previous poll.
    */
    float peakRight = 0.0f;

    /**
        Current device sample rate in Hz, or 0 when no device is running.
    */
    double sampleRate = 0.0;

    /**
        The engine's latency compensation in samples: the largest track latency plus the master chain's, and any
        advance a negative track offset needs (docs/PLAN.md, phase 13).
    */
    int latencySamples = 0;

    /**
        Whether recording is running.
    */
    bool recording = false;

    /**
        The preview's position in seconds, or a negative value when nothing is previewing.
    */
    double previewSeconds = -1.0;

    /**
        Highest absolute sample of the device input since the previous poll (all channels).
    */
    float inputPeak = 0.0f;

    /**
        Whether output protection muted the device output, and why (docs/design/output-protection.md, S3).
    */
    OutputMute outputMute = OutputMute::none;
};

/**
    A live note event from MIDI input or audition (docs/design/phase9-export-midi-input.md, M2, M3).
*/
struct LiveInput
{
    /**
        The track whose instrument plays it.
    */
    ObjectId trackId = 0;

    /**
        The live note (from AudioEngine::nextLiveNoteId(); negative). 0 for sustain and controllers.
    */
    ObjectId noteId = 0;

    /**
        noteOn, noteOff, expression, sustain or controller.
    */
    NoteEventType type = NoteEventType::noteOn;

    /**
        noteOn: base pitch in MIDI semitones.
    */
    double pitch = 60.0;

    /**
        noteOn: velocity; noteOff: release velocity (0..1).
    */
    float velocity = 0.8f;

    /**
        expression: the Lane as int; controller: the CC number, controllerPitchBend or controllerPressure.
    */
    int lane = 0;

    /**
        expression, sustain and controller: the value (see NoteEvent::value).
    */
    float value = 0.0f;

    /**
        noteOn: initial pitch offset (semitones), pressure and timbre, sounding from the first sample.
    */
    std::array<float, 3> initial { 0.0f, 0.0f, 0.5f };

    /**
        When it happened, in seconds of juce::Time::getMillisecondCounterHiRes() / 1000; 0 for as soon as possible.
        Timed events land at the matching sample of the next block (one block of latency, no jitter).
    */
    double time = 0.0;
};

/**
    A live event that happened while recording, with its timeline position (docs/design/phase9-export-midi-input.md,
    M5). Expression and controller values are the latest per block.
*/
struct RecordedInput
{
    /**
        The event as it was sent.
    */
    LiveInput input;

    /**
        Timeline position in ticks, corrected for the input latency (see AudioEngine::setInputLatency()).
    */
    double tick = 0.0;

    /**
        Which pass through the loop it was recorded in while loop recording, 0 for the first (or without a loop):
        each pass becomes a take of its own (docs/design/takes-lanes.md, T4).
    */
    int pass = 0;
};

/**
    Where one armed track records to (docs/design/phase7-audio-tracks.md, T9).
*/
struct RecordTarget
{
    /**
        The track.
    */
    ObjectId trackId = 0;

    /**
        First input channel.
    */
    int channel = 0;

    /**
        Whether the input is a stereo pair.
    */
    bool stereo = false;

    /**
        The writer the audio thread feeds (lock-free FIFO to a disk thread). Owned by the caller; must stay
        alive until stopRecording() has returned and one more audio block has run.
    */
    juce::AudioFormatWriter::ThreadedWriter* writer = nullptr;
};

/**
    Runs compiled engines on the audio thread: transport, track programs, mixing and the metronome.

    @note  setEngine(), play(), stop(), isPlaying(), collectGarbage() and getTelemetry(): message thread.
           The juce::AudioIODeviceCallback overrides and renderOffline(): audio thread (or a test thread
           standing in for it). The audio-thread side never locks or allocates.
*/
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    /**
        Creates the engine (and, on first use, its render workers, from setEngine()).
    */
    AudioEngine();

    /**
        Joins the render workers, if any. The device must have stopped calling the engine first.
    */
    ~AudioEngine() override;

    /**
        Publishes a compiled engine to the audio thread.

        @param engine  The engine to run from the next block on. Must not be null.
        @note  Message thread only.
    */
    void setEngine (std::shared_ptr<const CompiledEngine> engine);

    /**
        Frees engines the audio thread no longer uses.

        @note  Message thread only. Call regularly, e.g. from a timer.
    */
    void collectGarbage();

    /**
        Starts the transport from its current position at the next block.
    */
    void play() noexcept;

    /**
        Turns parallel rendering of tracks on or off: tracks render at the same time on a small worker pool, each
        once the tracks that fill its buses have, and every bus is summed in track order. Threads with no track to
        take help the others with their voices (Parallel). On by default; set the DAWG_PARALLEL_TRACKS environment
        variable to 0 before start-up to turn it off.

        @param enabled  Whether to use the pool.
        @note  Message thread only.
    */
    void setParallelTracks (bool enabled) noexcept;

    /**
        Returns true if parallel track rendering is enabled.
    */
    [[nodiscard]] bool parallelTracks() const noexcept { return m_parallelTracks.load(); }

    /**
        Stops the transport at the next block and returns the playhead to where playback started.
    */
    void stop() noexcept;

    /**
        Moves the playhead at the next block. Sounding notes are choked; while playing, notes overlapping the
        new position are chased.

        @param beats  The new position in beats (clamped to 0 or later).
    */
    void seek (double beats) noexcept;

    /**
        Returns true if the transport is running.
    */
    [[nodiscard]] bool isPlaying() const noexcept;

    /**
        Starts recording armed inputs into the given writers from a position on; the transport must be started
        separately (play()). With a count-in, start playback earlier than `fromBeats`.

        @param targets    Up to maxRecordTargets targets.
        @param fromBeats   Recording begins when the playhead reaches this position.
        @param untilBeats  Writing stops when it reaches this one (a punch-out), or never if negative.
        @note  Message thread.
    */
    void startRecording (std::span<const RecordTarget> targets, double fromBeats, double untilBeats = -1.0) noexcept;

    /**
        Stops recording. Writers may be finished (deleted) once one more audio block has run.

        @note  Message thread.
    */
    void stopRecording() noexcept;

    /**
        Returns true while recording.
    */
    [[nodiscard]] bool isRecording() const noexcept { return m_recording.load(); }

    /**
        Returns how many frames a target received.

        @param index  The target's index in the startRecording() call.
        @return       Frames written.
    */
    [[nodiscard]] juce::int64 recordedFrames (size_t index) const noexcept;

    /**
        Returns the position (beats) of the first recorded frame, or a negative value if nothing was recorded.
    */
    [[nodiscard]] double recordStartBeats() const noexcept { return m_recordStartBeats.load(); }

    /**
        Returns how often the transport loop wrapped while recording (docs/design/phase9-export-midi-input.md, M6).
    */
    [[nodiscard]] int recordLoopWraps() const noexcept { return m_recordLoopWraps.load(); }

    /**
        Returns the transport position in beats, as of the last block.
    */
    [[nodiscard]] double positionBeats() const noexcept { return m_positionForUi.load(); }

    /**
        Most tracks recorded at once.
    */
    static constexpr size_t maxRecordTargets = 32;

    /**
        Plays a file for the sound explorer, independent of the transport and the project, mixed after the
        master (docs/design/phase7-audio-tracks.md, T12). Replaces any running preview.

        @param source  The audio, or nullptr to stop.
        @note  Message thread.
    */
    void preview (std::shared_ptr<const AudioSource> source);

    /**
        Sets the preview volume.

        @param gain  Linear gain.
        @note  Any thread.
    */
    void setPreviewGain (float gain) noexcept { m_previewGain.store (gain); }

    /**
        Starts auditioning a note on a track, e.g. when a piano roll key is clicked.

        @param trackId   The track whose instrument plays the note.
        @param pitch     Pitch in MIDI semitones (may be fractional).
        @param velocity  Velocity, 0..1.
        @return          An id to pass to auditionNoteOff(), or 0 if the request queue was full.
        @note  Message thread only.
    */
    [[nodiscard]] ObjectId auditionNoteOn (ObjectId trackId, double pitch, float velocity) noexcept;

    /**
        Releases an auditioned note.

        @param trackId  The track the note was started on.
        @param noteId   The id auditionNoteOn() returned.
        @note  Message thread only.
    */
    void auditionNoteOff (ObjectId trackId, ObjectId noteId) noexcept;

    /**
        Returns a fresh id for a live note (negative, never clashing with model ids).

        @note  Any thread.
    */
    [[nodiscard]] ObjectId nextLiveNoteId() noexcept { return m_nextLiveId.fetch_sub (1); }

    /**
        Sends a live event from MIDI input (docs/design/phase9-export-midi-input.md, M3).

        @param input  The event, usually with a timestamp.
        @return       False if the queue was full (the event is dropped).
        @note  One producer at a time: callers on several MIDI threads must serialise their calls.
    */
    bool sendLiveInput (const LiveInput& input) noexcept;

    /**
        Takes the next event recorded while recording ran (live events with their timeline positions).

        @return  The event, or nothing when the queue is empty.
        @note  Message thread only.
    */
    [[nodiscard]] std::optional<RecordedInput> popRecordedInput() noexcept { return m_recordedInputs.pop(); }

    /**
        Sets how far recorded live events are moved back: the time between hearing the output and the event
        arriving, i.e. the device's output latency plus one block (docs/design/phase9-export-midi-input.md, M5).

        @param samples  The correction in samples.
        @note  Any thread.
    */
    void setInputLatency (int samples) noexcept { m_inputLatency.store (std::max (0, samples)); }

    /**
        Switches export rendering on or off (docs/design/phase9-export-midi-input.md, X1–X3). While exporting, the
        transport loop, the metronome and the sound explorer preview are left out; with stem tracks, only those
        tracks play (whatever their mute and solo), summed straight to the output without the master track or sends.

        @param exporting  Whether an export renders.
        @param tracks     The tracks to render (a group with its members), or none for the whole mix. At most
                          maxExportTracks.
        @param preFader   Whether the stem tracks are taken before their fader and pan (docs/design/audio-features.md,
                          AU14).
        @param taps       Output to track nodes of the stem tracks whose input also goes to output channels 2 and 3,
                          4 and 5, and so on, when the render has them: a freeze renders each output of the track
                          (docs/design/audio-features.md, AU15). At most maxExportTaps.
        @note  Any thread; takes effect at the next block.
    */
    void setExportMode (bool exporting, std::span<const ObjectId> tracks = {}, bool preFader = false,
                        std::span<const ObjectId> taps = {}) noexcept
    {
        const auto count = std::min (tracks.size(), maxExportTracks);

        for (size_t i = 0; i < count; ++i)
            m_exportTracks[i].store (tracks[i]);

        const auto tapCount = std::min (taps.size(), maxExportTaps);

        for (size_t i = 0; i < tapCount; ++i)
            m_exportTaps[i].store (taps[i]);

        m_numExportTaps.store (tapCount);
        m_numExportTracks.store (count);
        m_exportPreFader.store (preFader);
        m_exporting.store (exporting);
    }

    /**
        Most tracks in one stem render.
    */
    static constexpr size_t maxExportTracks = 64;

    /**
        Most Output to track nodes one render taps.
    */
    static constexpr size_t maxExportTaps = 16;

    /**
        Captures the output (without metronome and preview) into a writer while set: realtime export (X1).

        @param writer  The writer, or nullptr to stop. Must stay alive until the next block after it is replaced.
        @note  Any thread.
    */
    void setOutputCapture (juce::AudioFormatWriter::ThreadedWriter* writer) noexcept { m_outputCapture.store (writer); }

    /**
        Reads the current telemetry.

        Peak values are held until read: each call returns the highest peaks since the previous call.

        @return  The current telemetry.
        @note  Message thread only (a single poller, because reading resets the peaks).
    */
    [[nodiscard]] Telemetry getTelemetry() noexcept;

    /**
        A sample beyond this (+24 dBFS) mutes the device output at once (docs/design/output-protection.md, S3).
    */
    static constexpr float extremeLevel = 16.0f;

    /**
        A device output whose block RMS stays above full scale for this long is muted (S3).
    */
    static constexpr double overloadSeconds = 0.2;

    /**
        How long the device output takes to fade out when muted, and back in when unmuted (S3).
    */
    static constexpr double protectionFadeSeconds = 0.01;

    /**
        Unmutes a device output that output protection muted: it fades back in, and the detection starts afresh.

        @note  Message thread.
    */
    void unmuteOutput() noexcept { m_unmuteRequested.store (true); }

    /**
        Renders audio without a device, for tests and (later) offline export.

        @param outputs      One pointer per channel, each with room for numSamples samples.
        @param numChannels  Number of channels in outputs.
        @param numSamples   Number of samples to render.
        @param sampleRate   Sample rate to render at, in Hz. Must match the published engine's spec.
        @param inputs       Optional device input channels (numSamples each), as the device would pass them.
        @param numInputs    Number of channels in inputs.
    */
    void renderOffline (float* const* outputs, int numChannels, int numSamples, double sampleRate,
                        const float* const* inputs = nullptr, int numInputs = 0) noexcept;

    /**
        Renders one block for the audio device.

        @note  Audio thread. Inputs are ignored in phase 2.
    */
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) noexcept override;

    /**
        Prepares for playback on a device.

        @note  Called by the device before the first callback.
    */
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;

    /**
        Called when the device stops.
    */
    void audioDeviceStopped() override;

private:
    void process (float* const* outputs, int numChannels, int numSamples) noexcept;
    void adopt (const CompiledEngine& engine, double currentTick, bool hadEngine) noexcept;
    void renderBlock (const CompiledEngine& engine, std::span<float* const> channels, int numSamples) noexcept;
    void renderChunk (const CompiledEngine& engine, std::span<float* const> outputs, int offset,
                      int numSamples) noexcept;
    void declickLoopSeam (std::span<float* const> channels, int offset, int numSamples) noexcept;
    void clearLoopSeam() noexcept;
    void renderTrackSteps (const CompiledEngine& engine, const CompiledTrack& track, size_t index,
                           const Transport& transport, int offset, int numSamples, bool loopActive,
                           std::span<NoteEvent> liveScratch, Parallel* parallel = nullptr) noexcept;
    // What mixRenderedTrack() mixes: everything, only what goes to the master mix (or the output), or only what goes to
    // the buses one track reads (an index into CompiledEngine::tracks).
    static constexpr int mixEverything = -2;
    static constexpr int mixToMain = -1;

    void mixRenderedTrack (const CompiledEngine& engine, const CompiledTrack& track, const TrackMix& mix,
                           std::array<float*, 2> bus, std::span<const std::array<float*, 2>> taps, bool stems,
                           bool anySolo, int numSamples, int into = mixEverything) noexcept;
    [[nodiscard]] bool isExportTrack (ObjectId id) const noexcept;
    void runTrack (const CompiledTrack& track, const CompiledEngine& engine, const Transport& transport,
                   std::array<const float*, 2> input, int numSamples, int sampleOffset,
                   std::span<NoteEvent> liveScratch, Parallel* parallel = nullptr) noexcept;
    void meterInput (TrackState& state, int offset, int numSamples) const noexcept;
    void recordChunk (const Transport& transport, int offset, int numSamples) noexcept;
    [[nodiscard]] std::array<const float*, 2> inputFor (const TrackState& state, int offset) const noexcept;
    void renderMetronome (const CompiledEngine& engine, const Transport& transport, std::span<float* const> outputs,
                          int offset, int numSamples) noexcept;
    void publishPeak (float left, float right) noexcept;
    void protectOutput (float* const* outputs, int numChannels, int numSamples) noexcept;
    void detectRunaway (const float* const* outputs, int numChannels, int numSamples) noexcept;
    void takeLiveRequests (int numSamples) noexcept;
    void takeLiveRequest (const LiveInput& request, int numSamples) noexcept;
    [[nodiscard]] std::span<const NoteEvent> liveEventsFor (ObjectId trackId, int numSamples,
                                                            std::span<NoteEvent> scratch) noexcept;

    /**
        A small pool of threads that render independent tracks (created on the message thread when the first
        engine is published; the audio thread only wakes and waits for them).
    */
    class WorkerPool;
    [[nodiscard]] static bool defaultParallelTracks() noexcept;

    struct PendingLiveEvent
    {
        ObjectId trackId = 0;
        NoteEvent event;
        LiveInput input; // as sent, for recording
    };

    static constexpr size_t maxLiveNotes = 64;
    static constexpr size_t maxLiveEvents = 256; // per block, after coalescing expression

    SnapshotExchange<CompiledEngine> m_engines;
    std::unique_ptr<WorkerPool> m_workerPool; // created on the message thread, never on the audio thread
    std::atomic<bool> m_parallelTracks { defaultParallelTracks() };
    SpscQueue<LiveInput, 256> m_liveRequests;   // audition (message thread)
    SpscQueue<LiveInput, 1024> m_inputRequests; // MIDI input (serialised MIDI threads)
    SpscQueue<RecordedInput, 2048> m_recordedInputs;
    std::atomic<ObjectId> m_nextLiveId { -1 }; // live ids count down so they never clash with model ids
    std::atomic<int> m_inputLatency { 0 };
    std::atomic<double> m_callbackSeconds { 0.0 }; // when the current device callback started

    std::atomic<bool> m_playRequested { false };
    std::atomic<bool> m_stopRequested { false };
    std::atomic<double> m_seekRequest { -1.0 }; // beats; negative = none
    std::atomic<bool> m_playing { false };
    std::atomic<double> m_positionForUi { 0.0 };
    std::atomic<double> m_deviceSampleRateForUi { 0.0 };
    std::atomic<int> m_latencySamplesForUi { 0 };
    std::atomic<float> m_peakLeft { 0.0f };
    std::atomic<float> m_peakRight { 0.0f };

    // Audio-thread state.
    double m_deviceSampleRate = 0.0;
    const CompiledEngine* m_current = nullptr;
    TimeMap m_timeMap;
    juce::int64 m_samplePosition = 0;
    bool m_isPlaying = false;
    bool m_justStarted = false;
    bool m_justStopped = false;
    bool m_justJumped = false;
    bool m_justLooped = false;
    static constexpr int loopDeclickSamples = 64; // the step at a loop seam fades out over this many samples
    std::array<float, 2> m_seamLast {};           // the last sample before the seam
    std::array<float, 2> m_seamStep {};           // the step at the seam, still to fade out
    int m_seamRemaining = 0;
    bool m_seamPending = false; // a seam was crossed; the next rendered sample is the loop start

    // Device input of the current callback, and recording.
    static constexpr int maxInputs = 64;
    std::array<const float*, maxInputs> m_inputs {};
    int m_numInputs = 0;
    std::array<RecordTarget, maxRecordTargets> m_recordTargets {};
    std::array<std::atomic<juce::int64>, maxRecordTargets> m_recordedFrames {};
    std::atomic<size_t> m_numRecordTargets { 0 };
    std::atomic<bool> m_recording { false };
    std::atomic<double> m_recordFromBeats { 0.0 };
    std::atomic<double> m_recordUntilBeats { -1.0 };
    std::atomic<double> m_recordStartBeats { -1.0 };
    std::atomic<int> m_recordLoopWraps { 0 };
    std::atomic<bool> m_exporting { false };
    std::array<std::atomic<ObjectId>, maxExportTracks> m_exportTracks {};
    std::atomic<size_t> m_numExportTracks { 0 };
    std::array<std::atomic<ObjectId>, maxExportTaps> m_exportTaps {};
    std::atomic<size_t> m_numExportTaps { 0 };
    std::atomic<bool> m_exportPreFader { false };
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> m_outputCapture { nullptr };
    std::atomic<float> m_inputPeak { 0.0f };

    // Output protection (docs/design/output-protection.md, S2, S3): the mute's reason, shared with the message thread,
    // and the audio thread's fade gain and overload count.
    std::atomic<OutputMute> m_outputMute { OutputMute::none };
    std::atomic<bool> m_unmuteRequested { false };
    float m_protectionGain = 1.0f;
    double m_overloadSamples = 0.0;

    // Preview voice (sound explorer).
    struct PreviewVoice
    {
        std::shared_ptr<const AudioSource> source;
    };

    void renderPreview (std::span<float* const> outputs, int numSamples) noexcept;

    SnapshotExchange<PreviewVoice> m_previews;
    const PreviewVoice* m_preview = nullptr; // audio thread
    double m_previewFrame = 0.0;             // audio thread
    std::array<std::vector<float>, 2> m_previewScratch;
    std::atomic<float> m_previewGain { 0.7f };
    std::atomic<double> m_previewSecondsForUi { -1.0 };
    double m_playStartTick = 0.0;
    double m_clickPhase = 0.0;
    double m_clickFrequency = 0.0;
    double m_clickEnvelope = 0.0;
    double m_clickLevel = 1.0;
    std::array<CompiledNote, maxLiveNotes> m_liveNotes {};
    std::array<std::array<CurvePoint, 3>, maxLiveNotes> m_liveCurves {}; // initial pitch, pressure, timbre
    std::array<PendingLiveEvent, maxLiveEvents> m_pendingLive {};
    size_t m_pendingLiveCount = 0;
    std::array<NoteEvent, maxLiveEvents> m_trackLive {};
    bool m_deliverLive = false;
};

} // namespace dawg
