#include "SlotRegistry.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Print.hpp"
#include "Builtin/MixedSupport.hpp"
#include "Builtin/MultiNozzleWalls.hpp"
#include "Builtin/ResinSupport.hpp"
#include "NativeMods.hpp"

#include "../../../mods/sdk/include/magpie_slot_time_plan.h"

#include <boost/log/trivial.hpp>

#include <cmath>
#include <cstddef>
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
};

void register_builtin_modules(Registry &registry)
{
    // Orca's own normal and tree generators behind the slot. It produces exactly what an empty
    // support_generator does, which is how the slot itself is checked against vanilla output.
    registry.support_generators["orca"] = {"orca", "Orca", [](PrintObject &object) { object.generate_orca_support_material(); }};
    // "cura" (CuraEngine's normal support areas) is the native mod mods/cura_support, a support.areas
    // implementation (NativeSupport.hpp).
    // PrusaSlicer's SLA support points and trees, printed through Orca's normal-support pipeline. Its
    // settings are the parameters of the bundled resin_support mod (resources/mods/resin_support.json).
    registry.support_generators["resin"] = {"resin", "Resin", [](PrintObject &object) { ResinSupport::generate(object); }};
    // Normal and tree support in one object (resources/mods/mixed_support.json).
    registry.support_generators["mixed"] = {"mixed", "Mixed", [](PrintObject &object) { MixedSupport::generate(object); }};
    // Multi-nozzle walls (resources/mods/multi_nozzle_walls.json).
    registry.wall_planners["multi_nozzle"] = MultiNozzleWalls::make_planner();
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

namespace {

WaitWarning::Kind warning_kind(int32_t kind, const std::string &mod_id)
{
    switch (kind) {
    case MP_WAIT_WARNING_TOTAL_SHARE: return WaitWarning::Kind::TotalShare;
    case MP_WAIT_WARNING_SINGLE_WAIT: return WaitWarning::Kind::SingleWait;
    case MP_WAIT_WARNING_UNCERTAIN: return WaitWarning::Kind::Uncertain;
    default: break;
    }
    throw Slic3r::SlicingError(mod_id + ": the time planner returned an unknown warning kind " + std::to_string(kind));
}

// Calls the native slot and checks what comes back; nothing of a failed call reaches the G-code.
WaitPlan call_time_planner(const NativeEntry &entry, const mp_time_plan_v1 &table, const std::vector<WaitSite> &sites,
                           const DynamicPrintConfig &config, const std::function<bool()> &cancelled, double print_time_s)
{
    const std::string &mod_id = entry.mod->id();
    const size_t       n      = sites.size();
    std::vector<mp_wait_site> in(n);
    for (size_t k = 0; k < n; ++k) {
        in[k].layer_id         = uint64_t(sites[k].layer_id);
        in[k].interval_s       = sites[k].interval_s;
        in[k].park_trip_s      = sites[k].park_trip_s;
        in[k].tower_pre_s      = sites[k].tower_pre_s;
        in[k].tower_block_s    = sites[k].tower_block_s;
        in[k].constraint_break = sites[k].constraint_break ? 1 : 0;
        in[k].uncertain        = sites[k].uncertain ? 1 : 0;
    }
    // "No wait" everywhere, which the planner overwrites where it decides otherwise.
    WaitPlan plan;
    plan.dwell_s.assign(n, 0.);
    plan.added_s.assign(n, 0.);
    plan.target_s.resize(n);
    for (size_t k = 0; k < n; ++k)
        plan.target_s[k] = sites[k].interval_s;
    std::vector<uint8_t>         use_park(n, 0);
    // Room for warnings: the slot's three kinds, each more than once if a planner wants.
    std::vector<mp_wait_warning> warnings(16);
    mp_wait_plan out{plan.dwell_s.data(), use_park.data(), plan.added_s.data(), plan.target_s.data(),
                     warnings.data(), uint32_t(warnings.size()), 0, uint64_t(n)};

    mp_ctx ctx;
    ctx.mod          = entry.mod;
    ctx.print_config = &config;
    ctx.cancelled    = cancelled;
    std::string status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = table.plan(entry.user, &ctx, in.data(), uint64_t(n), print_time_s, &out);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc == MP_ERR_CANCELLED)
        throw Slic3r::CanceledException();
    if (rc != MP_OK)
        throw Slic3r::SlicingError(mod_id + ": " + (!status_message.empty() ? status_message :
                                                    !guard_message.empty()  ? guard_message : std::string("the time planner failed")));
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR)
            throw Slic3r::SlicingError(mod_id + ": " + text);
        plan.mod_warnings.push_back(text);
    }
    // The plan becomes G4 times and moves: every number must be a finite time, waits not negative, and every
    // site index one of these sites.
    for (size_t k = 0; k < n; ++k)
        if (!std::isfinite(plan.dwell_s[k]) || !std::isfinite(plan.added_s[k]) || !std::isfinite(plan.target_s[k]) ||
            plan.dwell_s[k] < 0. || plan.added_s[k] < 0.)
            throw Slic3r::SlicingError(mod_id + ": the time planner returned an invalid wait at site " + std::to_string(k));
    if (out.warnings_n > out.warnings_cap || out.tower_end > uint64_t(n))
        throw Slic3r::SlicingError(mod_id + ": the time planner returned a plan for other sites");
    plan.use_park.resize(n);
    for (size_t k = 0; k < n; ++k)
        plan.use_park[k] = use_park[k] != 0;
    plan.tower_end = size_t(out.tower_end);
    for (uint32_t i = 0; i < out.warnings_n; ++i) {
        const mp_wait_warning &w = warnings[i];
        if (w.source_site >= n || w.first_site >= n || w.last_site >= n)
            throw Slic3r::SlicingError(mod_id + ": the time planner named a site it was not given");
        WaitWarning warning{warning_kind(w.kind, mod_id)};
        warning.value       = w.value;
        warning.source_site = size_t(w.source_site);
        warning.first_site  = size_t(w.first_site);
        warning.last_site   = size_t(w.last_site);
        plan.warnings.push_back(warning);
    }
    return plan;
}

} // namespace

