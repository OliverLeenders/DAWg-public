#include "MainComponent.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/ClipOps.h"
#include "dawg/Export.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

#include <limits>
#include <thread>

// Export in the app (docs/design/phase9-export-midi-input.md, X1–X5): the options dialog's request becomes one pass
// per file (the mix and each stem), rendered offline on a worker thread while the device is paused, or in real time
// through the device. Each pass renders to a 32-bit float file first, which is then measured and written in the
// chosen format.

namespace dawg
{

namespace
{
constexpr double quietPeak = 3.1622776e-5; // −90 dBFS
constexpr double realtimeQuietSeconds = 1.0;
constexpr int progressIntervalMs = 100;

[[nodiscard]] ExportFormat formatFrom (const juce::String& name)
{
    if (name == "wav16")
        return ExportFormat::wav16;
    if (name == "wav32")
        return ExportFormat::wav32;
    if (name == "flac16")
        return ExportFormat::flac16;
    if (name == "flac24")
        return ExportFormat::flac24;
    if (name == "mp3")
        return ExportFormat::mp3;
    return ExportFormat::wav24;
}

[[nodiscard]] juce::String extensionOf (ExportFormat format)
{
    if (format == ExportFormat::mp3)
        return ".mp3";

    return format == ExportFormat::flac16 || format == ExportFormat::flac24 ? ".flac" : ".wav";
}

// The dialog's MP3 options (X6): `mp3: { quality: "cbr320" | "cbr256" | "cbr192" | "cbr128" | "v0" | "v2", tags }`.
[[nodiscard]] Mp3Settings mp3SettingsFrom (const juce::var& options)
{
    Mp3Settings settings;
    const auto& mp3 = options["mp3"];
    const auto quality = mp3["quality"].toString();

    if (quality.startsWith ("v"))
        settings.vbrQuality = std::clamp (quality.substring (1).getIntValue(), 0, 9);
    else if (const auto kbps = quality.substring (3).getIntValue(); kbps == 128 || kbps == 192 || kbps == 256)
        settings.bitrateKbps = kbps;

    const auto& tags = mp3["tags"];
    settings.tags = { tags["title"].toString(),  tags["artist"].toString(), tags["album"].toString(),
                      tags["year"].toString(),   tags["track"].toString(),  tags["genre"].toString(),
                      tags["comment"].toString() };
    return settings;
}

[[nodiscard]] double tailSecondsFrom (const juce::var& options)
{
    const auto tail = options["tail"].toString();

    if (tail == "fixed")
        return std::max (0.0, static_cast<double> (options["tailSeconds"]));

    return tail == "auto" ? 30.0 : 0.0; // the longest automatic tail, or none
}

[[nodiscard]] const char* endStateOf (bool failed, bool cancelled)
{
    if (failed)
        return "failed";

    return cancelled ? "cancelled" : "done";
}

// A 32-bit float stereo WAV writer for a render, or nullptr.
[[nodiscard]] std::unique_ptr<juce::AudioFormatWriter> floatWriter (const juce::File& file, double sampleRate)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return nullptr;

    return wav.createWriterFor (stream,
                                juce::AudioFormatWriterOptions {}
                                    .withSampleRate (sampleRate)
                                    .withNumChannels (2)
                                    .withBitsPerSample (32)
                                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
}

void setNonRealtime (const std::vector<std::shared_ptr<Node>>& instances, bool nonRealtime)
{
    for (const auto& node : instances)
        if (auto* plugin = dynamic_cast<PluginNode*> (node.get()); plugin != nullptr && plugin->processor() != nullptr)
            plugin->processor()->setNonRealtime (nonRealtime);
}

[[nodiscard]] double toDb (double gain)
{
    return gain > 0.0 ? 20.0 * std::log10 (gain) : -std::numeric_limits<double>::infinity();
}

struct RenderMeasurement
{
    juce::String error; // empty unless the render could not be read
    bool cancelled = false;
    double peak = 0.0;
    double lufs = 0.0;
};

// A render's peak and loudness, read in chunks so a cancel stays responsive.
[[nodiscard]] RenderMeasurement measureRender (const juce::File& render, const std::atomic<bool>& cancel)
{
    RenderMeasurement measurement;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (render.createInputStream().release(), true));

    if (reader == nullptr)
    {
        measurement.error = "Could not read the rendered audio";
        return measurement;
    }

    LoudnessMeter meter;
    meter.prepare (reader->sampleRate);
    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (2, chunk);

    for (juce::int64 position = 0; position < reader->lengthInSamples; position += chunk)
    {
        if (cancel.load())
        {
            measurement.cancelled = true;
            return measurement;
        }

        const auto count = static_cast<int> (std::min<juce::int64> (chunk, reader->lengthInSamples - position));
        std::ignore = reader->read (&buffer, 0, count, position, true, true);
        meter.process ({ buffer.getReadPointer (0), buffer.getReadPointer (1) }, count);
    }

    measurement.peak = static_cast<double> (meter.samplePeak());
    measurement.lufs = meter.integratedLufs();
    return measurement;
}
} // namespace

/**
    A running export.
*/
struct MainComponent::ExportJob
{
    ExportJob() = default;
    ExportJob (const ExportJob&) = delete;
    ExportJob& operator= (const ExportJob&) = delete;
    ExportJob (ExportJob&&) = delete;
    ExportJob& operator= (ExportJob&&) = delete;

