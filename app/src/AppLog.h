#pragma once

#include <juce_core/juce_core.h>

/**
    DAWg's log file (docs/design/ui-recovery.md, UR1): what happened to the app and its windows, with times, for working
    out afterwards what went wrong. One file, Logs/DAWg.log in the app's data folder, trimmed to its last megabyte at
    each start.
*/
namespace dawg::applog
{

/**
    Opens the log in a folder and writes the start of a session. Call once, early.

    @param folder  The app's data folder; the log goes into its Logs folder.
*/
void open (const juce::File& folder);

/**
    Adds a line, with the time. Any thread; does nothing before open().

    @param message  What happened.
*/
void write (const juce::String& message);

/**
    Returns the log file, for showing the user where it is.
*/
[[nodiscard]] juce::File file();

/**
    Closes the log, writing the end of the session.
*/
void close();

} // namespace dawg::applog
