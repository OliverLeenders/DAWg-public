#include "AudioLibrary.h"

#include "dawg/ClipOps.h"
#include "dawg/Onsets.h"
#include "dawg/PitchAnalysis.h"
#include "dawg/VarUtils.h"

namespace dawg
{

namespace
{
constexpr int framesPerPoint = 256;
constexpr int levelFactor = 4;
constexpr int minPoints = 64;
constexpr int readChunk = 65536;
constexpr int wavBits = 24;
constexpr const char* pitchModelFile = "fcpe.onnx"; // the shipped model (docs/design/pitch-correction.md, question 12)

[[nodiscard]] juce::File uniqueFileIn (const juce::File& folder, const juce::String& name,
                                       const juce::String& extension)
{
    auto file = folder.getChildFile (name + extension);

    for (int i = 2; file.exists(); ++i)
        file = folder.getChildFile (name + " (" + juce::String (i) + ")" + extension);

    return file;
}
} // namespace

AudioLibrary::AudioLibrary (std::function<juce::File()> projectFolder, juce::File cacheFolder,
                            std::function<void (const juce::String&)> peaksReady)
    : m_projectFolder (std::move (projectFolder)),
      m_cacheFolder (std::move (cacheFolder)),
      m_peaksReady (std::move (peaksReady))
{
    m_formats.registerBasicFormats();
    m_readAhead.startThread();
}

AudioLibrary::~AudioLibrary()
{
    *m_alive = false;
    m_jobs.removeAllJobs (true, 10000);
    m_readAhead.stopThread (2000);
}

juce::File AudioLibrary::locate (const AudioFile& file) const
{
    return juce::File::isAbsolutePath (file.path) ? juce::File (file.path) : m_projectFolder().getChildFile (file.path);
}

AudioSourceResolver AudioLibrary::resolver()
{
    return [this] (const AudioFile& file) -> std::shared_ptr<const AudioSource>
    {
        const auto location = locate (file);
        const auto& key = location.getFullPathName();

        if (auto existing = m_sources[key].lock())
            return existing;

        if (! location.existsAsFile())
            return nullptr;

        auto source = AudioSource::open (location, m_formats, m_readAhead);
        m_sources[key] = source;
        return source;
    };
}

StretchRenderer::Input AudioLibrary::stretchInput()
{
    return [this] (const AudioFile& file, juce::int64 start, juce::AudioBuffer<float>& into)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (juce::File (file.path)));

        if (reader == nullptr)
            return false;

        // Frames outside the file read as silence.
        return reader->read (&into, 0, into.getNumSamples(), start, true, true);
    };
}

juce::String AudioLibrary::identify (AudioFile& file) const
{
    const auto location = locate (file);
    file.path = location.getFullPathName();
    return file.path + "|" + juce::String (location.getSize()) + "|"
           + juce::String (location.getLastModificationTime().toMilliseconds());
}

