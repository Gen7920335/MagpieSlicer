#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectConfigService.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <array>
#include <limits>
#include <sstream>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/archives/binary.hpp>

using namespace Slic3r;

TEST_CASE("Tower Undo archive restores all plate coordinates and optional states",
          "[ProjectConfigService][TowerUndo]")
{
    const int optional_state = GENERATE(0, 1, 2, 3, 4, 5);
    const int deleted_plate = GENERATE(-1, 0, 1, 2);
    CAPTURE(optional_state, deleted_plate);
    DynamicPrintConfig original;
    original.set_key_value("wipe_tower_x", new ConfigOptionFloats{10., 20., 30.});
    original.set_key_value("wipe_tower_y", new ConfigOptionFloats{110., 120., 130.});
    original.set_key_value("wipe_tower_rotation_angle", new ConfigOptionFloat(0.));
    original.set_key_value("wall_loops", new ConfigOptionInt(7));
    const std::string tx = "support_interface_temperature_drop_tower_x";
    const std::string ty = "support_interface_temperature_drop_tower_y";
    if (optional_state == 1) {
        original.set_key_value(tx, new ConfigOptionFloats(std::vector<double>{}));
        original.set_key_value(ty, new ConfigOptionFloats(std::vector<double>{}));
    } else if (optional_state >= 2) {
        if (optional_state != 5)
            original.set_key_value(tx, new ConfigOptionFloats(optional_state == 3 ? std::vector<double>{-1., 65.} : std::vector<double>{-1., 65., 85.}));
        if (optional_state != 4)
            original.set_key_value(ty, new ConfigOptionFloats(optional_state == 3 ? std::vector<double>{75.} : std::vector<double>{-1., 75., 95.}));
    }
    Model saved;
    DynamicPrintConfig stale = original;
    stale.set_key_value(tx, new ConfigOptionFloats{250.});
    stale.set_key_value(ty, new ConfigOptionFloats{260.});
    capture_project_tower_positions(stale, saved.wipe_tower);
    capture_project_tower_positions(original, saved.wipe_tower);
    std::stringstream bytes;
    { cereal::BinaryOutputArchive archive(bytes); archive(saved.wipe_tower); }
    Model loaded;
    { cereal::BinaryInputArchive archive(bytes); archive(loaded.wipe_tower); }
    DynamicPrintConfig changed = original;
    if (deleted_plate >= 0) {
        for (const char *key : {"wipe_tower_x", "wipe_tower_y"}) {
            auto &values = changed.option<ConfigOptionFloats>(key)->values;
            values.erase(values.begin() + deleted_plate);
        }
        erase_temperature_drop_tower_plate(changed, size_t(deleted_plate), 3);
    } else {
        changed.set_key_value("wipe_tower_x", new ConfigOptionFloats{150., 160., 170.});
        changed.set_key_value("wipe_tower_y", new ConfigOptionFloats{180., 190., 200.});
        changed.set_key_value(tx, new ConfigOptionFloats{15., 25., 35.});
        changed.set_key_value(ty, new ConfigOptionFloats{45., 55., 65.});
    }
    changed.set_key_value("wall_loops", new ConfigOptionInt(9));
    const DynamicPrintConfig expected_redo = changed;
    Model redo_saved;
    capture_project_tower_positions(changed, redo_saved.wipe_tower);
    std::stringstream redo_bytes;
    { cereal::BinaryOutputArchive archive(redo_bytes); archive(redo_saved.wipe_tower); }
    Model redo_loaded;
    { cereal::BinaryInputArchive archive(redo_bytes); archive(redo_loaded.wipe_tower); }
    CHECK(restore_project_tower_positions(loaded.wipe_tower, changed));
    for (const std::string &key : {std::string("wipe_tower_x"), std::string("wipe_tower_y"), tx, ty}) {
        CAPTURE(key);
        CHECK(changed.has(key) == original.has(key));
        if (original.has(key)) {
            REQUIRE(changed.has(key));
            CHECK(*changed.option(key) == *original.option(key));
        }
    }
    CHECK(changed.opt_int("wall_loops") == 9);
    CHECK_FALSE(restore_project_tower_positions(loaded.wipe_tower, changed));
    CHECK(restore_project_tower_positions(redo_loaded.wipe_tower, changed));
    CHECK(changed == expected_redo);
    CHECK_FALSE(restore_project_tower_positions(redo_loaded.wipe_tower, changed));
    CHECK(restore_project_tower_positions(loaded.wipe_tower, changed));
}

