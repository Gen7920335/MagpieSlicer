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
        const fs::path dir = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" / "mods";
        ModHost::set_active_mods(std::make_shared<const ModHost::ModSet>(ModHost::ModSet::load_dir(dir, errors)));
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
    // A setting this build also defines is kept as written.
    CHECK(config.opt_bool("outer_wall_half_layer_height"));
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
