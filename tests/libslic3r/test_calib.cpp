#include <catch2/catch_all.hpp>

#include <array>

#include "libslic3r/calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// The width-resolution getters are protected; expose them so the resolution can be asserted directly.
struct PaPatternProbe : public CalibPressureAdvancePattern
{
    using CalibPressureAdvancePattern::CalibPressureAdvancePattern;
    using CalibPressureAdvancePattern::line_width;
    using CalibPressureAdvancePattern::line_width_first_layer;
};

} // namespace

TEST_CASE("Zero calibration line width resolves to a positive default", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"line_width", "0"},
        {"initial_layer_line_width", "0"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode = CalibMode::Calib_PA_Pattern;

    PaPatternProbe pattern(params, config, /* is_bbl_machine */ true, *model.objects.front(), Vec3d(0, 0, 0));

    REQUIRE(pattern.line_width() > 0.);
    REQUIRE(pattern.line_width_first_layer() > 0.);
}

TEST_CASE("LESIC layout follows bed and nozzle boundaries", "[Calib][LESIC]")
{
    Calib_Params params;
    params.start = 210.0;
    params.end = 170.0;
    params.step = 10.0;
    params.lesic_layers_per_temp = 10;

    struct Case {
        double bed;
        double nozzle;
        double diameter;
        bool small_bed;
    };

    const std::array<Case, 5> cases {{
        {120.0, 0.15, 100.0, true},
        {199.0, 0.20, 179.0, true},
        {200.0, 0.40, 180.0, false},
        {220.0, 0.60, 200.0, false},
        {350.0, 1.00, 330.0, false},
    }};

    for (const Case &test : cases) {
        const LesicCalibrationLayout layout =
            make_lesic_calibration_layout(test.bed, test.bed + 20.0, test.nozzle, params);
        CAPTURE(test.bed, test.nozzle);
        CHECK(layout.circle_diameter == Catch::Approx(test.diameter));
        CHECK(layout.line_width == Catch::Approx(test.nozzle * 1.2));
        CHECK(layout.layer_height == Catch::Approx(test.nozzle * 0.6));
        CHECK(layout.temperature_bands == 5);
        CHECK(layout.model_height == Catch::Approx(test.nozzle * 0.6 * 10.0 * 5.0));
        CHECK(layout.small_bed == test.small_bed);
    }
}

TEST_CASE("LESIC temperature ranges include the requested endpoint", "[Calib][LESIC]")
{
    struct Case {
        double start;
        double end;
        double step;
        int bands;
    };

    const std::array<Case, 6> cases {{
        {210.0, 165.0, 1.0, 46},
        {210.0, 165.0, 2.0, 24},
        {210.0, 165.0, 45.0, 2},
        {165.0, 210.0, 7.0, 8},
        {210.0, 210.0, 1.0, 0},
        {210.0, 205.0, 0.0, 0},
    }};

    for (const Case &test : cases) {
        CAPTURE(test.start, test.end, test.step);
        const int bands = lesic_temperature_band_count(test.start, test.end, test.step);
        CHECK(bands == test.bands);
        if (bands > 0)
            CHECK(lesic_temperature_for_band(test.start, test.end, test.step, bands - 1) ==
                  Catch::Approx(test.end));
    }
}

TEST_CASE("LESIC layer count scales model height without changing its footprint", "[Calib][LESIC]")
{
    Calib_Params params;
    params.start = 220.0;
    params.end = 170.0;
    params.step = 10.0;

    double previous_height = 0.0;
    for (int layers_per_temp : {1, 10, 100}) {
        params.lesic_layers_per_temp = layers_per_temp;
        const LesicCalibrationLayout layout =
            make_lesic_calibration_layout(256.0, 256.0, 0.4, params);
        CAPTURE(layers_per_temp);
        CHECK(layout.circle_diameter == Catch::Approx(236.0));
        CHECK(layout.model_height == Catch::Approx(0.24 * layers_per_temp * 6.0));
        CHECK(layout.model_height > previous_height);
        previous_height = layout.model_height;
    }
}