TEST_CASE("Deleting a filament preserves every custom tool change in order",
          "[ProjectConfigService][FilamentDeletion]")
{
    const int replacement = GENERATE(-1, 0, 1, 2);
    Model model;
    const std::vector<CustomGCode::Item> original {
        {0.2, CustomGCode::ToolChange, 2, "a", "first"},
        {0.4, CustomGCode::PausePrint, 2, "pause", "message"},
        {0.6, CustomGCode::ToolChange, 1, "b", "second"},
        {0.8, CustomGCode::ToolChange, 2, "c", "third"},
        {1.0, CustomGCode::Custom, 2, "", "M117 unchanged"},
        {1.2, CustomGCode::ToolChange, 4, "d", "fourth"},
        {1.4, CustomGCode::ToolChange, 3, "e", "fifth"}
    };
    for (int plate : {0, 2})
        model.plates_custom_gcodes[plate].gcodes = original;

    remap_model_tool_changes_after_filament_delete(model, 1, replacement);

    // Explicit expected sequence: no compacted tail, duplicate, lost pause or extra decrement.
    auto expected = original;
    expected[5].extruder = 3;
    expected[6].extruder = 2;
    if (replacement < 0) {
        expected.erase(expected.begin() + 3);
        expected.erase(expected.begin());
    } else {
        expected[0].extruder = replacement + 1;
        expected[3].extruder = replacement + 1;
    }
    for (int plate : {0, 2})
        CHECK(model.plates_custom_gcodes[plate].gcodes == expected);
}

TEST_CASE("Deleting a filament remaps every role at every configuration scope",
          "[ProjectConfigService][FilamentDeletion]")
{
    const int deleted = GENERATE(0, 1, 2, 3);
    const int replacement = GENERATE(-1, 0, 1, 2);
    const int old_id = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(deleted, replacement, old_id);
    const std::array<const char *, 12> keys {
        "extruder", "wall_filament", "sparse_infill_filament", "solid_infill_filament",
        "support_filament", "support_interface_filament", "outer_wall_filament_id", "inner_wall_filament_id",
        "sparse_infill_filament_id", "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"
    };
    DynamicPrintConfig global;
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh{});
    auto &layer = object->layer_config_ranges[{0.4, 1.2}];
    for (const char *key : keys) {
        global.set_key_value(key, new ConfigOptionInt(old_id));
        object->config.set_key_value(key, new ConfigOptionInt(old_id));
        volume->config.set_key_value(key, new ConfigOptionInt(old_id));
        layer.set_key_value(key, new ConfigOptionInt(old_id));
    }
    object->config.set("wall_loops", 7);
    const auto timestamp = layer.timestamp();
    remap_filament_assignments_after_delete(global, deleted, replacement);
    remap_model_filament_assignments_after_delete(model, 3, deleted, replacement);
    const bool removed = old_id == deleted + 1 && replacement < 0;
    const int expected = old_id == deleted + 1 ? (replacement < 0 ? 0 : replacement + 1) :
        (old_id > deleted + 1 ? old_id - 1 : old_id);
    for (const char *key : keys) {
        CAPTURE(key);
        CHECK(global.opt_int(key) == expected);
        if (removed) {
            if (std::string(key) == "extruder")
                CHECK(object->config.extruder() == 1);
            else
                CHECK_FALSE(object->config.has(key));
            CHECK_FALSE(volume->config.has(key));
            CHECK_FALSE(layer.has(key));
        } else {
            CHECK(object->config.opt_int(key) == expected);
            CHECK(volume->config.opt_int(key) == expected);
            CHECK(layer.opt_int(key) == expected);
        }
    }
    CHECK(object->config.opt_int("wall_loops") == 7);
    CHECK((layer.timestamp() != timestamp) == (old_id >= deleted + 1));
}