std::shared_ptr<const TimePlanner> find_time_planner(const std::string &name)
{
    if (name.empty())
        return nullptr;
    std::shared_ptr<const NativeModSet> native = active_native_mods();
    const NativeEntry *entry = native ? native->find_slot(MP_SLOT_TIME_PLAN, name, MP_SLOT_TIME_PLAN_VERSION) : nullptr;
    if (entry == nullptr)
        return nullptr;
    if (entry->table_size < offsetof(mp_time_plan_v1, plan) + sizeof(mp_time_plan_v1::plan) ||
        static_cast<const mp_time_plan_v1 *>(entry->table)->plan == nullptr) {
        // PERMANENT DIAGNOSTIC: the mod registered a table too short for interface version 1.
        BOOST_LOG_TRIVIAL(error) << "Mod " << entry->mod->id() << ": its " MP_SLOT_TIME_PLAN " table \"" << name << "\" has "
                                 << entry->table_size << " bytes, too few for version 1";
        return nullptr;
    }
    auto planner         = std::make_shared<TimePlanner>();
    planner->name        = name;
    planner->mod_id      = entry->mod->id();
    planner->mod_version = entry->mod->manifest().version;
    // The planner holds the mod set, so the library and its table stay loaded while it is in use.
    planner->plan = [native, entry](const std::vector<WaitSite> &sites, const DynamicPrintConfig &config,
                                    const std::function<bool()> &cancelled, double print_time_s) {
        return call_time_planner(*entry, *static_cast<const mp_time_plan_v1 *>(entry->table), sites, config, cancelled, print_time_s);
    };
    return planner;
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
