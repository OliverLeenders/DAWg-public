#pragma once

#include "dawg/Model.h"
#include "dawg/Node.h"

#include <vector>

/**
    The Drum Synth (docs/design/drum-synth.md): an instrument of sixteen pads on keys 36-51, each playing one of the
    voice models of dawg/DrumVoices.h, with choke groups, mono pads and an output per pad.
*/
namespace dawg
{

class NodeRegistry;

/**
    The highest choke group a pad can be in.
*/
inline constexpr int maxDrumChokeGroup = 8;

/**
    The Drum Synth's outputs: Main, then Out 2 to Out 16 (MO1).
*/
inline constexpr int maxDrumOutputs = 16;

/**
    The Drum Synth's parameters per pad: the common five, then every model's own (DR3, as built: one fixed list, so
    a pad keeps each model's settings while it plays another).
*/
[[nodiscard]] size_t drumParamsPerPad() noexcept;

/**
    The id of one pad's parameter: "p3.level" for a common one, "p3.snare.snappy" for a model's own.

    @param pad    The pad, 0..drumPadCount - 1 (shown as 1..16).
    @param model  The model whose parameter it is, or DrumModel::off for a common one.
    @param id     The parameter's id within the common or model table.
*/
[[nodiscard]] juce::String drumParamId (int pad, DrumModel model, const char* id);

/**
    The index in the Drum Synth's descriptor of one pad's parameter, or -1 if the model has no such parameter.

    @param pad    The pad, 0..drumPadCount - 1.
    @param model  The model, or DrumModel::off for a common parameter.
    @param id     The parameter's id within the common or model table.
*/
[[nodiscard]] int drumParamIndex (int pad, DrumModel model, const char* id) noexcept;

/**
    The model with an id ("kick"), or nullopt for an unknown one.

    @param id  The id, as drumModelId() gives it.
*/
[[nodiscard]] std::optional<DrumModel> drumModelFromId (const juce::String& id) noexcept;

/**
    The default kit's pads: General MIDI's kick to ride on keys 36-51, hats in choke group 1, kicks and toms mono.
*/
[[nodiscard]] std::vector<DrumPad> defaultDrumPads();

/**
    A node's pads: its own, or the default kit's when it has none.

    @param node  A Drum Synth node.
*/
[[nodiscard]] std::vector<DrumPad> drumPadsOf (const GraphNode& node);

/**
    The Drum Synth's descriptor: Notes in, Main and Out 2-16, drumParamsPerPad() parameters for each pad. Defaults
    follow the default kit (a tom's pitch depends on which tom it is).
*/
[[nodiscard]] const NodeDescriptor& drumSynthDescriptor();

/**
    Registers the Drum Synth. Called by registerBuiltinNodes().

    @param registry  The registry.
*/
void registerDrumSynth (NodeRegistry& registry);

} // namespace dawg
