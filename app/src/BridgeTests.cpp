// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's contract tests (`CollabGod --bridge-test`, run by ctest). No window and no
// web view: they drive the generated codec and dispatch, and the real BridgeHost, with
// every example in the contract. A message added to the contract is covered here
// without a change to this file. The page's side is packages/bridge/test, and
// `--bridge-check` runs both sides through the real web view.

#include "BridgeTests.h"

#include "BridgeContractExamples.h"
#include "BridgeHost.h"
#include "Log.h"
#include "UiFiles.h"

#include <iostream>
#include <limits>

namespace collabgod
{
    namespace
    {
        namespace ex = bridge::examples;

        struct Results
        {
            int passed = 0;
            int failed = 0;

            bool expect (bool condition, const juce::String& what)
            {
                if (condition)
                    ++passed;
                else
                {
                    ++failed;
                    std::cout << "FAIL " << what << std::endl;
                }
                return condition;
            }
        };

        juce::var parse (const char* json)
        {
            juce::var v;
            const auto result = juce::JSON::parse (juce::String::fromUTF8 (json), v);
            jassert (result.wasOk());
            return v;
        }

        /** A copy of an object with its first field removed, another added, or the first field's
            value replaced by an array, which no field in the contract accepts as itself. */
        enum class Mutation { removeField, addField, wrongType };

        juce::var mutate (const juce::var& v, Mutation how)
        {
            auto copy = juce::JSON::parse (juce::JSON::toString (v));
            auto* object = copy.getDynamicObject();
            if (object == nullptr)
                return {};

            const auto& properties = object->getProperties();

            switch (how)
            {
                case Mutation::removeField:
                    if (properties.size() == 0)
                        return {};
                    object->removeProperty (properties.getName (0));
                    break;
                case Mutation::addField:
                    object->setProperty ("notInTheContract", 1);
                    break;
                case Mutation::wrongType:
                {
                    if (properties.size() == 0)
                        return {};
                    const auto name = properties.getName (0);
                    const auto value = properties.getValueAt (0);
                    // An array of the value is wrong for every kind of field except an array;
                    // for an array, an object is wrong.
                    juce::var replacement = value.isArray() ? juce::var (new juce::DynamicObject())
                                                            : juce::var (juce::Array<juce::var> { value });
                    object->setProperty (name, replacement);
                    break;
                }
            }

            return copy;
        }

        const ex::Example* findExample (const juce::String& kind, const juce::String& name, const juce::String& role)
        {
            for (const auto& e : ex::all)
                if (kind == e.kind && name == e.name && role == e.role)
                    return &e;
            return nullptr;
        }

        void testExamples (Results& r)
        {
            for (const auto& e : ex::all)
            {
                const auto label = juce::String (e.kind) + " " + e.name + " " + e.role + " (" + e.type + ")";
                const auto in = parse (e.json);

                juce::var out;
                juce::String error;
                const bool decoded = ex::roundTrip (e.type, in, out, error);
                r.expect (decoded, label + " decodes: " + error);
                r.expect (decoded && bridge::sameJson (in, out), label + " encodes back to the same JSON: "
                                                                      + juce::JSON::toString (out, true));

                for (const auto how : { Mutation::removeField, Mutation::addField, Mutation::wrongType })
                {
                    const auto bad = mutate (in, how);
                    if (bad.isVoid())
                        continue;

                    juce::var ignored;
                    juce::String why;
                    const bool refused = ! ex::roundTrip (e.type, bad, ignored, why);
                    r.expect (refused && why.startsWith (e.type), label + " refuses " + juce::JSON::toString (bad, true)
                                                                      + (refused ? ", but says: " + why : juce::String()));
                }
            }
        }

