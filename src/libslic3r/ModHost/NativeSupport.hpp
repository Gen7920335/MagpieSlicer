#pragma once

// MAGPIE: the support.areas slot (mods/sdk/include/magpie_support.h). A native mod decides where an
// object's support goes, as three areas per object layer (body, top and bottom interface); the core
// gives it the object's layers and turns its areas into Orca support layers: raft, interfaces and
// toolpaths as for Orca's normal support. The active mods pick the implementation
// per object (ModHost::support_generator_name, SlotChoice.hpp); its name is looked up after the built-in
// generators (SlotRegistry.hpp).

#include <string>
#include <vector>

#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class PrintObject;

namespace ModHost {

struct NativeEntry;

namespace NativeSupport {

// The loaded support.areas implementation named `name`, or nullptr.
const NativeEntry *find(const std::string &name);

// Generates the object's support layers from the implementation's areas. mask: when set, per object
// layer, the only places overhangs may be supported (Mixed support).
void generate(PrintObject &object, const NativeEntry &entry, const std::vector<Polygons> *mask = nullptr);

// What the implementation would support on each object layer (its overhang areas).
std::vector<Polygons> overhang_demand(const PrintObject &object, const NativeEntry &entry);

} // namespace NativeSupport
} // namespace ModHost
} // namespace Slic3r