std::optional<AudioLibrary::Imported> AudioLibrary::importOne (const juce::File& source, const juce::File& audioFolder,
                                                               bool analyse)
{
    std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (source));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return std::nullopt;

    const auto keepFormat = source.hasFileExtension ("wav;aif;aiff;flac");
    const auto target = uniqueFileIn (audioFolder, source.getFileNameWithoutExtension(),
                                      keepFormat ? source.getFileExtension() : juce::String (".wav"));
    auto ok = false;

    if (keepFormat)
    {
        ok = source.copyFileTo (target);
    }
    else
    {
        // Decode once to WAV, so playback and seeking stay cheap (T5).
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate (reader->sampleRate)
                                 .withNumChannels (static_cast<int> (reader->numChannels))
                                 .withBitsPerSample (wavBits);

        if (stream != nullptr)
            if (auto writer = wav.createWriterFor (stream, options))
                ok = writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples);
    }

    if (! ok)
    {
        std::ignore = target.deleteFile();
        return std::nullopt;
    }

    // A tempo from the metadata, for the source tempo suggestion (TS11).
    auto metadataBpm = reader->metadataValues.getValue (juce::WavAudioFormat::acidTempo, "0").getDoubleValue();

    if (const auto beats = reader->metadataValues.getValue (juce::AiffAudioFormat::appleBeats, "0").getDoubleValue();
        metadataBpm <= 0.0 && beats > 0.0)
        metadataBpm = beats * 60.0 * reader->sampleRate / static_cast<double> (reader->lengthInSamples);

    Imported imported { "Audio/" + target.getFileName(),
                        source.getFileNameWithoutExtension(),
                        source.getFullPathName(),
                        reader->sampleRate,
                        static_cast<int> (reader->numChannels),
                        reader->lengthInSamples,
                        metadataBpm,
                        {},
                        -1,
                        {} };

    // The note, title and loop the file stores, for mapping (docs/design/sampler-mapping.md, SM5a).
    const auto& meta = reader->metadataValues;

    if (meta.containsKey ("MidiUnityNote"))
        imported.storedKey = std::clamp (meta.getValue ("MidiUnityNote", "60").getIntValue(), 0, 127);

    imported.title = meta.getValue (juce::WavAudioFormat::riffInfoTitle, {}).trim();

    if (meta.getValue ("NumSampleLoops", "0").getIntValue() > 0 && meta.containsKey ("Loop0Start"))
    {
        const auto start = meta.getValue ("Loop0Start", "0").getLargeIntValue();
        const auto end = meta.getValue ("Loop0End", "0").getLargeIntValue() + 1; // the chunk's end is inclusive

        if (start >= 0 && end > start + 1 && end <= reader->lengthInSamples)
        {
            imported.loopStart = start;
            imported.loopEnd = end;
        }
    }

    // Its slots and pitches, from its first ten minutes (docs/design/sampler-mapping.md, SM2).
    if (analyse && reader->sampleRate > 0.0)
    {
        constexpr double longest = 600.0;
        const auto frames = static_cast<int> (
            std::min<juce::int64> (reader->lengthInSamples, static_cast<juce::int64> (longest * reader->sampleRate)));
        juce::AudioBuffer<float> audio (static_cast<int> (std::min (2u, reader->numChannels)), frames);

        if (reader->read (&audio, 0, frames, 0, true, audio.getNumChannels() > 1))
            imported.slices = analyseSample (audio, reader->sampleRate);
    }

    return imported;
}

void AudioLibrary::importFiles (std::vector<juce::File> files, ImportDone done, bool analyse)
{
    const auto audioFolder = m_projectFolder().getChildFile ("Audio");
    const std::weak_ptr<bool> alive = m_alive;

    m_jobs.addJob (
        [this, jobFiles = std::move (files), audioFolder, onDone = std::move (done), alive, analyse]
        {
            std::vector<Imported> imported;
            juce::StringArray failed;
            std::ignore = audioFolder.createDirectory();

            for (const auto& source : jobFiles)
            {
                if (auto result = importOne (source, audioFolder, analyse))
                    imported.push_back (std::move (*result));
                else
                    failed.add (source.getFileName());
            }

            juce::MessageManager::callAsync (
                [alive, onDone, results = std::move (imported), failed]
                {
                    if (const auto a = alive.lock(); a != nullptr && *a)
                        onDone (results, failed);
                });
        });
}

void AudioLibrary::reverseFiles (const std::vector<AudioFile>& sources, ReverseDone done)
{
    const auto audioFolder = m_projectFolder().getChildFile ("Audio");
    const std::weak_ptr<bool> alive = m_alive;

    // Absolute paths now: locate() reads the project folder, which belongs to the message thread.
    std::vector<std::pair<AudioFile, juce::File>> jobs;
    jobs.reserve (sources.size());

    for (const auto& source : sources)
        jobs.emplace_back (source, locate (source));

    m_jobs.addJob (
        [this, jobFiles = std::move (jobs), audioFolder, onDone = std::move (done), alive]
        {
            std::map<ObjectId, Imported> written;
            juce::StringArray failed;
            std::ignore = audioFolder.createDirectory();

            for (const auto& [entry, source] : jobFiles)
            {
                const std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (source));
                const auto target =
                    uniqueFileIn (audioFolder, juce::File::createLegalFileName (entry.name + " (reversed)"), ".wav");

                if (reader != nullptr && writeReversed (*reader, target))
                {
                    written[entry.id] = Imported { "Audio/" + target.getFileName(),
                                                   target.getFileNameWithoutExtension(),
                                                   source.getFullPathName(),
                                                   reader->sampleRate,
                                                   static_cast<int> (reader->numChannels),
                                                   reader->lengthInSamples,
                                                   0.0,
                                                   {},
                                                   -1,
                                                   {} };
                }
                else
                {
                    std::ignore = target.deleteFile();
                    failed.add (entry.name);
                }
            }

            juce::MessageManager::callAsync (
                [alive, onDone, results = std::move (written), failed]
                {
                    if (const auto a = alive.lock(); a != nullptr && *a)
                        onDone (results, failed);
                });
        });
}

