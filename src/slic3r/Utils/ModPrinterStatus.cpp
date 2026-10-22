#include "ModPrinterStatus.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <regex>
#include <sstream>

namespace Slic3r {

using json = nlohmann::json;

namespace {

void merge_objects(json &into, const json &from)
{
    for (auto it = from.begin(); it != from.end(); ++it) {
        if (it.value().is_object() && into.contains(it.key()) && into[it.key()].is_object())
            merge_objects(into[it.key()], it.value());
        else
            into[it.key()] = it.value();
    }
}

// [current, target] of a neutral temperature pair, degrees C.
bool temperature_pair(const json &temps, const char *key, double &current, double &target)
{
    auto it = temps.find(key);
    if (it == temps.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
        return false;
    current = (*it)[0].get<double>();
    target  = (*it)[1].get<double>();
    return true;
}

// A fan percentage 0..100 as the 0..255 byte Orca's fan_gear packs.
uint32_t fan_byte(const json &fans, const char *key)
{
    auto it = fans.find(key);
    if (it == fans.end() || !it->is_number())
        return 0;
    const double pct = std::clamp(it->get<double>(), 0., 100.);
    return uint32_t(std::lround(pct * 255. / 100.));
}

json ams_payload(const json &units, const FilamentIdForType &filament_id)
{
    // Orca's AMS view: up to four slots per unit, a bit per unit in ams_exist_bits and a bit per slot (unit * 4 + slot)
    // in tray_exist_bits, both as hex text (the Moonraker agent's build_ams_payload layout).
    json          ams = json::array();
    unsigned long ams_bits = 0, tray_bits = 0;
    int           unit_index = 0;
    for (const json &unit : units) {
        // 32-bit masks, 4 slots per unit: at most 8 units fit.
        if (!unit.is_object() || unit_index >= 8)
            break;
        ams_bits |= 1ul << unit_index;
        json out = {{"id", std::to_string(unit_index)}, {"info", "0002"}};
        if (auto h = unit.find("humidity"); h != unit.end() && h->is_number())
            out["humidity_raw"] = std::to_string(int(std::lround(h->get<double>())));
        if (auto t = unit.find("temp"); t != unit.end() && t->is_number())
            out["temp"] = std::to_string(t->get<double>());
        json trays = json::array();
        const json *in_trays = unit.contains("trays") && unit["trays"].is_array() ? &unit["trays"] : nullptr;
        for (int slot = 0; in_trays && slot < 4 && slot < int(in_trays->size()); ++slot) {
            const json &t = (*in_trays)[slot];
            json        tray = {{"id", std::to_string(slot)}, {"tag_uid", "0000000000000000"}};
            const std::string state    = t.value("state", std::string());
            const std::string material = t.value("material", std::string());
            if (state != "empty" && !material.empty()) {
                tray_bits |= 1ul << (unit_index * 4 + slot);
                tray["tray_type"]     = material;
                tray["tray_color"]    = device_tray_color(t.value("color", std::string()));
                tray["tray_info_idx"] = filament_id ? filament_id(material) : std::string();
                if (auto r = t.find("remain_pct"); r != t.end() && r->is_number())
                    tray["remain"] = int(std::lround(std::clamp(r->get<double>(), 0., 100.)));
            } else {
                tray["tray_type"]             = "";
                tray["tray_color"]            = "00000000";
                tray["tray_info_idx"]         = "";
                tray["tray_slot_placeholder"] = "1";
            }
            trays.push_back(std::move(tray));
        }
        out["tray"] = std::move(trays);
        ams.push_back(std::move(out));
        ++unit_index;
    }
    std::ostringstream ams_hex, tray_hex;
    ams_hex << std::hex << std::uppercase << ams_bits;
    tray_hex << std::hex << std::uppercase << tray_bits;
    return {{"ams", std::move(ams)}, {"ams_exist_bits", ams_hex.str()}, {"tray_exist_bits", tray_hex.str()}};
}

} // namespace

bool merge_neutral_status(json &cache, const json &update)
{
    if (!update.is_object() || !update.contains("v") || !update["v"].is_number_integer() || update["v"].get<int>() != 1)
        return false;
    if (!cache.is_object())
        cache = json::object();
    merge_objects(cache, update);
    return true;
}

std::string device_tray_color(const std::string &color)
{
    std::string value = color;
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
        value = value.substr(2);
    else if (!value.empty() && value[0] == '#')
        value = value.substr(1);
    if ((value.size() != 6 && value.size() != 8) ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); }))
        return "00000000";
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return value.size() == 6 ? value + "FF" : value;
}

