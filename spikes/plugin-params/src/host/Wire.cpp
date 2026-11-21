// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.

#include "Wire.h"

#include <cstdio>

namespace spike
{

juce::String encodeHello (const Hello& h)
{
    auto plugin = std::make_unique<juce::DynamicObject>();
    plugin->setProperty ("name", h.plugin.name);
    plugin->setProperty ("vendor", h.plugin.vendor);
    plugin->setProperty ("version", h.plugin.version);
    plugin->setProperty ("format", h.plugin.format);
    plugin->setProperty ("uid", h.plugin.uid);
    plugin->setProperty ("instrument", h.plugin.instrument);

    juce::Array<juce::var> params;

    for (const auto& p : h.params)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("id", p.id);
        o->setProperty ("name", p.name);
        o->setProperty ("index", p.index);
        o->setProperty ("steps", p.steps);
        params.add (juce::var (o.release()));
    }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("t", "hello");
    root->setProperty ("member", h.member);
    root->setProperty ("plugin", juce::var (plugin.release()));
    root->setProperty ("params", params);
    return juce::JSON::toString (juce::var (root.release()), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none));
}

// A batch is kept small because it is sent up to 30 times a second per parameter:
// {"t":"batch","from":"m-1","c":[["1234",0.5,17,"m-1",0],...]}
juce::String encodeBatch (const Batch& b)
{
    juce::String out;
    out.preallocateBytes (32 + b.changes.size() * 48);
    out << "{\"t\":\"batch\",\"from\":" << juce::JSON::toString (b.from) << ",\"c\":[";

    for (size_t i = 0; i < b.changes.size(); ++i)
    {
        const auto& c = b.changes[i];
        if (i > 0)
            out << ",";
        // Nine significant digits bring a float back exactly. Fewer (it was seven decimal
        // places) left a turned knob a hair off on the receiver, so its saved state no
        // longer hashed the same as the sender's and every knob turn loaded a state.
        char value[32];
        std::snprintf (value, sizeof (value), "%.9g", (double) c.value);
        out << "[" << juce::JSON::toString (juce::String (c.param)) << ","
            << value << ","
            << juce::String ((juce::int64) c.stamp.counter) << ","
            << juce::JSON::toString (juce::String (c.stamp.member)) << ","
            << (int) c.flags << "]";
    }

    out << "]}";
    return out;
}

juce::String encodeState (const StateMessage& m)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("t", m.type);
    root->setProperty ("from", m.from);

    if (m.to.isNotEmpty())
        root->setProperty ("to", m.to);

    if (m.type == "state")
    {
        root->setProperty ("h", juce::String (m.ref.hash));
        root->setProperty ("n", (juce::int64) m.ref.size);
        root->setProperty ("c", (juce::int64) m.ref.stamp.counter);
        root->setProperty ("m", juce::String (m.ref.stamp.member));
    }
    else
    {
        root->setProperty ("h", m.hash);
    }

    if (m.type == "stateData")
    {
        root->setProperty ("i", m.chunk);
        root->setProperty ("of", m.chunks);
        root->setProperty ("z", (juce::int64) m.compressedSize);
        root->setProperty ("d", juce::Base64::toBase64 (m.data.getData(), m.data.getSize()));

        if (! m.params.empty())
        {
            juce::Array<juce::var> params;
            for (const auto& [id, vs] : m.params)
                params.add (juce::Array<juce::var> { juce::String (id), (double) vs.first,
                                                     (juce::int64) vs.second.counter, juce::String (vs.second.member) });
            root->setProperty ("p", params);
        }
    }

    return juce::JSON::toString (juce::var (root.release()), juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none));
}

juce::String encodePresence (const Presence& p)
{
    juce::String out;
    out << "{\"t\":\"presence\",\"from\":" << juce::JSON::toString (p.from)
        << ",\"open\":" << (p.windowOpen ? "true" : "false")
        << ",\"press\":" << (p.pressing ? "true" : "false") << "}";
    return out;
}