TEST_CASE("Filament deletion preserves absent and inherited overrides",
          "[ProjectConfigService][FilamentDeletion]")
{
    Model model;
    auto *object = model.add_object();
    auto *volume = object->add_volume(TriangleMesh{});
    auto &layer = object->layer_config_ranges[{0.2, 0.8}];
    layer.set("support_filament", 0);
    const auto timestamp = layer.timestamp();
    remap_model_filament_assignments_after_delete(model, 3, 1, 2);
    CHECK(object->config.empty());
    CHECK(volume->config.empty());
    CHECK(layer.size() == 1);
    CHECK(layer.opt_int("support_filament") == 0);
    CHECK(layer.timestamp() == timestamp);
}

TEST_CASE("Legacy physical tool enums are safe and preserve indexed values", "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;
    config.set_deserialize_strict("extruder_type", "Bowden");
    config.set_deserialize_strict("nozzle_volume_type", "High Flow");
    REQUIRE(normalize_project_tool_enums(config, 4).size() == 2);
    CHECK(config.option<ConfigOptionEnumsGeneric>("extruder_type")->values == std::vector<int>{1, 0, 0, 0});
    CHECK(config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values == std::vector<int>{1, 0, 0, 0});
    CHECK(normalize_project_tool_enums(config, 4).empty());
    config.option<ConfigOptionEnumsGeneric>("extruder_type")->values = {-1, 99};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values.clear();
    normalize_project_tool_enums(config, 2);
    CHECK(config.option<ConfigOptionEnumsGeneric>("extruder_type")->values == std::vector<int>{0, 0});
    CHECK(config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values == std::vector<int>{0, 0});
    CHECK(get_extruder_variant_string(ExtruderType(-1), nvtStandard).empty());
    CHECK(get_extruder_variant_string(etDirectDrive, NozzleVolumeType(-1)).empty());
}

TEST_CASE("Temperature drop tower plate vectors are normalized together", "[ProjectConfigService]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, 20.0}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({30.0, std::numeric_limits<double>::quiet_NaN(), 50.0, 60.0}));

    const std::vector<std::string> repaired =
        normalize_temperature_drop_tower_positions(config, 4);
    const auto &x = config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values;
    const auto &y = config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values;

    REQUIRE(repaired.size() == 2);
    REQUIRE(x.size() == 4);
    REQUIRE(y.size() == 4);
    CHECK(x == std::vector<double>{10.0, 20.0, -1.0, -1.0});
    CHECK(y == std::vector<double>{30.0, -1.0, 50.0, 60.0});
}

TEST_CASE("Temperature drop tower positions survive plate deletion by index", "[ProjectConfigService]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, 20.0, 30.0}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({40.0, 50.0, 60.0}));

    erase_temperature_drop_tower_plate(config, 1, 3);

    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values == std::vector<double>{10.0, 30.0});
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values == std::vector<double>{40.0, 60.0});
}

TEST_CASE("Temperature drop tower follows an instance to an automatic destination plate",
          "[ProjectConfigService][TemperatureDropTower]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, -1.0, 70.0}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({20.0, -1.0, 80.0}));

    SECTION("moving the final source instance moves the manual position") {
        transfer_temperature_drop_tower_plate(config, 0, 1, 3, true);
        CHECK(config.option<ConfigOptionFloats>(
            "support_interface_temperature_drop_tower_x")->values ==
            std::vector<double>{-1.0, 10.0, 70.0});
        CHECK(config.option<ConfigOptionFloats>(
            "support_interface_temperature_drop_tower_y")->values ==
            std::vector<double>{-1.0, 20.0, 80.0});
    }

    SECTION("a source plate with other instances keeps its position") {
        transfer_temperature_drop_tower_plate(config, 0, 1, 3, false);
        CHECK(config.option<ConfigOptionFloats>(
            "support_interface_temperature_drop_tower_x")->values ==
            std::vector<double>{10.0, 10.0, 70.0});
        CHECK(config.option<ConfigOptionFloats>(
            "support_interface_temperature_drop_tower_y")->values ==
            std::vector<double>{20.0, 20.0, 80.0});
    }
}