    ~ExportJob()
    {
        // The render thread uses the engine: it must be finished before this job (and the engine) go away. The
        // thread itself holds the job too, so the last reference can drop on it, after its work is done; a thread
        // can't join itself, so it is let go instead.
        if (! worker.joinable())
            return;

        if (worker.get_id() == std::this_thread::get_id())
            worker.detach();
        else
            worker.join();
    }

    struct Pass
    {
        ObjectId stemTrack = 0;
        juce::File target;
        juce::File render; // the float render
        juce::int64 frames = 0;
        std::vector<juce::File> tapTargets; // the range's taps (a freeze, docs/design/audio-features.md, AU15)
        std::vector<juce::File> tapRenders;
    };

    // Track renders (docs/design/audio-features.md, AU14): the event that reports progress, and what happens with
    // the renders instead of writing export files.
    juce::String event { "dawg_export" };
    std::function<void (ExportJob&)> onRendered;
    std::optional<Project> project; // compiled for the render instead of the session's: compiled again after

    ExportRange range;
    ExportFormat format = ExportFormat::wav24;
    Mp3Settings mp3;
    bool normalise = false;
    double normaliseDb = -1.0;
    bool realtime = false;
    std::vector<Pass> passes;
    size_t current = 0;
    double sampleRate = 48000.0;
    int blockSize = 512;
    double previousPosition = 0.0;
    std::vector<std::shared_ptr<Node>> instances;
    juce::Array<juce::var> results;
    std::atomic<bool> cancel { false };
    std::atomic<bool> running { false };
    std::thread worker; // the render, then the pass that writes the files

    // Realtime mode.
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    double tailStarted = -1.0;
    double quietSince = -1.0;
};

void MainComponent::waitForExport()
{
    // Closing during an export: its thread uses the engine, so stop it and wait. The thread checks the flag between
    // blocks and files, so joining returns promptly.
    if (m_export == nullptr)
        return;

    m_export->cancel.store (true);
    m_engine.setOutputCapture (nullptr);

    if (m_export->worker.joinable())
        m_export->worker.join();
}

bool MainComponent::offlineExportRunning() const
{
    return m_export != nullptr && ! m_export->realtime && m_export->running.load();
}

juce::File MainComponent::exportFolder() const
{
    // Untitled projects live in a hidden folder until saved: export next to the user's projects instead.
    return static_cast<bool> (m_projects->stateVar()["untitled"])
               ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("DAWg/Exports")
               : m_projects->projectFolder().getChildFile ("Exports");
}

void MainComponent::registerExportHandlers()
{
    m_bridge.on ("export.defaults",
                 [this] (const juce::var&)
                 {
                     const auto project = m_session.project();
                     Ticks end = 0;

                     for (const auto& track : project->tracks)
                         for (const auto& clip : track->clips)
                             end = std::max (end, clip->start + clip->length);

                     // The rate a render has, and the rate its MP3 is written at (X6), for the dialog to mention.
                     auto* device = m_deviceManager.getCurrentAudioDevice();
                     const auto rate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;

                     return makeObject ({ { "folder", exportFolder().getFullPathName() },
                                          { "name", m_projects->stateVar()["name"].toString() },
                                          { "songEndBeats", static_cast<double> (end) / ticksPerBeat },
                                          { "sampleRate", rate },
                                          { "mp3SampleRate", mp3SampleRate (rate) } });
                 });

    m_bridge.on ("export.chooseFolder",
                 [this] (const juce::var& p)
                 {
                     m_exportChooser =
                         std::make_unique<juce::FileChooser> ("Export to", juce::File (p["folder"].toString()));
                     m_exportChooser->launchAsync (
                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this] (const juce::FileChooser& chooser)
                         {
                             if (chooser.getResult() != juce::File())
                                 m_bridge.emit ("dawg_exportFolder", chooser.getResult().getFullPathName());
                         });
                     return juce::var();
                 });

    m_bridge.on ("export.start",
                 [this] (const juce::var& p)
                 {
                     if (m_export != nullptr)
                         throw std::runtime_error ("An export is already running");

                     startExport (p);
                     return juce::var();
                 });

    // Freeze (docs/design/audio-features.md, AU15). A frozen track is unfrozen first, so a refreeze renders it live.
    m_bridge.on ("audio.freezeTrack",
                 [this] (const juce::var& p)
                 {
                     if (m_export != nullptr)
                         throw std::runtime_error ("A render is already running");

                     const auto trackId = static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"]));
                     const auto* track = m_session.project()->findTrack (trackId);

                     if (track == nullptr)
                         throw std::runtime_error ("No such track");

                     if (track->freeze.active())
                     {
                         const auto result = m_session.apply ("unfreezeTrack", makeObject ({ { "trackId", trackId } }));

                         if (! result.ok)
                             throw std::runtime_error (result.error.toStdString());

                         recompileEngine();
                     }

                     freezeTrack (trackId);
                     return juce::var();
                 });

    // Bounce in place, Consolidate and Render selection (AU16, AU17).
    m_bridge.on ("audio.bounceTrack",
                 [this] (const juce::var& p)
                 {
                     if (m_export != nullptr)
                         throw std::runtime_error ("A render is already running");

                     bounceTrack (static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"])));
                     return juce::var();
                 });

    m_bridge.on ("audio.consolidateClips",
                 [this] (const juce::var& p)
                 {
                     if (m_export != nullptr)
                         throw std::runtime_error ("A render is already running");

                     consolidateClips (p["clipIds"]);
                     return juce::var();
                 });

    m_bridge.on ("audio.renderSelection",
                 [this] (const juce::var& p)
                 {
                     if (m_export != nullptr)
                         throw std::runtime_error ("A render is already running");

                     renderSelection (p["trackIds"], p["clipIds"]);
                     return juce::var();
                 });

    m_bridge.on ("audio.unfreezeTrack",
                 [this] (const juce::var& p)
                 {
                     const auto result = m_session.apply (
                         "unfreezeTrack", makeObject ({ { "trackId", static_cast<juce::int64> (p["trackId"]) } }));

                     if (! result.ok)
                         throw std::runtime_error (result.error.toStdString());

                     return juce::var();
                 });

    m_bridge.on ("export.cancel",
                 [this] (const juce::var&)
                 {
                     if (m_export != nullptr)
                         m_export->cancel.store (true);

                     return juce::var();
                 });
}

