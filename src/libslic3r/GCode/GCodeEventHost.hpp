#pragma once

// MAGPIE: the core side of the gcode.events slot's writer table (mods/sdk/include/magpie_slot_gcode_events.h):
// what a G-code event mod may read of the export and write into it, implemented on the G-code generator.
// ModHost/GCodeEvents.hpp opens the mods and dispatches; this file is the generator's part.

#include "../ExtrusionEntity.hpp"

#include "../../../mods/sdk/include/magpie_slot_gcode_events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class GCode;

struct GCodeEventHost
{
    // The writer every open mod gets (its w set per mod by ModHost::GCodeEvents).
    static const mp_gcode_writer_v1 &writer_table();
    // Raises `ev` at the open mods of `gcode`, their G-code appended to `out`.
    static void raise(GCode &gcode, mp_gcode_event_v1 &ev, std::string &out);
    // Per-point speeds a mod gives the path of `query`, or false.
    static bool path_speeds(GCode &gcode, const mp_gcode_speed_query_v1 &query, std::vector<double> &speeds);
    // Rethrows what a writer function could not complete during the last call into a mod.
    static void rethrow_pending(GCode &gcode);

    static int32_t       role_to_mod(ExtrusionRole role);
    static ExtrusionRole role_from_mod(int32_t role);
};

} // namespace Slic3r