json neutral_status_to_device_push(const json &status, const FilamentIdForType &filament_id)
{
    json p = {{"command", "push_status"}, {"msg", 0}, {"support_mqtt_alive", true}};

    // gcode_state and mc_print_stage as the Moonraker agent numbers them; Orca's HMS dialogs read print_error, which
    // stays 0 because printer error codes have no HMS meaning (mods report them in "errors" for the printer panel).
    static const std::map<std::string, std::pair<const char *, int>> states = {
        {"idle", {"IDLE", 0}},       {"preparing", {"PREPARE", 1}}, {"printing", {"RUNNING", 1}},
        {"paused", {"PAUSE", 2}},    {"finished", {"FINISH", 3}},   {"failed", {"FAILED", 4}},
        {"offline", {"IDLE", 0}}};
    const auto state = states.find(status.value("state", std::string("idle")));
    p["gcode_state"]         = state != states.end() ? state->second.first : "IDLE";
    p["mc_print_stage"]      = state != states.end() ? state->second.second : 0;
    p["mc_print_error_code"] = 0;
    p["print_error"]         = 0;
    // The send dialog refuses a printer without storage (DevStorage reads "sdcard"; a missing key means none). A
    // connected printer.agent printer always takes uploads; an offline one does not.
    p["sdcard"] = status.value("state", std::string("idle")) != "offline";

    if (auto it = status.find("progress"); it != status.end() && it->is_number())
        p["mc_percent"] = int(std::lround(std::clamp(it->get<double>(), 0., 100.)));
    // Orca shows whole minutes; a started minute counts, so 30 s left is not "0 min".
    if (auto it = status.find("remaining_s"); it != status.end() && it->is_number() && it->get<double>() >= 0.)
        p["mc_remaining_time"] = int(std::ceil(it->get<double>() / 60.));
    if (auto it = status.find("layer"); it != status.end() && it->is_number_integer())
        p["layer_num"] = it->get<int>();
    if (auto it = status.find("total_layers"); it != status.end() && it->is_number_integer())
        p["total_layer_num"] = it->get<int>();
    if (auto it = status.find("file"); it != status.end() && it->is_string()) {
        p["subtask_name"] = it->get<std::string>();
        p["gcode_file"]   = it->get<std::string>();
    }

    if (auto temps = status.find("temps"); temps != status.end() && temps->is_object()) {
        double cur = 0., target = 0.;
        if (temperature_pair(*temps, "nozzle", cur, target)) {
            p["nozzle_temper"]        = cur;
            p["nozzle_target_temper"] = target;
        }
        if (temperature_pair(*temps, "bed", cur, target)) {
            p["bed_temper"]        = cur;
            p["bed_target_temper"] = target;
        }
        if (temperature_pair(*temps, "chamber", cur, target)) {
            p["chamber_temper"] = cur;
            p["ctt"]            = target;
        }
    }
    if (auto fans = status.find("fans"); fans != status.end() && fans->is_object())
        // byte 0 part cooling, byte 1 auxiliary (DevFan::ParseV1_0); the box fan has no place there.
        p["fan_gear"] = fan_byte(*fans, "part") | (fan_byte(*fans, "aux") << 8);
    if (auto it = status.find("speed_mode"); it != status.end() && it->is_number_integer())
        p["spd_lvl"] = std::clamp(it->get<int>(), 1, 4);
    if (auto light = status.find("light"); light != status.end() && light->is_object())
        if (auto chamber = light->find("chamber"); chamber != light->end() && chamber->is_boolean())
            p["lights_report"] = json::array({{{"node", "chamber_light"}, {"mode", chamber->get<bool>() ? "on" : "off"}}});
    if (auto caps = status.find("caps"); caps != status.end() && caps->is_object()) {
        // Ranges the Device tab's temperature inputs accept, from the mod's own limits for the connected model.
        if (auto n = caps->find("nozzle_max"); n != caps->end() && n->is_number())
            p["nozzle_temp_range"] = {0, n->get<int>()};
        if (auto b = caps->find("bed_max"); b != caps->end() && b->is_number())
            p["bed_temp_range"] = {0, b->get<int>()};
    }
    if (auto units = status.find("units"); units != status.end() && units->is_array() && !units->empty())
        p["ams"] = ams_payload(*units, filament_id);

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return {{"print", std::move(p)}, {"t_utc", uint64_t(now_ms)}};
}

std::optional<json> device_command_to_neutral(const json &command)
{
    if (!command.is_object())
        return std::nullopt;
    if (auto sys = command.find("system"); sys != command.end() && sys->is_object()) {
        if (sys->value("command", std::string()) == "ledctrl" && sys->value("led_node", std::string()) == "chamber_light")
            return json{{"cmd", "set_light"}, {"light", "chamber"}, {"on", sys->value("led_mode", std::string()) != "off"}};
        return std::nullopt;
    }
    auto print = command.find("print");
    if (print == command.end() || !print->is_object())
        return std::nullopt;
    const std::string cmd = print->value("command", std::string());
    if (cmd == "pause" || cmd == "resume" || cmd == "stop")
        return json{{"cmd", cmd}};
    if (cmd == "set_bed_temp" && print->contains("temp") && (*print)["temp"].is_number())
        return json{{"cmd", "set_temp"}, {"heater", "bed"}, {"target", (*print)["temp"].get<double>()}};
    if (cmd == "set_nozzle_temp" && print->contains("target_temp") && (*print)["target_temp"].is_number())
        return json{{"cmd", "set_temp"}, {"heater", "nozzle"}, {"target", (*print)["target_temp"].get<double>()}};
    if (cmd == "print_speed" && print->contains("param") && (*print)["param"].is_string()) {
        const std::string level = (*print)["param"].get<std::string>();
        if (level.size() == 1 && level[0] >= '1' && level[0] <= '4')
            return json{{"cmd", "set_speed_mode"}, {"mode", std::min(level[0] - '0', 3)}};
        return std::nullopt;
    }
    if (cmd == "gcode_line" && print->contains("param") && (*print)["param"].is_string()) {
        // Only the single lines Orca's own controls send (DevFan, the temperature inputs, the home button).
        std::string line = (*print)["param"].get<std::string>();
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, std::regex(R"(M106 P([123]) S(\d{1,3}))"))) {
            static const char *fans[] = {"part", "aux", "box"};
            const int value = std::min(std::stoi(m[2].str()), 255);
            return json{{"cmd", "set_fan"}, {"fan", fans[std::stoi(m[1].str()) - 1]}, {"pct", int(std::lround(value * 100. / 255.))}};
        }
        if (std::regex_match(line, m, std::regex(R"((M104|M140) S(\d{1,3}))")))
            return json{{"cmd", "set_temp"}, {"heater", m[1].str() == "M104" ? "nozzle" : "bed"}, {"target", std::stoi(m[2].str())}};
        if (line == "G28")
            return json{{"cmd", "home"}, {"axes", "xyz"}};
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace Slic3r
