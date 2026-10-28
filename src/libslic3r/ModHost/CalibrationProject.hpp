#pragma once

// MAGPIE: the calibration.project slot (mods/sdk/include/magpie_slot_calibration_project.h). A mod declares a
// calibration of the Calibration menu with its form ("gui"."calibrations"[]."form"); the application draws the form,
// this checks its values with the mod and builds the calibration's project: objects, preset and object settings and
// the values the mod's G-code side reads back (Calib_Params::mod_values). Nothing here touches the GUI, so tests
// build a calibration project as the application does.

#include "libslic3r/Model.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace ModHost {

// A form's values by field id, as the form shows them.
using CalibrationFields = std::vector<std::pair<std::string, std::string>>;

// What the mod says about the form's values. `failure` is set when the mod could not be asked (not loaded, no
// calibration.project implementation, a crash); the form then cannot start.
struct CalibrationCheck
{
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::string              summary;
    std::string              failure;
    bool                     ok() const { return failure.empty() && errors.empty(); }
};

// Which preset a calibration setting goes to (MP_CALIB_PRINT, _FILAMENT, _PRINTER).
enum class CalibrationPreset { Print = 0, Filament = 1, Printer = 2 };

struct CalibrationOption
{
    CalibrationPreset preset = CalibrationPreset::Print;
    std::string       key;   // an Orca option or "<mod id>.<name>"
    std::string       value; // serialized
};

struct CalibrationProject
{
    std::string calibration; // "<mod id>.<calibration id>", mp_gcode_export_v1.calibration
    // The calibration's objects, each with one instance placed on the plate (plate coordinates).
    Model                          model;
    std::vector<CalibrationOption> options; // in the order the mod set them
    std::map<std::string, double>  values;
};

// Asks the mod `mod_id` about its calibration `calibration` with the form's values. `config` is the full
// configuration of the presets being edited.
CalibrationCheck check_calibration(const std::string &mod_id, const std::string &calibration, const DynamicPrintConfig &config,
                                   const CalibrationFields &fields);
// The calibration's project. Throws ModError with the mod's or the core's message when the mod fails or describes
// something the core refuses (an unknown key, a value a setting does not take).
CalibrationProject build_calibration(const std::string &mod_id, const std::string &calibration, const DynamicPrintConfig &config,
                                     const CalibrationFields &fields);

// Writes one setting into a preset's config: an Orca option (a list option takes its serialized list, or one value
// for every element) or a parameter "<mod id>.<name>" of an active mod into mod_settings, printer_mod_settings or
// every filament's filament_mod_settings by its scope. Throws ModError for an unknown key or a value it does not take.
void set_calibration_option(DynamicPrintConfig &config, const std::string &key, const std::string &value);
// The same for an object's settings: Orca options of the object's config, and parameters of object or region scope
// into object_mod_settings.
void set_calibration_object_option(ModelObject &object, const std::string &key, const std::string &value);
// Every option of `project` written into the configs of its preset.
void apply_calibration_options(const CalibrationProject &project, DynamicPrintConfig &print, DynamicPrintConfig &filament,
                               DynamicPrintConfig &printer);

} // namespace ModHost
} // namespace Slic3r
