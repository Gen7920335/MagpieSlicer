#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

TEST_CASE("Triangle selector overlays an independent painting channel", "[TriangleSelector][Painting][Mixed]")
{
    TriangleMesh mesh(its_make_cube(10.f, 10.f, 10.f));

    TriangleSelector base(mesh);
    base.set_facet(0, EnforcerBlockerType::ENFORCER);
    base.set_facet(1, EnforcerBlockerType::BLOCKER);

    TriangleSelector channel(mesh);
    channel.set_facet(0, EnforcerBlockerType::ENFORCER);
    channel.set_facet(1, EnforcerBlockerType::BLOCKER);

    EnforcerBlockerStateMap channel_map;
    channel_map.fill(EnforcerBlockerType::NONE);
    channel_map[size_t(EnforcerBlockerType::ENFORCER)] = EnforcerBlockerType::Extruder3;
    channel_map[size_t(EnforcerBlockerType::BLOCKER)] = EnforcerBlockerType::Extruder4;
    base.overlay_painting(channel.serialize(), channel_map);

    REQUIRE(base.has_facets(EnforcerBlockerType::Extruder3));
    REQUIRE(base.has_facets(EnforcerBlockerType::Extruder4));
    REQUIRE(base.num_facets(EnforcerBlockerType::Extruder3) == 1);
    REQUIRE(base.num_facets(EnforcerBlockerType::Extruder4) == 1);
    REQUIRE_FALSE(base.has_facets(EnforcerBlockerType::ENFORCER));
    REQUIRE_FALSE(base.has_facets(EnforcerBlockerType::BLOCKER));

    TriangleSelector support(mesh);
    support.deserialize(base.serialize(), false);
    EnforcerBlockerStateMap support_map;
    support_map.fill(EnforcerBlockerType::NONE);
    support_map[size_t(EnforcerBlockerType::Extruder3)] = EnforcerBlockerType::ENFORCER;
    support_map[size_t(EnforcerBlockerType::Extruder4)] = EnforcerBlockerType::ENFORCER;
    support.remap_triangle_state(support_map);

    TriangleSelector restored_channel(mesh);
    restored_channel.deserialize(base.serialize(), false);
    EnforcerBlockerStateMap restored_channel_map;
    restored_channel_map.fill(EnforcerBlockerType::NONE);
    restored_channel_map[size_t(EnforcerBlockerType::Extruder3)] = EnforcerBlockerType::ENFORCER;
    restored_channel_map[size_t(EnforcerBlockerType::Extruder4)] = EnforcerBlockerType::BLOCKER;
    restored_channel.remap_triangle_state(restored_channel_map);

    REQUIRE(support.has_facets(EnforcerBlockerType::ENFORCER));
    REQUIRE_FALSE(support.has_facets(EnforcerBlockerType::BLOCKER));
    REQUIRE(restored_channel.has_facets(EnforcerBlockerType::ENFORCER));
    REQUIRE(restored_channel.has_facets(EnforcerBlockerType::BLOCKER));
}
