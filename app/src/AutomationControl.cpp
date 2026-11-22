#include "MainComponent.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

// Automation in the app (docs/design/phase10-automation.md, A4, A6–A8): touches from DAWg's controls, plugin
// editors and MIDI learn; capture of the manual values while playing; one "Write automation" step per pass.

namespace dawg
{

namespace
{
constexpr double midiTouchSeconds = 0.5;
constexpr double simplifyTolerance = 0.004;

[[nodiscard]] juce::String keyOf (ObjectId trackId, const AutomationTarget& target)
{
    return juce::String (trackId) + ":" + juce::String (static_cast<int> (target.kind)) + ":"
           + juce::String (target.nodeId) + ":" + target.param;
}

[[nodiscard]] juce::var targetVar (const AutomationTarget& target)
{
    static constexpr std::array<const char*, 4> kinds { "gain", "pan", "node", "plugin" };
    return makeObject ({ { "kind", kinds[static_cast<size_t> (target.kind)] },
                         { "nodeId", target.nodeId },
                         { "param", target.param },
                         { "name", target.name } });
}

[[nodiscard]] AutomationTarget targetFrom (const juce::var& value)
{
    AutomationTarget target;
    const auto kind = value["kind"].toString();
    target.nodeId = static_cast<ObjectId> (value["nodeId"]);
    target.param = value["param"].toString();
    target.name = value["name"].toString();

    if (kind == "pan")
        target.kind = AutomationTargetKind::pan;
    else if (kind == "node")
        target.kind = AutomationTargetKind::nodeParam;
    else if (kind == "plugin")
        target.kind = AutomationTargetKind::pluginParam;
    else if (kind == "send")
        target.kind = AutomationTargetKind::send;
    else
        target.param = "gain";

    if (target.kind == AutomationTargetKind::pan)
        target.param = "pan";

    return target;
}

[[nodiscard]] const AutomationLane* laneFor (const Track& track, const AutomationTarget& target)
{
    const auto it =
        std::ranges::find_if (track.automation, [&] (const AutomationLane& l) { return l.target.sameAs (target); });
    return it != track.automation.end() ? &*it : nullptr;
}
} // namespace

/**
    One parameter being written.
*/
struct MainComponent::AutomationCapture
{
    ObjectId trackId = 0;
    AutomationTarget target;
    bool touching = false;
    double midiTouchUntil = 0.0;                                // MIDI learn touches end after a rest
    std::vector<std::vector<std::pair<double, double>>> passes; // (tick, value) runs; a loop wrap starts a new one
};

void MainComponent::registerAutomationHandlers()
{
    m_bridge.on ("automation.touch",
                 [this] (const juce::var& p)
                 {
                     touchAutomation (static_cast<ObjectId> (p["trackId"]), targetFrom (p["target"]),
                                      static_cast<bool> (p["touching"]));
                     return juce::var();
                 });

    m_bridge.on ("plugin.parameters",
                 [this] (const juce::var& p)
                 {
                     auto* node = dynamic_cast<PluginNode*> (m_compiler.findInstance (
                         static_cast<ObjectId> (p["trackId"]), static_cast<ObjectId> (p["nodeId"])));
                     return node != nullptr ? node->parameterList() : juce::var (juce::Array<juce::var>());
                 });

    // A step sequencer's preview while stopped (docs/design/step-sequencer.md, SS8): not undoable, not saved.
    m_bridge.on ("stepSequencer.preview",
                 [this] (const juce::var& p)
                 {
                     if (auto* node = m_compiler.findInstance (static_cast<ObjectId> (p["trackId"]),
                                                               static_cast<ObjectId> (p["nodeId"])))
                         node->setStepPreview (p["pattern"].isVoid() ? -1 : static_cast<int> (p["pattern"]));

                     return juce::var();
                 });

    // The pattern a held key plays while stopped: the one the panel shows.
    m_bridge.on ("stepSequencer.shown",
                 [this] (const juce::var& p)
                 {
                     if (auto* node = m_compiler.findInstance (static_cast<ObjectId> (p["trackId"]),
                                                               static_cast<ObjectId> (p["nodeId"])))
                         node->setStepAudition (static_cast<int> (p["pattern"]));

                     return juce::var();
                 });

    m_pluginHost->setGestureCallback (
        [this] (ObjectId trackId, ObjectId nodeId, const juce::String& paramId, const juce::String& name, bool begin)
        {
            AutomationTarget target;
            target.kind = AutomationTargetKind::pluginParam;
            target.nodeId = nodeId;
            target.param = paramId;
            target.name = name;
            touchAutomation (trackId, target, begin);
        });
}

void MainComponent::touchAutomation (ObjectId trackId, const AutomationTarget& touched, bool touching)
{
    // A parameter that follows a link is written as its chain's root (docs/design/parameter-linking.md, L9), as its
    // edits go there too (L4).
    auto target = touched;
    const auto project = m_session.project();

    if (target.kind == AutomationTargetKind::nodeParam || target.kind == AutomationTargetKind::pluginParam)
    {
        ParamAddress address { trackId, target.nodeId, target.param };

        for (size_t steps = 0; steps <= project->links.size(); ++steps)
        {
            const auto* link = project->linkDriving (address);

            if (link == nullptr || ! link->active)
                break;

            address = link->source;
        }

        if (address.nodeId != target.nodeId || address.param != target.param || address.trackId != trackId)
        {
            const auto* sourceTrack = project->findTrackOrMaster (address.trackId);
            const auto* sourceNode = sourceTrack != nullptr ? sourceTrack->findNode (address.nodeId) : nullptr;

            if (sourceNode == nullptr)
                return;

            trackId = address.trackId;
            target.nodeId = address.nodeId;
            target.param = address.param;
            target.kind = nodetype::isPlugin (sourceNode->type) ? AutomationTargetKind::pluginParam
                                                                : AutomationTargetKind::nodeParam;
            target.name = {};
        }
    }

    const auto* track = project->findTrackOrMaster (trackId);

    if (track == nullptr)
        return;

    const auto mode = track->automationMode;
    const auto key = keyOf (trackId, target);

    if (touching && (mode == AutomationMode::touch || mode == AutomationMode::latch || mode == AutomationMode::write))
    {
        auto& capture = m_automationCaptures[key];

        if (capture == nullptr)
        {
            capture = std::make_shared<AutomationCapture>();
            capture->trackId = trackId;
            capture->target = target;
        }

        capture->touching = true;
    }
    else if (const auto it = m_automationCaptures.find (key); it != m_automationCaptures.end())
    {
        // Touch: writing ends on release. Latch and Write go on until the transport stops.
        it->second->touching = mode != AutomationMode::touch;
    }

    syncAutomationOverrides();
}

void MainComponent::touchAutomationFromMidi (ObjectId trackId, const AutomationTarget& target)
{
    touchAutomation (trackId, target, true);

    if (const auto it = m_automationCaptures.find (keyOf (trackId, target)); it != m_automationCaptures.end())
        it->second->midiTouchUntil = juce::Time::getMillisecondCounterHiRes() * 0.001 + midiTouchSeconds;
}

void MainComponent::syncAutomationOverrides()
{
    // A lane being written leaves its parameter to the manual value (A4, A6).
    const auto project = m_session.project();
    auto tracks = project->tracks;

    if (project->master != nullptr)
        tracks.push_back (project->master);

    for (const auto& track : tracks)
    {
        for (const auto& lane : track->automation)
        {
            const auto it = m_automationCaptures.find (keyOf (track->id, lane.target));
            const auto writing = (it != m_automationCaptures.end() && it->second->touching)
                                 || (track->automationMode == AutomationMode::write && m_engine.isPlaying());
            m_compiler.laneState (lane.id)->overridden.store (writing);
        }
    }
}

double MainComponent::manualAutomationValue (const Track& track, const AutomationTarget& target) const
{
    switch (target.kind)
    {
        case AutomationTargetKind::gain:
            return std::sqrt (std::pow (10.0, track.gainDb / 20.0) / 2.0);

        case AutomationTargetKind::pan:
            return (track.pan + 1.0) / 2.0;

        case AutomationTargetKind::nodeParam:
        {
            const auto* found = track.findNode (target.nodeId); // a voice-graph node too

            if (found == nullptr)
                return -1.0;

            const auto& node = *found;
            const auto* descriptor = descriptorOf (track.id, node);
            const auto param = descriptor != nullptr ? descriptor->indexOfParam (target.param) : -1;

            if (param < 0)
                return -1.0;

            const auto& info = descriptor->params[static_cast<size_t> (param)];
            const auto value = node.params.contains (target.param) ? node.params.at (target.param) : info.defaultValue;
            return paramToNormalised (info, value);
        }

        case AutomationTargetKind::pluginParam:
        {
            auto* node = dynamic_cast<PluginNode*> (m_compiler.findInstance (track.id, target.nodeId));
            const auto index = node != nullptr ? node->parameterIndex (target.param) : -1;
            return index >= 0 ? static_cast<double> (node->processor()->getParameters()[index]->getValue()) : -1.0;
        }

        case AutomationTargetKind::send:
        {
            // A send level follows the fader curve, like the track gain.
            const auto send =
                std::ranges::find_if (track.sends, [&] (const Send& s) { return juce::String (s.id) == target.param; });
            return send != track.sends.end() ? std::sqrt (std::pow (10.0, send->levelDb / 20.0) / 2.0) : -1.0;
        }
    }

    return -1.0;
}

void MainComponent::pollAutomation()
{
    syncAutomationOverrides();
    const auto playing = m_engine.isPlaying();
    const auto project = m_session.project();

    if (playing)
        startWriteModeCaptures (*project);

    if (m_automationCaptures.empty())
        return;

    if (! playing)
    {
        finishAutomationPass();
        return;
    }

    const auto tick = m_engine.positionBeats() * ticksPerBeat;
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    for (auto& [key, capture] : m_automationCaptures)
    {
        if (capture->midiTouchUntil > 0.0 && now > capture->midiTouchUntil)
        {
            capture->midiTouchUntil = 0.0;
            touchAutomation (capture->trackId, capture->target, false);
        }

        if (capture->touching)
            sampleCapture (*capture, *project, tick);
    }
}

void MainComponent::startWriteModeCaptures (const Project& project)
{
    // Write mode captures every active lane of the track while playing (A6).
    auto tracks = project.tracks;

    if (project.master != nullptr)
        tracks.push_back (project.master);

    for (const auto& track : tracks)
        if (track->automationMode == AutomationMode::write)
            for (const auto& lane : track->automation)
                if (lane.active && ! m_automationCaptures.contains (keyOf (track->id, lane.target)))
                    touchAutomation (track->id, lane.target, true);
}

void MainComponent::sampleCapture (AutomationCapture& capture, const Project& project, double tick) const
{
    const auto* track = project.findTrackOrMaster (capture.trackId);
    const auto value = track != nullptr ? manualAutomationValue (*track, capture.target) : -1.0;

    if (value < 0.0)
        return;

    // A loop wrap (the position goes back) starts a new run (A8).
    if (capture.passes.empty() || (! capture.passes.back().empty() && tick < capture.passes.back().back().first))
        capture.passes.emplace_back();

    capture.passes.back().emplace_back (tick, std::clamp (value, 0.0, 1.0));
}

void MainComponent::finishAutomationPass()
{
    // One undo step for the whole pass (A8). New lanes get their points as they are added.
    juce::Array<juce::var> commands;
    const auto project = m_session.project();

    for (const auto& [key, capture] : m_automationCaptures)
        if (const auto* track = project->findTrackOrMaster (capture->trackId))
            addCaptureCommands (*capture, *track, commands);

    m_automationCaptures.clear();

    if (! commands.isEmpty())
    {
        const auto result =
            m_session.apply ("batch", makeObject ({ { "label", "Write automation" }, { "commands", commands } }));

        if (! result.ok)
            m_bridge.emit ("dawg_error", result.error);
    }

    syncAutomationOverrides();
}

void MainComponent::addCaptureCommands (const AutomationCapture& capture, const Track& track,
                                        juce::Array<juce::var>& commands)
{
    const auto* lane = laneFor (track, capture.target);

    // Sorted, one point per tick: loop passes may overlap.
    std::map<juce::int64, juce::var> byTick;

    for (const auto& run : capture.passes)
    {
        if (run.empty())
            continue;

        juce::Array<juce::var> points;

        for (const auto& [tick, value] : NoteRecorder::simplify (run, simplifyTolerance))
        {
            points.add (juce::Array<juce::var> { std::llround (tick), value });
            byTick[std::llround (tick)] = points.getLast();
        }

        // An existing lane gets each written range replaced.
        if (lane != nullptr)
            commands.add (makeObject ({ { "command", "writeAutomation" },
                                        { "args", makeObject ({ { "trackId", capture.trackId },
                                                                { "laneId", lane->id },
                                                                { "from", std::llround (run.front().first) },
                                                                { "to", std::llround (run.back().first) },
                                                                { "points", points } }) } }));
    }

    if (lane != nullptr || byTick.empty())
        return;

    juce::Array<juce::var> sorted;

    for (const auto& [tick, p] : byTick)
        sorted.add (p);

    commands.add (makeObject ({ { "command", "addAutomationLane" },
                                { "args", makeObject ({ { "trackId", capture.trackId },
                                                        { "target", targetVar (capture.target) },
                                                        { "points", sorted } }) } }));
}

juce::var MainComponent::automationValues()
{
    // The values the lanes play, for the UI's lanes and controls (A4).
    const auto values = newObject();
    const auto project = m_session.project();
    auto tracks = project->tracks;

    if (project->master != nullptr)
        tracks.push_back (project->master);

    for (const auto& track : tracks)
        for (const auto& lane : track->automation)
            if (const auto value = m_compiler.laneState (lane.id)->value.load(); value >= 0.0f)
                values->setProperty (juce::String (lane.id), value);

    return juce::var (values.get());
}

} // namespace dawg
