// Low temperature interface calibrations (Magpie). Two calibrations of the Calibration menu, declared with their forms
// in mod.json:
//   - "lesic", the LESIC ring of Magpie line 1: one nozzle temperature per band of layers, above the first layer a wall
//     speed that follows the angle around the ring (a volumetric speed from mvs_start to mvs_end), and on the first
//     layer the bottom label, the ring's value labels and an inner brim. Moved here from the lesic mod and, for its
//     project, from the application (Plater::calib_lesic), with its arithmetic and settings unchanged.
//   - "grid", the one-touch interface grid (Gen 2026-10-28): per specimen a 10 x 10 x 5 mm cube, the support interface
//     only, a 10 x 10 x 3 mm lid and a 5 mm handle of 10 mm on it, all model; one specimen for each pair of values of
//     two settings. The interfaces print at their low temperature through low_temp_interface; this mod lays the
//     specimens out hottest interface first and prints the row and column values on the first layer.
// The project comes from the calibration.project slot, the marks from gcode.events. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_calibration_project.h>
#include <magpie_slot_gcode_events.h>

#include "annotations.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#define LTC_MOD_ID "low_temp_calibration"

MAGPIE_MOD_DEFINE(LTC_MOD_ID, "1.1.0")

namespace lesic {

// -- Settings through the host --

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

// Number of elements of a list setting.
int32_t config_count(mp_ctx *ctx, const char *key)
{
    int32_t n = 0;
    while (config_f64(ctx, key, n))
        ++n;
    return n;
}

// The bounding box of the printer's printable area, mm.
struct Bed
{
    double min_x = 0., min_y = 0., max_x = 0., max_y = 0.;
    double width() const { return max_x - min_x; }
    double depth() const { return max_y - min_y; }
    double center_x() const { return 0.5 * (min_x + max_x); }
    double center_y() const { return 0.5 * (min_y + max_y); }
};

Bed bed_of(mp_ctx *ctx)
{
    Bed  bed;
    bool any = false;
    // Points are serialized "XxY"; an index past the list's end is refused.
    char buf[96];
    for (int32_t i = 0; magpie::host()->config_str(ctx, "printable_area", i, buf, uint32_t(sizeof(buf))) == MP_OK; ++i) {
        const std::string point(buf);
        const size_t      sep = point.find('x');
        if (sep == std::string::npos)
            break;
        const double x = std::strtod(point.substr(0, sep).c_str(), nullptr);
        const double y = std::strtod(point.substr(sep + 1).c_str(), nullptr);
        if (!any) {
            bed = Bed{x, y, x, y};
            any = true;
        } else {
            bed.min_x = std::min(bed.min_x, x);
            bed.min_y = std::min(bed.min_y, y);
            bed.max_x = std::max(bed.max_x, x);
            bed.max_y = std::max(bed.max_y, y);
        }
    }
    return bed;
}

// A number as the preset files write it, read back exactly (C locale).
std::string number(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    for (char *c = buf; *c != '\0'; ++c)
        if (*c == ',')
            *c = '.';
    return buf;
}

std::optional<double> field_number(const mp_calib_field_v1 *fields, uint32_t n, const char *id)
{
    for (uint32_t i = 0; i < n; ++i)
        if (std::strcmp(fields[i].id, id) == 0 && fields[i].value != nullptr) {
            char        *end = nullptr;
            const double v   = std::strtod(fields[i].value, &end);
            if (end != fields[i].value && *end == '\0' && std::isfinite(v))
                return v;
            return std::nullopt;
        }
    return std::nullopt;
}

std::string field_text(const mp_calib_field_v1 *fields, uint32_t n, const char *id)
{
    for (uint32_t i = 0; i < n; ++i)
        if (std::strcmp(fields[i].id, id) == 0 && fields[i].value != nullptr)
            return fields[i].value;
    return {};
}

void report(const mp_calib_project_v1 &project, int32_t kind, const char *text, std::initializer_list<double> args = {})
{
    const std::vector<double> a(args);
    project.report(project.p, kind, text, a.data(), uint32_t(a.size()));
}

// -- LESIC: the project (Plater::calib_lesic) --

// One temperature per band of layers_per_temp layers, from start toward end by step (calib.hpp).
int temperature_band_count(double start, double end, double step)
{
    const double positive_step = std::abs(step);
    const double span          = std::abs(start - end);
    if (positive_step <= 0.0 || span < positive_step)
        return 0;
    // 1e-9 degC absorbs the rounding of a span that is a whole number of steps.
    return static_cast<int>(std::ceil(span / positive_step - 1e-9)) + 1;
}

double temperature_for_band(double start, double end, double step, int band)
{
    const double offset = std::max(0, band) * std::abs(step);
    return start >= end ? std::max(end, start - offset) : std::min(end, start + offset);
}

struct LesicForm
{
    double start = 0., end = 0., step = 0., mvs_start = 0., mvs_end = 0.;
    double layers_per_temp = 0.;
};

std::optional<LesicForm> lesic_form(const mp_calib_field_v1 *fields, uint32_t n)
{
    LesicForm f;
    auto      read = [&](const char *id, double &out) {
        const std::optional<double> v = field_number(fields, n, id);
        if (v)
            out = *v;
        return v.has_value();
    };
    if (!read("start", f.start) || !read("end", f.end) || !read("step", f.step) || !read("mvs_start", f.mvs_start) ||
        !read("mvs_end", f.mvs_end) || !read("layers_per_temp", f.layers_per_temp))
        return std::nullopt;
    return f;
}

bool lesic_valid(const std::optional<LesicForm> &f)
{
    return f && f->start > 0.0 && f->end > 0.0 && f->step > 0.0 && std::abs(f->start - f->end) >= f->step && f->layers_per_temp >= 1. &&
           f->layers_per_temp == std::floor(f->layers_per_temp) && f->mvs_start > 0.0 && f->mvs_end > f->mvs_start;
}

void lesic_check(const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 &project)
{
    if (!lesic_valid(lesic_form(fields, n)))
        report(project, MP_CALIB_REPORT_ERROR,
               "Please input valid values:\nTemperature step > 0\nStart and end temperatures must differ by at least one step\nLayers per "
               "temperature > 0\nEnd MVS > Start MVS > 0");
}

void lesic_build(mp_ctx *ctx, const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 &project)
{
    const std::optional<LesicForm> form = lesic_form(fields, n);
    if (!lesic_valid(form)) {
        lesic_check(fields, n, project);
        return;
    }
    const LesicForm &f   = *form;
    const Bed        bed = bed_of(ctx);
    double nozzle_diameter = config_f64(ctx, "nozzle_diameter", 0).value_or(0.);
    if (nozzle_diameter <= 0.0)
        nozzle_diameter = 0.4;

    // The ring layout (calib.hpp make_lesic_calibration_layout). Bed sizes in mm: the ring keeps 10 mm from each
    // edge; beds under 200 mm get the compact labels.
    const int    layers_per_temp  = int(f.layers_per_temp);
    const double minimum_bed_size = std::max(1.0, std::min(bed.width(), bed.depth()));
    const double circle_diameter  = std::max(1.0, minimum_bed_size - 20.0);
    const double line_width       = nozzle_diameter * 1.2;
    const double layer_height     = nozzle_diameter * 0.6;
    const int    bands            = temperature_band_count(f.start, f.end, f.step);
    const double model_height     = layer_height * std::max(1, layers_per_temp) * bands;
    const bool   small_bed        = minimum_bed_size < 200.0;

    const int32_t object = project.add_object(project.p, "LESIC");
    project.add_cylinder(project.p, object, MP_CALIB_PART, bed.center_x(), bed.center_y(), 0., circle_diameter * 0.5, model_height,
                         M_PI / 180.0);

    auto print    = [&](const char *key, const std::string &value) { project.set_option(project.p, MP_CALIB_PRINT, key, value.c_str()); };
    auto filament = [&](const char *key, const std::string &value) { project.set_option(project.p, MP_CALIB_FILAMENT, key, value.c_str()); };
    auto printer  = [&](const char *key, const std::string &value) { project.set_option(project.p, MP_CALIB_PRINTER, key, value.c_str()); };
    auto obj      = [&](const char *key, const std::string &value) { project.set_object_option(project.p, object, key, value.c_str()); };

    // No nozzle may refuse the ring's layer height.
    std::string max_layer_heights;
    for (int32_t i = 0, count = config_count(ctx, "max_layer_height"); i < count; ++i)
        max_layer_heights += (i > 0 ? "," : "") + number(std::max(config_f64(ctx, "max_layer_height", i).value_or(0.), layer_height));
    if (!max_layer_heights.empty())
        printer("max_layer_height", max_layer_heights);

    const double filament_max_volumetric_speed = config_at(ctx, "filament_max_volumetric_speed", 0).value_or(0.);
    filament("filament_max_volumetric_speed", number(std::max(filament_max_volumetric_speed, f.mvs_end * 1.25)));
    filament("nozzle_temperature_initial_layer", std::to_string(int(std::lround(f.start))));
    filament("nozzle_temperature", std::to_string(int(std::lround(f.start))));
    filament("slow_down_layer_time", "0");
    filament("slow_down_min_speed", "0");
    filament("slow_down_for_layer_cooling", "0");

    printer("resonance_avoidance", "0");
    print("timelapse_type", "0");
    print("enable_wrapping_detection", "0");
    print("spiral_mode", "1");
    print("spiral_mode_smooth", "0");
    print("max_volumetric_extrusion_rate_slope", "0");
    print("initial_layer_print_height", number(layer_height));
    print("layer_height", number(layer_height));
    print("wall_loops", "1");
    print("top_shell_layers", "0");
    print("top_shell_thickness", "0");
    print("bottom_shell_layers", "0");
    print("bottom_shell_thickness", "0");
    print("sparse_infill_density", "0%");
    print("detect_thin_wall", "0");
    print("outer_wall_line_width", number(line_width));
    print("enable_overhang_speed", "0");

    obj("layer_height", number(layer_height));
    obj("outer_wall_line_width", number(line_width));
    obj("wall_loops", "1");
    obj("alternate_extra_wall", "0");
    obj("top_shell_layers", "0");
    obj("top_shell_thickness", "0");
    obj("bottom_shell_layers", "0");
    obj("bottom_shell_thickness", "0");
    obj("sparse_infill_density", "0%");
    print("brim_type", "no_brim");
    print("brim_width", "0");
    print("brim_object_gap", "0");
    obj("brim_type", "no_brim");
    obj("brim_width", "0");
    obj("brim_object_gap", "0");
    obj("precise_z_height", "0");

    auto value = [&](const char *name, double v) { project.set_value(project.p, name, v); };
    value("start", f.start);
    value("end", f.end);
    value("step", f.step);
    value("print_numbers", 1.);
    value("mvs_start", f.mvs_start);
    value("mvs_end", f.mvs_end);
    value("lesic_layers_per_temp", double(layers_per_temp));
    value("lesic_center_x", bed.center_x());
    value("lesic_center_y", bed.center_y());
    value("lesic_circle_diameter", circle_diameter);
    value("lesic_line_width", line_width);
    value("lesic_layer_height", layer_height);
    value("lesic_small_bed", small_bed ? 1. : 0.);
}

// -- LESIC: the G-code --

struct LesicState
{
    const mp_gcode_writer_v1 *w = nullptr;
    Calib_Params              params;
};

void text(const mp_gcode_writer_v1 &w, const std::string &t) { w.text(w.w, t.c_str()); }

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

void on_layer_begin(const LesicState &s)
{
    const Calib_Params &params          = s.params;
    const int           layers_per_temp = std::max(1, params.lesic_layers_per_temp);
    const int           temp_band       = std::max(0, int(s.w->layer_index(s.w->w))) / layers_per_temp;
    const double        temp            = temperature_for_band(params.start, params.end, params.step, temp_band);
    s.w->set_temperature(s.w->w, int32_t(static_cast<unsigned int>(std::lround(temp))), 0, -1, 0);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "; LESIC: TEMP:%g BAND:%d MVS:%g-%g\n", temp, temp_band, params.mvs_start, params.mvs_end);
    text(*s.w, buf);
}