void AudioLibrary::peaks (std::vector<PeakRequest> requests, std::function<void (std::vector<float>)> done)
{
    const std::weak_ptr<bool> alive = m_alive;

    m_jobs.addJob (
        [this, jobs = std::move (requests), onDone = std::move (done), alive]
        {
            std::vector<float> result;

            for (const auto& request : jobs)
            {
                const std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (request.file));
                auto peak = 0.0f;

                if (reader != nullptr)
                {
                    const auto channels = static_cast<int> (reader->numChannels);
                    std::vector<juce::Range<float>> levels (static_cast<size_t> (channels));

                    for (const auto& [from, to] : request.ranges)
                    {
                        reader->readMaxLevels (from, to - from, levels.data(), channels);

                        for (const auto& level : levels)
                            peak = std::max ({ peak, std::abs (level.getStart()), std::abs (level.getEnd()) });
                    }
                }

                result.push_back (peak);
            }

            juce::MessageManager::callAsync (
                [alive, onDone, peaks = std::move (result)]
                {
                    if (const auto a = alive.lock(); a != nullptr && *a)
                        onDone (peaks);
                });
        });
}

void AudioLibrary::analyse (const juce::File& file, juce::int64 start, juce::int64 count,
                            std::function<juce::var (const juce::AudioBuffer<float>&, double)> work,
                            std::function<void (juce::var)> done)
{
    const std::weak_ptr<bool> alive = m_alive;

    m_jobs.addJob (
        [this, file, start, count, job = std::move (work), onDone = std::move (done), alive]
        {
            juce::var result;

            if (const std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));
                reader != nullptr && count > 0 && count < std::numeric_limits<int>::max())
            {
                juce::AudioBuffer<float> audio (static_cast<int> (reader->numChannels), static_cast<int> (count));
                audio.clear();

                if (reader->read (&audio, 0, static_cast<int> (count), start, true, true))
                    result = job (audio, reader->sampleRate);
            }

            juce::MessageManager::callAsync (
                [alive, onDone, result]
                {
                    if (const auto a = alive.lock(); a != nullptr && *a)
                        onDone (result);
                });
        });
}

juce::File AudioLibrary::peakCacheFile (const juce::File& file) const
{
    // Keyed by path, size and modification time, so a changed file gets new peaks.
    const auto key = file.getFullPathName() + "|" + juce::String (file.getSize()) + "|"
                     + juce::String (file.getLastModificationTime().toMilliseconds());
    return m_cacheFolder.getChildFile (juce::String::toHexString (key.hashCode64()) + ".peaks");
}

namespace
{
// The cache file: channels, frames, sample rate, then each level's size and bytes.
[[nodiscard]] bool readPeakCache (const juce::File& cache, int& channels, juce::int64& frames, double& sampleRate,
                                  std::vector<std::vector<juce::int8>>& levels)
{
    juce::FileInputStream in (cache);

    if (! in.openedOk())
        return false;

    channels = in.readInt();
    frames = in.readInt64();
    sampleRate = in.readDouble();

    for (auto count = in.readInt(); count > 0 && ! in.isExhausted(); --count)
    {
        std::vector<juce::int8> level (static_cast<size_t> (in.readInt()));
        in.read (level.data(), static_cast<int> (level.size()));
        levels.push_back (std::move (level));
    }

    return channels > 0 && ! levels.empty();
}

void writePeakCache (const juce::File& cache, int channels, juce::int64 frames, double sampleRate,
                     const std::vector<std::vector<juce::int8>>& levels)
{
    std::ignore = cache.getParentDirectory().createDirectory();
    juce::FileOutputStream out (cache);

    if (! out.openedOk())
        return;

    std::ignore = out.setPosition (0);
    std::ignore = out.truncate();
    std::ignore = out.writeInt (channels);
    std::ignore = out.writeInt64 (frames);
    std::ignore = out.writeDouble (sampleRate);
    std::ignore = out.writeInt (static_cast<int> (levels.size()));

    for (const auto& level : levels)
    {
        std::ignore = out.writeInt (static_cast<int> (level.size()));
        std::ignore = out.write (level.data(), level.size());
    }
}

// One coarser level: each point combines levelFactor points of the level below.
[[nodiscard]] std::vector<juce::int8> coarser (const std::vector<juce::int8>& below, size_t channels)
{
    const auto belowPoints = below.size() / (channels * 2);
    const auto abovePoints = (belowPoints + levelFactor - 1) / levelFactor;
    std::vector<juce::int8> above (abovePoints * channels * 2);

    for (size_t p = 0; p < abovePoints; ++p)
    {
        for (size_t ch = 0; ch < channels; ++ch)
        {
            juce::int8 lo = 127;
            juce::int8 hi = -127;

            for (size_t k = p * levelFactor; k < std::min (belowPoints, (p + 1) * levelFactor); ++k)
            {
                lo = std::min (lo, below[(k * channels + ch) * 2]);
                hi = std::max (hi, below[(k * channels + ch) * 2 + 1]);
            }

            above[(p * channels + ch) * 2] = lo;
            above[(p * channels + ch) * 2 + 1] = hi;
        }
    }

    return above;
}
} // namespace

