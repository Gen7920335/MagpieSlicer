#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/SnapmakerHomingPolicy.hpp"
#include "slic3r/GUI/DeviceTab/SnapmakerGCodeLayer.hpp"
#include "slic3r/GUI/DeviceTab/SnapmakerMonitorUtils.hpp"

#include <cmath>
#include <string>

using Slic3r::GUI::SnapmakerGCodeLayer;
using Slic3r::GUI::SnapmakerGCodeLayerCache;
using Slic3r::GUI::is_public_snapmaker_macro;
using Slic3r::GUI::snapmaker_heater_script;
using Slic3r::GUI::snapmaker_jog_script;
using Slic3r::GUI::snapmaker_extrude_script;
using Slic3r::GUI::snapmaker_z_offset_script;
using Slic3r::GUI::snapmaker_motion_limit_script;
using Slic3r::GUI::snapmaker_pressure_advance_script;
using Slic3r::GUI::snapmaker_bed_mesh_profile_script;

namespace {

void check_point(const Slic3r::GUI::SnapmakerGCodePoint &point, double x, double y)
{
    CHECK(point.x == Catch::Approx(x));
    CHECK(point.y == Catch::Approx(y));
}

std::shared_ptr<SnapmakerGCodeLayerCache> parse(const std::string &gcode)
{
    return SnapmakerGCodeLayerCache::parse(gcode);
}

} // namespace

TEST_CASE("Snapmaker layer parser handles Orca absolute extrusion", "[SnapmakerMonitor][GCode]")
{
    const auto cache = parse(
        "; total layer number: 2\n"
        "G90\n"
        "M82\n"
        "G92 E0\n"
        ";LAYER_CHANGE\n"
        "G1 X10 Y10 F6000\n"
        "G1 X20 Y10 E1\n"
        ";LAYER_CHANGE\n"
        "G1 X20 Y20 E2\n");

    REQUIRE(cache->layer_count() == 2);
    const SnapmakerGCodeLayer first = cache->layer(1);
    REQUIRE(first.segments.size() == 1);
    check_point(first.segments[0].from, 10.0, 10.0);
    check_point(first.segments[0].to, 20.0, 10.0);

    const SnapmakerGCodeLayer second = cache->layer(2);
    REQUIRE(second.segments.size() == 1);
    check_point(second.segments[0].from, 20.0, 10.0);
    check_point(second.segments[0].to, 20.0, 20.0);
}

TEST_CASE("Snapmaker layer parser handles relative modes and reset", "[SnapmakerMonitor][GCode]")
{
    const auto cache = parse(
        "G90\n"
        "M83\n"
        ";LAYER_CHANGE\n"
        "G1 X5 Y5\n"
        "G1 X10 E0.5\n"
        "G1 E-0.2\n"
        "G1 X15 E0.4\n"
        "G91\n"
        "G1 X5 Y5 E0.3\n"
        "G90\n"
        "G92 X100 Y200 E0\n"
        "G1 X101 Y201 E0.2\n");

    const auto layer = cache->layer(1);
    REQUIRE(layer.segments.size() == 4);
    check_point(layer.segments[0].from, 5.0, 5.0);
    check_point(layer.segments[0].to, 10.0, 5.0);
    check_point(layer.segments[1].from, 10.0, 5.0);
    check_point(layer.segments[1].to, 15.0, 5.0);
    check_point(layer.segments[2].from, 15.0, 5.0);
    check_point(layer.segments[2].to, 20.0, 10.0);
    check_point(layer.segments[3].from, 100.0, 200.0);
    check_point(layer.segments[3].to, 101.0, 201.0);
}

TEST_CASE("Snapmaker layer parser keeps extrusion state per tool", "[SnapmakerMonitor][GCode]")
{
    const auto cache = parse(
        "G90\n"
        "M82\n"
        ";LAYER_CHANGE\n"
        "T0\n"
        "G92 E0\n"
        "G1 X10 E1\n"
        "T1\n"
        "G92 E0\n"
        "G1 X20 E1\n"
        "T0\n"
        "G1 X30 E2\n");

    const auto layer = cache->layer(1);
    REQUIRE(layer.segments.size() == 3);
    CHECK(layer.segments[0].tool == 0);
    CHECK(layer.segments[1].tool == 1);
    CHECK(layer.segments[2].tool == 0);
}

