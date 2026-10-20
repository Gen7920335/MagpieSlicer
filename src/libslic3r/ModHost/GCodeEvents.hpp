#pragma once

// MAGPIE: the gcode.events slot (mods/sdk/include/magpie_slot_gcode_events.h), design E15-E17. Native mods add
// G-code at fixed points of the export (paths, layer start and end, support end, the start G-code), ask for an
// export order (support after the object), give paths their speeds and print auxiliary towers, all through a
// writer table the core implements (GCode/GCodeEventHost.cpp). This file only opens the implementations and
// dispatches to them; it knows no feature.

#include "../../../mods/sdk/include/magpie_slot_gcode_events.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;
class Print;
class PrintObject;

namespace ModHost {

struct NativeEntry;

// The implementations open for one G-code export. The writer each gets is the core's `base` table with w
// pointing at that implementation's own call record, so the core knows who called (writer_mod_id).
class GCodeEvents
{
public:
    struct Open;
    // Opens every loaded gcode.events implementation for an export of `print`. `settings` is the export's
    // current configuration (the G-code generator's, object settings applied as it prints); `host_data` is
    // what writer functions get back from writer_host(); `calibration` names the print's calibration.
    // `after_call` runs after every call into a mod, also a failed one: the writer's owner rethrows there what one of
    // its functions could not complete, so no exception crosses into a mod.
    GCodeEvents(Print &print, const ConfigBase &settings, const mp_gcode_writer_v1 &base, void *host_data,
                const std::string &calibration, std::function<void()> after_call);
    ~GCodeEvents();
    GCodeEvents(const GCodeEvents &) = delete;
    GCodeEvents &operator=(const GCodeEvents &) = delete;

    bool empty() const { return m_open.empty(); }
    bool wants_paths() const { return m_wants & MP_GCODE_WANTS_PATHS; }
    bool wants_path_speeds() const { return m_wants & MP_GCODE_WANTS_PATH_SPEEDS; }
    bool wants_start_gcode() const { return m_wants & MP_GCODE_WANTS_START_GCODE; }
    bool changes_temperature() const { return m_wants & MP_GCODE_CHANGES_TEMPERATURE; }
    // Mods that affected the export so far (open flag or mark_used).
    const std::set<std::string> &used() const { return m_used; }

    // Raises `ev` (kind and its fields set) at every open implementation; owns_temperature collects theirs.
    void raise(mp_gcode_event_v1 &ev);
    // Whether some implementation prints the support of the current object after its other extrusions.
    bool support_last();
    // Whether some implementation prints an auxiliary tower on the layer at print_z (mm).
    bool tower_on_layer(double print_z);
    // Per-point speeds (mm/s) the first implementation that changes them gives, or nothing.
    bool path_speeds(const mp_gcode_speed_query_v1 &query, std::vector<double> &speeds);
    // Runs the start G-code filters on `text` at `phase`; their notes are appended to `notes`.
    void start_gcode(int32_t phase, std::string &text, std::vector<std::string> &notes);

    // For the writer functions: the core's data and the id of the mod whose call is running.
    static void *writer_host(void *w);
    static void  writer_mark_used(void *w);

private:
    void call(Open &open, const char *what, const std::function<mp_status(mp_ctx &)> &fn);

    Print                             &m_print;
    const ConfigBase                  &m_settings;
    std::vector<std::unique_ptr<Open>> m_open;
    uint32_t                           m_wants = 0;
    bool                               m_opening = false;
    std::function<void()>              m_after_call;
    std::set<std::string>              m_used;
};

// Whether every line of `text` is empty or a comment (starts with ';' after spaces), as mods may write it.
bool comment_lines_only(const std::string &text);

// Without an export (Print::validate): the first problem a gcode.events implementation finds with the object, as a
// translated message and the setting it is about.
std::optional<std::pair<std::string, std::string>> validate_gcode_events(const PrintObject &object);
// Whether an implementation prints an auxiliary tower with the support filament at full layer height for the object's
// settings (Print::validate's line width checks).
bool gcode_events_tower_requested(const PrintObject &object);
// The auxiliary tower the 3D scene shows for `config` (the plate's settings, mod rules resolved), from the first
// implementation that shows one.
std::optional<mp_tower_preview_v1> gcode_events_tower_preview(const DynamicPrintConfig &config);

// The calibration name gcode.events mods see for the print's calibration mode ("" for a normal print), and its value
// `name` (Calib_Params fields by their names).
std::string calibration_name(const Print &print);
bool        calibration_value(const Print &print, const char *name, double &out);

} // namespace ModHost
} // namespace Slic3r
