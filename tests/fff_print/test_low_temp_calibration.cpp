// MAGPIE: the low temperature interface calibrations of the low_temp_calibration mod, built as the application builds
// them (ModHost::build_calibration, the calibration.project slot) and sliced: the LESIC ring (ported from Magpie line
// 1's test_print.cpp) and the one-touch interface grid.
#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/CalibrationProject.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

const char *const MOD = "low_temp_calibration";

DynamicPrintConfig base_config(double bed, double nozzle)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    config.set_key_value("printable_area", new ConfigOptionPoints({ Vec2d(0, 0), Vec2d(bed, 0), Vec2d(bed, bed), Vec2d(0, bed) }));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ nozzle }));
    return config;
}

// The calibration's G-code as the application prints it: the project's settings written into the presets (here one
// config), its objects sliced with the calibration's values.
std::string calibration_gcode(const std::string &calibration, DynamicPrintConfig config, const ModHost::CalibrationFields &fields,
                              ModHost::CalibrationProject *out = nullptr, bool as_calibration = true)
{
    ModHost::CalibrationProject project = ModHost::build_calibration(MOD, calibration, config, fields);
    ModHost::apply_calibration_options(project, config, config, config);
    Print print;
    for (ModelObject *object : project.model.objects)
        print.auto_assign_extruders(object);
    print.apply(project.model, config);
    print.validate();
    if (as_calibration) {
        Calib_Params params;
        params.mode            = CalibMode::Calib_Mod;
        params.mod_calibration = project.calibration;
        params.mod_values      = project.values;
        print.set_calib_params(params);
    }
    std::string result = gcode(print);
    if (out != nullptr)
        *out = std::move(project);
    return result;
}

ModHost::CalibrationFields lesic_fields(double start, double end, double step, int layers, double mvs_start, double mvs_end)
{
    auto n = [](double v) { return ModHost::format_decimal(v); };
    return { { "start", n(start) }, { "end", n(end) }, { "step", n(step) }, { "layers_per_temp", std::to_string(layers) },
             { "mvs_start", n(mvs_start) }, { "mvs_end", n(mvs_end) } };
}

// The grid's form: the two axes and the fixed values of every axis setting (as the form passes the presets' values).
ModHost::CalibrationFields grid_fields(const std::string &x_axis, double x_start, double x_end, int x_count, const std::string &y_axis,
                                       double y_start, double y_end, int y_count)
{
    auto n = [](double v) { return ModHost::format_decimal(v); };
    return { { "combination", "custom" },
             { "x_axis", x_axis }, { "x_start", n(x_start) }, { "x_end", n(x_end) }, { "x_count", std::to_string(x_count) },
             { "y_axis", y_axis }, { "y_start", n(y_start) }, { "y_end", n(y_end) }, { "y_count", std::to_string(y_count) },
             { "pattern", "rectilinear" }, { "temperature", "170" }, { "top_z_distance", "0.2" }, { "bottom_z_distance", "0" },
             { "top_interface_layers", "2" }, { "bottom_interface_layers", "2" }, { "interface_spacing", "0.5" },
             { "heating_time", "5" }, { "aux_fan", "0" }, { "aux_fan_speed", "100" }, { "brush", "0" }, { "tower", "0" } };
}

DynamicPrintConfig grid_config()
{
    DynamicPrintConfig config = base_config(200., 0.4);
    config.set_deserialize_strict({
        { "nozzle_temperature", 220 },
        { "nozzle_temperature_initial_layer", 220 },
        { "support_interface_top_layers", 2 },
        // Interface at both ends of the support and no bottom gap: the gap holds the interface only.
        { "support_interface_bottom_layers", 2 },
        { "support_top_z_distance", 0.2 },
        { "support_bottom_z_distance", 0 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
    });
    set_mod_params(config, { { "low_temp_interface.heating_time", 5.0 } });
    return config;
}

struct LayerTrace
{
    std::vector<std::string> roles;        // ;TYPE: roles in print order
    std::vector<std::string> temperatures; // M104/M109 commands in order ("M109 R190")
    int                      dwells = 0;   // reheat dwells ("heat before next layer")
};

std::vector<LayerTrace> trace_layers(const std::string &gcode)
{
    std::vector<LayerTrace> layers;
    GCodeReader             reader;
    std::string             role;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        // The role carries on into the next layer until a ;TYPE: line changes it.
        if (raw == ";LAYER_CHANGE") {
            layers.emplace_back();
            return;
        }
        if (layers.empty())
            return;
        if (raw.rfind(";TYPE:", 0) == 0) {
            role = raw.substr(6);
            return;
        }
        float t = 0.f;
        if (line.cmd_is("M104") || line.cmd_is("M109")) {
            const char key = line.has_value('R', t) ? 'R' : line.has_value('S', t) ? 'S' : 0;
            if (key != 0)
                layers.back().temperatures.push_back(std::string(line.cmd()) + " " + key + std::to_string(int(t)));
        } else if (raw.find("heat before next layer") != std::string::npos)
            ++layers.back().dwells;
        else if (line.extruding(self) && line.dist_XY(self) > 0 && (layers.back().roles.empty() || layers.back().roles.back() != role))
            layers.back().roles.push_back(role);
    });
    return layers;
}

} // namespace

