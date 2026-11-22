#include "AppLog.h"

#include <memory>
#include <mutex>

namespace dawg::applog
{

namespace
{
constexpr juce::int64 keptBytes = juce::int64 { 1024 } * 1024;

std::mutex& lock()
{
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<juce::FileLogger>& logger()
{
    static std::unique_ptr<juce::FileLogger> instance;
    return instance;
}
} // namespace

void open (const juce::File& folder)
{
    const std::scoped_lock guard (lock());

    if (logger() != nullptr)
        return;

    const auto logFile = folder.getChildFile ("Logs").getChildFile ("DAWg.log");
    std::ignore = logFile.getParentDirectory().createDirectory();
    const auto welcome = "DAWg " + juce::String (JUCE_APPLICATION_VERSION_STRING) + " started, "
                         + juce::SystemStats::getOperatingSystemName() + ", "
                         + juce::String (juce::SystemStats::getNumCpus()) + " CPUs";
    logger() = std::make_unique<juce::FileLogger> (logFile, welcome, keptBytes);
}

void write (const juce::String& message)
{
    const std::scoped_lock guard (lock());

    if (logger() != nullptr)
        logger()->logMessage (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S ") + message);
}

juce::File file()
{
    const std::scoped_lock guard (lock());
    return logger() != nullptr ? logger()->getLogFile() : juce::File();
}

void close()
{
    write ("DAWg quit");
    const std::scoped_lock guard (lock());
    logger().reset();
}

} // namespace dawg::applog