TEST_CASE("Snapmaker layer parser tessellates IJ arcs", "[SnapmakerMonitor][GCode]")
{
    const auto cache = parse(
        "G90\n"
        "M83\n"
        ";LAYER_CHANGE\n"
        "G1 X10 Y0\n"
        "G3 X0 Y10 I-10 J0 E1\n");

    const auto layer = cache->layer(1);
    REQUIRE(layer.segments.size() > 2);
    check_point(layer.segments.front().from, 10.0, 0.0);
    check_point(layer.segments.back().to, 0.0, 10.0);
}

TEST_CASE("Snapmaker layer parser supports Cura and numbered markers", "[SnapmakerMonitor][GCode]")
{
    SECTION("Cura")
    {
        const auto cache = parse(
            "M83\n"
            ";LAYER:0\n"
            "G1 X1 E1\n"
            ";LAYER:1\n"
            "G1 X2 E1\n");
        REQUIRE(cache->layer_count() == 2);
        REQUIRE(cache->layer(2).segments.size() == 1);
    }

    SECTION("Numbered")
    {
        const auto cache = parse(
            "; layer num/total_layer_count: 1/2\n"
            "M83\n"
            "G1 X1 E1\n"
            "; layer num/total_layer_count: 2/2\n"
            "G1 X2 E1\n");
        REQUIRE(cache->layer_count() == 2);
        REQUIRE(cache->layer(2).segments.size() == 1);
    }
}

TEST_CASE("Snapmaker layer parser tags exclude objects", "[SnapmakerMonitor][GCode]")
{
    const auto cache = parse(
        "M83\n"
        ";LAYER_CHANGE\n"
        "EXCLUDE_OBJECT_START NAME=part_one\n"
        "G1 X10 E1\n"
        "EXCLUDE_OBJECT_END NAME=part_one\n"
        "G1 X20 E1\n");

    const auto layer = cache->layer(1);
    REQUIRE(layer.segments.size() == 2);
    CHECK(layer.segments[0].object == "part_one");
    CHECK(layer.segments[1].object.empty());
}

TEST_CASE("Snapmaker layer parser rejects invalid input and bounds", "[SnapmakerMonitor][GCode]")
{
    CHECK_THROWS_AS(parse("G1 X10 Y10 E1\n"), std::runtime_error);

    const auto cache = parse(
        ";LAYER_CHANGE\n"
        "G1 X10 Y10 E1\n");
    CHECK(cache->layer(0).segments.empty());
    CHECK(cache->layer(-1).segments.empty());
    CHECK(cache->layer(2).segments.empty());
}

TEST_CASE("Snapmaker monitor normalizes server URLs", "[SnapmakerMonitor][Server]")
{
    using Slic3r::GUI::normalize_snapmaker_base_url;

    CHECK(normalize_snapmaker_base_url("http://192.168.0.32/") == "http://192.168.0.32");
    CHECK(normalize_snapmaker_base_url("http://printer.local:7125/path/api") == "http://printer.local:7125");
    CHECK(normalize_snapmaker_base_url("192.168.0.32///") == "192.168.0.32");
    CHECK(normalize_snapmaker_base_url("") == "");
}

