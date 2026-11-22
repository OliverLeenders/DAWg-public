#include "MainComponent.h"
#include "dawg/MidiFileIO.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Recording.h"
#include "dawg/VarUtils.h"

// MIDI input in the app (docs/design/phase9-export-midi-input.md): devices, routing, recording and MIDI learn.

namespace dawg
{

namespace
{
// A controller resting this long ends its undo step (M8).
constexpr double mappingGestureSeconds = 0.5;

// Which parameter a mapping drives, as a key for gesture merging.
[[nodiscard]] juce::String targetKey (const MidiMapping& m)
{
    return juce::String (m.trackId) + ":" + juce::String (m.nodeId) + ":" + m.param;
}

// A controller value (0..127) on a parameter's range: the same curve the UI's sliders use (mid at the middle).
[[nodiscard]] double paramValue (const ParamInfo& info, int value)
{
    const auto t = std::clamp (value / 127.0, 0.0, 1.0);
    const double min = info.minValue;
    const double max = info.maxValue;
    const double mid = info.midValue;
    auto shaped = t;

    if (mid > min && mid < max)
    {
        const auto r = std::pow ((max - mid) / (mid - min), 2.0);

        if (std::abs (r - 1.0) > 1.0e-6)
            shaped = (std::pow (r, t) - 1.0) / (r - 1.0);
    }

    const auto result = min + (max - min) * shaped;
    return info.kind == ParamKind::continuous ? result : std::round (result);
}
} // namespace

void MainComponent::initialiseMidi()
{
    m_midi = std::make_unique<MidiInputHub> (m_deviceManager, m_engine, *m_properties.getUserSettings());
    updateMidiTargets();
}

void MainComponent::updateMidiTargets()
{
    if (m_midi == nullptr)
        return;

    // Armed instrument tracks hear input; with none armed, the selected instrument track (M4).
    const auto project = m_session.project();
    std::vector<ObjectId> targets;

    for (const auto& track : project->tracks)
        if (track->kind == TrackKind::instrument && m_armed.contains (track->id))
            targets.push_back (track->id);

    if (targets.empty())
        if (const auto* track = project->findTrack (m_selectedTrack);
            track != nullptr && track->kind == TrackKind::instrument)
            targets.push_back (track->id);

    m_midi->setTargets (std::move (targets));

    // Recorded live events are placed back by what the player heard: output latency plus one block (M5).
    if (auto* device = m_deviceManager.getCurrentAudioDevice())
        m_engine.setInputLatency (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples());
}

void MainComponent::registerMidiHandlers()
{
    m_bridge.on ("midi.devices", [this] (const juce::var&) { return m_midi->devices(); });

    // Loop recording of notes: a take per pass, or overdub (docs/design/takes-lanes.md, T4). Without "overdub", reads it.
    m_bridge.on ("record.loopOverdub",
                 [this] (const juce::var& p)
                 {
                     if (p.hasProperty ("overdub"))
                         m_properties.getUserSettings()->setValue ("loopOverdub", static_cast<bool> (p["overdub"]));

                     return juce::var (loopOverdub());
                 });

    m_bridge.on ("midi.configure",
                 [this] (const juce::var& p)
                 {
                     m_midi->configure (p["identifier"].toString(), p);
                     return m_midi->devices();
                 });

    m_bridge.on ("track.select",
                 [this] (const juce::var& p)
                 {
                     m_selectedTrack = static_cast<ObjectId> (p["trackId"]);
                     updateMidiTargets();
                     return juce::var();
                 });

    m_bridge.on ("midi.learn",
                 [this] (const juce::var& p)
                 {
                     // The next controller moved is mapped to this parameter (M8). A null target cancels.
                     if (! p.hasProperty ("trackId"))
                     {
                         m_learning.reset();
                         return juce::var();
                     }

                     MidiMapping target;
                     target.trackId = static_cast<ObjectId> (p["trackId"]);
                     target.nodeId = static_cast<ObjectId> (p["nodeId"]);
                     target.param = p["param"].toString();
                     m_learning = target;
                     return juce::var();
                 });
}

void MainComponent::pollMidi()
{
    if (m_midi == nullptr)
        return;

    m_midiActivity = m_midi->takeActivity();

    if (m_midi->takeSettingsChanged())
        m_bridge.emit ("dawg_midi", m_midi->devices());

    for (const auto& message : m_midi->takeControllers())
    {
        if (m_learning.has_value())
        {
            const auto mapping = m_learning.value_or (MidiMapping {});
            m_learning.reset();
            const auto result = m_session.apply ("setMidiMapping", makeObject ({ { "device", message.device },
                                                                                 { "channel", message.channel },
                                                                                 { "controller", message.controller },
                                                                                 { "trackId", mapping.trackId },
                                                                                 { "nodeId", mapping.nodeId },
                                                                                 { "param", mapping.param } }));
            m_bridge.emit ("dawg_learned", makeObject ({ { "ok", result.ok }, { "error", result.error } }));
            continue;
        }

        for (const auto& mapping : m_session.project()->midiMappings)
            if (mapping.controller == message.controller && mapping.channel == message.channel
                && (mapping.device.isEmpty() || mapping.device == message.device))
                applyMidiMapping (mapping, message.value);
    }

    // Live events recorded on armed instrument tracks (M5).
    while (const auto recorded = m_engine.popRecordedInput())
        if (const auto it = m_noteRecorders.find (recorded->input.trackId); it != m_noteRecorders.end())
            it->second.add (*recorded);
}

void MainComponent::applyMidiMapping (const MidiMapping& mapping, int value)
{
    // One continuous movement is one undo step: the gesture lasts until the controller rests (M8).
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    auto& [gesture, last] = m_mappingGestures[targetKey (mapping)];

    if (gesture.isEmpty() || now - last > mappingGestureSeconds)
        gesture = juce::Uuid().toString();

    last = now;
    const auto project = m_session.project();
    const auto* track = project->findTrackOrMaster (mapping.trackId);

    if (track == nullptr)
        return;

    // A controller movement is a touch for automation writing (docs/design/phase10-automation.md, A7).
    AutomationTarget target;
    target.nodeId = mapping.nodeId;
    target.param = mapping.param;
    target.kind = mapping.param == "pan" ? AutomationTargetKind::pan : AutomationTargetKind::gain;

    if (mapping.nodeId != 0)
        target.kind = AutomationTargetKind::nodeParam;
    touchAutomationFromMidi (track->id, target);

    if (mapping.nodeId == 0)
    {
        if (mapping.param == "pan")
        {
            // 64 is the centre.
            const auto pan = value == 64 ? 0.0 : std::clamp ((value - 64) / 63.0, -1.0, 1.0);
            std::ignore =
                m_session.apply ("setTrackPan", makeObject ({ { "id", track->id }, { "pan", pan } }), gesture);
        }
        else
        {
            // A fader curve: square law, 0 dB at about 72 %, +6 dB at the top, silence at 0.
            const auto x = value / 127.0;
            const auto gainDb = value == 0 ? limits::minGainDb : 20.0 * std::log10 (2.0 * x * x);
            std::ignore = m_session.apply (
                "setTrackGain",
                makeObject ({ { "id", track->id }, { "gainDb", std::max (limits::minGainDb, gainDb) } }), gesture);
        }

        return;
    }

    const auto* node = track->findNode (mapping.nodeId); // a voice-graph node too

    if (node == nullptr)
        return;

    const auto* descriptor = descriptorOf (track->id, *node);
    const auto param = descriptor != nullptr ? descriptor->indexOfParam (mapping.param) : -1;

    if (param < 0)
        return;

    std::ignore = m_session.apply (
        "setNodeParam",
        makeObject ({ { "trackId", track->id },
                      { "nodeId", mapping.nodeId },
                      { "param", mapping.param },
                      { "value", paramValue (descriptor->params[static_cast<size_t> (param)], value) } }),
        gesture);
}

bool MainComponent::loopOverdub() const
{
    return m_properties.getUserSettings()->getBoolValue ("loopOverdub", false);
}

juce::Array<juce::var> MainComponent::noteRecordingCommands (double stopBeats)
{
    juce::Array<juce::var> commands;
    const auto project = m_session.project();
    const auto& map = project->tempoMap;
    const auto from = static_cast<Ticks> (std::llround (m_engine.recordStartBeats() * ticksPerBeat));
    const auto stop = static_cast<Ticks> (std::llround (stopBeats * ticksPerBeat));
    const auto& loop = project->loop;

    // Loop recording: one clip per pass over the loop range, each in a takes lane of its own, or every pass in one clip
    // with overdub on (docs/design/takes-lanes.md, T4).
    const auto looped = loop.enabled && m_engine.recordLoopWraps() > 0;
    const auto overdub = loopOverdub();
    // Otherwise the clip covers whole bars around the notes.
    const auto barOf = [&map] (Ticks tick) { return map.barAt (static_cast<double> (tick)).bar; };
    auto clipStart = looped ? loop.start : map.tickOfBar (barOf (from));
    auto end = looped ? loop.end : std::max (stop, from + 1);
    auto clipEnd =
        looped ? loop.end : std::max (map.tickOfBar (barOf (clipStart) + 1), map.tickOfBar (barOf (end - 1) + 1));

    // A punch keeps the notes that start inside the punch range, and cuts those held at the punch-out
    // (docs/design/audio-features.md, AU11, AU13).
    if (project->punch.enabled)
    {
        const auto range = looped ? punchInLoop (project->punch, loop) : std::optional<PunchRange> (project->punch);

        if (! range.has_value())
            return commands;

        clipStart = range->start;
        clipEnd = range->end;
        end = looped ? range->end : std::min (range->end, std::max (stop, from + 1));
    }

    for (const auto& [trackId, recorder] : m_noteRecorders)
    {
        if (recorder.empty() || m_engine.recordStartBeats() < 0.0)
            continue;

        // The passes in order, so the last lands on top; a single clip without a loop or with overdub.
        const auto passes = looped && ! overdub ? std::max (1, recorder.passes()) : 1;

        for (int pass = 0; pass < passes; ++pass)
        {
            const auto only = passes > 1 ? pass : -1;
            const auto notes = recorder.notes (clipStart, end, looped ? loop.end : 0, only);
            const auto controllers = recorder.controllers (clipStart, end, only);

            if (notes.size() == 0 && controllers.getDynamicObject()->getProperties().isEmpty())
                continue;

            commands.add (makeObject ({ { "command", "addClip" },
                                        { "args", makeObject ({ { "trackId", trackId },
                                                                { "start", clipStart },
                                                                { "length", clipEnd - clipStart },
                                                                { "notes", notes },
                                                                { "controllers", controllers },
                                                                { "recorded", true } }) } }));
        }
    }

    m_noteRecorders.clear();
    return commands;
}

namespace
{
constexpr int maxMidiSearchResults = 100;

// The MIDI files that ship with the app: next to the executable, or in the macOS bundle's Resources.
[[nodiscard]] juce::File bundledMidiFiles()
{
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto beside = exeDir.getChildFile ("midi-files");
    return beside.isDirectory() ? beside : exeDir.getParentDirectory().getChildFile ("Resources/midi-files");
}

[[nodiscard]] bool isMidiFile (const juce::File& file)
{
    return file.hasFileExtension ("mid;midi");
}

// Thousands of files: too slow for the message thread.
[[nodiscard]] juce::StringArray indexMidiFiles()
{
    const auto root = bundledMidiFiles();
    juce::StringArray index;

    for (const auto& file : root.findChildFiles (juce::File::findFiles, true))
        if (isMidiFile (file))
            index.add (file.getRelativePathFrom (root));

    return index;
}

[[nodiscard]] juce::var entryOf (const juce::File& file)
{
    return makeObject (
        { { "name", file.getFileName() }, { "path", file.getFullPathName() }, { "dir", file.isDirectory() } });
}
} // namespace

// MIDI files (docs/design/sampler-midi-delay-limiter.md, section 3). The UI decides about the file's tempo and sends the
// importMidi command, so reading ends with handing it the contents.
void MainComponent::registerMidiFileHandlers()
{
    // The MIDI files browser: the files that ship with the app. Opening it starts their index, so a search rarely waits.
    const auto startMidiFilesIndex = [this]
    {
        if (! m_midiFilesIndex.valid())
            m_midiFilesIndex = std::async (std::launch::async, indexMidiFiles).share();
    };

    m_bridge.on ("midiFiles.list",
                 [startMidiFilesIndex] (const juce::var& p)
                 {
                     startMidiFilesIndex();
                     const auto folder = p["path"].isString() ? juce::File (p["path"].toString()) : bundledMidiFiles();
                     juce::Array<juce::var> entries;
                     auto children = folder.findChildFiles (
                         juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);
                     children.sort();

                     // Folders first, then files.
                     for (const auto directories : { true, false })
                         for (const auto& child : children)
                             if (child.isDirectory() == directories && (directories || isMidiFile (child)))
                                 entries.add (entryOf (child));

                     return makeObject ({ { "path", folder.getFullPathName() }, { "entries", entries } });
                 });

    m_bridge.on (
        "midiFiles.search",
        [this, startMidiFilesIndex] (const juce::var& p)
        {
            startMidiFilesIndex();
            const auto root = bundledMidiFiles();
            const auto words = juce::StringArray::fromTokens (p["query"].toString(), true);
            juce::Array<juce::var> found;

            // Every word somewhere in its folders or name, as "C Major nostalgic" should find.
            for (const auto& path : m_midiFilesIndex.get())
            {
                if (std::ranges::all_of (words,
                                         [&path] (const juce::String& word) { return path.containsIgnoreCase (word); }))
                {
                    const auto file = root.getChildFile (path);
                    found.add (makeObject (
                        { { "name", file.getFileName() }, { "path", file.getFullPathName() }, { "dir", false } }));
                }

                if (found.size() >= maxMidiSearchResults)
                    break;
            }

            return found;
        });

    registerMidiImportExportHandlers();
}

// Reading and writing MIDI files.
void MainComponent::registerMidiImportExportHandlers()
{
    m_bridge.on ("midi.parse",
                 [] (const juce::var& p)
                 {
                     // From a path (the MIDI files browser), or as bytes (a file dropped from the OS has no path).
                     if (p["path"].isString())
                     {
                         const juce::File file (p["path"].toString());
                         juce::MemoryBlock data;

                         if (! file.loadFileAsData (data))
                             throw std::runtime_error ("The file could not be read");

                         return parseMidiFile (data, file.getFileNameWithoutExtension());
                     }

                     juce::MemoryOutputStream bytes;

                     if (! juce::Base64::convertFromBase64 (bytes, p["data"].toString()))
                         throw std::runtime_error ("The file could not be read");

                     return parseMidiFile (bytes.getMemoryBlock(),
                                           juce::File (p["name"].toString()).getFileNameWithoutExtension());
                 });

    m_bridge.on (
        "midi.importDialog",
        [this] (const juce::var&)
        {
            m_midiChooser = std::make_unique<juce::FileChooser> ("Import MIDI file", juce::File(), "*.mid;*.midi");
            m_midiChooser->launchAsync (
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this] (const juce::FileChooser& chooser)
                {
                    const auto file = chooser.getResult();

                    if (file == juce::File())
                        return;

                    try
                    {
                        juce::MemoryBlock bytes;

                        if (! file.loadFileAsData (bytes))
                            throw std::runtime_error ("The file could not be read");

                        m_bridge.emit ("dawg_midiFile", parseMidiFile (bytes, file.getFileNameWithoutExtension()));
                    }
                    catch (const std::exception& e)
                    {
                        m_bridge.emit ("dawg_error", juce::String (e.what()));
                    }
                });
            return juce::var();
        });

    m_bridge.on ("midi.exportDialog",
                 [this] (const juce::var&)
                 {
                     m_midiChooser = std::make_unique<juce::FileChooser> ("Export MIDI file", juce::File(), "*.mid");
                     m_midiChooser->launchAsync (
                         juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this] (const juce::FileChooser& chooser)
                         {
                             auto file = chooser.getResult();

                             if (file == juce::File())
                                 return;

                             const auto bytes = writeMidiFile (*m_session.project());

                             if (! file.withFileExtension ("mid").replaceWithData (bytes.getData(), bytes.getSize()))
                                 m_bridge.emit ("dawg_error",
                                                "The MIDI file could not be written: " + file.getFullPathName());
                         });
                     return juce::var();
                 });
}

} // namespace dawg
