#include "SupportStyle.hpp"

#include "NativeMods.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/format.hpp"

#include "../../../mods/sdk/include/magpie_slot_support_style.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
namespace ModHost {

bool SupportStyle::any_interface_layer() const
{
    for (const auto &[total, layers] : interface_layers)
        if (std::any_of(layers.begin(), layers.end(), [](const auto &l) { return l.has_value(); }))
            return true;
    return false;
}

const InterfaceLayerStyle *SupportStyle::interface_layer(int number, int total) const
{
    auto it = interface_layers.find(total);
    if (it == interface_layers.end() || number < 1 || size_t(number) > it->second.size() || !it->second[number - 1])
        return nullptr;
    return &*it->second[number - 1];
}

namespace {

const mp_support_style_v1 &table_of(const NativeEntry &entry) { return *static_cast<const mp_support_style_v1 *>(entry.table); }

// Runs one call into the style's mod; a failure or a crash stops the slice naming the mod.
void call(const NativeEntry &entry, const std::function<mp_status()> &fn)
{
    std::string status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = fn();
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc == MP_ERR_CANCELLED)
        throw CanceledException();
    if (rc != MP_OK)
        throw SlicingError(entry.mod->id() + ": " + (!status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                                     !guard_message.empty()  ? guard_message : std::string("the support style failed")));
}

std::optional<InfillPattern> pattern_of(int32_t code, const NativeEntry &entry)
{
    switch (code) {
    case MP_SUPPORT_PATTERN_KEEP: return std::nullopt;
    case MP_SUPPORT_PATTERN_RECTILINEAR: return ipRectilinear;
    case MP_SUPPORT_PATTERN_GRID: return ipGrid;
    case MP_SUPPORT_PATTERN_TRIANGLES: return ipTriangles;
    case MP_SUPPORT_PATTERN_CONCENTRIC: return ipConcentric;
    }
    throw SlicingError(entry.mod->id() + ": the support style returned an unknown interface pattern " + std::to_string(code));
}

int wall_loops_of(int32_t loops, const NativeEntry &entry)
{
    // -1 keeps Orca's walls; the slot allows 0-10 walls.
    if (loops < -1 || loops > 10)
        throw SlicingError(entry.mod->id() + ": the support style returned " + std::to_string(loops) + " support walls (allowed: -1 to 10)");
    return loops;
}

int temperature_of(int32_t degc, const NativeEntry &entry)
{
    // A nozzle temperature, degC: 0 (the filament's) or a positive number; Print::validate checks its range.
    if (degc < 0)
        throw SlicingError(entry.mod->id() + ": the support style returned a negative temperature " + std::to_string(degc));
    return degc;
}

} // namespace

SupportStyle support_style(const PrintObject &object, const std::vector<int> &totals)
{
    SupportStyle                        style;
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return style;
    const NativeEntry *styling = nullptr;
    for (const NativeEntry *entry : mods->slots(MP_SUPPORT_STYLE_SLOT)) {
        if (entry->version < MP_SUPPORT_STYLE_VERSION || entry->table_size < sizeof(mp_support_style_v1))
            continue;
        const mp_support_style_v1 &t = table_of(*entry);
        mp_ctx ctx;
        ctx.mod           = entry->mod;
        ctx.print_config  = &object.print()->full_print_config();
        ctx.object_config = &object.config();
        ctx.cancelled     = [&object]() { return object.print()->canceled(); };
        mp_support_style_object_v1 out{};
        out.size = sizeof(out);
        void *state = nullptr;
        call(*entry, [&]() { return t.open(entry->user, &ctx, &out, &state); });
        // Closes the state however this leaves (nothing to close when the mod opened none).
        std::unique_ptr<void, std::function<void(void *)>> closer(state, [entry, &t](void *s) {
            entry->mod->guarded_call([&]() {
                t.close(entry->user, s);
                return int32_t(MP_OK);
            });
        });
        // What the mod warned about (host->warn): an error stops the slice, a warning reaches the object (review
        // 2026-10-22: they were dropped).
        for (const auto &[level, text] : ctx.warnings) {
            if (level >= MP_LOG_ERROR)
                throw SlicingError(entry->mod->id() + ": " + mod_text(entry->mod->id(), text));
            if (level == MP_LOG_WARNING)
                const_cast<PrintObject &>(object).add_module_warning(PrintStateBase::WarningLevel::NON_CRITICAL, mod_text(entry->mod->id(), text));
        }
        if (state == nullptr)
            continue;
        if (styling != nullptr)
            throw SlicingError(Slic3r::format(_u8L("The mods %1% and %2% both change how the support of object \"%3%\" is printed. "
                                                   "Turn one of them off."),
                                              styling->mod->id(), entry->mod->id(), object.model_object()->name));
        styling                 = entry;
        style.interface_pattern = pattern_of(out.interface_pattern, *entry);
        style.normal_wall_loops = wall_loops_of(out.normal_wall_loops, *entry);
        style.tree_wall_loops   = wall_loops_of(out.tree_wall_loops, *entry);
        style.interface_temperature = temperature_of(out.sublayer_temperature, *entry);
        for (const int total : totals) {
            if (total <= 0 || style.interface_layers.count(total))
                continue;
            std::vector<std::optional<InterfaceLayerStyle>> &layers = style.interface_layers[total];
            layers.resize(size_t(total));
            for (int number = 1; number <= total; ++number) {
                mp_support_interface_style_v1 layer{};
                layer.size = sizeof(layer);
                call(*entry, [&]() { return t.interface_layer(entry->user, state, number, total, &layer); });
                if (layer.restyle == 0)
                    continue;
                if (!std::isfinite(layer.angle_deg))
                    throw SlicingError(entry->mod->id() + ": the support style returned an angle that is not a number");
                layers[size_t(number - 1)] = InterfaceLayerStyle{pattern_of(layer.pattern, *entry), Geometry::deg2rad(float(layer.angle_deg))};
            }
        }
        if (object.has_support_material())
            object.note_native_mod(entry->mod->id());
    }
    return style;
}

} // namespace ModHost
} // namespace Slic3r

namespace Slic3r {
namespace ModHost {

int style_interface_temperature(const DynamicPrintConfig &print_config, const ConfigBase *object_config)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return 0;
    for (const NativeEntry *entry : mods->slots(MP_SUPPORT_STYLE_SLOT)) {
        if (entry->version < MP_SUPPORT_STYLE_VERSION || entry->table_size < sizeof(mp_support_style_v1))
            continue;
        const mp_support_style_v1 &t = table_of(*entry);
        mp_ctx ctx;
        ctx.mod           = entry->mod;
        ctx.print_config  = &print_config;
        ctx.object_config = object_config;
        ctx.cancelled     = []() { return false; };
        mp_support_style_object_v1 out{};
        out.size = sizeof(out);
        void *state = nullptr;
        call(*entry, [&]() { return t.open(entry->user, &ctx, &out, &state); });
        if (state == nullptr)
            continue;
        entry->mod->guarded_call([&]() {
            t.close(entry->user, state);
            return int32_t(MP_OK);
        });
        // The first implementation that styles these settings, as support_style() would refuse a second one.
        return temperature_of(out.sublayer_temperature, *entry);
    }
    return 0;
}

} // namespace ModHost
} // namespace Slic3r
