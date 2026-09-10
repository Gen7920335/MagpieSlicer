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

TEST_CASE("Layer height limits resolve material assignments to physical nozzles", "[LayerHeightMapping][ConfigurationOnly]")
{
    const bool permuted = GENERATE(false, true);
    const bool support = GENERATE(false, true);
    const int material = GENERATE(1, 2, 3, 4);
    CAPTURE(permuted, support, material);
    const std::vector<int> mapping = permuted ? std::vector<int>{4, 3, 1, 2} : std::vector<int>{1, 2, 3, 4};
    const std::vector<double> minimum_mm{0.02, 0.03, 0.04, 0.05};
    const std::vector<double> maximum_mm{0.12, 0.24, 0.36, 0.48};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75, 1.75});
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    config.set_key_value("filament_map", new ConfigOptionInts(mapping));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.2, 0.4, 0.6, 0.8});
    config.set_key_value("min_layer_height", new ConfigOptionFloats(minimum_mm));
    config.set_key_value("max_layer_height", new ConfigOptionFloats(maximum_mm));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("enable_support", new ConfigOptionBool(support));
    config.set_key_value("support_filament", new ConfigOptionInt(material));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(material));
    // With support, put the model on the smallest nozzle, making the separate
    // support maximum and the combined minimum independently observable.
    const int model_material = support ? int(std::find(mapping.begin(), mapping.end(), 1) - mapping.begin()) + 1 : material;
    for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                           "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"})
        config.set_key_value(key, new ConfigOptionInt(model_material));
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(10));
    object->add_instance();
    object->config.set_key_value("extruder", new ConfigOptionInt(model_material));
    PrintConfig effective;
    effective.apply(config, true);
    REQUIRE(effective.filament_diameter.size() == 4);
    REQUIRE(effective.filament_map.values == mapping);
    const auto parameters = PrintObject::slicing_parameters(config, *object, 10.f, Vec3d::Ones());
    const size_t physical_nozzle = size_t(mapping[material - 1] - 1);
    CHECK(parameters.min_layer_height == Catch::Approx(minimum_mm[physical_nozzle]));
    CHECK(parameters.max_layer_height == Catch::Approx(support ? maximum_mm.front() : maximum_mm[physical_nozzle]));
    if (support)
        CHECK(parameters.max_suport_layer_height == Catch::Approx(maximum_mm[physical_nozzle]));
    CHECK(parameters.layer_height == Catch::Approx(0.1));
}

TEST_CASE("Layer height mapping preserves defaults and shared nozzle limits", "[LayerHeightMapping][ConfigurationOnly]")
{
    const bool shared_nozzle = GENERATE(false, true);
    const bool automatic_limits = GENERATE(false, true);
    const int support_material = GENERATE(0, 1, 4);
    CAPTURE(shared_nozzle, automatic_limits, support_material);
    PrintConfig print_config;
    print_config.filament_diameter.values = {1.75, 1.75, 1.75, 1.75};
    print_config.filament_map.values = shared_nozzle ? std::vector<int>{1, 1, 1, 1} : std::vector<int>{4, 3, 1, 2};
    print_config.nozzle_diameter.values = shared_nozzle ? std::vector<double>{0.4} : std::vector<double>{0.2, 0.4, 0.6, 0.8};
    print_config.min_layer_height.values = automatic_limits ? std::vector<double>{0.0} : std::vector<double>{0.02};
    print_config.max_layer_height.values = automatic_limits ? std::vector<double>{0.0} : std::vector<double>{0.3};
    print_config.initial_layer_print_height.value = 0.1;
    PrintObjectConfig object_config;
    object_config.layer_height.value = 0.1;
    object_config.enable_support.value = true;
    object_config.support_filament.value = support_material;
    object_config.support_interface_filament.value = 0;
    // Model material 3 uses physical nozzle 1 in both mappings. Auto support
    // retains the existing nozzle-1 limit convention; no heterogeneous Auto
    // slicing is claimed by this configuration-only compatibility check.
    const auto parameters = SlicingParameters::create_from_config(print_config, object_config, 10., {2}, Vec3d::Ones());
    CHECK(parameters.min_layer_height == Catch::Approx(automatic_limits ? 0.07 : 0.02));
    CHECK(parameters.max_layer_height == Catch::Approx(automatic_limits && !shared_nozzle ? 0.15 : 0.3));
    CHECK(parameters.max_suport_layer_height == Catch::Approx(automatic_limits && !shared_nozzle ? 0.15 : 0.3));
    CHECK(parameters.layer_height == Catch::Approx(0.1));
}

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


