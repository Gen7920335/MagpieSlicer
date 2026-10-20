// LESIC ring calibration (Magpie line 1), the G-code side: one nozzle temperature per band of layers, above the
// first layer a wall speed that follows the angle around the ring (a volumetric speed from mvs_start to mvs_end),
// and on the first layer the bottom label, the ring's value labels and an inner brim, drawn as free moves. The
// calibration itself (menu, dialog, the ring model) stays in the application until calibrations are declared by
// mods (mod design 6-9); this mod reads its values through the gcode.events writer. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_gcode_events.h>

#include "annotations.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#define LESIC_MOD_ID "lesic"

MAGPIE_MOD_DEFINE(LESIC_MOD_ID, "2.0.0")

namespace lesic {

std::optional<double> config_f64(mp_ctx *ctx, const char *key, int32_t index = -1)
{
    double v = 0.;
    return magpie::host()->config_f64(ctx, key, index, &v) == MP_OK ? std::optional<double>(v) : std::nullopt;
}

// A list setting's element as the core's get_at() reads it: the first element for an index past the end.
std::optional<double> config_at(mp_ctx *ctx, const char *key, int32_t index)
{
    if (auto v = config_f64(ctx, key, index))
        return v;
    return index > 0 ? config_f64(ctx, key, 0) : std::nullopt;
}

std::string config_str(mp_ctx *ctx, const char *key, int32_t index = -1)
{
    std::vector<char> buf(1024);
    for (;;) {
        const int rc = magpie::host()->config_str(ctx, key, index, buf.data(), uint32_t(buf.size()));
        if (rc == MP_OK)
            return std::string(buf.data());
        if (rc == MP_ERR_ARGUMENT && index > 0)
            return config_str(ctx, key, 0);
        if (rc != MP_ERR_BUFFER || buf.size() > (1u << 24))
            return {};
        buf.resize(buf.size() * 4);
    }
}

// One temperature per band of layers_per_temp layers, from start toward end by step (calib.hpp).
double temperature_for_band(double start, double end, double step, int band)
{
    const double offset = std::max(0, band) * std::abs(step);
    return start >= end ? std::max(end, start - offset) : std::min(end, start + offset);
}

struct State
{
    const mp_gcode_writer_v1 *w = nullptr;
    Calib_Params              params;
};

void text(const State &s, const std::string &t) { s.w->text(s.w->w, t.c_str()); }

Calib_Params read_params(const mp_gcode_writer_v1 &w)
{
    Calib_Params p;
    auto get = [&w](const char *name, double fallback) {
        double v = fallback;
        return w.calibration_value(w.w, name, &v) == MP_OK ? v : fallback;
    };
    p.start                 = get("start", p.start);
    p.end                   = get("end", p.end);
    p.step                  = get("step", p.step);
    p.mvs_start             = get("mvs_start", p.mvs_start);
    p.mvs_end               = get("mvs_end", p.mvs_end);
    p.lesic_layers_per_temp = int(get("lesic_layers_per_temp", p.lesic_layers_per_temp));
    p.lesic_center_x        = get("lesic_center_x", p.lesic_center_x);
    p.lesic_center_y        = get("lesic_center_y", p.lesic_center_y);
    p.lesic_circle_diameter = get("lesic_circle_diameter", p.lesic_circle_diameter);
    p.lesic_line_width      = get("lesic_line_width", p.lesic_line_width);
    p.lesic_layer_height    = get("lesic_layer_height", p.lesic_layer_height);
    p.lesic_small_bed       = get("lesic_small_bed", 0.) != 0.;
    return p;
}

void on_layer_begin(const State &s)
{
    const Calib_Params &params          = s.params;
    const int           layers_per_temp = std::max(1, params.lesic_layers_per_temp);
    const int           temp_band       = std::max(0, int(s.w->layer_index(s.w->w))) / layers_per_temp;
    const double        temp            = temperature_for_band(params.start, params.end, params.step, temp_band);
    s.w->set_temperature(s.w->w, int32_t(static_cast<unsigned int>(std::lround(temp))), 0, -1, 0);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "; LESIC: TEMP:%g BAND:%d MVS:%g-%g\n", temp, temp_band, params.mvs_start, params.mvs_end);
    text(s, buf);
}

