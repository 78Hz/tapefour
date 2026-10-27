#pragma once

#include "../Source/Common.h"

namespace tf::pocket::SystemVolume
{
/** On a phone the player has one volume, the phone's own media volume: the side buttons change it,
    and the on-screen keys show and set the same one. On a computer there is none (false), and the
    player keeps a volume of its own. */
bool available();
/** Once the window is up. iOS: a hidden system volume control, so the on-screen keys can set the
    volume and the side buttons don't put the system's volume display over the cassette.
    Android: the side buttons set the media volume even while the tape is stopped. */
void attach();
/** 0..1 */
float get();
void set (float level);
/** How many steps the phone's volume has. */
int steps();
} // namespace tf::pocket::SystemVolume
