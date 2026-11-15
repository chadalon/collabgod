// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.
//
// The app for trying real plugins on real machines. Run two copies, on one
// machine or two, load the same VST3 in both, connect them, and move a knob in
// one plugin window: it moves in the other. Load a preset or edit an LFO: the
// plugin's saved state follows (#13). The window shows the message rates and
// counters the findings need, and writes a report to paste into the issue.
// See README.md, "Try it with real plugins", and README-STATE.md.

#include <JuceHeader.h>

#include "../host/Link.h"
#include "../host/PluginSession.h"
#include "../selftest/SelfTest.h"

namespace
{
namespace te = tracktion;
using spike::PluginSession;

juce::File reportFolder()
{
    // Not Documents/CollabGod: folder names ignore case on Windows and macOS, so
    // that is where a clone of the repository in Documents is likely to be.
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("CollabGod Spike Reports").getChildFile ("plugin-params");
}

juce::String randomMember()
{
    return "m-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt (0x1000000)).paddedLeft ('0', 6);
}

//==============================================================================
/** The plugin's own window. */
class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessorEditor* editor, const juce::String& title, std::function<void()> closed)
        : DocumentWindow (title, juce::Colours::darkgrey, DocumentWindow::closeButton), onClosed (std::move (closed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (onClosed)
            juce::MessageManager::callAsync (onClosed);
    }

private:
    std::function<void()> onClosed;
};

//==============================================================================
/** Per-second rates, and the highest seen since the last reset. */
struct Rates
{
    struct Row { const char* name; double now = 0.0, peak = 0.0; };
    std::array<Row, 6> rows { { { "plugin reports/s" }, { "reach the sync/s" }, { "changes sent/s" },
                                { "messages sent/s" }, { "bytes sent/s" }, { "changes received/s" } } };
    std::array<std::uint64_t, 6> last {};
    double lastMs = 0.0;

    void update (const std::array<std::uint64_t, 6>& totals, double nowMs)
    {
        if (lastMs > 0.0)
        {
            const double s = (nowMs - lastMs) / 1000.0;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                rows[i].now = (double) (totals[i] - last[i]) / s;
                rows[i].peak = std::max (rows[i].peak, rows[i].now);
            }
        }
        last = totals;
        lastMs = nowMs;
    }

    void resetPeaks()
    {
        for (auto& r : rows)
            r.peak = 0.0;
    }
};

//==============================================================================
class MainComponent final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::Timer
{
public:
    explicit MainComponent (const juce::StringArray& args)
        : engine ("CollabGodPluginParamsSpike")
    {
        engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
        member = randomMember();
        session = std::make_unique<PluginSession> (engine, member);
        wireSession();

        auto addButton = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> f)
        {
            b.setButtonText (text);
            b.onClick = std::move (f);
            addAndMakeVisible (b);
        };

        addButton (loadButton, "Load VST3...", [this] { choosePlugin(); });
        addButton (windowButton, "Plugin window", [this] { togglePluginWindow(); });
        addButton (playButton, "Play", [this] { setPlaying (! wantPlaying); });
        addButton (hostButton, "Host", [this] { startHosting(); });
        addButton (joinButton, "Join", [this] { joinHost(); });
        addButton (sweepButton, "Host sweep 1 s", [this] { startSweep(); });
        addButton (automationButton, "Add sweep automation", [this] { addAutomation(); });
        addButton (reportButton, "Save report", [this] { saveReport(); });
        addButton (peaksButton, "Reset peaks", [this] { rates.resetPeaks(); });
        addButton (sendStateButton, "Send state now", [this] { session->getState().sendNow (PluginSession::nowMs()); });
        addButton (readStateButton, "Read state now", [this] { session->getState().checkNow (PluginSession::nowMs()); });

        stateButton.setButtonText ("Sync state");
        stateButton.setToggleState (true, juce::dontSendNotification);
        stateButton.onClick = [this] { applyOptions(); };
        addAndMakeVisible (stateButton);

        detectBox.addItemList ({ "Find state changes from hints", "Hints, and poll while the window is open", "Hints, and poll all the time" }, 1);
        detectBox.setSelectedId (2, juce::dontSendNotification);
        detectBox.onChange = [this] { applyOptions(); };
        addAndMakeVisible (detectBox);

