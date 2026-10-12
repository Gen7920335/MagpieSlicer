#include <catch2/catch_test_macros.hpp>

#include <libslic3r/ModHost/ParamGraph.hpp>
#include <libslic3r/PrintConfig.hpp>

using namespace Slic3r;
using namespace Slic3r::ModHost;

TEST_CASE("Every Orca setting is a core node of the parameter graph", "[ParamGraph]")
{
    const ParamGraph graph = ParamGraph::with_core_options();

    REQUIRE(graph.size() == print_config_def.options.size());
    for (const auto &[key, def] : print_config_def.options) {
        const ParamNode *node = graph.find(key);
        REQUIRE(node != nullptr);
        CHECK(node->key == key);
        CHECK(node->type == def.type);
        CHECK(node->origin == ParamOrigin::Core);
    }
}

TEST_CASE("An unknown key is not a node", "[ParamGraph]")
{
    const ParamGraph graph = ParamGraph::with_core_options();
    CHECK(graph.find("no_such_setting") == nullptr);
    CHECK(graph.find("") == nullptr);
}

TEST_CASE("Core nodes come out in the same order on every build", "[ParamGraph]")
{
    const ParamGraph a = ParamGraph::with_core_options();
    const ParamGraph b = ParamGraph::with_core_options();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        CHECK(a.nodes()[i].key == b.nodes()[i].key);
}