void MainComponent::startExport (const juce::var& options)
{
    auto* device = m_deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        throw std::runtime_error ("No audio device is open");

    auto job = std::make_shared<ExportJob>();
    job->range.startBeats = std::max (0.0, static_cast<double> (options["startBeats"]));
    job->range.endBeats = static_cast<double> (options["endBeats"]);
    job->range.autoTail = options["tail"].toString() == "auto";
    job->range.tailSeconds = tailSecondsFrom (options);
    job->format = formatFrom (options["format"].toString());
    job->mp3 = mp3SettingsFrom (options);
    job->normalise = static_cast<bool> (options["normalise"]);
    job->normaliseDb = std::min (0.0, static_cast<double> (options.getProperty ("normaliseDb", -1.0)));
    job->realtime = static_cast<bool> (options["realtime"]);
    job->sampleRate = device->getCurrentSampleRate();
    job->blockSize = currentSpec().maxBlockSize;
    job->previousPosition = m_engine.positionBeats();

    if (job->range.endBeats <= job->range.startBeats)
        throw std::runtime_error ("The range to export is empty");

    const auto folder = juce::File (options["folder"].toString());

    if (! folder.createDirectory())
        throw std::runtime_error (("Cannot create " + folder.getFullPathName()).toStdString());

    const auto name = juce::File::createLegalFileName (
        options["name"].toString().trim().isEmpty() ? juce::String ("Export") : options["name"].toString().trim());
    const auto extension = extensionOf (job->format);
    const auto renderFile = [&folder] { return folder.getChildFile (".render-" + juce::Uuid().toString() + ".wav"); };

    if (static_cast<bool> (options.getProperty ("mix", true)))
        job->passes.push_back ({ 0, folder.getChildFile (name + extension), renderFile(), 0, {}, {} });

    // Stems: one pass per track, muted tracks left out (X3). Realtime export renders the mix only.
    if (const auto* stems = options["stems"].getArray(); stems != nullptr && ! job->realtime)
    {
        const auto project = m_session.project();

        for (const auto& id : *stems)
            if (const auto* track = project->findTrack (static_cast<ObjectId> (id)); track != nullptr && ! track->mute)
                job->passes.push_back (
                    { track->id,
                      folder.getChildFile (name + " - " + juce::File::createLegalFileName (track->name) + extension),
                      renderFile(),
                      0,
                      {},
                      {} });
    }

    if (job->passes.empty())
        throw std::runtime_error ("Nothing to export: choose the mix or a stem");

    m_export = job;

    // Stretched audio is rendered first, so the export never plays a preview (docs/design/time-stretching.md, TS10).
    // Pitch renders go first; stretch renders of corrected files read them (docs/design/pitch-correction.md, 5.2).
    auto stretch = m_stretch != nullptr ? m_stretch->pitchJobs (*m_session.project()) : std::vector<StretchJob> {};

    for (auto& stretchJob : stretchJobs (*m_session.project()))
        stretch.push_back (std::move (stretchJob));

    if (! stretch.empty() && m_stretch != nullptr)
    {
        renderStretchesThenExport (std::move (stretch));
        return;
    }

    if (job->realtime)
        startRealtimeExport();
    else
        startOfflineExport();
}

void MainComponent::renderStretchesThenExport (std::vector<StretchJob> stretch)
{
    auto job = m_export;
    m_bridge.emit (job->event, makeObject ({ { "state", "stretching" }, { "progress", 0.0 } }));

    // The paths are made absolute here, where the project folder is known; the renders run on the export thread.
    for (auto& entry : stretch)
        std::ignore = m_audio->identify (entry.file);

    job->worker = std::thread (
        [this, job, jobs = std::move (stretch), safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            const auto ok = m_stretch->renderNow (
                jobs,
                [this, job, safe] (double progress)
                {
                    juce::MessageManager::callAsync (
                        [this, safe, progress, event = job->event]
                        {
                            if (safe != nullptr)
                                m_bridge.emit (event,
                                               makeObject ({ { "state", "stretching" }, { "progress", progress } }));
                        });
                    return ! job->cancel.load();
                });

            juce::MessageManager::callAsync (
                [this, safe, ok]
                {
                    if (safe == nullptr || m_export == nullptr)
                        return;

                    if (m_export->worker.joinable())
                        m_export->worker.join();

                    if (! ok)
                    {
                        endExport (m_export->cancel.load() ? juce::String()
                                                           : juce::String ("Rendering stretched audio failed"));
                        return;
                    }

                    recompileEngine(); // with the renders in

                    if (m_export->realtime)
                        startRealtimeExport();
                    else
                        startOfflineExport();
                });
        });
}

