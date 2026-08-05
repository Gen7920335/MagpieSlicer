#include <catch2/catch_all.hpp>

#include <numeric>
#include <sstream>

#include "test_helpers.hpp" // get access to init_print, etc

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r::Test;
using namespace Slic3r;

/// Test the expected behavior for auto-width,
/// spacing, etc
SCENARIO("Flow math for non-bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        ConfigOptionFloatOrPercent	width(1.0, false);
        float nozzle_diameter	= 0.4f;
        float layer_height		= 0.4f;

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(1.125 * nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }

        THEN("Internal perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(1.125 *nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(width.value - layer_height * (1.0 - PI / 4.0)));
            flow = Flow::new_from_config_width(frPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(width.value - layer_height * (1.0 - PI / 4.0)));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25") {
        float nozzle_diameter	= 0.25f;
        float layer_height		= 0.5f;
        WHEN("layer height is set to 0.2") {
            layer_height = 0.15f;
            THEN("Max width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Catch::Approx(1.125 * nozzle_diameter));
            }
        }
        WHEN("Layer height is set to 0.25") {
            layer_height = 0.25f;
            THEN("Min width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Catch::Approx(1.125 * nozzle_diameter));
            }
        }
    }

#if 0
    /// Check for an edge case in the maths where the spacing could be 0; original
    /// math is 0.99. Slic3r issue #4654
    GIVEN("Input spacing of 0.414159 and a total width of 2") {
        double in_spacing = 0.414159;
        double total_width = 2.0;
        auto flow = Flow::new_from_spacing(1.0, 0.4, 0.3);
        WHEN("solid_spacing() is called") {
            double result = flow.solid_spacing(total_width, in_spacing);
            THEN("Yielded spacing is greater than 0") {
                REQUIRE(result > 0);
            }
        }
    }
#endif    

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow math for bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
		float nozzle_diameter	= 0.4f;
		float bridge_flow		= 1.0f;
        WHEN("Flow role is frExternalPerimeter") {
            auto flow = Flow::bridging_flow(nozzle_diameter * sqrt(bridge_flow), nozzle_diameter);
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE(flow.width() == Catch::Approx(nozzle_diameter));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING") {
                REQUIRE(flow.spacing() == Catch::Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING));
            }
        }
    }
}

TEST_CASE("Support interface density and spacing conversion", "[Flow][SupportInterface]")
{
    REQUIRE(support_interface_density_from_spacing(0.4, 0.0) == Catch::Approx(1.0));
    REQUIRE(support_interface_density_from_spacing(0.4, 0.4) == Catch::Approx(0.5));
    REQUIRE(support_interface_density_from_spacing(0.4, -1.0) == Catch::Approx(1.0));

    for (const double density : {0.01, 0.25, 0.5, 1.0}) {
        const double spacing = support_interface_spacing_from_density(0.4, density);
        REQUIRE(support_interface_density_from_spacing(0.4, spacing) == Catch::Approx(density));
    }
}

TEST_CASE("Multi-nozzle wall planning does not mutate base wall semantics", "[Flow][MultiNozzleWalls]")
{
    FullPrintConfig config;
    config.wall_loops.value = 3;
    config.crisp_corner_small_nozzle_wall_count.value = 4;
    config.crisp_corner_interlace_small_nozzle_walls.value = true;

    SECTION("stored detail values cannot enable the feature") {
        config.use_smaller_nozzles_in_crisp_corners.value = false;
        CHECK_FALSE(detail_walls_enabled(config));
        CHECK(detail_wall_count_for_layer(config, 0) == 0);
        CHECK(detail_wall_count_for_layer(config, 1) == 0);
        CHECK(total_wall_count_for_layer(config, 0) == 3);
    }

    SECTION("interlocking exchanges one boundary wall without changing the total") {
        config.use_smaller_nozzles_in_crisp_corners.value = true;
        CHECK(detail_wall_count_for_layer(config, 0) == 4);
        CHECK(detail_wall_count_for_layer(config, 1) == 3);
        CHECK(total_wall_count_for_layer(config, 1) == 7);
        CHECK(total_wall_count_for_layer(config, 0) == 7);
        CHECK(total_wall_count_for_layer(config, 0) - detail_wall_count_for_layer(config, 0) == 3);
        CHECK(total_wall_count_for_layer(config, 1) - detail_wall_count_for_layer(config, 1) == 4);
    }

    SECTION("minimum values remain printable") {
        config.use_smaller_nozzles_in_crisp_corners.value = true;
        config.wall_loops.value = 0;
        config.crisp_corner_small_nozzle_wall_count.value = 0;
        config.crisp_corner_interlace_small_nozzle_walls.value = false;
        CHECK(detail_wall_count_for_layer(config, 0) == 1);
        CHECK(total_wall_count_for_layer(config, 0) == 3);
    }
}

