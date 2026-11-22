#include "ProjectManager.h"

#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

namespace dawg
{

namespace
{
constexpr const char* recentKey = "recentProjects";
constexpr const char* emptyTemplate = "empty";
// Starter templates, built in code like the Empty one, so they ship without files and keep up with the format.
constexpr const char* mpeTemplate = "starter:mpe";
constexpr const char* drumsTemplate = "starter:drums";
constexpr const char* audioTemplate = "starter:audio";
constexpr const char* untitledName = "Untitled";
constexpr int maxRecent = 20;

[[nodiscard]] juce::File documentsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("DAWg");
}

// The Empty template: no tracks at all, and the master track that every project gets when it loads.
[[nodiscard]] Project emptyProject()
{
    return {};
}

// Adds a track and returns its id.
ObjectId addTrack (Project& project, const juce::var& args)
{
    const auto id = project.nextId;
    project = applyCommand (project, "addTrack", args);
    return id;
}

// A starter template (docs/design/ux-review.md, U1): an MPE synth with a reverb return, a drum part on the Sampler,
// or two audio tracks to record into.
[[nodiscard]] Project starterProject (const juce::String& id)
{
    Project project;

    if (id == mpeTemplate)
    {
        const auto synth = addTrack (project, makeObject ({ { "name", "MPE Synth" } }));
        const auto reverb = addTrack (project, makeObject ({ { "kind", "return" }, { "name", "Reverb" } }));
        project = applyCommand (project, "insertNode",
                                makeObject ({ { "trackId", reverb }, { "type", nodetype::loopReverb } }));
        const auto& graph = project.findTrack (reverb)->graph;
        const auto node =
            std::ranges::find_if (graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::loopReverb; });
        // A return's reverb is all wet: the dry signal reaches the master through the track itself.
        project = applyCommand (
            project, "setNodeParam",
            makeObject ({ { "trackId", reverb }, { "nodeId", node->id }, { "param", "mix" }, { "value", 100.0 } }));
        project = applyCommand (project, "addSend",
                                makeObject ({ { "trackId", synth }, { "target", reverb }, { "levelDb", -12.0 } }));
    }
    else if (id == drumsTemplate)
    {
        const auto drums = addTrack (
            project,
            makeObject ({ { "name", "Drums" }, { "device", makeObject ({ { "type", nodetype::sampler } }) } }));
        project = applyCommand (project, "setTrackDrumMode", makeObject ({ { "id", drums }, { "mode", "drums" } }));
    }
    else if (id == audioTemplate)
    {
        addTrack (project, makeObject ({ { "kind", "audio" }, { "name", "Audio 1" } }));
        addTrack (project, makeObject ({ { "kind", "audio" }, { "name", "Audio 2" } }));
    }

    return project;
}

[[nodiscard]] juce::var result (bool needsDecision)
{
    return makeObject ({ { "needsDecision", needsDecision } });
}

[[nodiscard]] juce::String appVersion()
{
    return JUCE_APPLICATION_VERSION_STRING;
}
} // namespace

ProjectManager::ProjectManager (Session& session, juce::PropertiesFile& settings, juce::File appDataFolder, Hooks hooks,
                                int autosaveMs)
    : m_session (session),
      m_settings (settings),
      m_appData (std::move (appDataFolder)),
      m_hooks (std::move (hooks)),
      m_autosaveMs (autosaveMs)
{
    std::ignore = m_appData.createDirectory();
    checkForCrash();
    writeLock();
    startTimer (autosaveMs);
}

ProjectManager::~ProjectManager()
{
    stopTimer();

    // A clean exit: unsaved changes were resolved before quitting.
    if (m_open)
        deleteAutosave();

    std::ignore = m_appData.getChildFile ("running.lock").deleteFile();
}

// ---------------------------------------------------------------------------------------------------------
// Actions

juce::var ProjectManager::discard (Mode mode, std::function<void()> action)
{
    if (m_open)
        m_hooks.stampAll(); // edits in plugin editors count as changes once stamped (F5)

    if (! m_open || ! m_session.isDirty() || mode == Mode::discard)
    {
        action();
        return result (false);
    }

    if (mode == Mode::ask)
        return result (true);

    save (
        [pending = std::move (action)] (bool saved)
        {
            if (saved)
                pending();
        });
    return result (false);
}

