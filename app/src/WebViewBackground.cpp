// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows and Linux. macOS is WebViewBackground_mac.mm.

#ifdef _WIN32
 // Before JUCE, whose public headers do not include it, so its macros cannot clash.
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <commctrl.h>
#endif

#include "WebViewBackground.h"

namespace collabgod
{
   #ifdef _WIN32
    namespace
    {
        constexpr UINT_PTR subclassId = 0xC0DE;

        /** Fills what the window has just exposed with the brush passed as `data`, before
            JUCE sees the paint message.

            JUCE validates the area without painting it and draws on the next display refresh.
            While the window is dragged bigger or smaller, Windows shows that area before
            then, white. So does the web view's edge, which follows a moment later. Filling it
            here, at once, shows the page's colour instead. It is safe only because the web
            view covers the whole window and JUCE itself draws nothing there but this colour. */
        LRESULT CALLBACK fillExposedArea (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
        {
            const auto brush = reinterpret_cast<HBRUSH> (data);

            switch (message)
            {
                case WM_ERASEBKGND:
                {
                    RECT client {};
                    GetClientRect (hwnd, &client);
                    FillRect (reinterpret_cast<HDC> (wParam), &client, brush);
                    return 1;
                }

                case WM_PAINT:
                {
                    if (auto region = CreateRectRgn (0, 0, 0, 0))
                    {
                        if (GetUpdateRgn (hwnd, region, FALSE) > NULLREGION)
                        {
                            if (auto dc = GetDC (hwnd))
                            {
                                FillRgn (dc, region, brush);
                                ReleaseDC (hwnd, dc);
                            }
                        }

                        DeleteObject (region);
                    }

                    break; // JUCE still handles the message as before.
                }

                case WM_NCDESTROY:
                    RemoveWindowSubclass (hwnd, fillExposedArea, subclassId);
                    DeleteObject (brush);
                    break;

                default:
                    break;
            }

            return DefSubclassProc (hwnd, message, wParam, lParam);
        }
    }

    void beforeCreatingWebView (juce::Colour colour)
    {
        // WebView2 reads this when it creates its window, so even its first frames and the
        // edge it grows into while resizing use the page's colour. The option JUCE sets
        // (WinWebView2::withBackgroundColour) only applies once the web view exists.
        const auto argb = juce::String::toHexString ((int) colour.getARGB()).paddedLeft ('0', 8).toUpperCase();
        SetEnvironmentVariableW (L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", argb.toWideCharPointer());
    }

    void matchWebViewBackground (juce::WebBrowserComponent&, juce::Colour)
    {
        // Windows: WinWebView2::withBackgroundColour in WebUi.cpp and beforeCreatingWebView do it.
    }

    void matchWindowBackground (juce::Component& window, juce::Colour colour)
    {
        auto* peer = window.getPeer();
        if (peer == nullptr)
            return;

        const auto hwnd = static_cast<HWND> (peer->getNativeHandle());
        DWORD_PTR existing = 0;
        if (hwnd == nullptr || GetWindowSubclass (hwnd, fillExposedArea, subclassId, &existing))
            return;

        const auto brush = CreateSolidBrush (RGB (colour.getRed(), colour.getGreen(), colour.getBlue()));
        if (brush != nullptr && ! SetWindowSubclass (hwnd, fillExposedArea, subclassId, reinterpret_cast<DWORD_PTR> (brush)))
            DeleteObject (brush);
    }
   #else
    void beforeCreatingWebView (juce::Colour) {}
    void matchWebViewBackground (juce::WebBrowserComponent&, juce::Colour) {}
    void matchWindowBackground (juce::Component&, juce::Colour) {}
   #endif
}
