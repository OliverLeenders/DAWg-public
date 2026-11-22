#include "MainComponent.h"
#include "dawg/FaustNode.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <regex>
#include <set>
#include <utility>
#include <vector>

// Faust scripting in the app (docs/design/phase11-faust-panels.md): the bundled libraries, background compiling, the
// scripts' status for the UI, and the script library.

namespace dawg
{

namespace
{
// A track's Faust nodes, those in its voice graphs and components too, each with its running instances: the track node's, or one per
// voice slot (docs/design/phase12-voice-scope.md, V15).
template <typename Visit>
void forEachFaustNode (const GraphCompiler& compiler, const Track& track, Visit&& visit)
{
    forEachNode (track.graph,
                 [&] (const GraphNode& node, bool voice)
                 {
                     if (node.type != nodetype::faust)
                         return;

                     std::vector<FaustNode*> instances;

                     if (! voice)
                         instances.push_back (dynamic_cast<FaustNode*> (compiler.findInstance (track.id, node.id)));
                     else
                         for (int slot = 0; std::cmp_less (slot, VoiceAllocator::numVoices); ++slot)
                             instances.push_back (
                                 dynamic_cast<FaustNode*> (compiler.findVoiceInstance (track.id, node.id, slot)));

                     std::erase (instances, nullptr);
                     visit (node, instances);
                 });
}

// The bundled Faust resources: next to the executable, or in the macOS bundle's Resources (F5).
[[nodiscard]] juce::File bundledFaustFolder()
{
    const auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    for (const auto& candidate :
         { exeDir.getChildFile ("faust"), exeDir.getParentDirectory().getChildFile ("Resources/faust") })
        if (candidate.getChildFile ("stdfaust.lib").existsAsFile())
            return candidate;

    return {};
}

// The user's script library (F6).
[[nodiscard]] juce::File libraryFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("DAWg")
        .getChildFile ("Faust");
}

// The examples: bundled, or the source tree's in development builds.
[[nodiscard]] juce::File examplesFolder()
{
    if (const auto bundled = bundledFaustFolder(); bundled.getChildFile ("examples").isDirectory())
        return bundled.getChildFile ("examples");

    return {};
}

// A script's display name: its `declare name` if it has one, else the file name.
[[nodiscard]] juce::String scriptName (const juce::File& file, const juce::String& code)
{
    for (const auto& line : juce::StringArray::fromLines (code))
        if (line.trimStart().startsWith ("declare name"))
            return line.fromFirstOccurrenceOf ("\"", false, false).upToLastOccurrenceOf ("\"", false, false);

    return file.getFileNameWithoutExtension();
}

// The standard libraries' definitions with their environment prefix ("fi.lowpass(N,fc)"), for completion in the
// code editor (F7): stdfaust.lib binds the prefixes to library files, whose top-level definitions start a line.
[[nodiscard]] juce::var libraryCompletions (const juce::File& libraries)
{
    juce::Array<juce::var> result;
    const std::regex binding (R"re(^\s*(\w+)\s*=\s*library\("([^"]+)"\))re");
    const std::regex definition (R"re(^([A-Za-z_]\w*)\s*(\([^)]*\))?\s*=[^=])re");

    for (const auto& line : juce::StringArray::fromLines (libraries.getChildFile ("stdfaust.lib").loadFileAsString()))
    {
        std::smatch bound;
        const auto text = line.toStdString();

        if (! std::regex_search (text, bound, binding))
            continue;

        std::set<std::string> seen;

        for (const auto& entry :
             juce::StringArray::fromLines (libraries.getChildFile (bound[2].str()).loadFileAsString()))
        {
            std::smatch defined;
            const auto source = entry.toStdString();

            if (! std::regex_search (source, defined, definition) || ! seen.insert (defined[1].str()).second)
                continue;

            result.add (makeObject ({ { "name", juce::String (bound[1].str() + "." + defined[1].str()) },
                                      { "signature", juce::String (defined[1].str() + defined[2].str()) } }));
        }
    }

    return result;
}

void addScripts (juce::Array<juce::var>& scripts, const juce::File& folder, const char* source)
{
    if (! folder.isDirectory())
        return;

    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.dsp");
    files.sort();

    for (const auto& file : files)
    {
        const auto code = file.loadFileAsString();
        scripts.add (makeObject ({ { "name", scriptName (file, code) },
                                   { "file", file.getFileName() },
                                   { "source", source },
                                   { "code", code } }));
    }
}
} // namespace

void MainComponent::initialiseFaust()
{
    auto& faust = FaustCompiler::shared();

    if (const auto bundled = bundledFaustFolder(); bundled != juce::File())
        faust.setLibraryDirectory (bundled);

    // A finished compilation swaps the node to its new program (or reports the error) on the message thread.
    faust.setListener (
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync (
                [safe]
                {
                    if (safe != nullptr)
                        safe->faustCompiled();
                });
        });
    faust.setBackground (true);
}

