#include "SystemVolume.h"

#import <UIKit/UIKit.h>
#import <MediaPlayer/MediaPlayer.h>
#import <AVFoundation/AVFoundation.h>

// iOS lets an app set the volume only through the system's own volume control (MPVolumeView).
// The player keeps one, hidden: its slider is the phone's volume, and while it is in the window
// the side buttons don't bring up the system's volume display over the cassette.

namespace tf::pocket::SystemVolume
{
namespace
{
    MPVolumeView* volumeView = nil;

    UIWindow* keyWindow()
    {
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass: UIWindowScene.class])
                for (UIWindow* w in ((UIWindowScene*) scene).windows)
                    if (w.isKeyWindow) return w;
        return nil;
    }

    UISlider* slider()
    {
        for (UIView* v in volumeView.subviews)
            if ([v isKindOfClass: UISlider.class]) return (UISlider*) v;
        return nil;
    }
}

bool available() { return true; }

void attach()
{
    if (volumeView != nil) return;
    auto* window = keyWindow();
    if (window == nil) return;
    [[AVAudioSession sharedInstance] setActive: YES error: nil];   // so its volume can be read
    volumeView = [[MPVolumeView alloc] initWithFrame: CGRectMake (-2000, -2000, 120, 40)];
    volumeView.alpha = 0.01f;
    volumeView.userInteractionEnabled = NO;
    [window addSubview: volumeView];
}

float get()
{
    return [AVAudioSession sharedInstance].outputVolume;
}

void set (float level)
{
    if (auto* s = slider())
    {
        s.value = juce::jlimit (0.0f, 1.0f, level);
        [s sendActionsForControlEvents: UIControlEventValueChanged];
    }
}

int steps() { return 16; }   // the side buttons move it in sixteenths
} // namespace tf::pocket::SystemVolume