juce::String decode (const juce::String& json, Hello& hello, Batch& batch, StateMessage& state, Presence& presence)
{
    const auto v = juce::JSON::parse (json);
    const auto type = v["t"].toString();

    if (type == "presence")
    {
        presence = {};
        presence.from = v["from"].toString();
        presence.windowOpen = (bool) v["open"];
        presence.pressing = (bool) v["press"];
        return type;
    }

    if (type.startsWith ("state"))
    {
        state = {};
        state.type = type;
        state.from = v["from"].toString();
        state.to = v["to"].toString();
        state.hash = v["h"].toString();

        if (type == "state")
        {
            state.ref.hash = state.hash.toStdString();
            state.ref.size = (std::uint64_t) (juce::int64) v["n"];
            state.ref.stamp.counter = (std::uint64_t) (juce::int64) v["c"];
            state.ref.stamp.member = v["m"].toString().toStdString();
        }

        if (type == "stateData")
        {
            state.chunk = (int) v["i"];
            state.chunks = (int) v["of"];
            state.compressedSize = (std::uint64_t) (juce::int64) v["z"];
            juce::MemoryOutputStream out (state.data, false);
            juce::Base64::convertFromBase64 (out, v["d"].toString());

            if (auto* arr = v["p"].getArray())
                for (const auto& e : *arr)
                    if (e.isArray() && e.size() >= 4)
                        state.params[e[0].toString().toStdString()] = { (float) (double) e[1],
                                                                         psync::Stamp { (std::uint64_t) (juce::int64) e[2], e[3].toString().toStdString() } };
        }

        return type;
    }

    if (type == "hello")
    {
        hello = {};
        hello.member = v["member"].toString();
        const auto& p = v["plugin"];
        hello.plugin.name = p["name"].toString();
        hello.plugin.vendor = p["vendor"].toString();
        hello.plugin.version = p["version"].toString();
        hello.plugin.format = p["format"].toString();
        hello.plugin.uid = p["uid"].toString();
        hello.plugin.instrument = (bool) p["instrument"];

        if (auto* arr = v["params"].getArray())
            for (const auto& o : *arr)
                hello.params.push_back ({ o["id"].toString(), o["name"].toString(), (int) o["index"],
                                          (int) o["steps"] >= 2, (int) o["steps"] });
        return type;
    }

    if (type == "batch")
    {
        batch = {};
        batch.from = v["from"].toString();

        if (auto* arr = v["c"].getArray())
        {
            for (const auto& c : *arr)
            {
                if (! c.isArray() || c.size() < 5)
                    continue;

                psync::Change change;
                change.param = c[0].toString().toStdString();
                change.value = (float) (double) c[1];
                change.stamp.counter = (std::uint64_t) (juce::int64) c[2];
                change.stamp.member = c[3].toString().toStdString();
                change.flags = (std::uint8_t) (int) c[4];
                batch.changes.push_back (std::move (change));
            }
        }
        return type;
    }

    return {};
}

juce::StringArray compareParams (const Hello& mine, const Hello& theirs)
{
    juce::StringArray lines;
    std::map<juce::String, const ParamInfo*> byId;

    for (const auto& p : theirs.params)
        byId[p.id] = &p;

    if (mine.plugin.uid != theirs.plugin.uid)
        lines.add ("different plugin: " + mine.plugin.name + " (" + mine.plugin.uid + ") here, "
                   + theirs.plugin.name + " (" + theirs.plugin.uid + ") for " + theirs.member);

    if (mine.plugin.version != theirs.plugin.version)
        lines.add ("different version: " + mine.plugin.version + " here, " + theirs.plugin.version + " for " + theirs.member);

    if (mine.params.size() != theirs.params.size())
        lines.add ("different parameter count: " + juce::String ((int) mine.params.size()) + " here, "
                   + juce::String ((int) theirs.params.size()) + " for " + theirs.member);

    int movedIndex = 0;

    for (const auto& p : mine.params)
    {
        auto it = byId.find (p.id);

        if (it == byId.end())
        {
            lines.add ("parameter " + p.id + " \"" + p.name + "\" only here");
            continue;
        }

        const auto& q = *it->second;

        if (q.name != p.name)
            lines.add ("parameter " + p.id + " is \"" + p.name + "\" here, \"" + q.name + "\" for " + theirs.member);

        if (q.steps != p.steps)
            lines.add ("parameter " + p.id + " \"" + p.name + "\" has " + juce::String (p.steps) + " steps here, "
                       + juce::String (q.steps) + " for " + theirs.member);

        if (q.index != p.index)
            ++movedIndex;

        byId.erase (it);
    }

    for (const auto& [id, q] : byId)
        lines.add ("parameter " + id + " \"" + q->name + "\" only for " + theirs.member);

    if (movedIndex > 0)
        lines.add (juce::String (movedIndex) + " shared parameters are at a different index (harmless: they are matched by ID)");

    return lines;
}

} // namespace spike