TEST_CASE("LESIC calibration slices a cylinder and emits calibration G-code", "[LESIC]")
{
    OfficialMods      mods;
    const std::string output = calibration_gcode("lesic", base_config(120., 0.4), lesic_fields(210., 205., 2., 1, 8., 24.));
    CHECK(output.find("; LESIC: TEMP:210") != std::string::npos);
    CHECK(output.find("; LESIC: TEMP:205") != std::string::npos);
    CHECK(output.find("; lesic_ring_mvs_values=8,10,15,20,24") != std::string::npos);
    CHECK(output.find("; lesic_inner_brim_lines=5") != std::string::npos);
    CHECK(output.find("; lesic_bottom_label_height=") != std::string::npos);
    CHECK(output.find("; lesic_annotation_segments=") != std::string::npos);
    CHECK(output.find("LESIC annotation speed") != std::string::npos);
    CHECK(output.find("\nG1 ") != std::string::npos);
}

TEST_CASE("LESIC speeds follow the angle around the ring and stay off outside the calibration", "[LESIC]")
{
    OfficialMods mods;
    // Distinct feed rates (mm/min) after the first layer.
    const auto ring_feeds = [](const std::string &gcode) {
        std::set<int> feeds;
        const size_t second_layer = gcode.find(";LAYER_CHANGE", gcode.find(";LAYER_CHANGE") + 1);
        static const std::regex move(R"(G1 [^;\n]*F([0-9]+))");
        for (std::sregex_iterator it(gcode.begin() + second_layer, gcode.end(), move), end; it != end; ++it)
            feeds.insert(std::stoi((*it)[1].str()));
        return feeds;
    };
    DynamicPrintConfig config = base_config(120., 0.4);
    // A slow volumetric limit would cap the ring's top speeds.
    config.set_deserialize_strict({ { "filament_max_volumetric_speed", 40 } });
    ModHost::CalibrationProject project;
    const std::string lesic = calibration_gcode("lesic", config, lesic_fields(210., 205., 5., 2, 8., 24.), &project);

    // The same ring and settings without the calibration: a plain vase.
    ModHost::apply_calibration_options(project, config, config, config);
    Print print;
    for (ModelObject *object : project.model.objects)
        print.auto_assign_extruders(object);
    print.apply(project.model, config);
    const std::string plain = gcode(print);

    // 8 to 24 mm3/s around the ring gives many speeds; the plain vase prints at one wall speed.
    CHECK(ring_feeds(lesic).size() > 20);
    CHECK(ring_feeds(plain).size() < 5);
    CHECK(plain.find("; LESIC") == std::string::npos);
    CHECK(plain.find("lesic_") == std::string::npos);
}

TEST_CASE("LESIC sizes its ring by the bed and its height by every temperature band", "[LESIC]")
{
    OfficialMods mods;
    // A 256 mm bed keeps 10 mm on each side; 0.4 mm nozzle: 0.24 mm layers; 210 to 165 degC by 1 degC is 46 bands,
    // the end included; 3 layers per band.
    const ModHost::CalibrationProject project =
        ModHost::build_calibration(MOD, "lesic", base_config(256., 0.4), lesic_fields(210., 165., 1., 3, 8., 24.));
    REQUIRE(project.model.objects.size() == 1);
    const Vec3d size = project.model.objects.front()->instance_bounding_box(0).size();
    CHECK_THAT(size.x(), Catch::Matchers::WithinAbs(236., 1e-3));
    CHECK_THAT(size.z(), Catch::Matchers::WithinAbs(0.24 * 3 * 46, 1e-3));
    CHECK(project.values.at("lesic_small_bed") == 0.);
    CHECK(ModHost::build_calibration(MOD, "lesic", base_config(180., 0.4), lesic_fields(210., 165., 1., 3, 8., 24.)).values.at("lesic_small_bed") == 1.);
}

