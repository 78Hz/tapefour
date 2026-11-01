#pragma once

namespace tf
{
/** A processor in a chain that can listen to the tape: the engine hands it every track's playback
    (mono, the same stretch as the block it's processing) just before each block. */
struct TapeFeedListener
{
    virtual ~TapeFeedListener() = default;
    virtual void tapeFeed (const float* const* tracks, int numTracks) noexcept = 0;
};
} // namespace tf
