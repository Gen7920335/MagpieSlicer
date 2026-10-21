// MAGPIE: per-nozzle line width fields and the routing table (libslic3r/LineWidths.hpp). The expected widths are
// stated by each test from the values it sets, never read back from the slicer's prepared configs.
#include <catch2/catch_all.hpp>

#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/LineWidths.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// Two toolheads, filament n on toolhead n. Everything is printed by filament `filament` (1-based); the reference
// nozzle of the absolute widths is printer_variant 0.4 when the printer has it.
DynamicPrintConfig two_nozzles(double first_mm, double second_mm, int filament = 1)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "outer_wall_filament_id", filament }, { "inner_wall_filament_id", filament },
        { "sparse_infill_filament_id", filament }, { "internal_solid_filament_id", filament },
        { "top_surface_filament_id", filament }, { "bottom_surface_filament_id", filament },
        { "support_filament", filament }, { "support_interface_filament", filament },
        { "layer_height", 0.1 }, { "initial_layer_print_height", 0.1 }, // mm
        { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", false },
        { "wall_loops", 2 }, { "sparse_infill_density", 15 },
        { "printer_variant", "0.4" },
        { "layer_change_gcode", "G92 E0\n" },
    });
    config.set_num_extruders(2);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ first_mm, second_mm });
    size_flush_to_nozzles(config);
    return config;
}

// A 20 mm table on a 10 mm leg: walls, sparse and solid infill, top and bottom surfaces, a bridge under the top,
// and support when enabled.
TriangleMesh table_mesh()
{
    TriangleMesh mesh = make_cube(10., 10., 4.);
    mesh.translate(5.f, 5.f, 0.f);
    TriangleMesh top = make_cube(20., 20., 2.);
    top.translate(0.f, 0.f, 4.f);
    mesh.merge(top);
    return mesh;
}

// Lines that differ between two exports of the same print are dropped: comments (the config block, the export
// time) and blank lines.
// Sets one option of an object or part from its text.
void set_model_option(ModelConfig &config, const std::string &key, const std::string &value)
{
    DynamicPrintConfig option;
    option.set_deserialize_strict({ { key, value } });
    config.set_key_value(key, option.option(key)->clone());
}

std::string gcode_moves(const std::string &gcode)
{
    std::string        body;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        line.erase(std::min(line.size(), line.find(';')));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty())
            body += line + '\n';
    }
    return body;
}

} // namespace

TEST_CASE("The line width routing table names existing settings in the config that holds them", "[LineWidths]")
{
    const PrintConfig       print_config;
    const PrintObjectConfig object_config;
    const PrintRegionConfig region_config;
    for (size_t idx = 0; idx < width_routes().size(); ++idx) {
        const WidthRoute &route = width_routes()[idx];
        CAPTURE(idx);
        CHECK(size_t(route.feature) == idx);
        for (const char *key : route.chain) {
            if (key == nullptr)
                continue;
            CAPTURE(key);
            const ConfigBase &holder = std::string(key) == "initial_layer_line_width" ? static_cast<const ConfigBase &>(print_config) :
                                       std::string(key) == "line_width" || std::string(key) == "support_line_width" ? static_cast<const ConfigBase &>(object_config) :
                                                                                                                       static_cast<const ConfigBase &>(region_config);
            CHECK(holder.option(key) != nullptr);
            CHECK(holder.option(per_nozzle_line_width_key(key)) != nullptr);
        }
        switch (route.nozzle_source) {
        case WidthNozzleSource::RegionFilament:
            REQUIRE(route.region_filament != nullptr);
            CHECK(region_config.option(route.filament_key) == &(region_config.*route.region_filament));
            CHECK(route.object_filament == nullptr);
            break;
        case WidthNozzleSource::ObjectFilament:
            REQUIRE(route.object_filament != nullptr);
            CHECK(object_config.option(route.filament_key) == &(object_config.*route.object_filament));
            CHECK(route.region_filament == nullptr);
            break;
        case WidthNozzleSource::PrintingTool:
            CHECK(route.filament_key == nullptr);
            CHECK(route.region_filament == nullptr);
            CHECK(route.object_filament == nullptr);
            break;
        }
    }
    // Every per-nozzle field is one of the 11 line widths, and every line width has one.
    CHECK(per_nozzle_line_width_base_keys().size() == nozzle_relative_line_width_keys().size());
    for (const char *key : per_nozzle_line_width_base_keys())
        CHECK(std::find(nozzle_relative_line_width_keys().begin(), nozzle_relative_line_width_keys().end(), key) != nozzle_relative_line_width_keys().end());
}