TEST_CASE("LESIC refuses a form without a whole step between its temperatures", "[LESIC]")
{
    OfficialMods mods;
    const DynamicPrintConfig config = base_config(200., 0.4);
    CHECK(ModHost::check_calibration(MOD, "lesic", config, lesic_fields(210., 165., 1., 10, 8., 24.)).ok());
    // Start and end closer than one step; layers 0; end MVS not above start MVS.
    CHECK_FALSE(ModHost::check_calibration(MOD, "lesic", config, lesic_fields(210., 209.5, 1., 10, 8., 24.)).ok());
    CHECK_FALSE(ModHost::check_calibration(MOD, "lesic", config, lesic_fields(210., 165., 1., 0, 8., 24.)).ok());
    CHECK_FALSE(ModHost::check_calibration(MOD, "lesic", config, lesic_fields(210., 165., 1., 10, 8., 8.)).ok());
    CHECK_THROWS_AS(ModHost::build_calibration(MOD, "lesic", config, lesic_fields(210., 165., 1., 0, 8., 24.)), ModHost::ModError);
}

TEST_CASE("The interface grid lays out one specimen per pair of values, hottest interface first", "[LowTemperatureCalibration]")
{
    OfficialMods                      mods;
    const ModHost::CalibrationProject project =
        ModHost::build_calibration(MOD, "grid", grid_config(), grid_fields("temperature", 200., 180., 3, "top_z_distance", 0., 0.1, 2));
    REQUIRE(project.model.objects.size() == 6);
    std::vector<std::string> names;
    for (const ModelObject *object : project.model.objects) {
        names.push_back(object->name);
        // The cube, the lid and the handle, and the enforcer in the gap.
        CHECK(object->volumes.size() == 4);
        CHECK(std::count_if(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *v) { return v->is_support_enforcer(); }) == 1);
    }
    // Hottest first; within a temperature in the grid's back-and-forth row order (the back row first).
    CHECK(names == std::vector<std::string>{ "T200_Z0.00", "T200_Z0.10", "T190_Z0.00", "T190_Z0.10", "T180_Z0.00", "T180_Z0.10" });
    // The gap holds the interface only: 2 top and 2 bottom interface layers of 0.2 mm, no bottom gap, the top gap;
    // the lid's underside sits there above the 5 mm cube (specimen height: 5 + gap + 3 + 10 mm handle).
    for (const ModelObject *object : project.model.objects) {
        const double top_z = object->name.find("Z0.10") != std::string::npos ? 0.1 : 0.;
        const double gap   = (2 + 2) * 0.2 + top_z;
        CHECK_THAT(object->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(5. + gap + 3. + 10., 1e-6));
    }
    // Specimen settings per object: the temperature axis as the mod's object parameter.
    CHECK(project.model.objects.front()->config.get().opt_string("object_mod_settings").find("\"low_temp_interface.temperature\":200") !=
          std::string::npos);
}

TEST_CASE("The interface grid goes from one specimen to the next without heating back, alternating the order by layer", "[LowTemperatureCalibration]")
{
    OfficialMods      mods;
    const std::string output =
        calibration_gcode("grid", grid_config(), grid_fields("temperature", 200., 180., 3, "none", 0., 0., 1));
    CHECK(output.find("; ltc_x_axis=low_temp_interface.temperature") != std::string::npos);
    CHECK(output.find("; ltc_x_values=200,190,180") != std::string::npos);
    CHECK(output.find("low temperature interface grid values") != std::string::npos);

    int interface_layers = 0;
    const std::vector<LayerTrace> layers = trace_layers(output);
    for (size_t index = 0; index < layers.size(); ++index) {
        const LayerTrace &layer = layers[index];
        // No support body anywhere: not in the gap, and no ring around the cube from the bed up (the grid's 2 mm
        // support_object_xy_distance; Orca's 0.35 mm left one on every layer).
        CHECK(std::count(layer.roles.begin(), layer.roles.end(), "Support") == 0);
        if (std::count(layer.roles.begin(), layer.roles.end(), "Support interface") == 0)
            continue;
        ++interface_layers;
        // Each specimen's interface after a wait for its temperature (R waits for cooling too, S for heating only),
        // then one heating back: hottest first on the first layer and every other one from there, coldest first on
        // the others (Gen 2026-10-29).
        const std::vector<std::string> expected = index % 2 == 0 ?
            std::vector<std::string>{ "M109 R200", "M109 R190", "M109 R180", "M104 S220" } :
            std::vector<std::string>{ "M109 R180", "M109 S190", "M109 S200", "M104 S220" };
        CHECK(layer.temperatures == expected);
        CHECK(layer.dwells == 1);
    }
    // The gap's interface layers (Orca may merge the 0.2 mm interface layers into fewer, thicker ones).
    CHECK(interface_layers >= 2);
}