std::shared_ptr<const AudioLibrary::Peaks> AudioLibrary::computePeaks (const juce::File& file)
{
    auto peaks = std::make_shared<Peaks>();
    const auto cache = peakCacheFile (file);

    if (readPeakCache (cache, peaks->channels, peaks->frames, peaks->sampleRate, peaks->levels))
        return peaks;

    peaks = std::make_shared<Peaks>();
    std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));

    if (reader == nullptr)
        return nullptr;

    peaks->channels = std::min (2, static_cast<int> (reader->numChannels));
    peaks->frames = reader->lengthInSamples;
    peaks->sampleRate = reader->sampleRate;

    const auto channels = static_cast<size_t> (peaks->channels);
    const auto points = static_cast<size_t> ((reader->lengthInSamples + framesPerPoint - 1) / framesPerPoint);
    std::vector<juce::int8> level0 (points * channels * 2);
    juce::AudioBuffer<float> buffer (peaks->channels, readChunk);
    const auto toByte = [] (float v)
    { return static_cast<juce::int8> (juce::jlimit (-127, 127, juce::roundToInt (v * 127.0f))); };

    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += readChunk)
    {
        const auto count = static_cast<int> (std::min<juce::int64> (readChunk, reader->lengthInSamples - pos));
        std::ignore = reader->read (&buffer, 0, count, pos, true, peaks->channels > 1);

        for (int i = 0; i < count; i += framesPerPoint)
        {
            const auto point = static_cast<size_t> ((pos + i) / framesPerPoint);

            for (size_t ch = 0; ch < channels; ++ch)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (
                    buffer.getReadPointer (static_cast<int> (ch), i), std::min (framesPerPoint, count - i));
                level0[(point * channels + ch) * 2] = toByte (range.getStart());
                level0[(point * channels + ch) * 2 + 1] = toByte (range.getEnd());
            }
        }
    }

    peaks->levels.push_back (std::move (level0));

    while (peaks->levels.back().size() / (channels * 2) > minPoints)
        peaks->levels.push_back (coarser (peaks->levels.back(), channels));

    writePeakCache (cache, peaks->channels, peaks->frames, peaks->sampleRate, peaks->levels);
    return peaks;
}

// Onsets are found once per file and kept in the cache folder beside the peaks, keyed the same way (TS11).
juce::var AudioLibrary::computeOnsets (const juce::File& file)
{
    const auto cache = m_cacheFolder.getParentDirectory().getChildFile ("onsets").getChildFile (
        peakCacheFile (file).getFileNameWithoutExtension() + ".json");

    if (auto cached = juce::JSON::parse (cache); cached.isArray())
        return cached;

    std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return juce::Array<juce::var>();

    juce::AudioBuffer<float> audio (static_cast<int> (std::min (reader->numChannels, 2u)),
                                    static_cast<int> (reader->lengthInSamples));

    if (! reader->read (&audio, 0, audio.getNumSamples(), 0, true, audio.getNumChannels() > 1))
        return juce::Array<juce::var>();

    juce::Array<juce::var> list;

    for (const auto& onset : detectOnsets (audio, reader->sampleRate))
        list.add (juce::Array<juce::var> { static_cast<juce::int64> (onset.frame),
                                           std::round (onset.strength * 1000.0f) / 1000.0f });

    std::ignore = cache.getParentDirectory().createDirectory();
    std::ignore = cache.replaceWithText (juce::JSON::toString (list, true));
    return list;
}