TEST_CASE("A region role's filament is the one the routing table names", "[LineWidths]")
{
    PrintRegionConfig config;
    config.outer_wall_filament_id.value     = 1;
    config.inner_wall_filament_id.value     = 2;
    config.sparse_infill_filament_id.value  = 3;
    config.internal_solid_filament_id.value = 4;
    config.top_surface_filament_id.value    = 5;
    const PrintRegion region(PrintRegionConfig(config), 0);
    CHECK(region.extruder(frExternalPerimeter) == 1);
    CHECK(region.extruder(frPerimeter) == 2);
    CHECK(region.extruder(frInfill) == 3);
    CHECK(region.extruder(frSolidInfill) == 4);
    CHECK(region.extruder(frTopSolidInfill) == 5);
}

TEST_CASE("A per-nozzle field reads 0 past its end and for a missing nozzle", "[LineWidths]")
{
    DynamicPrintConfig config;
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(105., true));
    config.set_key_value("outer_wall_line_width_per_nozzle", new ConfigOptionFloats{ 0.17, 0., -1. });
    CHECK(line_width_setting_on_nozzle(config, "outer_wall_line_width", 0) == ConfigOptionFloatOrPercent(0.17, false));
    // 0, a negative value and a nozzle past the end take the setting.
    for (const size_t nozzle : { 1, 2, 3, 7 })
        CHECK(line_width_setting_on_nozzle(config, "outer_wall_line_width", nozzle) == ConfigOptionFloatOrPercent(105., true));
    config.erase("outer_wall_line_width_per_nozzle");
    CHECK(line_width_setting_on_nozzle(config, "outer_wall_line_width", 0) == ConfigOptionFloatOrPercent(105., true));
}

