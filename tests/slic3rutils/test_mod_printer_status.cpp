// MAGPIE: the translations between printer.agent mods and Orca's device code (slic3r/Utils/ModPrinterStatus.hpp).
// Expected values are worked out from the inputs written here.
#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/ModPrinterStatus.hpp"

#include <string>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

std::string fake_filament_id(const std::string &material) { return material == "PLA" ? "GFL99" : ""; }

} // namespace

TEST_CASE("A partial neutral status updates only the fields it carries", "[ModPrinterStatus]")
{
    json cache;
    REQUIRE(merge_neutral_status(cache, json::parse(R"({"v": 1, "state": "printing", "progress": 10,
        "temps": {"nozzle": [200, 210], "bed": [60, 60]}})")));
    REQUIRE(merge_neutral_status(cache, json::parse(R"({"v": 1, "progress": 20, "temps": {"nozzle": [205, 210]}})")));
    CHECK(cache["state"] == "printing");
    CHECK(cache["progress"] == 20);
    CHECK(cache["temps"]["nozzle"] == json::array({205, 210}));
    CHECK(cache["temps"]["bed"] == json::array({60, 60})); // kept: the update had no bed
    // Another version, or no version, changes nothing.
    CHECK_FALSE(merge_neutral_status(cache, json::parse(R"({"v": 2, "progress": 99})")));
    CHECK_FALSE(merge_neutral_status(cache, json::parse(R"({"progress": 99})")));
    CHECK(cache["progress"] == 20);
}

TEST_CASE("A neutral status becomes the push message Orca's device code reads", "[ModPrinterStatus]")
{
    const json status = json::parse(R"({"v": 1, "state": "paused", "progress": 42.4, "layer": 12, "total_layers": 100,
        "remaining_s": 61, "file": "cube.gcode",
        "temps": {"nozzle": [215.5, 220], "bed": [59, 60], "chamber": [30, 0]},
        "fans": {"part": 100, "aux": 50, "box": 2}, "speed_mode": 3, "light": {"chamber": false, "camera": true},
        "caps": {"nozzle_max": 300, "bed_max": 110}, "some_future_field": {"x": 1}})");
    const json p = neutral_status_to_device_push(status, fake_filament_id)["print"];
    CHECK(p["command"] == "push_status");
    CHECK(p["gcode_state"] == "PAUSE");
    CHECK(p["mc_print_stage"] == 2);
    CHECK(p["mc_percent"] == 42);
    CHECK(p["mc_remaining_time"] == 2); // 61 s is a started second minute
    CHECK(p["layer_num"] == 12);
    CHECK(p["total_layer_num"] == 100);
    CHECK(p["subtask_name"] == "cube.gcode");
    CHECK(p["nozzle_temper"] == 215.5);
    CHECK(p["nozzle_target_temper"] == 220);
    CHECK(p["bed_target_temper"] == 60);
    CHECK(p["chamber_temper"] == 30);
    // part 100 % -> 255 in byte 0, aux 50 % -> round(127.5) = 128 in byte 1: 128 * 256 + 255.
    CHECK(p["fan_gear"] == 128 * 256 + 255);
    CHECK(p["spd_lvl"] == 3);
    CHECK(p["lights_report"][0]["mode"] == "off");
    CHECK(p["nozzle_temp_range"] == json::array({0, 300}));
    CHECK(p["print_error"] == 0);
    CHECK_FALSE(p.contains("ams"));
}

TEST_CASE("Neutral states map to Orca's print states", "[ModPrinterStatus]")
{
    const std::pair<const char *, const char *> cases[] = {{"idle", "IDLE"},     {"preparing", "PREPARE"}, {"printing", "RUNNING"},
                                                          {"finished", "FINISH"}, {"failed", "FAILED"},     {"offline", "IDLE"},
                                                          {"unknown_word", "IDLE"}};
    for (const auto &[in, out] : cases)
        CHECK(neutral_status_to_device_push(json{{"v", 1}, {"state", in}}, nullptr)["print"]["gcode_state"] == out);
}

