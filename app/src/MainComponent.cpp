#include "MainComponent.h"

#include "AppLog.h"
#include "DesktopCheck.h"
#include "WindowChrome.h"
#include "dawg/AllocationGuard.h"
#include "dawg/ClapPlugin.h"
#include "dawg/ClipOps.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Recording.h"
#include "dawg/SampleMapping.h"
#include "dawg/Sfz.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <stdexcept>

namespace dawg
{

namespace
{
constexpr const char* audioStateKey = "audioDeviceState";
constexpr int telemetryRateHz = 30;
constexpr int defaultWidth = 1400;
constexpr int defaultHeight = 860;
const juce::Colour backgroundColour { 0xff111318 };

[[nodiscard]] juce::File appDataFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DAWg");
}

// An instrument drives a track's notes: inserting one replaces the instrument the track already has.
[[nodiscard]] bool isInstrumentType (const juce::String& type)
{
    const auto* descriptor = NodeRegistry::builtin().find (type);
    return descriptor != nullptr && isInstrument (*descriptor);
}

// Files dropped from the OS are staged in the temp folder under an id the UI makes. The id becomes a directory name,
// so only plain characters are accepted.
[[nodiscard]] bool isValidUploadId (const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 64
           && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
}

// A dropped import can leave its staging folder behind (a failed chunk, or quitting before the import copies it).
void removeStaleUploads()
{
    constexpr juce::int64 staleAfterMs = 24LL * 60 * 60 * 1000;
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto now = juce::Time::getCurrentTime().toMilliseconds();

    for (const auto& entry : juce::RangedDirectoryIterator (root, false, "dawg-upload-*", juce::File::findDirectories))
        if (now - entry.getFile().getLastModificationTime().toMilliseconds() > staleAfterMs)
            std::ignore = entry.getFile().deleteRecursively();
}

// The audio clips among the given ids (a var array), skipping note clips, unknown ids and clips of missing pool
// entries.
[[nodiscard]] std::vector<ClipPtr> audioClipsOf (const Project& project, const juce::var& ids)
{
    std::vector<ClipPtr> result;

    if (const auto* list = ids.getArray())
        for (const auto& id : *list)
            for (const auto& track : project.tracks)
                for (const auto& clip : track->clips)
                    if (clip->id == static_cast<ObjectId> (static_cast<juce::int64> (id)) && clip->fileId != 0
                        && project.findAudioFile (clip->fileId) != nullptr)
                        result.push_back (clip);

    return result;
}

[[nodiscard]] juce::var historyResult (const Session::Result& result)
{
    return makeObject ({ { "rev", result.revision }, { "applied", result.ok } });
}
} // namespace

MainComponent::MainComponent (juce::ApplicationProperties& appProperties, std::optional<juce::String> devServerUrl)
    : m_properties (appProperties),
      m_devServerUrl (std::move (devServerUrl))
{
    initialiseAudio();
    removeStaleUploads();

    juce::addDefaultFormatsToManager (m_pluginFormats);
    m_pluginFormats.addFormat (std::make_unique<ClapPluginFormat>());
    m_pluginScanner = std::make_unique<PluginScanner> (m_pluginFormats, *m_properties.getUserSettings());
    m_pluginScanner->setChangeCallback ([this] { m_bridge.emit ("dawg_plugins", m_pluginScanner->toVar()); });

    m_pluginHost = std::make_unique<PluginHost> (
        m_pluginFormats, m_pluginScanner->getKnownPlugins(), [this] { return currentSpec(); },
        [this] (bool recompile)
        {
            if (recompile)
                recompileEngine();

            emitPluginStatus();
        });
    // On by default on Windows (docs/design/out-of-process-plugins.md, O9); on Linux only once real plugins have been tried
    // there (step 7). The environment overrides it, for trying things.
#if JUCE_WINDOWS
    constexpr auto processesByDefault = true;
#else
    constexpr auto processesByDefault = false;
#endif
    const auto processes = juce::SystemStats::getEnvironmentVariable ("DAWG_PLUGIN_PROCESSES", {});
    m_pluginHost->setOutOfProcess (processes.isNotEmpty() ? processes == "1"
                                                          : m_properties.getUserSettings()->getBoolValue (
                                                                "pluginsOutOfProcess", processesByDefault));
    m_pluginHost->setRunInside (
        juce::StringArray::fromTokens (m_properties.getUserSettings()->getValue ("pluginsRunInside"), "\n", {}));
    // A plugin whose parameters were added or removed comes back as a new instance with its state, which lists them
    // (docs/design/clap-hosting.md, section 3.6).
    m_pluginHost->setParameterListCallback (
        [this] (ObjectId trackId, ObjectId nodeId)
        {
            restartPluginNodes ([&] (ObjectId track, const GraphNode& node)
                                { return track == trackId && node.id == nodeId; });
        });
    m_audio = std::make_unique<AudioLibrary> (
        [this]
        { return m_projects != nullptr ? m_projects->projectFolder() : appDataFolder().getChildFile ("Unsaved"); },
        appDataFolder().getChildFile ("Cache").getChildFile ("peaks"),
        [this] (const juce::String& path) { m_bridge.emit ("dawg_peaks", makeObject ({ { "path", path } })); });
    m_audio->setOnsetsReady ([this] (const juce::String& path)
                             { m_bridge.emit ("dawg_onsets", makeObject ({ { "path", path } })); });
    m_audio->setPitchReady ([this] (const juce::String& path)
                            { m_bridge.emit ("dawg_pitchAnalysis", makeObject ({ { "path", path } })); });
    m_compiler.setAudioSourceResolver (m_audio->resolver());

    // Stretch renders (docs/design/time-stretching.md, TS7–TS9): one worker per spare core.
    m_stretch = std::make_unique<StretchRenderer> (
        appDataFolder().getChildFile ("Cache").getChildFile ("stretch"), makeRubberBandStretcher(),
        m_audio->stretchInput(), [this] (AudioFile& file) { return m_audio->identify (file); },
        std::max (1, juce::SystemStats::getNumCpus() - 2));
    // Pitch renders read the files' pitch tracks from the analysis cache (docs/design/pitch-correction.md, 5.2).
    m_stretch->setPitchTracks ([audio = m_audio.get()] (const AudioFile& file)
                               { return audio->pitchTrack (juce::File (file.path)); });
    m_stretch->setSizeLimit (
        static_cast<juce::int64> (m_properties.getUserSettings()
                                      ->getValue ("stretchCacheLimit", juce::String (StretchRenderer::defaultSizeLimit))
                                      .getLargeIntValue()));
    m_stretch->setReadyCallback (
        [safe = juce::Component::SafePointer<MainComponent> (this)] (const std::vector<ObjectId>& clips)
        {
            juce::Array<juce::var> ids;

            for (const auto id : clips)
                ids.add (static_cast<juce::int64> (id));

            juce::MessageManager::callAsync (
                [safe, ids]
                {
                    if (safe != nullptr)
                        safe->stretchReady (ids);
                });
        });
    m_compiler.setStretchRenderer (m_stretch.get());
    m_compiler.setPluginLoader (
        [this] (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target)
        { m_pluginHost->load (trackId, node, target); });
    m_pluginWindows = std::make_unique<PluginWindows> (*m_properties.getUserSettings(),
                                                       [this] (PluginWindows::Key key)
                                                       {
                                                           // Closing an editor is a good moment to keep its state.
                                                           stampPlugin (key.first, key.second);
                                                           emitPluginStatus();
                                                       });

    initialiseFaust();
    initialiseModel();
    initialiseProjects();
    initialiseMidi();

    registerHandlers();
    startUiRecovery();
    initialiseBrowser();

    startTimerHz (telemetryRateHz);
    setSize (defaultWidth, defaultHeight);
}

MainComponent::~MainComponent()
{
    stopTimer();
    shutdownFaust();
    m_midi.reset();
    waitForExport();
    stopUpdateFetch();
    m_projects.reset(); // a clean exit: removes the lock file
    m_pluginWindows.reset();
    m_deviceManager.removeChangeListener (this);
    m_deviceManager.removeAudioCallback (&m_engine);
    m_deviceManager.closeAudioDevice();
    m_mixerWindow.reset();
    m_pianoRollWindow.reset();
    m_audioEditorWindow.reset();
    m_instrumentWindow.reset();
    m_bridge.detach (m_browser.get());
}

void MainComponent::initialiseAudio()
{
    const auto savedState = m_properties.getUserSettings()->getXmlValue (audioStateKey);
    // Two inputs by default, for recording (phase 7); the audio settings choose which.
    const auto error = m_deviceManager.initialise (2, 2, savedState.get(), true);

    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Audio device error: " + error);

    m_deviceManager.addAudioCallback (&m_engine);
    m_deviceManager.addChangeListener (this);
}

void MainComponent::initialiseModel()
{
    // No project is open at startup: the welcome screen shows (docs/design/phase5-project-workflow.md).
    updateEngine();

    m_session.setChangeListener (
        [this] (const ProjectPtr& /*before*/, const ProjectPtr& after, const juce::var& patch)
        {
            // Editors of removed nodes close before their plugins can go away.
            m_pluginWindows->closeUnless (
                [&] (PluginWindows::Key key)
                {
                    const auto* track = after->findTrackOrMaster (key.first);
                    return track != nullptr && track->findNode (key.second) != nullptr;
                });

            scheduleEngineUpdate();
            m_bridge.emit ("dawg_patch", patch);
            emitPluginStatus();

            if (m_projects != nullptr)
                m_projects->sessionChanged();
        });
}

ProcessSpec MainComponent::currentSpec() const
{
    constexpr double fallbackSampleRate = 48000.0;
    constexpr int maxBlockSize = 512;

    auto* device = m_deviceManager.getCurrentAudioDevice();
    return { device != nullptr ? device->getCurrentSampleRate() : fallbackSampleRate, maxBlockSize };
}

void MainComponent::updateEngine()
{
    // The export thread is the engine's only caller while it renders (X1); the change is applied when it is done.
    if (offlineExportRunning())
        return;

    if (auto engine = m_compiler.update (*m_session.project(), currentSpec()))
    {
        m_engine.setEngine (std::move (engine));
        emitStretchStatus ({});
    }
}

// Renders finished (docs/design/time-stretching.md, TS9): they play from the next compilation, which is coalesced
// when several finish together.
void MainComponent::stretchReady (const juce::Array<juce::var>& clipIds)
{
    emitStretchStatus (clipIds);

    if (m_stretchRecompileScheduled)
        return;

    m_stretchRecompileScheduled = true;
    juce::Timer::callAfterDelay (30,
                                 [safe = juce::Component::SafePointer (this)]
                                 {
                                     if (safe == nullptr)
                                         return;

                                     safe->m_stretchRecompileScheduled = false;
                                     safe->recompileEngine();
                                 });
}

// Tells the UI which clips wait for renders, and which just got theirs (the dawg_stretch event).
void MainComponent::emitStretchStatus (const juce::Array<juce::var>& ready)
{
    if (m_stretch == nullptr)
        return;

    juce::Array<juce::var> pending;

    for (const auto id : m_stretch->pendingClips())
        pending.add (static_cast<juce::int64> (id));

    if (ready.isEmpty() && pending == m_lastStretchPending)
        return;

    m_lastStretchPending = pending;
    m_bridge.emit ("dawg_stretch", makeObject ({ { "ready", ready }, { "pending", pending } }));
}

void MainComponent::scheduleEngineUpdate()
{
    if (m_engineUpdateScheduled)
        return;

    m_engineUpdateScheduled = true;
    // Let the project-open response and the first UI paint complete before planning every track. Further edits
    // during this window are coalesced into the same compile.
    juce::Timer::callAfterDelay (50,
                                 [safe = juce::Component::SafePointer (this)]
                                 {
                                     if (safe == nullptr)
                                         return;

                                     safe->m_engineUpdateScheduled = false;
                                     safe->updateEngine();
                                 });
}

void MainComponent::restartPlugins (const std::function<bool (const PluginRef&)>& which)
{
    restartPluginNodes ([&] (ObjectId, const GraphNode& node) { return which (*node.plugin); });
}

void MainComponent::restartPluginNodes (const std::function<bool (ObjectId trackId, const GraphNode& node)>& which)
{
    const auto restart = [&] (const Track& track)
    {
        forEachNode (track.graph,
                     [&] (const GraphNode& node, bool)
                     {
                         if (! node.plugin.has_value() || ! which (track.id, node))
                             return;

                         // The new instance starts from the model, so the model has to know the state first.
                         stampPlugin (track.id, node.id);
                         m_pluginWindows->close ({ track.id, node.id });
                         m_compiler.forgetPluginInstance (track.id, node.id);
                     });
    };

    const auto project = m_session.project();

    for (const auto& track : project->tracks)
        restart (*track);

    if (project->master != nullptr)
        restart (*project->master);

    recompileEngine();
    emitPluginStatus();
}

