// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's entry point: one window holding the interface in a web view, and the bridge
// between them (#26). See usage() for the command line.

#include <JuceHeader.h>

#include "BridgeHost.h"
#include "BridgeTests.h"
#include "Log.h"
#include "ProjectFileTests.h"
#include "SmokeTest.h"
#include "WebUi.h"
#include "WebViewBackground.h"

#include <iostream>

namespace collabgod
{
    namespace
    {
        juce::String usage()
        {
            return "Usage: CollabGod [options]\n"
                   "  --dev                        Serve the interface from packages/interface/dist in the source tree\n"
                   "                               and reload it when it changes (the default in Debug builds)\n"
                   "  --ui-dir <folder>            Serve the interface from this folder, and reload it when it changes\n"
                   "  --frame-delivery push|pull   How engine frames reach the page (default pull; ADR-0016)\n"
                   "  --diagnostics                Open the diagnostics panel at start (Ctrl+Shift+D or Cmd+Shift+D toggles it)\n"
                   "  --bridge-check               Open the window, run the bridge self-check in the page, close the\n"
                   "                               window and exit: 0 if every message crossed the bridge both ways\n"
                   "  --bridge-test                Run the bridge contract tests without a window\n"
                   "  --project-file-test          Run the atomic save tests, crashes included, without a window\n"
                   "  --smoke-test                 Check the linked engine without a window or audio device\n"
                   "  --version                    Print the version and quit\n";
        }

        juce::String valueAfter (const juce::StringArray& args, const juce::String& flag)
        {
            const auto i = args.indexOf (flag);
            return i >= 0 && i + 1 < args.size() && ! args[i + 1].startsWith ("--") ? args[i + 1] : juce::String();
        }

        /** The interface copied beside the app by the build: Contents/Resources/ui in the macOS
            bundle, a `ui` folder next to the executable elsewhere. */
        juce::File bundledUiFolder()
        {
           #if JUCE_MAC
            return juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/ui");
           #else
            return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ui");
           #endif
        }

        /** The interface's build output in the source tree, which `npm run dev` rebuilds on change. */
        juce::File sourceUiFolder()
        {
            return juce::File (COLLABGOD_SOURCE_UI_DIR);
        }

       #if COLLABGOD_DEVELOPMENT
        constexpr bool developmentByDefault = true;
       #else
        constexpr bool developmentByDefault = false;
       #endif
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (BridgeHost& host, UiSettings ui)
            : DocumentWindow (JUCE_APPLICATION_NAME_STRING,
                              juce::Colour (bridge::backgroundArgb),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new WebUi (host, std::move (ui)), false);
            setResizable (true, true);
            setResizeLimits (640, 400, 10000, 10000);
            centreWithSize (1280, 800);
            setVisible (true);

            // After the last change that recreates the native window (the title bar setting).
            matchWindowBackground (*this, juce::Colour (bridge::backgroundArgb));
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    class Application final : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
        const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
        bool moreThanOneInstanceAllowed() override { return true; }

        void initialise (const juce::String& commandLine) override
        {
            juce::StringArray args;
            for (const auto& arg : juce::StringArray::fromTokens (commandLine, true))
                args.add (arg.unquoted());

            if (args.contains ("--help") || args.contains ("-h"))
            {
                std::cout << usage();
                quitWith (0);
                return;
            }

            if (args.contains ("--version"))
            {
                std::cout << versionLine() << std::endl;
                quitWith (0);
                return;
            }

            if (args.contains ("--smoke-test"))
            {
                quitWith (runSmokeTest());
                return;
            }

            if (args.contains ("--bridge-test"))
            {
                quitWith (runBridgeTests());
                return;
            }

            if (args.contains ("--project-file-test"))
            {
                quitWith (runProjectFileTests());
                return;
            }

            // Not in --help: the crash test starts the app this way, then kills it.
            if (const auto target = valueAfter (args, "--project-file-crash-writer"); target.isNotEmpty())
            {
                quitWith (runCrashWriter (juce::File (target)));
                return;
            }

            startLog();
            log (versionLine() + " on " + juce::SystemStats::getOperatingSystemName());
            log ("Log file: " + logFile().getFullPathName());

            HostSettings hostSettings;
            UiSettings uiSettings;

            const auto uiDir = valueAfter (args, "--ui-dir");
            if (uiDir.isNotEmpty())
            {
                uiSettings.folder = juce::File::getCurrentWorkingDirectory().getChildFile (uiDir);
                uiSettings.reloadOnChange = true;
            }
            else if (args.contains ("--dev") || developmentByDefault)
            {
                uiSettings.folder = sourceUiFolder();
                uiSettings.reloadOnChange = true;
            }
            else
            {
                uiSettings.folder = bundledUiFolder();
            }

            hostSettings.development = uiSettings.reloadOnChange;
            hostSettings.showDiagnostics = args.contains ("--diagnostics");
            hostSettings.selfCheck = args.contains ("--bridge-check");

            const auto delivery = valueAfter (args, "--frame-delivery");
            if (delivery == "push")
                hostSettings.frameDelivery = bridge::FrameDelivery::push;
            else if (delivery.isNotEmpty() && delivery != "pull")
            {
                std::cout << "--frame-delivery takes push or pull, not " << delivery << "\n" << usage();
                quitWith (2);
                return;
            }

            log ("Serving the interface from " + uiSettings.folder.getFullPathName());

            host = std::make_unique<BridgeHost> (hostSettings);

            if (hostSettings.selfCheck)
                startBridgeCheck();

            mainWindow = std::make_unique<MainWindow> (*host, std::move (uiSettings));

            // Started from a terminal (ctest), the app is not activated and its window can open behind
            // others. WKWebView stops requestAnimationFrame in a covered window, and pulled frames come
            // from that loop, so the check would fail on a desktop with other windows open.
            if (hostSettings.selfCheck)
            {
                juce::Process::makeForegroundProcess();
                mainWindow->toFront (true);
            }
        }

        void shutdown() override
        {
            if (host == nullptr)
                return; // --version and the tests open nothing and log nothing

            mainWindow.reset();
            host.reset();
            log ("Quit");
            stopLog();
        }

    private:
        /** --bridge-check: the page runs its self-check and reports it. Then the window is closed
            the way its close button closes it, so the run also shows the process ends with it. */
        void startBridgeCheck()
        {
            setApplicationReturnValue (1);

            host->onSelfCheckDone = [this] (bool passed)
            {
                log (passed ? "PASS bridge check: every message crossed the bridge both ways"
                            : "FAIL bridge check");
                setApplicationReturnValue (passed ? 0 : 1);

                // After the page's call has been answered.
                juce::MessageManager::callAsync ([this]
                {
                    log ("Closing the window");
                    if (mainWindow != nullptr)
                        mainWindow->closeButtonPressed();
                });
            };

            // A page that never reports must not hang a test run. Start-up took up to 13 s on CI runners.
            juce::Timer::callAfterDelay (120000, [this]
            {
                log ("FAIL bridge check: the page did not report within 120 s");
                quitWith (1);
            });
        }

        void quitWith (int exitCode)
        {
            setApplicationReturnValue (exitCode);
            quit();
        }

        std::unique_ptr<BridgeHost> host;
        std::unique_ptr<MainWindow> mainWindow;
    };
}

START_JUCE_APPLICATION (collabgod::Application)