TEST_CASE("Outside the grid a per-object interface temperature still heats back after each object", "[LowTemperatureCalibration]")
{
    OfficialMods mods;
    // The grid's objects and settings sliced as a normal print, not the calibration.
    const std::string output = calibration_gcode("grid", grid_config(), grid_fields("temperature", 200., 190., 2, "none", 0., 0., 1), nullptr, false);
    int interface_layers = 0;
    for (const LayerTrace &layer : trace_layers(output)) {
        if (std::count(layer.roles.begin(), layer.roles.end(), "Support interface") == 0)
            continue;
        ++interface_layers;
        const std::vector<std::string> expected{ "M109 R200", "M104 S220", "M109 R190", "M104 S220" };
        CHECK(layer.temperatures == expected);
        CHECK(layer.dwells == 2);
    }
    CHECK(interface_layers >= 2);
}

TEST_CASE("The interface grid refuses grids the bed cannot hold and equal axes", "[LowTemperatureCalibration]")
{
    OfficialMods             mods;
    const DynamicPrintConfig config = grid_config();
    const ModHost::CalibrationCheck fits = ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 210., 170., 5, "top_z_distance", 0., 0.2, 5));
    CHECK(fits.ok());
    CHECK(fits.summary.find("25") != std::string::npos);
    // 10 x 10 cells are 145 mm plus the labels' margins: not on a 150 mm bed.
    DynamicPrintConfig small = config;
    small.set_key_value("printable_area", new ConfigOptionPoints({ Vec2d(0, 0), Vec2d(150, 0), Vec2d(150, 150), Vec2d(0, 150) }));
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", small, grid_fields("temperature", 210., 170., 10, "top_z_distance", 0., 0.2, 10)).ok());
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 210., 170., 5, "temperature", 200., 190., 2)).ok());
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 210., 170., 11, "none", 0., 0., 1)).ok());
    // The bottom of the interface is a fixed value only, not an axis.
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", config, grid_fields("bottom_z_distance", 0., 0.2, 3, "none", 0., 0., 1)).ok());
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 210., 190., 3, "bottom_interface_layers", 0., 2., 3)).ok());
    // A bottom gap leaves a support base layer under the interface: a warning.
    ModHost::CalibrationFields gapped = grid_fields("temperature", 210., 190., 3, "none", 0., 0., 1);
    for (auto &[id, value] : gapped)
        if (id == "bottom_z_distance")
            value = "0.2";
    CHECK_FALSE(ModHost::check_calibration(MOD, "grid", config, gapped).warnings.empty());
    CHECK(ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 210., 190., 3, "none", 0., 0., 1)).warnings.empty());
    // Below the firmware's cold extrusion limit: a warning, not a refusal.
    const ModHost::CalibrationCheck cold = ModHost::check_calibration(MOD, "grid", config, grid_fields("temperature", 180., 160., 3, "none", 0., 0., 1));
    CHECK(cold.ok());
    CHECK_FALSE(cold.warnings.empty());
}