void MainComponent::recompileEngine()
{
    // As above: compiling would prepare the instances the export thread is rendering with.
    if (offlineExportRunning())
        return;

    m_engine.setEngine (m_compiler.compile (*m_session.project(), currentSpec()));
    emitStretchStatus ({});
}

juce::var MainComponent::pluginStatus()
{
    auto status = m_pluginHost->statusVar();

    for (const auto& property : status.getDynamicObject()->getProperties())
    {
        const auto ids = juce::StringArray::fromTokens (property.name.toString(), ":", "");
        const PluginWindows::Key key { ids[0].getLargeIntValue(), ids[1].getLargeIntValue() };
        property.value.getDynamicObject()->setProperty ("editorOpen", m_pluginWindows->isOpen (key));
    }

    addFaustStatus (*status.getDynamicObject());
    return status;
}

// The ports' widths in the last compilation, by "track:node", for the nodes with a mono port or a folding input
// (docs/design/mono-ports.md, MP8, MP9).
juce::var MainComponent::portWidths() const
{
    const auto result = newObject();
    const auto list = [] (const std::vector<int>& values)
    {
        juce::Array<juce::var> array;

        for (const auto value : values)
            array.add (value);

        return juce::var (array);
    };

    for (const auto& [key, widths] : m_compiler.portWidths())
        result->setProperty (juce::String (key.first) + ":" + juce::String (key.second),
                             makeObject ({ { "inputs", list (widths.inputs) },
                                           { "outputs", list (widths.outputs) },
                                           { "folded", list (widths.folded) } }));

    return juce::var (result.get());
}

void MainComponent::emitPluginStatus()
{
    // Widths change with the compilation that runs after a command, as node status does.
    auto widths = portWidths();

    if (auto widthsText = juce::JSON::toString (widths, false); widthsText != m_lastPortWidths)
    {
        m_lastPortWidths = std::move (widthsText);
        m_bridge.emit ("dawg_widths", widths);
    }

    auto status = pluginStatus();
    auto text = juce::JSON::toString (status, false);

    // Node status is asked for after every command, but it only changes when nodes come and go, a plugin finishes
    // loading, or an editor opens or closes. Sending it unchanged makes every view that shows a node re-render.
    if (text == m_lastPluginStatus)
        return;

    m_lastPluginStatus = std::move (text);
    m_bridge.emit ("dawg_nodes", status);
}

const NodeDescriptor* MainComponent::descriptorOf (ObjectId trackId, const GraphNode& node) const
{
    // The running instance's: a Faust node's parameters are its script's (docs/design/phase11-faust-panels.md, F3).
    if (const auto* instance = m_compiler.findInstance (trackId, node.id))
        return &instance->descriptor();

    return NodeRegistry::builtin().find (node.type);
}

PluginNode& MainComponent::requirePluginNode (ObjectId trackId, ObjectId nodeId) const
{
    auto* node = dynamic_cast<PluginNode*> (m_compiler.findInstance (trackId, nodeId));

    if (node == nullptr)
        throw std::runtime_error ("No such plugin node");

    return *node;
}

void MainComponent::stampPlugin (ObjectId trackId, ObjectId nodeId)
{
    auto* node = dynamic_cast<PluginNode*> (m_compiler.findInstance (trackId, nodeId));

    if (node == nullptr || node->processor() == nullptr)
        return;

    auto state = std::make_shared<juce::MemoryBlock>();
    node->processor()->getStateInformation (*state);

    const auto* track = m_session.project()->findTrackOrMaster (trackId);
    const auto* model = track != nullptr ? track->findNode (nodeId) : nullptr;

    if (model == nullptr)
        return;

    const auto& current = model->pluginState;

    if (current != nullptr && *current == *state)
        return;

    std::ignore = m_session.amend ([&] (const Project& project)
                                   { return withPluginState (project, trackId, nodeId, std::move (state)); });
}

juce::MemoryBlock MainComponent::duplicateState (ObjectId trackId, ObjectId nodeId)
{
    juce::MemoryBlock state;
    auto* node = dynamic_cast<PluginNode*> (m_compiler.findInstance (trackId, nodeId));

    if (node == nullptr || node->processor() == nullptr)
        return state;

    if (auto* forCopy = dynamic_cast<DuplicateStateSource*> (node->processor()))
        forCopy->getStateForDuplicate (state);
    else
        node->processor()->getStateInformation (state);

    return state;
}

void MainComponent::stampTrackPlugins (const Track& track)
{
    forEachNode (track.graph,
                 [&] (const GraphNode& node, bool)
                 {
                     if (node.plugin.has_value())
                         stampPlugin (track.id, node.id);
                 });
}

void MainComponent::stampBeforeCommand (const juce::String& command, const juce::var& args)
{
    // A command that removes a plugin node, or a track holding one, keeps its latest state for undo
    // (docs/design/phase4-plugin-hosting.md, P3).
    if (command == "removeNode")
    {
        stampPlugin (static_cast<ObjectId> (args["trackId"]), static_cast<ObjectId> (args["nodeId"]));
    }
    else if (command == "removeNodes")
    {
        if (const auto* ids = args["nodeIds"].getArray())
            for (const auto& id : *ids)
                stampPlugin (static_cast<ObjectId> (args["trackId"]), static_cast<ObjectId> (id));
    }
    else if (command == "removeTrack")
    {
        const auto project = m_session.project(); // stamping replaces the snapshot; keep this one alive

        if (const auto* track = project->findTrackOrMaster (static_cast<ObjectId> (args["id"])))
            stampTrackPlugins (*track);
    }
    else if (command == "insertNode" && isInstrumentType (args["type"].toString()))
    {
        // The new instrument takes the place of the one the track has (Commands: insertNode).
        const auto project = m_session.project(); // stamping replaces the snapshot; keep this one alive

        if (const auto* track = project->findTrackOrMaster (static_cast<ObjectId> (args["trackId"])))
            stampTrackPlugins (*track);
    }
    else if (command == "duplicateTrack")
    {
        // The copy starts from what each plugin saves for a copy of itself (DuplicateStateSource); the original's
        // state is stamped too, so the copy of anything else is current.
        const auto project = m_session.project(); // stamping replaces the snapshot; keep this one alive
        const auto id = static_cast<ObjectId> (args["id"]);
        auto states = newObject();

        for (const auto& track : project->tracks)
        {
            auto inside = track->id == id;

            for (auto parent = track->parent; ! inside && parent != 0;)
            {
                inside = parent == id;
                const auto* above = project->findTrack (parent);
                parent = above != nullptr ? above->parent : 0;
            }

            if (! inside)
                continue;

            stampTrackPlugins (*track);
            forEachNode (track->graph,
                         [&] (const GraphNode& node, bool)
                         {
                             if (node.plugin.has_value())
                                 if (const auto state = duplicateState (track->id, node.id); state.getSize() > 0)
                                     states->setProperty (juce::String (node.id), state.toBase64Encoding());
                         });
        }

        if (auto* object = args.getDynamicObject())
            object->setProperty ("pluginStates", juce::var (states.get()));
    }
    else if (command == "addNodes")
    {
        // A pasted plugin node names its source; it starts from what that saves for a copy of itself.
        if (const auto* nodes = args["nodes"].getArray())
            for (const auto& spec : *nodes)
                if (spec["plugin"].isObject() && spec["source"].isObject())
                    if (const auto state = duplicateState (static_cast<ObjectId> (spec["source"]["trackId"]),
                                                           static_cast<ObjectId> (spec["source"]["nodeId"]));
                        state.getSize() > 0)
                        if (auto* object = spec.getDynamicObject())
                            object->setProperty ("pluginState", state.toBase64Encoding());
    }
    else if (command == "batch")
    {
        if (const auto* commands = args["commands"].getArray())
            for (const auto& entry : *commands)
                if (entry["command"].toString() != "batch")
                    stampBeforeCommand (entry["command"].toString(), entry["args"]);
    }
}

void MainComponent::stampAllPlugins()
{
    const auto project = m_session.project(); // stamping replaces the snapshot; keep this one alive

    auto tracks = project->tracks;

    if (project->master != nullptr)
        tracks.push_back (project->master);

    for (const auto& track : tracks)
        stampTrackPlugins (*track);
}

void MainComponent::initialiseProjects()
{
    ProjectManager::Hooks hooks;
    hooks.stampAll = [this] { stampAllPlugins(); };
    hooks.beforeReplace = [this]
    {
        // A different project: ids may repeat with a different meaning, so nothing is reused.
        m_pluginWindows->closeUnless ([] (PluginWindows::Key) { return false; });
        m_compiler.reset();
        m_armed.clear();
        m_bridge.emit ("dawg_armed", juce::var (juce::Array<juce::var>()));
    };
    hooks.stateChanged = [this] { emitProjectState(); };
    hooks.error = [this] (const juce::String& message) { m_bridge.emit ("dawg_error", message); };
    hooks.quit = [] { juce::JUCEApplication::quit(); };
    hooks.copyAudio = [this] (const Project& project, const juce::File& from, const juce::File& to)
    { m_audio->copyProjectAudio (project, from, to); };

    m_projects = std::make_unique<ProjectManager> (m_session, *m_properties.getUserSettings(), appDataFolder(),
                                                   std::move (hooks));
}

void MainComponent::emitProjectState()
{
    const auto state = m_projects->stateVar();

    if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
    {
        const auto name = state["name"].toString();
        window->setName (name.isEmpty() ? juce::String ("DAWg")
                                        : name + (static_cast<bool> (state["dirty"]) ? " *" : "") + " - DAWg");
    }

    m_bridge.emit ("dawg_projectState", state);
}

void MainComponent::requestQuit()
{
    // Unsaved changes are resolved in the UI first (an in-app prompt), which then calls project.quit; the app asks
    // itself when the page's prompt can't be seen (docs/design/ui-recovery.md, UR6).
    if (static_cast<bool> (m_projects->requestQuit (ProjectManager::Mode::ask)["needsDecision"]))
        askToQuit();
}

juce::WebBrowserComponent::Options MainComponent::browserOptions()
{
    using Options = juce::WebBrowserComponent::Options;

    // Every window's web view shares one user data folder, so they share one browser process (mixer.md, X11).
    const auto options =
        Options {}
            .withBackend (Options::Backend::webview2)
            .withWinWebView2Options (Options::WinWebView2 {}
                                         .withUserDataFolder (appDataFolder().getChildFile ("WebView2"))
                                         .withBackgroundColour (backgroundColour)
                                         .withStatusBarDisabled())
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withResourceProvider ([this] (const juce::String& url) { return m_uiResources.get (url); });

    return m_bridge.install (options);
}

juce::String MainComponent::pageUrl (const juce::String& window) const
{
    const auto root = m_devServerUrl.value_or (juce::WebBrowserComponent::getResourceProviderRoot());
    if (m_devServerUrl.has_value())
        return window.isEmpty() ? root : root + (root.endsWith ("/") ? "" : "/") + "?window=" + window;

    const auto revision =
        m_uiResources.getRoot().getChildFile ("index.html").getLastModificationTime().toMilliseconds();
    juce::String query = "ui=" + juce::String (revision);
    if (! window.isEmpty())
        query += "&window=" + window;
    return root + (root.endsWith ("/") ? "" : "/") + "?" + query;
}

void MainComponent::initialiseBrowser()
{
    m_browser = std::make_unique<juce::WebBrowserComponent> (browserOptions());
    m_bridge.attach (m_browser.get());
    addAndMakeVisible (*m_browser);

    if (m_devServerUrl.has_value() || m_uiResources.isAvailable())
        m_browser->goToURL (pageUrl ({}));
    else
        m_missingUiMessage = "Web UI not found in " + m_uiResources.getRoot().getFullPathName()
                             + "\nBuild it with: cd ui && npm ci && npm run build, or start with --dev.";

    m_browser->setVisible (m_missingUiMessage.isEmpty());
}

void MainComponent::showMixer (bool show)
{
    if (show && m_mixerWindow == nullptr && m_missingUiMessage.isEmpty())
    {
        m_mixerWindow = std::make_unique<MixerWindow> (m_bridge, browserOptions(), pageUrl ("mixer"),
                                                       *m_properties.getUserSettings(),
                                                       [safe = juce::Component::SafePointer (this)]
                                                       {
                                                           if (safe != nullptr)
                                                               safe->showMixer (false);
                                                       });
    }
    else if (show && m_mixerWindow != nullptr)
    {
        m_mixerWindow->toFront (true);
    }
    else if (! show && m_mixerWindow != nullptr)
    {
        m_mixerWindow.reset();
        watchNodes ("mixer", {}); // a closed window watches nothing
    }

    m_bridge.emit ("dawg_mixer", makeObject ({ { "open", m_mixerWindow != nullptr } }));
}

