// MAGPIE: Magpie line 1 settings become mod parameters when a line 1 project or preset loads.
#include <catch2/catch_all.hpp>

#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>

#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// The bundled mods are the active set while a test runs; the previous set comes back afterwards.
struct BundledMods
{
    std::shared_ptr<const ModHost::ModSet> previous = ModHost::active_mods();
    BundledMods()
    {
        std::vector<std::string> errors;
        // The bundled JSON mods and the official native mods' manifests (Cura support is one).
        std::vector<fs::path> dirs{fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" / "mods"};
#ifdef MAGPIE_OFFICIAL_MODS_DIR
        dirs.emplace_back(MAGPIE_OFFICIAL_MODS_DIR);
#endif
        ModHost::set_active_mods(std::make_shared<const ModHost::ModSet>(ModHost::ModSet::load_dirs(dirs, errors)));
        REQUIRE(errors.empty());
    }
    ~BundledMods() { ModHost::set_active_mods(previous); }
};

// Loads `settings` as a process preset file, the way projects and presets are read.
DynamicPrintConfig load(const nlohmann::json &settings)
{
    ScopedTemporaryFile file(".json");
    nlohmann::json j = settings;
    j["type"] = "process";
    j["name"] = "line 1 test";
    j["from"] = "User";
    j["version"] = "2.5.0.0";
    {
        boost::nowide::ofstream out(file.string());
        out << j.dump();
    }
    DynamicPrintConfig config;
    std::map<std::string, std::string> key_values;
    std::string reason;
    config.load_from_json(file.string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
    return config;
}

nlohmann::json mod_settings(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionString>("mod_settings");
    return opt && !opt->value.empty() ? nlohmann::json::parse(opt->value) : nlohmann::json::object();
}

} // namespace

TEST_CASE("Line 1 support types load as Orca support with the generator's mod switched on", "[LegacySettings]")
{
    BundledMods mods;
    const auto [line1, orca, param] = GENERATE(table<const char *, const char *, const char *>({
        { "normal_cura(auto)", "normal(auto)", "cura_support.enable" },
        { "normal_cura(manual)", "normal(manual)", "cura_support.enable" },
        { "mixed(auto)", "normal(auto)", "mixed_support.enable" },
        { "resin(auto)", "normal(auto)", "resin_support.enable" },
    }));
    CAPTURE(line1);
    const DynamicPrintConfig config = load({ { "support_type", line1 } });
    CHECK(config.opt_serialize("support_type") == orca);
    CHECK(mod_settings(config)[param] == true);
}

TEST_CASE("Line 1 feature settings become mod parameters and line 1 only keys are dropped", "[LegacySettings]")
{
    BundledMods mods;
    const DynamicPrintConfig config = load({
        { "outer_wall_half_layer_height", "1" },
        { "single_nozzle_low_temperature_interface", "1" },
        { "support_interface_temperature", "175" },
        { "cura_support_join_distance", "3.5" },
        { "mixed_normal_support_generator", "cura" },
        { "mixed_normal_coverage_threshold", "80%" },
        { "resin_branching_support_pillar_diameter", "1.6" },
        { "resin_support_head_width", "1" }, // the default: not written
        { "support_interface_pattern", "triangles" },
        { "support_interface_sublayer_pattern", "1" },
        { "support_interface_sublayer_start_layer", "2" },
        { "support_interface_sublayer_end_layer", "3" },
        { "support_interface_sublayer_pattern_type", "grid" },
        { "support_wall_count", "2" },
    });
    const nlohmann::json m = mod_settings(config);
    CHECK(m["half_layer.outer_walls"] == true);
    CHECK(m["low_temp_interface.enable"] == true);
    CHECK(m["low_temp_interface.temperature"] == 175);
    CHECK(m["cura_support.join_distance"] == 3.5);
    CHECK(m["mixed_support.normal_generator"] == "cura");
    CHECK(m["mixed_support.coverage_threshold"] == 80.);
    CHECK(m["resin_support.branching_pillar_diameter"] == 1.6);
    CHECK_FALSE(m.contains("resin_support.default_head_width"));
    CHECK(m["support_extras.interface_pattern"] == "triangles");
    CHECK(m["support_extras.sublayer_start"] == 2);
    CHECK(m["support_extras.sublayer_end"] == 3);
    CHECK(m["support_extras.sublayer_pattern"] == "grid");
    CHECK(m["support_extras.normal_wall_loops"] == 2);
    CHECK(config.opt_serialize("support_interface_pattern") == "auto");
    CHECK_FALSE(config.has("cura_support_join_distance"));
    CHECK_FALSE(config.has("resin_branching_support_pillar_diameter"));
    // Settings this build no longer defines (H/2 and the low temperature interface are native mods) are dropped.
    CHECK_FALSE(config.has("outer_wall_half_layer_height"));
    CHECK_FALSE(config.has("single_nozzle_low_temperature_interface"));
    CHECK_FALSE(config.has("support_interface_temperature"));
}

TEST_CASE("A migrated line 1 feature turns off with its mod parameter", "[LegacySettings]")
{
    BundledMods mods;
    // Before: the line 1 key kept its value 1, differed from the system preset, and so counted as a
    // user change; the mod's rule skipped it and the feature stayed on with the parameter off.
    // The multi-nozzle walls mod's rule writes crisp_corner_small_nozzle_wall_speed, a key this build defines.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::string off = config.opt_serialize("crisp_corner_small_nozzle_wall_speed");
    config.apply(load({ { "crisp_corner_small_nozzle_wall_speed", "30" } }));
    REQUIRE(mod_settings(config).contains("multi_nozzle_walls.small_wall_speed"));
    CHECK(config.opt_serialize("crisp_corner_small_nozzle_wall_speed") == off);
    config.set_key_value("different_settings_to_system", new ConfigOptionStrings({ "", "", "" }));

    DynamicPrintConfig on = config;
    REQUIRE(ModHost::active_mods()->resolve(on).empty());
    CHECK(on.opt_serialize("crisp_corner_small_nozzle_wall_speed") == "30");

    nlohmann::json m = mod_settings(config);
    m["multi_nozzle_walls.small_wall_speed"] = 0;
    config.set_key_value("mod_settings", new ConfigOptionString(m.dump()));
    REQUIRE(ModHost::active_mods()->resolve(config).empty());
    CHECK(config.opt_serialize("crisp_corner_small_nozzle_wall_speed") == off);
}

TEST_CASE("A line 2 file that stores a mod-owned key at its default adds no mod parameter", "[LegacySettings]")
{
    BundledMods mods;
    // Line 2 saves these keys too; a default value carries nothing to migrate (crisp_corner's 0 walls
    // differ from the mod's default of 1 and would otherwise be written into mod_settings).
    const DynamicPrintConfig config = load({ { "crisp_corner_small_nozzle_wall_count", "0" }, { "single_nozzle_low_temperature_interface", "0" } });
    CHECK_FALSE(mod_settings(config).contains("multi_nozzle_walls.small_wall_count"));
    CHECK_FALSE(mod_settings(config).contains("low_temp_interface.enable"));
}

TEST_CASE("A value already in mod_settings wins over the line 1 setting", "[LegacySettings]")
{
    BundledMods mods;
    const DynamicPrintConfig config = load({
        { "mod_settings", R"({"half_layer.outer_walls": false, "other.value": 3})" },
        { "outer_wall_half_layer_height", "1" },
    });
    const nlohmann::json m = mod_settings(config);
    CHECK(m["half_layer.outer_walls"] == false);
    CHECK(m["other.value"] == 3);
}

TEST_CASE("A file without line 1 settings loads without mod_settings changes", "[LegacySettings]")
{
    BundledMods mods;
    const DynamicPrintConfig config = load({
        { "support_type", "tree(auto)" },
        { "support_interface_pattern", "grid" },
        { "outer_wall_half_layer_height", "0" },
        { "support_interface_sublayer_pattern", "" },
    });
    CHECK_FALSE(config.has("mod_settings"));
    CHECK(config.opt_serialize("support_type") == "tree(auto)");
    CHECK(config.opt_serialize("support_interface_pattern") == "grid");
}

TEST_CASE("Settings the support extras rules wrote are dropped without migrating", "[LegacySettings][SupportExtras]")
{
    // Before support_extras became a native mod its rules wrote these settings; mod_settings carries what the user
    // set. SeeMeCNC's Orca profiles carry support_wall_loops, a key Orca does not define: it must not set the
    // mod's normal wall count.
    BundledMods mods;
    const DynamicPrintConfig config = load({
        { "support_wall_loops", "0" },
        { "tree_support_wall_loops", "3" },
        { "support_interface_pattern_override", "grid" },
        { "support_interface_sublayer_start", "2" },
        { "support_interface_sublayer_end", "3" },
        { "support_interface_sublayer_pattern", "concentric" },
    });
    CHECK_FALSE(config.has("mod_settings"));
    for (const char *key : { "support_wall_loops", "tree_support_wall_loops", "support_interface_pattern_override",
                             "support_interface_sublayer_start", "support_interface_sublayer_end", "support_interface_sublayer_pattern" })
        CHECK_FALSE(config.has(key));
    // Line 1's sublayer angle (the same key name) still migrates; its default carries nothing.
    CHECK(mod_settings(load({ { "support_interface_sublayer_angle", "45" } }))["support_extras.sublayer_angle"] == 45.);
    CHECK_FALSE(load({ { "support_interface_sublayer_angle", "0" } }).has("mod_settings"));
}

TEST_CASE("Line 1 toolhead line widths become per-nozzle line widths in mm", "[LegacySettings][LineWidths]")
{
    BundledMods mods;
    // A percentage is of that toolhead's nozzle: 50 % of 0.2 mm is 0.1 mm. nil and 0 take the process width.
    const DynamicPrintConfig config = load({
        { "nozzle_diameter", { "0.15", "0.4", "0.2", "0.4" } },
        { "line_width", "0.42" }, { "outer_wall_line_width", "0.42" }, { "support_line_width", "0.42" },
        { "toolhead_line_width", "0.17,0,0.17,0" },
        { "toolhead_outer_wall_line_width", "0.17,0,50%,nil" },
        { "toolhead_support_line_width", "0,0,0,0" },
    });
    CHECK_FALSE(config.has("toolhead_outer_wall_line_width"));
    CHECK_FALSE(config.has("toolhead_line_width"));
    REQUIRE(config.has("outer_wall_line_width_per_nozzle"));
    const std::vector<double> outer = config.option<ConfigOptionFloats>("outer_wall_line_width_per_nozzle")->values;
    REQUIRE(outer.size() == 4);
    CHECK_THAT(outer[0], Catch::Matchers::WithinAbs(0.17, 1e-12));
    CHECK(outer[1] == 0.);
    CHECK_THAT(outer[2], Catch::Matchers::WithinAbs(0.1, 1e-12));
    CHECK(outer[3] == 0.);
    CHECK(config.option<ConfigOptionFloats>("line_width_per_nozzle")->values == std::vector<double>{ 0.17, 0., 0.17, 0. });
    // A list of zeros sets nothing.
    CHECK_FALSE(config.has("support_line_width_per_nozzle"));
}

TEST_CASE("Line 1 toolhead line widths without process settings to land in are dropped", "[LegacySettings][LineWidths]")
{
    BundledMods mods;
    // A printer preset alone: nozzle_diameter but no line width settings.
    const DynamicPrintConfig config = load({
        { "nozzle_diameter", { "0.15", "0.4" } },
        { "toolhead_outer_wall_line_width", "0.17,0" },
    });
    CHECK_FALSE(config.has("toolhead_outer_wall_line_width"));
    CHECK_FALSE(config.has("outer_wall_line_width_per_nozzle"));
}

TEST_CASE("Line 1 brush and cooling positions become the low temperature interface's printer parameters", "[LegacySettings]")
{
    BundledMods mods;
    // Line 1's printer settings: positions as "XxY" (mm), the brush's round trips and speed. The U1 guard's switch was a
    // rule output of line 2 and carries nothing (the printer's mod parameter does).
    const DynamicPrintConfig config = load({
        { "support_interface_brush_start", "10x12" },
        { "support_interface_brush_end", "30x12" },
        { "support_interface_cooling_position", "-1x-1" }, // the default: not written
        { "support_interface_brush_repetitions", "3" },
        { "support_interface_brush_speed", "80" },          // the default: not written
        { "snapmaker_u1_start_guard", "0" },
    });
    const auto *printer = config.option<ConfigOptionString>("printer_mod_settings");
    REQUIRE(printer != nullptr);
    const nlohmann::json p = nlohmann::json::parse(printer->value);
    CHECK(p["low_temp_interface.brush_start_x"] == 10.);
    CHECK(p["low_temp_interface.brush_start_y"] == 12.);
    CHECK(p["low_temp_interface.brush_end_x"] == 30.);
    CHECK(p["low_temp_interface.brush_end_y"] == 12.);
    CHECK(p["low_temp_interface.brush_repetitions"] == 3);
    CHECK_FALSE(p.contains("low_temp_interface.cooling_position_x"));
    CHECK_FALSE(p.contains("low_temp_interface.brush_speed"));
    CHECK_FALSE(p.contains("snapmaker_u1.start_guard"));
    CHECK_FALSE(mod_settings(config).contains("low_temp_interface.brush_repetitions"));
    for (const char *key : { "support_interface_brush_start", "support_interface_brush_end", "support_interface_cooling_position",
                             "support_interface_brush_repetitions", "support_interface_brush_speed", "snapmaker_u1_start_guard" })
        CHECK_FALSE(config.has(key));
}
