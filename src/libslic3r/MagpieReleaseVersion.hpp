#pragma once

// MAGPIE: Magpie Slicer release versions (docs/MAGPIE_VERSIONING.md) as the update check reads them
// from GitHub release tags: "v2.5.0.2.0", "2.5.0.2.1-beta.1". Orca's Semver cannot hold them:
// semver_parse() folds a fourth numeric field into the patch and rejects a fifth, and the
// application's SoftFever_VERSION ("2.5.0-modified.2.0") does not match the update check's pattern.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

struct MagpieReleaseVersion
{
    std::vector<long> numbers;    // dot-separated numeric fields, at least two
    std::string       prerelease; // text after the first '-', empty for a final release

    // Accepts an optional leading 'v', two or more numeric fields, then an optional "-<label>".
    static std::optional<MagpieReleaseVersion> parse(const std::string &text)
    {
        std::string s = text;
        if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
            s.erase(0, 1);
        MagpieReleaseVersion out;
        const size_t dash = s.find('-');
        if (dash != std::string::npos) {
            out.prerelease = s.substr(dash + 1);
            if (out.prerelease.empty())
                return std::nullopt;
            s.erase(dash);
        }
        size_t start = 0;
        while (true) {
            const size_t dot   = s.find('.', start);
            const std::string field = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (field.empty() || field.size() > 9 || field.find_first_not_of("0123456789") != std::string::npos)
                return std::nullopt;
            out.numbers.push_back(std::stol(field));
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        if (out.numbers.size() < 2)
            return std::nullopt;
        return out;
    }

    std::string to_string() const
    {
        std::string s;
        for (size_t i = 0; i < numbers.size(); ++i)
            s += (i ? "." : "") + std::to_string(numbers[i]);
        return prerelease.empty() ? s : s + "-" + prerelease;
    }
};

// Negative, zero or positive as a is older than, the same as or newer than b. Missing numeric fields
// count as 0 (2.5.0.2 == 2.5.0.2.0). With equal numbers a final release is newer than any of its
// pre-releases; pre-release labels compare field by field on '.', numerically where both fields are
// numbers (beta.10 > beta.2), otherwise as text.
inline int compare(const MagpieReleaseVersion &a, const MagpieReleaseVersion &b)
{
    const size_t n = std::max(a.numbers.size(), b.numbers.size());
    for (size_t i = 0; i < n; ++i) {
        const long x = i < a.numbers.size() ? a.numbers[i] : 0;
        const long y = i < b.numbers.size() ? b.numbers[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (a.prerelease.empty() || b.prerelease.empty())
        return a.prerelease.empty() == b.prerelease.empty() ? 0 : (a.prerelease.empty() ? 1 : -1);
    auto split = [](const std::string &s) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t dot = s.find('.', start);
            parts.push_back(s.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos)
                return parts;
            start = dot + 1;
        }
    };
    auto is_number = [](const std::string &s) {
        return !s.empty() && s.size() <= 9 && s.find_first_not_of("0123456789") == std::string::npos;
    };
    const std::vector<std::string> pa = split(a.prerelease), pb = split(b.prerelease);
    for (size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) {
        if (is_number(pa[i]) && is_number(pb[i])) {
            const long x = std::stol(pa[i]), y = std::stol(pb[i]);
            if (x != y)
                return x < y ? -1 : 1;
        } else if (pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    }
    return pa.size() == pb.size() ? 0 : (pa.size() < pb.size() ? -1 : 1);
}

} // namespace Slic3r