juce::DocumentWindow* MainComponent::windowNamed (const juce::String& name)
{
    if (name == "mixer")
        return m_mixerWindow.get();

    if (name == "roll")
        return m_pianoRollWindow.get();

    if (name == "audio")
        return m_audioEditorWindow.get();

    if (name == "instrument")
        return m_instrumentWindow.get();

    return dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
}

void MainComponent::showPianoRoll (bool show, int trackId, int clipId)
{
    if (show && m_pianoRollWindow == nullptr && m_missingUiMessage.isEmpty())
    {
        auto url = pageUrl ("roll");

        // Open on the track and clip that were selected in the main window; later selection changes arrive through
        // the selection relay.
        if (trackId > 0)
            url += "&track=" + juce::String (trackId);

        if (clipId > 0)
            url += "&clip=" + juce::String (clipId);

        m_pianoRollWindow =
            std::make_unique<PianoRollWindow> (m_bridge, browserOptions(), url, *m_properties.getUserSettings(),
                                               [safe = juce::Component::SafePointer (this)]
                                               {
                                                   if (safe != nullptr)
                                                       safe->showPianoRoll (false, 0, 0);
                                               });
    }
    else if (show && m_pianoRollWindow != nullptr)
    {
        m_pianoRollWindow->toFront (true);
    }
    else if (! show && m_pianoRollWindow != nullptr)
    {
        m_pianoRollWindow.reset();
        watchNodes ("roll", {}); // a closed window watches nothing
    }

    m_bridge.emit ("dawg_editor", makeObject ({ { "open", m_pianoRollWindow != nullptr } }));
}

// The audio editor (docs/design/time-stretching.md, TS13): its own window on the same bridge, like the MIDI editor's.
// Opening it on another clip while it is open tells it which.
void MainComponent::showAudioEditor (bool show, int clipId)
{
    if (show && m_audioEditorWindow == nullptr && m_missingUiMessage.isEmpty())
    {
        auto url = pageUrl ("audio");

        if (clipId > 0)
            url += "&clip=" + juce::String (clipId);

        m_audioEditorWindow = std::make_unique<PianoRollWindow> (
            m_bridge, browserOptions(), url, *m_properties.getUserSettings(),
            [safe = juce::Component::SafePointer (this)]
            {
                if (safe != nullptr)
                    safe->showAudioEditor (false, 0);
            },
            "Audio Editor - DAWg", "audioEditorWindow");
    }
    else if (show && m_audioEditorWindow != nullptr)
    {
        if (clipId > 0)
            m_bridge.emit ("dawg_audioEditorClip", makeObject ({ { "clip", clipId } }));

        m_audioEditorWindow->toFront (true);
    }
    else if (! show && m_audioEditorWindow != nullptr)
    {
        m_audioEditorWindow.reset();
        watchNodes ("audio", {}); // a closed window watches nothing
    }

    m_bridge.emit ("dawg_audioEditor", makeObject ({ { "open", m_audioEditorWindow != nullptr } }));
}

// An instrument's editor (docs/design/macros-and-modulation-column.md, MC14): its own window on the same bridge, like the
// audio editor's. Opening it on another instrument while it is open tells it which.
void MainComponent::showInstrumentEditor (bool show, int trackId, int nodeId)
{
    if (show && m_instrumentWindow == nullptr && m_missingUiMessage.isEmpty())
    {
        const auto url = pageUrl ("instrument") + "&track=" + juce::String (trackId) + "&node=" + juce::String (nodeId);
        m_instrumentWindow = std::make_unique<PianoRollWindow> (
            m_bridge, browserOptions(), url, *m_properties.getUserSettings(),
            [safe = juce::Component::SafePointer (this)]
            {
                if (safe != nullptr)
                    safe->showInstrumentEditor (false, 0, 0);
            },
            "Instrument - DAWg", "instrumentWindow");
    }
    else if (show && m_instrumentWindow != nullptr)
    {
        m_bridge.emit ("dawg_instrumentEditor", makeObject ({ { "trackId", trackId }, { "nodeId", nodeId } }));
        m_instrumentWindow->toFront (true);
    }
    else if (! show && m_instrumentWindow != nullptr)
    {
        m_instrumentWindow.reset();
        watchNodes ("instrument", {}); // a closed window watches nothing
    }
}

void MainComponent::registerWindowHandlers()
{
    // An instrument's editor window (MC14).
    m_bridge.on ("instrumentEditor.show",
                 [this] (const juce::var& p)
                 {
                     const auto id = [&p] (const char* key)
                     { return p[key].isInt() || p[key].isDouble() ? static_cast<int> (p[key]) : 0; };
                     juce::MessageManager::callAsync (
                         [safe = juce::Component::SafePointer (this), open = static_cast<bool> (p["open"]),
                          track = id ("trackId"), node = id ("nodeId")]
                         {
                             if (safe != nullptr)
                                 safe->showInstrumentEditor (open, track, node);
                         });
                     return juce::var();
                 });

    // The audio editor window (docs/design/time-stretching.md, TS13).
    m_bridge.on ("audioEditor.state",
                 [this] (const juce::var&) { return makeObject ({ { "open", m_audioEditorWindow != nullptr } }); });
    m_bridge.on ("audioEditor.show",
                 [this] (const juce::var& p)
                 {
                     const auto clip = p["clip"].isInt() || p["clip"].isDouble() ? static_cast<int> (p["clip"]) : 0;
                     juce::MessageManager::callAsync (
                         [safe = juce::Component::SafePointer (this), open = static_cast<bool> (p["open"]), clip]
                         {
                             if (safe != nullptr)
                                 safe->showAudioEditor (open, clip);
                         });
                     return juce::var();
                 });

    // The mixer window (docs/design/mixer.md, X1).
    m_bridge.on ("mixer.state",
                 [this] (const juce::var&) { return makeObject ({ { "open", m_mixerWindow != nullptr } }); });
    m_bridge.on ("mixer.show",
                 [this] (const juce::var& p)
                 {
                     // Deferred: closing the window from its own web view must not destroy it inside the call.
                     juce::MessageManager::callAsync (
                         [safe = juce::Component::SafePointer (this), open = static_cast<bool> (p["open"])]
                         {
                             if (safe != nullptr)
                                 safe->showMixer (open);
                         });
                     return juce::var();
                 });

    // The MIDI editor window.
    m_bridge.on ("editor.state",
                 [this] (const juce::var&) { return makeObject ({ { "open", m_pianoRollWindow != nullptr } }); });
    m_bridge.on ("editor.show",
                 [this] (const juce::var& p)
                 {
                     // Deferred: closing the window from its own web view must not destroy it inside the call.
                     const auto idOf = [] (const juce::var& value)
                     { return value.isInt() || value.isDouble() ? static_cast<int> (value) : 0; };
                     juce::MessageManager::callAsync (
                         [safe = juce::Component::SafePointer (this), open = static_cast<bool> (p["open"]),
                          track = idOf (p["track"]), clip = idOf (p["clip"])]
                         {
                             if (safe != nullptr)
                                 safe->showPianoRoll (open, track, clip);
                         });
                     return juce::var();
                 });

    // The window buttons and dragging where the UI draws the title bar (docs/design/window-chrome.md). Deferred: these
    // must not run inside the web view's call, since a drag only returns with the mouse button and a close destroys it.
    m_bridge.on (
        "window.control",
        [this] (const juce::var& p)
        {
            auto* window = windowNamed (p["window"].toString());

            if (window == nullptr)
                throw std::runtime_error ("No such window");

            const auto maximised = window->isFullScreen();
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<juce::DocumentWindow> (window), action = p["action"].toString()]
                {
                    if (safe == nullptr)
                        return;

                    if (action == "drag")
                        windowchrome::startDrag (*safe);
                    else if (action == "minimise")
                        safe->setMinimised (true);
                    else if (action == "maximise")
                        safe->setFullScreen (! safe->isFullScreen());
                    else if (action == "close")
                        safe->closeButtonPressed();
                });
            return makeObject ({ { "maximised", maximised } });
        });

    // How narrow a window's page lets it get (the instrument window's Sampler panel).
    m_bridge.on ("window.minWidth",
                 [this] (const juce::var& p)
                 {
                     auto* window = dynamic_cast<PianoRollWindow*> (windowNamed (p["window"].toString()));

                     if (window == nullptr)
                         throw std::runtime_error ("This window has no page minimum");

                     window->setContentMinWidth (static_cast<int> (p["width"]));
                     return juce::var();
                 });

    // Settings that belong to the user rather than a project (user presets, drum maps), as text the UI understands.
    m_bridge.on ("settings.get",
                 [this] (const juce::var& p)
                 {
                     const auto key = "ui." + p["key"].toString();
                     auto* settings = m_properties.getUserSettings();
                     return settings->containsKey (key) ? juce::var (settings->getValue (key)) : juce::var();
                 });
    m_bridge.on ("settings.set",
                 [this] (const juce::var& p)
                 {
                     m_properties.getUserSettings()->setValue ("ui." + p["key"].toString(), p["value"].toString());
                     return juce::var();
                 });

    // Selection is UI state in each window; a window's selection is relayed to the others (X11), which ignore their
    // own by its source.
    m_bridge.on ("ui.select",
                 [this] (const juce::var& p)
                 {
                     m_bridge.emit ("dawg_selection", p);
                     return juce::var();
                 });
}

void MainComponent::registerHandlers()
{
    m_bridge.on ("app.info",
                 [] (const juce::var&)
                 {
                     // A Linux desktop that moves windows on Alt+drag: the UI says once what to use instead.
                     const auto& grab = desktop::altDragGrabHere();
                     const auto altDrag =
                         grab.has_value() ? makeObject ({ { "desktop", grab->desktop }, { "setting", grab->setting } })
                                          : juce::var();
                     return makeObject ({ { "version", JUCE_APPLICATION_VERSION_STRING },
                                          { "juceVersion", juce::SystemStats::getJUCEVersion() },
                                          { "vst2Enabled", DAWG_ENABLE_VST2 != 0 },
                                          { "allocationGuard", allocationGuardEnabled },
                                          { "customTitleBar", windowchrome::usesCustomTitleBar() },
                                          { "altDrag", altDrag } });
                 });

    registerModelHandlers();
    registerTransportHandlers();
    registerPluginHandlers();
    registerFaustHandlers();
    registerPanelHandlers();
    registerProjectHandlers();
    registerMidiFileHandlers();
    registerAudioHandlers();
    registerAudioFileHandlers();
    registerAudioClipHandlers();
    registerAudioAnalysisHandlers();
    registerAudioNormaliseHandlers();
    registerRecordHandlers();
    registerExplorerHandlers();
    registerWindowHandlers();
    registerMidiHandlers();
    registerExportHandlers();
    registerUpdateHandlers();
    registerRecoveryHandlers();
    registerComponentHandlers();
    registerAutomationHandlers();

    m_bridge.on ("audio.get", [this] (const juce::var&) { return audioDeviceInfo(); });
    m_bridge.on ("audio.openSettings",
                 [this] (const juce::var&)
                 {
                     openAudioSettings();
                     return juce::var();
                 });

    m_bridge.on ("plugins.get", [this] (const juce::var&) { return m_pluginScanner->toVar(); });
    m_bridge.on ("plugins.scan",
                 [this] (const juce::var&)
                 {
                     m_pluginScanner->startScan();
                     return juce::var();
                 });
}

void MainComponent::registerModelHandlers()
{
    m_bridge.on ("model.get", [this] (const juce::var&) { return m_session.snapshot(); });

    m_bridge.on ("model.apply",
                 [this] (const juce::var& p)
                 {
                     stampBeforeCommand (p["command"].toString(), p["args"]);
                     const auto result = m_session.apply (p["command"].toString(), p["args"], p["gesture"].toString());

                     if (! result.ok)
                         throw std::runtime_error (result.error.toStdString());

                     return makeObject ({ { "rev", result.revision } });
                 });

    m_bridge.on ("model.undo", [this] (const juce::var&) { return historyResult (m_session.undo()); });
    m_bridge.on ("model.redo", [this] (const juce::var&) { return historyResult (m_session.redo()); });
}

void MainComponent::registerTransportHandlers()
{
    m_bridge.on ("transport.play",
                 [this] (const juce::var&)
                 {
                     m_engine.play();
                     return juce::var();
                 });

    // Output protection (docs/design/output-protection.md, S3): the user unmutes a runaway output.
    m_bridge.on ("audio.unmute",
                 [this] (const juce::var&)
                 {
                     m_engine.unmuteOutput();
                     return juce::var();
                 });

    m_bridge.on ("transport.stop",
                 [this] (const juce::var&)
                 {
                     stopTransport();
                     return juce::var();
                 });

    m_bridge.on ("transport.seek",
                 [this] (const juce::var& p)
                 {
                     m_engine.seek (static_cast<double> (p["beats"]));
                     return juce::var();
                 });

    m_bridge.on ("audition.noteOn",
                 [this] (const juce::var& p)
                 {
                     const auto id = m_engine.auditionNoteOn (static_cast<ObjectId> (p["trackId"]),
                                                              static_cast<double> (p["pitch"]),
                                                              static_cast<float> (static_cast<double> (p["velocity"])));
                     return makeObject ({ { "id", id } });
                 });

    m_bridge.on ("audition.noteOff",
                 [this] (const juce::var& p)
                 {
                     m_engine.auditionNoteOff (static_cast<ObjectId> (p["trackId"]), static_cast<ObjectId> (p["id"]));
                     return juce::var();
                 });

    m_bridge.on ("transport.toggle",
                 [this] (const juce::var&)
                 {
                     if (m_engine.isPlaying())
                         stopTransport();
                     else
                         m_engine.play();

                     return juce::var();
                 });
}

