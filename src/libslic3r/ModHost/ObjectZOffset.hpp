#pragma once

// MAGPIE: the object.z_offset slot (mods/sdk/include/magpie_slot_object_z_offset.h). A native mod lifts an
// object off the bed or the raft so that its support goes under the object's bottom too. SlicingParameters
// applies the lift only to an object with support of its own.

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class PrintObjectConfig;

namespace ModHost {

// How far the loaded object.z_offset implementations lift an object with these settings when it has
// support, mm (0: not lifted). Two implementations lifting it, or one failing, is an error: with `error`
// given it is set to the message and the result is 0; without, SlicingError is thrown.
double object_z_offset(const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config, const std::string &object_name,
                       std::string *error = nullptr);

} // namespace ModHost
} // namespace Slic3r
