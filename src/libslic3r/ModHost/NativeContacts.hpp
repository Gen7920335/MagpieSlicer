#pragma once

// MAGPIE: the support.contacts slot (mods/sdk/include/magpie_slot_support_contacts.h). A native mod decides
// where an object's support touches it (contacts per object layer) and keeps a support body of its own;
// the core runs Orca's normal-support generator on them (SupportMaterialOverrides): interfaces, gaps,
// raft and toolpaths as Orca's, the body cut at every intermediate support layer's print Z. The
// support_generator setting picks the implementation by name.

#include <string>

namespace Slic3r {

class PrintObject;

namespace ModHost {

struct NativeEntry;

namespace NativeContacts {

// The loaded support.contacts implementation named `name`, or nullptr.
const NativeEntry *find(const std::string &name);

// Generates the object's support layers from the implementation's contacts and body. Throws
// SlicingError naming the mod when it fails, CanceledException when the slice was cancelled.
void generate(PrintObject &object, const NativeEntry &entry);

} // namespace NativeContacts
} // namespace ModHost
} // namespace Slic3r
