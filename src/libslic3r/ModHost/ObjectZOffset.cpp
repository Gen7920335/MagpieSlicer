#include "ObjectZOffset.hpp"

#include "NativeMods.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"

#include "../../../mods/sdk/include/magpie_slot_object_z_offset.h"

#include <cmath>
#include <memory>

namespace Slic3r {
namespace ModHost {

namespace {

// The lift one implementation reports, or the reason it has none.
bool lift_of(const NativeEntry &entry, const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config, double &lift_mm,
             std::string &error)
{
    const auto &table = *static_cast<const mp_object_z_offset_v1 *>(entry.table);
    mp_ctx      ctx;
    ctx.mod           = entry.mod;
    ctx.print_config  = &print_config;
    ctx.object_config = &object_config;
    ctx.cancelled     = []() { return false; };
    std::string   status_message, guard_message;
    double        lift = 0.;
    const int32_t rc   = entry.mod->guarded_call([&]() {
        const mp_status st = table.z_offset(entry.user, &ctx, &lift);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc != MP_OK) {
        error = entry.mod->id() + ": " + (!status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                          !guard_message.empty()  ? guard_message : std::string("the object lift failed"));
        return false;
    }
    if (!std::isfinite(lift)) {
        error = entry.mod->id() + ": the object lift is not a number";
        return false;
    }
    lift_mm = lift;
    return true;
}

} // namespace

double object_z_offset(const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config, const std::string &object_name,
                       std::string *error)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return 0.;
    auto fail = [error](const std::string &message) {
        if (error == nullptr)
            throw SlicingError(message);
        *error = message;
        return 0.;
    };
    double             result  = 0.;
    const NativeEntry *lifting = nullptr;
    for (const NativeEntry *entry : mods->slots(MP_OBJECT_Z_OFFSET_SLOT)) {
        if (entry->version < MP_OBJECT_Z_OFFSET_VERSION || entry->table_size < sizeof(mp_object_z_offset_v1))
            continue;
        double      lift = 0.;
        std::string message;
        if (!lift_of(*entry, print_config, object_config, lift, message))
            return fail(message);
        // Zero or less: this implementation does not lift the object (SlicingParameters lifts by more than zero only).
        if (lift <= 0.)
            continue;
        if (lifting != nullptr)
            return fail(Slic3r::format(_u8L("The mods %1% and %2% both lift object \"%3%\" off the bed. Turn one of them off."),
                                       lifting->mod->id(), entry->mod->id(), object_name));
        lifting = entry;
        result  = lift;
    }
    return result;
}

} // namespace ModHost
} // namespace Slic3r