void MainComponent::startOfflineExport()
{
    auto job = m_export;

    // A track render of some clips compiles a project of its own (docs/design/audio-features.md, AU16).
    if (job->project.has_value())
        m_engine.setEngine (m_compiler.compile (*job->project, currentSpec()));

    // The worker thread becomes the engine's only caller (X1): the device is paused, plugins render offline.
    m_engine.stop();
    m_deviceManager.removeAudioCallback (&m_engine);
    job->instances = m_compiler.instances();
    setNonRealtime (job->instances, true);
    job->running.store (true);

    job->worker = std::thread (
        [this, job, spec = currentSpec(), safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            auto error = renderExportPasses (*job, spec,
                                             [this, safe, event = job->event] (const juce::var& progress)
                                             {
                                                 juce::MessageManager::callAsync (
                                                     [this, safe, progress, event]
                                                     {
                                                         if (safe != nullptr)
                                                             m_bridge.emit (event, progress);
                                                     });
                                             });
            job->running.store (false);

            juce::MessageManager::callAsync (
                [this, safe, error]
                {
                    if (safe == nullptr)
                        return;

                    // Back to the device (X1).
                    setNonRealtime (m_export->instances, false);
                    m_deviceManager.addAudioCallback (&m_engine);
                    updateEngine(); // a change made while rendering was held back
                    m_engine.seek (m_export->previousPosition);

                    if (error.isNotEmpty() || m_export->cancel.load())
                        endExport (error);
                    else if (m_export->onRendered != nullptr)
                        finishTrackRender();
                    else
                        finaliseExport();
                });
        });
}

juce::String MainComponent::renderExportPasses (ExportJob& job, const ProcessSpec& spec,
                                                const std::function<void (const juce::var&)>& progress)
{
    // Every Convolution Reverb renders with the IR it should have (docs/design/convolution-reverb.md, I15).
    waitForConvolutionBuilds();

    for (size_t i = 0; i < job.passes.size() && ! job.cancel.load(); ++i)
    {
        const auto& pass = job.passes[i];

        // Every pass starts from fresh state: no tails from the previous one (X3), or from a loop still ringing when the first began.
        for (const auto& node : job.instances)
            node->prepare (spec);

        auto writer = floatWriter (pass.render, job.sampleRate);

        if (writer == nullptr)
            return "Cannot write to " + pass.render.getParentDirectory().getFullPathName();

        std::vector<std::unique_ptr<juce::AudioFormatWriter>> tapWriters;

        for (const auto& file : pass.tapRenders)
            if (tapWriters.push_back (floatWriter (file, job.sampleRate)); tapWriters.back() == nullptr)
                return "Cannot write to " + file.getParentDirectory().getFullPathName();

        auto range = job.range;
        if (pass.stemTrack != 0)
            range.tracks = { pass.stemTrack };

        range.taps.resize (std::min (range.taps.size(), tapWriters.size()));
        const auto beats = range.endBeats - range.startBeats;
        auto lastProgress = juce::Time::getMillisecondCounter();

        const auto frames = renderRange (
            m_engine, range, job.sampleRate, job.blockSize,
            [&] (std::array<const float*, 2> channels, int count)
            {
                if (! writer->writeFromFloatArrays (channels.data(), 2, count))
                    return false;

                if (const auto now = juce::Time::getMillisecondCounter(); now - lastProgress > progressIntervalMs)
                {
                    lastProgress = now;
                    progress (makeObject (
                        { { "state", "rendering" },
                          { "pass", static_cast<int> (i) },
                          { "passes", static_cast<int> (job.passes.size()) },
                          { "progress", std::min (1.0, (m_engine.positionBeats() - range.startBeats) / beats) } }));
                }

                return ! job.cancel.load();
            },
            [&] (std::span<const float* const> taps, int count)
            {
                for (size_t k = 0; k < tapWriters.size() && 2 * k + 1 < taps.size(); ++k)
                    if (! tapWriters[k]->writeFromFloatArrays (taps.data() + 2 * k, 2, count))
                        return false;

                return true;
            });

        writer.reset();
        tapWriters.clear();
        job.passes[i].frames = frames;

        if (frames < 0 && ! job.cancel.load())
            return "Writing " + pass.render.getFileName() + " failed";
    }

    return {};
}

