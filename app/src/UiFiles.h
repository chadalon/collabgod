// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Serving the interface (packages/interface/dist) to the web view, and noticing when
// it changes so a development build can reload it.

#pragma once

#include <JuceHeader.h>

#include <optional>

namespace collabgod
{
    /** Serves a file from `folder` for a request path: "/" is index.html. Nothing outside
        the folder, and nothing that is missing. */
    std::optional<juce::WebBrowserComponent::Resource> serveUiFile (const juce::File& folder, const juce::String& path);

    /** The MIME type for a file served to the web view. */
    juce::String mimeTypeFor (const juce::File&);

    /** A page that says the interface has not been built, served in its place. */
    juce::WebBrowserComponent::Resource missingUiPage (const juce::File& folder);

    /** Notices when anything in a folder changes: a file added, removed, resized or rewritten.
        A change counts once the folder has stayed the same for one more poll, so a build
        that writes several files is reloaded once, after it has finished. */
    class FolderWatcher
    {
    public:
        explicit FolderWatcher (juce::File folder);

        /** Looks at the folder again. True once, when a change has settled. */
        bool poll();

    private:
        juce::String fingerprint() const;

        juce::File folder;
        juce::String settled;
        juce::String pending;
    };
}
