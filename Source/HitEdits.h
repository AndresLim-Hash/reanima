#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <vector>

// LOOP: the user's say over the hits the analysis found. Some turned off, some added. Kept by position (samples
// of the loop) and matched within a tolerance, so they hold when the sample is analysed again or the sensitivity
// moves. They apply to every list of cuts made from the hits: SLICE's Transitorios (and Por golpe) and Beats.
struct HitEdits
{
    std::vector<int> off, on;       // sorted

    bool empty() const noexcept { return off.empty() && on.empty(); }

    // Within this many samples a position is the same hit (10 ms).
    static int tolerance (double sampleRate) noexcept { return std::max (1, (int) (0.010 * sampleRate)); }

    // "off:1200,5300|on:9000" (the project's and the preset's state).
    juce::String toString() const
    {
        auto list = [] (const std::vector<int>& v)
        {
            juce::StringArray t;
            for (const int x : v)
                t.add (juce::String (x));
            return t.joinIntoString (",");
        };
        if (empty())
            return {};
        return "off:" + list (off) + "|on:" + list (on);
    }

    static HitEdits fromString (const juce::String& text)
    {
        HitEdits e;
        for (const auto& part : juce::StringArray::fromTokens (text, "|", ""))
        {
            const auto name = part.upToFirstOccurrenceOf (":", false, false).trim();
            auto& into = name == "off" ? e.off : name == "on" ? e.on : e.off;
            if (name != "off" && name != "on")
                continue;
            for (const auto& t : juce::StringArray::fromTokens (part.fromFirstOccurrenceOf (":", false, false), ",", ""))
                if (t.trim().isNotEmpty() && t.getIntValue() > 0)
                    into.push_back (t.getIntValue());
        }
        std::sort (e.off.begin(), e.off.end());
        std::sort (e.on.begin(), e.on.end());
        return e;
    }

    static bool near (const std::vector<int>& sorted, int at, int tol) noexcept
    {
        const auto it = std::lower_bound (sorted.begin(), sorted.end(), at - tol);
        return it != sorted.end() && *it <= at + tol;
    }

    // A list of cuts (from 0, sorted) as the user wants it: without the ones turned off, with the added ones, at
    // least minGap apart (an added one wins over a found one it is too close to), none within minGap of the end.
    std::vector<int> apply (const std::vector<int>& cuts, int length, int minGap, int tol) const
    {
        if (empty())
            return cuts;
        std::vector<int> found;
        for (const int c : cuts)
            if (c == 0 || ! near (off, c, tol))
                found.push_back (c);
        std::vector<int> out { 0 };
        std::vector<int> added;
        for (const int a : on)
            if (a > 0 && a < length - minGap)
                added.push_back (a);
        // Added ones first: a found cut too close to one of them goes.
        std::vector<int> all;
        for (const int c : found)
            if (c > 0 && ! near (added, c, minGap - 1))
                all.push_back (c);
        all.insert (all.end(), added.begin(), added.end());
        std::sort (all.begin(), all.end());
        for (const int c : all)
            if (c - out.back() >= minGap && length - c >= minGap)
                out.push_back (c);
        return out;
    }

    // The user clicked a cut at `at` (or near it): one that is on goes off, one turned off comes back, an added
    // one goes. Returns true if anything changed.
    bool toggle (int at, bool isCut, int tol)
    {
        auto drop = [tol] (std::vector<int>& v, int x)
        {
            const auto before = v.size();
            v.erase (std::remove_if (v.begin(), v.end(), [x, tol] (int y) { return std::abs (y - x) <= tol; }), v.end());
            return v.size() != before;
        };
        if (drop (on, at))
            return true;            // an added one: gone
        if (drop (off, at))
            return true;            // turned off: back on
        if (! isCut)
            return false;
        off.insert (std::upper_bound (off.begin(), off.end(), at), at);
        return true;
    }

    // A new cut where there was none (or where one was turned off: it comes back).
    void add (int at, int tol)
    {
        off.erase (std::remove_if (off.begin(), off.end(), [at, tol] (int y) { return std::abs (y - at) <= tol; }), off.end());
        if (! near (on, at, tol))
            on.insert (std::upper_bound (on.begin(), on.end(), at), at);
    }
};