void MainComponent::freezeTrack (ObjectId trackId)
{
    const auto project = m_session.project();
    const auto* track = project->findTrack (trackId);

    if (track == nullptr)
        return;

    if (track->kind != TrackKind::instrument && track->kind != TrackKind::audio)
        throw std::runtime_error ("Only instrument and audio tracks can be frozen");

    if (track->clips.empty())
        throw std::runtime_error ("The track has no clips to freeze");

    Ticks start = std::numeric_limits<Ticks>::max();
    Ticks end = 0;

    for (const auto& clip : track->clips)
    {
        start = std::min (start, clip->start);
        end = std::max (end, clip->start + clip->length);
    }

    // Named by the key: freezing again without changes finds the render and doesn't make it again.
    const auto key = freezeKey (*project, *track);
    const auto folder = m_projects->projectFolder().getChildFile ("Audio").getChildFile ("Frozen");
    const auto base = juce::File::createLegalFileName (track->name) + " " + key;
    const auto target = folder.getChildFile (base + ".wav");

    // Each Output to track that feeds an output track is rendered too, so the output track plays on (AU15).
    std::vector<ObjectId> taps;
    std::vector<juce::File> tapTargets;

    for (const auto& node : track->graph.nodes)
        if (const auto* output = project->findTrack (node.target);
            node.type == nodetype::busOutput && output != nullptr && output->kind == TrackKind::output
            && output->source == trackId && taps.size() < AudioEngine::maxExportTaps)
        {
            taps.push_back (node.id);
            tapTargets.push_back (folder.getChildFile (base + " " + juce::String (node.id) + ".wav"));
        }

    const auto apply =
        [this, trackId, start, name = track->name, taps, tapTargets] (const juce::File& file, juce::int64 frames)
    {
        const auto fileVar = [this, frames] (const juce::File& render, const juce::String& title)
        {
            return makeObject (
                { { "path", render.getRelativePathFrom (m_projects->projectFolder()).replaceCharacter ('\\', '/') },
                  { "name", title },
                  { "sampleRate", m_deviceManager.getCurrentAudioDevice() != nullptr
                                      ? m_deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                                      : 48000.0 },
                  { "channels", 2 },
                  { "frames", frames } });
        };
        juce::Array<juce::var> outputs;

        for (size_t k = 0; k < taps.size(); ++k)
            outputs.add (
                makeObject ({ { "node", taps[k] }, { "file", fileVar (tapTargets[k], name + " (frozen output)") } }));

        const auto result =
            m_session.apply ("freezeTrack", makeObject ({ { "trackId", trackId },
                                                          { "start", start },
                                                          { "file", fileVar (file, name + " (frozen)") },
                                                          { "outputs", outputs } }));

        if (! result.ok)
            m_bridge.emit ("dawg_error", result.error);
    };

    // A render made before (with every output's) is used again.
    const auto lengthOf = [this] (const juce::File& file) -> juce::int64
    {
        const std::unique_ptr<juce::AudioFormatReader> reader (
            file.existsAsFile() ? m_audio->formats().createReaderFor (file) : nullptr);
        return reader != nullptr ? reader->lengthInSamples : 0;
    };

    if (const auto frames = lengthOf (target);
        frames > 0 && std::ranges::all_of (tapTargets, [&] (const juce::File& f) { return lengthOf (f) == frames; }))
    {
        apply (target, frames);
        return;
    }

    TrackRender render;
    render.range.startBeats = static_cast<double> (start) / ticksPerBeat;
    render.range.endBeats = static_cast<double> (end) / ticksPerBeat;
    render.range.autoTail = true;
    render.range.tailSeconds = 30.0;
    render.range.tracks = { trackId };
    render.range.preFader = true;
    render.range.taps = taps;
    render.target = target;
    render.tapTargets = tapTargets;
    render.label = "Freezing " + track->name;
    render.done = apply;
    startTrackRender (std::move (render));
}

// Renders tracks offline through the engine, as an export does (AU14), into a 32-bit float WAV, then hands it on.
void MainComponent::startTrackRender (TrackRender render)
{
    auto* device = m_deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        throw std::runtime_error ("No audio device is open");

    if (! render.target.getParentDirectory().createDirectory())
        throw std::runtime_error (
            ("Cannot create " + render.target.getParentDirectory().getFullPathName()).toStdString());

    auto job = std::make_shared<ExportJob>();
    job->range = render.range;
    job->sampleRate = device->getCurrentSampleRate();
    job->blockSize = currentSpec().maxBlockSize;
    job->previousPosition = m_engine.positionBeats();
    job->event = "dawg_render";
    const auto scratch = [] (const juce::File& target)
    { return target.getSiblingFile (".render-" + juce::Uuid().toString() + ".wav"); };
    ExportJob::Pass pass { render.range.tracks.size() == 1 ? render.range.tracks[0] : ObjectId { 0 },
                           render.target,
                           scratch (render.target),
                           0,
                           {},
                           {} };
    pass.tapTargets = render.tapTargets;

    for (const auto& target : render.tapTargets)
        pass.tapRenders.push_back (scratch (target));

    job->passes.push_back (std::move (pass));
    job->onRendered = [done = render.done] (ExportJob& finished)
    {
        const auto& rendered = finished.passes.front();

        for (size_t k = 0; k < rendered.tapRenders.size(); ++k)
        {
            std::ignore = rendered.tapTargets[k].deleteFile();

            if (! rendered.tapRenders[k].moveFileTo (rendered.tapTargets[k]))
                return;
        }

        std::ignore = rendered.target.deleteFile();

        if (rendered.render.moveFileTo (rendered.target))
            done (rendered.target, rendered.frames);
    };
    m_export = job;
    m_bridge.emit ("dawg_render",
                   makeObject ({ { "state", "rendering" }, { "label", render.label }, { "progress", 0.0 } }));
    job->project = std::move (render.project);

    // Stretched audio first, as for an export (docs/design/time-stretching.md, TS10).
    auto stretch = m_stretch != nullptr ? m_stretch->pitchJobs (*m_session.project()) : std::vector<StretchJob> {};

    for (auto& stretchJob : stretchJobs (*m_session.project()))
        stretch.push_back (std::move (stretchJob));

    if (! stretch.empty() && m_stretch != nullptr)
        renderStretchesThenExport (std::move (stretch));
    else
        startOfflineExport();
}

