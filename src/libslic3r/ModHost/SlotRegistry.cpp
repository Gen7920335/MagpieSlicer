#include "SlotRegistry.hpp"

#include "libslic3r/Print.hpp"
#include "Builtin/CuraSupport.hpp"

#include <map>
#include <mutex>
#include <utility>

namespace Slic3r {
namespace ModHost {

namespace {

struct Registry
{
    std::mutex                              mutex;
    std::map<std::string, SupportGenerator> support_generators;
};

void register_builtin_modules(Registry &registry)
{
    // Orca's own normal and tree generators behind the slot. It produces exactly what an empty
    // support_generator does, which is how the slot itself is checked against vanilla output.
    registry.support_generators["orca"] = {"orca", "Orca", [](PrintObject &object) { object.generate_orca_support_material(); }};
    // CuraEngine's normal support areas, printed with Orca's toolpaths. Its settings are the
    // parameters of the bundled cura_support mod (resources/mods/cura_support.json).
    registry.support_generators["cura"] = {"cura", "Cura", [](PrintObject &object) { CuraSupport::generate(object); }};
}

Registry &registry()
{
    static Registry *instance = [] {
        auto *r = new Registry;
        register_builtin_modules(*r);
        return r;
    }();
    return *instance;
}

} // namespace

const SupportGenerator *find_support_generator(const std::string &name)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto                        it = r.support_generators.find(name);
    // Entries are never removed, only replaced in place, so the pointer stays valid.
    return it == r.support_generators.end() ? nullptr : &it->second;
}

std::vector<const SupportGenerator *> support_generators()
{
    Registry                             &r = registry();
    std::lock_guard<std::mutex>           lock(r.mutex);
    std::vector<const SupportGenerator *> out;
    for (const auto &[name, generator] : r.support_generators)
        out.push_back(&generator);
    return out;
}

void register_support_generator(SupportGenerator generator)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::string                 name = generator.name;
    r.support_generators[name]       = std::move(generator);
}

} // namespace ModHost
} // namespace Slic3r
