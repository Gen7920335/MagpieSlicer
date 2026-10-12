#pragma once

#include "libslic3r/Config.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace ModHost {

// Where a parameter is defined. Core parameters are Orca's own print, filament and printer
// settings; mod parameters are added by a loaded mod under its "<mod id>.<name>" namespace.
enum class ParamOrigin { Core, Mod };

struct ParamNode
{
    std::string      key;
    ConfigOptionType type;
    ParamOrigin      origin;
};

// The parameter graph: one node per setting the slicer resolves. Every Orca setting is a core
// node; mods add their own nodes and attach expressions to existing ones. With no mod loaded the
// graph holds only core nodes and changes no value, which is what keeps the feature-off output
// equal to vanilla Orca.
class ParamGraph
{
public:
    // A graph holding every option of print_config_def as a core node without an expression.
    static ParamGraph with_core_options();

    const ParamNode *find(const std::string &key) const;
    size_t           size() const { return m_nodes.size(); }
    const std::vector<ParamNode> &nodes() const { return m_nodes; }

private:
    void add(ParamNode node);

    std::vector<ParamNode>                  m_nodes;
    std::unordered_map<std::string, size_t> m_index;
};

} // namespace ModHost
} // namespace Slic3r
