// Snapmaker U1 start G-code guard (Magpie line 1). On a U1 only: a start G-code without the stock pre-print
// sequence (PRINT_START, homing, bed mesh, purge line) is replaced by it, and a start G-code that would move before
// homing gets a G28 after PRINT_START. It never does so in silence: a comment where the start G-code begins and a
// slicing warning (audit decision C). The stock U1 profiles already carry the sequence, so with them nothing
// changes. A native mod on the gcode.events slot's start G-code filter (mod design 6-10); built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_gcode_events.h>

#include "homing_policy.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define U1_MOD_ID "snapmaker_u1"

MAGPIE_MOD_DEFINE(U1_MOD_ID, "2.0.0")

namespace u1 {

std::string config_str(mp_ctx *ctx, const char *key)
{
    std::vector<char> buf(4096);
    for (;;) {
        const int rc = magpie::host()->config_str(ctx, key, -1, buf.data(), uint32_t(buf.size()));
        if (rc == MP_OK)
            return std::string(buf.data());
        if (rc != MP_ERR_BUFFER || buf.size() > (1u << 26))
            return {};
        buf.resize(buf.size() * 4);
    }
}

char *host_copy(const std::string &text)
{
    char *out = static_cast<char *>(magpie::host()->alloc(text.size() + 1));
    if (out == nullptr)
        throw std::bad_alloc();
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

struct State
{
    // The configured start G-code lacked the stock sequence and was replaced.
    bool replaced = false;
};

mp_status open(void *, mp_ctx *ctx, const mp_gcode_export_v1 *, const mp_gcode_writer_v1 *, void **state, uint32_t *flags)
{
    return magpie::guarded([&] {
        *state = nullptr;
        // On by default (Gen, 2026-10-19).
        bool guard = true;
        if (auto v = magpie::param_i64(ctx, "start_guard"))
            guard = *v != 0;
        if (!guard || !is_snapmaker_u1_model(config_str(ctx, "printer_model")))
            return;
        auto s      = std::make_unique<State>();
        s->replaced = !snapmaker_u1_has_native_start(config_str(ctx, "machine_start_gcode"));
        *flags      = MP_GCODE_WANTS_START_GCODE | (s->replaced ? MP_GCODE_USED : 0u);
        *state      = s.release();
    });
}

mp_status event(void *, void *, mp_ctx *, mp_gcode_event_v1 *) { return {MP_OK, nullptr}; }

mp_status support_last(void *, void *, mp_ctx *, int32_t *out)
{
    *out = 0;
    return {MP_OK, nullptr};
}

mp_status tower_on_layer(void *, void *, mp_ctx *, double, int32_t *out)
{
    *out = 0;
    return {MP_OK, nullptr};
}

mp_status path_speeds(void *, void *, mp_ctx *, const mp_gcode_speed_query_v1 *, double *, int32_t *changed)
{
    *changed = 0;
    return {MP_OK, nullptr};
}

mp_status start_gcode(void *, void *state, mp_ctx *ctx, int32_t phase, const char *in, mp_gcode_text_v1 *out)
{
    return magpie::guarded([&] {
        State &s = *static_cast<State *>(state);
        if (phase == MP_START_TEMPLATE) {
            // The stock pre-print sequence when the configured start G-code lacks it.
            const std::string text = snapmaker_u1_start_gcode_template(in);
            if (text != in)
                out->text = host_copy(text);
            return;
        }
        // Filled in: home after PRINT_START when the start G-code would move unhomed.
        std::string text          = in;
        const bool  homing_added  = !snapmaker_u1_has_native_start(text) && ensure_snapmaker_u1_safe_homing(text);
        if (homing_added)
            out->text = host_copy(text);
        if (s.replaced) {
            out->note = host_copy("; Magpie: the start G-code lacked the Snapmaker U1 pre-print sequence; the stock U1 start G-code replaced it\n");
            magpie::host()->warn(ctx, MP_LOG_WARNING,
                                 "The start G-code lacks the Snapmaker U1 pre-print sequence, so the stock U1 start G-code was used instead. "
                                 "Turn off the Snapmaker U1 start guard on the printer's Mods page to keep your own.");
        } else if (homing_added) {
            out->note = host_copy("; Magpie: the start G-code moved before homing; a G28 was added after PRINT_START\n");
            magpie::host()->warn(ctx, MP_LOG_WARNING, "The start G-code moves the Snapmaker U1 before homing, so a G28 was added after PRINT_START.");
        }
    });
}

void close(void *, void *state) { delete static_cast<State *>(state); }

mp_status validate(void *, mp_ctx *, const mp_gcode_validate_v1 *, mp_gcode_problem_v1 *) { return {MP_OK, nullptr}; }

mp_status tower_preview(void *, mp_ctx *, int32_t, mp_tower_preview_v1 *out)
{
    out->show = 0;
    return {MP_OK, nullptr};
}

mp_status tower_requested(void *, mp_ctx *, int32_t *out)
{
    *out = 0;
    return {MP_OK, nullptr};
}

const mp_gcode_events_v1 table = {sizeof(mp_gcode_events_v1), open, event, support_last, tower_on_layer, path_speeds, start_gcode, close,
                                  validate, tower_preview, tower_requested};

} // namespace u1

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_GCODE_EVENTS_SLOT, MP_GCODE_EVENTS_VERSION, "snapmaker_u1", &u1::table, sizeof(u1::table),
                                   nullptr);
}

void magpie::mod_shutdown() {}