// The ring labels and inner brim on the first layer.
void on_first_layer_end(const State &s, mp_ctx *ctx)
{
    const Calib_Params &params      = s.params;
    const int32_t       filament_id = s.w->filament(s.w->w);
    if (params.lesic_circle_diameter <= 0.0 || params.mvs_end <= params.mvs_start || filament_id < 0)
        return;

    const double filament_diameter = config_at(ctx, "filament_diameter", filament_id).value_or(0.);
    const double filament_area     = M_PI * ((filament_diameter * 0.5) * (filament_diameter * 0.5));
    if (filament_area <= EPSILON)
        return;

    const double layer_height = params.lesic_layer_height > 0.0 ? params.lesic_layer_height : std::max(0.05, s.w->last_height(s.w->w));
    const double flow_scale   = layer_height * config_f64(ctx, "print_flow_ratio").value_or(1.) / filament_area;
    LabelConfig  label;
    label.printer_model     = config_str(ctx, "printer_model");
    label.filament_type     = config_str(ctx, "filament_type", filament_id);
    label.filament_vendor   = config_str(ctx, "filament_vendor", filament_id);
    label.nozzle_diameter_0 = config_at(ctx, "nozzle_diameter", 0).value_or(0.);
    label.layer_height      = config_f64(ctx, "layer_height").value_or(0.);
    std::vector<LesicAnnotationSegment>       segments      = lesic_bottom_label_segments(params, label);
    const std::vector<LesicAnnotationSegment> ring_segments = lesic_ring_annotation_segments(params);
    segments.insert(segments.end(), ring_segments.begin(), ring_segments.end());
    const size_t                              label_and_ring_segment_count = segments.size();
    const std::vector<LesicAnnotationSegment> inner_brim_segments          = lesic_inner_brim_segments(params, segments);
    segments.insert(segments.end(), inner_brim_segments.begin(), inner_brim_segments.end());
    if (segments.empty() || flow_scale <= EPSILON)
        return;

    std::ostringstream gcode;
    gcode << "\n; ---------- LESIC labels and ring annotations ----------\n";
    const std::vector<std::string> label_lines = lesic_label_lines(params, label);
    gcode << "; lesic_bottom_label_lines=";
    for (size_t i = 0; i < label_lines.size(); ++i) {
        if (i > 0)
            gcode << " | ";
        gcode << label_lines[i];
    }
    gcode << "\n";
    gcode << "; lesic_ring_mvs_values=";
    const std::vector<double> label_values = lesic_ring_label_values(params.mvs_start, params.mvs_end);
    for (size_t i = 0; i < label_values.size(); ++i) {
        if (i > 0)
            gcode << ",";
        gcode << lesic_format_label_value(label_values[i]);
    }
    gcode << "\n";
    gcode << "; lesic_bottom_label_height=" << lesic_format_label_value(lesic_bottom_label_height(params, label_lines)) << "\n";
    gcode << "; lesic_bottom_label_stroke_width=" << lesic_format_label_value(lesic_bottom_label_stroke_width(params)) << "\n";
    gcode << "; lesic_annotation_segments=" << segments.size() << "\n";
    gcode << "; lesic_label_and_ring_segments=" << label_and_ring_segment_count << "\n";
    gcode << "; lesic_inner_brim_lines=" << LESIC_INNER_BRIM_LINES << "\n";
    gcode << "; lesic_inner_brim_segments=" << inner_brim_segments.size() << "\n";
    text(s, gcode.str());
    s.w->set_feedrate(s.w->w, 20.0 * 60.0, "LESIC annotation speed");
    // The segments are in plate coordinates.
    s.w->plate_frame(s.w->w);

    for (const LesicAnnotationSegment &segment : segments) {
        const double length = (segment.b - segment.a).norm();
        if (length <= EPSILON)
            continue;
        s.w->raw_travel(s.w->w, segment.a.x(), segment.a.y(), "LESIC annotation travel");
        s.w->raw_move(s.w->w, segment.b.x(), segment.b.y(), flow_scale * segment.width * length, 0, "LESIC annotation");
    }
    text(s, "; ---------- end LESIC labels and ring annotations ----------\n\n");
}

