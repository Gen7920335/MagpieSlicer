// MAGPIE: LESIC ring calibration (Magpie line 1), ported from its test_print.cpp.
#include <catch2/catch_all.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"

#include "test_helpers.hpp"

#include <regex>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("LESIC calibration slices a cylinder and emits calibration G-code", "[LESIC]")
{
    Calib_Params params;
    params.mode = CalibMode::Calib_LESIC;
    params.start = 210.0;
    params.end = 205.0;
    params.step = 2.0;
    params.mvs_start = 8.0;
    params.mvs_end = 24.0;
    params.lesic_layers_per_temp = 1;

    const LesicCalibrationLayout layout =
        make_lesic_calibration_layout(120.0, 120.0, 0.4, params);
    params.lesic_circle_diameter = layout.circle_diameter;
    params.lesic_line_width = layout.line_width;
    params.lesic_layer_height = layout.layer_height;
    params.lesic_small_bed = layout.small_bed;

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height", new ConfigOptionFloat(layout.layer_height));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layout.layer_height));
    config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(layout.line_width, false));
    config.set_key_value("wall_loops", new ConfigOptionInt(1));
    config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    config.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    config.set_key_value("spiral_mode", new ConfigOptionBool(true));

    TriangleMesh cylinder(its_make_cylinder(
        layout.circle_diameter * 0.5, layout.model_height, PI / 180.0));
    Print print;
    Model model;
    init_print({cylinder}, print, model, config);

    const Vec3d center = model.objects.front()->bounding_box_exact().center();
    params.lesic_center_x = center.x();
    params.lesic_center_y = center.y();
    print.set_calib_params(params);

    const std::string output = gcode(print);
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
    const auto ring_gcode = [](CalibMode mode) {
        Calib_Params params;
        params.mode = mode;
        params.start = 210.0;
        params.end = 205.0;
        params.step = 5.0;
        params.mvs_start = 8.0;
        params.mvs_end = 24.0;
        params.lesic_layers_per_temp = 2;
        const LesicCalibrationLayout layout = make_lesic_calibration_layout(120.0, 120.0, 0.4, params);
        params.lesic_circle_diameter = layout.circle_diameter;
        params.lesic_line_width = layout.line_width;
        params.lesic_layer_height = layout.layer_height;

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "layer_height", layout.layer_height },
            { "initial_layer_print_height", layout.layer_height },
            { "wall_loops", 1 },
            { "top_shell_layers", 0 },
            { "bottom_shell_layers", 0 },
            { "sparse_infill_density", "0%" },
            { "spiral_mode", true },
            // A slow volumetric limit would cap the ring's top speeds.
            { "filament_max_volumetric_speed", 40 },
        });
        TriangleMesh cylinder(its_make_cylinder(layout.circle_diameter * 0.5, layout.model_height, PI / 180.0));
        Print print;
        Model model;
        init_print({ cylinder }, print, model, config);
        const Vec3d center = model.objects.front()->bounding_box_exact().center();
        params.lesic_center_x = center.x();
        params.lesic_center_y = center.y();
        print.set_calib_params(params);
        return gcode(print);
    };

    // Distinct feed rates (mm/min) after the first layer.
    const auto ring_feeds = [](const std::string &gcode) {
        std::set<int> feeds;
        const size_t second_layer = gcode.find(";LAYER_CHANGE", gcode.find(";LAYER_CHANGE") + 1);
        static const std::regex move(R"(G1 [^;\n]*F([0-9]+))");
        for (std::sregex_iterator it(gcode.begin() + second_layer, gcode.end(), move), end; it != end; ++it)
            feeds.insert(std::stoi((*it)[1].str()));
        return feeds;
    };

    const std::string lesic = ring_gcode(CalibMode::Calib_LESIC);
    const std::string plain = ring_gcode(CalibMode::Calib_None);
    // 8 to 24 mm3/s around the ring gives many speeds; the plain vase prints at one wall speed.
    CHECK(ring_feeds(lesic).size() > 20);
    CHECK(ring_feeds(plain).size() < 5);
    CHECK(plain.find("; LESIC") == std::string::npos);
    CHECK(plain.find("lesic_") == std::string::npos);
}
