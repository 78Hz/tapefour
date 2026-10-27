#include "PlayerEngine.h"

namespace tf::pocket
{
PlayerEngine::PlayerEngine()
{
    engine.setReadOnly (true);   // a player: it never records, never moves the cassette's own position, never wears it
    engine.params.speedSel = 1;
    engine.params.nrOn = true;
    engine.params.nrType = 1;    // B
    engine.params.pitch = 0.0f;
    setPocketMachine (engine.machine);
    setVolume (7);
}

PlayerEngine::~PlayerEngine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void PlayerEngine::setPocketMachine (TapeEngine::Machine& m)
{
    // A good portable: more wow and flutter than a deck, a little fast, heads to about 14 kHz.
    m.wowPct = 0.18f;
    m.flutterPct = 0.12f;
    m.driftPct = 0.3f;
    m.speedErrorPct = 0.4f;
    m.cutoffHz = 14000.0f;
    m.crosstalkDb = -60.0f;
    m.honoursTapeCondition = true;
}

juce::String PlayerEngine::start()
{
    auto error = deviceManager.initialiseWithDefaultDevices (0, 2);
    deviceManager.addAudioCallback (this);
    return error;
}

std::unique_ptr<Tape> PlayerEngine::load (std::unique_ptr<Tape> t, int sideUp, double where)
{
    engine.command (TapeEngine::Command::stop);
    const juce::SpinLock::ScopedLockType sl (lock);
    auto old = std::move (tape);
    tape = std::move (t);
    side = 0;
    if (tape != nullptr)
    {
        side = sideUp == 1 ? 1 : 0;
        tape->turnOver (side == 1);
        // Automatic tape select: the playback EQ the cassette's notches ask for.
        engine.params.tapeSwitch = tape->naturalPosition();
    }
    engine.setTape (tape.get(), false);
    const auto at = tape != nullptr ? juce::jlimit (0.0, (double) tape->getLengthCells(), where) : 0.0;
    engine.seek (at);
    position = at;
    return old;
}

void PlayerEngine::press (Key k)
{
    switch (k)
    {
        case Key::play: engine.command (TapeEngine::Command::play); break;
        case Key::stop: engine.command (TapeEngine::Command::stop); break;
        case Key::ff:   engine.command (TapeEngine::Command::ff); break;
        case Key::rew:  engine.command (TapeEngine::Command::rew); break;
    }
}

void PlayerEngine::turnOver()
{
    engine.command (TapeEngine::Command::stop);
    const juce::SpinLock::ScopedLockType sl (lock);
    if (tape == nullptr) return;
    const auto len = (double) tape->getLengthCells();
    const auto pos = engine.getPosition();   // the audio callback is locked out
    side = 1 - side;
    tape->turnOver (side == 1);
    engine.setTape (tape.get(), false);
    const auto at = juce::jlimit (0.0, len, len - pos);   // side B's start is side A's end
    engine.seek (at);
    position = at;
}

double PlayerEngine::getPosition() const noexcept
{
    return position.load();
}

double PlayerEngine::getFraction() const noexcept
{
    if (tape == nullptr) return 0.0;
    return juce::jlimit (0.0, 1.0, position.load() / (double) tape->getLengthCells());
}

void PlayerEngine::setVolume (int v) noexcept
{
    v = juce::jlimit (0, 10, v);
    volume = v;
    // The wheel's numbers: 10 is full level, each step down about 4.5 dB, 0 is off.
    gain = v == 0 ? 0.0f : juce::Decibels::decibelsToGain (-4.5f * (float) (10 - v));
}

void PlayerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    maxBlock = juce::jmax (4096, device->getCurrentBufferSizeSamples() * 2);
    for (int t = 0; t < numTracks; ++t)
    {
        outs[t].assign ((size_t) maxBlock, 0.0f);
        outPtrs[t] = outs[t].data();
    }
    const juce::SpinLock::ScopedLockType sl (lock);
    engine.prepare (sampleRate, maxBlock);
    engine.setTape (tape.get(), false);
    smoothedGain.reset (sampleRate, 0.05);
    smoothedGain.setCurrentAndTargetValue (gain.load());
}

void PlayerEngine::audioDeviceStopped()
{
    engine.command (TapeEngine::Command::stop);
}

void PlayerEngine::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);

    const juce::SpinLock::ScopedTryLockType sl (lock);
    if (! sl.isLocked() || tape == nullptr || n > maxBlock) return;

    bool armed[numTracks] {};
    engine.beginBlock (n, armed);
    engine.readAll (outPtrs, n);
    engine.endBlock();
    position = engine.getPosition();

    // One stereo head: the deck's tracks 1 and 2 are left and right.
    smoothedGain.setTargetValue (gain.load());
    for (int i = 0; i < n; ++i)
    {
        const auto g = smoothedGain.getNextValue();
        if (numOut > 0 && out[0] != nullptr) out[0][i] = outs[0][(size_t) i] * g;
        if (numOut > 1 && out[1] != nullptr) out[1][i] = outs[1][(size_t) i] * g;
    }
}
} // namespace tf::pocket