        /** Every function, called through dispatch on a real BridgeHost with the contract's example. */
        void testDispatch (Results& r)
        {
            BridgeHost host ({});

            for (const auto* name : bridge::functionNames)
            {
                const auto* params = findExample ("function", name, "params");
                const auto* result = findExample ("function", name, "result");

                juce::Array<juce::var> args;
                if (params != nullptr)
                    args.add (parse (params->json));

                const auto answer = bridge::dispatch (host, name, args);
                const bool ok = r.expect (static_cast<bool> (answer["ok"]),
                                          juce::String (name) + " answers ok: " + juce::JSON::toString (answer, true));

                if (! ok)
                    continue;

                if (result == nullptr)
                {
                    r.expect (! answer.hasProperty ("result"), juce::String (name) + " returns nothing");
                    continue;
                }

                juce::var out;
                juce::String error;
                r.expect (ex::roundTrip (result->type, answer["result"], out, error),
                          juce::String (name) + "'s answer matches the contract: " + error);

                // Wrong arguments are refused with a reason, and the handler is not called.
                if (params != nullptr)
                {
                    const auto refused = bridge::dispatch (host, name, { mutate (parse (params->json), Mutation::removeField) });
                    r.expect (! static_cast<bool> (refused["ok"]) && refused["error"].toString().contains ("is missing"),
                              juce::String (name) + " refuses arguments with a field missing: " + juce::JSON::toString (refused, true));

                    const auto none = bridge::dispatch (host, name, {});
                    r.expect (! static_cast<bool> (none["ok"]), juce::String (name) + " refuses no arguments");
                }
                else
                {
                    const auto extra = bridge::dispatch (host, name, { 1 });
                    r.expect (! static_cast<bool> (extra["ok"]), juce::String (name) + " refuses an argument it does not take");
                }
            }

            const auto unknown = bridge::dispatch (host, "noSuchFunction", {});
            r.expect (! static_cast<bool> (unknown["ok"]) && unknown["error"].toString().contains ("noSuchFunction"),
                      "an unknown function is refused by name");

            // Every function and every command reached the host, so none is left out of dispatch.
            juce::Array<juce::var> batch;
            for (const auto* name : bridge::commandNames)
            {
                const auto* args = findExample ("command", name, "args");
                r.expect (args != nullptr, juce::String (name) + " has an example");
                if (args == nullptr)
                    continue;
                auto* command = new juce::DynamicObject();
                command->setProperty ("type", name);
                command->setProperty ("args", parse (args->json));
                batch.add (juce::var (command));
            }

            auto* params = new juce::DynamicObject();
            params->setProperty ("commands", batch);
            const auto answer = bridge::dispatch (host, bridge::commandBatchFunction, { juce::var (params) });
            r.expect (static_cast<bool> (answer["ok"]), "a batch of every command is carried out: " + juce::JSON::toString (answer, true));
            r.expect (host.unused().isEmpty(), "every function and command reached the host; not: " + host.unused().joinIntoString (", "));
        }

        void testCommands (Results& r)
        {
            BridgeHost host ({});

            const auto batch = [] (const juce::String& json) { return juce::Array<juce::var> { juce::JSON::parse (json) }; };

            auto answer = bridge::dispatch (host, "commands", batch (R"({"commands":[{"type":"probe","args":{"id":1}},{"type":"probe","args":{"id":2}}]})"));
            r.expect (static_cast<bool> (answer["ok"]), "a batch of two probes is accepted");
            r.expect (host.buildFrame().lastProbe == 2, "commands run in the order they were sent");

            answer = bridge::dispatch (host, "commands", batch (R"({"commands":[{"type":"probe","args":{"id":3}},{"type":"probe","args":{"id":"four"}}]})"));
            r.expect (! static_cast<bool> (answer["ok"]) && answer["error"].toString().contains ("commands[1].args.id"),
                      "a batch with a bad command is refused, naming it: " + answer["error"].toString());
            r.expect (host.buildFrame().lastProbe == 2, "a refused batch runs none of its commands");

            answer = bridge::dispatch (host, "commands", batch (R"({"commands":[{"type":"explode","args":{}}]})"));
            r.expect (! static_cast<bool> (answer["ok"]) && answer["error"].toString().contains ("commands[0].type"),
                      "an unknown command is refused: " + answer["error"].toString());

            answer = bridge::dispatch (host, "commands", batch (R"({"commands":[]})"));
            r.expect (static_cast<bool> (answer["ok"]), "an empty batch is accepted");

            const auto first = host.buildFrame();
            const auto second = host.buildFrame();
            r.expect (second.seq == first.seq + 1 && second.hostTimeMs >= first.hostTimeMs, "frames count up and time moves forward");
        }

