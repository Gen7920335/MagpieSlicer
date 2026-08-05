#include <catch2/catch_all.hpp>

#include "libslic3r/HotendPresetStore.hpp"

#include <boost/filesystem.hpp>

#include <map>

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TemporaryDirectory {
    TemporaryDirectory() : path(fs::temp_directory_path() / fs::unique_path("magpie-hotend-presets-%%%%-%%%%-%%%%"))
    {
        fs::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

DynamicPrintConfig hotend_config(double nozzle, double width)
{
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({nozzle}));
    for (const char *key : hotend_preset_width_keys())
        config.set_key_value(key, new ConfigOptionFloatsOrPercents({FloatOrPercent(width, false)}));
    return config;
}

DynamicPrintConfig printer_config()
{
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.2, 0.6, 0.8}));
    for (const char *key : hotend_preset_width_keys())
        config.set_key_value(key, new ConfigOptionFloatsOrPercents({
            FloatOrPercent(0.45, false), FloatOrPercent(0.22, false),
            FloatOrPercent(0.66, false), FloatOrPercent(0.88, false)}));
    return config;
}

DynamicPrintConfig printer_config_with_count(size_t count)
{
    DynamicPrintConfig config;
    std::vector<double> nozzles(count);
    std::vector<FloatOrPercent> widths(count);
    for (size_t index = 0; index < count; ++index) {
        nozzles[index] = 0.3 + 0.05 * double(index);
        widths[index] = FloatOrPercent(0.34 + 0.05 * double(index), false);
    }
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles));
    for (const char *key : hotend_preset_width_keys())
        config.set_key_value(key, new ConfigOptionFloatsOrPercents(widths));
    return config;
}

} // namespace

TEST_CASE("Hotend presets round-trip independently", "[HotendPresetStore]")
{
    TemporaryDirectory temporary;
    HotendPresetStore store(temporary.path);
    DynamicPrintConfig saved = hotend_config(0.2, 0.24);

    store.save("Fine detail", saved);
    REQUIRE(store.contains("Fine detail"));
    REQUIRE(store.names() == std::vector<std::string>{"Fine detail"});

    DynamicPrintConfig loaded = hotend_config(0.4, 0.42);
    store.load("Fine detail", loaded);
    REQUIRE(loaded.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.2});
    REQUIRE(loaded.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.front().value == Catch::Approx(0.24));
}

TEST_CASE("Hotend preset overwrite replaces values", "[HotendPresetStore]")
{
    TemporaryDirectory temporary;
    HotendPresetStore store(temporary.path);
    store.save("Shared", hotend_config(0.2, 0.24));
    store.save("Shared", hotend_config(0.6, 0.66));

    DynamicPrintConfig loaded = hotend_config(0.4, 0.42);
    store.load("Shared", loaded);
    REQUIRE(loaded.option<ConfigOptionFloats>("nozzle_diameter")->values.front() == Catch::Approx(0.6));
}

TEST_CASE("Hotend preset names cannot escape their directory", "[HotendPresetStore]")
{
    REQUIRE(HotendPresetStore::is_valid_name("0.4 brass"));
    for (const std::string &name : {"", ".", "..", "../outside", "nested/preset", "bad:name", "trailing.", " trailing"})
        REQUIRE_FALSE(HotendPresetStore::is_valid_name(name));
}

TEST_CASE("Hotend presets reject percentages and missing files", "[HotendPresetStore]")
{
    TemporaryDirectory temporary;
    HotendPresetStore store(temporary.path);
    DynamicPrintConfig invalid = hotend_config(0.4, 0.42);
    invalid.set_key_value("toolhead_outer_wall_line_width",
                          new ConfigOptionFloatsOrPercents({FloatOrPercent(100., true)}));

    REQUIRE_THROWS_AS(store.save("Percentage", invalid), std::runtime_error);
    DynamicPrintConfig defaults = hotend_config(0.4, 0.42);
    REQUIRE_THROWS_AS(store.load("Missing", defaults), std::runtime_error);
}

TEST_CASE("Hotend config extraction isolates one toolhead", "[HotendConfigService]")
{
    const DynamicPrintConfig printer = printer_config();
    const DynamicPrintConfig hotend = HotendConfigService::extract(printer, 2);

    REQUIRE(hotend.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.6});
    REQUIRE(hotend.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.size() == 1);
    REQUIRE(hotend.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.front().value == Catch::Approx(0.66));
}

