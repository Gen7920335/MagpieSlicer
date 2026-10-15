#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class PrintObject;

namespace ModHost {

// support.generator: replaces Orca's support generation for an object. It fills the object's
// support layers the way PrintObject::generate_orca_support_material() does, and is chosen per
// object by the support_generator setting. An empty setting keeps Orca's own generator.
struct SupportGenerator
{
    std::string                        name;  // the support_generator value that selects it
    std::string                        label; // shown in the settings
    std::function<void(PrintObject &)> generate;
};

// The generator registered under `name`, or nullptr.
const SupportGenerator *find_support_generator(const std::string &name);
// Every registered generator, in name order.
std::vector<const SupportGenerator *> support_generators();
// Registers a generator; a later registration under the same name replaces the earlier one.
void register_support_generator(SupportGenerator generator);

} // namespace ModHost
} // namespace Slic3r
