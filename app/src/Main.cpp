// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's entry point. For now a placeholder window: the web view host and the
// typed bridge come with #26.
//
//   CollabGod                Open the window
//   CollabGod --version      Print the version and quit
//   CollabGod --smoke-test   Check the linked engine without a window or audio device

#include <JuceHeader.h>

#include "SmokeTest.h"

#include <iostream>

namespace collabgod
{
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : DocumentWindow (JUCE_APPLICATION_NAME_STRING,
                              juce::Colours::black,
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);

            label.setText (versionLine(), juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setColour (juce::Label::textColourId, juce::Colours::white);
            label.setSize (800, 500);
            setContentNonOwned (&label, true);

            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        juce::Label label;

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
            const auto args = juce::StringArray::fromTokens (commandLine, true);

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

            mainWindow = std::make_unique<MainWindow>();
        }

        void shutdown() override
        {
            mainWindow.reset();
        }

    private:
        void quitWith (int exitCode)
        {
            setApplicationReturnValue (exitCode);
            quit();
        }

        std::unique_ptr<MainWindow> mainWindow;
    };
}

START_JUCE_APPLICATION (collabgod::Application)