juce::var AudioLibrary::onsets (const juce::File& file)
{
    const auto& key = file.getFullPathName();

    if (const auto it = m_onsets.find (key); it != m_onsets.end())
        return makeObject ({ { "ready", true }, { "onsets", it->second } });

    if (! m_onsetsPending[key] && file.existsAsFile())
    {
        m_onsetsPending[key] = true;
        const std::weak_ptr<bool> alive = m_alive;

        m_jobs.addJob (
            [this, file, key, alive]
            {
                auto found = computeOnsets (file);
                juce::MessageManager::callAsync (
                    [this, key, alive, found]
                    {
                        if (const auto a = alive.lock(); a == nullptr || ! *a)
                            return;

                        m_onsetsPending[key] = false;
                        m_onsets[key] = found;

                        if (m_onsetsReady != nullptr)
                            m_onsetsReady (key);
                    });
            });
    }

    return makeObject ({ { "ready", false } });
}

namespace
{
constexpr int trackMagic = 0x44505431; // "DPT1"

// The full pitch track beside the analysis: rate, start, hop, count, then the pitch per frame (NaN unvoiced).
void writeTrack (const juce::File& target, const PitchTrack& track)
{
    juce::FileOutputStream out (target);

    if (! out.openedOk())
        return;

    std::ignore = out.setPosition (0);
    std::ignore = out.truncate();
    std::ignore = out.writeInt (trackMagic);
    std::ignore = out.writeDouble (track.sampleRate);
    std::ignore = out.writeDouble (track.startSeconds);
    std::ignore = out.writeDouble (track.hopSeconds);
    std::ignore = out.writeInt (static_cast<int> (track.size()));
    std::ignore = out.write (track.pitch.data(), track.size() * sizeof (float));
}

[[nodiscard]] std::shared_ptr<const PitchTrack> readTrack (const juce::File& source)
{
    juce::FileInputStream in (source);

    if (! in.openedOk() || in.readInt() != trackMagic)
        return nullptr;

    auto track = std::make_shared<PitchTrack>();
    track->sampleRate = in.readDouble();
    track->startSeconds = in.readDouble();
    track->hopSeconds = in.readDouble();
    const auto count = in.readInt();

    if (count <= 0 || track->sampleRate <= 0.0 || track->hopSeconds <= 0.0)
        return nullptr;

    track->pitch.resize (static_cast<size_t> (count));

    if (in.read (track->pitch.data(), count * static_cast<int> (sizeof (float)))
        != count * static_cast<int> (sizeof (float)))
        return nullptr;

    track->voicedProb.assign (track->pitch.size(), 0.0f);
    track->levelDb.assign (track->pitch.size(), 0.0f);

    for (size_t i = 0; i < track->size(); ++i)
        track->voicedProb[i] = track->voiced (i) ? 1.0f : 0.0f;

    return track;
}
} // namespace

juce::File AudioLibrary::pitchCacheFile (const juce::File& file, const char* extension) const
{
    return m_cacheFolder.getParentDirectory().getChildFile ("pitch").getChildFile (
        peakCacheFile (file).getFileNameWithoutExtension() + extension);
}

std::shared_ptr<const PitchTrack> AudioLibrary::pitchTrack (const juce::File& file)
{
    const auto source = pitchCacheFile (file, ".track");
    const std::scoped_lock lock (m_tracksLock);
    auto& cached = m_tracks[source.getFullPathName()];

    if (cached == nullptr)
        cached = readTrack (source);

    return cached;
}