TEST_CASE("Object and part widths beat per-nozzle fields, which beat the scaled print widths", "[LineWidths][Precedence]")
{
    // print width x print field x override x printer; the expected width of each nozzle is stated below from these.
    const char *print_width = GENERATE("0", "0.42", "105%");
    const char *print_field = GENERATE("", "0.17,0", "0,0.5");
    const int   override    = GENERATE(0, 1, 2, 3, 4, 5, 6); // none, object 0.3, object 80 %, object 0, object field, part 0.3 + field, part 0.3
    const bool  mixed       = GENERATE(true, false);
    CAPTURE(print_width, print_field, override, mixed);
    const std::vector<double> nozzles = mixed ? std::vector<double>{ 0.15, 0.4 } : std::vector<double>{ 0.4, 0.4 }; // mm
    DynamicPrintConfig config = two_nozzles(nozzles[0], nozzles[1]);
    config.set_deserialize_strict({ { "outer_wall_line_width", print_width } });
    if (*print_field != 0)
        config.set_deserialize_strict({ { "outer_wall_line_width_per_nozzle", print_field } });
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 2.) }, print, model, config);
    ModelObject &object = *model.objects.front();
    ModelVolume &part   = *object.volumes.front();
    switch (override) {
    case 1: set_model_option(object.config, "outer_wall_line_width", "0.3"); break;
    case 2: set_model_option(object.config, "outer_wall_line_width", "80%"); break;
    case 3: set_model_option(object.config, "outer_wall_line_width", "0"); break;
    case 4: set_model_option(object.config, "outer_wall_line_width_per_nozzle", "0.2,0.3"); break;
    case 5:
        set_model_option(object.config, "outer_wall_line_width_per_nozzle", "0.2,0.3");
        set_model_option(part.config, "outer_wall_line_width", "0.3");
        break;
    case 6: set_model_option(part.config, "outer_wall_line_width", "0.3"); break;
    default: break;
    }
    print.apply(model, config);
    const PrintRegion &region = print.objects().front()->printing_region(0);

    // The test's own statement of the rule. The width: the last override that sets it, else the print's. The
    // field: none on a printer with one nozzle size; else the last override that sets the field, unless a later
    // one sets the width; else the print's field unless an override sets the width.
    struct Width { double value; bool percent; };
    auto parse = [](const std::string &text) { return Width{ std::stod(text), text.back() == '%' }; };
    Width width = parse(print_width);
    if (override == 1 || override == 5 || override == 6) width = { 0.3, false };
    if (override == 2) width = { 80., true };
    if (override == 3) width = { 0., false };
    std::vector<double> field;
    if (mixed) {
        if (override == 4)
            field = { 0.2, 0.3 };
        else if (override == 0 && *print_field != 0)
            field = std::string(print_field) == "0.17,0" ? std::vector<double>{ 0.17, 0. } : std::vector<double>{ 0., 0.5 };
    }
    const double reference_mm = mixed ? 0.4 : 0.; // printer_variant
    for (size_t nozzle = 0; nozzle < nozzles.size(); ++nozzle) {
        CAPTURE(nozzle);
        double expected_mm;
        if (nozzle < field.size() && field[nozzle] > 0.)
            expected_mm = field[nozzle];
        else if (width.percent)
            expected_mm = 0.01 * width.value * nozzles[nozzle];
        else if (reference_mm > 0.)
            expected_mm = width.value * nozzles[nozzle] / reference_mm;
        else
            expected_mm = width.value;
        CHECK_THAT(line_width_setting_on_nozzle(region.config(), "outer_wall_line_width", nozzle).get_abs_value(nozzles[nozzle]),
                   WithinAbs(expected_mm, 1e-9));
    }
}

TEST_CASE("Per-nozzle fields take one entry per nozzle and are dropped on a printer with one nozzle size", "[LineWidths]")
{
    const char *field = GENERATE("0.2", "0.2,0.3", "0.2,0.3,0.4,0.5,0.6,0.7");
    const bool  mixed = GENERATE(true, false);
    CAPTURE(field, mixed);
    DynamicPrintConfig config = two_nozzles(mixed ? 0.15 : 0.4, 0.4);
    set_toolhead_nozzle_diameter(config, 3, 0.4); // four toolheads
    size_flush_to_nozzles(config);
    config.set_deserialize_strict({ { "inner_wall_line_width_per_nozzle", field }, { "support_line_width_per_nozzle", field },
                                    { "initial_layer_line_width_per_nozzle", field } });
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 2.) }, print, model, config);
    const std::vector<double> given = config.option<ConfigOptionFloats>("inner_wall_line_width_per_nozzle")->values;
    std::vector<double> expected(4, 0.);
    for (size_t idx = 0; idx < expected.size() && idx < given.size(); ++idx)
        expected[idx] = given[idx];
    if (!mixed)
        expected = { 0. };
    CHECK(print.objects().front()->printing_region(0).config().inner_wall_line_width_per_nozzle.values == expected);
    CHECK(print.objects().front()->config().support_line_width_per_nozzle.values == expected);
    CHECK(print.config().initial_layer_line_width_per_nozzle.values == expected);
    // Written out as set.
    CHECK(print.full_print_config_as_set().option<ConfigOptionFloats>("inner_wall_line_width_per_nozzle")->values == given);
}

