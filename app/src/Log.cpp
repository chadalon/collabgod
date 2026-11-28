// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Log.h"

#include <iostream>

namespace collabgod
{
    namespace
    {
        /** JUCE's file logger, plus standard output so a terminal or CI sees the same lines. */
        class AppLogger final : public juce::Logger
        {
        public:
            AppLogger()
                : file (juce::FileLogger::createDefaultAppLogger ("CollabGod", "CollabGod.log",
                                                                  "CollabGod " + juce::String (JUCE_APPLICATION_VERSION_STRING),
                                                                  512 * 1024))
            {
            }

            void logMessage (const juce::String& message) override
            {
                std::cout << message << std::endl;
                if (file != nullptr)
                    file->logMessage (message);
            }

            juce::File getFile() const { return file != nullptr ? file->getLogFile() : juce::File(); }

        private:
            std::unique_ptr<juce::FileLogger> file;
        };

        std::unique_ptr<AppLogger> logger;
        int silenced = 0;
    }

    ScopedSilentLog::ScopedSilentLog() { ++silenced; }
    ScopedSilentLog::~ScopedSilentLog() { --silenced; }

    namespace
    {
        std::unique_ptr<juce::StringArray> captured;
    }

    ScopedLogCapture::ScopedLogCapture()
    {
        jassert (captured == nullptr);
        captured = std::make_unique<juce::StringArray>();
    }

    ScopedLogCapture::~ScopedLogCapture()
    {
        captured.reset();
    }

    const juce::StringArray& ScopedLogCapture::lines() const
    {
        return *captured;
    }

    void startLog()
    {
        logger = std::make_unique<AppLogger>();
        juce::Logger::setCurrentLogger (logger.get());
    }

    void stopLog()
    {
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    juce::File logFile()
    {
        return logger != nullptr ? logger->getFile() : juce::File();
    }

    void log (const juce::String& message)
    {
        if (captured != nullptr)
            captured->add (message);

        if (silenced > 0)
            return;

        const auto line = juce::Time::getCurrentTime().formatted ("%H:%M:%S ") + message;
        if (logger != nullptr)
            logger->logMessage (line);
        else
            std::cout << line << std::endl;
    }
}