// Pitch analyses are cached beside the onsets, keyed the same way, and redone when the analyser changes
// (docs/design/pitch-correction.md, section 3.6). The full track goes beside them for pitch renders.
juce::var AudioLibrary::computePitch (const juce::File& file, const std::weak_ptr<bool>& alive)
{
    const auto cache = pitchCacheFile (file, ".json");
    const auto trackFile = pitchCacheFile (file, ".track");
    const std::scoped_lock lock (m_pitchModelLock);

    // The model the user waits for gets four threads (section 3.2); analyses run one at a time under the lock.
    if (m_pitchModel == nullptr && m_pitchModelError.isEmpty())
    {
        m_pitchModel = loadPitchModel (pitchModelsFolder().getChildFile (pitchModelFile), m_pitchModelError, 4);

        if (m_pitchModel == nullptr && m_pitchModelError.isEmpty())
            m_pitchModelError = "The pitch model could not be loaded.";
    }

    if (m_pitchModel == nullptr)
        return makeObject ({ { "error", m_pitchModelError } });

    const auto analyser = juce::String (pitchAnalysisVersion) + "/" + m_pitchModel->identity();

    if (auto cached = juce::JSON::parse (cache);
        cached.isObject() && cached["analyser"].toString() == analyser && trackFile.existsAsFile())
        return cached;

    std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples >= std::numeric_limits<int>::max())
        return makeObject ({ { "error", "The file could not be read." } });

    juce::AudioBuffer<float> audio (static_cast<int> (std::min (reader->numChannels, 2u)),
                                    static_cast<int> (reader->lengthInSamples));

    if (! reader->read (&audio, 0, audio.getNumSamples(), 0, true, audio.getNumChannels() > 1))
        return makeObject ({ { "error", "The file could not be read." } });

    std::vector<Onset> onsets;

    if (const auto* list = computeOnsets (file).getArray())
        for (const auto& onset : *list)
            onsets.push_back ({ static_cast<juce::int64> (onset[0]), static_cast<float> (onset[1]) });

    const auto analysis = analysePitch (audio, reader->sampleRate, *m_pitchModel, &onsets,
                                        [&alive]
                                        {
                                            const auto a = alive.lock();
                                            return a == nullptr || ! *a;
                                        });

    if (analysis.track.size() == 0)
        return {};

    auto result = pitchAnalysisToVar (analysis);
    std::ignore = cache.getParentDirectory().createDirectory();
    writeTrack (trackFile, analysis.track);
    {
        const std::scoped_lock tracks (m_tracksLock);
        m_tracks.erase (trackFile.getFullPathName());
    }
    std::ignore = cache.replaceWithText (juce::JSON::toString (result, true));
    return result;
}

juce::var AudioLibrary::pitchAnalysis (const juce::File& file)
{
    const auto& key = file.getFullPathName();

    if (const auto it = m_pitch.find (key); it != m_pitch.end())
        return it->second;

    if (! m_pitchPending[key] && file.existsAsFile())
    {
        m_pitchPending[key] = true;
        const std::weak_ptr<bool> alive = m_alive;

        m_jobs.addJob (
            [this, file, key, alive]
            {
                auto found = computePitch (file, alive);
                juce::MessageManager::callAsync (
                    [this, key, alive, found]
                    {
                        if (const auto a = alive.lock(); a == nullptr || ! *a)
                            return;

                        m_pitchPending[key] = false;

                        if (! found.isObject())
                            return;

                        found.getDynamicObject()->setProperty ("ready", true);
                        m_pitch[key] = found;

                        if (m_pitchReady != nullptr)
                            m_pitchReady (key);
                    });
            });
    }

    return makeObject ({ { "ready", false } });
}

double AudioLibrary::pitchOf (const juce::File& file, juce::int64 start, juce::int64 end)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0)
        return 0.0;

    // Enough for soundPitch's second after the attack.
    start = std::clamp<juce::int64> (start, 0, reader->lengthInSamples);
    end = end <= start ? reader->lengthInSamples : std::min (end, reader->lengthInSamples);
    const auto frames =
        static_cast<int> (std::min<juce::int64> (end - start, static_cast<juce::int64> (1.5 * reader->sampleRate)));

    if (frames <= 0)
        return 0.0;

    juce::AudioBuffer<float> audio (static_cast<int> (std::min (2u, reader->numChannels)), frames);

    if (! reader->read (&audio, 0, frames, start, true, audio.getNumChannels() > 1))
        return 0.0;

    return soundPitch (audio, reader->sampleRate, 0, frames);
}

