#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace dawg
{

/**
    Creates a CLAP plugin's own editor, embedded in a JUCE component (docs/design/clap-hosting.md, section 3.3).

    @param processor  The plugin.
    @return           The editor, or nullptr if the processor isn't a CLAP plugin with an editor it can embed here.
*/
[[nodiscard]] std::unique_ptr<juce::AudioProcessorEditor> createClapEditor (juce::AudioProcessor& processor);

} // namespace dawg
