#include "ScanWorker.h"

#include "dawg/ClapPlugin.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

namespace dawg
{

ScanWorkerExitCode runScanWorker (const juce::StringArray& args)
{
    const auto switchIndex = args.indexOf (scanWorkerSwitch);

    if (switchIndex < 0 || args.size() < switchIndex + 4)
        return ScanWorkerExitCode::badArguments;

    const auto formatName = args[switchIndex + 1].unquoted();
    const auto identifier = args[switchIndex + 2].unquoted();
    const juce::File output (args[switchIndex + 3].unquoted());

    juce::AudioPluginFormatManager formats;
    juce::addHeadlessDefaultFormatsToManager (formats);
    formats.addFormat (std::make_unique<ClapPluginFormat>());

    for (auto* format : formats.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, identifier);

        juce::XmlElement root ("PLUGINS");

        for (const auto* description : found)
            root.addChildElement (description->createXml().release());

        return root.writeTo (output) ? ScanWorkerExitCode::success : ScanWorkerExitCode::writeFailed;
    }

    return ScanWorkerExitCode::unknownFormat;
}

} // namespace dawg