TEST_CASE("Temperature drop tower transfer preserves an existing destination position",
          "[ProjectConfigService][TemperatureDropTower]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, 30.0}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({20.0, 40.0}));

    transfer_temperature_drop_tower_plate(config, 0, 1, 2, true);

    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values ==
        std::vector<double>{-1.0, 30.0});
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values ==
        std::vector<double>{-1.0, 40.0});
}

TEST_CASE("Temperature drop tower reads are safe for legacy short vectors", "[ProjectConfigService]")
{
    const ConfigOptionFloats positions({12.5});
    CHECK(temperature_drop_tower_position_at(&positions, 0) == 12.5);
    CHECK(temperature_drop_tower_position_at(&positions, 4) == -1.0);
    CHECK(temperature_drop_tower_position_at(nullptr, 0) == -1.0);
}

TEST_CASE("Legacy project filament maps are repaired against loaded tools",
          "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts({2, 0, 9}));

    const std::vector<std::string> repaired =
        normalize_project_filament_map(config, 4, 3);

    CHECK(repaired == std::vector<std::string>{"filament_map"});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values ==
          std::vector<int>{2, 2, 3, 3});
}

TEST_CASE("Missing legacy project filament maps receive deterministic defaults",
          "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;

    const std::vector<std::string> repaired =
        normalize_project_filament_map(config, 3, 1);

    CHECK(repaired == std::vector<std::string>{"filament_map"});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values ==
          std::vector<int>{1, 1, 1});
}

TEST_CASE("Valid project filament maps survive compatibility normalization",
          "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts({4, 1, 3, 2}));

    const std::vector<std::string> repaired =
        normalize_project_filament_map(config, 4, 4);

    CHECK(repaired.empty());
    CHECK(config.option<ConfigOptionInts>("filament_map")->values ==
          std::vector<int>{4, 1, 3, 2});
}

TEST_CASE("Loaded project normalization repairs all project-owned compatibility data once",
          "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, std::numeric_limits<double>::infinity()}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({20.0}));
    config.set_key_value("filament_map", new ConfigOptionInts({0, 9}));

    ProjectConfigNormalizationContext context;
    context.plate_count = 3;
    context.filament_count = 4;
    context.toolhead_count = 2;
    const std::vector<std::string> repaired = normalize_loaded_project_config(config, context);

    CHECK(repaired == std::vector<std::string>{
        "support_interface_temperature_drop_tower_x",
        "support_interface_temperature_drop_tower_y",
        "filament_map"});
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values ==
        std::vector<double>{10.0, -1.0, -1.0});
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values ==
        std::vector<double>{20.0, -1.0, -1.0});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values ==
        std::vector<int>{1, 2, 2, 2});

    CHECK(normalize_loaded_project_config(config, context).empty());
}

TEST_CASE("Loaded project normalization preserves valid settings",
          "[ProjectConfigService][Compatibility]")
{
    DynamicPrintConfig config;
    config.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0, 30.0}));
    config.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({20.0, 40.0}));
    config.set_key_value("filament_map", new ConfigOptionInts({2, 1}));

    ProjectConfigNormalizationContext context;
    context.plate_count = 2;
    context.filament_count = 2;
    context.toolhead_count = 2;
    const std::vector<std::string> repaired = normalize_loaded_project_config(config, context);

    CHECK(repaired.empty());
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values ==
        std::vector<double>{10.0, 30.0});
    CHECK(config.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values ==
        std::vector<double>{20.0, 40.0});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values ==
        std::vector<int>{2, 1});
}

