#pragma once

// MAGPIE: the support.compose slot (mods/sdk/include/magpie_slot_support_compose.h). A native mod builds an
// object's support out of the core's generators: it asks for support demand, runs Orca's normal support, a
// support.areas mod or Orca's tree support on masks of its own, and merges two of the results into the
// object's support layers (a layer of both is a multi-channel layer, Layer.hpp). The core owns every support
// layer; the mod only names sets of them. The active mods pick the implementation
// per object (ModHost::support_generator_name, SlotChoice.hpp).

#include <string>

namespace Slic3r {

class PrintObject;

namespace ModHost {

struct NativeEntry;

namespace NativeCompose {

// The loaded support.compose implementation named `name`, or nullptr.
const NativeEntry *find(const std::string &name);

// Generates the object's support layers with the implementation. On any failure the object keeps no
// support layers and the error is thrown (SlicingError naming the mod, or CanceledException).
void generate(PrintObject &object, const NativeEntry &entry);

} // namespace NativeCompose
} // namespace ModHost
} // namespace Slic3r
