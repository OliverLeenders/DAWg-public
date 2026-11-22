#pragma once

#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dawg
{

/**
    Owns the project life cycle: the open file, new / open / save / close, templates, the recent list,
    autosave and crash recovery. See docs/design/phase5-project-workflow.md, F6–F10.

    Everything that would discard the open project goes through discard(): plugin state is stamped first,
    then, if there are unsaved changes, the caller is asked to decide (Mode::ask returns needsDecision), saves
    first (Mode::save) or discards (Mode::discard).

    @note  Message thread only.
*/
class ProjectManager final : private juce::Timer
{
public:
    /**
        What to do with unsaved changes when an action would discard them.
    */
    enum class Mode
    {
        ask,     ///< Don't act; report that a decision is needed.
        save,    ///< Save first (asking for a file if untitled); act only if saving succeeded.
        discard, ///< Act without saving.
    };

    /**
        What the manager needs from the app.
    */
    struct Hooks
    {
        /**
            Refreshes plugin state from the live instances into the session (stamps).
        */
        std::function<void()> stampAll;

        /**
            Called before the session's project is replaced by a different one (close editor windows, reset
            the graph compiler).
        */
        std::function<void()> beforeReplace;

        /**
            Called whenever the state from stateVar() may have changed.
        */
        std::function<void()> stateChanged;

        /**
            Reports an error to the user.
        */
        std::function<void (const juce::String&)> error;

        /**
            Quits the application (after requestQuit() resolved).
        */
        std::function<void()> quit;

        /**
            Copies the project's audio from one project folder to another before it is saved there.
        */
        std::function<void (const Project& project, const juce::File& from, const juce::File& to)> copyAudio;
    };

    /**
        Creates the manager, detects a crashed previous session and starts with no project open (the welcome
        screen).

        @param session         The session holding the model. Must outlive the manager.
        @param settings        App settings (recent list). Must outlive the manager.
        @param appDataFolder   Where autosaves and the lock file live.
        @param hooks           Callbacks into the app.
        @param autosaveMs      Autosave interval.
    */
    ProjectManager (Session& session, juce::PropertiesFile& settings, juce::File appDataFolder, Hooks hooks,
                    int autosaveMs = 2 * 60 * 1000);

    /**
        A clean exit: removes the lock file and the current autosave.
    */
    ~ProjectManager() override;

    ProjectManager (const ProjectManager&) = delete;
    ProjectManager& operator= (const ProjectManager&) = delete;
    ProjectManager (ProjectManager&&) = delete;
    ProjectManager& operator= (ProjectManager&&) = delete;

    /**
        Creates a new untitled project from a template.

        @param templateId  "empty", a starter template ("starter:…") or a template file path (see templates()).
        @param mode        What to do with unsaved changes in the open project.
        @return            { needsDecision }
    */
    [[nodiscard]] juce::var newProject (const juce::String& templateId, Mode mode);

    /**
        Opens a project file, or asks for one when path is empty.

        @param path  The file, or empty for a file dialog.
        @param mode  What to do with unsaved changes in the open project.
        @return      { needsDecision }
    */
    [[nodiscard]] juce::var open (const juce::String& path, Mode mode);

    /**
        Closes the project and returns to the welcome screen.

        @param mode  What to do with unsaved changes.
        @return      { needsDecision }
    */
    [[nodiscard]] juce::var close (Mode mode);

    /**
        Quits DAWg, resolving unsaved changes first.

        @param mode  What to do with unsaved changes.
        @return      { needsDecision }
    */
    [[nodiscard]] juce::var requestQuit (Mode mode);

    /**
        Saves to the open file, or asks for one if the project is untitled.

        @param then  Called with true once saved, false if saving failed or was cancelled.
    */
    void save (const std::function<void (bool)>& then = {});

    /**
        Asks for a name and folder and saves there. A new project folder is created (F1).

        @param then  Called with true once saved, false if saving failed or was cancelled.
    */
    void saveAs (const std::function<void (bool)>& then = {});

    /**
        Saves the project as a template in the user's templates folder.

        @param name  The template's name.
    */
    void saveAsTemplate (const juce::String& name);

    /**
        Accepts or declines the crash-recovery offer.

        @param accept  True to open the autosave, false to delete it.
    */
    void recover (bool accept);

    /**
        Removes an entry from the recent list.

        @param path  The project file path.
    */
    void removeRecent (const juce::String& path);

    /**
        Returns the state for the UI.

        @return  { open, name, path, dirty, untitled, recovery: { name, path, time } or null }
    */
    [[nodiscard]] juce::var stateVar() const;

    /**
        Returns the recent projects, newest first.

        @return  [{ path, name, folder, opened, missing, tempo, tracks, lengthSeconds }]
    */
    [[nodiscard]] juce::var recent() const;

    /**
        Returns the available templates ("empty" first, then the starter templates).

        @return  [{ id, name }]
    */
    [[nodiscard]] juce::var templates() const;

    /**
        Returns the open project's folder: where its file is, or a private folder in app data while the project
        is untitled. Pool paths ("Audio/...") resolve against it.
    */
    [[nodiscard]] juce::File projectFolder() const;

    /**
        Returns the user's templates folder.
    */
    [[nodiscard]] static juce::File templatesFolder();

    /**
        Call after every session change, so the unsaved marker stays current.
    */
    void sessionChanged();

private:
    [[nodiscard]] juce::var discard (Mode mode, std::function<void()> action);
    void load (const Project& project, const juce::File& file, const juce::String& id, bool dirty);
    void writeTo (const juce::File& file, const std::function<void (bool)>& then);
    void addRecent (const juce::File& file);
    void writeLock() const;
    void deleteAutosave() const;
    void checkForCrash();
    [[nodiscard]] juce::File autosaveFile (const juce::String& id) const;
    [[nodiscard]] juce::String displayName() const;
    void timerCallback() override;

    Session& m_session;
    juce::PropertiesFile& m_settings;
    juce::File m_appData;
    Hooks m_hooks;

    bool m_open = false;
    juce::File m_file; // empty while untitled
    juce::String m_id;
    bool m_wasDirty = false;
    int m_autosaveMs = 0;
    juce::uint32 m_lastChange = 0; // when the session last changed (Time::getMillisecondCounter)
    ProjectPtr m_autosaved;
    juce::var m_recovery; // { id, name, path, time } or void
    bool m_autosaveErrorReported = false;
    std::unique_ptr<juce::FileChooser> m_chooser;
};

} // namespace dawg
