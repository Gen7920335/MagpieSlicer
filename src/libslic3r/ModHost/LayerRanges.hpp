#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace ModHost {

// A layer range a mod parameter assigns to a toolhead: "first:last:toolhead", layers numbered from 1, the
// toolhead from 1 (multi_nozzle_walls.override_regions). The settings tab edits the parameter as a table of these.
struct LayerRange
{
    std::size_t first    = 1;
    std::size_t last     = 1;
    std::size_t toolhead = 1;
    bool operator==(const LayerRange &rhs) const { return first == rhs.first && last == rhs.last && toolhead == rhs.toolhead; }
};

struct LayerRangeList
{
    // The well-formed entries, in their order: where ranges overlap the last one wins, so the order is kept.
    std::vector<LayerRange>  ranges;
    // Entries the reader cannot take (not three whole numbers above 0), kept as written so an edit of the table
    // does not drop text the user typed; the mod ignores them as it always did.
    std::vector<std::string> unread;
};

// Splits the ';'-separated entries the way the multi-nozzle walls mod reads them: no blanks allowed, each number
// a whole number above 0, a range written last:first kept as written. Empty entries are dropped.
LayerRangeList parse_layer_ranges(const std::string &text);
// The text of `list`: its ranges in order, then the entries it could not read.
std::string serialize_layer_ranges(const LayerRangeList &list);

} // namespace ModHost
} // namespace Slic3r