        muteButton.setButtonText ("Mute this one");
        muteButton.onClick = [this] { session->getEdit().getMasterVolumePlugin()->setVolumeDb (muteButton.getToggleState() ? -100.0f : 0.0f); };
        addAndMakeVisible (muteButton);

        logButton.setButtonText ("Log events to CSV");
        logButton.onClick = [this] { updateEventLog(); };
        addAndMakeVisible (logButton);

        portEditor.setText ("52799");
        addressEditor.setText ("127.0.0.1:52799");
        filterEditor.setTextToShowWhenEmpty ("Filter parameters", juce::Colours::grey);
        filterEditor.onTextChange = [this] { refilter(); };
        for (auto* e : { &portEditor, &addressEditor, &filterEditor })
            addAndMakeVisible (e);

        throttleBox.addItemList ({ "Throttle off", "Throttle 16 ms", "Throttle 33 ms", "Throttle 50 ms", "Throttle 100 ms" }, 1);
        throttleBox.setSelectedId (3, juce::dontSendNotification);
        throttleBox.onChange = [this] { applyOptions(); };
        echoBox.addItemList ({ "Echo window off", "Echo window 100 ms", "Echo window 250 ms", "Echo window 500 ms" }, 1);
        echoBox.setSelectedId (3, juce::dontSendNotification);
        echoBox.onChange = [this] { applyOptions(); };
        addAndMakeVisible (throttleBox);
        addAndMakeVisible (echoBox);

        statusLabel.setText ("Member " + member + ". Load a plugin, then host or join.", juce::dontSendNotification);
        addAndMakeVisible (statusLabel);

        list.setModel (this);
        list.setRowHeight (24);
        addAndMakeVisible (list);

        statsView.setMultiLine (true);
        statsView.setReadOnly (true);
        statsView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        addAndMakeVisible (statsView);

        logView.setMultiLine (true);
        logView.setReadOnly (true);
        logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        addAndMakeVisible (logView);

        setSize (1100, 800);
        startTimer (250);
        mouseTimer.startTimerHz (60);
        startedMs = PluginSession::nowMs();

        // Optional shortcuts, so two copies can be started from a script:
        //   --load <file.vst3>  --host <port>  --join <address:port>  --play
        for (int i = 0; i < args.size(); ++i)
        {
            const auto next = args[i + 1];
            if (args[i] == "--load")      loadPlugin (juce::File (next));
            else if (args[i] == "--host") { portEditor.setText (next); startHosting(); }
            else if (args[i] == "--join") { addressEditor.setText (next); joinHost(); }
            else if (args[i] == "--play") setPlaying (true);
        }
    }

    ~MainComponent() override
    {
        stopTimer();
        mouseTimer.stopTimer();
        pluginWindow.reset();
        link.close();
        session.reset();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto row = [&r] { auto x = r.removeFromTop (28); r.removeFromTop (6); return x; };

        auto r1 = row();
        loadButton.setBounds (r1.removeFromLeft (110));
        r1.removeFromLeft (6);
        windowButton.setBounds (r1.removeFromLeft (110));
        r1.removeFromLeft (6);
        playButton.setBounds (r1.removeFromLeft (70));
        r1.removeFromLeft (6);
        muteButton.setBounds (r1.removeFromLeft (130));
        statusLabel.setBounds (r1);

        auto r2 = row();
        portEditor.setBounds (r2.removeFromLeft (70));
        r2.removeFromLeft (6);
        hostButton.setBounds (r2.removeFromLeft (70));
        r2.removeFromLeft (20);
        addressEditor.setBounds (r2.removeFromLeft (180));
        r2.removeFromLeft (6);
        joinButton.setBounds (r2.removeFromLeft (70));
        r2.removeFromLeft (20);
        throttleBox.setBounds (r2.removeFromLeft (140));
        r2.removeFromLeft (6);
        echoBox.setBounds (r2.removeFromLeft (160));
        r2.removeFromLeft (6);
        logButton.setBounds (r2.removeFromLeft (150));

        auto r3 = row();
        stateButton.setBounds (r3.removeFromLeft (100));
        r3.removeFromLeft (6);
        detectBox.setBounds (r3.removeFromLeft (300));
        r3.removeFromLeft (6);
        sendStateButton.setBounds (r3.removeFromLeft (120));
        r3.removeFromLeft (6);
        readStateButton.setBounds (r3.removeFromLeft (120));

        auto left = r.removeFromLeft (r.getWidth() / 2);
        filterEditor.setBounds (left.removeFromTop (26));
        left.removeFromTop (4);
        auto buttons = left.removeFromBottom (28);
        left.removeFromBottom (4);
        list.setBounds (left);
        sweepButton.setBounds (buttons.removeFromLeft (130));
        buttons.removeFromLeft (6);
        automationButton.setBounds (buttons.removeFromLeft (170));

        r.removeFromLeft (8);
        auto bottomButtons = r.removeFromBottom (28);
        r.removeFromBottom (4);
        reportButton.setBounds (bottomButtons.removeFromLeft (110));
        bottomButtons.removeFromLeft (6);
        peaksButton.setBounds (bottomButtons.removeFromLeft (110));
        statsView.setBounds (r.removeFromTop (r.getHeight() * 3 / 5));
        r.removeFromTop (4);
        logView.setBounds (r);
    }

