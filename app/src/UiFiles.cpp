// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UiFiles.h"

#include "BridgeContract.h"

namespace collabgod
{
    namespace
    {
        std::vector<std::byte> toBytes (const void* data, size_t size)
        {
            const auto* bytes = static_cast<const std::byte*> (data);
            return { bytes, bytes + size };
        }

        juce::String escapeHtml (const juce::String& s)
        {
            return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;");
        }
    }

    juce::String mimeTypeFor (const juce::File& file)
    {
        const auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".html") return "text/html";
        if (ext == ".js" || ext == ".mjs") return "text/javascript";
        if (ext == ".css") return "text/css";
        if (ext == ".json" || ext == ".map") return "application/json";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".png") return "image/png";
        if (ext == ".woff2") return "font/woff2";
        if (ext == ".wasm") return "application/wasm";
        return "application/octet-stream";
    }

    std::optional<juce::WebBrowserComponent::Resource> serveUiFile (const juce::File& folder, const juce::String& path)
    {
        auto relative = path.upToFirstOccurrenceOf ("?", false, false)
                            .upToFirstOccurrenceOf ("#", false, false)
                            .trimCharactersAtStart ("/");

        if (relative.isEmpty())
            relative = "index.html";

        // Never anything outside the folder.
        if (relative.contains ("..") || relative.contains ("\\") || relative.contains (":"))
            return std::nullopt;

        const auto file = folder.getChildFile (relative);

        if (! file.isAChildOf (folder) || ! file.existsAsFile())
            return std::nullopt;

        juce::MemoryBlock data;
        if (! file.loadFileAsData (data))
            return std::nullopt;

        return juce::WebBrowserComponent::Resource { toBytes (data.getData(), data.getSize()), mimeTypeFor (file) };
    }

    juce::WebBrowserComponent::Resource missingUiPage (const juce::File& folder)
    {
        const auto background = "#" + juce::Colour (bridge::backgroundArgb).toDisplayString (false).toLowerCase();
        const auto html = "<!doctype html><meta charset=utf-8><title>CollabGod</title>"
                          "<body style=\"font:14px system-ui;background:" + background + ";color:#d8dde8;padding:2em\">"
                          "<h1>The interface has not been built</h1>"
                          "<p>There is no <code>index.html</code> in <code>" + escapeHtml (folder.getFullPathName()) + "</code>.</p>"
                          "<p>At the root of the repository run <code>npm run build</code>, then start the app again.</p>";

        return { toBytes (html.toRawUTF8(), html.getNumBytesAsUTF8()), "text/html" };
    }

    FolderWatcher::FolderWatcher (juce::File f) : folder (std::move (f)), settled (fingerprint()), pending (settled)
    {
    }

    juce::String FolderWatcher::fingerprint() const
    {
        juce::StringArray entries;
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
            entries.add (entry.getFile().getRelativePathFrom (folder) + ":" + juce::String (entry.getFileSize())
                         + ":" + juce::String (entry.getModificationTime().toMilliseconds()));
        entries.sort (false);
        return entries.joinIntoString ("\n");
    }

    bool FolderWatcher::poll()
    {
        const auto now = fingerprint();

        if (now != pending)
        {
            pending = now; // still changing: wait for it to stay the same
            return false;
        }

        if (now == settled)
            return false;

        settled = now;
        return true;
    }
}