void MainComponent::stopTransport()
{
    if (m_engine.isRecording())
        finishRecording();
    else
        m_engine.stop();
}

void MainComponent::setArmed (ObjectId trackId, bool armed)
{
    if (armed)
        m_armed.insert (trackId);
    else
        m_armed.erase (trackId);

    m_compiler.trackState (trackId)->armed.store (armed);
    updateMidiTargets();

    juce::Array<juce::var> ids;
    for (const auto id : m_armed)
        ids.add (id);

    m_bridge.emit ("dawg_armed", juce::var (ids));
}

void MainComponent::startRecording (bool countIn)
{
    if (m_engine.isRecording())
        return;

    const auto project = m_session.project();
    const auto audioFolder = m_projects->projectFolder().getChildFile ("Audio");
    std::ignore = audioFolder.createDirectory();
    auto* device = m_deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        m_bridge.emit ("dawg_error", juce::String ("No audio device is open"));
        return;
    }

    const auto inputs = device->getActiveInputChannels().countNumberOfSetBits();
    const auto sampleRate = device->getCurrentSampleRate();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    std::vector<RecordTarget> targets;
    m_takes.clear();

    for (const auto& track : project->tracks)
    {
        if (! m_armed.contains (track->id) || track->kind != TrackKind::audio || track->inputChannel < 0
            || track->inputChannel >= inputs)
            continue;

        Take take;
        take.trackId = track->id;
        take.channels = track->inputStereo && track->inputChannel + 1 < inputs ? 2 : 1;
        take.file = audioFolder.getChildFile (juce::File::createLegalFileName (track->name + " " + stamp) + ".wav");

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (take.file.createOutputStream());
        auto writer = stream == nullptr ? nullptr
                                        : wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                                           .withSampleRate (sampleRate)
                                                                           .withNumChannels (take.channels)
                                                                           .withBitsPerSample (24));

        if (writer == nullptr)
        {
            m_bridge.emit ("dawg_error", "Could not write " + take.file.getFullPathName());
            continue;
        }

        constexpr int fifoFrames = 1 << 18; // about 5 s at 48 kHz
        take.writer =
            std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), m_recordThread, fifoFrames);
        targets.push_back ({ track->id, track->inputChannel, take.channels == 2, take.writer.get() });
        m_takes.push_back (std::move (take));
    }

    // Armed instrument tracks record notes (docs/design/phase9-export-midi-input.md, M5).
    m_noteRecorders.clear();

    for (const auto& track : project->tracks)
        if (m_armed.contains (track->id) && track->kind == TrackKind::instrument)
            m_noteRecorders[track->id] = {};

    if (targets.empty() && m_noteRecorders.empty())
    {
        m_bridge.emit ("dawg_error",
                       juce::String ("Arm an instrument track, or an audio track with an input, to record"));
        return;
    }

    if (! targets.empty() && ! m_recordThread.isThreadRunning())
        m_recordThread.startThread();

    // The pre-roll (docs/design/audio-features.md, AU12): with the count-in, playback starts that many bars before
    // the record position. With punch on, the record position is the punch-in (AU11), unless the playhead is earlier,
    // and the file records from where playback starts, so the pre-roll is a handle.
    const auto& map = project->tempoMap;
    const auto playhead = m_engine.getTelemetry().positionBeats;
    const auto& punch = project->punch;

    if (punch.enabled && project->loop.enabled && ! punchInLoop (punch, project->loop).has_value())
    {
        m_bridge.emit ("dawg_error", juce::String ("Punch range is outside the loop"));
        return;
    }

    const auto anchor = punch.enabled ? ticksToBeats (static_cast<double> (punch.start)) : playhead;
    auto playFrom = anchor;

    for (int bar = 0; countIn && bar < preRollBars() && playFrom > 0.0; ++bar)
    {
        const auto& signature = map.signatureAt (playFrom * ticksPerBeat - 1.0);
        playFrom = std::max (0.0, playFrom - ticksToBeats (static_cast<double> (signature.barTicks())));
    }

    if (punch.enabled)
        playFrom = std::min (playFrom, playhead);

    // Without a loop, a punch stops writing a second after the punch-out.
    const auto until = punch.enabled && ! project->loop.enabled
                           ? ticksToBeats (map.tickAt (map.secondsAt (static_cast<double> (punch.end)) + 1.0))
                           : -1.0;

    m_engine.startRecording (targets, punch.enabled ? playFrom : anchor, until);

    if (! juce::exactlyEqual (playFrom, playhead))
        m_engine.seek (playFrom);

    m_engine.play();
}

int MainComponent::preRollBars() const
{
    return std::clamp (m_properties.getUserSettings()->getIntValue ("preRollBars", 1), 0, 4);
}

void MainComponent::finishRecording()
{
    const auto stopBeats = m_engine.positionBeats();
    m_engine.stopRecording();
    m_engine.stop();

    // The audio thread may still be inside a block that writes: finish the files a little later.
    juce::Timer::callAfterDelay (
        150,
        [this, stopBeats, safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe == nullptr)
                return;

            pollMidi(); // the last recorded live events

            auto* device = m_deviceManager.getCurrentAudioDevice();
            const auto latency =
                device != nullptr ? device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() : 0;
            const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
            const auto project = m_session.project();
            // The take's content ticks run at the tempo where it starts (docs/design/tempo-map.md, TM8).
            const auto ticksPerSample =
                project->tempoMap.bpmAt (m_engine.recordStartBeats() * ticksPerBeat) / 60.0 * ticksPerBeat / sampleRate;

            // Shift earlier by the round-trip latency, so the take lines up with what was heard (T9).
            auto start = static_cast<Ticks> (
                std::llround (m_engine.recordStartBeats() * ticksPerBeat - latency * ticksPerSample));
            const auto offset = std::max (Ticks { 0 }, -start);
            start = std::max (Ticks { 0 }, start);

            auto commands = noteRecordingCommands (stopBeats);
            const auto folder = m_projects->projectFolder();
            const auto& loop = project->loop;
            const auto latencyTicks = static_cast<Ticks> (std::llround (latency * ticksPerSample));

            for (size_t i = 0; i < m_takes.size(); ++i)
            {
                auto& take = m_takes[i];
                const auto frames = m_engine.recordedFrames (i);
                take.writer.reset(); // flushes and closes the file

                if (frames <= 0 || m_engine.recordStartBeats() < 0.0)
                {
                    std::ignore = take.file.deleteFile();
                    continue;
                }

                auto args = makeObject (
                    { { "trackId", take.trackId },
                      { "start", start },
                      { "offset", offset },
                      { "recorded", true },
                      { "file",
                        makeObject ({ { "path", take.file.getRelativePathFrom (folder).replaceCharacter ('\\', '/') },
                                      { "name", take.file.getFileNameWithoutExtension() },
                                      { "sampleRate", sampleRate },
                                      { "channels", take.channels },
                                      { "frames", frames } }) } });

                // Loop passes and the punch range (docs/design/takes-lanes.md, T4; audio-features.md, AU11, AU13).
                const auto spans = recordedTakes (
                    static_cast<Ticks> (std::llround (m_engine.recordStartBeats() * ticksPerBeat)),
                    static_cast<Ticks> (std::llround (static_cast<double> (frames) * ticksPerSample)), latencyTicks,
                    loop.enabled && m_engine.recordLoopWraps() > 0 ? std::optional<LoopRange> (loop) : std::nullopt,
                    project->punch.enabled ? std::optional<PunchRange> (project->punch) : std::nullopt);

                for (const auto& span : spans)
                {
                    auto clipArgs = args.clone();
                    auto* object = clipArgs.getDynamicObject();
                    object->setProperty ("start", span.start);
                    object->setProperty ("offset", span.offset);

                    if (span.length > 0)
                        object->setProperty ("length", span.length);

                    commands.add (makeObject ({ { "command", "addAudioClip" }, { "args", clipArgs } }));
                }

                if (spans.empty())
                    std::ignore = take.file.deleteFile();
            }

            m_takes.clear();

            if (! commands.isEmpty())
            {
                const auto result =
                    m_session.apply ("batch", makeObject ({ { "label", "Record" }, { "commands", commands } }));

                if (! result.ok)
                    m_bridge.emit ("dawg_error", result.error);
            }
        });
}

juce::var MainComponent::explorerRoots() const
{
    // The user's favourite folders, then the open project's Audio folder.
    juce::Array<juce::var> roots;
    const auto saved = juce::JSON::parse (m_properties.getUserSettings()->getValue ("soundRoots"));

    if (const auto* list = saved.getArray())
        for (const auto& path : *list)
            roots.add (makeObject (
                { { "name", juce::File (path.toString()).getFileName() }, { "path", path }, { "project", false } }));

    if (m_projects != nullptr)
        if (const auto audio = m_projects->projectFolder().getChildFile ("Audio"); audio.isDirectory())
            roots.add (
                makeObject ({ { "name", "Project audio" }, { "path", audio.getFullPathName() }, { "project", true } }));

    return roots;
}

void MainComponent::addExplorerRoot (const juce::File& folder)
{
    if (folder == juce::File())
        return;

    const auto saved = juce::JSON::parse (m_properties.getUserSettings()->getValue ("soundRoots"));
    juce::Array<juce::var> roots;

    if (const auto* existing = saved.getArray())
        roots = *existing;

    if (! roots.contains (folder.getFullPathName()))
        roots.add (folder.getFullPathName());

    m_properties.getUserSettings()->setValue ("soundRoots", juce::JSON::toString (juce::var (roots), true));
    m_bridge.emit ("dawg_roots", explorerRoots());
}