void MainComponent::finishTrackRender()
{
    auto job = m_export;

    if (job->worker.joinable())
        job->worker.join();

    m_export.reset();

    if (job->project.has_value())
        recompileEngine();

    job->onRendered (*job);
    m_bridge.emit (job->event, makeObject ({ { "state", "done" } }));
}

juce::var MainComponent::renderedFile (const juce::File& file, juce::int64 frames, const juce::String& name) const
{
    auto* device = m_deviceManager.getCurrentAudioDevice();
    return makeObject (
        { { "path", file.getRelativePathFrom (m_projects->projectFolder()).replaceCharacter ('\\', '/') },
          { "name", name },
          { "sampleRate", device != nullptr ? device->getCurrentSampleRate() : 48000.0 },
          { "channels", 2 },
          { "frames", frames } });
}

namespace
{
// A file in a folder of the project's Audio folder that doesn't exist yet: "<name>.wav", else "<name> (2).wav", ...
[[nodiscard]] juce::File newRenderFile (const juce::File& projectFolder, const juce::String& folder,
                                        const juce::String& name)
{
    const auto dir = projectFolder.getChildFile ("Audio").getChildFile (folder);
    const auto base = juce::File::createLegalFileName (name);
    auto file = dir.getChildFile (base + ".wav");

    for (int i = 2; file.exists(); ++i)
        file = dir.getChildFile (base + " (" + juce::String (i) + ").wav");

    return file;
}

// The timeline span of clips.
[[nodiscard]] std::pair<Ticks, Ticks> spanOf (const std::vector<ClipPtr>& clips)
{
    Ticks start = std::numeric_limits<Ticks>::max();
    Ticks end = 0;

    for (const auto& clip : clips)
    {
        start = std::min (start, clip->start);
        end = std::max (end, clip->start + clip->length);
    }

    return { start, end };
}

[[nodiscard]] juce::var command (const char* name, juce::var args)
{
    return makeObject ({ { "command", name }, { "args", std::move (args) } });
}
} // namespace

// Bounce in place (AU16): the whole track through its chain, before the fader. An audio track gets one clip of the
// render with its inserts bypassed; an instrument track gets a new audio track after it, and is muted.
void MainComponent::bounceTrack (ObjectId trackId)
{
    const auto project = m_session.project();
    const auto* track = project->findTrack (trackId);

    if (track == nullptr || (track->kind != TrackKind::instrument && track->kind != TrackKind::audio))
        throw std::runtime_error ("Only instrument and audio tracks can be bounced");

    if (track->freeze.active())
        throw std::runtime_error ("Unfreeze the track to bounce it");

    if (track->clips.empty())
        throw std::runtime_error ("The track has no clips to bounce");

    const auto span = spanOf (track->clips);
    const auto start = span.first;
    TrackRender render;
    render.range = { static_cast<double> (start) / ticksPerBeat,
                     static_cast<double> (span.second) / ticksPerBeat,
                     true,
                     30.0,
                     { trackId },
                     false,
                     {} };
    render.range.preFader = true;
    render.target = newRenderFile (m_projects->projectFolder(), "Bounces", track->name + " bounce");
    render.label = "Bouncing " + track->name;
    render.done = [this, trackId, start, audio = track->kind == TrackKind::audio,
                   name = track->name] (const juce::File& file, juce::int64 frames)
    {
        const auto now = m_session.project();
        const auto* current = now->findTrack (trackId);

        if (current == nullptr)
            return;

        juce::Array<juce::var> commands;
        const auto fileVar = renderedFile (file, frames, name + " bounce");

        if (audio)
        {
            juce::Array<juce::var> ids;
            for (const auto& clip : current->clips)
                ids.add (clip->id);

            commands.add (command ("deleteClips", makeObject ({ { "ids", ids } })));
            commands.add (command (
                "addAudioClip",
                makeObject (
                    { { "trackId", trackId }, { "start", start }, { "file", fileVar }, { "recorded", true } })));

            for (const auto& node : current->graph.nodes)
                if (node.type != nodetype::audioPlayer && node.type != nodetype::trackOutput && ! node.bypassed)
                    commands.add (
                        command ("setNodeBypass",
                                 makeObject ({ { "trackId", trackId }, { "nodeId", node.id }, { "bypassed", true } })));
        }
        else
        {
            commands.add (command (
                "addRenderTrack",
                makeObject ({ { "after", trackId }, { "start", start }, { "like", true }, { "file", fileVar } })));
            commands.add (command ("setTrackSwitches", makeObject ({ { "id", trackId }, { "mute", true } })));
        }

        const auto result =
            m_session.apply ("batch", makeObject ({ { "label", "Bounce in place" }, { "commands", commands } }));

        if (! result.ok)
            m_bridge.emit ("dawg_error", result.error);
    };
    startTrackRender (std::move (render));
}