TEST_CASE("Detail tool selection uses actual nozzle diameters", "[Flow][MultiNozzleWalls]")
{
    FullPrintConfig config;
    config.use_smaller_nozzles_in_crisp_corners.value = true;
    config.nozzle_diameter.values = { 0.4, 0.2, 0.15 };
    config.filament_colour.values = { "#FF0000", "#FF0000", "#0000FF" };

    SECTION("same-colour smaller nozzle has priority over manual fallback") {
        config.crisp_corner_detail_toolhead.value = 3;
        CHECK(detail_wall_tool(config, config, 1).filament_id_1based == 2);
    }

    SECTION("manual selection is used when no same-colour candidate exists") {
        config.filament_colour.values = { "#FF0000", "#00FF00", "#0000FF" };
        config.crisp_corner_detail_toolhead.value = 2;
        CHECK(detail_wall_tool(config, config, 1).filament_id_1based == 2);
    }

    SECTION("disabled feature always returns the base tool") {
        config.use_smaller_nozzles_in_crisp_corners.value = false;
        config.crisp_corner_small_nozzle_wall_count.value = 4;
        CHECK(detail_wall_tool(config, config, 1).filament_id_1based == 1);
    }

    SECTION("larger nozzles are never detail candidates even with a narrow line width") {
        config.nozzle_diameter.values = { 0.4, 0.6 };
        config.filament_colour.values = { "#FF0000", "#FF0000" };
        config.toolhead_outer_wall_line_width.values = { FloatOrPercent(0., false), FloatOrPercent(0.1, false) };
        config.crisp_corner_detail_toolhead.value = 2;
        CHECK(detail_wall_tool(config, config, 1).filament_id_1based == 1);
    }

    SECTION("base tool reverses when the selected base nozzle is larger") {
        config.nozzle_diameter.values = { 0.4, 0.8, 0.2 };
        config.filament_colour.values = { "#FF0000", "#FF0000", "#0000FF" };
        config.crisp_corner_detail_toolhead.value = 0;
        CHECK(detail_wall_tool(config, config, 2).filament_id_1based == 1);
    }

    SECTION("four independent hotends use the smallest valid nozzle for each base tool") {
        config.nozzle_diameter.values = { 0.4, 0.15, 0.6, 0.8 };
        config.filament_colour.values = { "#110000", "#001100", "#000011", "#111100" };
        config.crisp_corner_detail_toolhead.value = 0;
        CHECK(detail_wall_tool(config, config, 1).filament_id_1based == 2);
        CHECK(detail_wall_tool(config, config, 2).filament_id_1based == 2);
        CHECK(detail_wall_tool(config, config, 3).filament_id_1based == 2);
        CHECK(detail_wall_tool(config, config, 4).filament_id_1based == 2);
    }
}

TEST_CASE("Unset toolhead widths preserve process widths", "[Flow][MultiNozzleWalls]")
{
    FullPrintConfig config;
    const ConfigOptionFloatOrPercent process_width(0.42, false);
    ConfigOptionFloatOrPercent resolved = toolhead_line_width_or(config, frExternalPerimeter, 1, false, process_width);
    CHECK(resolved.value == 0.42);
    CHECK_FALSE(resolved.percent);

    config.toolhead_outer_wall_line_width.values = { FloatOrPercent(0.31, false) };
    resolved = toolhead_line_width_or(config, frExternalPerimeter, 1, false, process_width);
    CHECK(resolved.value == 0.31);
    CHECK_FALSE(resolved.percent);
}