juce::var MainComponent::listFolder (const juce::File& folder)
{
    // Folders first, then audio files and SFZ patches. Durations follow in a dawg_durations event.
    juce::Array<juce::var> entries;
    std::vector<juce::File> audioFiles;

    // The directory entries say which are folders, so a large folder isn't asked about each file again.
    std::vector<juce::File> folders;
    std::vector<juce::File> files;

    for (const auto& entry : juce::RangedDirectoryIterator (
             folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
        (entry.isDirectory() ? folders : files).push_back (entry.getFile());

    const auto byName = [] (const juce::File& a, const juce::File& b)
    { return a.getFileName().compareNatural (b.getFileName()) < 0; };
    std::ranges::sort (folders, byName);
    std::ranges::sort (files, byName);

    for (const auto& child : folders)
        entries.add (
            makeObject ({ { "name", child.getFileName() }, { "path", child.getFullPathName() }, { "dir", true } }));

    for (const auto& child : files)
    {
        if (! (AudioLibrary::isAudioFile (child) || child.hasFileExtension ("sfz")))
            continue;

        entries.add (
            makeObject ({ { "name", child.getFileName() }, { "path", child.getFullPathName() }, { "dir", false } }));

        // An SFZ patch has no length of its own (docs/design/sampler-mapping.md, SF1).
        if (! child.hasFileExtension ("sfz"))
            audioFiles.push_back (child);
    }

    m_audio->readDurations (std::move (audioFiles),
                            [this] (const juce::var& durations) { m_bridge.emit ("dawg_durations", durations); });
    return makeObject ({ { "path", folder.getFullPathName() }, { "entries", entries } });
}

juce::var MainComponent::searchFiles (const juce::File& folder, const juce::String& query)
{
    // By name, recursively, bounded so huge libraries stay responsive.
    constexpr int maxResults = 300;
    constexpr int maxVisited = 50000;
    juce::Array<juce::var> results;
    int visited = 0;

    for (const auto& entry :
         juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
    {
        if (++visited > maxVisited || results.size() >= maxResults)
            break;

        const auto& file = entry.getFile();

        if ((AudioLibrary::isAudioFile (file) || file.hasFileExtension ("sfz"))
            && file.getFileName().containsIgnoreCase (query))
            results.add (
                makeObject ({ { "name", file.getFileName() }, { "path", file.getFullPathName() }, { "dir", false } }));
    }

    return makeObject ({ { "path", folder.getFullPathName() }, { "entries", results } });
}

void MainComponent::registerExplorerHandlers()
{
    m_bridge.on ("files.roots", [this] (const juce::var&) { return explorerRoots(); });

    m_bridge.on ("files.addRoot",
                 [this] (const juce::var&)
                 {
                     m_audioChooser = std::make_unique<juce::FileChooser> ("Add a folder to the sound explorer");
                     m_audioChooser->launchAsync (
                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this] (const juce::FileChooser& chooser) { addExplorerRoot (chooser.getResult()); });
                     return juce::var();
                 });

    m_bridge.on ("files.removeRoot",
                 [this] (const juce::var& p)
                 {
                     const auto saved = juce::JSON::parse (m_properties.getUserSettings()->getValue ("soundRoots"));
                     juce::Array<juce::var> roots;

                     if (const auto* list = saved.getArray())
                         for (const auto& path : *list)
                             if (path.toString() != p["path"].toString())
                                 roots.add (path);

                     m_properties.getUserSettings()->setValue ("soundRoots",
                                                               juce::JSON::toString (juce::var (roots), true));
                     return explorerRoots();
                 });

    m_bridge.on ("files.list", [this] (const juce::var& p) { return listFolder (juce::File (p["path"].toString())); });
    m_bridge.on ("files.search", [] (const juce::var& p)
                 { return searchFiles (juce::File (p["path"].toString()), p["query"].toString().trim()); });

    m_bridge.on ("preview.play",
                 [this] (const juce::var& p)
                 {
                     auto source = m_audio->openForPreview (juce::File (p["path"].toString()));

                     if (source == nullptr)
                         throw std::runtime_error ("Can't play "
                                                   + juce::File (p["path"].toString()).getFileName().toStdString());

                     m_engine.preview (std::move (source));
                     return juce::var();
                 });

    m_bridge.on ("preview.stop",
                 [this] (const juce::var&)
                 {
                     m_engine.preview (nullptr);
                     return juce::var();
                 });

    m_bridge.on ("preview.setVolume",
                 [this] (const juce::var& p)
                 {
                     m_engine.setPreviewGain (static_cast<float> (static_cast<double> (p["gain"])));
                     return juce::var();
                 });
}

void MainComponent::registerRecordHandlers()
{
    m_bridge.on ("transport.record",
                 [this] (const juce::var& p)
                 {
                     if (m_engine.isRecording())
                         finishRecording();
                     else
                         startRecording (static_cast<bool> (p["countIn"]));

                     return juce::var();
                 });

    // The pre-roll in bars (docs/design/audio-features.md, AU12), an app setting. Without "bars", reads it.
    m_bridge.on ("record.preRoll",
                 [this] (const juce::var& p)
                 {
                     if (p.hasProperty ("bars"))
                         m_properties.getUserSettings()->setValue ("preRollBars",
                                                                   std::clamp (static_cast<int> (p["bars"]), 0, 4));

                     return juce::var (preRollBars());
                 });

    m_bridge.on ("track.arm",
                 [this] (const juce::var& p)
                 {
                     setArmed (static_cast<ObjectId> (p["trackId"]), static_cast<bool> (p["armed"]));
                     return juce::var();
                 });

    m_bridge.on ("audio.inputs",
                 [this] (const juce::var&)
                 {
                     juce::Array<juce::var> names;

                     if (auto* device = m_deviceManager.getCurrentAudioDevice())
                     {
                         const auto all = device->getInputChannelNames();
                         const auto active = device->getActiveInputChannels();

                         for (int ch = 0; ch < all.size(); ++ch)
                             if (active[ch])
                                 names.add (all[ch]);
                     }

                     return juce::var (names);
                 });
}

// Sounds for a Sampler are copied into the project like audio clips' files, then become zones
// (docs/design/sampler-midi-delay-limiter.md, section 4).
void MainComponent::importSamples (std::vector<juce::File> files, ObjectId trackId, ObjectId nodeId,
                                   const juce::var& options)
{
    // A folder brings its audio files, by name, not its subfolders (docs/design/sampler-mapping.md, SM1); an SFZ patch
    // brings its regions (SF1).
    constexpr int mostFiles = 512;
    std::vector<juce::File> expanded;

    for (const auto& file : files)
    {
        if (file.hasFileExtension ("sfz"))
        {
            importSfz (file, trackId, nodeId);
            continue;
        }

        if (! file.isDirectory())
        {
            expanded.push_back (file);
            continue;
        }

        auto children = file.findChildFiles (juce::File::findFiles, false);
        std::sort (children.begin(), children.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });

        for (const auto& child : children)
            if (AudioLibrary::isAudioFile (child) && static_cast<int> (expanded.size()) < mostFiles)
                expanded.push_back (child);
    }

    if (expanded.empty())
    {
        if (std::ranges::none_of (files, [] (const juce::File& f) { return f.hasFileExtension ("sfz"); }))
            m_bridge.emit ("dawg_error", juce::String ("No audio files to add"));

        return;
    }

    // Dropped on a pad or a key, a file goes there; otherwise the files are mapped by their notes (SM1).
    const auto onKey = options.hasProperty ("key") && ! options["key"].isVoid() && ! options["key"].isUndefined();
    const auto mapped = ! onKey && ! options.hasProperty ("replaceZone");

    m_audio->importFiles (
        std::move (expanded),
        [this, trackId, nodeId, options, mapped] (const std::vector<AudioLibrary::Imported>& imported,
                                                  const juce::StringArray& failed)
        {
            if (! failed.isEmpty())
                m_bridge.emit ("dawg_error", "Could not import " + failed.joinIntoString (", "));

            if (imported.empty())
                return;

            juce::Array<juce::var> list;

            for (const auto& file : imported)
                list.add (makeObject ({ { "path", file.path },
                                        { "name", file.name },
                                        { "sampleRate", file.sampleRate },
                                        { "channels", file.channels },
                                        { "frames", file.frames } }));

            auto args = makeObject ({ { "trackId", trackId }, { "nodeId", nodeId }, { "files", list } });
            juce::String mappedHint;

            if (mapped)
            {
                std::vector<SampleToMap> sounds;

                for (const auto& file : imported)
                    sounds.push_back (
                        { file.name, file.slices, file.storedKey, file.title, file.loopStart, file.loopEnd });

                if (const auto plan = planSampleZones (sounds))
                {
                    juce::Array<juce::var> zones;

                    for (const auto& zone : *plan)
                    {
                        auto fields = makeObject ({ { "file", static_cast<int> (zone.file) },
                                                    { "start", zone.start },
                                                    { "end", zone.end },
                                                    { "rootKey", zone.rootKey },
                                                    { "lowKey", zone.lowKey },
                                                    { "highKey", zone.highKey },
                                                    { "lowVelocity", zone.lowVelocity },
                                                    { "highVelocity", zone.highVelocity },
                                                    { "layer", zone.layer },
                                                    { "pan", zone.pan } });

                        // A loop the file stores plays as one (SM5a).
                        if (zone.loopStart >= 0 && zone.loopEnd > zone.loopStart)
                        {
                            fields.getDynamicObject()->setProperty ("mode", "loop");
                            fields.getDynamicObject()->setProperty ("loopStart", zone.loopStart);
                            fields.getDynamicObject()->setProperty ("loopEnd", zone.loopEnd);
                        }

                        zones.add (fields);
                    }

                    args.getDynamicObject()->setProperty ("zones", zones);
                    mappedHint = juce::String (plan->size()) + (plan->size() == 1 ? " zone" : " zones")
                                 + " mapped from " + juce::String (imported.size())
                                 + (imported.size() == 1 ? " file" : " files");
                }
            }

            for (const auto* option : { "key", "replaceZone" })
                if (options.hasProperty (option) && ! options[option].isVoid() && ! options[option].isUndefined())
                    args.getDynamicObject()->setProperty (option, options[option]);

            if (const auto result = m_session.apply ("addSampleZones", args); ! result.ok)
                m_bridge.emit ("dawg_error", result.error);
            else if (mappedHint.isNotEmpty() && imported.size() > 1)
                m_bridge.emit ("dawg_hint", mappedHint);
        },
        mapped);
}

// An SFZ patch's regions as zones, after those there are (docs/design/sampler-mapping.md, section SF).
void MainComponent::importSfz (const juce::File& patchFile, ObjectId trackId, ObjectId nodeId)
{
    auto patch = readSfzFile (patchFile);

    if (patch.regions.empty())
    {
        m_bridge.emit ("dawg_error", "No samples to add from " + patchFile.getFileName());
        return;
    }

    std::vector<juce::File> samples;

    for (const auto& region : patch.regions)
        if (std::ranges::find (samples, region.sample) == samples.end())
            samples.push_back (region.sample);

    m_audio->importFiles (
        std::move (samples),
        [this, trackId, nodeId, patch = std::move (patch), name = patchFile.getFileName()] (
            const std::vector<AudioLibrary::Imported>& imported, const juce::StringArray& failed)
        {
            if (! failed.isEmpty())
                m_bridge.emit ("dawg_error", "Could not import " + failed.joinIntoString (", "));

            juce::Array<juce::var> list;
            std::map<juce::String, int> index;

            for (const auto& file : imported)
            {
                index[file.source] = list.size();
                list.add (makeObject ({ { "path", file.path },
                                        { "name", file.name },
                                        { "sampleRate", file.sampleRate },
                                        { "channels", file.channels },
                                        { "frames", file.frames } }));
            }

            juce::Array<juce::var> zones;

            for (const auto& region : patch.regions)
            {
                const auto found = index.find (region.sample.getFullPathName());

                if (found == index.end())
                    continue;

                auto fields = region.fields.clone();
                fields.getDynamicObject()->setProperty ("file", found->second);
                zones.add (fields);
            }

            if (zones.isEmpty())
                return;

            juce::Array<juce::var> commands;
            commands.add (makeObject (
                { { "command", "addSampleZones" },
                  { "args",
                    makeObject (
                        { { "trackId", trackId }, { "nodeId", nodeId }, { "files", list }, { "zones", zones } }) } }));

            if (patch.randomTakes)
                commands.add (makeObject ({ { "command", "setNodeParam" },
                                            { "args", makeObject ({ { "trackId", trackId },
                                                                    { "nodeId", nodeId },
                                                                    { "param", "takes" },
                                                                    { "value", 1.0 } }) } }));

            if (patch.velocityTracking >= 0.0)
                commands.add (makeObject ({ { "command", "setNodeParam" },
                                            { "args", makeObject ({ { "trackId", trackId },
                                                                    { "nodeId", nodeId },
                                                                    { "param", "velocity" },
                                                                    { "value", patch.velocityTracking } }) } }));

            const auto args = makeObject ({ { "label", "Add " + name }, { "commands", commands } });

            if (const auto result = m_session.apply ("batch", args); ! result.ok)
            {
                m_bridge.emit ("dawg_error", result.error);
                return;
            }

            // What came in, and what didn't (SF6).
            auto hint = juce::String (zones.size()) + " zones from " + name;

            if (patch.skipped > 0)
                hint << "; " << patch.skipped << (patch.skipped == 1 ? " region" : " regions")
                     << " left out (release triggers or missing samples)";

            if (! patch.ignored.isEmpty())
            {
                auto shown = patch.ignored;
                shown.removeRange (6, shown.size());
                hint << "; not used: " << shown.joinIntoString (", ") << (patch.ignored.size() > 6 ? "…" : "");
            }

            m_bridge.emit ("dawg_hint", hint);
        });
}

void MainComponent::importFiles (std::vector<juce::File> files, ObjectId trackId, Ticks start)
{
    m_audio->importFiles (
        std::move (files),
        [this, trackId, start] (const std::vector<AudioLibrary::Imported>& imported, const juce::StringArray& failed)
        {
            if (! failed.isEmpty())
                m_bridge.emit ("dawg_error", "Could not import " + failed.joinIntoString (", "));

            if (imported.empty())
                return;

            // On an audio track the files follow each other; otherwise each gets a new audio track.
            const auto* track = m_session.project()->findTrack (trackId);
            const auto onTrack = track != nullptr && track->kind == TrackKind::audio;
            const auto snapshot = m_session.project();
            const auto& map = snapshot->tempoMap;
            juce::Array<juce::var> commands;
            auto position = start;

            for (const auto& file : imported)
            {
                auto args = makeObject ({
                    { "start", position },
                    { "file", makeObject ({ { "path", file.path },
                                            { "name", file.name },
                                            { "sampleRate", file.sampleRate },
                                            { "channels", file.channels },
                                            { "frames", file.frames },
                                            { "metadataBpm", file.metadataBpm } }) },
                });

                if (onTrack)
                    args.getDynamicObject()->setProperty ("trackId", trackId);
                else
                    args.getDynamicObject()->setProperty ("newTrack", true);

                commands.add (makeObject ({ { "command", "addAudioClip" }, { "args", args } }));

                if (onTrack)
                {
                    const auto end = map.secondsAt (static_cast<double> (position))
                                     + static_cast<double> (file.frames) / file.sampleRate;
                    position = static_cast<Ticks> (std::llround (map.tickAt (end)));
                }
            }

            const auto result =
                m_session.apply ("batch", makeObject ({ { "label", "Import audio" }, { "commands", commands } }));

            if (! result.ok)
                m_bridge.emit ("dawg_error", result.error);
        });
}

// A Convolution Reverb's impulse response, copied into the pool like a sample (docs/design/convolution-reverb.md, I10).
void MainComponent::importImpulse (juce::File file, ObjectId trackId, ObjectId nodeId)
{
    m_audio->importFiles (
        { std::move (file) },
        [this, trackId, nodeId] (const std::vector<AudioLibrary::Imported>& imported, const juce::StringArray& failed)
        {
            if (! failed.isEmpty())
                m_bridge.emit ("dawg_error", "Could not import " + failed.joinIntoString (", "));

            if (imported.empty())
                return;

            const auto& first = imported.front();
            const auto fileArgs = makeObject ({ { "path", first.path },
                                                { "name", first.name },
                                                { "sampleRate", first.sampleRate },
                                                { "channels", first.channels },
                                                { "frames", first.frames } });

            if (const auto result = m_session.apply (
                    "setNodeFile", makeObject ({ { "trackId", trackId }, { "nodeId", nodeId }, { "file", fileArgs } }));
                ! result.ok)
                m_bridge.emit ("dawg_error", result.error);
        });
}

void MainComponent::locateMissing (ObjectId fileId)
{
    const auto* missing = m_session.project()->findAudioFile (fileId);

    if (missing == nullptr)
        return;

    m_audioChooser = std::make_unique<juce::FileChooser> ("Locate " + missing->name, juce::File(),
                                                          "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    m_audioChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, fileId] (const juce::FileChooser& chooser)
        {
            const auto found = chooser.getResult();

            if (found == juce::File())
                return;

            // The chosen file, and other missing files with the same names in that folder (T11).
            juce::Array<juce::var> commands;
            const auto audioFolder = m_projects->projectFolder().getChildFile ("Audio");
            std::ignore = audioFolder.createDirectory();

            for (const auto& file : m_audio->missing (*m_session.project()))
            {
                const auto candidate =
                    file.id == fileId ? found
                                      : found.getParentDirectory().getChildFile (juce::File (file.path).getFileName());

                if (! candidate.existsAsFile())
                    continue;

                auto target = audioFolder.getChildFile (candidate.getFileName());

                if (target != candidate && ! target.existsAsFile())
                    std::ignore = candidate.copyFileTo (target);

                commands.add (makeObject (
                    { { "command", "relinkAudioFile" },
                      { "args", makeObject ({ { "id", file.id }, { "path", "Audio/" + target.getFileName() } }) } }));
            }

            if (! commands.isEmpty())
                std::ignore = m_session.apply (
                    "batch", makeObject ({ { "label", "Locate audio files" }, { "commands", commands } }));
        });
}

void MainComponent::registerAudioHandlers()
{
    const auto target = [] (const juce::var& p)
    {
        return std::pair { static_cast<ObjectId> (p["trackId"]),
                           static_cast<Ticks> (static_cast<double> (p["start"])) };
    };

    m_bridge.on ("audio.importDialog",
                 [this, target] (const juce::var& p)
                 {
                     const auto [trackId, start] = target (p);
                     m_audioChooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File(),
                                                                           "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
                     m_audioChooser->launchAsync (
                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this, trackId, start] (const juce::FileChooser& chooser)
                         {
                             const auto results = chooser.getResults();
                             if (! results.isEmpty())
                                 importFiles ({ results.begin(), results.end() }, trackId, start);
                         });
                     return juce::var();
                 });

    m_bridge.on ("sampler.import",
                 [this] (const juce::var& p)
                 {
                     std::vector<juce::File> files;

                     if (const auto* paths = p["paths"].getArray())
                         for (const auto& path : *paths)
                             files.emplace_back (path.toString());

                     importSamples (std::move (files), static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"])),
                                    static_cast<ObjectId> (static_cast<juce::int64> (p["nodeId"])), p);
                     return juce::var();
                 });

    m_bridge.on ("sampler.importDialog",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"]));
                     const auto nodeId = static_cast<ObjectId> (static_cast<juce::int64> (p["nodeId"]));
                     m_samplerChooser = std::make_unique<juce::FileChooser> (
                         "Add samples", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.sfz");
                     m_samplerChooser->launchAsync (
                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this, trackId, nodeId, p] (const juce::FileChooser& chooser)
                         {
                             const auto results = chooser.getResults();
                             if (! results.isEmpty())
                                 importSamples ({ results.begin(), results.end() }, trackId, nodeId, p);
                         });
                     return juce::var();
                 });

    // A drop takes its first file (docs/design/convolution-reverb.md, I10).
    m_bridge.on ("convolution.import",
                 [this] (const juce::var& p)
                 {
                     if (const auto* paths = p["paths"].getArray(); paths != nullptr && ! paths->isEmpty())
                         importImpulse (juce::File (paths->getFirst().toString()),
                                        static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"])),
                                        static_cast<ObjectId> (static_cast<juce::int64> (p["nodeId"])));

                     return juce::var();
                 });

    m_bridge.on ("convolution.importDialog",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (static_cast<juce::int64> (p["trackId"]));
                     const auto nodeId = static_cast<ObjectId> (static_cast<juce::int64> (p["nodeId"]));
                     m_samplerChooser = std::make_unique<juce::FileChooser> ("Load impulse response", juce::File(),
                                                                             "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
                     m_samplerChooser->launchAsync (
                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, trackId, nodeId] (const juce::FileChooser& chooser)
                         {
                             if (const auto result = chooser.getResult(); result != juce::File())
                                 importImpulse (result, trackId, nodeId);
                         });
                     return juce::var();
                 });

    m_bridge.on ("audio.import",
                 [this, target] (const juce::var& p)
                 {
                     const auto [trackId, start] = target (p);
                     std::vector<juce::File> files;

                     if (const auto* paths = p["paths"].getArray())
                         for (const auto& path : *paths)
                             files.emplace_back (path.toString());

                     importFiles (std::move (files), trackId, start);
                     return juce::var();
                 });

    // Files dropped from the OS reach the web UI without a path; it sends their bytes in chunks.
    m_bridge.on ("audio.upload",
                 [this, target] (const juce::var& p)
                 {
                     const auto id = p["id"].toString();

                     if (! isValidUploadId (id))
                         throw std::runtime_error ("Bad upload id");

                     auto& file = m_uploads[id];

                     if (file == juce::File())
                         file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("dawg-upload-" + id)
                                    .getChildFile (juce::File::createLegalFileName (p["name"].toString()));

                     const auto folder = file.getParentDirectory();
                     std::ignore = folder.createDirectory();
                     juce::MemoryOutputStream decoded;

                     if (! juce::Base64::convertFromBase64 (decoded, p["data"].toString()))
                     {
                         m_uploads.erase (id);
                         std::ignore = folder.deleteRecursively();
                         throw std::runtime_error ("Bad upload data");
                     }

                     if (! file.appendData (decoded.getData(), decoded.getDataSize()))
                     {
                         m_uploads.erase (id);
                         std::ignore = folder.deleteRecursively();
                         throw std::runtime_error ("Could not store the upload");
                     }

                     if (static_cast<bool> (p["last"]))
                     {
                         const auto [trackId, start] = target (p);
                         const auto uploaded = file;
                         m_uploads.erase (id);
                         importFiles ({ uploaded }, trackId, start);
                     }

                     return juce::var();
                 });
}