TEST_CASE("Project save normalization returns a repaired snapshot without mutating its source",
          "[ProjectConfigService][Compatibility][Save]")
{
    DynamicPrintConfig source;
    source.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({10.0}));
    source.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({20.0, 30.0, 40.0}));
    source.set_key_value("filament_map", new ConfigOptionInts({0, 4, 2}));

    ProjectConfigNormalizationContext context;
    context.plate_count = 2;
    context.filament_count = 3;
    context.toolhead_count = 2;
    const DynamicPrintConfig normalized = normalized_project_config_for_save(source, context);

    CHECK(source.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values == std::vector<double>{10.0});
    CHECK(source.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values ==
        std::vector<double>{20.0, 30.0, 40.0});
    CHECK(source.option<ConfigOptionInts>("filament_map")->values ==
        std::vector<int>{0, 4, 2});

    CHECK(normalized.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values ==
        std::vector<double>{10.0, -1.0});
    CHECK(normalized.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values ==
        std::vector<double>{20.0, 30.0});
    CHECK(normalized.option<ConfigOptionInts>("filament_map")->values ==
        std::vector<int>{1, 2, 2});
}

TEST_CASE("Project save normalization follows serialization boundary counts",
          "[ProjectConfigService][Compatibility][Save]")
{
    const size_t count = GENERATE(1u, 2u, 4u, 8u);
    DynamicPrintConfig source;
    source.set_key_value("support_interface_temperature_drop_tower_x",
        new ConfigOptionFloats({-1.0}));
    source.set_key_value("support_interface_temperature_drop_tower_y",
        new ConfigOptionFloats({-1.0}));
    source.set_key_value("filament_map", new ConfigOptionInts({99}));

    ProjectConfigNormalizationContext context;
    context.plate_count = count;
    context.filament_count = count;
    context.toolhead_count = count;
    const DynamicPrintConfig normalized = normalized_project_config_for_save(source, context);

    CHECK(normalized.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x")->values.size() == count);
    CHECK(normalized.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y")->values.size() == count);
    REQUIRE(normalized.option<ConfigOptionInts>("filament_map")->values.size() == count);
    for (size_t index = 0; index < count; ++index)
        CHECK(normalized.option<ConfigOptionInts>("filament_map")->values[index] == int(index + 1));
}

TEST_CASE("Optional plate filament maps preserve inheritance and repair explicit overrides",
          "[ProjectConfigService][Compatibility][Plate]")
{
    std::vector<int> inherited;
    CHECK_FALSE(normalize_optional_filament_map(inherited, 4, 2));
    CHECK(inherited.empty());

    std::vector<int> override_map {0, 4};
    CHECK(normalize_optional_filament_map(override_map, 4, 2));
    CHECK(override_map == std::vector<int>{1, 2, 2, 2});
    CHECK_FALSE(normalize_optional_filament_map(override_map, 4, 2));
}

TEST_CASE("Project normalization context prefers preset identity over stale map lengths",
          "[ProjectConfigService][Compatibility][Plate]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_settings_id", new ConfigOptionStrings({"A", "B", "C"}));
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#1", "#2"}));
    config.set_key_value("filament_map", new ConfigOptionInts({1, 2, 3, 4, 5}));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.2}));

    const ProjectConfigNormalizationContext context =
        infer_project_config_normalization_context(config, 4);
    CHECK(context.plate_count == 4);
    CHECK(context.filament_count == 3);
    CHECK(context.toolhead_count == 2);
}