TEST_CASE("Snapmaker camera endpoint parsing covers IPv4 IPv6 and ports", "[SnapmakerMonitor][Camera]")
{
    using Slic3r::GUI::parse_snapmaker_websocket_endpoint;

    const auto ipv4 = parse_snapmaker_websocket_endpoint("http://192.168.0.32");
    CHECK(ipv4.host == "192.168.0.32");
    CHECK(ipv4.port == "80");
    CHECK(ipv4.host_header == "192.168.0.32");

    const auto named = parse_snapmaker_websocket_endpoint("http://printer.local:7125/path");
    CHECK(named.host == "printer.local");
    CHECK(named.port == "7125");
    CHECK(named.host_header == "printer.local:7125");

    const auto ipv6 = parse_snapmaker_websocket_endpoint("http://[fe80::1]:7125");
    CHECK(ipv6.host == "fe80::1");
    CHECK(ipv6.port == "7125");
    CHECK(ipv6.host_header == "[fe80::1]:7125");

    CHECK_THROWS(parse_snapmaker_websocket_endpoint("https://printer.local"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://[fe80::1"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://printer.local:"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://printer.local:http"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://printer.local:0"));
    CHECK_THROWS(parse_snapmaker_websocket_endpoint("http://printer.local:65536"));
}

TEST_CASE("Snapmaker object names are command safe", "[SnapmakerMonitor][Control]")
{
    using Slic3r::GUI::is_valid_snapmaker_object_name;

    CHECK(is_valid_snapmaker_object_name("PART_1"));
    CHECK(is_valid_snapmaker_object_name("part-1.2"));
    CHECK_FALSE(is_valid_snapmaker_object_name(""));
    CHECK_FALSE(is_valid_snapmaker_object_name("PART 1"));
    CHECK_FALSE(is_valid_snapmaker_object_name("PART\nCANCEL_PRINT"));
    CHECK_FALSE(is_valid_snapmaker_object_name("한글"));
}

TEST_CASE("Snapmaker public macros hide internal and tool aliases", "[SnapmakerMonitor][Control]")
{
    CHECK(is_public_snapmaker_macro("BED_MESH_CALIBRATE"));
    CHECK(is_public_snapmaker_macro("SHAPER_CALIBRATE"));
    CHECK_FALSE(is_public_snapmaker_macro("_CLIENT_EXTRUDE"));
    CHECK_FALSE(is_public_snapmaker_macro("T0"));
    CHECK_FALSE(is_public_snapmaker_macro("T31"));
    CHECK_FALSE(is_public_snapmaker_macro("PAUSE"));
    CHECK_FALSE(is_public_snapmaker_macro("BAD COMMAND"));
}

TEST_CASE("Snapmaker control scripts validate motion and heater inputs", "[SnapmakerMonitor][Control]")
{
    CHECK(snapmaker_jog_script('x', 10.0) == "G91\nG0 X10.000 F6000\nG90");
    CHECK(snapmaker_jog_script('Z', -0.1) == "G91\nG0 Z-0.100 F600\nG90");
    CHECK_THROWS(snapmaker_jog_script('E', 1.0));
    CHECK_THROWS(snapmaker_jog_script('X', 0.0));
    CHECK_THROWS(snapmaker_jog_script('X', 101.0));

    CHECK(snapmaker_heater_script("extruder3", 215.0) ==
          "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=215.0");
    CHECK(snapmaker_heater_script("heater_bed", 60.0) ==
          "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60.0");
    CHECK_THROWS(snapmaker_heater_script("heater bed", 60.0));
    CHECK_THROWS(snapmaker_heater_script("extruder", 401.0));
}

TEST_CASE("Snapmaker advanced controls generate bounded Klipper commands", "[SnapmakerMonitor][Control]")
{
    CHECK(snapmaker_extrude_script(10.0, 5.0) ==
          "SAVE_GCODE_STATE NAME=MAGPIE_MANUAL_EXTRUDE\nM83\nG1 E10.000 F300.0\n"
          "RESTORE_GCODE_STATE NAME=MAGPIE_MANUAL_EXTRUDE");
    CHECK(snapmaker_extrude_script(-2.5, 1.5).find("G1 E-2.500 F90.0") != std::string::npos);
    CHECK_NOTHROW(snapmaker_extrude_script(0.1, 0.1));
    CHECK_NOTHROW(snapmaker_extrude_script(-100.0, 100.0));
    CHECK_NOTHROW(snapmaker_extrude_script(100.0, 50.0));
    CHECK_THROWS(snapmaker_extrude_script(0.0, 5.0));
    CHECK_THROWS(snapmaker_extrude_script(101.0, 5.0));
    CHECK_THROWS(snapmaker_extrude_script(10.0, 0.0));

    CHECK(snapmaker_z_offset_script(0.05) == "SET_GCODE_OFFSET Z_ADJUST=0.050 MOVE=1");
    CHECK(snapmaker_z_offset_script(-1.0) == "SET_GCODE_OFFSET Z_ADJUST=-1.000 MOVE=1");
    CHECK_NOTHROW(snapmaker_z_offset_script(0.005));
    CHECK_NOTHROW(snapmaker_z_offset_script(-0.005));
    CHECK_NOTHROW(snapmaker_z_offset_script(1.0));
    CHECK_THROWS(snapmaker_z_offset_script(0.0));
    CHECK_THROWS(snapmaker_z_offset_script(1.001));

    CHECK(snapmaker_motion_limit_script(300.0, 10000.0, 5.0) ==
          "SET_VELOCITY_LIMIT VELOCITY=300.0 ACCEL=10000.0 SQUARE_CORNER_VELOCITY=5.0");
    CHECK_NOTHROW(snapmaker_motion_limit_script(1.0, 100.0, 0.1));
    CHECK_NOTHROW(snapmaker_motion_limit_script(500.0, 25000.0, 50.0));
    CHECK_NOTHROW(snapmaker_motion_limit_script(1000.0, 50000.0, 100.0));
    CHECK_THROWS(snapmaker_motion_limit_script(0.0, 10000.0, 5.0));
    CHECK_THROWS(snapmaker_motion_limit_script(300.0, 50001.0, 5.0));

    CHECK(snapmaker_pressure_advance_script("extruder3", 0.035, 0.04) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder3 ADVANCE=0.0350 SMOOTH_TIME=0.0400");
    CHECK_NOTHROW(snapmaker_pressure_advance_script("extruder", 0.0, 0.001));
    CHECK_NOTHROW(snapmaker_pressure_advance_script("extruder1", 1.0, 0.5));
    CHECK_NOTHROW(snapmaker_pressure_advance_script("extruder31", 2.0, 1.0));
    CHECK_THROWS(snapmaker_pressure_advance_script("heater_bed", 0.035, 0.04));
    CHECK_THROWS(snapmaker_pressure_advance_script("extruder 1", 0.035, 0.04));
    CHECK_THROWS(snapmaker_pressure_advance_script("extruder", 2.1, 0.04));

    CHECK(snapmaker_bed_mesh_profile_script("SAVE", "default") == "BED_MESH_PROFILE SAVE=default");
    CHECK(snapmaker_bed_mesh_profile_script("LOAD", "u1-pla") == "BED_MESH_PROFILE LOAD=u1-pla");
    CHECK(snapmaker_bed_mesh_profile_script("REMOVE", "mesh_01") == "BED_MESH_PROFILE REMOVE=mesh_01");
    CHECK_THROWS(snapmaker_bed_mesh_profile_script("IMPORT", "default"));
    CHECK_THROWS(snapmaker_bed_mesh_profile_script("SAVE", "bad profile"));
}

TEST_CASE("Snapmaker U1 homing follows PRINT_START before motion", "[SnapmakerMonitor][GCode]")
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

TEST_CASE("Snapmaker U1 incomplete start code uses native sequence", "[SnapmakerMonitor][GCode]")
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

TEST_CASE("Snapmaker control availability follows printer state", "[SnapmakerMonitor][Control]")
{
    using Slic3r::GUI::snapmaker_control_availability;

    const auto printing = snapmaker_control_availability(true, false, true, "printing", true, true);
    CHECK(printing.pause_resume);
    CHECK(printing.cancel);
    CHECK(printing.refresh);
    CHECK(printing.skip_object);
    CHECK_FALSE(printing.resume_mode);

    const auto paused = snapmaker_control_availability(true, false, true, "paused", true, true);
    CHECK(paused.pause_resume);
    CHECK(paused.cancel);
    CHECK(paused.skip_object);
    CHECK(paused.resume_mode);

    const auto idle = snapmaker_control_availability(true, false, true, "standby", true, true);
    CHECK_FALSE(idle.pause_resume);
    CHECK_FALSE(idle.cancel);
    CHECK_FALSE(idle.skip_object);
    CHECK(idle.refresh);

    const auto busy = snapmaker_control_availability(true, true, true, "printing", true, true);
    CHECK_FALSE(busy.pause_resume);
    CHECK_FALSE(busy.cancel);
    CHECK_FALSE(busy.refresh);
    CHECK_FALSE(busy.skip_object);

    const auto disconnected = snapmaker_control_availability(false, false, true, "printing", true, true);
    CHECK_FALSE(disconnected.pause_resume);
    CHECK_FALSE(disconnected.cancel);
    CHECK_FALSE(disconnected.skip_object);
    CHECK(disconnected.refresh);
}

TEST_CASE("Snapmaker layer bounds reject mismatched printer metadata", "[SnapmakerMonitor][GCode]")
{
    using Slic3r::GUI::valid_snapmaker_layer_number;

    CHECK(valid_snapmaker_layer_number(1, 1) == 1);
    CHECK(valid_snapmaker_layer_number(748, 748) == 748);
    CHECK(valid_snapmaker_layer_number(0, 748) == 0);
    CHECK(valid_snapmaker_layer_number(-1, 748) == 0);
    CHECK(valid_snapmaker_layer_number(749, 748) == 0);
    CHECK(valid_snapmaker_layer_number(1, 0) == 0);
}

TEST_CASE("Snapmaker HTTP status classification is explicit", "[SnapmakerMonitor][Network]")
{
    using Slic3r::GUI::is_success_http_status;

    CHECK(is_success_http_status(200));
    CHECK(is_success_http_status(204));
    CHECK(is_success_http_status(299));
    CHECK_FALSE(is_success_http_status(0));
    CHECK_FALSE(is_success_http_status(199));
    CHECK_FALSE(is_success_http_status(300));
    CHECK_FALSE(is_success_http_status(404));
    CHECK_FALSE(is_success_http_status(500));
}
