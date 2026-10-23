#pragma once

#include "TapeShelf.h"

namespace tf
{
/**
    One background thread for all the slow tape work: saving (including every few
    seconds while recording, so a crash loses seconds rather than a take), loading,
    and making blank tapes. Jobs run one at a time, in the order they were queued,
    so "save this tape, then load that one" can't overlap.
*/
class TapeWriter : private juce::Thread
{
public:
    TapeWriter();
    ~TapeWriter() override;

    /** Runs job on the background thread; then (if given) whenDone on the message thread. */
    void enqueue (std::function<void()> job, std::function<void()> whenDone = {});

    /** Blocks until every queued job has finished. */
    void waitUntilIdle();
    bool isBusy() const;

    /** Queues a save of everything about this tape that changed. Tape metadata and the deck are
        snapshotted now (message thread); the audio is written in the background.
        whenDone(ok) runs on the message thread. Tapes taken out go through retire(), so the
        tape outlives the job. */
    void saveTape (Tape& tape, std::function<void (bool ok)> whenDone = {});

    /** A tape that has been taken out: it is deleted once every job and every whenDone queued
        before now has finished with it. */
    void retire (std::unique_ptr<Tape> tape);

private:
    void run() override;

    juce::CriticalSection lock;
    juce::WaitableEvent wake, idleEvent;
    struct Job { std::function<void()> job, whenDone; };
    std::deque<Job> jobs;
    bool running = false;
};
} // namespace tf
