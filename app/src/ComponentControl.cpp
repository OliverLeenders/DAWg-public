#include "MainComponent.h"
#include "dawg/ProjectFile.h"
#include "dawg/VarUtils.h"

// Saved components (docs/design/component-nodes.md, N9): one file each in a folder of their own, as a plugin's state can be megabytes. The list
// leaves out what's in them, which is only read when one is dropped in.

namespace dawg
{

namespace
{
constexpr const char* componentExtension = ".dawgcomponent";

[[nodiscard]] juce::File componentsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("DAWg")
        .getChildFile ("Components");
}

// A file named by the list, and only in that folder.
[[nodiscard]] juce::File savedComponentFile (const juce::String& name)
{
    const auto file = componentsFolder().getChildFile (juce::File::createLegalFileName (name));

    if (file.getFileExtension() != componentExtension || ! file.existsAsFile())
        throw std::runtime_error ("No such saved component");

    return file;
}
} // namespace

void MainComponent::registerComponentHandlers()
{
    m_bridge.on ("components.list",
                 [] (const juce::var&)
                 {
                     juce::Array<juce::var> list;

                     for (const auto& file : componentsFolder().findChildFiles (
                              juce::File::findFiles, false, juce::String ("*") + componentExtension))
                     {
                         const auto json = juce::JSON::parse (file);

                         if (! json.isObject())
                             continue;

                         list.add (makeObject ({ { "file", file.getFileName() },
                                                 { "name", json["name"].toString().isNotEmpty()
                                                               ? json["name"]
                                                               : juce::var (file.getFileNameWithoutExtension()) },
                                                 { "inputs", json["inputs"] },
                                                 { "outputs", json["outputs"] } }));
                     }

                     return juce::var (list);
                 });

    m_bridge.on ("components.load",
                 [] (const juce::var& p)
                 {
                     const auto json = juce::JSON::parse (savedComponentFile (p["file"].toString()));

                     if (! json.isObject())
                         throw std::runtime_error ("That saved component is damaged");

                     return json;
                 });

    // Saves a component under its name, replacing one of the same name, with its plugins' latest state.
    m_bridge.on (
        "components.save",
        [this] (const juce::var& p)
        {
            const auto trackId = static_cast<ObjectId> (p["trackId"]);
            const auto nodeId = static_cast<ObjectId> (p["nodeId"]);
            const auto* before = m_session.project()->findTrackOrMaster (trackId);

            if (before == nullptr)
                throw std::runtime_error ("No such track");

            stampTrackPlugins (*before);

            const auto project = m_session.project();
            const auto* track = project->findTrackOrMaster (trackId);
            const auto* node = track != nullptr ? track->findNode (nodeId) : nullptr;

            if (node == nullptr || node->component == nullptr)
                throw std::runtime_error ("Not a component");

            const auto name = node->component->name.isNotEmpty() ? node->component->name : juce::String ("Component");
            const auto file =
                componentsFolder().getChildFile (juce::File::createLegalFileName (name) + componentExtension);

            if (! componentsFolder().createDirectory()
                || ! file.replaceWithText (juce::JSON::toString (projectfile::savedComponentVar (*node))))
                throw std::runtime_error ("Cannot write to " + componentsFolder().getFullPathName().toStdString());

            return juce::var (file.getFileName());
        });

    m_bridge.on ("components.delete",
                 [] (const juce::var& p)
                 {
                     if (! savedComponentFile (p["file"].toString()).deleteFile())
                         throw std::runtime_error ("Cannot delete that saved component");

                     return juce::var();
                 });
}

} // namespace dawg