TEST_CASE("Filament-indexed project arrays normalize across boundary counts",
          "[ProjectConfigService][Compatibility][Arrays]")
{
    const size_t count = GENERATE(1u, 2u, 4u, 8u);
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#111111"}));
    config.set_key_value("filament_colour_type", new ConfigOptionStrings({"1"}));
    config.set_key_value("filament_multi_colour", new ConfigOptionStrings({"#111111"}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({100.0, 120.0}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0.0}));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({0.5}));

    normalize_project_filament_arrays(config, count, count);

    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values.size() == count);
    CHECK(config.option<ConfigOptionStrings>("filament_colour_type")->values.size() == count);
    CHECK(config.option<ConfigOptionStrings>("filament_multi_colour")->values.size() == count);
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values.size() == 2 * count);
    CHECK(config.option<ConfigOptionFloats>("flush_multiplier")->values.size() == count);
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == count * count * count);
    CHECK(normalize_project_filament_arrays(config, count, count).empty());
}

TEST_CASE("Logical filament assignments preserve defaults and clear only invalid ids",
          "[ProjectConfigService][Compatibility][Assignments]")
{
    DynamicPrintConfig config;
    config.set_key_value("extruder", new ConfigOptionInt(0));
    config.set_key_value("wall_filament", new ConfigOptionInt(2));
    config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(8));
    config.set_key_value("support_filament", new ConfigOptionInt(9));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(-1));

    const std::vector<std::string> repaired = normalize_filament_assignment_options(config, 4);

    CHECK(config.option<ConfigOptionInt>("extruder")->value == 0);
    CHECK(config.option<ConfigOptionInt>("wall_filament")->value == 2);
    CHECK(config.option<ConfigOptionInt>("outer_wall_filament_id")->value == 0);
    CHECK(config.option<ConfigOptionInt>("support_filament")->value == 0);
    CHECK(config.option<ConfigOptionInt>("support_interface_filament")->value == 0);
    CHECK(repaired == std::vector<std::string>{
        "support_filament", "support_interface_filament", "outer_wall_filament_id"});
}

TEST_CASE("Project filament array normalization preserves overlapping flush values",
          "[ProjectConfigService][Compatibility][Arrays]")
{
    DynamicPrintConfig config;
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({10.0, 20.0, 30.0, 40.0}));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({0.5, 0.75}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({
        0.0, 11.0, 12.0, 0.0,
        0.0, 21.0, 22.0, 0.0
    }));

    normalize_project_filament_arrays(config, 3, 3);

    const auto &vector = config.option<ConfigOptionFloats>("flush_volumes_vector")->values;
    const auto &matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    CHECK(vector == std::vector<double>{10.0, 20.0, 30.0, 40.0, 10.0, 20.0});
    CHECK(matrix.size() == 27);
    CHECK(matrix[1] == 11.0);
    CHECK(matrix[3] == 12.0);
    CHECK(matrix[10] == 21.0);
    CHECK(matrix[12] == 22.0);
    CHECK(matrix[5] == 50.0);
    CHECK(matrix[6] == 30.0);
}

TEST_CASE("Model filament assignments normalize object and volume configuration",
          "[ProjectConfigService][Compatibility][Assignments][Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh{});
    object->config.set("extruder", 9);
    object->config.set("support_filament", 2);
    volume->config.set("extruder", 3);
    volume->config.set("outer_wall_filament_id", 8);

    const size_t repaired = normalize_model_filament_assignments(model, 4);

    CHECK(repaired == 3);
    CHECK_FALSE(object->config.has("extruder"));
    CHECK(object->config.opt_int("support_filament") == 2);
    CHECK_FALSE(volume->config.has("extruder"));
    CHECK(volume->config.opt_int("outer_wall_filament_id") == 0);
    CHECK(normalize_model_filament_assignments(model, 4) == 0);
}