juce::var ProjectManager::newProject (const juce::String& templateId, Mode mode)
{
    return discard (mode,
                    [this, templateId]
                    {
                        try
                        {
                            const auto project = [&templateId]
                            {
                                if (templateId == emptyTemplate || templateId.isEmpty())
                                    return emptyProject();

                                if (templateId.startsWith ("starter:"))
                                    return starterProject (templateId);

                                return projectfile::read (juce::File (templateId)).project;
                            }();
                            load (project, {}, juce::Uuid().toString(), false);
                        }
                        catch (const CommandError& e)
                        {
                            m_hooks.error (juce::String ("Could not create the project: ") + e.what());
                        }
                    });
}

juce::var ProjectManager::open (const juce::String& path, Mode mode)
{
    return discard (mode,
                    [this, path]
                    {
                        const auto openFile = [this] (const juce::File& file)
                        {
                            try
                            {
                                auto loaded = projectfile::read (file);
                                // A version 0 file (.dawg.json) is saved in the new format on the next save.
                                const auto isCurrent = file.hasFileExtension (projectfile::extension);
                                load (loaded.project, isCurrent ? file : juce::File(), loaded.metadata.id, false);

                                if (isCurrent)
                                    addRecent (file);
                            }
                            catch (const CommandError& e)
                            {
                                m_hooks.error (e.what());
                            }
                        };

                        if (path.isNotEmpty())
                        {
                            openFile (juce::File (path));
                            return;
                        }

                        const auto start = m_file != juce::File() ? m_file.getParentDirectory()
                                                                  : documentsFolder().getChildFile ("Projects");
                        m_chooser = std::make_unique<juce::FileChooser> ("Open project", start, "*.dawg;*.json");
                        m_chooser->launchAsync (juce::FileBrowserComponent::openMode
                                                    | juce::FileBrowserComponent::canSelectFiles,
                                                [openFile] (const juce::FileChooser& chooser)
                                                {
                                                    if (const auto file = chooser.getResult(); file != juce::File())
                                                        openFile (file);
                                                });
                    });
}

juce::var ProjectManager::close (Mode mode)
{
    return discard (mode,
                    [this]
                    {
                        deleteAutosave();

                        // An untitled project's audio goes with it (the user chose not to save it).
                        if (m_file == juce::File() && m_id.isNotEmpty())
                            std::ignore = projectFolder().deleteRecursively();

                        m_hooks.beforeReplace();
                        m_session.reset ({});
                        m_open = false;
                        m_file = juce::File();
                        m_id.clear();
                        m_autosaved = nullptr;
                        writeLock();
                        m_hooks.stateChanged();
                    });
}

juce::var ProjectManager::requestQuit (Mode mode)
{
    return discard (mode, [this] { m_hooks.quit(); });
}

void ProjectManager::save (const std::function<void (bool)>& then)
{
    if (! m_open)
        return;

    if (m_file == juce::File())
        saveAs (then);
    else
        writeTo (m_file, then);
}

void ProjectManager::saveAs (const std::function<void (bool)>& then)
{
    if (! m_open)
        return;

    const auto start = m_file != juce::File()
                           ? m_file
                           : documentsFolder()
                                 .getChildFile ("Projects")
                                 .getChildFile (untitledName + juce::String (projectfile::extension));
    std::ignore = start.getParentDirectory().createDirectory();

    m_chooser = std::make_unique<juce::FileChooser> ("Save project: choose a name and the folder to put it in", start,
                                                     juce::String ("*") + projectfile::extension);
    m_chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, then] (const juce::FileChooser& chooser)
                            {
                                auto file = chooser.getResult();

                                if (file == juce::File())
                                {
                                    if (then != nullptr)
                                        then (false);
                                    return;
                                }

                                file = file.withFileExtension (projectfile::extension);

                                // Each project gets its own folder (F1), unless the user picked one already.
                                if (file.getParentDirectory().getFileName() != file.getFileNameWithoutExtension())
                                {
                                    const auto folder =
                                        file.getParentDirectory().getChildFile (file.getFileNameWithoutExtension());

                                    if (! folder.createDirectory())
                                    {
                                        m_hooks.error ("Could not create " + folder.getFullPathName());

                                        if (then != nullptr)
                                            then (false);
                                        return;
                                    }

                                    file = folder.getChildFile (file.getFileName());
                                }

                                writeTo (file, then);
                            });
}

