#pragma once

#include "Common.h"

namespace tf::CrashHandler
{
/** Writes a crash report if TapeFour crashes, in <app data>/TapeFour/Crashes: on macOS and Linux a text
    file with the version, the signal and a stack trace; on Windows a minidump (with the stack) and a
    text file with the version. The writing uses nothing that's unsafe after a crash. The newest 20
    reports are kept. */
void install();

juce::File getFolder();

/** Reports written since the last time this was asked (for "TAPEFOUR CLOSED UNEXPECTEDLY" on start). */
int takeNewReportCount (juce::PropertiesFile& settings);
} // namespace tf::CrashHandler
