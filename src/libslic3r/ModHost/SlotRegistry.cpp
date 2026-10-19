#include "SlotRegistry.hpp"

#include "libslic3r/Print.hpp"
#include "Builtin/CuraSupport.hpp"
#include "Builtin/LayerTimeBlend.hpp"
#include "Builtin/MixedSupport.hpp"
#include "Builtin/MultiNozzleWalls.hpp"
#include "Builtin/ResinSupport.hpp"

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
    std::map<std::string, WallPlanner>      wall_planners;
    std::map<std::string, TimePlanner>      time_planners;
};

void register_builtin_modules(Registry &registry)
{
    // Orca's own normal and tree generators behind the slot. It produces exactly what an empty
    // support_generator does, which is how the slot itself is checked against vanilla output.
    registry.support_generators["orca"] = {"orca", "Orca", [](PrintObject &object) { object.generate_orca_support_material(); }};
    // CuraEngine's normal support areas, printed with Orca's toolpaths. Its settings are the
    // parameters of the bundled cura_support mod (resources/mods/cura_support.json).
    registry.support_generators["cura"] = {"cura", "Cura", [](PrintObject &object) { CuraSupport::generate(object); }};
    // PrusaSlicer's SLA support points and trees, printed through Orca's normal-support pipeline. Its
    // settings are the parameters of the bundled resin_support mod (resources/mods/resin_support.json).
    registry.support_generators["resin"] = {"resin", "Resin", [](PrintObject &object) { ResinSupport::generate(object); }};
    // Normal and tree support in one object (resources/mods/mixed_support.json).
    registry.support_generators["mixed"] = {"mixed", "Mixed", [](PrintObject &object) { MixedSupport::generate(object); }};
    // Multi-nozzle walls (resources/mods/multi_nozzle_walls.json).
    registry.wall_planners["multi_nozzle"] = MultiNozzleWalls::make_planner();
    // Layer time blending (resources/mods/layer_time_blend.json).
    registry.time_planners["blend"] = LayerTimeBlend::make_planner();
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

const WallPlanner *find_wall_planner(const std::string &name)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto                        it = r.wall_planners.find(name);
    return it == r.wall_planners.end() ? nullptr : &it->second;
}

void register_wall_planner(WallPlanner planner)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::string                 name = planner.name;
    r.wall_planners[name]            = std::move(planner);
}

const TimePlanner *find_time_planner(const std::string &name)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto                        it = r.time_planners.find(name);
    return it == r.time_planners.end() ? nullptr : &it->second;
}

void register_time_planner(TimePlanner planner)
{
    Registry                   &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::string                 name = planner.name;
    r.time_planners[name]            = std::move(planner);
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