TEST_CASE("Changing a per-nozzle line width reslices to what a fresh print gives", "[LineWidths][Invalidation]")
{
    // One case per field. Everything prints with the 0.15 mm nozzle; each case sets what makes its field print.
    struct Case { const char *key; std::vector<std::pair<std::string, std::string>> extra; };
    const std::vector<Case> cases {
        { "line_width_per_nozzle", { { "outer_wall_line_width", "0" }, { "inner_wall_line_width", "0" }, { "sparse_infill_line_width", "0" },
                                      { "internal_solid_infill_line_width", "0" }, { "top_surface_line_width", "0" } } },
        { "initial_layer_line_width_per_nozzle", {} },
        { "outer_wall_line_width_per_nozzle", {} },
        { "inner_wall_line_width_per_nozzle", {} },
        { "sparse_infill_line_width_per_nozzle", {} },
        { "internal_solid_infill_line_width_per_nozzle", {} },
        { "top_surface_line_width_per_nozzle", {} },
        { "bridge_line_width_per_nozzle", { { "thick_bridges", "0" } } },
        { "skin_infill_line_width_per_nozzle", { { "sparse_infill_pattern", "lockedzag" }, { "sparse_infill_density", "30%" } } },
        { "skeleton_infill_line_width_per_nozzle", { { "sparse_infill_pattern", "lockedzag" }, { "sparse_infill_density", "30%" } } },
        { "support_line_width_per_nozzle", { { "enable_support", "1" }, { "support_type", "normal(auto)" } } },
        // The line widths themselves, which the fields follow.
        { "outer_wall_line_width", {} },
        { "inner_wall_line_width", {} },
    };
    const size_t idx = GENERATE(range(0, 13));
    const Case  &c   = cases[idx];
    CAPTURE(c.key);
    DynamicPrintConfig before = two_nozzles(0.15, 0.4);
    for (const auto &[key, value] : c.extra)
        before.set_deserialize_strict({ { key, value } });
    const bool field = std::string(c.key).find("_per_nozzle") != std::string::npos;
    before.set_deserialize_strict({ { c.key, field ? "0.17,0" : "0.17" } });
    DynamicPrintConfig after = before;
    after.set_deserialize_strict({ { c.key, field ? "0.19,0" : "0.19" } }); // mm, both wider than the 0.1 mm layers

    // Both prints are placed from `before` (init_print also sets the test G-code options), so only the slicing
    // history differs: one is sliced before the change, the other only after it.
    auto slice_after = [&](bool slice_before) {
        Print print;
        Model model;
        init_print({ table_mesh() }, print, model, before);
        std::string sliced_before;
        if (slice_before)
            sliced_before = gcode_moves(gcode(print));
        print.apply(model, after);
        return std::make_pair(sliced_before, gcode_moves(gcode(print)));
    };
    const auto [sliced_before, resliced] = slice_after(true);
    const std::string expected           = slice_after(false).second;
    // The change prints (else the test proves nothing), and the resliced print is the fresh one.
    CHECK(sliced_before != expected);
    CHECK(resliced == expected);
}

