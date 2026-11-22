#include "dawg/Session.h"

#include "dawg/VarUtils.h"

namespace dawg
{

Session::Session (Project initial)
    : m_current (std::make_shared<const Project> (std::move (initial))),
      m_saved (m_current)
{
}

Session::Result Session::apply (const juce::String& command, const juce::var& args, const juce::String& gesture)
{
    ProjectPtr next;

    try
    {
        next = std::make_shared<const Project> (applyCommand (*m_current, command, args));
    }
    catch (const CommandError& e)
    {
        return { false, e.what(), m_revision };
    }

    const auto mergeWithPrevious = gesture.isNotEmpty() && gesture == m_lastGesture && canUndo();

    if (! mergeWithPrevious)
    {
        m_undoStack.push_back ({ m_current, describeCommand (command, args) });

        if (m_undoStack.size() > maxUndoSteps)
            m_undoStack.erase (m_undoStack.begin());
    }

    m_redoStack.clear();
    m_lastGesture = gesture;
    replaceProject (std::move (next));
    return { true, {}, m_revision };
}

Session::Result Session::amend (const std::function<Project (const Project&)>& change)
{
    try
    {
        replaceProject (std::make_shared<const Project> (change (*m_current)));
    }
    catch (const CommandError& e)
    {
        return { false, e.what(), m_revision };
    }

    return { true, {}, m_revision };
}

void Session::reset (Project project)
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastGesture.clear();
    m_saved = std::make_shared<const Project> (std::move (project));
    replaceProject (m_saved);
}

Session::Result Session::undo()
{
    if (! canUndo())
        return { false, "Nothing to undo", m_revision };

    auto entry = std::move (m_undoStack.back());
    m_undoStack.pop_back();
    m_redoStack.push_back ({ m_current, entry.label });
    m_lastGesture.clear();
    replaceProject (std::move (entry.project));
    return { true, {}, m_revision };
}

Session::Result Session::redo()
{
    if (! canRedo())
        return { false, "Nothing to redo", m_revision };

    auto entry = std::move (m_redoStack.back());
    m_redoStack.pop_back();
    m_undoStack.push_back ({ m_current, entry.label });
    m_lastGesture.clear();
    replaceProject (std::move (entry.project));
    return { true, {}, m_revision };
}

juce::String Session::undoLabel() const
{
    return canUndo() ? m_undoStack.back().label : juce::String();
}

juce::String Session::redoLabel() const
{
    return canRedo() ? m_redoStack.back().label : juce::String();
}

void Session::replaceProject (ProjectPtr next)
{
    auto before = std::exchange (m_current, std::move (next));
    ++m_revision;

    if (m_onChange == nullptr)
        return;

    const auto patch = makeObject ({
        { "rev", m_revision },
        { "ops", diff (*before, *m_current) },
        { "history", historyVar() },
    });

    m_onChange (before, m_current, patch);
}

juce::var Session::historyVar() const
{
    return makeObject ({
        { "canUndo", canUndo() },
        { "canRedo", canRedo() },
        { "undoLabel", undoLabel() },
        { "redoLabel", redoLabel() },
    });
}

juce::var Session::snapshot() const
{
    return makeObject ({
        { "rev", m_revision },
        { "project", toVar (*m_current) },
        { "history", historyVar() },
    });
}

} // namespace dawg