TEST_CASE("Hotend config application preserves every unselected toolhead", "[HotendConfigService]")
{
    DynamicPrintConfig printer = printer_config();
    HotendConfigService::apply(printer, 1, hotend_config(0.15, 0.17));

    REQUIRE(printer.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.4, 0.15, 0.6, 0.8});
    for (const char *key : hotend_preset_width_keys()) {
        const auto &widths = printer.option<ConfigOptionFloatsOrPercents>(key)->values;
        REQUIRE(widths.size() == 4);
        REQUIRE(widths[0].value == Catch::Approx(0.45));
        REQUIRE(widths[1].value == Catch::Approx(0.17));
        REQUIRE(widths[2].value == Catch::Approx(0.66));
        REQUIRE(widths[3].value == Catch::Approx(0.88));
    }
}

TEST_CASE("Hotend normalization preserves explicit millimeter widths", "[HotendConfigService]")
{
    const DynamicPrintConfig printer = printer_config();
    const DynamicPrintConfig patch = HotendConfigService::normalization_patch(printer, 3);

    for (const char *key : hotend_preset_width_keys())
        REQUIRE(patch.option<ConfigOptionFloatsOrPercents>(key)->values ==
                printer.option<ConfigOptionFloatsOrPercents>(key)->values);
}

TEST_CASE("Hotend single-value updates preserve unrelated settings", "[HotendConfigService]")
{
    DynamicPrintConfig printer = printer_config();
    const auto inner_before = printer.option<ConfigOptionFloatsOrPercents>("toolhead_inner_wall_line_width")->values;

    HotendConfigService::apply_width(printer, 2, "toolhead_outer_wall_line_width", FloatOrPercent(0.7, false));

    const auto &outer = printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values;
    REQUIRE(outer[0].value == Catch::Approx(0.45));
    REQUIRE(outer[1].value == Catch::Approx(0.22));
    REQUIRE(outer[2].value == Catch::Approx(0.7));
    REQUIRE(outer[3].value == Catch::Approx(0.88));
    REQUIRE(printer.option<ConfigOptionFloatsOrPercents>("toolhead_inner_wall_line_width")->values == inner_before);
    REQUIRE(printer.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{0.4, 0.2, 0.6, 0.8});
}

TEST_CASE("Hotend config application rejects an invalid toolhead index", "[HotendConfigService]")
{
    DynamicPrintConfig printer = printer_config();
    REQUIRE_THROWS_AS(HotendConfigService::apply(printer, 4, hotend_config(0.4, 0.45)), std::out_of_range);
}

TEST_CASE("Hotend config supports one two four and eight toolheads", "[HotendConfigService]")
{
    for (const size_t count : {size_t(1), size_t(2), size_t(4), size_t(8)}) {
        DYNAMIC_SECTION(count << " toolheads") {
            DynamicPrintConfig printer = printer_config_with_count(count);
            const std::vector<double> original_nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter")->values;
            const auto original_widths = printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values;
            const size_t selected = count - 1;

            HotendConfigService::apply(printer, selected, hotend_config(0.15, 0.17));

            const auto &nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter")->values;
            const auto &widths = printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values;
            REQUIRE(nozzles.size() == count);
            REQUIRE(widths.size() == count);
            for (size_t index = 0; index < selected; ++index) {
                REQUIRE(nozzles[index] == Catch::Approx(original_nozzles[index]));
                REQUIRE(widths[index].value == Catch::Approx(original_widths[index].value));
            }
            REQUIRE(nozzles[selected] == Catch::Approx(0.15));
            REQUIRE(widths[selected].value == Catch::Approx(0.17));
        }
    }
}

TEST_CASE("Hotend config JSON round-trip preserves one two four and eight toolheads", "[HotendConfigService]")
{
    TemporaryDirectory temporary;
    for (const size_t count : {size_t(1), size_t(2), size_t(4), size_t(8)}) {
        DYNAMIC_SECTION(count << " toolheads") {
            const DynamicPrintConfig saved = printer_config_with_count(count);
            const fs::path path = temporary.path / ("hotend-" + std::to_string(count) + ".json");
            saved.save_to_json(path.string(), "Hotend round-trip", "User", "2.5.0");

            DynamicPrintConfig loaded;
            std::map<std::string, std::string> metadata;
            std::string reason;
            const ConfigSubstitutions substitutions = loaded.load_from_json(
                path.string(), ForwardCompatibilitySubstitutionRule::Disable, metadata, reason);
            REQUIRE(reason.empty());
            REQUIRE(substitutions.empty());

            HotendConfigService::normalize_printer_config(loaded);
            const auto &loaded_nozzles = loaded.option<ConfigOptionFloats>("nozzle_diameter")->values;
            const auto &saved_nozzles = saved.option<ConfigOptionFloats>("nozzle_diameter")->values;
            REQUIRE(loaded_nozzles.size() == saved_nozzles.size());
            for (size_t index = 0; index < saved_nozzles.size(); ++index)
                REQUIRE(loaded_nozzles[index] == Catch::Approx(saved_nozzles[index]));
            for (const char *key : hotend_preset_width_keys()) {
                const auto &loaded_widths = loaded.option<ConfigOptionFloatsOrPercents>(key)->values;
                const auto &saved_widths = saved.option<ConfigOptionFloatsOrPercents>(key)->values;
                REQUIRE(loaded_widths.size() == saved_widths.size());
                for (size_t index = 0; index < saved_widths.size(); ++index) {
                    REQUIRE(loaded_widths[index].percent == saved_widths[index].percent);
                    REQUIRE(loaded_widths[index].value == Catch::Approx(saved_widths[index].value));
                }
            }
        }
    }
}

