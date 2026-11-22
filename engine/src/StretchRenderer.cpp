#include "dawg/StretchRenderer.h"

#include "dawg/PitchShifter.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace dawg
{

namespace
{
class Worker final : public juce::Thread
{
public:
    explicit Worker (std::function<void()> body)
        : juce::Thread ("DAWg stretch"),
          m_body (std::move (body))
    {
    }

    void run() override { m_body(); }

private:
    std::function<void()> m_body;
};

constexpr int wavBits = 32;
constexpr double pitchPadSeconds = 0.05;  // around a phrase, so its first and last grains are whole
constexpr double pitchHopSeconds = 0.005; // the correction is sampled this often for the shifter

// A file's correction at the pitch hop from `start`, quantised to 0.1 cent: what a pitch render's key hashes.
[[nodiscard]] juce::String correctionKey (const AudioFile& file, juce::int64 start, juce::int64 frames,
                                          const PitchTrack* track)
{
    const auto hop = std::max (1, static_cast<int> (std::lround (pitchHopSeconds * file.sampleRate)));
    const auto values =
        pitchCorrection (file.pitch, file.sampleRate, start, static_cast<size_t> (frames / hop + 1), hop, track);
    juce::String text;

    for (const auto v : values)
        text << std::lround (v * 1000.0) << ",";

    return text;
}
} // namespace

StretchRenderer::StretchRenderer (juce::File folder, std::shared_ptr<const Stretcher> stretcher, Input input,
                                  Identify identify, int threads)
    : m_folder (std::move (folder)),
      m_stretcher (std::move (stretcher)),
      m_input (std::move (input)),
      m_identify (std::move (identify))
{
    m_formats.registerFormat (std::make_unique<juce::WavAudioFormat>().release(), true); // the manager owns it
    m_readAhead.startThread();

    for (int i = 0; i < std::max (1, threads); ++i)
    {
        m_workers.push_back (std::make_unique<Worker> ([this] { work(); }));
        // Below the audio thread's priority (TS9).
        m_workers.back()->startThread (juce::Thread::Priority::low);
    }
}

StretchRenderer::~StretchRenderer()
{
    {
        const std::scoped_lock lock (m_mutex);
        m_stopping = true;
        m_pending.clear();
    }

    m_wake.notify_all();

    for (auto& worker : m_workers)
        worker->stopThread (-1);

    m_readAhead.stopThread (2000);
}

void StretchRenderer::setReadyCallback (Ready ready)
{
    const std::scoped_lock lock (m_mutex);
    m_ready = std::move (ready);
}

void StretchRenderer::setPitchTracks (PitchTrackOf tracks)
{
    const std::scoped_lock lock (m_mutex);
    m_pitchTracks = std::move (tracks);
}

std::shared_ptr<const PitchTrack> StretchRenderer::trackOf (const AudioFile& file) const
{
    PitchTrackOf tracks;
    {
        const std::scoped_lock lock (m_mutex);
        tracks = m_pitchTracks;
    }

    return tracks != nullptr ? tracks (file) : nullptr;
}

// The renders of a file's phrases (docs/design/pitch-correction.md, section 5.2), each padded a little.
std::vector<StretchJob> StretchRenderer::pitchJobsOf (const AudioFile& file) const
{
    std::vector<StretchJob> jobs;

    if (! file.pitch.edited())
        return jobs;

    const auto track = trackOf (file);

    if (track == nullptr)
        return jobs;

    const auto pad = static_cast<juce::int64> (pitchPadSeconds * file.sampleRate);

    for (const auto& phrase : pitchPhrases (file.pitch, file.sampleRate, track.get()))
    {
        StretchJob job;
        job.file = file;
        job.pitch = true;
        job.inputStart = std::max<juce::int64> (0, phrase.start - pad);
        job.inputFrames = std::min (file.frames, phrase.end + pad) - job.inputStart;
        job.spec.sampleRate = file.sampleRate;
        job.spec.outputFrames = job.inputFrames;
        jobs.push_back (std::move (job));
    }

    return jobs;
}

std::vector<AudioSource::Overlay> StretchRenderer::pitchOverlays (const AudioFile& file, ObjectId clipId, int& pending)
{
    std::vector<AudioSource::Overlay> overlays;
    auto prepared = file;

    if (m_identify != nullptr)
        std::ignore = m_identify (prepared);

    // Each render covers its padding too: PSOLA leaves audio it doesn't shift exactly as it was.
    for (const auto& job : pitchJobsOf (prepared))
    {
        if (auto render = find (job, clipId))
            overlays.push_back (
                { job.inputStart, job.inputStart + job.inputFrames, job.inputStart, std::move (render) });
        else
            ++pending;
    }

    return overlays;
}

std::vector<StretchJob> StretchRenderer::pitchJobs (const Project& project)
{
    std::vector<StretchJob> jobs;

    for (auto file : project.audioFiles)
    {
        if (! file.pitch.edited())
            continue;

        if (m_identify != nullptr)
            std::ignore = m_identify (file);

        for (auto& job : pitchJobsOf (file))
            jobs.push_back (std::move (job));
    }

    return jobs;
}

juce::String StretchRenderer::keyOf (const StretchJob& job) const
{
    auto copy = job;
    return prepare (copy);
}

juce::String StretchRenderer::prepare (StretchJob& job) const
{
    juce::String text;

    if (job.pitch)
    {
        // A pitch render: the file, the range and the correction over it (section 5.2).
        const auto identity = m_identify != nullptr ? m_identify (job.file) : job.file.path;
        const auto track = trackOf (job.file);
        text << "psola 1|" << identity << "|" << job.file.sampleRate << "|" << job.file.channels << "|"
             << job.inputStart << "|" << job.inputFrames << "|"
             << correctionKey (job.file, job.inputStart, job.inputFrames, track.get());
        return juce::String::toHexString (text.hashCode64())
               + juce::String::toHexString (static_cast<juce::int64> (std::hash<std::string> {}(text.toStdString())));
    }

    text << m_stretcher->identity() << "|" << (m_identify != nullptr ? m_identify (job.file) : job.file.path) << "|"
         << job.file.sampleRate << "|" << job.file.channels << "|" << job.inputStart << "|" << job.inputFrames << "|"
         << job.spec.outputFrames << "|" << juce::String (job.spec.pitchScale, 12) << "|";

    for (const auto& [in, out] : job.spec.keyFrames)
        text << in << ":" << out << ",";

    // Only when off, so renders from before the choice existed keep their keys.
    if (! job.spec.formants)
        text << "|formants shifted";

    // A corrected file is stretched from its corrected audio: the key holds the correction over the input.
    if (job.file.pitch.edited())
    {
        const auto track = trackOf (job.file);
        text << "|pitch " << correctionKey (job.file, job.inputStart, job.inputFrames, track.get());
    }

    // Two different hash functions (JUCE's, and the standard library's) make a collision practically impossible.
    // (juce::String::hash() is the same function as hashCode64() on 64-bit builds.)
    return juce::String::toHexString (text.hashCode64())
           + juce::String::toHexString (static_cast<juce::int64> (std::hash<std::string> {}(text.toStdString())));
}

juce::File StretchRenderer::fileFor (const juce::String& key) const
{
    return m_folder.getChildFile (key + ".wav");
}

std::shared_ptr<const AudioSource> StretchRenderer::openCached (const juce::String& key)
{
    if (auto open = m_open[key].lock())
        return open;

    const auto file = fileFor (key);

    if (! file.existsAsFile())
        return nullptr;

    auto source = AudioSource::open (file, m_formats, m_readAhead);

    if (source != nullptr)
    {
        std::ignore = file.setLastAccessTime (juce::Time::getCurrentTime());
        m_open[key] = source;
    }

    return source;
}

std::shared_ptr<const AudioSource> StretchRenderer::lookup (const StretchJob& job)
{
    const auto key = keyOf (job);
    const std::scoped_lock lock (m_mutex);
    return openCached (key);
}

double StretchRenderer::cached (const juce::String& key, const std::function<double()>& compute)
{
    {
        const std::scoped_lock lock (m_mutex);

        if (const auto it = m_values.find (key); it != m_values.end())
            return it->second;
    }

    const auto value = compute();
    const std::scoped_lock lock (m_mutex);

    // A small cache: forget it all now and then rather than tracking use.
    if (m_values.size() > 100000)
        m_values.clear();

    m_values[key] = value;
    return value;
}

std::shared_ptr<const AudioSource> StretchRenderer::find (const StretchJob& request, ObjectId clipId,
                                                          juce::String* keyOut)
{
    auto job = request;
    const auto key = prepare (job);

    if (keyOut != nullptr)
        *keyOut = key;
    {
        const std::scoped_lock lock (m_mutex);

        if (auto source = openCached (key))
            return source;

        const auto it = std::ranges::find (m_pending, key, &Pending::key);

        if (it != m_pending.end())
        {
            m_cancelled.erase (key); // wanted again after all
            it->clips.insert (clipId);
            it->requested = true;
            it->job.seconds = std::min (it->job.seconds, job.seconds);
            return nullptr;
        }

        m_pending.push_back ({ job, key, { clipId }, true, false });
    }

    m_wake.notify_one();
    return nullptr;
}

void StretchRenderer::setHeld (bool held)
{
    {
        const std::scoped_lock lock (m_mutex);
        m_heldUntil = held ? std::chrono::steady_clock::now() + std::chrono::seconds (maxHoldSeconds)
                           : std::chrono::steady_clock::time_point {};
    }

    m_wake.notify_all();
}

void StretchRenderer::beginRequests()
{
    const std::scoped_lock lock (m_mutex);
    m_quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds (quietMilliseconds);

    for (auto& pending : m_pending)
        pending.requested = false;
}

void StretchRenderer::endRequests()
{
    {
        const std::scoped_lock lock (m_mutex);

        for (const auto& pending : m_pending)
            if (! pending.requested && pending.running)
                m_cancelled.insert (pending.key);

        std::erase_if (m_pending, [] (const Pending& p) { return ! p.requested && ! p.running; });
        m_idle.notify_all();
    }

    m_wake.notify_all(); // the quiet time may have changed
}

void StretchRenderer::setPlayhead (double seconds)
{
    const std::scoped_lock lock (m_mutex);
    m_playhead = seconds;
}

void StretchRenderer::setSizeLimit (juce::int64 bytes)
{
    {
        const std::scoped_lock lock (m_mutex);
        m_sizeLimit = std::max (juce::int64 { 0 }, bytes);
    }

    trim();
}

void StretchRenderer::clear()
{
    const std::scoped_lock lock (m_mutex);

    for (const auto& file : m_folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
        std::ignore = file.deleteFile(); // renders being played stay until released

    m_open.clear();
}

int StretchRenderer::pendingCount() const
{
    const std::scoped_lock lock (m_mutex);
    return static_cast<int> (m_pending.size());
}

std::vector<ObjectId> StretchRenderer::pendingClips() const
{
    const std::scoped_lock lock (m_mutex);
    std::set<ObjectId> clips;

    for (const auto& pending : m_pending)
        clips.insert (pending.clips.begin(), pending.clips.end());

    return { clips.begin(), clips.end() };
}

bool StretchRenderer::waitUntilIdle (int milliseconds)
{
    std::unique_lock lock (m_mutex);
    return m_idle.wait_for (lock, std::chrono::milliseconds (milliseconds), [this] { return m_pending.empty(); });
}

juce::int64 StretchRenderer::cacheSize() const
{
    juce::int64 total = 0;

    for (const auto& file : m_folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
        total += file.getSize();

    return total;
}

void StretchRenderer::work()
{
    auto* thread = juce::Thread::getCurrentThread();

    while (true)
    {
        Pending task;
        {
            std::unique_lock lock (m_mutex);

            // Wait for a render to do, then for the drag holding renders back and the quiet time after the last
            // edit (TS9: only what the user settles on is rendered).
            while (true)
            {
                if (m_stopping || (thread != nullptr && thread->threadShouldExit()))
                    return;

                if (std::ranges::none_of (m_pending, [] (const Pending& p) { return ! p.running; }))
                {
                    m_wake.wait (lock);
                    continue;
                }

                const auto until = std::max (m_heldUntil, m_quietUntil);

                if (std::chrono::steady_clock::now() < until)
                {
                    m_wake.wait_until (lock, until);
                    continue;
                }

                break;
            }

            // The waiting render nearest the playhead first, preferring those ahead of it (TS9).
            auto best = m_pending.end();
            auto bestDistance = 0.0;

            for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
            {
                if (it->running)
                    continue;

                const auto ahead = it->job.seconds - m_playhead;
                const auto distance = ahead >= -1.0 ? ahead : 1.0e6 - ahead;

                if (best == m_pending.end() || distance < bestDistance)
                {
                    best = it;
                    bestDistance = distance;
                }
            }

            best->running = true;
            task = *best;
        }

        const auto ok = renderOne (task.job, task.key);
        Ready ready;
        std::vector<ObjectId> clips;
        {
            const std::scoped_lock lock (m_mutex);
            std::erase_if (m_pending, [&] (const Pending& p) { return p.key == task.key; });
            m_cancelled.erase (task.key);

            if (ok)
            {
                ready = m_ready;
                clips.assign (task.clips.begin(), task.clips.end());
            }

            m_idle.notify_all();
        }

        if (ok)
            trim();

        if (ready != nullptr)
            ready (clips);
    }
}

bool StretchRenderer::renderOne (const StretchJob& job, const juce::String& key)
{
    const auto target = fileFor (key);

    if (target.existsAsFile())
        return true; // another round rendered it meanwhile

    const auto channels = std::clamp (job.file.channels, 1, 2);
    juce::AudioBuffer<float> input (channels, static_cast<int> (std::max (juce::int64 { 1 }, job.inputFrames)));

    if (m_input == nullptr || ! m_input (job.file, job.inputStart, input))
        return false;

    const auto cancelled = [this, &key]
    {
        const std::scoped_lock lock (m_mutex);
        return m_stopping || m_cancelled.contains (key);
    };

    if (! job.pitch && ! applyPitch (job, input))
        return false;

    auto output = job.pitch ? renderPitch (job, input, cancelled) : m_stretcher->render (input, job.spec, cancelled);

    if (output.getNumSamples() == 0 && job.spec.outputFrames > 0)
        return false;

    std::ignore = m_folder.createDirectory();
    const auto temporary = target.getSiblingFile (key + "-" + juce::Uuid().toString() + ".tmp");
    {
        std::unique_ptr<juce::OutputStream> stream (temporary.createOutputStream());

        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate (job.file.sampleRate)
                                 .withNumChannels (output.getNumChannels())
                                 .withBitsPerSample (wavBits)
                                 .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor (stream, options);

        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples()))
            return false;
    }

    if (! temporary.moveFileTo (target))
    {
        // Another thread may have rendered the same job meanwhile.
        std::ignore = temporary.deleteFile();
        return target.existsAsFile();
    }

    ++m_rendered;
    return true;
}

// A phrase shifted by its correction with PSOLA (docs/design/pitch-correction.md, section 5.1).
juce::AudioBuffer<float> StretchRenderer::renderPitch (const StretchJob& job, const juce::AudioBuffer<float>& input,
                                                       const std::function<bool()>& cancelled)
{
    const auto track = trackOf (job.file);

    if (track == nullptr)
        return {};

    // The track and the correction, both relative to the input's first frame.
    auto local = *track;
    local.startSeconds -= static_cast<double> (job.inputStart) / track->sampleRate;
    const auto hop = std::max (1, static_cast<int> (std::lround (pitchHopSeconds * job.file.sampleRate)));
    const auto delta = pitchCorrection (job.file.pitch, job.file.sampleRate, job.inputStart,
                                        static_cast<size_t> (job.inputFrames / hop + 2), hop, track.get());
    std::vector<float> ratio (delta.size());
    std::ranges::transform (delta, ratio.begin(), [] (float d) { return std::exp2 (d / 12.0f); });

    return makePsolaPitchShifter (std::move (local))->render (input, job.file.sampleRate, ratio, hop, cancelled);
}

// Puts a corrected file's phrase renders into a stretch job's input (section 5.2), rendering any not cached yet.
bool StretchRenderer::applyPitch (const StretchJob& job, juce::AudioBuffer<float>& input)
{
    if (! job.file.pitch.edited())
        return true;

    const auto end = job.inputStart + job.inputFrames;

    for (auto pitchJob : pitchJobsOf (job.file))
    {
        if (pitchJob.inputStart >= end || pitchJob.inputStart + pitchJob.inputFrames <= job.inputStart)
            continue;

        const auto key = prepare (pitchJob);
        std::shared_ptr<const AudioSource> render;
        {
            const std::scoped_lock lock (m_mutex);
            render = openCached (key);
        }

        if (render == nullptr)
        {
            if (! renderOne (pitchJob, key))
                return false;

            const std::scoped_lock lock (m_mutex);
            render = openCached (key);
        }

        if (render == nullptr)
            return false;

        const auto first = std::max (job.inputStart, pitchJob.inputStart);
        const auto last = std::min (end, pitchJob.inputStart + pitchJob.inputFrames);
        juce::AudioBuffer<float> part (2, static_cast<int> (last - first));

        if (! render->readBlocking ({ part.getWritePointer (0), part.getWritePointer (1) }, first - pitchJob.inputStart,
                                    part.getNumSamples()))
            return false;

        for (int c = 0; c < input.getNumChannels(); ++c)
            input.copyFrom (c, static_cast<int> (first - job.inputStart), part, c, 0, part.getNumSamples());
    }

    return true;
}

bool StretchRenderer::renderNow (const std::vector<StretchJob>& jobs, const std::function<bool (double)>& progress)
{
    std::vector<std::pair<StretchJob, juce::String>> missing;

    for (auto job : jobs)
    {
        auto key = prepare (job);
        const std::scoped_lock lock (m_mutex);

        if (openCached (key) == nullptr)
            missing.emplace_back (std::move (job), std::move (key));
    }

    for (size_t i = 0; i < missing.size(); ++i)
    {
        if (progress != nullptr && ! progress (static_cast<double> (i) / static_cast<double> (missing.size())))
            return false;

        if (! renderOne (missing[i].first, missing[i].second))
            return false;
    }

    trim();
    return progress == nullptr || progress (1.0);
}

void StretchRenderer::trim()
{
    auto files = m_folder.findChildFiles (juce::File::findFiles, false, "*.wav");
    juce::int64 total = 0;

    for (const auto& file : files)
        total += file.getSize();

    juce::int64 limit = 0;
    {
        const std::scoped_lock lock (m_mutex);
        limit = m_sizeLimit;
    }

    if (total <= limit)
        return;

    // Least recently used first.
    std::ranges::sort (files, [] (const juce::File& a, const juce::File& b)
                       { return a.getLastAccessTime() < b.getLastAccessTime(); });

    for (const auto& file : files)
    {
        if (total <= limit)
            break;

        const auto size = file.getSize();
        const auto key = file.getFileNameWithoutExtension();
        {
            const std::scoped_lock lock (m_mutex);

            if (m_open[key].lock() != nullptr)
                continue; // in use
        }

        if (file.deleteFile())
            total -= size;
    }
}

} // namespace dawg