        void testHost (Results& r)
        {
            HostSettings settings;
            settings.development = true;
            settings.selfCheck = true;
            BridgeHost host (settings);

            r.expect (! host.shouldPush(), "nothing is pushed before the page says hello");
            const auto info = host.hello();
            r.expect (info.contractVersion == bridge::contractVersion && info.development && info.selfCheck,
                      "hello describes the app");
            r.expect (info.frameDelivery == bridge::FrameDelivery::pull && ! host.shouldPush(), "pull is the default");

            host.setFrameDelivery ({ bridge::FrameDelivery::push });
            r.expect (host.shouldPush(), "push once the page asks for it");
            host.pageUnloaded();
            r.expect (! host.shouldPush(), "nothing is pushed while the page reloads");

            bool reported = false;
            bool passed = true;
            host.onSelfCheckDone = [&] (bool p) { reported = true; passed = p; };
            host.selfCheckDone ({ true, 1, {}, {} });
            r.expect (reported && ! passed, "the self-check fails on the app's side when the page has not used every message");
        }

        void testCodec (Results& r)
        {
            juce::int64 i = 0;
            juce::String error;
            r.expect (bridge::read (juce::var (3.0), i, "x", error) && i == 3, "a whole number may arrive as 3.0");
            r.expect (! bridge::read (juce::var (3.5), i, "x", error), "3.5 is not a whole number");
            r.expect (! bridge::read (juce::var ((juce::int64) 1 << 60), i, "x", error), "2^60 is outside JavaScript's safe range");

            double d = 0;
            r.expect (! bridge::read (juce::var (std::numeric_limits<double>::quiet_NaN()), d, "x", error) && error == "x is the number nan, not a finite number",
                      "NaN is refused: " + error);
            r.expect (bridge::write (std::numeric_limits<double>::infinity()).isVoid(), "infinity is written as null, which the page refuses");

            bridge::Frame frame;
            r.expect (! bridge::read (juce::JSON::parse (R"({"seq":1,"hostTimeMs":2,"lastProbe":-1,"extra":0})"), frame, "frame", error)
                          && error == "frame.extra is not a field of Frame",
                      "an unknown field is refused by name: " + error);

            r.expect (bridge::sameJson (juce::JSON::parse (R"({"a":[1,2.5],"b":{"c":true}})"), juce::JSON::parse (R"({"b":{"c":true},"a":[1.0,2.5]})")),
                      "sameJson ignores field order and how a number is written");
            r.expect (! bridge::sameJson (juce::JSON::parse (R"({"a":1})"), juce::JSON::parse (R"({"a":"1"})")), "sameJson tells 1 from \"1\"");
        }

        void testUiFiles (Results& r)
        {
            const auto folder = juce::File::createTempFile ("collabgod-ui");
            folder.createDirectory();
            folder.getChildFile ("index.html").replaceWithText ("<!doctype html>");
            folder.getChildFile ("app.js").replaceWithText ("1");
            folder.getSiblingFile ("secret.txt").replaceWithText ("no");

            r.expect (serveUiFile (folder, "/").has_value() && serveUiFile (folder, "/")->mimeType == "text/html", "/ serves index.html");
            r.expect (serveUiFile (folder, "/app.js?v=2").has_value(), "a query string is ignored");
            r.expect (! serveUiFile (folder, "/../secret.txt").has_value(), "nothing outside the folder");
            r.expect (! serveUiFile (folder, "/missing.js").has_value(), "nothing that is missing");

            FolderWatcher watcher (folder);
            r.expect (! watcher.poll(), "an unchanged folder is not a change");
            folder.getChildFile ("app.js").replaceWithText ("22");
            r.expect (! watcher.poll(), "a change is not reported while it may still be under way");
            r.expect (watcher.poll(), "a change is reported once the folder has stayed the same");
            r.expect (! watcher.poll(), "and only once");

            folder.getSiblingFile ("secret.txt").deleteFile();
            folder.deleteRecursively();
        }
    }

    int runBridgeTests()
    {
        // The host logs what the page sends; the tests print their own results.
        const ScopedSilentLog silent;
        Results r;
        testExamples (r);
        testDispatch (r);
        testCommands (r);
        testHost (r);
        testCodec (r);
        testUiFiles (r);

        std::cout << r.passed << " passed, " << r.failed << " failed, across " << ex::all.size() << " examples of "
                  << bridge::functionNames.size() << " functions, " << bridge::eventNames.size() << " events and "
                  << bridge::commandNames.size() << " commands" << std::endl;
        return r.failed == 0 ? 0 : 1;
    }
}
