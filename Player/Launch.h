#pragma once

#include "../Source/Common.h"

namespace tf::pocket
{
/** The document the player was opened with by another app, if any (Android: the Intent's data,
    usually a content:// address; elsewhere the system passes it on the command line instead). */
juce::URL documentOpenedWith();
} // namespace tf::pocket