juce::var AudioLibrary::samples (const juce::File& file, juce::int64 start, int count)
{
    constexpr int most = 65536;
    count = std::clamp (count, 0, most);
    auto& reader = m_sampleReaders[file.getFullPathName()];

    if (reader == nullptr)
    {
        // A few files at a time are zoomed into; keep their readers open, but not every file ever drawn.
        if (m_sampleReaders.size() > 16)
        {
            m_sampleReaders.clear();
            return samples (file, start, count);
        }

        reader.reset (m_formats.createReaderFor (file));

        if (reader == nullptr)
            return {};
    }

    juce::AudioBuffer<float> buffer (2, std::max (1, count));
    std::ignore = reader->read (&buffer, 0, count, start, true, true); // outside the file: silence
    juce::MemoryBlock data (static_cast<size_t> (count) * sizeof (float));
    auto* out = static_cast<float*> (data.getData());

    for (int i = 0; i < count; ++i)
        out[i] = 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i));

    return makeObject ({ { "start", start },
                         { "count", count },
                         { "data", juce::Base64::toBase64 (data.getData(), data.getSize()) } });
}

juce::var AudioLibrary::peaks (const juce::File& file, int level)
{
    const auto& key = file.getFullPathName();

    if (const auto it = m_peaks.find (key); it != m_peaks.end() && it->second != nullptr)
    {
        const auto& peaks = *it->second;
        const auto index = static_cast<size_t> (std::clamp (level, 0, static_cast<int> (peaks.levels.size()) - 1));
        const auto& data = peaks.levels[index];
        auto spp = framesPerPoint;

        for (size_t i = 0; i < index; ++i)
            spp *= levelFactor;

        return makeObject ({
            { "ready", true },
            { "samplesPerPoint", spp },
            { "channels", peaks.channels },
            { "points", static_cast<int> (data.size() / static_cast<size_t> (peaks.channels * 2)) },
            { "frames", peaks.frames },
            { "sampleRate", peaks.sampleRate },
            { "levels", static_cast<int> (peaks.levels.size()) },
            { "data", juce::Base64::toBase64 (data.data(), data.size()) },
        });
    }

    if (! m_peaksPending[key] && file.existsAsFile())
    {
        m_peaksPending[key] = true;
        const std::weak_ptr<bool> alive = m_alive;

        m_jobs.addJob (
            [this, file, key, alive]
            {
                auto computed = computePeaks (file);
                juce::MessageManager::callAsync (
                    [this, key, alive, ready = std::move (computed)]
                    {
                        if (const auto a = alive.lock(); a == nullptr || ! *a)
                            return;

                        m_peaksPending[key] = false;
                        m_peaks[key] = ready;
                        m_peaksReady (key);
                    });
            });
    }

    return makeObject ({ { "ready", false } });
}

std::shared_ptr<const AudioSource> AudioLibrary::openForPreview (const juce::File& file)
{
    return AudioSource::open (file, m_formats, m_readAhead);
}

bool AudioLibrary::isAudioFile (const juce::File& file)
{
    return file.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
}

void AudioLibrary::readDurations (std::vector<juce::File> files, std::function<void (juce::var)> done)
{
    const std::weak_ptr<bool> alive = m_alive;

    m_jobs.addJob (
        [this, jobFiles = std::move (files), onDone = std::move (done), alive]
        {
            const auto durations = newObject();

            for (const auto& file : jobFiles)
            {
                const std::unique_ptr<juce::AudioFormatReader> reader (m_formats.createReaderFor (file));

                if (reader != nullptr && reader->sampleRate > 0)
                    durations->setProperty (file.getFullPathName(),
                                            static_cast<double> (reader->lengthInSamples) / reader->sampleRate);
            }

            juce::MessageManager::callAsync (
                [alive, onDone, result = juce::var (durations.get())]
                {
                    if (const auto a = alive.lock(); a != nullptr && *a)
                        onDone (result);
                });
        });
}

void AudioLibrary::copyProjectAudio (const Project& project, const juce::File& from, const juce::File& to)
{
    for (const auto& file : project.audioFiles)
    {
        if (juce::File::isAbsolutePath (file.path))
            continue;

        const auto source = from.getChildFile (file.path);
        const auto target = to.getChildFile (file.path);

        if (source.existsAsFile() && ! target.exists())
        {
            std::ignore = target.getParentDirectory().createDirectory();
            std::ignore = source.copyFileTo (target);
        }
    }
}

std::vector<AudioFile> AudioLibrary::missing (const Project& project) const
{
    std::vector<AudioFile> result;

    for (const auto& file : project.audioFiles)
        if (! locate (file).existsAsFile())
            result.push_back (file);

    return result;
}

} // namespace dawg
