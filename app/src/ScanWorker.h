#pragma once

#include <juce_core/juce_core.h>

namespace dawg
{

/**
    Command-line switch that runs the executable as a one-shot plugin scanner.

    Usage: `DAWg --scan-plugin FORMAT_NAME FILE_OR_IDENTIFIER OUTPUT_XML_FILE`

    Scanning happens in a child process so a plugin that crashes or hangs while being loaded cannot take
    the DAW down with it.
*/
inline constexpr const char* scanWorkerSwitch = "--scan-plugin";

/**
    Process exit codes of the scan worker.
*/
enum class ScanWorkerExitCode
{
    success = 0,
    badArguments = 2,
    writeFailed = 3,
    unknownFormat = 4,
};

/**
    Runs the scan described by the command line and writes the found plugins as XML.

    @param args  The process command line, split into arguments.
    @return      The process exit code.
*/
[[nodiscard]] ScanWorkerExitCode runScanWorker (const juce::StringArray& args);

} // namespace dawg