TEST_CASE("Plate slice metadata rejects every stale filament and tool index source",
          "[ProjectConfigService][Compatibility][Plate][Metadata]")
{
    const int invalid_source = GENERATE(range(0, 7));
    ProjectConfigNormalizationContext context;
    context.filament_count = 2;
    context.toolhead_count = 2;

    PlateData plate;
    plate.is_sliced_valid = true;
    plate.filament_change_sequence = {0u, 1u};
    plate.nozzle_change_sequence = {0u, 1u};
    plate.optimal_assignment = {0, 1};
    plate.limit_filament_maps = {1, 2};
    plate.slice_filaments_info = {FilamentInfo{}, FilamentInfo{}};
    plate.slice_filaments_info[0].id = 0;
    plate.slice_filaments_info[1].id = 1;
    plate.layer_filaments[std::vector<unsigned int>{0u, 1u}] = {{0, 0}};

    bool force = false;
    switch (invalid_source) {
    case 0: plate.filament_change_sequence.push_back(2u); break;
    case 1: plate.nozzle_change_sequence.push_back(2u); break;
    case 2: plate.limit_filament_maps[0] = 4; break;
    case 3: plate.slice_filaments_info[0].id = -1; break;
    case 4: plate.optimal_assignment[0] = 2; break;
    case 5:
        plate.layer_filaments.clear();
        plate.layer_filaments[std::vector<unsigned int>{2u}] = {{0, 0}};
        break;
    case 6: force = true; break;
    default: FAIL("Unhandled generated metadata source");
    }

    CHECK(normalize_plate_slice_metadata(plate, context, force));
    CHECK_FALSE(plate.is_sliced_valid);
    CHECK(plate.filament_change_sequence.empty());
    CHECK(plate.nozzle_change_sequence.empty());
    CHECK(plate.optimal_assignment.empty());
    CHECK(plate.limit_filament_maps.empty());
    CHECK(plate.slice_filaments_info.empty());
    CHECK(plate.layer_filaments.empty());
}

TEST_CASE("Compatible plate slice metadata remains intact",
          "[ProjectConfigService][Compatibility][Plate][Metadata]")
{
    ProjectConfigNormalizationContext context;
    context.filament_count = 2;
    context.toolhead_count = 2;

    PlateData plate;
    plate.is_sliced_valid = true;
    plate.filament_change_sequence = {0u, 1u};
    plate.nozzle_change_sequence = {0u, 1u};
    plate.optimal_assignment = {0, 1};
    plate.limit_filament_maps = {1, 2};
    plate.slice_filaments_info = {FilamentInfo{}, FilamentInfo{}};
    plate.slice_filaments_info[0].id = 0;
    plate.slice_filaments_info[1].id = 1;
    plate.layer_filaments[std::vector<unsigned int>{0u, 1u}] = {{0, 0}};

    CHECK_FALSE(normalize_plate_slice_metadata(plate, context));
    CHECK(plate.is_sliced_valid);
    CHECK(plate.filament_change_sequence == std::vector<unsigned int>{0u, 1u});
    CHECK(plate.nozzle_change_sequence == std::vector<unsigned int>{0u, 1u});
    CHECK(plate.optimal_assignment == std::vector<int>{0, 1});
    CHECK(plate.limit_filament_maps == std::vector<int>{1, 2});
    CHECK(plate.slice_filaments_info.size() == 2);
    CHECK(plate.layer_filaments.size() == 1);
}

