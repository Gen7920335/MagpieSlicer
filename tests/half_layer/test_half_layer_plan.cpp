#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/HalfLayerPlan.hpp"
#include "libslic3r/LayerBands.hpp"

using namespace Slic3r;

TEST_CASE("Half layers preserve physical bounds and parent identity", "[HalfLayer][Z]")
{
    const double height = GENERATE(0.04, 0.1, 0.15, 0.2, 0.3, 0.6);
    const double bottom = GENERATE(0., 0.2, 1.73, 100.1);
    const auto bands = split_model_layer_half(17, bottom, bottom + height);
    REQUIRE(bands[0].model_layer_id == 17);
    REQUIRE(bands[1].model_layer_id == 17);
    CHECK(bands[0].sublayer == 0);
    CHECK(bands[1].sublayer == 1);
    CHECK(bands[0].bottom_z_mm == bottom);
    CHECK(bands[0].print_z_mm == bands[1].bottom_z_mm);
    CHECK(bands[1].print_z_mm == bottom + height);
    CHECK(bands[0].height_mm() == Catch::Approx(height / 2.));
    CHECK(bands[1].height_mm() == Catch::Approx(height / 2.));
    CHECK(bands[0].slice_z_mm() == Catch::Approx(bottom + height / 4.));
    CHECK(bands[1].slice_z_mm() == Catch::Approx(bottom + 3. * height / 4.));
}

TEST_CASE("Adaptive half layers never drift across parent boundaries", "[HalfLayer][Z]")
{
    const std::vector<double> boundaries{0., 0.3, 0.43, 0.67, 0.71, 1.0};
    double last = boundaries.front();
    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const auto bands = split_model_layer_half(i, boundaries[i], boundaries[i + 1]);
        REQUIRE(bands[0].bottom_z_mm == last);
        REQUIRE(bands[0].print_z_mm == bands[1].bottom_z_mm);
        last = bands[1].print_z_mm;
    }
    CHECK(last == boundaries.back());
    CHECK_THROWS_AS(split_model_layer_half(0, 0., 0.), std::invalid_argument);
    CHECK_THROWS_AS(split_model_layer_half(0, 1., 0.9), std::invalid_argument);
    CHECK_THROWS_AS(split_model_layer_half(0, 0., INFINITY), std::invalid_argument);
    CHECK_THROWS_AS(split_model_layer_half(0, NAN, 1.), std::invalid_argument);
}

TEST_CASE("Half-layer roles preserve effective XY count", "[HalfLayer][Walls]")
{
    const int walls = GENERATE(0, 1, 2, 3, 4, 7, 12);
    const auto off = half_layer_wall_counts(walls, false);
    const auto on = half_layer_wall_counts(walls, true);
    CHECK(on.outer + on.inner == walls);
    CHECK(off.outer + off.inner == walls);
    CHECK(on.outer == std::min(walls, 2));
    CHECK(off.outer == std::min(walls, 1));
    CHECK_FALSE(is_half_layer_outer_wall(0, false));
    CHECK(is_half_layer_outer_wall(0, true));
    CHECK(is_half_layer_outer_wall(1, true));
    CHECK_FALSE(is_half_layer_outer_wall(2, true));
}

TEST_CASE("Band travel lift uses the parent height without adding to the configured hop", "[HalfLayer][Hop]")
{
    // H/2's plan lifts at least 1.5 model layer heights; 0 keeps the configured hop.
    const double lift_layers = GENERATE(0., 1.5);
    const double configured = GENERATE(0., 0.1, 0.2999, 0.3, 0.3001, 0.8);
    const double expected = lift_layers > 0. ? std::max(configured, 0.3) : configured;
    CHECK(band_travel_lift_mm(configured, 0.2, lift_layers) == Catch::Approx(expected));
    CHECK_THROWS_AS(band_travel_lift_mm(-0.1, 0.2, lift_layers), std::invalid_argument);
}

TEST_CASE("Half-layer path time divides length by speed", "[HalfLayer][Time]")
{
    CHECK(half_layer_path_seconds(120., 30.) == 4.);
    CHECK_THROWS_AS(half_layer_path_seconds(120., 0.), std::invalid_argument);
}

TEST_CASE("Half-layer tool visits retain returns and shared-nozzle material changes", "[HalfLayer][Tools]")
{
    std::vector<HalfLayerToolVisit> visits;
    append_half_layer_tool_visit(visits, {0, 0});
    append_half_layer_tool_visit(visits, {0, 0});
    append_half_layer_tool_visit(visits, {1, 1});
    append_half_layer_tool_visit(visits, {0, 0});
    append_half_layer_tool_visit(visits, {0, 2});
    REQUIRE(visits.size() == 4);
    CHECK(visits[0].physical_tool == 0);
    CHECK(visits[1].physical_tool == 1);
    CHECK(visits[2].physical_tool == 0);
    CHECK(visits[3].physical_tool == 0);
    CHECK(visits[3].filament == 2);
}
