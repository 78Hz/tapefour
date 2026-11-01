#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace tf
{
/** A processor's editor, for a window to own. If the processor still points at an editor (one already
    gone, or one somewhere else), it's told to forget it first: it makes no new one while it thinks it has
    one, and the window would open empty. */
inline juce::AudioProcessorEditor* takeEditor (juce::AudioProcessor& p)
{
    if (auto* old = p.getActiveEditor())
        p.editorBeingDeleted (old);
    return p.createEditorIfNeeded();
}

/** Called just before deleting an editor taken with takeEditor: tells its processor it's going. JUCE's
    editors no longer tell it themselves: VST3, AU and LV2 plug-ins do it from their own editors, but the
    built-in instruments and CLAP plug-ins don't, so without this the next window opens empty. */
inline void editorGoing (juce::AudioProcessor& p, juce::AudioProcessorEditor* editor)
{
    if (editor != nullptr)
        p.editorBeingDeleted (editor);
}
} // namespace tf