// Consolidate (AU16): what some audio clips of one track play, without the track's chain, in one new clip.
void MainComponent::consolidateClips (const juce::var& clipIds)
{
    const auto project = m_session.project();
    std::vector<ClipPtr> clips;
    const Track* owner = nullptr;
    std::vector<ObjectId> ids;

    if (const auto* list = clipIds.getArray())
        for (const auto& id : *list)
            for (const auto& track : project->tracks)
                if (const auto index = track->indexOfClip (static_cast<ObjectId> (static_cast<juce::int64> (id)));
                    index >= 0)
                {
                    if (owner != nullptr && owner != track.get())
                        throw std::runtime_error ("Consolidate clips of one track at a time");

                    owner = track.get();
                    clips.push_back (track->clips[static_cast<size_t> (index)]);
                    ids.push_back (clips.back()->id);
                }

    if (owner == nullptr || owner->kind != TrackKind::audio)
        throw std::runtime_error ("Select audio clips to consolidate");

    if (owner->freeze.active())
        throw std::runtime_error ("Unfreeze the track to edit it");

    const auto span = spanOf (clips);
    const auto start = span.first;
    const auto trackId = owner->id;
    TrackRender render;
    render.range = { static_cast<double> (start) / ticksPerBeat,
                     static_cast<double> (span.second) / ticksPerBeat,
                     false,
                     0.0,
                     { trackId },
                     false,
                     {} };
    render.range.preFader = true;
    render.project = clipsRenderProject (*project, trackId, ids, true);
    render.target = newRenderFile (m_projects->projectFolder(), "Bounces", owner->name + " consolidated");
    render.label = "Consolidating";
    render.done = [this, trackId, start, ids, name = owner->name] (const juce::File& file, juce::int64 frames)
    {
        juce::Array<juce::var> idList;
        for (const auto id : ids)
            idList.add (id);

        const auto commands = juce::Array<juce::var> {
            command ("deleteClips", makeObject ({ { "ids", idList } })),
            command ("addAudioClip", makeObject ({ { "trackId", trackId },
                                                   { "start", start },
                                                   { "file", renderedFile (file, frames, name + " consolidated") },
                                                   { "recorded", true } })),
        };
        const auto result =
            m_session.apply ("batch", makeObject ({ { "label", "Consolidate" }, { "commands", commands } }));

        if (! result.ok)
            m_bridge.emit ("dawg_error", result.error);
    };
    startTrackRender (std::move (render));
}

// Render selection (AU17): the selected tracks after their faders, over the loop range if it is on, else the selected
// clips' span, into a new audio track after the last of them.
void MainComponent::renderSelection (const juce::var& trackIds, const juce::var& clipIds)
{
    const auto project = m_session.project();
    std::vector<ObjectId> tracks;
    int last = -1;

    if (const auto* list = trackIds.getArray())
        for (const auto& id : *list)
            if (const auto index = project->indexOfTrack (static_cast<ObjectId> (static_cast<juce::int64> (id)));
                index >= 0)
            {
                tracks.push_back (project->tracks[static_cast<size_t> (index)]->id);
                last = std::max (last, index);
            }

    if (tracks.empty())
        throw std::runtime_error ("Select the tracks to render");

    if (tracks.size() > AudioEngine::maxExportTracks)
        throw std::runtime_error ("Too many tracks to render at once");

    Ticks start = 0;
    Ticks end = 0;

    if (project->loop.enabled)
    {
        start = project->loop.start;
        end = project->loop.end;
    }
    else
    {
        std::vector<ClipPtr> clips;

        if (const auto* list = clipIds.getArray())
            for (const auto& id : *list)
                for (const auto& track : project->tracks)
                    if (const auto index = track->indexOfClip (static_cast<ObjectId> (static_cast<juce::int64> (id)));
                        index >= 0)
                        clips.push_back (track->clips[static_cast<size_t> (index)]);

        if (clips.empty())
            throw std::runtime_error ("Turn the loop on, or select clips, to choose the range to render");

        std::tie (start, end) = spanOf (clips);
    }

    const auto& first = *project->findTrack (tracks.front());
    const auto& lastTrack = *project->tracks[static_cast<size_t> (last)];
    const auto afterId = lastTrack.kind == TrackKind::returnTrack ? tracks.front() : lastTrack.id;
    const auto name = first.name + " render";
    TrackRender render;
    render.range = { static_cast<double> (start) / ticksPerBeat,
                     static_cast<double> (end) / ticksPerBeat,
                     true,
                     30.0,
                     tracks,
                     false,
                     {} };
    render.target = newRenderFile (m_projects->projectFolder(), "Bounces", name);
    render.label =
        "Rendering " + juce::String (static_cast<int> (tracks.size())) + (tracks.size() == 1 ? " track" : " tracks");
    render.done = [this, afterId, start, name] (const juce::File& file, juce::int64 frames)
    {
        const auto result =
            m_session.apply ("addRenderTrack", makeObject ({ { "after", afterId },
                                                             { "start", start },
                                                             { "name", name },
                                                             { "file", renderedFile (file, frames, name) } }));

        if (! result.ok)
            m_bridge.emit ("dawg_error", result.error);
    };
    startTrackRender (std::move (render));
}

