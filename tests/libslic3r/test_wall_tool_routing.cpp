#include <catch2/catch_all.hpp>

#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <array>
#include <numeric>

using namespace Slic3r;

namespace {

PrintConfig routing_config(const std::vector<int> &filament_map,
                           const std::vector<std::string> &colours = {"red", "green", "blue", "yellow"})
{
    PrintConfig config;
    config.nozzle_diameter.values = {0.15, 0.20, 0.40, 0.80};
    config.filament_map.values = filament_map;
    config.filament_diameter.values.assign(filament_map.empty() ? 4 : filament_map.size(), 1.75);
    config.filament_colour.values = colours;
    return config;
}

PrintRegionConfig detail_region(int manual_hotend = 0)
{
    PrintRegionConfig region;
    region.use_smaller_nozzles_in_crisp_corners.value = true;
    region.crisp_corner_detail_toolhead.value = manual_hotend;
    return region;
}

unsigned int filament_mapped_to(const std::vector<int> &map, unsigned int hotend_1based)
{
    const auto it = std::find(map.begin(), map.end(), int(hotend_1based));
    return it == map.end() ? 0u : unsigned(std::distance(map.begin(), it) + 1);
}

} // namespace

TEST_CASE("Wall tool routing resolves identity and permuted filament maps", "[WallToolRouting]")
{
    SECTION("identity map") {
        const PrintConfig config = routing_config({1, 2, 3, 4});
        const ResolvedWallTool tool = wall_tool_for_filament(config, 3);
        REQUIRE(tool);
        CHECK(tool.filament_id_1based == 3);
        CHECK(tool.hotend_id_1based == 3);
        CHECK(tool.nozzle_diameter == Catch::Approx(0.40));
    }

    SECTION("permuted map") {
        const PrintConfig config = routing_config({3, 1, 4, 2});
        const ResolvedWallTool tool = wall_tool_for_filament(config, 2);
        REQUIRE(tool);
        CHECK(tool.filament_id_1based == 2);
        CHECK(tool.hotend_id_1based == 1);
        CHECK(tool.nozzle_diameter == Catch::Approx(0.15));
    }
}

TEST_CASE("Legacy partial filament maps retain identity fallback", "[WallToolRouting]")
{
    PrintConfig config = routing_config({1});
    config.filament_diameter.values.assign(4, 1.75);
    config.filament_colour.values = {"same", "same", "same", "same"};

    const ResolvedWallTool fourth = wall_tool_for_filament(config, 4);
    REQUIRE(fourth);
    CHECK(fourth.filament_id_1based == 4);
    CHECK(fourth.hotend_id_1based == 4);
    CHECK(fourth.nozzle_diameter == Catch::Approx(0.80));

    const ResolvedWallTool detail = detail_wall_tool(config, detail_region(), 3);
    REQUIRE(detail);
    CHECK(detail.hotend_id_1based == 1);
    CHECK(detail.filament_id_1based == 1);
}

TEST_CASE("Reverse hotend routing preserves the preferred material", "[WallToolRouting]")
{
    const PrintConfig config = routing_config({2, 1, 2, 3}, {"blue", "red", "red", "black"});

    const ResolvedWallTool preferred = wall_tool_for_hotend(config, 2, 3);
    REQUIRE(preferred);
    CHECK(preferred.filament_id_1based == 3);

    const ResolvedWallTool same_colour = wall_tool_for_hotend(config, 2, 2);
    REQUIRE(same_colour);
    CHECK(same_colour.filament_id_1based == 3);
}

TEST_CASE("Detail routing remains correct for every four-hotend permutation", "[WallToolRouting]")
{
    std::array<int, 4> map {1, 2, 3, 4};
    size_t checked = 0;
    do {
        const std::vector<int> permutation(map.begin(), map.end());
        const PrintConfig config = routing_config(permutation, {"same", "same", "same", "same"});
        const unsigned int base_filament = filament_mapped_to(permutation, 3);
        const unsigned int expected_detail_filament = filament_mapped_to(permutation, 1);
        const ResolvedWallTool detail = detail_wall_tool(config, detail_region(), base_filament);

        REQUIRE(detail);
        CHECK(detail.hotend_id_1based == 1);
        CHECK(detail.filament_id_1based == expected_detail_filament);
        CHECK(detail.nozzle_diameter == Catch::Approx(0.15));
        ++checked;
    } while (std::next_permutation(map.begin(), map.end()));

    CHECK(checked == 24);
}

TEST_CASE("Detail routing applies colour priority then manual hotend", "[WallToolRouting]")
{
    const std::vector<int> map {3, 1, 4, 2};
    const unsigned int base_filament = filament_mapped_to(map, 3);

    SECTION("same colour has priority") {
        const PrintConfig config = routing_config(map, {"blue", "red", "green", "red"});
        const ResolvedWallTool detail = detail_wall_tool(config, detail_region(2), base_filament);
        REQUIRE(detail);
        CHECK(detail.hotend_id_1based == 2);
        CHECK(detail.filament_id_1based == filament_mapped_to(map, 2));
    }

    SECTION("manual hotend is used when no smaller hotend has the base colour") {
        const PrintConfig config = routing_config(map, {"blue", "red", "green", "yellow"});
        const ResolvedWallTool detail = detail_wall_tool(config, detail_region(2), base_filament);
        REQUIRE(detail);
        CHECK(detail.hotend_id_1based == 2);
        CHECK(detail.filament_id_1based == filament_mapped_to(map, 2));
    }
}

TEST_CASE("Unmapped hotends never alias another filament", "[WallToolRouting]")
{
    PrintConfig config = routing_config({1, 3}, {"red", "red"});
    const ResolvedWallTool missing = wall_tool_for_hotend(config, 2, 1);
    CHECK_FALSE(missing);

    const ResolvedWallTool fallback = detail_wall_tool(config, detail_region(2), 2);
    REQUIRE(fallback);
    CHECK(fallback.hotend_id_1based != 2);
    CHECK(fallback.filament_id_1based == 1);
    CHECK(fallback.hotend_id_1based == 1);
}

TEST_CASE("Large nozzle override resolves layer ranges to mapped filaments", "[WallToolRouting]")
{
    const std::vector<int> map {3, 1, 4, 2};
    const PrintConfig config = routing_config(map);
    PrintRegionConfig region = detail_region();
    region.crisp_corner_large_nozzle_override_regions.values = {"1:20:4", "5:10:3"};

    const ResolvedWallTool layer_2 = large_nozzle_override_wall_tool(config, region, 1, 1);
    REQUIRE(layer_2);
    CHECK(layer_2.hotend_id_1based == 4);
    CHECK(layer_2.filament_id_1based == filament_mapped_to(map, 4));

    const ResolvedWallTool overlapping_layer_6 = large_nozzle_override_wall_tool(config, region, 5, 1);
    REQUIRE(overlapping_layer_6);
    CHECK(overlapping_layer_6.hotend_id_1based == 3);
    CHECK(overlapping_layer_6.filament_id_1based == filament_mapped_to(map, 3));

    CHECK_FALSE(large_nozzle_override_wall_tool(config, region, 20, 1));
}