#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include <boost/filesystem/operations.hpp>

TEST_CASE("3MF material-only height range inherits object layer height", "[LayerRangeInheritance][ConfigurationOnly]")
{
    Model source;
    auto *object = source.add_object();
    object->add_volume(Slic3r::Test::cube(10));
    object->add_instance();
    object->layer_config_ranges[{2., 8.}].set_key_value("extruder", new ConfigOptionInt(2));
    const auto output = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("magpie-range-height-%%%%-%%%%.3mf");
    const std::string output_string = output.string();
    DynamicPrintConfig source_config;
    std::set<std::pair<int, int>> object_instances{{0, 0}};
    PlateData source_plate(0, object_instances, false);
    StoreParams store;
    store.path = output_string.c_str();
    store.model = &source;
    store.plate_data_list = {&source_plate};
    store.config = &source_config;
    store.strategy = SaveStrategy::Silence | SaveStrategy::Zip64 | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store));
    Model restored;
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    bool is_bambu = false, is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(output_string.c_str(), &restored_config, &substitutions, &restored,
        &plates, &presets, &is_bambu, &is_orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances));
    release_PlateData_list(plates);
    for (Preset *preset : presets)
        delete preset;
    boost::filesystem::remove(output);
    REQUIRE(restored.objects.size() == 1);
    const auto &ranges = restored.objects.front()->layer_config_ranges;
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges.begin()->second.option("layer_height") == nullptr);
    REQUIRE(ranges.begin()->second.option("extruder")->getInt() == 2);
    SlicingParameters parameters;
    parameters.layer_height = 0.2;
    parameters.first_object_layer_height = 0.2;
    parameters.object_print_z_max = 10.;
    parameters.object_print_z_uncompensated_max = 10.;
    // The unmodified owner dereferenced null here after the real importer.
    CHECK(layer_height_profile_from_ranges(parameters, ranges) ==
          layer_height_profile_from_ranges(parameters, {}));
    CHECK(ranges.begin()->second.option("layer_height") == nullptr);
    CHECK(ranges.begin()->second.option("extruder")->getInt() == 2);
}

TEST_CASE("Missing range height preserves explicit neighboring ranges", "[LayerRangeInheritance][ConfigurationOnly]")
{
    const double base_height_mm = GENERATE(0.08, 0.2, 0.28);
    const bool raft = GENERATE(false, true);
    const bool overlap = GENERATE(false, true);
    SlicingParameters parameters;
    parameters.layer_height = base_height_mm;
    parameters.first_object_layer_height = 0.2;
    parameters.object_print_z_min = raft ? 1. : 0.;
    parameters.object_print_z_max = 10. + parameters.object_print_z_min;
    parameters.object_print_z_uncompensated_max = parameters.object_print_z_max;
    parameters.base_raft_layers = raft ? 1 : 0;
    t_layer_config_ranges partial;
    partial[{0., 1.}].set_key_value("extruder", new ConfigOptionInt(2));
    partial[{2., 4.}].set_key_value("layer_height", new ConfigOptionFloat(0.12));
    partial[{overlap ? 3. : 5., 7.}].set_key_value("wall_loops", new ConfigOptionInt(3));
    partial[{9., 12.}]; // Empty imported range still inherits the object setting.
    auto explicit_defaults = partial;
    for (auto &range : explicit_defaults)
        if (!range.second.has("layer_height"))
            range.second.set_key_value("layer_height", new ConfigOptionFloat(base_height_mm));
    const auto expected = layer_height_profile_from_ranges(parameters, explicit_defaults);
    const auto actual = layer_height_profile_from_ranges(parameters, partial);
    CHECK(actual == expected);
    REQUIRE(actual.size() % 2 == 0);
    for (size_t i = 0; i < actual.size(); i += 2) {
        CHECK(actual[i] >= 0.);
        CHECK(actual[i] <= 10.);
        CHECK(actual[i + 1] > 0.);
        if (i >= 2)
            CHECK(actual[i] >= actual[i - 2]);
    }
    CHECK(partial.begin()->second.option("layer_height") == nullptr);
}