TEST_CASE("Per-nozzle line widths round-trip through a project file", "[LineWidths][3mf]")
{
    DynamicPrintConfig config = two_nozzles(0.15, 0.4);
    config.set_deserialize_strict({ { "outer_wall_line_width_per_nozzle", "0.17,0" }, { "support_line_width_per_nozzle", "0,0.45" },
                                    { "initial_layer_line_width_per_nozzle", "0.2,0" } });
    ScopedTemporaryDir  backup_dir("orca_line_widths_src");
    ScopedTemporaryFile file(".3mf");
    const std::string   path = file.string();
    {
        Model model;
        model.set_backup_path(backup_dir.string());
        ModelObject *object = model.add_object();
        object->add_volume(make_cube(10., 10., 10.));
        object->add_instance();
        set_model_option(object->config, "inner_wall_line_width_per_nozzle", "0.18,0.5");
        PlateData plate;
        plate.plate_index = 0;
        StoreParams params;
        params.path     = path.c_str();
        params.model    = &model;
        params.config   = &config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        params.plate_data_list.push_back(&plate);
        REQUIRE(store_bbs_3mf(params));
    }

    ScopedTemporaryDir        load_dir("orca_line_widths_dst");
    Model                     loaded;
    loaded.set_backup_path(load_dir.string());
    DynamicPrintConfig        loaded_config;
    ConfigSubstitutionContext substitutions{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(path.c_str(), &loaded_config, &substitutions, &loaded, &plates, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                         &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    for (const char *key : { "outer_wall_line_width_per_nozzle", "support_line_width_per_nozzle", "initial_layer_line_width_per_nozzle" }) {
        CAPTURE(key);
        REQUIRE(loaded_config.option(key) != nullptr);
        CHECK(*loaded_config.option(key) == *config.option(key));
    }
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->config.has("inner_wall_line_width_per_nozzle"));
    const std::vector<double> loaded_values = loaded.objects.front()->config.get().option<ConfigOptionFloats>("inner_wall_line_width_per_nozzle")->values;
    CHECK(loaded_values == std::vector<double>{ 0.18, 0.5 });
    release_PlateData_list(plates);
}

TEST_CASE("A per-nozzle line width is checked against its own nozzle", "[LineWidths][validate]")
{
    // 5 x the 0.15 mm nozzle is 0.75 mm; the layers are 0.1 mm. The 0.4 mm nozzle prints nothing in case 3.
    const int scenario = GENERATE(0, 1, 2, 3); // fine, too wide, not wider than the layer, too wide on an idle nozzle
    CAPTURE(scenario);
    DynamicPrintConfig config = two_nozzles(0.15, 0.4);
    const char *field = scenario == 0 ? "0.7,0" : scenario == 1 ? "0.8,0" : scenario == 2 ? "0.1,0" : "0.17,2.5";
    config.set_deserialize_strict({ { "sparse_infill_line_width_per_nozzle", field } });
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 2.) }, print, model, config);
    const StringObjectException error = print.validate();
    if (scenario == 0 || scenario == 3) {
        CHECK(error.string.empty());
    } else {
        CHECK(error.opt_key == "sparse_infill_line_width_per_nozzle");
        CHECK(error.string.find("nozzle 1 (0.15 mm)") != std::string::npos);
    }
}

TEST_CASE("A fixed width below 0.1 mm plus 0.4 times its nozzle is warned about on a mixed-nozzle printer", "[LineWidths][validate]")
{
    // 0.17 mm on the 0.4 mm nozzle is below 0.1 + 0.4 x 0.4 = 0.26 mm. Arachne walls vary their width, so they are
    // not judged; 105 % of the 0.15 mm nozzle (0.1575 mm) is at the nozzle-diameter cap of the bound, not below it.
    const int scenario = GENERATE(0, 1, 2, 3, 4); // classic walls 0.17, arachne walls 0.17, sparse infill 0.17, stock 0.15 nozzle widths, one size
    CAPTURE(scenario);
    DynamicPrintConfig config = two_nozzles(scenario == 4 ? 0.4 : 0.15, 0.4, 2);
    config.set_deserialize_strict({ { "wall_generator", scenario == 1 ? "arachne" : "classic" } });
    if (scenario == 0 || scenario == 1 || scenario == 4)
        config.set_deserialize_strict({ { "outer_wall_line_width_per_nozzle", "0,0.17" } });
    if (scenario == 2)
        config.set_deserialize_strict({ { "sparse_infill_line_width_per_nozzle", "0,0.17" } });
    if (scenario == 3) {
        config = two_nozzles(0.15, 0.4, 1);
        config.set_deserialize_strict({ { "outer_wall_line_width", "105%" }, { "line_width", "105%" } });
    }
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 2.) }, print, model, config);
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    const bool warned = std::any_of(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
        return w.string.find("may not extrude reliably") != std::string::npos;
    });
    CHECK(warned == (scenario == 0 || scenario == 2));
}

TEST_CASE("A bottom surface is resolved on the nozzle of its own filament", "[LineWidths]")
{
    // Internal solid infill on the 0.4 mm nozzle, bottom surface on the 0.15 mm one, both at 100 %.
    DynamicPrintConfig config = two_nozzles(0.15, 0.4, 2);
    config.set_deserialize_strict({ { "bottom_surface_filament_id", 1 }, { "internal_solid_infill_line_width", "100%" } });
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 2.) }, print, model, config);
    const PrintObject &object = *print.objects().front();
    const PrintRegion &region = object.printing_region(0);
    CHECK_THAT(region.flow(object, WidthFeature::BottomSurface, 0.1, false).width(), WithinAbs(0.15, 1e-6));
    CHECK_THAT(region.flow(object, WidthFeature::InternalSolidInfill, 0.1, false).width(), WithinAbs(0.4, 1e-6));
}