mp_status open(void *, mp_ctx *, const mp_gcode_export_v1 *info, const mp_gcode_writer_v1 *writer, void **state, uint32_t *flags)
{
    return magpie::guarded([&] {
        *state = nullptr;
        if (info->calibration == nullptr || std::string(info->calibration) != "lesic")
            return;
        auto s    = std::make_unique<State>();
        s->w      = writer;
        s->params = read_params(*writer);
        *flags    = MP_GCODE_WANTS_PATH_SPEEDS | MP_GCODE_USED;
        *state    = s.release();
    });
}

mp_status event(void *, void *state, mp_ctx *ctx, mp_gcode_event_v1 *ev)
{
    return magpie::guarded([&] {
        const State &s = *static_cast<const State *>(state);
        if (ev->kind == MP_GCODE_EV_LAYER_BEGIN)
            on_layer_begin(s);
        else if (ev->kind == MP_GCODE_EV_FIRST_LAYER_END)
            on_first_layer_end(s, ctx);
    });
}

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

// Above the first layer the walls of the ring print at a volumetric speed that grows with the angle around the
// ring, from mvs_start to mvs_end mm3/s.
mp_status path_speeds(void *, void *state, mp_ctx *, const mp_gcode_speed_query_v1 *query, double *speeds, int32_t *changed)
{
    return magpie::guarded([&] {
        *changed       = 0;
        const State &s = *static_cast<const State *>(state);
        if (s.w->first_layer(s.w->w) != 0 || s.w->over_raft(s.w->w) != 0 || !(query->mm3_per_mm > EPSILON) ||
            (query->role != MP_ROLE_EXTERNAL_PERIMETER && query->role != MP_ROLE_PERIMETER) || query->n <= 1)
            return;
        const Calib_Params &params = s.params;
        Vec2d               center(params.lesic_center_x, params.lesic_center_y);
        if (params.lesic_circle_diameter <= 0.0) {
            // The bed's centre (its printable area's bounding box).
            std::vector<double> area(2 * size_t(s.w->printable_area(s.w->w, nullptr, 0)));
            s.w->printable_area(s.w->w, area.data(), uint32_t(area.size() / 2));
            Vec2d min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            Vec2d max(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
            for (size_t i = 0; i + 1 < area.size(); i += 2) {
                min = Vec2d(std::min(min.x(), area[i]), std::min(min.y(), area[i + 1]));
                max = Vec2d(std::max(max.x(), area[i]), std::max(max.y(), area[i + 1]));
            }
            center = (min + max) * 0.5;
        }
        const double mvs_start = params.mvs_start;
        const double mvs_range = params.mvs_end - params.mvs_start;
        for (uint32_t i = 0; i < query->n; ++i) {
            const uint32_t next = std::min(i + 1, query->n - 1);
            const Vec2d    a(query->xy[2 * i], query->xy[2 * i + 1]);
            const Vec2d    b(query->xy[2 * next], query->xy[2 * next + 1]);
            const Vec2d    mid        = (a + b) * 0.5;
            const double   angle      = std::atan2(mid.y() - center.y(), mid.x() - center.x()) * 180.0 / M_PI;
            const double   ratio      = lesic_angle_progress(angle);
            const double   target_mvs = mvs_start + mvs_range * ratio;
            // Slowest speed, mm/s.
            speeds[i] = std::max(1.0, target_mvs / query->mm3_per_mm);
        }
        *changed = 1;
    });
}

mp_status start_gcode(void *, void *, mp_ctx *, int32_t, const char *, mp_gcode_text_v1 *) { return {MP_OK, nullptr}; }

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

} // namespace lesic

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_GCODE_EVENTS_SLOT, MP_GCODE_EVENTS_VERSION, "lesic", &lesic::table, sizeof(lesic::table),
                                   nullptr);
}

void magpie::mod_shutdown() {}