void MainComponent::shutdownFaust()
{
    FaustCompiler::shared().setListener (nullptr);
}

void MainComponent::faustCompiled()
{
    m_compiler.invalidate();
    updateEngine();
    emitPluginStatus();
}

void MainComponent::addFaustStatus (juce::DynamicObject& status)
{
    auto tracks = m_session.project()->tracks;

    if (m_session.project()->master != nullptr)
        tracks.push_back (m_session.project()->master);

    for (const auto& track : tracks)
        forEachFaustNode (m_compiler, *track,
                          [&] (const GraphNode& node, const std::vector<FaustNode*>& instances)
                          {
                              // The script's own result (pending or failed), and what the node is running: the last script that
                              // compiled (F4). Every voice slot runs the same program. A voice node that doesn't reach the Voice Output
                              // has no instance, so it asks for its script's program here (compiling it in the background if needed).
                              const auto result = instances.empty() ? FaustCompiler::shared().request (node.code)
                                                                    : FaustCompiler::shared().find (node.code);
                              const auto program = ! instances.empty() ? instances.front()->program() : result.program;
                              const auto* state = "ready";

                              if (result.pending)
                                  state = "compiling";
                              else if (result.error.isNotEmpty())
                                  state = "failed";

                              status.setProperty (
                                  juce::String (track->id) + ":" + juce::String (node.id),
                                  makeObject ({ { "status", state },
                                                { "error", result.error },
                                                { "program", program != nullptr ? program->toVar() : juce::var() } }));
                          });
}

juce::var MainComponent::faustMeters()
{
    // Bargraph values of every Faust node that has some, by "track:node" (F8).
    const auto result = newObject();
    auto tracks = m_session.project()->tracks;

    if (m_session.project()->master != nullptr)
        tracks.push_back (m_session.project()->master);

    for (const auto& track : tracks)
        forEachFaustNode (m_compiler, *track,
                          [&] (const GraphNode& node, const std::vector<FaustNode*>& instances)
                          {
                              const auto& program = ! instances.empty() ? instances.front()->program() : nullptr;

                              if (program == nullptr || program->numMeters() == 0)
                                  return;

                              // A voice node shows the largest value of its voices.
                              juce::Array<juce::var> values;

                              for (size_t i = 0; i < program->numMeters(); ++i)
                              {
                                  float value = 0.0f;

                                  for (const auto* instance : instances)
                                      if (instance->program() == program)
                                          value = std::max (value, instance->meterValue (i));

                                  values.add (value);
                              }

                              result->setProperty (juce::String (track->id) + ":" + juce::String (node.id), values);
                          });

    return juce::var (result.get());
}

void MainComponent::registerFaustHandlers()
{
    // The examples and the user's library (F6).
    m_bridge.on ("faust.scripts",
                 [] (const juce::var&)
                 {
                     juce::Array<juce::var> scripts;
                     addScripts (scripts, examplesFolder(), "example");
                     addScripts (scripts, libraryFolder(), "library");
                     return juce::var (scripts);
                 });

    // Saves a script to the library as <name>.dsp, replacing a script of the same name.
    m_bridge.on ("faust.saveScript",
                 [] (const juce::var& p)
                 {
                     const auto name = juce::File::createLegalFileName (p["name"].toString().trim());

                     if (name.isEmpty())
                         throw std::runtime_error ("The script needs a name");

                     const auto folder = libraryFolder();

                     if (! folder.createDirectory())
                         throw std::runtime_error ("Could not create " + folder.getFullPathName().toStdString());

                     const auto file = folder.getChildFile (name + ".dsp");

                     if (! file.replaceWithText (p["code"].toString()))
                         throw std::runtime_error ("Could not write " + file.getFullPathName().toStdString());

                     return juce::var (file.getFileName());
                 });

    m_bridge.on ("faust.completions",
                 [] (const juce::var&)
                 {
                     static const auto completions = libraryCompletions (FaustCompiler::shared().libraryDirectory());
                     return completions;
                 });

    m_bridge.on ("faust.revealLibrary",
                 [] (const juce::var&)
                 {
                     const auto folder = libraryFolder();
                     std::ignore = folder.createDirectory();
                     folder.revealToUser();
                     return juce::var();
                 });
}

} // namespace dawg
