// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioSettings.h"

namespace collabgod
{
    namespace
    {
        const juce::String audioKey ("audioOutput");
    }

    std::unique_ptr<juce::XmlElement> AudioChoice::toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("AUDIO_OUTPUT");
        xml->setAttribute ("type", type);
        xml->setAttribute ("device", device);
        xml->setAttribute ("sampleRate", sampleRate);
        xml->setAttribute ("bufferSize", bufferSize);
        xml->setAttribute ("audioThreads", audioThreads);
        return xml;
    }

    AudioChoice AudioChoice::fromXml (const juce::XmlElement* xml)
    {
        AudioChoice choice;

        if (xml == nullptr || ! xml->hasTagName ("AUDIO_OUTPUT"))
            return choice;

        choice.type = xml->getStringAttribute ("type");
        choice.device = xml->getStringAttribute ("device");
        // A hand-edited or damaged file must not open a device at a nonsense setting.
        const auto rate = xml->getDoubleAttribute ("sampleRate");
        choice.sampleRate = std::isfinite (rate) && rate > 0.0 ? rate : 0.0;
        choice.bufferSize = juce::jmax (0, xml->getIntAttribute ("bufferSize"));
        choice.audioThreads = juce::jmax (0, xml->getIntAttribute ("audioThreads"));
        return choice;
    }

    std::unique_ptr<juce::XmlElement> AudioChoice::toDeviceSetupXml() const
    {
        // The attributes juce::AudioDeviceManager::createStateXml writes and initialiseFromXML
        // reads. Input device and channels are set to none explicitly: initialiseFromXML opens
        // whatever input it is given, whatever the number of inputs asked for, and that is
        // what made macOS ask the spikes for the microphone.
        auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        xml->setAttribute ("deviceType", type);
        xml->setAttribute ("audioOutputDeviceName", device);
        xml->setAttribute ("audioInputDeviceName", juce::String());
        xml->setAttribute ("audioDeviceInChans", "0");

        if (sampleRate > 0.0)
            xml->setAttribute ("audioDeviceRate", sampleRate);
        if (bufferSize > 0)
            xml->setAttribute ("audioDeviceBufferSize", bufferSize);

        return xml;
    }

    int defaultAudioThreads (int numCpus)
    {
        juce::ignoreUnused (numCpus);
        return 1;
    }

    bool isExclusiveModeType (const juce::String& type)
    {
        return type.containsIgnoreCase ("exclusive");
    }

    int safeExclusiveBlockSize (const juce::Array<int>& available, double sampleRate, int current)
    {
        if (available.isEmpty() || sampleRate <= 0.0)
            return current;

        const auto minimum = static_cast<int> (std::ceil (sampleRate * 0.010));
        auto sorted = available;
        sorted.sort();

        for (const auto size : sorted)
            if (size >= minimum)
                return size;

        return sorted.getLast();
    }

    SettingsFile::SettingsFile (const juce::File& f) : folder (f)
    {
        folder.createDirectory();

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 0; // a crash must not lose what was just chosen
        options.commonToAllUsers = false;
        properties = std::make_unique<juce::PropertiesFile> (getFile(), options);
    }

    juce::File SettingsFile::defaultFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("CollabGod");
    }

    juce::File SettingsFile::getFile() const
    {
        return folder.getChildFile ("settings.xml");
    }

    AudioChoice SettingsFile::audioChoice() const
    {
        const auto xml = properties->getXmlValue (audioKey);
        return AudioChoice::fromXml (xml.get());
    }

    void SettingsFile::setAudioChoice (const AudioChoice& choice)
    {
        const auto xml = choice.toXml();
        properties->setValue (audioKey, xml.get());
        properties->saveIfNeeded();
    }
}