// The ring labels and inner brim on the first layer.
void on_first_layer_end(const LesicState &s, mp_ctx *ctx)
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
    text(*s.w, gcode.str());
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
    text(*s.w, "; ---------- end LESIC labels and ring annotations ----------\n\n");
}

// Above the first layer the walls of the ring print at a volumetric speed that grows with the angle around the
// ring, from mvs_start to mvs_end mm3/s.
void lesic_path_speeds(const LesicState &s, const mp_gcode_speed_query_v1 *query, double *speeds, int32_t *changed)
{
    *changed = 0;
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
}

} // namespace lesic

namespace ltc {

using lesic::config_at;
using lesic::config_f64;
using lesic::field_number;
using lesic::field_text;
using lesic::number;
using lesic::report;

// -- The grid: specimens and their layout --

// Specimen and grid sizes, mm (Gen 2026-10-28): a 10 x 10 x 5 cube (its top on the nearest layer boundary, cube_top),
// a 10 x 10 x 3 lid, a 5 mm diameter handle 10 mm tall, 5 mm between specimens.
constexpr double SPECIMEN_SIZE   = 10.;
constexpr double CUBE_HEIGHT     = 5.;
constexpr double LID_HEIGHT      = 3.;
constexpr double HANDLE_DIAMETER = 5.;
constexpr double HANDLE_HEIGHT   = 10.;
constexpr double PITCH           = 15.;
// Room for the row and column values printed on the first layer beside the grid, mm.
constexpr double LABEL_HEIGHT = 3.;
constexpr double LABEL_MARGIN = 7.;
// Cells per axis at most (Gen 2026-10-28).
constexpr int MAX_CELLS = 10;
// Marlin EXTRUDE_MINTEMP and Klipper min_extrude_temp both default to 170 degC (low_temp_interface's validate).
constexpr double FIRMWARE_MIN_EXTRUDE_C = 170.;

// The settings an axis varies, per specimen.
struct Axis
{
    const char *id;       // the form's value
    const char *key;      // the object setting it writes
    const char *prefix;   // in the specimen's name
    bool        integer;  // a count of layers
    double      minimum;  // the smallest value the setting takes (degC, mm or layers)
    bool        axis;     // an axis may vary it; otherwise only a fixed value
};

// The bottom of the interface is a fixed value only, not an axis (Gen 2026-10-29).
const Axis AXES[] = {
    { "temperature", "low_temp_interface.temperature", "T", true, 1., true },
    { "top_z_distance", "support_top_z_distance", "Z", false, 0., true },
    { "bottom_z_distance", "support_bottom_z_distance", "B", false, 0., false },
    { "top_interface_layers", "support_interface_top_layers", "L", true, 1., true },
    { "bottom_interface_layers", "support_interface_bottom_layers", "BL", true, 0., false },
    { "interface_spacing", "support_interface_spacing", "S", false, 0., true },
};

const Axis *find_axis(const std::string &id)
{
    for (const Axis &axis : AXES)
        if (id == axis.id)
            return &axis;
    return nullptr;
}

struct AxisValues
{
    const Axis         *axis = nullptr; // nullptr: no axis (one cell)
    std::vector<double> values;
};

// The values of one axis of the form ("x" or "y"), or an error text.
std::optional<AxisValues> axis_values(const mp_calib_field_v1 *fields, uint32_t n, const std::string &name, std::string &error)
{
    AxisValues        out;
    const std::string id = field_text(fields, n, (name + "_axis").c_str());
    if (id == "none" && name == "y") {
        out.values.push_back(0.);
        return out;
    }
    out.axis = find_axis(id);
    if (out.axis != nullptr && !out.axis->axis)
        out.axis = nullptr;
    const std::optional<double> start = field_number(fields, n, (name + "_start").c_str());
    const std::optional<double> end   = field_number(fields, n, (name + "_end").c_str());
    const std::optional<double> count = field_number(fields, n, (name + "_count").c_str());
    if (out.axis == nullptr || !start || !end || !count) {
        error = "Every axis needs a setting, a start, an end and a number of cells.";
        return std::nullopt;
    }
    if (*count < 1. || *count > MAX_CELLS || *count != std::floor(*count)) {
        error = "An axis has 1 to 10 cells.";
        return std::nullopt;
    }
    const int cells = int(*count);
    for (int i = 0; i < cells; ++i) {
        double v = cells == 1 ? *start : *start + (*end - *start) * i / (cells - 1);
        // Hundredths of a mm (or degC) are as fine as a setting is given; layers and degrees are whole.
        v = out.axis->integer ? std::round(v) : std::round(v * 100.) / 100.;
        if (v < out.axis->minimum) {
            error = "A value of an axis is below what its setting takes.";
            return std::nullopt;
        }
        out.values.push_back(v);
    }
    return out;
}

struct Grid
{
    AxisValues x, y;
    // The form's fixed value of every axis setting (its "setting" field of the same id, the presets' value).
    std::map<std::string, double> fixed;
    int        nx() const { return int(x.values.size()); }
    int        ny() const { return int(y.values.size()); }
    double     width() const { return (nx() - 1) * PITCH + SPECIMEN_SIZE; }
    double     depth() const { return (ny() - 1) * PITCH + SPECIMEN_SIZE; }
};

std::optional<Grid> grid_of(const mp_calib_field_v1 *fields, uint32_t n, std::string &error)
{
    Grid g;
    std::optional<AxisValues> x = axis_values(fields, n, "x", error);
    if (!x)
        return std::nullopt;
    std::optional<AxisValues> y = axis_values(fields, n, "y", error);
    if (!y)
        return std::nullopt;
    if (y->axis != nullptr && y->axis == x->axis) {
        error = "The two axes need different settings.";
        return std::nullopt;
    }
    g.x = *x;
    g.y = *y;
    for (const Axis &axis : AXES) {
        const std::optional<double> v = field_number(fields, n, axis.id);
        if (!v) {
            error = "The form has no value for an axis setting.";
            return std::nullopt;
        }
        g.fixed[axis.id] = *v;
    }
    return g;
}

// The value of `axis` in cell (i, j), or nothing for an axis the grid does not vary.
std::optional<double> cell_value(const Grid &g, const Axis &axis, int i, int j)
{
    if (g.x.axis == &axis)
        return g.x.values[size_t(i)];
    if (g.y.axis == &axis)
        return g.y.values[size_t(j)];
    return std::nullopt;
}

// The value of the axis setting `id` in cell (i, j): the axis value, or the form's fixed value.
double cell_setting(const Grid &g, const char *id, int i, int j)
{
    if (std::optional<double> v = cell_value(g, *find_axis(id), i, j))
        return *v;
    return g.fixed.at(id);
}

// The interface temperature of a cell, degC.
double cell_temperature(const Grid &g, int i, int j) { return cell_setting(g, "temperature", i, j); }

// The gap between cube and lid, mm: only the interface fills it.
double gap_height(mp_ctx *ctx, const Grid &g, int i, int j)
{
    const double layer_height  = config_f64(ctx, "layer_height").value_or(0.2);
    const double top_z         = cell_setting(g, "top_z_distance", i, j);
    double       bottom_z      = cell_setting(g, "bottom_z_distance", i, j);
    const double top_layers    = cell_setting(g, "top_interface_layers", i, j);
    double       bottom_layers = cell_setting(g, "bottom_interface_layers", i, j);
    // Orca: -1 bottom interface layers are as many as the top; with no top gap the bottom interface touches the
    // object too (support_bottom_z_distance's tooltip).
    if (bottom_layers < 0.)
        bottom_layers = top_layers;
    if (top_z == 0. && bottom_layers > 0.)
        bottom_z = 0.;
    return bottom_z + (top_layers + bottom_layers) * layer_height + top_z;
}

// -- The grid: layers of the lid --

// One layer of a specimen, mm above the bed.
struct LayerSpan
{
    double bottom, top;
};

// Two print_z within this distance are one layer boundary, mm.
constexpr double Z_EPSILON = 1e-6;

// The layer boundaries every specimen shares: the first layer, then one layer height after another (mm).
struct LayerGrid
{
    double first_layer, layer_height;
    explicit LayerGrid(mp_ctx *ctx)
    {
        layer_height = config_f64(ctx, "layer_height").value_or(0.2);
        first_layer  = config_f64(ctx, "initial_layer_print_height").value_or(layer_height);
    }
    // The highest boundary at or below z.
    double below(double z) const { return first_layer + std::floor((z - first_layer) / layer_height + Z_EPSILON) * layer_height; }
    // The boundary nearest z.
    double nearest(double z) const { return first_layer + std::round((z - first_layer) / layer_height) * layer_height; }
};

// The cube's top: the shared boundary nearest its nominal height, so the cube's last layer ends exactly there and the
// gap above is measured from what prints (Gen 2026-10-29; 5.05 mm with a 0.25 mm first layer and 0.2 mm layers).
double cube_top(mp_ctx *ctx)
{
    const LayerGrid grid(ctx);
    return grid.layer_height > 0. ? std::max(grid.first_layer, grid.nearest(CUBE_HEIGHT)) : CUBE_HEIGHT;
}

// The specimen's own layers from below the lid's bottom (Gen 2026-10-29): the lid starts exactly at its gap, its
// first two layers have the layer height, the third and fourth are as thick as brings the fourth's top back onto
// the layers every specimen shares, from the fifth on all specimens print together. Empty when the lid's bottom is a
// shared layer boundary already; nothing either when no correction fits the printer's layer heights (`fits` false).
std::vector<LayerSpan> lid_layers(mp_ctx *ctx, double lid_bottom, bool *fits = nullptr)
{
    if (fits != nullptr)
        *fits = true;
    const LayerGrid grid(ctx);
    const double    layer_height = grid.layer_height;
    const double    first_layer  = grid.first_layer;
    // As Slicing's min_layer_height_from_nozzle / max_layer_height_from_nozzle for the first nozzle, which prints
    // the specimens and their support (0 is 0.07 mm, at least 0.01 mm; a maximum of 0 is 3/4 of the nozzle), and
    // never past the layer height itself (generate_object_layers asserts both).
    const double min_setting  = config_at(ctx, "min_layer_height", 0).value_or(0.);
    const double max_setting  = config_at(ctx, "max_layer_height", 0).value_or(0.);
    const double min_height   = std::min(min_setting == 0. ? 0.07 : std::max(0.01, min_setting), layer_height);
    const double max_height   = std::max(std::max(min_height, max_setting == 0. ? 0.75 * config_at(ctx, "nozzle_diameter", 0).value_or(0.4) : max_setting),
                                         layer_height);
    if (!(layer_height > 0.) || lid_bottom <= first_layer + layer_height)
        return {};
    auto grid_below = [&grid](double z) { return grid.below(z); };

    std::vector<LayerSpan> out;
    const double below = grid_below(lid_bottom);
    const double rest  = lid_bottom - below; // mm of the gap above the last shared boundary
    if (rest < Z_EPSILON)
        return {};
    if (rest >= min_height - Z_EPSILON)
        out.push_back({ below, lid_bottom });
    else {
        // Too thin for a layer: the layer below takes it, as two layers of half the sum.
        const double half = 0.5 * (layer_height + rest);
        out.push_back({ below - layer_height, below - layer_height + half });
        out.push_back({ below - layer_height + half, lid_bottom });
    }
    out.push_back({ lid_bottom, lid_bottom + layer_height });
    out.push_back({ lid_bottom + layer_height, lid_bottom + 2. * layer_height });
    // The third and fourth layers end on a shared boundary: of the boundaries 1 to 4 layer heights above the
    // second's top, the one whose two layers are nearest the layer height within the printer's limits.
    const double second_top = lid_bottom + 2. * layer_height;
    const double base       = grid_below(second_top);
    double       best       = 0.;
    for (int k = 1; k <= 4; ++k) {
        const double h = 0.5 * (base + k * layer_height - second_top);
        if (h >= min_height - Z_EPSILON && h <= max_height + Z_EPSILON && (best == 0. || std::abs(h - layer_height) < std::abs(best - layer_height)))
            best = h;
    }
    if (best == 0.) {
        if (fits != nullptr)
            *fits = false;
        return {};
    }
    out.push_back({ second_top, second_top + best });
    out.push_back({ second_top + best, second_top + 2. * best });
    return out;
}

std::string cell_name(const Grid &g, int i, int j)
{
    std::string name;
    auto        add = [&name](const AxisValues &a, double v) {
        if (a.axis == nullptr)
            return;
        if (!name.empty())
            name += "_";
        char buf[32];
        std::snprintf(buf, sizeof(buf), a.axis->integer ? "%s%.0f" : "%s%.2f", a.axis->prefix, v);
        name += buf;
    };
    add(g.x, g.x.values[size_t(i)]);
    add(g.y, g.y.values[size_t(j)]);
    return name;
}

// Cells in the object list: the hottest interface first, rows alternating direction, so neighbours follow each
// other. Every other layer prints them in reverse (reverse_object_order), so a layer starts at the temperature the
// last one ended at (Gen 2026-10-29).
std::vector<std::pair<int, int>> print_order(const Grid &g)
{
    std::vector<std::pair<int, int>> cells;
    for (int j = 0; j < g.ny(); ++j)
        for (int k = 0; k < g.nx(); ++k)
            cells.emplace_back((j % 2 == 0) ? k : g.nx() - 1 - k, j);
    std::stable_sort(cells.begin(), cells.end(), [&](const auto &a, const auto &b) {
        return cell_temperature(g, a.first, a.second) > cell_temperature(g, b.first, b.second);
    });
    return cells;
}

// The bottom-left corner of cell (i, j) in plate coordinates: the grid centred on the bed, column i from the left,
// row j from the back.
std::pair<double, double> cell_corner(const lesic::Bed &bed, const Grid &g, int i, int j)
{
    const double left = bed.center_x() - 0.5 * g.width();
    const double back = bed.center_y() + 0.5 * g.depth();
    return { left + i * PITCH, back - SPECIMEN_SIZE - j * PITCH };
}

void grid_check(mp_ctx *ctx, const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 &project)
{
    std::string               error;
    const std::optional<Grid> grid = grid_of(fields, n, error);
    if (!grid) {
        report(project, MP_CALIB_REPORT_ERROR, error.c_str());
        return;
    }
    const Grid      &g   = *grid;
    const lesic::Bed bed = lesic::bed_of(ctx);
    if (g.width() + 2. * LABEL_MARGIN > bed.width() || g.depth() + 2. * LABEL_MARGIN > bed.depth())
        report(project, MP_CALIB_REPORT_ERROR, "The grid of %1% x %2% mm does not fit on the bed.", { g.width(), g.depth() });
    double coldest = std::numeric_limits<double>::max(), hottest = 0.;
    for (int i = 0; i < g.nx(); ++i)
        for (int j = 0; j < g.ny(); ++j) {
            coldest = std::min(coldest, cell_temperature(g, i, j));
            hottest = std::max(hottest, cell_temperature(g, i, j));
        }
    if (coldest < FIRMWARE_MIN_EXTRUDE_C)
        report(project, MP_CALIB_REPORT_WARNING, "Interface temperatures below %1% °C can stop at the firmware's cold extrusion limit.",
               { FIRMWARE_MIN_EXTRUDE_C });
    // Orca prints a support base layer under the interface where the support's bottom has no interface layer or
    // keeps a gap to the cube (toggle test 10-28: bottom 2 layers and 0 mm interface only; 0 layers or 0.2 mm a
    // base layer at the bottom of the gap).
    bool base_layer = false;
    for (int i = 0; i < g.nx(); ++i)
        for (int j = 0; j < g.ny(); ++j) {
            double bottom_layers = cell_setting(g, "bottom_interface_layers", i, j);
            if (bottom_layers < 0.)
                bottom_layers = cell_setting(g, "top_interface_layers", i, j);
            const bool touches = cell_setting(g, "bottom_z_distance", i, j) == 0. || cell_setting(g, "top_z_distance", i, j) == 0.;
            base_layer = base_layer || bottom_layers < 1. || !touches;
        }
    bool fits = true;
    for (int i = 0; i < g.nx() && fits; ++i)
        for (int j = 0; j < g.ny() && fits; ++j)
            lid_layers(ctx, cube_top(ctx) + gap_height(ctx, g, i, j), &fits);
    if (!fits)
        report(project, MP_CALIB_REPORT_WARNING,
               "Some lids cannot get back onto the shared layers within the printer's layer heights; those specimens keep Orca's layers.");
    if (base_layer)
        report(project, MP_CALIB_REPORT_WARNING,
               "Some specimens print a support base layer under the interface: for the interface only, use bottom interface layers and a bottom Z distance of 0.");
    const double nozzle = config_at(ctx, "nozzle_temperature", 0).value_or(0.);
    if (hottest >= nozzle)
        report(project, MP_CALIB_REPORT_WARNING,
               "Some interface temperatures are not below the filament's %1% °C; those specimens print without cooling.", { nozzle });
    report(project, MP_CALIB_REPORT_SUMMARY, "Grid %1% x %2% mm, %3% specimens", { g.width(), g.depth(), double(g.nx() * g.ny()) });
}

void grid_build(mp_ctx *ctx, const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 &project)
{
    std::string               error;
    const std::optional<Grid> grid = grid_of(fields, n, error);
    if (!grid) {
        report(project, MP_CALIB_REPORT_ERROR, error.c_str());
        return;
    }
    const Grid      &g   = *grid;
    const lesic::Bed bed = lesic::bed_of(ctx);

    auto print = [&](const char *key, const std::string &value) { project.set_option(project.p, MP_CALIB_PRINT, key, value.c_str()); };
    // Support of the normal generator under each lid (the specimens' enforcers), resting on the cube; the interface
    // with the support's filament, which low_temp_interface prints cold; specimens in the order they are added.
    print("enable_support", "1");
    print("support_type", "normal(auto)");
    print("support_on_build_plate_only", "0");
    print("support_filament", "0");
    print("support_interface_filament", "0");
    print("print_sequence", "by layer");
    print("print_order", "as_obj_list");
    print("low_temp_interface.enable", "1");
    // Orca's normal support reaches past the lid's edge and, with the default 0.35 mm, prints a ring of support
    // around the cube from the bed up to the lid. 2 mm keeps the support on the cube (toggle test 10-28: 0.35 mm
    // ring on every layer, 1 mm only the first layer, 2 mm none).
    print("support_object_xy_distance", "2");
    // Snug support follows the lid's underside. Orca's default style takes a support area 0.6 mm wider than the cube
    // on each side (11.4 against the cube top's 10.3 mm contact) and fills the margin of a bottom interface layer that
    // the top interface does not reach with support body, printed hot onto the cube's edge (grid with one top
    // interface layer, 0.2 mm layers; snug removed it, test "The interface grid fills the gap with interface ...").
    print("support_style", "snug");
    double coldest = std::numeric_limits<double>::max();
    for (int i = 0; i < g.nx(); ++i)
        for (int j = 0; j < g.ny(); ++j)
            coldest = std::min(coldest, cell_temperature(g, i, j));
    // The temperature drop tower is sized for the process value: the coldest specimen's.
    if (cell_value(g, *find_axis("temperature"), 0, 0))
        print("low_temp_interface.temperature", number(coldest));

    const double cube = cube_top(ctx);
    for (const auto &[i, j] : print_order(g)) {
        const auto [x0, y0] = cell_corner(bed, g, i, j);
        const double  gap   = gap_height(ctx, g, i, j);
        const int32_t obj   = project.add_object(project.p, cell_name(g, i, j).c_str());
        project.add_box(project.p, obj, MP_CALIB_PART, x0, y0, 0., SPECIMEN_SIZE, SPECIMEN_SIZE, cube);
        project.add_box(project.p, obj, MP_CALIB_PART, x0, y0, cube + gap, SPECIMEN_SIZE, SPECIMEN_SIZE, LID_HEIGHT);
        // 64 corners around the handle.
        project.add_cylinder(project.p, obj, MP_CALIB_PART, x0 + 0.5 * SPECIMEN_SIZE, y0 + 0.5 * SPECIMEN_SIZE, cube + gap + LID_HEIGHT,
                             0.5 * HANDLE_DIAMETER, HANDLE_HEIGHT, 2. * M_PI / 64.);
        // The enforcer fills the gap and reaches 1 mm into the cube and the lid, so the lid's whole underside is an
        // overhang to support.
        project.add_box(project.p, obj, MP_CALIB_SUPPORT_ENFORCER, x0, y0, cube - 1., SPECIMEN_SIZE, SPECIMEN_SIZE, gap + 2.);
        // A height range per layer, so each layer begins where the last one ends.
        for (const LayerSpan &layer : lid_layers(ctx, cube + gap))
            project.add_layer_range(project.p, obj, layer.bottom, layer.top, layer.top - layer.bottom);
        for (const AxisValues *a : { &g.x, &g.y })
            if (a->axis != nullptr) {
                const double v = *cell_value(g, *a->axis, i, j);
                project.set_object_option(project.p, obj, a->axis->key, a->axis->integer ? std::to_string(int(v)).c_str() : number(v).c_str());
            }
    }

    // The layout, for the first layer's values and the header table (gcode.events).
    auto value = [&](const std::string &name, double v) { project.set_value(project.p, name.c_str(), v); };
    const auto [left, bottom] = cell_corner(bed, g, 0, g.ny() - 1);
    value("grid_left", left);
    value("grid_back", bottom + g.depth());
    value("grid_pitch", PITCH);
    value("grid_size", SPECIMEN_SIZE);
    for (const auto &[name, a] : { std::pair<std::string, const AxisValues *>("x", &g.x), std::pair<std::string, const AxisValues *>("y", &g.y) }) {
        value(name + "_count", double(a->values.size()));
        value(name + "_axis", a->axis != nullptr ? double(a->axis - AXES) : -1.);
        for (size_t k = 0; k < a->values.size(); ++k)
            value(name + "_" + std::to_string(k), a->values[k]);
    }
}

// -- The grid: G-code --

struct GridState
{
    const mp_gcode_writer_v1 *w = nullptr;
    std::map<std::string, double> values;
    double get(const std::string &name) const
    {
        auto it = values.find(name);
        return it == values.end() ? 0. : it->second;
    }
};

GridState read_grid(const mp_gcode_writer_v1 &w)
{
    GridState s;
    s.w = &w;
    auto read = [&](const std::string &name) {
        double v = 0.;
        if (w.calibration_value(w.w, name.c_str(), &v) == MP_OK)
            s.values[name] = v;
    };
    for (const char *name : { "grid_left", "grid_back", "grid_pitch", "grid_size", "x_count", "x_axis", "y_count", "y_axis" })
        read(name);
    for (const char *axis : { "x", "y" })
        for (int k = 0; k < int(s.get(std::string(axis) + "_count")); ++k)
            read(std::string(axis) + "_" + std::to_string(k));
    return s;
}

std::string label_text(const GridState &s, const char *axis, int k)
{
    const int index = int(s.get(std::string(axis) + "_axis"));
    const double v  = s.get(std::string(axis) + "_" + std::to_string(k));
    char buf[32];
    std::snprintf(buf, sizeof(buf), index >= 0 && AXES[index].integer ? "%.0f" : "%g", v);
    return buf;
}

// Which setting each axis is and the values per column and row, as comments at the start.
void grid_header(const GridState &s)
{
    std::ostringstream out;
    out << "; ---------- low temperature interface grid ----------\n";
    for (const char *axis : { "x", "y" }) {
        const int index = int(s.get(std::string(axis) + "_axis"));
        if (index < 0)
            continue;
        out << "; ltc_" << axis << "_axis=" << AXES[index].key << "\n; ltc_" << axis << "_values=";
        for (int k = 0; k < int(s.get(std::string(axis) + "_count")); ++k)
            out << (k > 0 ? "," : "") << label_text(s, axis, k);
        out << "\n";
    }
    out << "; columns from the left, rows from the back; specimen names give both values\n";
    s.w->text(s.w->w, out.str().c_str());
}

// The column values above the grid and the row values left of it, drawn on the first layer.
void grid_labels(const GridState &s, mp_ctx *ctx)
{
    const int32_t filament_id = s.w->filament(s.w->w);
    if (filament_id < 0)
        return;
    const double filament_diameter = config_at(ctx, "filament_diameter", filament_id).value_or(0.);
    const double filament_area     = M_PI * ((filament_diameter * 0.5) * (filament_diameter * 0.5));
    const double layer_height      = std::max(0.05, s.w->last_height(s.w->w));
    if (filament_area <= lesic::EPSILON)
        return;
    const double flow_scale = layer_height * config_f64(ctx, "print_flow_ratio").value_or(1.) / filament_area;
    const double left = s.get("grid_left"), back = s.get("grid_back"), pitch = s.get("grid_pitch"), size = s.get("grid_size");
    std::vector<lesic::LesicAnnotationSegment> segments;
    auto place = [&](const std::string &label, double cx, double cy) {
        for (lesic::LesicAnnotationSegment seg : lesic::lesic_single_line_label_segments(label, LABEL_HEIGHT)) {
            seg.a = lesic::Vec2d(seg.a.x() + cx, seg.a.y() + cy);
            seg.b = lesic::Vec2d(seg.b.x() + cx, seg.b.y() + cy);
            segments.push_back(seg);
        }
    };
    for (int k = 0; k < int(s.get("x_count")); ++k)
        place(label_text(s, "x", k), left + k * pitch + 0.5 * size, back + 0.5 * LABEL_MARGIN);
    if (int(s.get("y_axis")) >= 0)
        for (int k = 0; k < int(s.get("y_count")); ++k)
            place(label_text(s, "y", k), left - 0.5 * LABEL_MARGIN, back - k * pitch - 0.5 * size);
    if (segments.empty())
        return;
    s.w->text(s.w->w, "; ---------- low temperature interface grid values ----------\n");
    s.w->set_feedrate(s.w->w, 20.0 * 60.0, "grid label speed");
    s.w->plate_frame(s.w->w);
    for (const lesic::LesicAnnotationSegment &segment : segments) {
        const double length = (segment.b - segment.a).norm();
        if (length <= lesic::EPSILON)
            continue;
        s.w->raw_travel(s.w->w, segment.a.x(), segment.a.y(), "grid label travel");
        s.w->raw_move(s.w->w, segment.b.x(), segment.b.y(), flow_scale * segment.width * length, 0, "grid label");
    }
    s.w->text(s.w->w, "; ---------- end low temperature interface grid values ----------\n");
}

// -- Slot tables --

enum class Kind { Lesic, Grid };

struct State
{
    Kind              kind = Kind::Lesic;
    lesic::LesicState lesic;
    GridState         grid;
    bool              header = false;
};

mp_status open(void *, mp_ctx *, const mp_gcode_export_v1 *info, const mp_gcode_writer_v1 *writer, void **state, uint32_t *flags)
{
    return magpie::guarded([&] {
        *state = nullptr;
        const std::string calibration = info->calibration != nullptr ? info->calibration : "";
        auto              s           = std::make_unique<State>();
        if (calibration == LTC_MOD_ID ".lesic") {
            s->kind         = Kind::Lesic;
            s->lesic.w      = writer;
            s->lesic.params = lesic::read_params(*writer);
            *flags          = MP_GCODE_WANTS_PATH_SPEEDS | MP_GCODE_USED;
        } else if (calibration == LTC_MOD_ID ".grid") {
            s->kind = Kind::Grid;
            s->grid = read_grid(*writer);
            *flags  = MP_GCODE_USED;
        } else
            return;
        *state = s.release();
    });
}

mp_status event(void *, void *state, mp_ctx *ctx, mp_gcode_event_v1 *ev)
{
    return magpie::guarded([&] {
        State &s = *static_cast<State *>(state);
        if (s.kind == Kind::Lesic) {
            if (ev->kind == MP_GCODE_EV_LAYER_BEGIN)
                lesic::on_layer_begin(s.lesic);
            else if (ev->kind == MP_GCODE_EV_FIRST_LAYER_END)
                lesic::on_first_layer_end(s.lesic, ctx);
        } else {
            if (ev->kind == MP_GCODE_EV_LAYER_BEGIN && !s.header) {
                grid_header(s.grid);
                s.header = true;
            } else if (ev->kind == MP_GCODE_EV_FIRST_LAYER_END)
                grid_labels(s.grid, ctx);
        }
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

mp_status reverse_object_order(void *, void *state, mp_ctx *, int32_t layer_index, int32_t *out)
{
    *out = static_cast<const State *>(state)->kind == Kind::Grid && layer_index % 2 == 1 ? 1 : 0;
    return {MP_OK, nullptr};
}

mp_status path_speeds(void *, void *state, mp_ctx *, const mp_gcode_speed_query_v1 *query, double *speeds, int32_t *changed)
{
    return magpie::guarded([&] {
        *changed       = 0;
        const State &s = *static_cast<const State *>(state);
        if (s.kind == Kind::Lesic)
            lesic::lesic_path_speeds(s.lesic, query, speeds, changed);
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

const mp_gcode_events_v1 events_table = {sizeof(mp_gcode_events_v1), open, event, support_last, tower_on_layer, path_speeds, start_gcode,
                                         close, validate, tower_preview, tower_requested, reverse_object_order};

mp_status check(void *, mp_ctx *ctx, const char *calibration, const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 *project)
{
    return magpie::guarded([&] {
        const std::string id = calibration != nullptr ? calibration : "";
        if (id == "lesic")
            lesic::lesic_check(fields, n, *project);
        else if (id == "grid")
            grid_check(ctx, fields, n, *project);
        else
            throw std::runtime_error("Unknown calibration " + id);
    });
}

mp_status build(void *, mp_ctx *ctx, const char *calibration, const mp_calib_field_v1 *fields, uint32_t n, const mp_calib_project_v1 *project)
{
    return magpie::guarded([&] {
        const std::string id = calibration != nullptr ? calibration : "";
        if (id == "lesic")
            lesic::lesic_build(ctx, fields, n, *project);
        else if (id == "grid")
            grid_build(ctx, fields, n, *project);
        else
            throw std::runtime_error("Unknown calibration " + id);
    });
}

const mp_calibration_project_v1 project_table = {sizeof(mp_calibration_project_v1), check, build};

} // namespace ltc

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    const int32_t rc = registrar.register_slot(&registrar, MP_GCODE_EVENTS_SLOT, MP_GCODE_EVENTS_VERSION, LTC_MOD_ID, &ltc::events_table,
                                               sizeof(ltc::events_table), nullptr);
    if (rc != MP_OK)
        return rc;
    return registrar.register_slot(&registrar, MP_CALIBRATION_PROJECT_SLOT, MP_CALIBRATION_PROJECT_VERSION, LTC_MOD_ID, &ltc::project_table,
                                   sizeof(ltc::project_table), nullptr);
}

void magpie::mod_shutdown() {}