void MainComponent::registerAudioFileHandlers()
{
    m_bridge.on ("audio.peaks",
                 [this] (const juce::var& p)
                 {
                     juce::File file;

                     if (p.hasProperty ("fileId"))
                     {
                         if (const auto* entry =
                                 m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"])))
                             file = m_audio->locate (*entry);
                     }
                     else
                     {
                         file = juce::File (p["path"].toString());
                     }

                     auto result = m_audio->peaks (file, static_cast<int> (p["level"]));
                     result.getDynamicObject()->setProperty ("path", file.getFullPathName());
                     return result;
                 });

    // The stretch cache (docs/design/time-stretching.md, TS8).
    m_bridge.on ("audio.stretchCache",
                 [this] (const juce::var&)
                 {
                     return makeObject ({ { "size", m_stretch->cacheSize() },
                                          { "limit", m_properties.getUserSettings()
                                                         ->getValue ("stretchCacheLimit",
                                                                     juce::String (StretchRenderer::defaultSizeLimit))
                                                         .getLargeIntValue() } });
                 });

    m_bridge.on ("audio.setStretchCacheLimit",
                 [this] (const juce::var& p)
                 {
                     const auto bytes = std::max (juce::int64 { 0 }, static_cast<juce::int64> (p["bytes"]));
                     m_properties.getUserSettings()->setValue ("stretchCacheLimit", juce::String (bytes));
                     m_stretch->setSizeLimit (bytes);
                     return juce::var();
                 });

    // A drag that changes renders (a timing marker, the transpose) holds them back until it ends (TS9).
    m_bridge.on ("stretch.hold",
                 [this] (const juce::var& p)
                 {
                     m_stretch->setHeld (static_cast<bool> (p["held"]));
                     return juce::var();
                 });

    m_bridge.on ("audio.clearStretchCache",
                 [this] (const juce::var&)
                 {
                     m_stretch->clear();
                     recompileEngine(); // what was playing renders again
                     return juce::var();
                 });

    // Raw frames for drawing the wave when zoomed in past the peaks.
    m_bridge.on ("audio.samples",
                 [this] (const juce::var& p)
                 {
                     // A pool file, or a file on disk by its path (the sound browser's preview), as for the peaks.
                     juce::File file;

                     if (p.hasProperty ("fileId"))
                     {
                         const auto* entry = m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"]));

                         if (entry == nullptr)
                             throw std::runtime_error ("No such audio file");

                         file = m_audio->locate (*entry);
                     }
                     else
                     {
                         file = juce::File (p["path"].toString());
                     }

                     return m_audio->samples (file, static_cast<juce::int64> (p["start"]),
                                              static_cast<int> (p["count"]));
                 });

    // A zone's pitch, for its Detect pitch (docs/design/sampler-mapping.md, SM4): MIDI semitones, or 0 for none.
    m_bridge.on ("sampler.detectPitch",
                 [this] (const juce::var& p)
                 {
                     const auto* entry = m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"]));

                     if (entry == nullptr)
                         throw std::runtime_error ("No such audio file");

                     return juce::var (m_audio->pitchOf (m_audio->locate (*entry),
                                                         static_cast<juce::int64> (p["start"]),
                                                         static_cast<juce::int64> (p["end"])));
                 });

    // Onsets for the audio editor's transient lines and marker snapping (TS11, TS13).
    m_bridge.on ("audio.onsets",
                 [this] (const juce::var& p)
                 {
                     const auto* entry = m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"]));

                     if (entry == nullptr)
                         throw std::runtime_error ("No such audio file");

                     const auto file = m_audio->locate (*entry);
                     auto result = m_audio->onsets (file);
                     result.getDynamicObject()->setProperty ("path", file.getFullPathName());
                     return result;
                 });

    // A pool file's pitch analysis for the audio editor's Pitch view (docs/design/pitch-correction.md, section 3.6).
    m_bridge.on ("audio.pitchAnalysis",
                 [this] (const juce::var& p)
                 {
                     const auto* entry = m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"]));

                     if (entry == nullptr)
                         throw std::runtime_error ("No such audio file");

                     const auto file = m_audio->locate (*entry);
                     auto result = m_audio->pitchAnalysis (file);
                     result.getDynamicObject()->setProperty ("path", file.getFullPathName());
                     return result;
                 });

    // Plays one note of a pool file (docs/design/pitch-correction.md, step 3's audition): its lead-in to its end, as
    // corrected where the phrase render is ready, through the preview player.
    m_bridge.on ("audio.auditionNote",
                 [this] (const juce::var& p)
                 {
                     const auto* entry = m_session.project()->findAudioFile (static_cast<ObjectId> (p["fileId"]));

                     if (entry == nullptr)
                         throw std::runtime_error ("No such audio file");

                     const auto* note = entry->pitch.find (static_cast<juce::int64> (p["noteId"]));

                     if (note == nullptr)
                         throw std::runtime_error ("No such note");

                     std::shared_ptr<const AudioSource> source = m_audio->openForPreview (m_audio->locate (*entry));

                     if (source == nullptr)
                         throw std::runtime_error ("Can't play " + entry->name.toStdString());

                     auto pending = 0;

                     if (entry->pitch.edited())
                         if (auto overlays = m_stretch->pitchOverlays (*entry, 0, pending); ! overlays.empty())
                             source = AudioSource::withOverlays (source, std::move (overlays));

                     // At most 30 s, with 5 ms fades so the cut doesn't click.
                     const auto start = note->leadStart;
                     const auto count = static_cast<int> (std::min<juce::int64> (
                         note->end - start, static_cast<juce::int64> (30.0 * source->sampleRate())));
                     juce::AudioBuffer<float> audio (2, std::max (1, count));
                     std::ignore = source->readBlocking ({ audio.getWritePointer (0), audio.getWritePointer (1) },
                                                         start, audio.getNumSamples());
                     const auto fade =
                         std::min (audio.getNumSamples() / 2, static_cast<int> (0.005 * source->sampleRate()));
                     audio.applyGainRamp (0, fade, 0.0f, 1.0f);
                     audio.applyGainRamp (audio.getNumSamples() - fade, fade, 1.0f, 0.0f);
                     m_engine.preview (AudioSource::fromBuffer (std::move (audio), source->sampleRate()));
                     return juce::var();
                 });

    m_bridge.on ("audio.missing",
                 [this] (const juce::var&)
                 {
                     juce::Array<juce::var> result;

                     for (const auto& file : m_audio->missing (*m_session.project()))
                         result.add (makeObject ({ { "id", file.id }, { "name", file.name }, { "path", file.path } }));

                     return juce::var (result);
                 });

    m_bridge.on ("audio.locate",
                 [this] (const juce::var& p)
                 {
                     locateMissing (static_cast<ObjectId> (p["fileId"]));
                     return juce::var();
                 });
}

void MainComponent::registerAudioClipHandlers()
{
    // Reverse (docs/design/audio-features.md, AU1): the copies are written first, then reverseClips points the clips
    // at them in one undo step.
    m_bridge.on ("audio.reverseClips",
                 [this] (const juce::var& p)
                 {
                     const auto project = m_session.project();
                     const auto& clipIds = p["clipIds"];
                     std::vector<AudioFile> needed;

                     for (const auto& clip : audioClipsOf (*project, clipIds))
                     {
                         const auto* file = project->findAudioFile (clip->fileId);
                         const auto hasCopy = file->reverseOf != 0
                                              || std::ranges::any_of (project->audioFiles, [file] (const AudioFile& f)
                                                                      { return f.reverseOf == file->id; });

                         if (! hasCopy
                             && std::ranges::none_of (needed, [file] (const AudioFile& f) { return f.id == file->id; }))
                             needed.push_back (*file);
                     }

                     const auto apply = [this, clipIds] (const juce::var& files)
                     {
                         const auto result = m_session.apply (
                             "reverseClips", makeObject ({ { "clipIds", clipIds }, { "files", files } }));

                         if (! result.ok)
                             m_bridge.emit ("dawg_error", result.error);
                     };

                     if (needed.empty())
                     {
                         apply (makeObject ({}));
                         return juce::var();
                     }

                     m_audio->reverseFiles (
                         needed,
                         [this, apply] (const std::map<ObjectId, AudioLibrary::Imported>& written,
                                        const juce::StringArray& failed)
                         {
                             if (! failed.isEmpty())
                             {
                                 m_bridge.emit ("dawg_error", "Could not reverse " + failed.joinIntoString (", "));
                                 return;
                             }

                             const auto files = makeObject ({});

                             for (const auto& [id, file] : written)
                                 files.getDynamicObject()->setProperty (juce::String (id),
                                                                        makeObject ({ { "path", file.path },
                                                                                      { "name", file.name },
                                                                                      { "sampleRate", file.sampleRate },
                                                                                      { "channels", file.channels },
                                                                                      { "frames", file.frames } }));
                             apply (files);
                         });

                     return juce::var();
                 });

    // Slice to Sampler (AU3): where a clip would be cut, at transients (the file's onsets, found on first use), the
    // grid or the timing markers. { ready: false } while the onsets are still being found (dawg_onsets when done).
    m_bridge.on (
        "audio.slicePoints",
        [this] (const juce::var& p)
        {
            const auto project = m_session.project();
            const auto clips = audioClipsOf (*project, juce::Array<juce::var> { p["clipId"] });

            if (clips.empty())
                throw std::runtime_error ("No such audio clip");

            const auto& clip = *clips.front();
            const auto& file = *project->findAudioFile (clip.fileId);
            const ClipFrames frames (clip, file, project->tempoMap);
            const auto range = frames.range();
            const auto mode = p["mode"].toString();
            std::vector<juce::int64> cuts;

            if (mode == "grid")
            {
                cuts = gridSlices (frames, clip,
                                   std::max<Ticks> (1, static_cast<Ticks> (static_cast<juce::int64> (p["division"]))));
            }
            else if (mode == "markers")
            {
                for (const auto& marker : file.timing.markers)
                    if (const auto frame = static_cast<juce::int64> (std::llround (marker.frame));
                        frame > range.first && frame < range.second)
                        cuts.push_back (frame);
            }
            else
            {
                const auto found = m_audio->onsets (m_audio->locate (file));

                if (! static_cast<bool> (found["ready"]))
                    return makeObject ({ { "ready", false } });

                std::vector<std::pair<juce::int64, float>> onsets;

                if (const auto* list = found["onsets"].getArray())
                    for (const auto& onset : *list)
                        onsets.emplace_back (static_cast<juce::int64> (onset[0]),
                                             static_cast<float> (static_cast<double> (onset[1])));

                const auto minFrames = static_cast<juce::int64> (0.03 * file.sampleRate); // 30 ms
                cuts = onsetSlices (onsets, range, static_cast<double> (p.getProperty ("sensitivity", 0.5)), minFrames);
            }

            juce::Array<juce::var> list;
            for (const auto cut : cuts)
                list.add (cut);

            return makeObject ({ { "ready", true },
                                 { "slices", list },
                                 { "range", juce::Array<juce::var> { range.first, range.second } },
                                 { "sampleRate", file.sampleRate } });
        });
}

void MainComponent::registerAudioAnalysisHandlers()
{
    // Audio to MIDI (AU4): drums (onsets) or monophonic pitch, found in the background in what the clip plays, onto a
    // new instrument track after the clip's track.
    m_bridge.on (
        "audio.toMidi",
        [this] (const juce::var& p)
        {
            const auto project = m_session.project();
            const auto clips = audioClipsOf (*project, juce::Array<juce::var> { p["clipId"] });

            if (clips.empty())
                throw std::runtime_error ("No such audio clip");

            const auto clip = *clips.front();

            if (clip.loopLength > 0)
                throw std::runtime_error ("Unloop the clip to convert it");

            const auto& file = *project->findAudioFile (clip.fileId);
            const auto frames = std::make_shared<const ClipFrames> (clip, file, project->tempoMap);
            const auto range = frames->range();
            const auto drums = p["kind"].toString() == "drums";
            const auto sensitivity = static_cast<double> (p.getProperty ("sensitivity", 0.5));
            const auto split = static_cast<bool> (p["split"]);
            const auto curves = static_cast<bool> (p.getProperty ("curves", true));
            ObjectId trackId = 0;

            for (const auto& track : project->tracks)
                if (track->indexOfClip (clip.id) >= 0)
                    trackId = track->id;

            const auto name = (clip.name.isNotEmpty() ? clip.name : file.name) + (drums ? " drums" : " notes");
            m_bridge.emit (
                "dawg_render",
                makeObject ({ { "state", "rendering" }, { "label", "Finding notes" }, { "progress", 0.0 } }));

            m_audio->analyse (
                m_audio->locate (file), range.first, range.second - range.first,
                [project, frames, range, drums, sensitivity, split, curves,
                 start = clip.start] (const juce::AudioBuffer<float>& audio, double rate)
                {
                    const auto notes =
                        drums ? drumNotes (audio, rate, sensitivity, split) : pitchNotes (detectPitch (audio, rate));
                    return notesVar (notes, *frames, start, range.first, rate, curves && ! drums);
                },
                [this, trackId, name, drums, start = clip.start, length = clip.length] (const juce::var& notes)
                {
                    m_bridge.emit ("dawg_render", makeObject ({ { "state", "done" } }));

                    if (! notes.isArray() || notes.getArray()->isEmpty())
                    {
                        m_bridge.emit ("dawg_error",
                                       juce::String (notes.isArray() ? "No notes found" : "Could not read the audio"));
                        return;
                    }

                    const auto result = m_session.apply ("addNoteTrack", makeObject ({ { "after", trackId },
                                                                                       { "name", name },
                                                                                       { "start", start },
                                                                                       { "length", length },
                                                                                       { "notes", notes },
                                                                                       { "drums", drums } }));

                    if (! result.ok)
                        m_bridge.emit ("dawg_error", result.error);
                });

            return juce::var();
        });
}

void MainComponent::registerAudioNormaliseHandlers()
{
    // Normalise (AU2): the clips' peaks are measured in the background, then their gains are set in one undo step.
    m_bridge.on (
        "audio.normaliseClips",
        [this] (const juce::var& p)
        {
            const auto project = m_session.project();
            const auto targetDb = static_cast<double> (p.getProperty ("targetDb", 0.0));
            const auto together = static_cast<bool> (p["together"]);
            std::vector<AudioLibrary::PeakRequest> requests;
            std::vector<ObjectId> ids;

            for (const auto& clip : audioClipsOf (*project, p["clipIds"]))
            {
                const auto& file = *project->findAudioFile (clip->fileId);
                requests.push_back ({ m_audio->locate (file), playedFrameRanges (*clip, file, project->tempoMap) });
                ids.push_back (clip->id);
            }

            if (ids.empty())
                return juce::var();

            m_audio->peaks (
                std::move (requests),
                [this, ids, targetDb, together] (const std::vector<float>& peaks)
                {
                    const auto changes = normaliseChanges (peaks, targetDb, together);
                    const auto now = m_session.project();
                    juce::Array<juce::var> commands;

                    for (size_t i = 0; i < ids.size(); ++i)
                    {
                        const auto clips = audioClipsOf (*now, juce::Array<juce::var> { ids[i] });

                        if (clips.empty() || juce::exactlyEqual (changes[i], 0.0))
                            continue;

                        const auto gain =
                            std::clamp (clips[0]->gainDb + changes[i], limits::minGainDb, limits::maxGainDb);
                        commands.add (
                            makeObject ({ { "command", "setClipProps" },
                                          { "args", makeObject ({ { "id", ids[i] }, { "gainDb", gain } }) } }));
                    }

                    if (commands.isEmpty())
                        return;

                    const auto result =
                        m_session.apply ("batch", makeObject ({ { "label", "Normalise" }, { "commands", commands } }));

                    if (! result.ok)
                        m_bridge.emit ("dawg_error", result.error);
                });

            return juce::var();
        });
}

void MainComponent::registerProjectHandlers()
{
    const auto mode = [] (const juce::var& p)
    {
        const auto name = p["mode"].toString();

        if (name == "save")
            return ProjectManager::Mode::save;

        return name == "discard" ? ProjectManager::Mode::discard : ProjectManager::Mode::ask;
    };

    m_bridge.on ("project.state", [this] (const juce::var&) { return m_projects->stateVar(); });
    m_bridge.on ("project.recent", [this] (const juce::var&) { return m_projects->recent(); });
    m_bridge.on ("project.templates", [this] (const juce::var&) { return m_projects->templates(); });

    m_bridge.on ("project.new", [this, mode] (const juce::var& p)
                 { return m_projects->newProject (p["template"].toString(), mode (p)); });
    m_bridge.on ("project.open",
                 [this, mode] (const juce::var& p) { return m_projects->open (p["path"].toString(), mode (p)); });
    m_bridge.on ("project.close", [this, mode] (const juce::var& p) { return m_projects->close (mode (p)); });
    m_bridge.on ("project.quit", [this, mode] (const juce::var& p) { return m_projects->requestQuit (mode (p)); });

    m_bridge.on ("project.save",
                 [this] (const juce::var& p)
                 {
                     if (static_cast<bool> (p["saveAs"]))
                         m_projects->saveAs();
                     else
                         m_projects->save();

                     return juce::var();
                 });

    m_bridge.on ("project.saveAsTemplate",
                 [this] (const juce::var& p)
                 {
                     m_projects->saveAsTemplate (p["name"].toString());
                     return juce::var();
                 });

    m_bridge.on ("project.recover",
                 [this] (const juce::var& p)
                 {
                     m_projects->recover (static_cast<bool> (p["accept"]));
                     return juce::var();
                 });

    m_bridge.on ("project.removeRecent",
                 [this] (const juce::var& p)
                 {
                     m_projects->removeRecent (p["path"].toString());
                     return juce::var();
                 });
}

void MainComponent::registerPluginHandlers()
{
    m_bridge.on ("plugins.status", [this] (const juce::var&) { return pluginStatus(); });
    m_bridge.on ("nodes.widths", [this] (const juce::var&) { return portWidths(); });

    // Node types for the graph view (docs/design/phase8-node-graph.md, G2).
    m_bridge.on ("nodes.types",
                 [] (const juce::var&)
                 {
                     juce::Array<juce::var> types;

                     for (const auto* descriptor : NodeRegistry::builtin().descriptors())
                         types.add (toVar (*descriptor));

                     return juce::var (types);
                 });

    m_bridge.on ("plugins.options",
                 [this] (const juce::var&)
                 {
                     const auto options = newObject();
                     options->setProperty ("available", remotePluginsAvailable());
                     options->setProperty ("outOfProcess", m_pluginHost->outOfProcess());
                     return juce::var (options.get());
                 });

    m_bridge.on ("plugins.setOutOfProcess",
                 [this] (const juce::var& p)
                 {
                     const auto enabled = static_cast<bool> (p["enabled"]);
                     m_properties.getUserSettings()->setValue ("pluginsOutOfProcess", enabled);
                     m_pluginHost->setOutOfProcess (enabled);
                     restartPlugins ([] (const PluginRef&) { return true; });
                     return juce::var();
                 });

    m_bridge.on ("plugin.setRunInside",
                 [this] (const juce::var& p)
                 {
                     const auto identifier = p["identifier"].toString();
                     auto inside = m_pluginHost->runInside();
                     inside.removeString (identifier);

                     if (static_cast<bool> (p["inside"]))
                         inside.add (identifier);

                     m_properties.getUserSettings()->setValue ("pluginsRunInside", inside.joinIntoString ("\n"));
                     m_pluginHost->setRunInside (inside);
                     restartPlugins ([&] (const PluginRef& plugin) { return plugin.identifier == identifier; });
                     return juce::var();
                 });

    m_bridge.on ("plugin.reload",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (p["trackId"]);
                     const auto nodeId = static_cast<ObjectId> (p["nodeId"]);
                     const auto* track = m_session.project()->findTrackOrMaster (trackId);
                     const auto* model = track != nullptr ? track->findNode (nodeId) : nullptr;

                     if (model == nullptr)
                         throw std::runtime_error ("No such plugin node");

                     const auto shared = m_compiler.findPluginInstance (trackId, nodeId);

                     if (shared == nullptr)
                         throw std::runtime_error ("No such plugin node");

                     m_pluginHost->reload (trackId, *model, shared);
                     return juce::var();
                 });

    m_bridge.on ("plugin.openEditor",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (p["trackId"]);
                     const auto nodeId = static_cast<ObjectId> (p["nodeId"]);
                     auto& node = requirePluginNode (trackId, nodeId);

                     if (node.processor() == nullptr)
                         throw std::runtime_error ("The plugin is not loaded");

                     const auto* track = m_session.project()->findTrackOrMaster (trackId);
                     const auto* found = track != nullptr ? track->findNode (nodeId) : nullptr;

                     if (found == nullptr)
                         throw std::runtime_error ("No such plugin node");

                     const auto& model = *found;
                     const auto pluginName =
                         model.plugin.has_value() ? model.plugin->name : node.processor()->getName();

                     m_pluginWindows->open ({ trackId, nodeId }, *node.processor(), track->name + " - " + pluginName,
                                            model.plugin.has_value() ? model.plugin->identifier : pluginName);
                     emitPluginStatus();
                     return juce::var();
                 });

    m_bridge.on ("node.clear",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (p["trackId"]);
                     const auto nodeId = static_cast<ObjectId> (p["nodeId"]);

                     if (auto* node = m_compiler.findInstance (trackId, nodeId))
                         node->requestClear();

                     for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
                         if (auto* node = m_compiler.findVoiceInstance (trackId, nodeId, voice))
                             node->requestClear();

                     return juce::var();
                 });

    m_bridge.on ("plugin.closeEditor",
                 [this] (const juce::var& p)
                 {
                     const auto trackId = static_cast<ObjectId> (p["trackId"]);
                     const auto nodeId = static_cast<ObjectId> (p["nodeId"]);
                     m_pluginWindows->close ({ trackId, nodeId });
                     stampPlugin (trackId, nodeId);
                     emitPluginStatus();
                     return juce::var();
                 });
}