void ProjectManager::writeTo (const juce::File& file, const std::function<void (bool)>& then)
{
    m_hooks.stampAll();
    const auto saved = m_session.project();

    // Saving somewhere new (first save, Save as): the audio comes along.
    const auto oldFolder = projectFolder();
    const auto newFolder = file.getParentDirectory();
    const auto wasUntitled = m_file == juce::File();

    if (oldFolder != newFolder && m_hooks.copyAudio != nullptr)
        m_hooks.copyAudio (*saved, oldFolder, newFolder);
    const auto error = projectfile::write (*saved, { m_id, appVersion() }, file);

    if (error.isNotEmpty())
    {
        m_hooks.error (error);

        if (then != nullptr)
            then (false);
        return;
    }

    if (m_session.project() == saved)
        m_session.markSaved();

    deleteAutosave();

    if (wasUntitled && oldFolder != newFolder)
        std::ignore = oldFolder.deleteRecursively();

    m_file = file;
    addRecent (file);
    writeLock();
    sessionChanged();
    m_hooks.stateChanged();

    if (then != nullptr)
        then (true);
}

void ProjectManager::saveAsTemplate (const juce::String& name)
{
    if (! m_open)
        return;

    const auto clean = juce::File::createLegalFileName (name.trim());

    if (clean.isEmpty())
    {
        m_hooks.error ("A template needs a name");
        return;
    }

    m_hooks.stampAll();
    const auto folder = templatesFolder();
    std::ignore = folder.createDirectory();
    const auto error = projectfile::write (*m_session.project(), { juce::Uuid().toString(), appVersion() },
                                           folder.getChildFile (clean + projectfile::extension), false);

    if (error.isNotEmpty())
        m_hooks.error (error);
}

void ProjectManager::recover (bool accept)
{
    if (! m_recovery.isObject())
        return;

    const auto autosave = autosaveFile (m_recovery["id"].toString());
    const juce::File original (m_recovery["path"].toString());
    m_recovery = juce::var();

    if (! accept)
    {
        std::ignore = autosave.deleteFile();
        m_hooks.stateChanged();
        return;
    }

    try
    {
        auto loaded = projectfile::read (autosave);
        // The recovered state is newer than the file: it stays unsaved until the user saves.
        load (loaded.project, original.existsAsFile() ? original : juce::File(), loaded.metadata.id, true);
    }
    catch (const CommandError& e)
    {
        m_hooks.error (juce::String ("Could not recover the project: ") + e.what());
        m_hooks.stateChanged();
    }
}

// ---------------------------------------------------------------------------------------------------------
// State

void ProjectManager::load (const Project& project, const juce::File& file, const juce::String& id, bool dirty)
{
    m_hooks.beforeReplace();
    m_session.reset (withMaster (project));

    if (dirty)
        m_session.markUnsaved();

    m_open = true;
    m_file = file;
    m_id = id;
    m_autosaved = nullptr;
    m_wasDirty = m_session.isDirty();
    writeLock();
    m_hooks.stateChanged();
}

void ProjectManager::sessionChanged()
{
    m_lastChange = juce::Time::getMillisecondCounter();

    if (m_session.isDirty() != m_wasDirty)
    {
        m_wasDirty = m_session.isDirty();
        m_hooks.stateChanged();
    }
}

juce::String ProjectManager::displayName() const
{
    return m_file != juce::File() ? m_file.getFileNameWithoutExtension() : juce::String (untitledName);
}

juce::var ProjectManager::stateVar() const
{
    return makeObject ({
        { "open", m_open },
        { "name", m_open ? displayName() : juce::String() },
        { "path", m_file.getFullPathName() },
        { "dirty", m_open && m_session.isDirty() },
        { "untitled", m_file == juce::File() },
        { "recovery", m_recovery },
    });
}

// ---------------------------------------------------------------------------------------------------------
// Recent projects and templates

void ProjectManager::addRecent (const juce::File& file)
{
    const auto summary = projectfile::summarise (*m_session.project());
    juce::Array<juce::var> entries;
    entries.add (makeObject ({
        { "path", file.getFullPathName() },
        { "name", file.getFileNameWithoutExtension() },
        { "opened", juce::Time::currentTimeMillis() },
        { "tempo", summary.tempo },
        { "tracks", summary.tracks },
        { "lengthSeconds", summary.lengthSeconds },
    }));

    const auto stored = juce::JSON::parse (m_settings.getValue (recentKey)); // keep the var alive

    if (const auto* existing = stored.getArray())
        for (const auto& entry : *existing)
            if (entry["path"].toString() != file.getFullPathName() && entries.size() < maxRecent)
                entries.add (entry);

    m_settings.setValue (recentKey, juce::JSON::toString (entries, true));
    std::ignore = m_settings.saveIfNeeded();
}

void ProjectManager::removeRecent (const juce::String& path)
{
    juce::Array<juce::var> entries;

    const auto stored = juce::JSON::parse (m_settings.getValue (recentKey)); // keep the var alive

    if (const auto* existing = stored.getArray())
        for (const auto& entry : *existing)
            if (entry["path"].toString() != path)
                entries.add (entry);

    m_settings.setValue (recentKey, juce::JSON::toString (entries, true));
    std::ignore = m_settings.saveIfNeeded();
}

