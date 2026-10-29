// The Pocket on a Mac: a screen's true size, so the cassette can be drawn at a real cassette's size.
// (JUCE's Display::dpi on macOS is 72 x the backing scale, not the screen's real density.)

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>

/** Points per millimetre of the screen whose frame (in points) is this size and starts at this x; 0 if unknown. */
double tfPocketScreenPointsPerMm (double frameX, double frameWidth, double frameHeight)
{
    for (NSScreen* s in [NSScreen screens])
    {
        const NSRect f = [s frame];
        if (fabs (f.origin.x - frameX) > 1.0 || fabs (f.size.width - frameWidth) > 1.0 || fabs (f.size.height - frameHeight) > 1.0)
            continue;
        NSNumber* number = [[s deviceDescription] objectForKey: @"NSScreenNumber"];
        if (number == nil) return 0.0;
        const CGSize mm = CGDisplayScreenSize ((CGDirectDisplayID) [number unsignedIntValue]);
        return mm.width > 1.0 ? f.size.width / mm.width : 0.0;
    }
    return 0.0;
}
