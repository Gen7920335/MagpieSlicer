// The layer time blending mod: fills the core slot "gcode.time_plan" (magpie_slot_time_plan.h) with the
// planner of planner.cpp under the name "blend", picked while blending is on (mod.json). Its
// settings are the mod's own parameters (mod.json), read through the host for each print.
#include <magpie_mod.hpp>
#include <magpie_slot_time_plan.h>

#include "planner.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#define LTB_MOD_ID "layer_time_blend"

namespace {

double required_f64(mp_ctx *ctx, const char *key)
{
    if (std::optional<double> v = magpie::param_f64(ctx, key))
        return *v;
    throw std::runtime_error(std::string("parameter ") + key + " cannot be read");
}

int required_i64(mp_ctx *ctx, const char *key)
{
    if (std::optional<int64_t> v = magpie::param_i64(ctx, key))
        return int(*v);
    throw std::runtime_error(std::string("parameter ") + key + " cannot be read");
}

std::string required_str(mp_ctx *ctx, const char *key)
{
    if (std::optional<std::string> v = magpie::param_str(ctx, key))
        return *v;
    throw std::runtime_error(std::string("parameter ") + key + " cannot be read");
}

LayerTimeBlend::Settings settings_from(mp_ctx *ctx)
{
    LayerTimeBlend::Settings s;
    s.log            = required_str(ctx, "blend_type") == "log";
    s.max_diff_s     = required_f64(ctx, "max_diff");
    s.max_ratio_pct  = required_f64(ctx, "max_ratio");
    s.ease_layers    = required_i64(ctx, "ease_layers");
    s.start_layer    = required_i64(ctx, "start_layer");
    s.wait_cap_s     = required_f64(ctx, "wait_cap");
    s.exception_cost = required_f64(ctx, "exception_cost");
    s.warn_total_pct = required_f64(ctx, "warn_total_pct");
    s.warn_single_s  = required_f64(ctx, "warn_single");
    const std::string wait_at = required_str(ctx, "wait_at");
    // The core offers a park trip (park_trip_s > 0) only at sites it parks from: park and purge_tower, and tower
    // where it parks instead (a by-object print, or a tower that does not fit).
    s.park           = wait_at != "in_place";
    s.park_min_s     = required_f64(ctx, "in_place_max");
    return s;
}

int32_t warning_kind(LayerTimeBlend::WaitWarning::Kind kind)
{
    switch (kind) {
    case LayerTimeBlend::WaitWarning::Kind::TotalShare: return MP_WAIT_WARNING_TOTAL_SHARE;
    case LayerTimeBlend::WaitWarning::Kind::SingleWait: return MP_WAIT_WARNING_SINGLE_WAIT;
    case LayerTimeBlend::WaitWarning::Kind::Uncertain: break;
    }
    return MP_WAIT_WARNING_UNCERTAIN;
}

mp_status plan(void * /* user */, mp_ctx *ctx, const mp_wait_site *in, uint64_t n, double print_time_s, mp_wait_plan *out)
{
    return magpie::guarded([&] {
        if ((in == nullptr && n > 0) || out == nullptr)
            throw std::invalid_argument("null sites or plan");
        std::vector<LayerTimeBlend::WaitSite> sites(static_cast<size_t>(n));
        for (size_t k = 0; k < sites.size(); ++k) {
            sites[k].layer_id         = size_t(in[k].layer_id);
            sites[k].interval_s       = in[k].interval_s;
            sites[k].park_trip_s      = in[k].park_trip_s;
            sites[k].constraint_break = in[k].constraint_break != 0;
            sites[k].uncertain        = in[k].uncertain != 0;
            sites[k].tower_pre_s      = in[k].tower_pre_s;
            sites[k].tower_block_s    = in[k].tower_block_s;
        }
        const LayerTimeBlend::WaitPlan p = LayerTimeBlend::plan(sites, settings_from(ctx), print_time_s);
        for (size_t k = 0; k < sites.size(); ++k) {
            out->dwell_s[k]  = p.dwell_s[k];
            out->use_park[k] = p.use_park[k] ? 1 : 0;
            out->added_s[k]  = p.added_s[k];
            out->target_s[k] = p.target_s[k];
        }
        out->tower_end  = p.tower_end;
        out->warnings_n = 0;
        for (const LayerTimeBlend::WaitWarning &w : p.warnings) {
            if (out->warnings_n >= out->warnings_cap)
                throw std::length_error("more warnings than the plan has room for");
            mp_wait_warning &o = out->warnings[out->warnings_n++];
            o.kind        = warning_kind(w.kind);
            o.value       = w.value;
            o.source_site = w.source_site;
            o.first_site  = w.first_site;
            o.last_site   = w.last_site;
        }
    });
}

const mp_time_plan_v1 time_plan_table = {sizeof(mp_time_plan_v1), plan};

} // namespace

int32_t magpie::mod_init(const MagpieHostApi &host, MagpieRegistrar &registrar)
{
    LayerTimeBlend::set_diagnostic_sink([](const std::string &text) { magpie::log(MP_LOG_ERROR, LTB_MOD_ID, text); });
    (void) host;
    return registrar.register_slot(&registrar, MP_SLOT_TIME_PLAN, MP_SLOT_TIME_PLAN_VERSION, "blend", &time_plan_table,
                                   sizeof(time_plan_table), nullptr);
}

void magpie::mod_shutdown() { LayerTimeBlend::set_diagnostic_sink({}); }

MAGPIE_MOD_DEFINE(LTB_MOD_ID, "1.5.1")