TEST_CASE("Each lid starts its gap above the cube's last layer and is back on the shared layers from its fifth layer", "[LowTemperatureCalibration]")
{
    OfficialMods mods;
    // A 0.25 mm first layer puts the shared boundaries at 0.25 + 0.2 k mm, so the 5 mm cube ends on the nearest one,
    // 5.05 mm; top Z gaps of 0 to 0.2 mm by 0.05 mm put the lids' undersides at 5.05 + (2 + 2) * 0.2 + top Z = 5.85 to
    // 6.05 mm, on a shared boundary only at 5.85 and 6.05 mm.
    const double first_layer = 0.25, layer_height = 0.2;
    DynamicPrintConfig config = grid_config();
    // The corrected third and fourth layers stay within these, mm.
    const double min_height = 0.07, max_height = 0.3;
    config.set_deserialize_strict({ { "initial_layer_print_height", first_layer }, { "min_layer_height", min_height }, { "max_layer_height", max_height } });
    ModHost::CalibrationProject project =
        ModHost::build_calibration(MOD, "grid", config, grid_fields("top_z_distance", 0., 0.2, 5, "none", 0., 0., 1));
    ModHost::apply_calibration_options(project, config, config, config);
    Print print;
    for (ModelObject *object : project.model.objects)
        print.auto_assign_extruders(object);
    print.apply(project.model, config);
    print.validate();
    print.process();

    auto on_grid = [&](double z) {
        const double k = (z - first_layer) / layer_height;
        return std::abs(k - std::round(k)) < 1e-6;
    };
    REQUIRE(print.objects().size() == 5);
    double rejoined = 0.;                    // mm
    std::vector<std::vector<double>> tops;   // per object, the print_z of every layer
    for (const PrintObject *object : print.objects()) {
        const std::string &name = object->model_object()->name;
        const double top_z      = std::stod(name.substr(name.find('Z') + 1));
        const double cube_top   = 5.05;
        const double lid_bottom = cube_top + (2 + 2) * 0.2 + top_z;
        std::vector<double> zs;
        size_t first_lid = size_t(-1);
        bool   cube_ends = false; // a layer of the cube ends at cube_top
        for (const Layer *layer : object->layers()) {
            zs.push_back(layer->print_z);
            if (std::abs(layer->print_z - cube_top) < 1e-6 && !layer->lslices.empty())
                cube_ends = true;
            if (first_lid == size_t(-1) && std::abs(layer->bottom_z() - lid_bottom) < 1e-6)
                first_lid = zs.size() - 1;
        }
        INFO(name);
        // The cube's last layer ends at its top, and the gap is measured from there: the lid's first layer begins
        // exactly the gap above it; its first two are layer_height thick.
        CHECK(cube_ends);
        CHECK_THAT(object->model_object()->instance_bounding_box(0).size().z(),
                   Catch::Matchers::WithinAbs(cube_top + (2 + 2) * 0.2 + top_z + 3. + 10., 1e-6));
        REQUIRE(first_lid != size_t(-1));
        REQUIRE(first_lid + 4 < object->layers().size());
        CHECK_THAT(object->layers()[first_lid]->height, Catch::Matchers::WithinAbs(layer_height, 1e-6));
        CHECK_THAT(object->layers()[first_lid + 1]->height, Catch::Matchers::WithinAbs(layer_height, 1e-6));
        // The fourth ends on a shared boundary, and from there every layer is one.
        const double fourth_top = object->layers()[first_lid + 3]->print_z;
        CHECK(on_grid(fourth_top));
        for (size_t l : { first_lid + 2, first_lid + 3 }) {
            CHECK(object->layers()[l]->height >= min_height - 1e-6);
            CHECK(object->layers()[l]->height <= max_height + 1e-6);
        }
        for (size_t l = first_lid + 3; l < object->layers().size(); ++l)
            CHECK(on_grid(object->layers()[l]->print_z));
        rejoined = std::max(rejoined, fourth_top);
        tops.push_back(zs);
    }
    // Above the last lid to rejoin and below the lowest specimen's top (the specimens' heights differ by their gaps),
    // every specimen prints the same layers.
    double lowest_top = std::numeric_limits<double>::max();
    for (const std::vector<double> &zs : tops)
        lowest_top = std::min(lowest_top, zs.back() - layer_height);
    auto between = [&](const std::vector<double> &zs) {
        std::vector<double> out;
        for (double z : zs)
            if (z > rejoined + 1e-6 && z < lowest_top - 1e-6)
                out.push_back(z);
        return out;
    };
    const std::vector<double> reference = between(tops.front());
    REQUIRE(reference.size() > 10);
    for (const std::vector<double> &zs : tops) {
        const std::vector<double> above = between(zs);
        REQUIRE(above.size() == reference.size());
        for (size_t k = 0; k < above.size(); ++k)
            CHECK_THAT(above[k], Catch::Matchers::WithinAbs(reference[k], 1e-6));
    }
}