TEST_CASE("Placeholders resolve a line width on the current nozzle with its per-nozzle field", "[LineWidths][PlaceholderParser]")
{
    DynamicPrintConfig config = two_nozzles(0.15, 0.4);
    config.set_deserialize_strict({ { "outer_wall_line_width", "100%" }, { "outer_wall_line_width_per_nozzle", "0.17,0" } });
    CHECK_THAT(Flow::extrusion_width("outer_wall_line_width", config, 0), WithinAbs(0.17, 1e-9));
    // 100 % of the 0.4 mm nozzle, resolved on the float diameter Flow keeps.
    CHECK_THAT(Flow::extrusion_width("outer_wall_line_width", config, 1), WithinAbs(0.4, 1e-6));
    // One nozzle size: the field is not read.
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.4 });
    CHECK_THAT(Flow::extrusion_width("outer_wall_line_width", config, 0), WithinAbs(0.4, 1e-6));
}

// MAGPIE: on a BBL printer filament_map names the nozzle printing a filament (printing_nozzle_index), and an automatic
// map mode derives the map from the sliced objects (ToolOrdering), after the walls were sliced with the map the config
// held: on an H2D with 0.4 and 0.8 mm nozzles the walls of the 0.8 mm nozzle came out 0.42 mm wide (review
// 2026-10-22). Now the objects are sliced again with the derived map (Print::reslice_for_derived_filament_map).
TEST_CASE("An automatic filament map slices the walls on the nozzle it derives", "[LineWidths][FilamentMap]")
{
    // Each nozzle has a wall width of its own; whichever nozzle the engine picks, the walls are that nozzle's width.
    auto slice_with = [](const std::vector<int> &initial_map) {
        DynamicPrintConfig config = two_nozzles(0.4, 0.8);
        config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmAutoForFlush;
        config.set_key_value("filament_map", new ConfigOptionInts(initial_map));
        config.set_key_value("filament_colour", new ConfigOptionStrings{ "#F2754E", "#F2754E" });
        config.set_deserialize_strict({ { "outer_wall_line_width_per_nozzle", "0.42,0.84" }, { "inner_wall_line_width_per_nozzle", "0.42,0.84" } });
        Print print;
        print.is_BBL_printer() = true;
        Model model;
        init_print({ make_cube(20., 20., 20.) }, print, model, config);
        print.process();
        const std::string output = gcode(print);
        // A BBL printer names features "; FEATURE: <role>" and widths "; LINE_WIDTH: <mm>".
        std::set<double> wall_widths;
        double           width = 0.;
        std::string      role;
        GCodeReader      reader;
        reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            const std::string raw(line.raw());
            if (raw.rfind("; FEATURE: ", 0) == 0)
                role = raw.substr(11);
            else if (raw.rfind("; LINE_WIDTH: ", 0) == 0)
                width = std::stod(raw.substr(14));
            else if (line.extruding(self) && line.dist_XY(self) > 0 && (role == "Outer wall" || role == "Inner wall"))
                wall_widths.insert(width);
        });
        return std::make_pair(print.config().filament_map.values, wall_widths);
    };
    const auto [derived, widths] = slice_with({ 1, 1 });
    REQUIRE(derived.size() == 2);
    REQUIRE(derived[0] >= 1);
    REQUIRE(derived[0] <= 2);
    // Started from the other nozzle, the engine derives the same map, and the walls follow it both times.
    const auto [derived_again, widths_again] = slice_with({ derived[0] == 1 ? 2 : 1, derived[1] });
    CHECK(derived_again == derived);
    const double expected = derived[0] == 1 ? 0.42 : 0.84; // mm, the derived nozzle's wall width
    for (const std::set<double> &found : { widths, widths_again }) {
        REQUIRE(found.size() == 1);
        CHECK_THAT(*found.begin(), WithinAbs(expected, 1e-3));
    }
}
