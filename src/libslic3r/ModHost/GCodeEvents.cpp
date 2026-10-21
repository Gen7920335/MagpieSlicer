#include "GCodeEvents.hpp"

#include "NativeMods.hpp"
#include "SupportStyle.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace ModHost {

// One open implementation: its entry, state, flags and the writer table it was given (w = this record).
struct GCodeEvents::Open
{
    GCodeEvents       *events = nullptr;
    void              *host_data = nullptr;
    const NativeEntry *entry = nullptr;
    void              *state = nullptr;
    uint32_t           flags = 0;
    mp_gcode_writer_v1 writer{};
};

namespace {

const mp_gcode_events_v1 &table_of(const NativeEntry &entry) { return *static_cast<const mp_gcode_events_v1 *>(entry.table); }

bool usable(const NativeEntry &entry) { return entry.version >= MP_GCODE_EVENTS_VERSION && entry.table_size >= sizeof(mp_gcode_events_v1); }

// The slicing warnings a call left in ctx: errors stop the slice, warnings reach the print's active step.
void flush_warnings(Print &print, const NativeEntry &entry, mp_ctx &ctx)
{
    std::vector<std::pair<int32_t, std::string>> warnings;
    {
        std::lock_guard<std::mutex> lock(ctx.warnings_mutex);
        warnings.swap(ctx.warnings);
    }
    for (const auto &[level, text] : warnings) {
        if (level >= MP_LOG_ERROR)
            throw SlicingError(entry.mod->id() + ": " + mod_text(entry.mod->id(), text));
        print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, mod_text(entry.mod->id(), text));
    }
}

// Runs one call into a mod outside an export (validation, previews); a failure is returned as a message.
bool call_without_export(const NativeEntry &entry, const std::function<mp_status()> &fn, std::string &error)
{
    std::string   status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = fn();
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc == MP_OK)
        return true;
    error = entry.mod->id() + ": " + (!status_message.empty() ? mod_text(entry.mod->id(), status_message) :
                                      !guard_message.empty()  ? guard_message : std::string("the G-code mod failed"));
    return false;
}

} // namespace

bool comment_lines_only(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = std::min(text.find('\n', begin), text.size());
        const size_t first = text.find_first_not_of(" \t\r", begin);
        if (first < end && text[first] != ';')
            return false;
        begin = end + 1;
    }
    return true;
}

GCodeEvents::GCodeEvents(Print &print, const ConfigBase &settings, const mp_gcode_writer_v1 &base, void *host_data,
                         const std::string &calibration, std::function<void()> after_call)
    : m_print(print), m_settings(settings), m_after_call(std::move(after_call))
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return;
    mp_gcode_export_v1 info{};
    info.size        = sizeof(info);
    info.calibration = calibration.c_str();
    for (const NativeEntry *entry : mods->slots(MP_GCODE_EVENTS_SLOT)) {
        if (!usable(*entry))
            continue;
        auto open       = std::make_unique<Open>();
        open->events    = this;
        open->host_data = host_data;
        open->entry     = entry;
        open->writer    = base;
        open->writer.w  = open.get();
        void    *state = nullptr;
        uint32_t flags = 0;
        // Opened before the export applies its settings: the mod reads the print's own.
        m_opening = true;
        this->call(*open, "open", [&](mp_ctx &ctx) { return table_of(*entry).open(entry->user, &ctx, &info, &open->writer, &state, &flags); });
        m_opening = false;
        if (state == nullptr)
            continue;
        // Only official mods may change the start G-code (audit decision B, as for rules on G-code settings).
        if ((flags & MP_GCODE_WANTS_START_GCODE) && !entry->mod->manifest().official) {
            BOOST_LOG_TRIVIAL(warning) << entry->mod->id() << ": not an official mod, its start G-code filter is not run";
            flags &= ~uint32_t(MP_GCODE_WANTS_START_GCODE);
        }
        open->state = state;
        open->flags = flags;
        m_wants |= flags;
        if (flags & MP_GCODE_USED)
            m_used.insert(entry->mod->id());
        m_open.push_back(std::move(open));
    }
}

