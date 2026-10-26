#include "TapeWriter.h"

namespace tf
{
TapeWriter::TapeWriter() : juce::Thread ("TapeFour tape writer"), idleEvent (true)
{
    idleEvent.signal();
    startThread (juce::Thread::Priority::normal);
}

TapeWriter::~TapeWriter()
{
    waitUntilIdle();
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

void TapeWriter::enqueue (std::function<void()> job, std::function<void()> whenDone)
{
    {
        const juce::ScopedLock sl (lock);
        jobs.push_back ({ std::move (job), std::move (whenDone) });
        idleEvent.reset();
    }
    wake.signal();
}

bool TapeWriter::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return running || ! jobs.empty();
}

void TapeWriter::waitUntilIdle()
{
    idleEvent.wait (-1);
}

void TapeWriter::run()
{
    while (! threadShouldExit())
    {
        Job next;
        {
            const juce::ScopedLock sl (lock);
            if (jobs.empty())
            {
                running = false;
                idleEvent.signal();
            }
            else
            {
                next = std::move (jobs.front());
                jobs.pop_front();
                running = true;
            }
        }

        if (next.job == nullptr)
        {
            wake.wait (500);
            continue;
        }

        next.job();
        if (next.whenDone)
            juce::MessageManager::callAsync (std::move (next.whenDone));
    }
}

void TapeWriter::saveTape (Tape& tape, std::function<void (bool)> whenDone)
{
    // Snapshot what the message thread owns.
    auto folder = TapeShelf::folderFor (tape.id);
    auto meta = tape.snapshot();
    std::shared_ptr<juce::XmlElement> session;
    if (tape.session != nullptr && tape.sessionNeedsWriting)
    {
        session = std::make_shared<juce::XmlElement> (*tape.session);
        tape.sessionNeedsWriting = false;
    }
    tape.markMetaSaved();

    auto ok = std::make_shared<bool> (true);
    auto* t = &tape;
    enqueue ([t, folder, meta, session, ok]
             {
                 // Audio first: the cassette's description never claims a recording whose audio isn't on disk.
                 *ok = t->writeAudio (folder);
                 if (session != nullptr) *ok = Tape::writeSession (*session, folder) && *ok;
                 *ok = Tape::writeSnapshot (*meta, folder) && *ok;
             },
             [ok, whenDone, session, t]
             {
                 // Saving failed: mark it so the next autosave tries again. (The tape is still alive:
                 // a retired tape is deleted after this has run.)
                 if (! *ok)
                 {
                     t->restoreDirtyAfterFailedSave();
                     if (session != nullptr) t->sessionNeedsWriting = true;
                 }
                 if (whenDone) whenDone (*ok);
             });
}

void TapeWriter::retire (std::unique_ptr<Tape> tape)
{
    if (tape == nullptr) return;
    // Deleted on the message thread, after the callbacks of every job queued before it.
    std::shared_ptr<Tape> holder (tape.release());
    enqueue ([] {}, [holder] {});
}
} // namespace tf
