#ifndef slic3r_ModPrinterStatus_hpp_
#define slic3r_ModPrinterStatus_hpp_

// MAGPIE: the two translations between a printer.agent mod and Orca's device code (ModPrinterAgent.hpp), kept free of
// GUI state so they are tested on their own:
//   - the Magpie neutral status JSON v1 a mod posts -> the "print" push message MachineObject parses;
//   - the commands Orca's Device tab publishes (Bambu-format "print"/"system" JSON) -> neutral commands.
// The neutral format is described in mods/sdk/include/magpie_slot_printer_agent.h and docs/HLSD/mods.md. Fields are
// only ever added to it; fields this code does not know are ignored.

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

namespace Slic3r {

// Merges a partial neutral status into the cached one: objects merge key by key, everything else (numbers, strings,
// arrays such as "units") replaces. A status whose "v" is not 1 changes nothing and returns false.
bool merge_neutral_status(nlohmann::json &cache, const nlohmann::json &update);

// The printer's material id for a material name ("PLA"), or "" when unknown (ModPrinterAgent resolves Orca's
// generic filament presets; tests pass their own).
using FilamentIdForType = std::function<std::string(const std::string &material)>;

// The push message for a full neutral status: {"print": {"command": "push_status", "gcode_state", "mc_percent", ...,
// "ams": {...}}}. Units of more than four trays keep their first four (Orca's AMS view has four slots per unit).
nlohmann::json neutral_status_to_device_push(const nlohmann::json &status, const FilamentIdForType &filament_id);

// "RRGGBBAA" (upper case) from "#RRGGBB", "RRGGBB", "0xRRGGBB" or "RRGGBBAA"; "00000000" for anything else.
std::string device_tray_color(const std::string &color);

// The neutral command for a command Orca's Device tab published, or nothing when it has no neutral form (the agent
// then answers ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED). Raw G-code is never passed on: of "gcode_line" only the fan,
// temperature and homing lines Orca's own controls send are recognised.
std::optional<nlohmann::json> device_command_to_neutral(const nlohmann::json &command);

} // namespace Slic3r

#endif