juce::var MainComponent::audioDeviceInfo() const
{
    auto* device = m_deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return makeObject ({ { "open", false } });

    return makeObject ({ { "open", true },
                         { "type", device->getTypeName() },
                         { "name", device->getName() },
                         { "sampleRate", device->getCurrentSampleRate() },
                         { "bufferSize", device->getCurrentBufferSizeSamples() },
                         { "outputLatency", device->getOutputLatencyInSamples() } });
}

void MainComponent::openAudioSettings()
{
    constexpr int dialogWidth = 520;
    constexpr int dialogHeight = 480;

    auto selector =
        std::make_unique<juce::AudioDeviceSelectorComponent> (m_deviceManager, 0, 2, 0, 2, true, true, true, false);
    selector->setSize (dialogWidth, dialogHeight);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = backgroundColour;
    options.componentToCentreAround = this;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    std::ignore = options.launchAsync();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* /*source*/)
{
    // The audio device changed: persist it and tell the UI.
    if (auto* device = m_deviceManager.getCurrentAudioDevice())
        applog::write ("Audio device: " + device->getName() + ", " + juce::String (device->getCurrentSampleRate())
                       + " Hz, " + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
    else
        applog::write ("Audio device: none");

    if (const auto xml = m_deviceManager.createStateXml())
        m_properties.getUserSettings()->setValue (audioStateKey, xml.get());

    m_properties.saveIfNeeded();
    updateEngine(); // recompiles if the sample rate changed
    m_bridge.emit ("dawg_audio", audioDeviceInfo());
}

void MainComponent::checkAllocationGuard()
{
    const auto violations = allocationViolationCount();

    if (violations == m_reportedAllocationViolations)
        return;

    DBG ("Audio thread allocated memory " << static_cast<int> (violations - m_reportedAllocationViolations)
                                          << " time(s). See docs/CODING_GUIDELINES.md, section 4.");
    m_reportedAllocationViolations = violations;
    jassertfalse;
}

namespace
{
// Why output protection muted the device output, for the UI (docs/design/output-protection.md, S3), or null.
[[nodiscard]] juce::var outputMuteName (OutputMute mute)
{
    switch (mute)
    {
        case OutputMute::overload:
            return "overload";
        case OutputMute::extreme:
            return "extreme";
        case OutputMute::none:
            break;
    }

    return {};
}
} // namespace

void MainComponent::timerCallback()
{
    m_engine.collectGarbage();
    checkAllocationGuard();
    pollMidi();
    pollAutomation();

    const auto* window = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent());
    // Late blocks change without anything else happening. A second's delay is fine for showing them.
    if (const auto now = juce::Time::getMillisecondCounter(); now - m_lastLateCheck > 1000)
    {
        m_lastLateCheck = now;
        emitPluginStatus();
    }

    const auto appActive = juce::Process::isForegroundProcess() || remoteHelperIsForeground();
    m_pluginWindows->update (appActive, window != nullptr && window->isMinimised());
    checkUiHeartbeat (appActive, window != nullptr && window->isMinimised());

    // The instrument window stays above the main one while DAWg is in front, like plugin windows, so a sound dragged
    // from the main window's browser onto it doesn't hide it.
    if (m_instrumentWindow != nullptr && m_instrumentWindow->isAlwaysOnTop() != appActive)
        m_instrumentWindow->setAlwaysOnTop (appActive);

    emitAnalysis();

    const auto t = m_engine.getTelemetry();

    // Renders nearest the playhead go first (docs/design/time-stretching.md, TS9).
    if (m_stretch != nullptr)
        m_stretch->setPlayhead (m_session.project()->tempoMap.secondsAt (t.positionBeats * ticksPerBeat));
    pollRealtimeExport (std::max (t.peakLeft, t.peakRight));
    m_bridge.emit ("dawg_telemetry",
                   makeObject ({ { "playing", t.playing },
                                 { "positionBeats", t.positionBeats },
                                 { "peakLeft", t.peakLeft },
                                 { "peakRight", t.peakRight },
                                 { "sampleRate", t.sampleRate },
                                 { "latencySamples", t.latencySamples },
                                 { "cpu", m_deviceManager.getCpuUsage() },
                                 { "allocationViolations", static_cast<int> (allocationViolationCount()) },
                                 { "recording", t.recording },
                                 { "previewSeconds", t.previewSeconds },
                                 { "recordStartBeats", m_engine.recordStartBeats() },
                                 { "inputPeak", t.inputPeak },
                                 { "midiActivity", m_midiActivity },
                                 { "automation", automationValues() },
                                 { "meters", trackMeters() },
                                 { "faustMeters", faustMeters() },
                                 { "loops", loopMeters() },
                                 { "voices", voiceCounts() },
                                 { "outputMute", outputMuteName (t.outputMute) },
                                 { "invalidTracks", invalidTracks() },
                                 { "pluginAllocations", static_cast<int> (exemptAllocationCount()) } }));
}