GCodeEvents::~GCodeEvents()
{
    for (auto it = m_open.rbegin(); it != m_open.rend(); ++it) {
        const NativeEntry *entry = (*it)->entry;
        void              *state = (*it)->state;
        entry->mod->guarded_call([&]() {
            table_of(*entry).close(entry->user, state);
            return int32_t(MP_OK);
        });
    }
}

void GCodeEvents::call(Open &open, const char *what, const std::function<mp_status(mp_ctx &)> &fn)
{
    mp_ctx ctx;
    ctx.mod           = open.entry->mod;
    ctx.print_config  = &m_print.full_print_config();
    ctx.object_config = m_opening ? nullptr : &m_settings;
    ctx.cancelled     = [this]() { return m_print.canceled(); };
    std::string   status_message, guard_message;
    const int32_t rc = open.entry->mod->guarded_call([&]() {
        const mp_status st = fn(ctx);
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    // What the writer could not complete comes first: it is the cause of a failure the mod reports after it.
    if (m_after_call)
        m_after_call();
    if (rc == MP_ERR_CANCELLED)
        throw CanceledException();
    if (rc != MP_OK)
        throw SlicingError(open.entry->mod->id() + ": " +
                           (!status_message.empty() ? mod_text(open.entry->mod->id(), status_message) :
                            !guard_message.empty()  ? guard_message : std::string("the G-code mod failed in ") + what));
    flush_warnings(m_print, *open.entry, ctx);
}

void GCodeEvents::raise(mp_gcode_event_v1 &ev)
{
    int32_t owns = 0;
    for (const std::unique_ptr<Open> &open : m_open) {
        if (ev.kind == MP_GCODE_EV_PATH && !(open->flags & MP_GCODE_WANTS_PATHS))
            continue;
        ev.owns_temperature = 0;
        this->call(*open, "event", [&](mp_ctx &ctx) { return table_of(*open->entry).event(open->entry->user, open->state, &ctx, &ev); });
        owns |= ev.owns_temperature;
    }
    ev.owns_temperature = owns;
}

bool GCodeEvents::support_last()
{
    bool last = false;
    for (const std::unique_ptr<Open> &open : m_open) {
        int32_t out = 0;
        this->call(*open, "support_last",
                   [&](mp_ctx &ctx) { return table_of(*open->entry).support_last(open->entry->user, open->state, &ctx, &out); });
        last |= out != 0;
    }
    return last;
}

bool GCodeEvents::tower_on_layer(double print_z)
{
    bool tower = false;
    for (const std::unique_ptr<Open> &open : m_open) {
        int32_t out = 0;
        this->call(*open, "tower_on_layer",
                   [&](mp_ctx &ctx) { return table_of(*open->entry).tower_on_layer(open->entry->user, open->state, &ctx, print_z, &out); });
        tower |= out != 0;
    }
    return tower;
}

bool GCodeEvents::path_speeds(const mp_gcode_speed_query_v1 &query, std::vector<double> &speeds)
{
    for (const std::unique_ptr<Open> &open : m_open) {
        if (!(open->flags & MP_GCODE_WANTS_PATH_SPEEDS))
            continue;
        speeds.assign(query.n, query.speed);
        int32_t changed = 0;
        this->call(*open, "path_speeds", [&](mp_ctx &ctx) {
            return table_of(*open->entry).path_speeds(open->entry->user, open->state, &ctx, &query, speeds.data(), &changed);
        });
        if (changed == 0)
            continue;
        for (const double s : speeds)
            if (!std::isfinite(s) || s <= 0.)
                throw SlicingError(open->entry->mod->id() + ": a path speed is not a positive number");
        return true;
    }
    return false;
}

void GCodeEvents::start_gcode(int32_t phase, std::string &text, std::vector<std::string> &notes)
{
    for (const std::unique_ptr<Open> &open : m_open) {
        if (!(open->flags & MP_GCODE_WANTS_START_GCODE))
            continue;
        mp_gcode_text_v1 out{};
        out.size = sizeof(out);
        // Frees what the mod allocated however this leaves.
        std::unique_ptr<mp_gcode_text_v1, void (*)(mp_gcode_text_v1 *)> release(&out, [](mp_gcode_text_v1 *o) {
            native_host_api().free(o->text);
            native_host_api().free(o->note);
        });
        this->call(*open, "start_gcode", [&](mp_ctx &ctx) {
            return table_of(*open->entry).start_gcode(open->entry->user, open->state, &ctx, phase, text.c_str(), &out);
        });
        if (out.text != nullptr)
            text = out.text;
        if (out.note != nullptr) {
            std::string note(out.note);
            if (!note.empty() && note.back() != '\n')
                note += '\n';
            if (!comment_lines_only(note))
                throw SlicingError(open->entry->mod->id() + ": a start G-code note must be comment lines");
            notes.push_back(std::move(note));
        }
    }
}

void *GCodeEvents::writer_host(void *w) { return w != nullptr ? static_cast<Open *>(w)->host_data : nullptr; }

void GCodeEvents::writer_mark_used(void *w)
{
    if (w != nullptr)
        static_cast<Open *>(w)->events->m_used.insert(static_cast<Open *>(w)->entry->mod->id());
}

std::optional<std::pair<std::string, std::string>> validate_gcode_events(const PrintObject &object)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return std::nullopt;
    const PrintObjectConfig &oc = object.config();
    std::vector<int32_t>     filaments;
    if (oc.support_filament.value > 0)
        filaments.push_back(int32_t(oc.support_filament.value - 1));
    else
        for (const unsigned int filament : object.object_extruders())
            filaments.push_back(int32_t(filament));
    mp_gcode_validate_v1 in{};
    in.size                = sizeof(in);
    in.has_support         = object.has_support_material() ? 1 : 0;
    in.support_filaments   = filaments.data();
    in.n_support_filaments = uint32_t(filaments.size());
    for (const NativeEntry *entry : mods->slots(MP_GCODE_EVENTS_SLOT)) {
        if (!usable(*entry))
            continue;
        mp_ctx ctx;
        ctx.mod           = entry->mod;
        ctx.print_config  = &object.print()->full_print_config();
        ctx.object_config = &oc;
        ctx.cancelled     = []() { return false; };
        mp_gcode_problem_v1 out{};
        out.size = sizeof(out);
        std::string error;
        if (!call_without_export(*entry, [&]() { return table_of(*entry).validate(entry->user, &ctx, &in, &out); }, error))
            return std::make_pair(error, std::string());
        if (out.message != nullptr) {
            // The translated text with its placeholders filled in, as Slic3r::format() would.
            boost::format f(mod_text(entry->mod->id(), out.message));
            f.exceptions(boost::io::all_error_bits ^ (boost::io::too_many_args_bit | boost::io::too_few_args_bit));
            for (uint32_t i = 0; i < std::min<uint32_t>(out.n_args, 4); ++i)
                f % out.args[i];
            return std::make_pair(f.str(), std::string(out.setting != nullptr ? out.setting : ""));
        }
    }
    return std::nullopt;
}

bool gcode_events_tower_requested(const PrintObject &object)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return false;
    for (const NativeEntry *entry : mods->slots(MP_GCODE_EVENTS_SLOT)) {
        if (!usable(*entry))
            continue;
        mp_ctx ctx;
        ctx.mod           = entry->mod;
        ctx.print_config  = &object.print()->full_print_config();
        ctx.object_config = &object.config();
        ctx.cancelled     = []() { return false; };
        int32_t     out = 0;
        std::string error;
        if (!call_without_export(*entry, [&]() { return table_of(*entry).tower_requested(entry->user, &ctx, &out); }, error))
            throw SlicingError(error);
        if (out != 0)
            return true;
    }
    return false;
}

