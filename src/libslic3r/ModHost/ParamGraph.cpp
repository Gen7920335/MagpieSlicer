#include "ParamGraph.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <utility>

namespace Slic3r {
namespace ModHost {

ParamGraph ParamGraph::with_core_options()
{
    ParamGraph graph;
    // print_config_def keeps its options in a std::map, so the nodes come out in key order and
    // the graph is the same on every run.
    for (const auto &[key, def] : print_config_def.options)
        graph.add({key, def.type, ParamOrigin::Core});
    return graph;
}

const ParamNode *ParamGraph::find(const std::string &key) const
{
    auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_nodes[it->second];
}

void ParamGraph::add(ParamNode node)
{
    m_index.emplace(node.key, m_nodes.size());
    m_nodes.push_back(std::move(node));
}

} // namespace ModHost
} // namespace Slic3r
