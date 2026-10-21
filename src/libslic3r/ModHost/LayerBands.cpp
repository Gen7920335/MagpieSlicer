#include "LayerBands.hpp"

#include "NativeMods.hpp"

#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"

#include <boost/log/trivial.hpp>

#include "../../../mods/sdk/include/magpie_slot_layer_bands.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace Slic3r {
namespace ModHost {

namespace {

// What one implementation's plan call reports, or why it has none.
bool plan_of(const NativeEntry &entry, const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config,
             mp_layer_bands_object_v1 &out, std::string &error)
{
    const auto &table = *static_cast<const mp_layer_bands_v1 *>(entry.table);
    mp_ctx      ctx;
    ctx.mod           = entry.mod;
    ctx.print_config  = &print_config;
    ctx.object_config = &object_config;
    ctx.cancelled     = []() { return false; };
    out               = {};
    out.size          = sizeof(out);
    std::string   status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = table.plan(entry.user, &ctx, &out);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc != MP_OK) {
        error = entry.mod->id() + ": " + (!status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                          !guard_message.empty()  ? guard_message : std::string("the layer bands failed"));
        return false;
    }
    // What the mod warned about (host->warn): an error stops the plan, the rest goes to the log (review 2026-10-22:
    // they were dropped).
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR) {
            error = entry.mod->id() + ": " + mod_text(entry.mod->id(), text);
            return false;
        }
        BOOST_LOG_TRIVIAL(warning) << entry.mod->id() << ": " << mod_text(entry.mod->id(), text);
    }
    return true;
}

// The implementation's insertion choice as a LayerBandScheduler. `mods` keeps the library loaded while a plan
// holds the scheduler.
LayerBandScheduler scheduler_of(std::shared_ptr<const NativeModSet> mods, const NativeEntry &entry)
{
    return [mods = std::move(mods), &entry](double base_seconds, double first_start_seconds, double first_end_seconds,
                                            double block_seconds, const std::vector<HalfLayerInsertionBoundary> &boundaries) {
        const auto &table = *static_cast<const mp_layer_bands_v1 *>(entry.table);
        mp_band_timing_v1 timing{sizeof(mp_band_timing_v1), base_seconds, first_start_seconds, first_end_seconds, block_seconds};
        std::vector<mp_band_boundary_v1> list;
        list.reserve(boundaries.size());
        for (const HalfLayerInsertionBoundary &b : boundaries)
            list.push_back({b.elapsed_seconds, b.approach_seconds, b.departure_seconds, b.replaced_transition_seconds, b.allowed ? 1 : 0});
        mp_band_choice_v1 choice{};
        choice.size = sizeof(choice);
        std::string   status_message, guard_message;
        const int32_t rc = entry.mod->guarded_call([&]() {
            const mp_status st = table.schedule(entry.user, &timing, list.data(), uint32_t(list.size()), &choice);
            if (st.code != MP_OK && st.message != nullptr)
                status_message = st.message;
            return st.code;
        }, &guard_message);
        if (rc != MP_OK)
            throw std::logic_error(entry.mod->id() + ": " + (!status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                                             !guard_message.empty()  ? guard_message : std::string("the band schedule failed")));
        HalfLayerInsertion insertion;
        if (choice.boundary != UINT32_MAX) {
            insertion.boundary               = size_t(choice.boundary);
            insertion.second_start_seconds   = choice.block_start_s;
            insertion.total_seconds          = choice.total_s;
            insertion.midpoint_error_seconds = choice.error_s;
        }
        return insertion;
    };
}

} // namespace

LayerBandPlan layer_band_plan(const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config, const std::string &object_name)
{
    LayerBandPlan                       plan;
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return plan;
    const NativeEntry *banding = nullptr;
    for (const NativeEntry *entry : mods->slots(MP_LAYER_BANDS_SLOT)) {
        if (entry->version < MP_LAYER_BANDS_VERSION || entry->table_size < sizeof(mp_layer_bands_v1))
            continue;
        mp_layer_bands_object_v1 answer;
        std::string              error;
        if (!plan_of(*entry, print_config, object_config, answer, error)) {
            LayerBandPlan failed;
            failed.error = error;
            return failed;
        }
        if (answer.wall_depths <= 0 && answer.support == 0)
            continue;
        if (banding != nullptr) {
            LayerBandPlan failed;
            failed.error = Slic3r::format(_u8L("The mods %1% and %2% both print object \"%3%\" in layer bands. Turn one of them off."),
                                          banding->mod->id(), entry->mod->id(), object_name);
            return failed;
        }
        banding = entry;
        plan.mod_id      = entry->mod->id();
        plan.wall_depths = std::max(0, int(answer.wall_depths));
        plan.support     = answer.support != 0;
        plan.band_fractions.clear();
        for (uint32_t i = 0; i < answer.band_count && i < MP_LAYER_BANDS_MAX; ++i)
            plan.band_fractions.push_back(answer.band_fraction[i]);
        if (answer.band_count > MP_LAYER_BANDS_MAX)
            plan.band_fractions.push_back(0.); // more bands than the slot carries: refused below
        plan.min_travel_lift_layers  = answer.min_travel_lift_layers;
        plan.support_band_min_layers = answer.support_band_min_layers;
        plan.support_band_max_layers = answer.support_band_max_layers;
        plan.scheduler               = scheduler_of(mods, *entry);
    }
    return plan;
}

} // namespace ModHost
} // namespace Slic3r