std::optional<mp_tower_preview_v1> gcode_events_tower_preview(const DynamicPrintConfig &config)
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (!mods)
        return std::nullopt;
    const int32_t style_temperature = style_interface_temperature(config, nullptr);
    for (const NativeEntry *entry : mods->slots(MP_GCODE_EVENTS_SLOT)) {
        if (!usable(*entry))
            continue;
        mp_ctx ctx;
        ctx.mod          = entry->mod;
        ctx.print_config = &config;
        ctx.cancelled    = []() { return false; };
        mp_tower_preview_v1 out{};
        out.size = sizeof(out);
        std::string error;
        // A preview never stops anything: a failing mod shows no tower.
        if (!call_without_export(*entry, [&]() { return table_of(*entry).tower_preview(entry->user, &ctx, style_temperature, &out); }, error))
            continue;
        if (out.show != 0 && std::isfinite(out.requested_size) && std::isfinite(out.band_width) && std::isfinite(out.brim_width))
            return out;
    }
    return std::nullopt;
}

std::string calibration_name(const Print &print)
{
    switch (print.calib_mode()) {
    case CalibMode::Calib_None: return "";
    case CalibMode::Calib_PA_Line: return "pa_line";
    case CalibMode::Calib_PA_Pattern: return "pa_pattern";
    case CalibMode::Calib_PA_Tower: return "pa_tower";
    case CalibMode::Calib_Auto_PA_Line: return "auto_pa_line";
    case CalibMode::Calib_Flow_Rate: return "flow_rate";
    case CalibMode::Calib_Temp_Tower: return "temp_tower";
    case CalibMode::Calib_Vol_speed_Tower: return "vol_speed_tower";
    case CalibMode::Calib_VFA_Tower: return "vfa_tower";
    case CalibMode::Calib_Retraction_tower: return "retraction_tower";
    case CalibMode::Calib_Input_shaping_freq: return "input_shaping_freq";
    case CalibMode::Calib_Input_shaping_damp: return "input_shaping_damp";
    case CalibMode::Calib_Cornering: return "cornering";
    case CalibMode::Calib_LESIC: return "lesic";
    }
    return "unknown";
}

