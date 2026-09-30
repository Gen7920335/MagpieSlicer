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

TEST_CASE("Automatic detail walls do not substitute a different material", "[WallToolRouting][MaterialSafety]")
{
    PrintConfig config = routing_config({1, 2, 3, 4}, {"same", "same", "same", "same"});
    config.filament_type.values = {"PVA", "PETG", "PLA", "PLA"};
    config.filament_soluble.values = {true, false, false, false};
    CHECK(detail_wall_tool(config, detail_region(), 3).filament_id_1based == 3);
    // Explicitly chosen smaller tool remains available for intentional combinations.
    CHECK(detail_wall_tool(config, detail_region(1), 3).filament_id_1based == 1);
    config.filament_type.values[1] = "PLA";
    CHECK(detail_wall_tool(config, detail_region(), 3).filament_id_1based == 2);
    config.filament_type.values[0] = "PLA";
    CHECK(detail_wall_tool(config, detail_region(), 3).filament_id_1based == 2);
    config.filament_soluble.values[0] = false;
    CHECK(detail_wall_tool(config, detail_region(), 3).filament_id_1based == 1);
    config.filament_type.values = {"PVA", "PLA", "PLA", "PLA"};
    config.filament_map.values = {1, 1, 3, 4};
    CHECK(detail_wall_tool(config, detail_region(), 3).filament_id_1based == 2);
}

TEST_CASE("Auto detail routing preserves distinct assigned colours for every hotend permutation",
          "[WallToolRouting][PaintColourSafety]")
{
    std::array<int, 4> map {1, 2, 3, 4};
    do {
        PrintConfig config = routing_config(std::vector<int>(map.begin(), map.end()));
        config.filament_type.values.assign(4, "PLA");
        config.filament_soluble.values.assign(4, false);
        for (unsigned int filament = 1; filament <= 4; ++filament) {
            CAPTURE(map, filament);
            const ResolvedWallTool tool = detail_wall_tool(config, detail_region(), filament);
            REQUIRE(tool);
            CHECK(tool.filament_id_1based == filament);
            CHECK(tool.hotend_id_1based == unsigned(map[filament - 1]));
            CHECK(tool.nozzle_diameter == Catch::Approx(config.nozzle_diameter.values[map[filament - 1] - 1]));
        }
    } while (std::next_permutation(map.begin(), map.end()));
}

TEST_CASE("Auto detail routing requires known matching colour metadata", "[WallToolRouting][PaintColourSafety]")
{
    PrintConfig config = routing_config({3, 1, 4, 2});
    config.filament_type.values.assign(4, "PLA");
    config.filament_soluble.values.assign(4, false);

    SECTION("missing colour vector") { config.filament_colour.values.clear(); }
    SECTION("empty colour strings are not colour matches") { config.filament_colour.values.assign(4, ""); }
    SECTION("short colour vector does not identify any smaller candidate") { config.filament_colour.values = {"blue"}; }
    SECTION("same nozzle sizes preserve the assignment") { config.nozzle_diameter.values.assign(4, 0.4); }

    const ResolvedWallTool tool = detail_wall_tool(config, detail_region(), 1);
    REQUIRE(tool);
    CHECK(tool.filament_id_1based == 1);
    CHECK(tool.hotend_id_1based == 3);
    CHECK(tool.nozzle_diameter == Catch::Approx(0.4));
}

TEST_CASE("Auto detail routing still selects a compatible same-colour smaller nozzle",
          "[WallToolRouting][PaintColourSafety]")
{
    PrintConfig config = routing_config({3, 1, 4, 2}, {"blue", "red", "green", "blue"});
    config.filament_type.values.assign(4, "PLA");
    config.filament_soluble.values.assign(4, false);
    unsigned int expected_filament = 4;
    unsigned int expected_hotend = 2;
    double expected_nozzle = 0.2;

    SECTION("same colour remains available in Auto") {}
    SECTION("different material is not compatible") {
        config.filament_type.values[3] = "PETG";
        expected_filament = 1;
        expected_hotend = 3;
        expected_nozzle = 0.4;
    }

    const ResolvedWallTool tool = detail_wall_tool(config, detail_region(), 1);
    REQUIRE(tool);
    CHECK(tool.filament_id_1based == expected_filament);
    CHECK(tool.hotend_id_1based == expected_hotend);
    CHECK(tool.nozzle_diameter == Catch::Approx(expected_nozzle));
    // A compatible same-colour candidate still takes precedence over manual fallback.
    if (expected_filament == 4)
        CHECK(detail_wall_tool(config, detail_region(1), 1).filament_id_1based == 4);
}

TEST_CASE("Explicit detail tool choices and feature-off assignments are preserved",
          "[WallToolRouting][PaintColourSafety]")
{
    PrintConfig config = routing_config({3, 1, 4, 2});
    config.filament_type.values.assign(4, "PLA");
    config.filament_soluble.values.assign(4, false);
    PrintRegionConfig region = detail_region(2);
    unsigned int expected_filament = 4;
    unsigned int expected_hotend = 2;

    SECTION("explicit tool can use another colour") {}
    SECTION("explicit tool can use another material") { config.filament_type.values[3] = "PETG"; }
    SECTION("legacy explicit fallback remains available when the requested nozzle is not smaller") {
        region.crisp_corner_detail_toolhead.value = 4;
        expected_filament = 2;
        expected_hotend = 1;
    }

    const ResolvedWallTool manual = detail_wall_tool(config, region, 1);
    REQUIRE(manual);
    CHECK(manual.filament_id_1based == expected_filament);
    CHECK(manual.hotend_id_1based == expected_hotend);

    region.use_smaller_nozzles_in_crisp_corners.value = false;
    const ResolvedWallTool disabled = detail_wall_tool(config, region, 1);
    REQUIRE(disabled);
    CHECK(disabled.filament_id_1based == 1);
    CHECK(disabled.hotend_id_1based == 3);
    CHECK(disabled.nozzle_diameter == Catch::Approx(0.4));
}

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