TEST_CASE("Hotend config normalization repairs legacy width vectors", "[HotendConfigService]")
{
    DynamicPrintConfig legacy = printer_config_with_count(4);
    legacy.set_key_value("toolhead_outer_wall_line_width", new ConfigOptionFloatsOrPercents({
        FloatOrPercent(0., false), FloatOrPercent(100., true)}));
    legacy.set_key_value("toolhead_bridge_line_width", new ConfigOptionFloatsOrPercents({}));

    HotendConfigService::normalize_printer_config(legacy);

    const auto &nozzles = legacy.option<ConfigOptionFloats>("nozzle_diameter")->values;
    const auto &outer = legacy.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values;
    const auto &bridge = legacy.option<ConfigOptionFloatsOrPercents>("toolhead_bridge_line_width")->values;
    REQUIRE(outer.size() == 4);
    REQUIRE(bridge.size() == 4);
    REQUIRE(outer[0].value == Catch::Approx(default_toolhead_line_width_for_nozzle("toolhead_outer_wall_line_width", nozzles[0]).value));
    REQUIRE(outer[1].value == Catch::Approx(nozzles[1]));
    for (size_t index = 0; index < nozzles.size(); ++index) {
        REQUIRE_FALSE(outer[index].percent);
        REQUIRE_FALSE(bridge[index].percent);
        REQUIRE(outer[index].value > 0.);
        REQUIRE(bridge[index].value == Catch::Approx(nozzles[index]));
    }
}

TEST_CASE("Printer preset remains authoritative across hotend views and preset transfers",
          "[HotendConfigService][HotendPresetStore][SourceOfTruth]")
{
    DynamicPrintConfig printer = DynamicPrintConfig::full_print_config();
    printer.set_num_extruders(4);
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4, 0.4, 0.4 }));
    for (const char *key : hotend_preset_width_keys())
        printer.set_key_value(key, new ConfigOptionFloatsOrPercents(
            std::vector<FloatOrPercent>(4, FloatOrPercent(0.44, false))));

    const std::array<double, 4> nozzles { 0.15, 0.20, 0.60, 0.80 };
    const std::array<double, 4> widths { 0.18, 0.24, 0.66, 0.88 };
    for (size_t index = 0; index < nozzles.size(); ++index) {
        HotendConfigService::apply_nozzle_diameter(printer, index, nozzles[index]);
        HotendConfigService::apply_width(
            printer, index, "toolhead_outer_wall_line_width", FloatOrPercent(widths[index], false));
    }

    for (size_t index = 0; index < nozzles.size(); ++index) {
        const DynamicPrintConfig view = HotendConfigService::extract(printer, index);
        CHECK(view.option<ConfigOptionFloats>("nozzle_diameter")->values.front() == Catch::Approx(nozzles[index]));
        CHECK(view.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.front().value ==
              Catch::Approx(widths[index]));
    }

    const DynamicPrintConfig stale_view = HotendConfigService::extract(printer, 1);
    HotendConfigService::apply_width(
        printer, 1, "toolhead_outer_wall_line_width", FloatOrPercent(0.31, false));
    CHECK(stale_view.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.front().value ==
          Catch::Approx(0.24));
    CHECK(HotendConfigService::extract(printer, 1)
              .option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values.front().value ==
          Catch::Approx(0.31));

    TemporaryDirectory temporary;
    HotendPresetStore store(temporary.path);
    store.save("Tool 2", HotendConfigService::extract(printer, 1));
    DynamicPrintConfig transferred = hotend_config(0.4, 0.42);
    store.load("Tool 2", transferred);
    HotendConfigService::apply(printer, 3, transferred);

    const auto &stored_nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter")->values;
    REQUIRE(stored_nozzles.size() == 4);
    CHECK(stored_nozzles[0] == Catch::Approx(0.15));
    CHECK(stored_nozzles[1] == Catch::Approx(0.20));
    CHECK(stored_nozzles[2] == Catch::Approx(0.60));
    CHECK(stored_nozzles[3] == Catch::Approx(0.20));
    CHECK(printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width")->values[3].value ==
          Catch::Approx(0.31));
}
