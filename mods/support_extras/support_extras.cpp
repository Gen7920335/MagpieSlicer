// Support extras: an interface pattern of its own (triangles, grid, rectilinear or concentric), interface
// sublayers (interface layers counted from the contact printed with their own pattern and angle) and separate
// wall counts for normal and tree support, for every support generator. Ported from Magpie line 1, then from the
// core's settings onto the support.style slot: this mod answers how an object's support is styled, the core's
// support toolpaths print it, the sublayers at their own temperature when one is set. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_support_style.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#define EXTRAS_MOD_ID "support_extras"

MAGPIE_MOD_DEFINE(EXTRAS_MOD_ID, "2.1.1")

namespace extras {

// What open() reads once per object.
struct State
{
    int32_t interface_pattern = MP_SUPPORT_PATTERN_KEEP;
    int     sublayer_start    = 0; // 0: no sublayers
    int     sublayer_end      = 0;
    int32_t sublayer_pattern  = MP_SUPPORT_PATTERN_KEEP;
    double  sublayer_angle    = 0.; // degrees
    int     normal_wall_loops = -1;
    int     tree_wall_loops   = -1;
    int     sublayer_temperature = 0; // degC, 0 = the filament's
};

// "triangles", "grid", "rectilinear", "concentric"; anything else keeps the core's pattern.
int32_t pattern_of(const std::string &name)
{
    if (name == "triangles")   return MP_SUPPORT_PATTERN_TRIANGLES;
    if (name == "grid")        return MP_SUPPORT_PATTERN_GRID;
    if (name == "rectilinear") return MP_SUPPORT_PATTERN_RECTILINEAR;
    if (name == "concentric")  return MP_SUPPORT_PATTERN_CONCENTRIC;
    return MP_SUPPORT_PATTERN_KEEP;
}

// A wall count: below -1 is Orca's (-1), above 10 the slot's limit (10).
int wall_loops(int64_t v) { return int(std::clamp<int64_t>(v, -1, 10)); }

// Whether interface layer `number` (1 = the contact layer) of `total` is a sublayer: numbers start..end,
// clamped to 2..total (Magpie line 1); the contact layer never is one.
bool sublayer_selected(const State &s, int number, int total)
{
    if (s.sublayer_start <= 0 || total <= 1 || number < 2)
        return false;
    const int first = std::clamp(s.sublayer_start, 2, total);
    const int last  = std::clamp(s.sublayer_end, first, total);
    return number >= first && number <= last;
}

mp_status open(void *, mp_ctx *ctx, mp_support_style_object_v1 *out, void **state)
{
    return magpie::guarded([&] {
        *state = nullptr;
        auto s = std::make_unique<State>();
        if (auto v = magpie::param_str(ctx, "interface_pattern"))
            s->interface_pattern = pattern_of(*v);
        if (auto v = magpie::param_i64(ctx, "sublayer_start"))
            s->sublayer_start = int(std::max<int64_t>(0, *v));
        if (auto v = magpie::param_i64(ctx, "sublayer_end"))
            s->sublayer_end = int(std::clamp<int64_t>(*v, INT32_MIN, INT32_MAX));
        if (auto v = magpie::param_str(ctx, "sublayer_pattern"))
            s->sublayer_pattern = pattern_of(*v);
        if (auto v = magpie::param_f64(ctx, "sublayer_angle"))
            s->sublayer_angle = *v;
        if (auto v = magpie::param_i64(ctx, "normal_wall_loops"))
            s->normal_wall_loops = wall_loops(*v);
        if (auto v = magpie::param_i64(ctx, "tree_wall_loops"))
            s->tree_wall_loops = wall_loops(*v);
        if (auto v = magpie::param_i64(ctx, "sublayer_temperature"))
            s->sublayer_temperature = int(std::clamp<int64_t>(*v, 0, INT32_MAX));
        // Nothing set: Orca's support, and the mod is not used for this object.
        if (s->interface_pattern == MP_SUPPORT_PATTERN_KEEP && s->sublayer_start == 0 && s->normal_wall_loops == -1 &&
            s->tree_wall_loops == -1)
            return;
        out->interface_pattern = s->interface_pattern;
        out->normal_wall_loops = s->normal_wall_loops;
        out->tree_wall_loops   = s->tree_wall_loops;
        // The sublayers' own temperature, while they are on (a field of API 1.1: only when the core's struct has it).
        if (out->size >= offsetof(mp_support_style_object_v1, sublayer_temperature) + sizeof(out->sublayer_temperature))
            out->sublayer_temperature = s->sublayer_start > 0 ? s->sublayer_temperature : 0;
        *state                 = s.release();
    });
}

mp_status interface_layer(void *, void *state, int32_t number, int32_t total, mp_support_interface_style_v1 *out)
{
    return magpie::guarded([&] {
        const State &s = *static_cast<const State *>(state);
        out->restyle   = sublayer_selected(s, number, total) ? 1 : 0;
        // The interface pattern unless the sublayers have their own.
        out->pattern   = s.sublayer_pattern;
        out->angle_deg = s.sublayer_angle;
    });
}

void close(void *, void *state) { delete static_cast<State *>(state); }

const mp_support_style_v1 table = {sizeof(mp_support_style_v1), open, interface_layer, close};

} // namespace extras

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_SUPPORT_STYLE_SLOT, MP_SUPPORT_STYLE_VERSION, "extras", &extras::table, sizeof(extras::table),
                                   nullptr);
}

void magpie::mod_shutdown() {}
