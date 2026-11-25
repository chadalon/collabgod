// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hand-written half of the bridge codec: reading and writing the values JSON
// carries, and the envelope every answer to the page travels in. The generated
// BridgeContract.h (from packages/bridge) builds every message's codec from these.
//
// The rules match packages/bridge/src/schema.ts: every field is required, unknown
// fields are refused, numbers are finite and integers are whole and within
// JavaScript's safe range.

#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace collabgod::bridge
{
    /** Sets `error` to "<path> <problem>" and returns false. */
    inline bool fail (juce::String& error, const juce::String& path, const juce::String& problem)
    {
        error = path + " " + problem;
        return false;
    }

    /** What kind of JSON value this is, for an error message. */
    inline juce::String describe (const juce::var& v)
    {
        if (v.isVoid() || v.isUndefined()) return "nothing";
        if (v.isBool()) return "a boolean";
        if (v.isInt() || v.isInt64() || v.isDouble()) return "the number " + v.toString();
        if (v.isString()) return "a string";
        if (v.isArray()) return "an array";
        if (v.isObject()) return "an object";
        return "an unexpected value";
    }

    inline bool read (const juce::var& v, bool& out, const juce::String& path, juce::String& error)
    {
        if (! v.isBool())
            return fail (error, path, "is " + describe (v) + ", not a boolean");

        out = static_cast<bool> (v);
        return true;
    }

    inline bool read (const juce::var& v, double& out, const juce::String& path, juce::String& error)
    {
        if (! (v.isInt() || v.isInt64() || v.isDouble()) || ! std::isfinite (static_cast<double> (v)))
            return fail (error, path, "is " + describe (v) + ", not a finite number");

        out = static_cast<double> (v);
        return true;
    }

    inline constexpr double maxSafeInteger = 9007199254740991.0; // Number.MAX_SAFE_INTEGER

    inline bool read (const juce::var& v, juce::int64& out, const juce::String& path, juce::String& error)
    {
        if (v.isInt() || v.isInt64())
        {
            const auto i = static_cast<juce::int64> (v);
            if (std::abs (static_cast<double> (i)) <= maxSafeInteger)
            {
                out = i;
                return true;
            }
        }
        else if (v.isDouble())
        {
            // JSON has one kind of number: 3 may arrive as 3.0.
            const auto d = static_cast<double> (v);
            if (std::isfinite (d) && juce::exactlyEqual (std::floor (d), d) && std::abs (d) <= maxSafeInteger)
            {
                out = static_cast<juce::int64> (d);
                return true;
            }
        }

        return fail (error, path, "is " + describe (v) + ", not a whole number");
    }

    inline bool read (const juce::var& v, juce::String& out, const juce::String& path, juce::String& error)
    {
        if (! v.isString())
            return fail (error, path, "is " + describe (v) + ", not a string");

        out = v.toString();
        return true;
    }

    template <typename T>
    bool read (const juce::var& v, std::vector<T>& out, const juce::String& path, juce::String& error)
    {
        const auto* items = v.getArray();
        if (items == nullptr)
            return fail (error, path, "is " + describe (v) + ", not an array");

        std::vector<T> result;
        result.reserve (static_cast<size_t> (items->size()));

        for (int i = 0; i < items->size(); ++i)
        {
            T item {};
            if (! read (items->getReference (i), item, path + "[" + juce::String (i) + "]", error))
                return false;
            result.push_back (std::move (item));
        }

        out = std::move (result);
        return true;
    }

    /** Checks that `v` is an object with exactly these fields. */
    inline bool expectObject (const juce::var& v, std::initializer_list<const char*> fields, const char* typeName,
                              const juce::String& path, juce::String& error)
    {
        auto* object = v.getDynamicObject();
        if (object == nullptr || v.isArray())
            return fail (error, path, "is " + describe (v) + ", not an object (" + typeName + ")");

        const auto& properties = object->getProperties();

        for (const auto* field : fields)
            if (! properties.contains (field))
                return fail (error, path + "." + field, "is missing");

        for (const auto& property : properties)
        {
            bool known = false;
            for (const auto* field : fields)
                known = known || property.name.toString() == field;

            if (! known)
                return fail (error, path + "." + property.name.toString(), "is not a field of " + juce::String (typeName));
        }

        return true;
    }

    inline juce::var write (bool b) { return b; }

    /** A number that is not finite becomes null, which the page refuses with the field's name,
        rather than text that is not JSON at all. */
    inline juce::var write (double d) { return std::isfinite (d) ? juce::var (d) : juce::var(); }

    inline juce::var write (juce::int64 i) { return i; }

    inline juce::var write (const juce::String& s) { return s; }

    template <typename T>
    juce::var write (const std::vector<T>& items)
    {
        juce::Array<juce::var> out;
        out.ensureStorageAllocated (static_cast<int> (items.size()));
        for (const auto& item : items)
            out.add (write (item));
        return out;
    }

    /** The answer to a call that worked: { "ok": true, "result": ... }. */
    inline juce::var success (const juce::var& result)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("ok", true);
        object->setProperty ("result", result);
        return juce::var (object);
    }

    /** The answer to a call that worked and returns nothing: { "ok": true }. */
    inline juce::var success()
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("ok", true);
        return juce::var (object);
    }

    /** The answer to a call the app refused or could not carry out: { "ok": false, "error": ... }. */
    inline juce::var failure (const juce::String& error)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("ok", false);
        object->setProperty ("error", error);
        return juce::var (object);
    }

    /** Whether two JSON values are the same: numbers by value, objects by their fields in any order. */
    inline bool sameJson (const juce::var& a, const juce::var& b)
    {
        const auto isNumber = [] (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); };

        if (isNumber (a) || isNumber (b))
            return isNumber (a) && isNumber (b) && juce::exactlyEqual (static_cast<double> (a), static_cast<double> (b));

        if (a.isArray() || b.isArray())
        {
            const auto* x = a.getArray();
            const auto* y = b.getArray();
            if (x == nullptr || y == nullptr || x->size() != y->size())
                return false;
            for (int i = 0; i < x->size(); ++i)
                if (! sameJson (x->getReference (i), y->getReference (i)))
                    return false;
            return true;
        }

        if (a.isObject() || b.isObject())
        {
            const auto* x = a.getDynamicObject();
            const auto* y = b.getDynamicObject();
            if (x == nullptr || y == nullptr || x->getProperties().size() != y->getProperties().size())
                return false;
            for (const auto& p : x->getProperties())
                if (! y->hasProperty (p.name) || ! sameJson (p.value, y->getProperty (p.name)))
                    return false;
            return true;
        }

        if (a.isBool() || b.isBool())
            return a.isBool() && b.isBool() && static_cast<bool> (a) == static_cast<bool> (b);

        if (a.isString() || b.isString())
            return a.isString() && b.isString() && a.toString() == b.toString();

        return (a.isVoid() || a.isUndefined()) && (b.isVoid() || b.isUndefined());
    }
} // namespace collabgod::bridge