juce::var MainComponent::loopMeters()
{
    // Each Feedback node's limit and shortest length, by "track:node" (docs/design/feedback-routing.md, R6).
    const auto result = newObject();
    const auto add = [&] (const Track& track, const TrackGraph& graph, bool voices, const auto& self) -> void
    {
        for (const auto& node : graph.nodes)
        {
            if (node.voiceGraph != nullptr)
                self (track, *node.voiceGraph, true, self);

            if (node.component != nullptr)
                self (track, node.component->graph, voices, self);

            if (node.type != nodetype::feedback)
                continue;

            auto limiting = false;
            juce::var shortest;
            const auto read = [&] (const Node* instance)
            {
                if (instance == nullptr)
                    return;

                const auto meters = instance->meters();
                limiting = limiting || static_cast<bool> (meters["limiting"]);
                shortest = meters["shortestMs"];
            };

            if (voices)
                for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
                    read (m_compiler.findVoiceInstance (track.id, node.id, voice));
            else
                read (m_compiler.findInstance (track.id, node.id));

            // Not compiled yet, or not reaching the output.
            if (shortest.isVoid())
                continue;

            const auto entry = newObject();
            entry->setProperty ("limiting", limiting);
            entry->setProperty ("shortestMs", shortest);
            result->setProperty (juce::String (track.id) + ":" + juce::String (node.id), juce::var (entry.get()));
        }
    };

    const auto project = m_session.project();

    for (const auto& track : project->tracks)
        add (*track, track->graph, false, add);

    if (project->master != nullptr)
        add (*project->master, project->master->graph, false, add);

    return juce::var (result.get());
}

juce::var MainComponent::voiceCounts()
{
    // The sounding voices of every Voice Instrument, by "track:node" (docs/design/phase12-voice-scope.md, V7).
    const auto result = newObject();

    for (const auto& track : m_session.project()->tracks)
        forEachNode (track->graph,
                     [&] (const GraphNode& node, bool)
                     {
                         if (node.type == nodetype::voiceInstrument)
                             if (const auto* instance = m_compiler.findInstance (track->id, node.id))
                                 result->setProperty (juce::String (track->id) + ":" + juce::String (node.id),
                                                      instance->meters()["voices"]);
                     });

    return juce::var (result.get());
}

juce::var MainComponent::invalidTracks()
{
    // The tracks whose output had NaN or infinity since the last poll, and were silenced and reset
    // (docs/design/output-protection.md, S1).
    const auto project = m_session.project();
    juce::Array<juce::var> ids;
    auto tracks = project->tracks;

    if (project->master != nullptr)
        tracks.push_back (project->master);

    for (const auto& track : tracks)
        if (m_compiler.trackState (track->id)->invalidBlocks.exchange (0) > 0)
            ids.add (track->id);

    return ids;
}

juce::var MainComponent::trackMeters()
{
    // Per-track meters (docs/design/mixing-basics.md, M2): { trackId: [left, right, input] }, reset on read.
    const auto project = m_session.project();
    const auto meters = newObject();
    auto tracks = project->tracks;

    if (project->master != nullptr)
        tracks.push_back (project->master);

    for (const auto& track : tracks)
    {
        const auto state = m_compiler.trackState (track->id);
        meters->setProperty (juce::String (track->id),
                             juce::Array<juce::var> { state->peakLeft.exchange (0.0f), state->peakRight.exchange (0.0f),
                                                      state->inputPeak.exchange (0.0f) });
    }

    return juce::var (meters.get());
}

void MainComponent::resized()
{
    m_browser->setBounds (getLocalBounds());
}

void MainComponent::paint (juce::Graphics& g)
{
    constexpr float messageFontSize = 16.0f;
    constexpr int messageMargin = 40;
    constexpr int messageMaxLines = 6;

    g.fillAll (backgroundColour);

    if (m_missingUiMessage.isEmpty())
        return;

    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.setFont (juce::FontOptions (messageFontSize));
    g.drawFittedText (m_missingUiMessage, getLocalBounds().reduced (messageMargin), juce::Justification::centred,
                      messageMaxLines);
}

} // namespace dawg