TEST_CASE("Material units become Orca's AMS units", "[ModPrinterStatus]")
{
    const json status = json::parse(R"({"v": 1, "units": [
        {"id": 0, "humidity": 31.6, "trays": [
            {"slot": 0, "material": "PLA", "color": "#ff8800", "state": "loaded", "remain_pct": 80},
            {"slot": 1, "material": "", "state": "empty"},
            {"slot": 2, "material": "PETG", "color": "00FF00", "state": "loaded"},
            {"slot": 3, "material": "PLA", "color": "bad", "state": "feeding"},
            {"slot": 4, "material": "PLA", "color": "#000000", "state": "loaded"}]},
        {"id": 1, "trays": [{"slot": 0, "material": "ABS", "color": "#123456", "state": "loaded"}]}]})");
    const json ams = neutral_status_to_device_push(status, fake_filament_id)["print"]["ams"];
    REQUIRE(ams["ams"].size() == 2);
    CHECK(ams["ams_exist_bits"] == "3");
    // Unit 0: slots 0, 2, 3 loaded (bits 0, 2, 3); unit 1: slot 0 (bit 4) -> 0b11101 = 0x1D. The fifth tray is cut.
    CHECK(ams["tray_exist_bits"] == "1D");
    const json &trays = ams["ams"][0]["tray"];
    REQUIRE(trays.size() == 4);
    CHECK(trays[0]["tray_color"] == "FF8800FF");
    CHECK(trays[0]["tray_info_idx"] == "GFL99");
    CHECK(trays[0]["remain"] == 80);
    CHECK(trays[1]["tray_slot_placeholder"] == "1");
    CHECK(trays[2]["tray_info_idx"] == "");
    CHECK(trays[3]["tray_color"] == "00000000");
    CHECK(ams["ams"][0]["humidity_raw"] == "32");
}

TEST_CASE("Tray colours are normalised to RRGGBBAA", "[ModPrinterStatus]")
{
    CHECK(device_tray_color("#a1b2c3") == "A1B2C3FF");
    CHECK(device_tray_color("0xA1B2C3") == "A1B2C3FF");
    CHECK(device_tray_color("A1B2C380") == "A1B2C380");
    CHECK(device_tray_color(" #a1b2c3 ") == "A1B2C3FF");
    for (const char *bad : {"", "#abc", "#a1b2c3d", "#GGGGGG", "red"})
        CHECK(device_tray_color(bad) == "00000000");
}

TEST_CASE("Device tab commands become neutral commands, and raw G-code does not", "[ModPrinterStatus]")
{
    auto neutral = [](const char *text) { return device_command_to_neutral(json::parse(text)); };
    CHECK(*neutral(R"({"print": {"command": "pause"}})") == json{{"cmd", "pause"}});
    CHECK(*neutral(R"({"print": {"command": "stop"}})") == json{{"cmd", "stop"}});
    CHECK((*neutral(R"({"print": {"command": "set_bed_temp", "temp": 60}})"))["heater"] == "bed");
    CHECK((*neutral(R"({"print": {"command": "set_nozzle_temp", "target_temp": 210, "extruder_index": 0}})"))["target"] == 210);
    CHECK((*neutral(R"({"print": {"command": "print_speed", "param": "4"}})"))["mode"] == 3); // ludicrous -> fastest of 3
    CHECK((*neutral(R"({"print": {"command": "print_speed", "param": "1"}})"))["mode"] == 1);
    const json fan = *neutral(R"({"print": {"command": "gcode_line", "param": "M106 P2 S255 \n"}})");
    CHECK(fan["fan"] == "aux");
    CHECK(fan["pct"] == 100);
    CHECK((*neutral(R"({"print": {"command": "gcode_line", "param": "M106 P1 S128\n"}})"))["pct"] == 50); // round(50.2)
    CHECK((*neutral(R"({"print": {"command": "gcode_line", "param": "M140 S70\n"}})"))["heater"] == "bed");
    CHECK((*neutral(R"({"print": {"command": "gcode_line", "param": "G28 \n"}})"))["cmd"] == "home");
    CHECK((*neutral(R"({"system": {"command": "ledctrl", "led_node": "chamber_light", "led_mode": "on"}})"))["on"] == true);
    // Anything else is not passed on: arbitrary G-code, a second line, a moved fan index, unknown commands.
    for (const char *text : {R"({"print": {"command": "gcode_line", "param": "G1 X10 F3000\n"}})",
                             R"({"print": {"command": "gcode_line", "param": "M106 P1 S255\nG28\n"}})",
                             R"({"print": {"command": "gcode_line", "param": "M106 P4 S255"}})",
                             R"({"print": {"command": "print_speed", "param": "5"}})",
                             R"({"print": {"command": "calibration"}})",
                             R"({"system": {"command": "ledctrl", "led_node": "chamber_light2", "led_mode": "on"}})",
                             R"({"info": {"command": "get_version"}})", R"([1, 2])"})
        CHECK_FALSE(device_command_to_neutral(json::parse(text)).has_value());
}
