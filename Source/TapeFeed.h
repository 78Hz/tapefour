#pragma once

namespace tf
{
/** What a processor in a chain can listen to besides its own input, for this block (a sampler's sources):
    every track's tape playback (this block), and every track as it leaves its channel strip, before and
    after its fader (the block before this one: the tracks run one after another), with whether each is
    heard in the mix (mute and solo). Mono, the same length as the block being processed. */
struct TrackFeed
{
    static constexpr int maxTracks = 8;
    int numTracks = 0;
    int ownTrack = -1;                         // the track this chain belongs to
    const float* tape[maxTracks] {};
    const float* preFader[maxTracks] {};
    const float* postFader[maxTracks] {};
    bool heard[maxTracks] {};
};

/** A processor that listens to the feed: the engine hands it over just before each block. */
struct TapeFeedListener
{
    virtual ~TapeFeedListener() = default;
    virtual void tapeFeed (const TrackFeed&) noexcept = 0;
};
} // namespace tf
