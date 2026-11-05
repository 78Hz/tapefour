#pragma once

#include <juce_core/juce_core.h>

namespace tf
{
/** A built-in instrument with outputs of its own beside its main one (FM Drums: one a drum track).
    Each extra output is also in the main mix unless it has been moved to another track, so the
    instrument needs telling when one leaves. aux counts from 0 (the first output after the main). */
struct MultiOutSource
{
    virtual ~MultiOutSource() = default;
    /** Any thread: true takes that output's sound out of the main mix. */
    virtual void setOutputLeavesMain (int aux, bool leaves) noexcept = 0;
    /** What the output carries now (the machine on that drum track, say). Message thread. */
    virtual juce::String outputLabel (int aux) const = 0;
};
} // namespace tf