private:
    //==========================================================================
    void wireSession()
    {
        session->send = [this] (const juce::String& m) { link.send (m); };
        session->log = [this] (const juce::String& s) { addLog (s); };
        link.onMessage = [this] (const juce::String& m) { session->receive (m); };
        link.onPeersChanged = [this]
        {
            const int n = link.numConnections();
            if (n < connections)
                session->peersLeft();
            if (n > connections)
                session->sendHello();
            connections = n;
        };
    }

    void addLog (const juce::String& s)
    {
        logLines.add (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + s);
        while (logLines.size() > 300)
            logLines.remove (0);
        logView.setText (logLines.joinIntoString ("\n"), false);
        logView.moveCaretToEnd();
    }

    void choosePlugin()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a VST3 plugin", juce::File(), "*.vst3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() != juce::File())
                                      loadPlugin (fc.getResult());
                              });
    }

    void loadPlugin (const juce::File& file)
    {
        pluginWindow.reset();
        setPlaying (false);
        session = std::make_unique<PluginSession> (engine, member);
        wireSession();
        applyOptions();

        juce::String error;
        if (! session->loadPlugin (file, error))
        {
            addLog ("ERROR " + error);
            return;
        }

        if (session->getHello().plugin.instrument)
            session->addHeldChord();
        else
            session->getEdit().getTransport().setLoopRange ({ te::TimePosition(), te::TimePosition::fromSeconds (8.0) });

        session->getEdit().getTransport().looping = true;
        updateEventLog();
        refilter();

        if (connections > 0)
            session->sendHello();

        loadedAtMs = PluginSession::nowMs();
    }

    void togglePluginWindow()
    {
        if (pluginWindow != nullptr)
        {
            pluginWindow.reset();
            return;
        }

        if (auto* instance = session->getInstance())
        {
            auto* editor = instance->createEditorAndMakeActive();

            if (editor == nullptr)
            {
                addLog ("This plugin has no window of its own; use the sliders here.");
                return;
            }

            pluginWindow = std::make_unique<PluginWindow> (editor, session->getHello().plugin.name + " (" + member + ")",
                                                           [this] { pluginWindow.reset(); });
        }
    }

    //==========================================================================
    // Transport. Two lessons from spikes/engine-toolchain: a play pressed before
    // the engine settles is lost (finding 8), and a MIDI device change stops the
    // transport (finding 10). So play waits, and a stop nobody asked for is undone.

    void setPlaying (bool shouldPlay)
    {
        wantPlaying = shouldPlay;
        playButton.setButtonText (wantPlaying ? "Stop" : "Play");

        if (! shouldPlay && session != nullptr)
            session->getEdit().getTransport().stop (false, false);
    }

    void keepPlaying()
    {
        if (session == nullptr || session->getPlugin() == nullptr)
            return;

        auto& transport = session->getEdit().getTransport();
        const bool settled = PluginSession::nowMs() - loadedAtMs > 1500.0;

        if (wantPlaying && settled && ! transport.isPlaying())
        {
            transport.ensureContextAllocated();
            transport.play (false);
        }
    }

    //==========================================================================
    void startHosting()
    {
        const int port = link.host (portEditor.getText().getIntValue());
        addLog (port > 0 ? "Hosting on port " + juce::String (port) + ". Others join this machine's address."
                         : "ERROR could not listen on port " + portEditor.getText());
    }

    void joinHost()
    {
        const auto text = addressEditor.getText().trim();
        const auto address = text.upToLastOccurrenceOf (":", false, false);
        const int port = text.fromLastOccurrenceOf (":", false, false).getIntValue();
        addLog (link.join (address, port) ? "Joined " + text : "ERROR could not connect to " + text);
    }

    void applyOptions()
    {
        static const double throttles[] { 0.0, 16.0, 33.0, 50.0, 100.0 };
        static const double windows[] { 0.0, 100.0, 250.0, 500.0 };
        auto o = session->getSync().options();
        o.flushIntervalMs = throttles[juce::jlimit (0, 4, throttleBox.getSelectedId() - 1)];
        o.echoWindowMs = windows[juce::jlimit (0, 3, echoBox.getSelectedId() - 1)];
        session->getSync().setOptions (o);

        static const spike::Detect detects[] { spike::Detect::hints, spike::Detect::pollWindow, spike::Detect::pollAlways };
        auto& state = session->getState();
        state.enabled = stateButton.getToggleState();
        state.detect = detects[juce::jlimit (0, 2, detectBox.getSelectedId() - 1)];
    }

    /** Watches the mouse over the plugin's window: letting go of it there is a
        sign that something in the plugin changed, whatever the plugin says. The
        plugin's own view is not a JUCE component, so this asks the system for
        the button state and the pointer position instead of receiving events. */
    void watchMouse()
    {
        auto& state = session->getState();
        state.windowOpen = pluginWindow != nullptr;

        if (pluginWindow == nullptr)
            return;

        const bool down = juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown();
        const auto where = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        const bool inside = pluginWindow->getScreenBounds().contains (where);

        // A press that started inside counts until it is let go, wherever the pointer is then.
        if (down && inside)
            pressedInWindow = true;

        state.windowMouse (down && pressedInWindow, PluginSession::nowMs());

        if (! down)
            pressedInWindow = false;
    }

    void updateEventLog()
    {
        if (logButton.getToggleState() && session->getPlugin() != nullptr)
        {
            reportFolder().createDirectory();
            eventLogFile = reportFolder().getChildFile ("events-" + juce::File::createLegalFileName (session->getHello().plugin.name)
                                                        + "-" + member + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".csv");
            session->setEventLog (eventLogFile);
            addLog ("Logging events to " + eventLogFile.getFullPathName());
        }
        else
        {
            session->setEventLog ({});
        }
    }

    //==========================================================================
    // The parameter list

    void refilter()
    {
        visible.clear();
        const auto filter = filterEditor.getText().trim();

        for (size_t i = 0; i < session->getParams().size(); ++i)
            if (filter.isEmpty() || session->getParams()[i].name.containsIgnoreCase (filter))
                visible.push_back (i);

        list.updateContent();
        list.repaint();
    }

    int getNumRows() override { return (int) visible.size(); }

    void paintListBoxItem (int, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected)
            g.fillAll (juce::Colours::darkslateblue.withAlpha (0.5f));
        juce::ignoreUnused (w, h);
    }

    class Row final : public juce::Component
    {
    public:
        explicit Row (MainComponent& o) : owner (o)
        {
            addAndMakeVisible (name);
            addAndMakeVisible (held);
            addAndMakeVisible (slider);
            name.setInterceptsMouseClicks (false, false);
            held.setInterceptsMouseClicks (false, false);
            held.setColour (juce::Label::textColourId, juce::Colours::orange);
            slider.setSliderStyle (juce::Slider::LinearBar);
            slider.setRange (0.0, 1.0);
            slider.setNumDecimalPlacesToDisplay (3);
            slider.onDragStart = [this] { dragging = true; owner.session->hostGestureBegin (id); };
            slider.onValueChange = [this] { if (dragging) owner.session->hostSet (id, (float) slider.getValue()); };
            slider.onDragEnd = [this] { owner.session->hostGestureEnd (id); dragging = false; };
        }

        void set (int rowNumber, const PluginSession::Param& p)
        {
            row = rowNumber;
            id = p.id;
            name.setText (p.name, juce::dontSendNotification);
            refresh();
        }

        void refresh()
        {
            if (auto* p = owner.session->findParam (id))
            {
                if (! dragging)
                    slider.setValue (p->jp->getValue(), juce::dontSendNotification);
                const auto by = owner.session->getSync().heldBy (id.toStdString());
                held.setText (by.empty() ? juce::String() : "held by " + juce::String (by), juce::dontSendNotification);
            }
        }

        void mouseDown (const juce::MouseEvent&) override { owner.list.selectRow (row); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 1);
            name.setBounds (r.removeFromLeft (r.getWidth() * 2 / 5));
            held.setBounds (r.removeFromRight (110));
            slider.setBounds (r);
        }

        juce::String id;

    private:
        MainComponent& owner;
        juce::Label name, held;
        juce::Slider slider;
        int row = -1;
        bool dragging = false;
    };

    juce::Component* refreshComponentForRow (int rowNumber, bool, juce::Component* existing) override
    {
        if (rowNumber < 0 || rowNumber >= (int) visible.size())
        {
            delete existing;
            return nullptr;
        }

        auto* r = dynamic_cast<Row*> (existing);
        if (r == nullptr)
        {
            delete existing;
            r = new Row (*this);
        }

        r->set (rowNumber, session->getParams()[visible[(size_t) rowNumber]]);
        return r;
    }

    const PluginSession::Param* selectedParam() const
    {
        const int row = list.getSelectedRow();
        return row >= 0 && row < (int) visible.size() ? &session->getParams()[visible[(size_t) row]] : nullptr;
    }

    //==========================================================================
    // Measurements

    void startSweep()
    {
        auto* p = selectedParam();
        if (p == nullptr)
        {
            addLog ("Select a parameter first.");
            return;
        }

        sweepId = p->id;
        sweepStep = 0;
        sweepFrom = p->jp->getValue();
        sweepTo = sweepFrom < 0.5f ? 0.95f : 0.05f;
        rates.resetPeaks();
        session->hostGestureBegin (sweepId);
        sweepTimer.startTimer (5);
        addLog ("Sweeping " + p->name + " from the host: 200 values over about a second.");
    }

    void sweepTick()
    {
        ++sweepStep;
        session->hostSet (sweepId, sweepFrom + (sweepTo - sweepFrom) * (float) sweepStep / 200.0f);

        if (sweepStep >= 200)
        {
            sweepTimer.stopTimer();
            session->hostGestureEnd (sweepId);
        }
    }

    void addAutomation()
    {
        if (auto* p = selectedParam())
        {
            const double seconds = session->getEdit().getTransport().getLoopRange().getLength().inSeconds();
            session->addSweepAutomation (p->id, seconds > 0.0 ? seconds : 8.0);
            addLog ("Automation added to " + p->name + ". Add the same on the other side: automation is never sent, each side plays its own.");
        }
    }

    void timerCallback() override
    {
        keepPlaying();

        const auto s = session->getStats();
        const auto& y = session->getSync().stats();
        rates.update ({ s.rawValueEvents, s.trackedEvents, y.sentChanges, y.sentBatches, session->bytesSent(), y.received },
                      PluginSession::nowMs());

        for (int i = 0; i < list.getNumRowsOnScreen() + 1; ++i)
            if (auto* r = dynamic_cast<Row*> (list.getComponentForRowNumber (list.getRowContainingPosition (0, 0) + i)))
                r->refresh();

        statusLabel.setText ("Member " + member + ", " + juce::String (connections) + " connection(s), "
                             + juce::String ((int) session->getPeers().size()) + " other member(s)"
                             + (session->getEdit().getTransport().isPlaying() ? ", playing" : ""),
                             juce::dontSendNotification);
        statsView.setText (statsText(), false);
    }

    juce::String statsText() const
    {
        const auto& h = session->getHello();
        const auto s = session->getStats();
        const auto& y = session->getSync().stats();
        juce::String t;

        if (session->getPlugin() == nullptr)
            return "No plugin loaded.";

        t << h.plugin.name << " " << h.plugin.version << " by " << h.plugin.vendor << " (" << h.plugin.uid << ")\n"
          << (int) h.params.size() << " parameters synced, of " << session->getInstance()->getParameters().size()
          << " the plugin reports\n\n";

        t << "                         now     peak\n";
        for (auto& r : rates.rows)
            t << juce::String (r.name).paddedRight (' ', 22) << juce::String (r.now, 0).paddedLeft (' ', 8)
              << juce::String (r.peak, 0).paddedLeft (' ', 9) << "\n";

        auto line = [&t] (const char* name, std::uint64_t v) { t << juce::String (name).paddedRight (' ', 38) << (juce::int64) v << "\n"; };
        t << "\n";
        line ("plugin reports, total", s.rawValueEvents);
        line ("  of them off the message thread", s.rawValueEventsOffMessageThread);
        line ("gesture reports", s.rawGestureEvents);
        line ("reached the sync", s.trackedEvents);
        line ("  from this app's sliders", s.fromHostControls);
        line ("  from the plugin", s.fromPlugin);
        line ("ignored: automation playing", s.automationIgnored);
        line ("not sent: same value", y.duplicates);
        line ("not sent: echo of a remote value", y.echoes);
        line ("not sent: late echo, newest re-set", y.echoesStale);
        line ("not sent: someone else holds it", y.echoesAltered);
        line ("not sent: replaced before sending", y.coalesced);
        line ("sent changes", y.sentChanges);
        line ("sent messages", y.sentBatches);
        line ("received changes", y.received);
        line ("applied to this plugin", y.applied);
        line ("ignored: older than what is here", y.stale);
        line ("ignored: held here", y.heldDuringGesture);
        line ("ignored: unknown parameter ID", s.unknownRemoteParams);

        for (const auto& [m, lines] : session->getMismatches())
            if (! lines.isEmpty())
                t << "\nParameter lists differ from " << m << ":\n  " << lines.joinIntoString ("\n  ") << "\n";

        t << "\n" << stateText();
        return t;
    }

    /** The plugin's saved state (#13). */
    juce::String stateText() const
    {
        auto& state = session->getState();
        const auto& s = state.getStats();
        const auto& y = state.getSync().stats();
        const auto& last = state.lastRead();
        juce::String t;
        auto line = [&t] (const char* name, const juce::String& v) { t << juce::String (name).paddedRight (' ', 38) << v << "\n"; };
        auto n = [] (std::uint64_t v) { return juce::String ((juce::int64) v); };
        auto kib = [] (std::uint64_t v) { return juce::String ((double) v / 1024.0, 1) + " KiB"; };

        t << "State: " << (state.enabled ? "synced" : "not synced") << ", " << detectBox.getText().toLowerCase()
          << (state.getSync().isNoisy() ? "; NOISY: it changes with nothing done, so it is not polled" : "") << "\n";
        line ("size now", kib (last.bytes.getSize()) + " (component " + kib (last.componentBytes) + ", controller " + kib (last.controllerBytes) + ")");
        line ("reads", n (y.reads) + ", " + juce::String (y.reads > 0 ? y.readMsTotal / (double) y.reads : 0.0, 2) + " ms on average, "
                      + juce::String (y.readMsMax, 2) + " ms at most; polling every " + juce::String (state.getSync().pollIntervalMs(), 0) + " ms");
        line ("hints: setDirty", n (s.hintSetDirty));
        line ("hints: restart / program", n (s.hintRestart));
        line ("hints: unchanged parameter reports", n (s.hintSameValues));
        line ("hints: mouse let go in the window", n (s.hintWindow));
        line ("changes found here", n (y.localChanges) + " (" + n (y.unprompted) + " with no hint or activity)");
        line ("taken as settling after a load", n (y.absorbedSettling));
        line ("taken as others' parameters", n (y.absorbedRemoteParams));
        line ("announced", n (s.announcesSent) + " (" + n (y.forced) + " early, after someone else's)");
        line ("received / older, ignored", n (y.received) + " / " + n (y.stale));
        line ("already had it (same hash)", n (y.matched));
        line ("fetched / applied", n (y.fetches) + " / " + n (s.applies) + ", applying took "
                                   + juce::String (s.applies > 0 ? s.applyMsTotal / (double) s.applies : 0.0, 1) + " ms on average, "
                                   + juce::String (s.applyMsMax, 1) + " at most");
        line ("last transfer", kib (s.lastRawSize) + ", " + kib (s.lastCompressedSize) + " compressed, " + juce::String (s.lastTransferMs, 0) + " ms");
        line ("state data sent / received", kib (s.dataBytesSent) + " / " + kib (s.dataBytesReceived));
        line ("parameters it changed silently", n (y.paramsDiscovered));
        line ("parameters put back after a load", n (y.paramsPutBack));
        line ("files named but not on this machine", n (s.pathsMissingHere));

        for (const auto& [outcome, count] : s.outcomes)
            line (("applied: " + outcome).toRawUTF8(), n (count));

        return t;
    }

    void saveReport()
    {
        if (session->getPlugin() == nullptr)
            return;

        reportFolder().createDirectory();
        const auto& h = session->getHello();
        const auto file = reportFolder().getChildFile ("report-" + juce::File::createLegalFileName (h.plugin.name) + "-" + member + "-"
                                                       + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".md");
        juce::String t;
        t << "## " << h.plugin.name << " " << h.plugin.version << " (" << juce::SystemStats::getOperatingSystemName() << ")\n\n"
          << "Throttle: " << throttleBox.getText() << ". " << echoBox.getText() << ". Members connected: "
          << (int) session->getPeers().size() << ".\n\n```\n" << statsText() << "```\n\n"
          << "Parameter IDs (first 40):\n\n```\n";

        for (size_t i = 0; i < std::min<size_t> (40, h.params.size()); ++i)
            t << h.params[i].index << "  " << h.params[i].id << "  " << h.params[i].name << "\n";

        t << "```\n";

        const auto history = session->getState().history();
        if (! history.isEmpty())
            t << "\nState events:\n\n```\n" << history.joinIntoString ("\n") << "\n```\n";

        if (eventLogFile.existsAsFile())
            t << "\nEvent log: " << eventLogFile.getFileName() << "\n";

        file.replaceWithText (t);
        addLog ("Report saved to " + file.getFullPathName());
        juce::SystemClipboard::copyTextToClipboard (t);
        addLog ("... and copied to the clipboard.");
    }

    //==========================================================================
    struct SweepTimer final : juce::Timer
    {
        explicit SweepTimer (MainComponent& o) : owner (o) {}
        void timerCallback() override { owner.sweepTick(); }
        MainComponent& owner;
    };

    struct MouseTimer final : juce::Timer
    {
        explicit MouseTimer (MainComponent& o) : owner (o) {}
        void timerCallback() override { owner.watchMouse(); }
        MainComponent& owner;
    };

    te::Engine engine;
    juce::String member;
    std::unique_ptr<PluginSession> session;
    spike::Link link;
    int connections = 0;

    juce::TextButton loadButton, windowButton, playButton, hostButton, joinButton, sweepButton, automationButton,
                     reportButton, peaksButton, sendStateButton, readStateButton;
    juce::ToggleButton muteButton, logButton, stateButton;
    juce::ComboBox detectBox;
    bool pressedInWindow = false;
    juce::TextEditor portEditor, addressEditor, filterEditor, statsView, logView;
    juce::ComboBox throttleBox, echoBox;
    juce::Label statusLabel;
    juce::ListBox list;
    std::vector<size_t> visible;
    juce::StringArray logLines;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<PluginWindow> pluginWindow;
    juce::File eventLogFile;

    bool wantPlaying = false;
    double startedMs = 0.0, loadedAtMs = 0.0;

    Rates rates;
    SweepTimer sweepTimer { *this };
    MouseTimer mouseTimer { *this };
    juce::String sweepId;
    int sweepStep = 0;
    float sweepFrom = 0.0f, sweepTo = 0.0f;
};

//==============================================================================
class App final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "CollabGod Plugin Params Spike"; }
    const juce::String getApplicationVersion() override { return "0.0.1"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();

        if (args.contains ("--probe-state"))
        {
            setApplicationReturnValue (spike::runStateProbe (args));
            quit();
            return;
        }

        if (args.contains ("--self-test-state"))
        {
            setApplicationReturnValue (spike::runStateSelfTest (args));
            quit();
            return;
        }

        if (args.contains ("--self-test"))
        {
            // Runs to the end here, pumping the message loop itself, then quits.
            setApplicationReturnValue (spike::runSelfTest (args));
            quit();
            return;
        }

        window = std::make_unique<Window> (args);
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    struct Window final : juce::DocumentWindow
    {
        explicit Window (const juce::StringArray& args)
            : DocumentWindow ("CollabGod Plugin Params Spike", juce::Colours::black, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (args), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<Window> window;
};
} // namespace

START_JUCE_APPLICATION (App)