void MainComponent::startRealtimeExport()
{
    const auto& job = m_export;
    const auto& pass = job->passes.front();
    auto writer = floatWriter (pass.render, job->sampleRate);

    if (writer == nullptr)
    {
        endExport ("Cannot write to " + pass.render.getParentDirectory().getFullPathName());
        return;
    }

    if (! m_recordThread.isThreadRunning())
        m_recordThread.startThread();

    constexpr int fifoFrames = 1 << 18;
    job->writer =
        std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), m_recordThread, fifoFrames);
    m_engine.stop();
    m_engine.setExportMode (true);
    m_engine.seek (job->range.startBeats);
    m_engine.setOutputCapture (job->writer.get());
    m_engine.play();
    m_bridge.emit ("dawg_export",
                   makeObject ({ { "state", "rendering" }, { "pass", 0 }, { "passes", 1 }, { "progress", 0.0 } }));
}

void MainComponent::pollRealtimeExport (float peak)
{
    if (m_export == nullptr || ! m_export->realtime || m_export->writer == nullptr)
        return;

    auto& job = *m_export;
    const auto position = m_engine.positionBeats();
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    auto done = job.cancel.load();

    if (! done && position >= job.range.endBeats)
    {
        // The tail: fixed, or until the output stays quiet for a second (X2).
        if (job.tailStarted < 0.0)
            job.tailStarted = now;

        if (peak >= quietPeak)
            job.quietSince = -1.0;
        else if (job.quietSince < 0.0)
            job.quietSince = now;

        done = now - job.tailStarted >= job.range.tailSeconds
               || (job.range.autoTail && job.quietSince >= 0.0 && now - job.quietSince >= realtimeQuietSeconds);
    }

    const auto beats = job.range.endBeats - job.range.startBeats;
    m_bridge.emit ("dawg_export",
                   makeObject ({ { "state", "rendering" },
                                 { "pass", 0 },
                                 { "passes", 1 },
                                 { "progress", std::clamp ((position - job.range.startBeats) / beats, 0.0, 1.0) } }));

    if (! done)
        return;

    m_engine.setOutputCapture (nullptr);
    m_engine.stop();
    m_engine.setExportMode (false);

    // The audio thread may still be writing one block: close the file a little later.
    juce::Timer::callAfterDelay (150,
                                 [this, safe = juce::Component::SafePointer<MainComponent> (this)]
                                 {
                                     if (safe == nullptr || m_export == nullptr)
                                         return;

                                     m_export->writer.reset();
                                     m_engine.seek (m_export->previousPosition);

                                     if (m_export->cancel.load())
                                         endExport ({});
                                     else
                                         finaliseExport();
                                 });
}

void MainComponent::finaliseExport()
{
    auto job = m_export;
    m_bridge.emit ("dawg_export", makeObject ({ { "state", "writing" } }));
    job->running.store (true);

    if (job->worker.joinable())
        job->worker.join(); // the render pass, which has just posted this call

    // Measure each render, then write it in the final format (X4, X5), off the message thread.
    job->worker = std::thread (
        [this, job, safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::String error;
            juce::Array<juce::var> results;

            for (const auto& pass : job->passes)
            {
                if (job->cancel.load())
                    break;

                const auto measured = measureRender (pass.render, job->cancel);

                if (measured.cancelled)
                    break;

                if (measured.error.isNotEmpty())
                {
                    error = measured.error;
                    break;
                }

                const auto gain = job->normalise && measured.peak > 0.0
                                      ? std::pow (10.0, job->normaliseDb / 20.0) / measured.peak
                                      : 1.0;
                // A stem's MP3 is titled after its own file unless a title was typed (X6).
                auto mp3 = job->mp3;

                if (pass.stemTrack != 0 && mp3.tags.title.trim().isNotEmpty())
                    mp3.tags.title =
                        mp3.tags.title.trim() + " - "
                        + pass.target.getFileNameWithoutExtension().fromLastOccurrenceOf (" - ", false, false);

                error = writeExport (pass.render, pass.target, job->format, static_cast<float> (gain), mp3);
                std::ignore = pass.render.deleteFile();

                if (error.isNotEmpty())
                    break;

                // An MP3's report is of the file as written: encoding can raise the peak (X6).
                auto peak = measured.peak * gain;
                auto lufs = measured.lufs + toDb (gain);

                if (job->format == ExportFormat::mp3)
                {
                    if (const auto written = measureFile (pass.target))
                    {
                        peak = written->peak;
                        lufs = written->lufs;
                    }
                }

                results.add (makeObject ({ { "name", pass.target.getFileName() },
                                           { "path", pass.target.getFullPathName() },
                                           { "peakDb", toDb (peak) },
                                           { "lufs", lufs } }));
            }

            job->running.store (false);

            juce::MessageManager::callAsync (
                [this, safe, error, results]
                {
                    if (safe == nullptr)
                        return;

                    m_export->results = results;
                    endExport (error);
                });
        });
}

void MainComponent::endExport (const juce::String& error)
{
    if (m_export == nullptr)
        return;

    for (const auto& pass : m_export->passes)
        std::ignore = pass.render.deleteFile();

    m_bridge.emit (m_export->event, makeObject ({ { "state", endStateOf (error.isNotEmpty(), m_export->cancel.load()) },
                                                  { "error", error },
                                                  { "files", m_export->results } }));
    const auto ownProject = m_export->project.has_value();
    m_export.reset();

    if (ownProject)
        recompileEngine(); // back to the session's project
}

} // namespace dawg
