#include "LayerRanges.hpp"

#include <charconv>
#include <string_view>
#include <system_error>

namespace Slic3r {
namespace ModHost {

namespace {

bool parse_positive(std::string_view text, std::size_t &value)
{
    std::size_t parsed = 0;
    const auto  result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0)
        return false;
    value = parsed;
    return true;
}

} // namespace

LayerRangeList parse_layer_ranges(const std::string &text)
{
    LayerRangeList out;
    std::size_t    begin = 0;
    while (begin <= text.size()) {
        const std::size_t end   = text.find(';', begin);
        const std::string entry = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin                   = end == std::string::npos ? text.size() + 1 : end + 1;
        if (entry.empty())
            continue;
        const std::size_t a = entry.find(':');
        const std::size_t b = a == std::string::npos ? std::string::npos : entry.find(':', a + 1);
        LayerRange        r;
        const std::string_view v(entry);
        if (a != std::string::npos && b != std::string::npos && entry.find(':', b + 1) == std::string::npos &&
            parse_positive(v.substr(0, a), r.first) && parse_positive(v.substr(a + 1, b - a - 1), r.last) &&
            parse_positive(v.substr(b + 1), r.toolhead))
            out.ranges.push_back(r);
        else
            out.unread.push_back(entry);
    }
    return out;
}

std::string serialize_layer_ranges(const LayerRangeList &list)
{
    std::string out;
    auto add = [&out](const std::string &entry) {
        if (!out.empty())
            out += ';';
        out += entry;
    };
    for (const LayerRange &r : list.ranges)
        add(std::to_string(r.first) + ":" + std::to_string(r.last) + ":" + std::to_string(r.toolhead));
    for (const std::string &entry : list.unread)
        add(entry);
    return out;
}

} // namespace ModHost
} // namespace Slic3r