TEST_CASE("Project filament arrays satisfy size and idempotence properties",
          "[ProjectConfigService][Compatibility][Arrays][Stress]")
{
    for (size_t filament_count = 1; filament_count <= 12; ++filament_count) {
        for (size_t toolhead_count = 1; toolhead_count <= 8; ++toolhead_count) {
            INFO("filaments=" << filament_count << " tools=" << toolhead_count);
            DynamicPrintConfig config;
            config.set_key_value("filament_colour",
                new ConfigOptionStrings(std::vector<std::string>(filament_count + 2, "#123456")));
            config.set_key_value("filament_colour_type",
                new ConfigOptionStrings(std::vector<std::string>(filament_count + 1, "1")));
            config.set_key_value("filament_multi_colour",
                new ConfigOptionStrings(std::vector<std::string>(filament_count + 3, "#654321")));
            config.set_key_value("flush_volumes_vector",
                new ConfigOptionFloats(std::vector<double>(2 * filament_count + 3, 125.0)));
            config.set_key_value("flush_multiplier",
                new ConfigOptionFloats(std::vector<double>(toolhead_count + 2, 0.75)));
            config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({17.0, 23.0, 31.0}));

            const std::vector<std::string> repaired = normalize_project_filament_arrays(
                config, filament_count, toolhead_count);

            CHECK_FALSE(repaired.empty());
            CHECK(config.option<ConfigOptionStrings>("filament_colour")->values.size() == filament_count);
            CHECK(config.option<ConfigOptionStrings>("filament_colour_type")->values.size() == filament_count);
            CHECK(config.option<ConfigOptionStrings>("filament_multi_colour")->values.size() == filament_count);
            CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values.size() == 2 * filament_count);
            CHECK(config.option<ConfigOptionFloats>("flush_multiplier")->values.size() == toolhead_count);
            CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() ==
                filament_count * filament_count * toolhead_count);
            CHECK(normalize_project_filament_arrays(config, filament_count, toolhead_count).empty());
        }
    }
}

TEST_CASE("Every logical filament assignment key obeys default and boundary ids",
          "[ProjectConfigService][Compatibility][Assignments][Stress]")
{
    static constexpr std::array<const char *, 11> assignment_keys {
        "wall_filament", "sparse_infill_filament", "solid_infill_filament",
        "support_filament", "support_interface_filament",
        "outer_wall_filament_id", "inner_wall_filament_id",
        "sparse_infill_filament_id", "internal_solid_filament_id",
        "top_surface_filament_id", "bottom_surface_filament_id"
    };

    for (const size_t filament_count : {1u, 2u, 4u, 8u}) {
        for (const char *key : assignment_keys) {
            for (const int value : {-3, -1, 0, 1, int(filament_count), int(filament_count + 1)}) {
                INFO("key=" << key << " value=" << value << " filaments=" << filament_count);
                DynamicPrintConfig config;
                config.set_key_value(key, new ConfigOptionInt(value));
                const bool valid = value >= 0 && size_t(value) <= filament_count;

                const std::vector<std::string> repaired =
                    normalize_filament_assignment_options(config, filament_count);

                CHECK(config.option<ConfigOptionInt>(key)->value == (valid ? value : 0));
                CHECK(repaired.empty() == valid);
                CHECK(normalize_filament_assignment_options(config, filament_count).empty());
            }
        }
    }
}

TEST_CASE("Compatible plate metadata survives filament and tool boundary cross product",
          "[ProjectConfigService][Compatibility][Plate][Metadata][Stress]")
{
    for (const size_t filament_count : {1u, 2u, 4u, 8u}) {
        for (const size_t toolhead_count : {1u, 2u, 4u, 8u}) {
            INFO("filaments=" << filament_count << " tools=" << toolhead_count);
            ProjectConfigNormalizationContext context;
            context.filament_count = filament_count;
            context.toolhead_count = toolhead_count;

            PlateData plate;
            plate.is_sliced_valid = true;
            plate.filament_change_sequence = {unsigned(filament_count - 1)};
            plate.nozzle_change_sequence = {unsigned(toolhead_count - 1)};
            plate.optimal_assignment = {int(toolhead_count - 1)};
            plate.limit_filament_maps.assign(filament_count, int((1u << toolhead_count) - 1u));
            plate.slice_filaments_info = {FilamentInfo{}};
            plate.slice_filaments_info.front().id = int(filament_count - 1);
            plate.layer_filaments[std::vector<unsigned int>{unsigned(filament_count - 1)}] = {{0, 0}};

            CHECK_FALSE(normalize_plate_slice_metadata(plate, context));
            CHECK(plate.is_sliced_valid);

            plate.nozzle_change_sequence.push_back(unsigned(toolhead_count));
            CHECK(normalize_plate_slice_metadata(plate, context));
            CHECK_FALSE(plate.is_sliced_valid);
        }
    }
}
