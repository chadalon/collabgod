// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace collabgod
{
    /** Write `size` bytes to `target` so that a crash at any moment leaves either the whole
        old file or the whole new one, never a mix or a truncated file (#25).

        The bytes go to a new temporary file beside the target, which is flushed to the disk
        and then renamed over the target in one step. On POSIX the folder is flushed too, so
        the rename itself survives a power cut; on macOS the flush is F_FULLFSYNC, since a
        plain fsync there does not reach the disk. If anything fails before the rename, the
        temporary file is removed and the target is untouched. A temporary file can be left
        behind only by a crash; it is named `.<target name>.<random>.tmp` and is never read.

        Project files and this install's sample location table are written this way. Never
        call it on the audio thread: it blocks on the disk. */
    juce::Result writeFileAtomically (const juce::File& target, const void* data, size_t size);

    /** The same, for text: written as UTF-8, with no byte order mark. */
    juce::Result writeFileAtomically (const juce::File& target, const juce::String& text);

    /** The name pattern of the temporary files `writeFileAtomically` makes beside `target`. */
    juce::String atomicWriteTemporaryPattern (const juce::File& target);
}