juce::var ProjectManager::recent() const
{
    juce::Array<juce::var> entries;

    const auto stored = juce::JSON::parse (m_settings.getValue (recentKey)); // keep the var alive

    if (const auto* existing = stored.getArray())
    {
        for (const auto& entry : *existing)
        {
            const juce::File file (entry["path"].toString());
            auto copy = entry.clone();
            // The folder that holds the project folder (F1): where the user put it.
            copy.getDynamicObject()->setProperty ("folder",
                                                  file.getParentDirectory().getParentDirectory().getFullPathName());
            copy.getDynamicObject()->setProperty ("missing", ! file.existsAsFile());
            entries.add (copy);
        }
    }

    return entries;
}

juce::File ProjectManager::projectFolder() const
{
    if (m_file != juce::File())
        return m_file.getParentDirectory();

    return m_appData.getChildFile ("Unsaved").getChildFile (m_id.isNotEmpty() ? m_id : juce::String ("none"));
}

juce::File ProjectManager::templatesFolder()
{
    return documentsFolder().getChildFile ("Templates");
}

juce::var ProjectManager::templates() const
{
    juce::Array<juce::var> result { makeObject ({ { "id", emptyTemplate }, { "name", "Empty" } }),
                                    makeObject ({ { "id", mpeTemplate }, { "name", "MPE synth" } }),
                                    makeObject ({ { "id", drumsTemplate }, { "name", "Drums" } }),
                                    makeObject ({ { "id", audioTemplate }, { "name", "Audio recording" } }) };
    auto files =
        templatesFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + projectfile::extension);
    files.sort();

    for (const auto& file : files)
        result.add (makeObject ({ { "id", file.getFullPathName() }, { "name", file.getFileNameWithoutExtension() } }));

    return result;
}

// ---------------------------------------------------------------------------------------------------------
// Autosave and crash recovery (F6)

juce::File ProjectManager::autosaveFile (const juce::String& id) const
{
    return m_appData.getChildFile ("Autosave").getChildFile (id + projectfile::extension);
}

void ProjectManager::deleteAutosave() const
{
    if (m_id.isNotEmpty())
        std::ignore = autosaveFile (m_id).deleteFile();
}

void ProjectManager::writeLock() const
{
    const auto lock = makeObject ({
        { "id", m_open ? m_id : juce::String() },
        { "name", m_open ? displayName() : juce::String() },
        { "path", m_file.getFullPathName() },
    });
    std::ignore = m_appData.getChildFile ("running.lock").replaceWithText (juce::JSON::toString (lock));
}

void ProjectManager::checkForCrash()
{
    const auto lockFile = m_appData.getChildFile ("running.lock");

    if (! lockFile.existsAsFile())
        return;

    // The previous session didn't exit cleanly. Offer its autosave if it is newer than the saved file.
    const auto lock = juce::JSON::parse (lockFile);
    const auto id = lock["id"].toString();
    const auto autosave = id.isNotEmpty() ? autosaveFile (id) : juce::File();
    const juce::File original (lock["path"].toString());

    if (autosave.existsAsFile()
        && (! original.existsAsFile() || autosave.getLastModificationTime() > original.getLastModificationTime()))
    {
        m_recovery = makeObject ({
            { "id", id },
            { "name", lock["name"].toString() },
            { "path", original.getFullPathName() },
            { "time", autosave.getLastModificationTime().toMilliseconds() },
        });
    }
}

void ProjectManager::timerCallback()
{
    if (! m_open || ! m_session.isDirty() || m_session.project() == m_autosaved)
        return;

    // Not in the middle of an edit: a drag changes the project many times a second, and writing the file (and stamping
    // every plugin) then would stall it. Look again once the edits have paused for a moment.
    constexpr juce::uint32 quietMs = 1500;

    if (juce::Time::getMillisecondCounter() - m_lastChange < quietMs)
    {
        startTimer (static_cast<int> (quietMs));
        return;
    }

    startTimer (m_autosaveMs);

    m_hooks.stampAll();
    const auto file = autosaveFile (m_id);
    std::ignore = file.getParentDirectory().createDirectory();

    const auto error = projectfile::write (*m_session.project(), { m_id, appVersion() }, file, false);
    if (error.isEmpty())
    {
        m_autosaved = m_session.project();
        m_autosaveErrorReported = false;
    }
    else if (! m_autosaveErrorReported)
    {
        m_autosaveErrorReported = true;
        m_hooks.error ("Could not autosave the project: " + error);
    }
}

} // namespace dawg
