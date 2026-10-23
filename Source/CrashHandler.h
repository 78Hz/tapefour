#pragma once

#include "Common.h"

namespace tf::CrashHandler
{
/** Writes a crash report if TapeFour crashes: a minidump on Windows, and on every platform a
    text file with the version and a stack trace, in <app data>/TapeFour/Crashes. */
void install();

juce::File getFolder();

/** Reports written since the last time this was asked (for "TAPEFOUR CLOSED UNEXPECTEDLY" on start). */
int takeNewReportCount (juce::PropertiesFile& settings);
} // namespace tf::CrashHandler
