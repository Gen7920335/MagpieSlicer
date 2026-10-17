#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/SnapmakerHomingPolicy.hpp"

#include <string>

TEST_CASE("Snapmaker U1 homing follows PRINT_START before motion", "[SnapmakerU1]")
{
    std::string unsafe =
        "G28\n"
        "PRINT_START TOOL=0\n"
        "M84\n"
        "M109 S220\n"
        "G0 X0 Y0 Z10\n";
    CHECK_FALSE(Slic3r::snapmaker_u1_has_safe_homing(unsafe));
    CHECK(Slic3r::ensure_snapmaker_u1_safe_homing(unsafe));
    CHECK(Slic3r::snapmaker_u1_has_safe_homing(unsafe));

    const size_t print_start = unsafe.find("PRINT_START TOOL=0");
    const size_t inserted_home = unsafe.find(
        "; Magpie: mandatory Snapmaker U1 homing after PRINT_START\nG28\n");
    const size_t first_motion = unsafe.find("G0 X0 Y0 Z10");
    REQUIRE(print_start != std::string::npos);
    REQUIRE(inserted_home != std::string::npos);
    REQUIRE(first_motion != std::string::npos);
    CHECK(print_start < inserted_home);
    CHECK(inserted_home < first_motion);
    CHECK_FALSE(Slic3r::ensure_snapmaker_u1_safe_homing(unsafe));

    CHECK(Slic3r::snapmaker_u1_has_safe_homing(
        "PRINT_STRAT TOOL=0\nG28\nG0 X0 Y0 Z10\n"));
    CHECK(Slic3r::snapmaker_u1_has_safe_homing(
        "PRINT_START\nG28 X Y\nG28 Z I140 J140\nG0 Z5\n"));
    CHECK_FALSE(Slic3r::snapmaker_u1_has_safe_homing(
        "PRINT_START TOOL=0\nG28 X Y\nG0 Z10\n"));
    CHECK_FALSE(Slic3r::snapmaker_u1_has_safe_homing(
        "PRINT_START TOOL=0\n; G28\nG0 Z10\n"));
}

TEST_CASE("Snapmaker U1 incomplete start code uses native sequence", "[SnapmakerU1]")
{
    const std::string configured =
        "PRINT_START TOOL_TEMP=220 BED_TEMP=55\n"
        "M109 S220\n"
        "G0 X0 Y0 Z10\n";
    const std::string selected = Slic3r::snapmaker_u1_start_gcode_template(configured);

    CHECK(selected != configured);
    CHECK(Slic3r::snapmaker_u1_has_native_start(selected));
    CHECK(selected.find("M190 S{bed_temperature_initial_layer_single}") != std::string::npos);
    CHECK(selected.find("BED_MESH_CALIBRATE PROBE_COUNT=11,11") != std::string::npos);
    CHECK(selected.find("Z_OFFSET=-0.07") != std::string::npos);

    const std::string customized_native =
        "PRINT_START\n"
        "M140 S55\n"
        "G28 X Y\n"
        "G28 Z I140 J140\n"
        "M190 S55\n"
        "G28 Z\n"
        "BED_MESH_CALIBRATE\n"
        "M109 S220\n"
        "G1 X185 E15 F360\n"
        "; user customization\n";
    CHECK(Slic3r::snapmaker_u1_start_gcode_template(customized_native) == customized_native);
}

TEST_CASE("Snapmaker U1 stale inherited toolchanger start code is replaced", "[SnapmakerU1]")
{
    const std::string stale_toolchanger_start =
        "PRINT_START TOOL_TEMP=220 T0_TEMP=220 BED_TEMP=55 TOOL=0\n"
        "M83\n"
        "M109 T0 S220\n"
        "T0\n"
        "G92 E0\n"
        "G0 X0 Y0 Z10 F12000\n";

    CHECK(Slic3r::is_snapmaker_u1_model("Snapmaker U1"));
    CHECK(Slic3r::is_snapmaker_u1_model("797581801"));
    CHECK_FALSE(Slic3r::is_snapmaker_u1_model("MyToolChanger"));
    CHECK_FALSE(Slic3r::snapmaker_u1_has_native_start(stale_toolchanger_start));
    CHECK_FALSE(Slic3r::snapmaker_u1_has_safe_homing(stale_toolchanger_start));

    const std::string selected = Slic3r::machine_start_gcode_template_for_printer(
        "Snapmaker U1", stale_toolchanger_start);
    CHECK(selected != stale_toolchanger_start);
    CHECK(Slic3r::snapmaker_u1_has_native_start(selected));
    CHECK(Slic3r::snapmaker_u1_has_safe_homing(selected));
    CHECK(selected.find("G0 X0 Y0 Z10 F12000") == std::string::npos);

    CHECK(Slic3r::machine_start_gcode_template_for_printer(
        "MyToolChanger", stale_toolchanger_start) == stale_toolchanger_start);
}