bool calibration_value(const Print &print, const char *name, double &out)
{
    if (name == nullptr)
        return false;
    const Calib_Params &p = print.calib_params();
    const std::pair<const char *, double> values[] = {
        {"extruder_id", double(p.extruder_id)},
        {"start", p.start},
        {"end", p.end},
        {"step", p.step},
        {"print_numbers", p.print_numbers ? 1. : 0.},
        {"freq_start_x", p.freqStartX},
        {"freq_end_x", p.freqEndX},
        {"freq_start_y", p.freqStartY},
        {"freq_end_y", p.freqEndY},
        {"test_model", double(p.test_model)},
        {"vfa_layer_height", p.vfa_layer_height},
        {"nozzle_based_resize", p.nozzle_based_resize ? 1. : 0.},
        {"mvs_start", p.mvs_start},
        {"mvs_end", p.mvs_end},
        {"lesic_layers_per_temp", double(p.lesic_layers_per_temp)},
        {"lesic_center_x", p.lesic_center_x},
        {"lesic_center_y", p.lesic_center_y},
        {"lesic_circle_diameter", p.lesic_circle_diameter},
        {"lesic_line_width", p.lesic_line_width},
        {"lesic_layer_height", p.lesic_layer_height},
        {"lesic_small_bed", p.lesic_small_bed ? 1. : 0.},
    };
    for (const auto &[n, v] : values)
        if (std::strcmp(n, name) == 0) {
            out = v;
            return true;
        }
    return false;
}

} // namespace ModHost
} // namespace Slic3r
