#pragma once

#include "dawg/Commands.h"

#include <functional>
#include <vector>

namespace dawg
{

/**
    Owns the current project snapshot, the undo/redo history and the revision counter.

    All model changes go through apply(), undo() or redo(), so the UI mirror, persistence and the audio
    engine are all driven from one change stream.

    @note Not thread-safe. Use from the message thread only.
*/
class Session final
{
public:
    /**
        Outcome of a change request.
    */
    struct Result
    {
        /**
            Whether the change was applied.
        */
        bool ok = true;

        /**
            User-facing reason when ok is false.
        */
        juce::String error;

        /**
            The revision after the call (unchanged on failure).
        */
        juce::int64 revision = 0;
    };

    /**
        Called after every change.

        Receives the old and the new snapshot and a patch { rev, ops, history } where ops follow diff().
    */
    using ChangeListener =
        std::function<void (const ProjectPtr& before, const ProjectPtr& after, const juce::var& patch)>;

    /**
        Most undo steps kept; older steps are discarded.
    */
    static constexpr size_t maxUndoSteps = 500;

    /**
        Creates a session at revision 0 with an empty history.

        @param initial  The starting project.
    */
    explicit Session (Project initial = {});

    /**
        Applies a command and records it in the undo history.

        Consecutive commands with the same non-empty gesture id are merged into a single undo step, so a
        whole fader drag undoes at once.

        @param command  The command name (see applyCommand()).
        @param args     The command arguments.
        @param gesture  Optional gesture id for merging continuous edits.
        @return         The outcome; on failure the project and history are unchanged.
    */
    [[nodiscard]] Result apply (const juce::String& command, const juce::var& args, const juce::String& gesture = {});

    /**
        Changes the current project without recording an undo step.

        Used for values the user didn't edit through a command but that must be part of the current state,
        such as plugin state refreshed from live instances (docs/design/phase4-plugin-hosting.md, P3). The
        change becomes part of the current snapshot; undo history and the gesture merge state are kept.

        @param change  Builds the new project from the current one. May throw CommandError.
        @return        The outcome; on failure the project is unchanged.
    */
    [[nodiscard]] Result amend (const std::function<Project (const Project&)>& change);

    /**
        Replaces the whole project (e.g. after opening a file) and clears the undo history. The new project
        counts as saved (not dirty). Unlike assigning a new Session, the revision keeps counting and listeners
        receive a normal patch.

        @param project  The new project.
    */
    void reset (Project project);

    /**
        Reverts the most recent change.

        @return  The outcome; fails if there is nothing to undo.
    */
    [[nodiscard]] Result undo();

    /**
        Re-applies the most recently undone change.

        @return  The outcome; fails if there is nothing to redo.
    */
    [[nodiscard]] Result redo();

    /**
        Returns true if undo() would succeed.
    */
    [[nodiscard]] bool canUndo() const noexcept { return ! m_undoStack.empty(); }

    /**
        Returns true if redo() would succeed.
    */
    [[nodiscard]] bool canRedo() const noexcept { return ! m_redoStack.empty(); }

    /**
        Returns the label of the step undo() would revert, or an empty string.
    */
    [[nodiscard]] juce::String undoLabel() const;

    /**
        Returns the label of the step redo() would re-apply, or an empty string.
    */
    [[nodiscard]] juce::String redoLabel() const;

    /**
        Returns the current project snapshot.
    */
    [[nodiscard]] const ProjectPtr& project() const noexcept { return m_current; }

    /**
        Remembers the current snapshot as the saved one (after saving or opening a file).
    */
    void markSaved() noexcept { m_saved = m_current; }

    /**
        Marks the project as having unsaved changes (e.g. after recovering an autosave).
    */
    void markUnsaved() noexcept { m_saved = nullptr; }

    /**
        Returns true if the project differs from the saved snapshot. Snapshots are immutable and shared, so
        undoing back to the saved state makes the project clean again (docs/design/phase5-project-workflow.md,
        F5).
    */
    [[nodiscard]] bool isDirty() const noexcept { return m_current != m_saved; }

    /**
        Returns the current revision. It increases by one with every applied change, undo or redo.
    */
    [[nodiscard]] juce::int64 revision() const noexcept { return m_revision; }

    /**
        Returns the full state for a UI that is (re)synchronising.

        @return  { rev, project, history: { canUndo, canRedo, undoLabel, redoLabel } }
    */
    [[nodiscard]] juce::var snapshot() const;

    /**
        Sets the listener notified after every change. Replaces any previous listener.

        @param listener  The callback, or nullptr to remove it.
    */
    void setChangeListener (ChangeListener listener) { m_onChange = std::move (listener); }

private:
    struct HistoryEntry
    {
        ProjectPtr project;
        juce::String label;
    };

    void replaceProject (ProjectPtr next);
    [[nodiscard]] juce::var historyVar() const;

    ProjectPtr m_current;
    ProjectPtr m_saved;
    juce::int64 m_revision = 0;
    std::vector<HistoryEntry> m_undoStack;
    std::vector<HistoryEntry> m_redoStack;
    juce::String m_lastGesture;
    ChangeListener m_onChange;
};

} // namespace dawg
