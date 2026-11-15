// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.

#include "Wire.h"

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
        out << "[" << juce::JSON::toString (juce::String (c.param)) << ","
            << juce::String (c.value, 7) << ","
            << juce::String ((juce::int64) c.stamp.counter) << ","
            << juce::JSON::toString (juce::String (c.stamp.member)) << ","
            << (int) c.flags << "]";
    }

    out << "]}";
    return out;
}

juce::String decode (const juce::String& json, Hello& hello, Batch& batch)
{
    const auto v = juce::JSON::parse (json);
    const auto type = v["t"].toString();

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
