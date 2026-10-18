// MAGPIE: Magpie line 1 (Beta 7) fixes to Orca's config normalization and layer height ranges.

#include <catch2/catch_all.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>

#include <array>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Legacy Elegoo printer Z offset edits survive profile normalization", "[Config][Legacy]")
{
    for (const double z_offset : std::array<double, 4>{ -2.0, -0.3, 0.0, 2.0 }) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };
        config.set_key_value("extruder_variant_list", new ConfigOptionStrings());
        config.set_key_value("printer_extruder_id", new ConfigOptionInts());
        config.set_key_value("printer_extruder_variant", new ConfigOptionStrings());
        config.option<ConfigOptionFloat>("z_offset")->value = z_offset;

        DYNAMIC_SECTION("Z offset " << z_offset) {
            REQUIRE_NOTHROW(Preset::normalize(config));
            CHECK(config.option<ConfigOptionFloat>("z_offset")->value == Catch::Approx(z_offset));
            CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 1);
            CHECK(config.option<ConfigOptionStrings>("extruder_variant_list")->values.size() == 1);
            CHECK(config.option<ConfigOptionInts>("printer_extruder_id")->values.size() == 1);
            CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values.size() == 1);
        }
    }
}

TEST_CASE("Legacy printer vectors are normalized to the nozzle count", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.2, 0.6, 0.8 };
    config.set_key_value("extruder_variant_list", new ConfigOptionStrings());
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1 }));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({ "Direct Drive Standard" }));

    REQUIRE_NOTHROW(config.set_num_extruders(4));
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);
    CHECK(config.option<ConfigOptionStrings>("extruder_variant_list")->values.size() == 4);
    CHECK(config.option<ConfigOptionInts>("printer_extruder_id")->values.size() == 4);
    CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values.size() == 4);

    config.option<ConfigOptionFloats>("nozzle_diameter")->values.clear();
    REQUIRE_NOTHROW(Preset::normalize(config));
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 1);
    CHECK(config.option<ConfigOptionStrings>("extruder_variant_list")->values.size() == 1);
}

TEST_CASE("3MF material-only height range inherits object layer height", "[Slicing][LayerRange]")
{
    Model source;
    auto *object = source.add_object();
    object->add_volume(make_cube(10., 10., 10.));
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
    // Whether the importer leaves the height out is what decides if the owner below can crash.
    const bool importer_left_height_out = ranges.begin()->second.option("layer_height") == nullptr;
    CAPTURE(importer_left_height_out);
    REQUIRE(ranges.begin()->second.option("extruder")->getInt() == 2);
    if (importer_left_height_out) {
        SlicingParameters parameters;
        parameters.layer_height = 0.2;
        parameters.first_object_layer_height = 0.2;
        parameters.object_print_z_max = 10.;
        parameters.object_print_z_uncompensated_max = 10.;
        CHECK(layer_height_profile_from_ranges(parameters, ranges) ==
              layer_height_profile_from_ranges(parameters, {}));
    }
}

TEST_CASE("Missing range height preserves explicit neighboring ranges", "[Slicing][LayerRange]")
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
    partial[{9., 12.}];
    auto explicit_defaults = partial;
    for (auto &range : explicit_defaults)
        if (!range.second.has("layer_height"))
            range.second.set_key_value("layer_height", new ConfigOptionFloat(base_height_mm));
    const auto expected = layer_height_profile_from_ranges(parameters, explicit_defaults);
    const auto actual = layer_height_profile_from_ranges(parameters, partial);
    CHECK(actual == expected);
    CHECK(partial.begin()->second.option("layer_height") == nullptr);
}
